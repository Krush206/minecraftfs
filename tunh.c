#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int main(int argc, char *argv[])
{
	pid_t pid;
	int pin[2];
	int pout[2];
	int fd;
	struct sockaddr_in addr;
	char *vp[] = { "9", "9p", "rdwr", "minecraftfs/data", NULL };

	if(pipe(pin) < 0)
	{
		perror("pipe() failed");
		return 1;
	}
	if(pipe(pout) < 0)
	{
		perror("pipe() failed");
		close(pin[0]);
		close(pin[1]);
		return 1;
	}
	pid = fork();
	if(pid < 0)
	{
		perror("fork() failed");
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		return 1;
	}
	if(pid == 0)
	{
		close(pin[1]);
		close(pout[0]);
		close(0);
		dup(pin[0]);
		close(1);
		dup(pout[1]);
		execvp(vp[0], vp);
		perror("execvp() failed");
		close(pin[0]);
		close(pout[1]);
		return 1;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0)
	{
		perror("socket() failed");
		kill(pid, SIGINT);
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		return 1;
	}
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5505);
	inet_aton("127.0.0.1", &addr.sin_addr);
	if(connect(fd, (struct sockaddr *) &addr, sizeof addr) < 0)
	{
		perror("connect() failed");
		kill(pid, SIGINT);
		close(fd);
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		return 1;
	}
	close(pin[0]);
	close(pout[1]);
	while(1)
	{
		char c;

		if(read(fd, &c, (size_t) 1) <= 0)
			break;
		if(write(pin[1], &c, (size_t) 1) <= 0)
			break;
		if(read(pout[0], &c, (size_t) 1) <= 0)
			break;
		if(write(fd, &c, (size_t) 1) <= 0)
			break;
	}
	return 0;
}
