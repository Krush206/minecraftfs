/*
 * tunh - host-side TCP-to-9p tunnel for minecraftfs.
 *
 * Dials the real Minecraft server and forwards that connection, full
 * duplex, through a minecraftfs data file (via `9 9p rdwr`), where a
 * tunc on the far side accepted the client. See tunc.c for the
 * protocol note: forwarding is two concurrent copy loops, not a
 * lock-step exchange, so full-duplex traffic cannot deadlock.
 *
 * Usage: tunh [-c addr] [-p port] [-f 9pfile] [-s server]
 * Defaults: addr 127.0.0.1, port 5505, file "minecraftfs/data".
 * -s NAME is shorthand for -f minecraftfs/NAME/data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

static char *xfile = "minecraftfs/data";
static char srvbuf[128];

static void
usage(void)
{
	fprintf(stderr, "usage: tunh [-c addr] [-p port] [-f 9pfile] [-s server]\n");
	exit(1);
}

static ssize_t
xwrite(int fd, const void *buf, size_t n)
{
	size_t off;
	ssize_t m;

	off = 0;
	while(off < n){
		m = write(fd, (const char *)buf+off, n-off);
		if(m < 0){
			if(errno == EINTR)
				continue;
			return -1;
		}
		if(m == 0)
			return -1;
		off += m;
	}
	return off;
}

/* Copy src -> dst until EOF or error. Then shut down dst's write side. */
static void
copyfd(int src, int dst, int dstissock)
{
	char buf[8192];
	ssize_t n;

	for(;;){
		n = read(src, buf, sizeof buf);
		if(n <= 0)
			break;
		if(xwrite(dst, buf, n) < 0)
			break;
	}
	if(dstissock)
		shutdown(dst, SHUT_WR);
}

int main(int argc, char *argv[])
{
	pid_t pid, cpid;
	int pin[2];
	int pout[2];
	int fd;
	int status;
	char *dialaddr;
	char *vp[5];
	int port;
	struct sockaddr_in addr;
	int o;

	dialaddr = "127.0.0.1";
	port = 5505;
	while((o = getopt(argc, argv, "c:p:f:s:h")) != -1){
		switch(o){
		case 'c':
			dialaddr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'f':
			xfile = optarg;
			break;
		case 's':
			snprintf(srvbuf, sizeof srvbuf, "minecraftfs/%s/data", optarg);
			xfile = srvbuf;
			break;
		default:
			usage();
		}
	}
	if(optind != argc)
		usage();

	signal(SIGPIPE, SIG_IGN);

	if(pipe(pin) < 0){
		perror("pipe() failed");
		return 1;
	}
	if(pipe(pout) < 0){
		perror("pipe() failed");
		close(pin[0]);
		close(pin[1]);
		return 1;
	}
	pid = fork();
	if(pid < 0){
		perror("fork() failed");
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		return 1;
	}
	if(pid == 0){
		close(pin[1]);
		close(pout[0]);
		if(dup2(pin[0], 0) < 0 || dup2(pout[1], 1) < 0){
			perror("dup2() failed");
			_exit(1);
		}
		close(pin[0]);
		close(pout[1]);
		vp[0] = "9";
		vp[1] = "9p";
		vp[2] = "rdwr";
		vp[3] = xfile;
		vp[4] = NULL;
		execvp(vp[0], vp);
		perror("execvp() failed");
		_exit(1);
	}
	close(pin[0]);
	close(pout[1]);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0){
		perror("socket() failed");
		kill(pid, SIGKILL);
		goto out;
	}
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if(inet_aton(dialaddr, &addr.sin_addr) == 0){
		fprintf(stderr, "bad dial address '%s'\n", dialaddr);
		kill(pid, SIGKILL);
		close(fd);
		goto out;
	}
	if(connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
		perror("connect() failed");
		kill(pid, SIGKILL);
		close(fd);
		goto out;
	}

	/* Full duplex: one direction per process. */
	cpid = fork();
	if(cpid < 0){
		perror("fork() failed");
		kill(pid, SIGKILL);
		close(fd);
		goto out;
	}
	if(cpid == 0){
		close(pin[1]);
		copyfd(pout[0], fd, 1);
		close(pout[0]);
		close(fd);
		_exit(0);
	}
	close(pout[0]);
	copyfd(fd, pin[1], 0);
	close(pin[1]);
	shutdown(fd, SHUT_RDWR);
	close(fd);
	waitpid(cpid, &status, 0);
out:
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	close(pin[1]);
	close(pout[0]);
	return 0;
}
