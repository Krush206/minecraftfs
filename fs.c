/*
 * minecraftfs - allocate Minecraft backend servers through a 9p file tree.
 *
 * A Plan 9 from User Space (plan9port) file server. It does not run
 * Minecraft itself; it allocates names for backend Minecraft servers
 * (real Java/Bedrock processes listening on TCP) and proxies the
 * per-server "data" files to them, so the tunnel helpers (tunc/tunh)
 * and the "9" tool can reach a whole farm through a single mount.
 *
 * Namespace (posted as "minecraftfs"):
 *
 *	ctl		read: "name dialaddr" lines, one per server
 *			write: "new NAME ADDR" | "new ADDR" | "free NAME"
 *	clone		read allocates a server named sN, returns "sN\n".
 *			point it at a backend with sN/addr.
 *	data		legacy alias: proxies the "default" server, or the
 *			only server when exactly one exists.
 *	<name>/
 *		addr	backend dial string; write to repoint it.
 *		data	bidirectional byte stream to the backend.
 *		ctl	read shows "name dialaddr"; write "free" or "addr ADDR".
 *
 * ADDR is "host:port", "port" (meaning localhost), or a literal
 * "tcp!host!port" dial string. A backend address may only be
 * allocated once, so `cat ctl` doubles as the port inventory.
 *
 * Build (plan9port):	mk
 * Run:			minecraftfs [-s srvname]
 * Use:			9p rdwr minecraftfs/survival/data
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

enum {
	MaxName = 28,		/* max server name length */
	MaxAddr = 160,		/* max dial string length */
	MaxSrv = 64,		/* max allocated servers */
	MaxMsg = 512,		/* max control message length */
};

static char Einvalid[] = "invalid control message";
static char Eunknown[] = "unknown file";
static char Eexists[] = "server already exists";
static char Enosrv[] = "no such server";
static char Eaddrinuse[] = "address already allocated";
static char Efull[] = "too many servers";
static char Ebadname[] = "bad server name";
static char Ebadaddr[] = "bad server address";
static char Enodefault[] = "no default server";
static char Efreed[] = "server was freed";
static char Eperm[] = "permission denied";
static char Eisdir[] = "is a directory";
static char Edial[] = "dial failed";
static char Eio[] = "backend i/o error";
static char Emem[] = "out of memory";

typedef struct Server Server;
struct Server {
	int used;
	char name[MaxName+1];
	char dialaddr[MaxAddr+1];
	File *dir;
	File *addrf;
	File *dataf;
	File *ctlf;
};

typedef struct Conn Conn;
struct Conn {
	int fd;			/* dialled backend, one per open fid */
};

static Server srvs[MaxSrv];
static Lock srclock;
static Tree *tree;
static File *rootctl;
static File *rootclone;
static File *rootdata;
static int nauto;

int
threadmaybackground(void)
{
	return 1;
}

static void
usage(void)
{
	fprint(2, "usage: minecraftfs [-s srvname]\n");
	threadexits("usage");
}

/* names: short, no slashes or spaces, usable as 9p path elements */
static int
validname(char *s)
{
	int n, c;

	n = strlen(s);
	if(n == 0 || n > MaxName)
		return 0;
	if(strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
		return 0;
	for(; *s; s++){
		c = *s;
		if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		   (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
			continue;
		return 0;
	}
	return 1;
}

/*
 * Normalize a user-supplied backend address into a dial string.
 * Accepts "host:port", "port", or a literal "tcp!host!port".
 */
static int
mknorm(char *dst, int ndst, char *addr)
{
	char tmp[256];
	char *p, *colon;
	int n, ncol;

	USED(ndst);
	while(*addr == ' ' || *addr == '\t' || *addr == '\r' || *addr == '\n')
		addr++;
	n = strlen(addr);
	while(n > 0 && (addr[n-1] == ' ' || addr[n-1] == '\t' ||
	       addr[n-1] == '\r' || addr[n-1] == '\n'))
		n--;
	if(n == 0 || n >= (int)sizeof tmp)
		return -1;
	memmove(tmp, addr, n);
	tmp[n] = '\0';
	if(strchr(tmp, '!') != nil){
		if(strlen(tmp) > MaxAddr)
			return -1;
		strcpy(dst, tmp);
		return 0;
	}
	ncol = 0;
	for(p = tmp; *p; p++){
		if(*p == ':')
			ncol++;
		if(*p == ' ' || *p == '\t')
			return -1;
	}
	if(ncol == 1){
		colon = strchr(tmp, ':');
		if(colon == tmp || colon[1] == '\0')
			return -1;
		*colon = '\0';
		if(4 + strlen(tmp) + 1 + strlen(colon+1) > MaxAddr)
			return -1;
		sprint(dst, "tcp!%s!%s", tmp, colon+1);
		return 0;
	}
	if(ncol == 0){
		if(15 + strlen(tmp) > MaxAddr)
			return -1;
		sprint(dst, "tcp!127.0.0.1!%s", tmp);
		return 0;
	}
	if(4 + strlen(tmp) > MaxAddr)
		return -1;
	sprint(dst, "tcp!%s", tmp);
	return 0;
}

/* Split msg on whitespace, NUL-terminating fields. Returns field count. */
static int
splitfields(char *s, char **a, int na)
{
	int n;

	n = 0;
	for(;;){
		while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
			s++;
		if(*s == '\0')
			break;
		if(n < na)
			a[n] = s;
		n++;
		while(*s != '\0' && *s != ' ' && *s != '\t' &&
		       *s != '\r' && *s != '\n')
			s++;
		if(*s == '\0')
			break;
		if(n <= na)
			*s = '\0';
		s++;
	}
	return n;
}

/* The next routines assume srclock is held. */
static Server*
findsrv(char *name)
{
	Server *s;

	for(s = srvs; s < srvs+MaxSrv; s++)
		if(s->used && strcmp(s->name, name) == 0)
			return s;
	return nil;
}

static Server*
findaddr(char *dialaddr)
{
	Server *s;

	for(s = srvs; s < srvs+MaxSrv; s++)
		if(s->used && s->dialaddr[0] != '\0' &&
		   strcmp(s->dialaddr, dialaddr) == 0)
			return s;
	return nil;
}

static Server*
emptyslot(void)
{
	Server *s;

	for(s = srvs; s < srvs+MaxSrv; s++)
		if(!s->used)
			return s;
	return nil;
}

/* Validate that f is still one of the live files of a live server. */
static Server*
lockedself(File *f)
{
	Server *s;

	s = f->aux;
	if(s < srvs || s >= srvs+MaxSrv || !s->used)
		return nil;
	if(f != s->addrf && f != s->dataf && f != s->ctlf)
		return nil;
	return s;
}

static Server*
defaultserver(void)
{
	Server *s, *only;
	int n;

	only = nil;
	n = 0;
	for(s = srvs; s < srvs+MaxSrv; s++){
		if(!s->used)
			continue;
		if(strcmp(s->name, "default") == 0)
			return s;
		only = s;
		n++;
	}
	if(n == 1)
		return only;
	return nil;
}

static void
dropfiles(Server *s)
{
	if(s->addrf != nil){
		removefile(s->addrf);
		s->addrf = nil;
	}
	if(s->dataf != nil){
		removefile(s->dataf);
		s->dataf = nil;
	}
	if(s->ctlf != nil){
		removefile(s->ctlf);
		s->ctlf = nil;
	}
	if(s->dir != nil){
		removefile(s->dir);
		s->dir = nil;
	}
}

static int
makedirs(Server *s)
{
	s->dir = createfile(tree->root, s->name, nil, 0775|DMDIR, s);
	if(s->dir == nil)
		return -1;
	s->addrf = createfile(s->dir, "addr", nil, 0664, s);
	s->dataf = createfile(s->dir, "data", nil, 0660, s);
	s->ctlf = createfile(s->dir, "ctl", nil, 0660, s);
	if(s->addrf == nil || s->dataf == nil || s->ctlf == nil){
		dropfiles(s);
		return -1;
	}
	return 0;
}

/* Allocate a server. name==nil picks sN. Lock is held on entry. */
static char*
allocserver(char *name, char *addrstr)
{
	char norm[MaxAddr+1];
	char auto_[MaxName+1];
	Server *s;

	if(mknorm(norm, sizeof norm, addrstr) < 0)
		return Ebadaddr;
	if(name == nil){
		do {
			sprint(auto_, "s%d", nauto++);
		} while(findsrv(auto_) != nil);
		name = auto_;
	}else{
		if(!validname(name))
			return Ebadname;
		if(findsrv(name) != nil)
			return Eexists;
	}
	if(findaddr(norm) != nil)
		return Eaddrinuse;
	s = emptyslot();
	if(s == nil)
		return Efull;
	strcpy(s->name, name);
	strcpy(s->dialaddr, norm);
	s->used = 1;
	if(makedirs(s) < 0){
		s->used = 0;
		s->name[0] = '\0';
		s->dialaddr[0] = '\0';
		return Eio;
	}
	fprint(2, "minecraftfs: new %s %s\n", s->name, s->dialaddr);
	return nil;
}

static char*
freeserver(char *name)
{
	Server *s;

	s = findsrv(name);
	if(s == nil)
		return Enosrv;
	dropfiles(s);
	s->used = 0;
	s->name[0] = '\0';
	s->dialaddr[0] = '\0';
	fprint(2, "minecraftfs: free %s\n", name);
	return nil;
}

/* Repoint a server. Lock is held on entry. */
static char*
setaddr(Server *s, char *addrstr)
{
	char norm[MaxAddr+1];
	Server *other;

	if(mknorm(norm, sizeof norm, addrstr) < 0)
		return Ebadaddr;
	other = findaddr(norm);
	if(other != nil && other != s)
		return Eaddrinuse;
	strcpy(s->dialaddr, norm);
	fprint(2, "minecraftfs: addr %s %s\n", s->name, s->dialaddr);
	return nil;
}

/* Root ctl commands. Returns nil on success, error string otherwise. */
static char*
docmd(char *msg)
{
	char *f[4];
	char *err;
	int nf;

	nf = splitfields(msg, f, 4);
	if(nf == 3 && strcmp(f[0], "new") == 0){
		lock(&srclock);
		err = allocserver(f[1], f[2]);
		unlock(&srclock);
		return err;
	}
	if(nf == 2 && strcmp(f[0], "new") == 0){
		lock(&srclock);
		err = allocserver(nil, f[1]);
		unlock(&srclock);
		return err;
	}
	if(nf == 2 && strcmp(f[0], "free") == 0){
		lock(&srclock);
		err = freeserver(f[1]);
		unlock(&srclock);
		return err;
	}
	return Einvalid;
}

/* Per-server ctl commands ("free", "addr ADDR"). Lock is taken here. */
static char*
srvctlcmd(File *f, char *msg)
{
	char *a[3];
	char nm[MaxName+1];
	Server *s;
	char *err;
	int n;

	n = splitfields(msg, a, 3);
	lock(&srclock);
	s = lockedself(f);
	if(s == nil){
		unlock(&srclock);
		return Efreed;
	}
	if(n == 1 && strcmp(a[0], "free") == 0){
		strcpy(nm, s->name);
		dropfiles(s);
		s->used = 0;
		s->name[0] = '\0';
		s->dialaddr[0] = '\0';
		fprint(2, "minecraftfs: free %s\n", nm);
		unlock(&srclock);
		return nil;
	}
	if(n == 2 && strcmp(a[0], "addr") == 0){
		err = setaddr(s, a[1]);
		unlock(&srclock);
		return err;
	}
	unlock(&srclock);
	return Einvalid;
}

/* Whole-file write to an addr file. Lock is taken here. */
static char*
addrcmd(File *f, char *msg)
{
	Server *s;
	char *err;

	lock(&srclock);
	s = lockedself(f);
	if(s == nil){
		unlock(&srclock);
		return Efreed;
	}
	err = setaddr(s, msg);
	unlock(&srclock);
	return err;
}

static char*
buildlisting(void)
{
	char *b;
	Server *s;
	int m;

	b = malloc(MaxSrv*(MaxName+MaxAddr+4) + 1);
	if(b == nil)
		return nil;
	lock(&srclock);
	m = 0;
	for(s = srvs; s < srvs+MaxSrv; s++){
		if(!s->used)
			continue;
		if(s->dialaddr[0] != '\0')
			m += sprint(b+m, "%s %s\n", s->name, s->dialaddr);
		else
			m += sprint(b+m, "%s -\n", s->name);
	}
	unlock(&srclock);
	b[m] = '\0';
	return b;
}

static void
readstr(Req *r, char *s)
{
	ulong n, off, cnt;

	n = strlen(s);
	off = r->ifcall.offset;
	cnt = r->ifcall.count;
	if(off >= n){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	n -= off;
	if(n > cnt)
		n = cnt;
	memmove(r->ofcall.data, (uchar*)s+off, n);
	r->ofcall.count = n;
	respond(r, nil);
}

enum {
	Krootctl,
	Kclone,
	Krootdata,
	Kaddr,
	Kdata,
	Ksrvctl,
};

/* Classify f. Never dereferences a server File after unlocking. */
static int
filekind(File *f)
{
	Server *s;
	int k;

	if(f == rootctl)
		return Krootctl;
	if(f == rootclone)
		return Kclone;
	if(f == rootdata)
		return Krootdata;
	if(f->qid.type & QTDIR)
		return -1;
	lock(&srclock);
	s = lockedself(f);
	k = -1;
	if(s != nil){
		if(f == s->addrf)
			k = Kaddr;
		else if(f == s->dataf)
			k = Kdata;
		else if(f == s->ctlf)
			k = Ksrvctl;
	}
	unlock(&srclock);
	return k;
}

static void
fsopen(Req *r)
{
	File *f;
	Server *s;
	char addr[MaxAddr+1];
	Conn *c;
	int fd, i, k;

	f = r->fid->file;
	if(f->qid.type & QTDIR){
		respond(r, nil);
		return;
	}
	k = filekind(f);
	if(k == Kclone){
		/* every open of clone allocates a fresh unconnected server */
		lock(&srclock);
		s = emptyslot();
		if(s == nil){
			unlock(&srclock);
			respond(r, Efull);
			return;
		}
		do {
			sprint(s->name, "s%d", nauto++);
		} while(findsrv(s->name) != nil);
		s->dialaddr[0] = '\0';
		s->used = 1;
		if(makedirs(s) < 0){
			s->used = 0;
			s->name[0] = '\0';
			unlock(&srclock);
			respond(r, Eio);
			return;
		}
		i = s-srvs;
		fprint(2, "minecraftfs: clone %s\n", s->name);
		/* low bit tags clone fids; malloc'd Conn pointers stay even */
		r->fid->aux = (void*)(((uintptr)i<<1)|1);
		unlock(&srclock);
		respond(r, nil);
		return;
	}
	if(k == Kdata || k == Krootdata){
		lock(&srclock);
		if(k == Krootdata)
			s = defaultserver();
		else
			s = lockedself(f);
		if(s == nil){
			unlock(&srclock);
			respond(r, k == Krootdata ? Enodefault : Efreed);
			return;
		}
		if(s->dialaddr[0] == '\0'){
			unlock(&srclock);
			respond(r, Ebadaddr);
			return;
		}
		strcpy(addr, s->dialaddr);
		unlock(&srclock);
		fd = dial(addr, nil, nil, nil);
		if(fd < 0){
			respond(r, Edial);
			return;
		}
		c = malloc(sizeof *c);
		if(c == nil){
			close(fd);
			respond(r, Emem);
			return;
		}
		c->fd = fd;
		r->fid->aux = c;
		respond(r, nil);
		return;
	}
	if(k < 0){
		respond(r, Eunknown);
		return;
	}
	respond(r, nil);
}

static void
fsread(Req *r)
{
	File *f;
	Server *s;
	char line[256];
	char *list;
	uintptr a;
	Conn *c;
	long n;
	int i, k;

	f = r->fid->file;
	if(f->qid.type & QTDIR){
		respond(r, Eisdir);
		return;
	}
	k = filekind(f);
	switch(k){
	case Kclone:
		a = (uintptr)r->fid->aux;
		if((a & 1) == 0){
			respond(r, Einvalid);
			return;
		}
		i = a>>1;
		lock(&srclock);
		if(i < 0 || i >= MaxSrv || !srvs[i].used){
			unlock(&srclock);
			respond(r, Efreed);
			return;
		}
		sprint(line, "%s\n", srvs[i].name);
		unlock(&srclock);
		readstr(r, line);
		return;
	case Krootctl:
		list = buildlisting();
		if(list == nil){
			respond(r, Emem);
			return;
		}
		readstr(r, list);
		free(list);
		return;
	case Ksrvctl:
		lock(&srclock);
		s = lockedself(f);
		if(s == nil){
			unlock(&srclock);
			respond(r, Efreed);
			return;
		}
		if(s->dialaddr[0] != '\0')
			sprint(line, "%s %s\n", s->name, s->dialaddr);
		else
			sprint(line, "%s -\n", s->name);
		unlock(&srclock);
		readstr(r, line);
		return;
	case Kaddr:
		lock(&srclock);
		s = lockedself(f);
		if(s == nil){
			unlock(&srclock);
			respond(r, Efreed);
			return;
		}
		if(s->dialaddr[0] != '\0')
			sprint(line, "%s\n", s->dialaddr);
		else
			sprint(line, "-\n");
		unlock(&srclock);
		readstr(r, line);
		return;
	case Kdata:
	case Krootdata:
		a = (uintptr)r->fid->aux;
		if(a == 0 || (a & 1)){
			respond(r, Einvalid);
			return;
		}
		c = (Conn*)a;
		n = read(c->fd, r->ofcall.data, r->ifcall.count);
		if(n < 0){
			respond(r, Eio);
			return;
		}
		r->ofcall.count = n;
		respond(r, nil);
		return;
	default:
		respond(r, Eunknown);
		return;
	}
}

static void
fswrite(Req *r)
{
	File *f;
	char msg[MaxMsg+1];
	char *err;
	uintptr a;
	Conn *c;
	uchar *p;
	long m, n;
	int k;

	f = r->fid->file;
	if(f->qid.type & QTDIR){
		respond(r, Eisdir);
		return;
	}
	k = filekind(f);
	if(k == Kdata || k == Krootdata){
		a = (uintptr)r->fid->aux;
		if(a == 0 || (a & 1)){
			respond(r, Einvalid);
			return;
		}
		c = (Conn*)a;
		p = r->ifcall.data;
		m = r->ifcall.count;
		while(m > 0){
			n = write(c->fd, p, m);
			if(n <= 0){
				respond(r, Eio);
				return;
			}
			p += n;
			m -= n;
		}
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(r->ifcall.count > MaxMsg){
		respond(r, Einvalid);
		return;
	}
	memmove(msg, r->ifcall.data, r->ifcall.count);
	msg[r->ifcall.count] = '\0';
	switch(k){
	case Krootctl:
		err = docmd(msg);
		break;
	case Ksrvctl:
		err = srvctlcmd(f, msg);
		break;
	case Kaddr:
		err = addrcmd(f, msg);
		break;
	default:
		err = Eperm;	/* clone is read-only */
		break;
	}
	if(err != nil){
		respond(r, err);
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
fsclunk(Fid *fid)
{
	uintptr a;
	Conn *c;

	a = (uintptr)fid->aux;
	fid->aux = nil;
	if(a == 0 || (a & 1))
		return;		/* nil, or a clone tag: nothing per-fid to free */
	c = (Conn*)a;
	close(c->fd);
	free(c);
}

static void
fsdeny(Req *r)
{
	respond(r, Eperm);
}

static Srv fs = {
	.open=		fsopen,
	.read=		fsread,
	.write=		fswrite,
	.create=	fsdeny,
	.remove=	fsdeny,
	.wstat=		fsdeny,
	.destroyfid=	fsclunk,
};

void
threadmain(int argc, char *argv[])
{
	char *srvname;

	srvname = "minecraftfs";
	ARGBEGIN{
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	tree = alloctree(nil, nil, 0777|DMDIR, nil);
	rootctl = createfile(tree->root, "ctl", nil, 0660, nil);
	rootclone = createfile(tree->root, "clone", nil, 0444, nil);
	rootdata = createfile(tree->root, "data", nil, 0664, nil);
	if(tree == nil || rootctl == nil || rootclone == nil || rootdata == nil){
		fprint(2, "minecraftfs: cannot create files\n");
		threadexits("create");
	}
	threadpostmountsrv(&fs, srvname, nil, MBEFORE);
	threadexits(nil);
}
