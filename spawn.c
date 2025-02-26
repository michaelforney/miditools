#include <fcntl.h>
#include <unistd.h>
#include "spawn.h"
#include "fatal.h"

void
spawn(const char *path, char *const argv[], int mode, int fd[2])
{
	pid_t pid;
	int p[4], t[2];

	t[0] = t[1] = p[0] = p[2] = -1;
	if (mode & READ) {
		if (pipe(p) != 0)
			fatal("pipe:");
		if (fcntl(p[0], F_SETFD, FD_CLOEXEC) != 0)
			fatal("fcntl FD_CLOEXEC:");
		if (fcntl(p[1], F_SETFD, FD_CLOEXEC) != 0)
			fatal("fcntl FD_CLOEXEC:");
		t[1] = p[1];
		p[1] = fd[0];
	}
	if (mode & WRITE) {
		if (pipe(p + 2) != 0)
			fatal("pipe:");
		if (fcntl(p[2], F_SETFD, FD_CLOEXEC) != 0)
			fatal("fcntl FD_CLOEXEC:");
		if (fcntl(p[3], F_SETFD, FD_CLOEXEC) != 0)
			fatal("fcntl FD_CLOEXEC:");
		t[0] = p[2];
		p[2] = p[3];
		p[3] = fd[1];
	}
	if (mode == (READ | WRITE) && p[1] == p[2]) {
		/* swap dup order to avoid clobber */
		p[1] = p[3];
		p[3] = p[2];
		p[2] = p[0];
		p[0] = p[3];
	}
	pid = fork();
	if (pid == -1)
		fatal("fork");
	if (pid == 0) {
		close(p[0]);
		close(p[2]);
		fd[0] = t[0];
		fd[1] = t[1];
	} else {
		if (p[0] != -1 && dup2(p[0], p[1]) < 0)
			fatal("dup2:");
		if (p[2] != -1 && dup2(p[2], p[3]) < 0)
			fatal("dup2:");
		execvp(argv[0], argv);
		fatal("exec %s:", argv[0]);
	}
}

