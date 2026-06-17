#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

static char Einvalid[] = "invalid control";
static char Eunknown[] = "unknown file";
static char Eperm[] = "permission denied";

static File *data;
static File *ctl;

int
threadmaybackground(void)
{
	return 1;
}

static void
fsread(Req *r)
{
	File *f;

	f = r->fid->file;
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	File *f;
	char msg[64];

	f = r->fid->file;
	r->ofcall.count = r->ifcall.count;
	memcpy(msg, r->ifcall.data, r->ifcall.count);
	msg[r->ifcall.count] = '\0';
	fprint(2, "%s\n", msg);
	respond(r, nil);
}

static void
fscreate(Req *r)
{
	respond(r, Eperm);
}

static void
fsremove(Req *r)
{
	respond(r, Eperm);
}

static void
fswstat(Req *r)
{
	respond(r, nil);
}

static Srv fs = {
	.read=		fsread,
	.write=		fswrite,
	.create=	fscreate,
	.remove=	fsremove,
	.wstat=		fswstat
};

void
threadmain(int argc, char *argv[])
{
	char *srvname;

	fs.tree = alloctree(nil, nil, 0777 | DMDIR, nil);
	data = createfile(fs.tree->root, "data", nil, 0664, nil);
	ctl = createfile(fs.tree->root, "ctl", nil, 0660, nil);
	srvname = "minecraftfs";
	threadpostmountsrv(&fs, srvname, nil, MBEFORE);
	threadexits(nil);
}
