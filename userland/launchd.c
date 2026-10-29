/*
 * Our own init for the polaris iOS 14 port.
 *
 * Apple ships launchd, but it needs dyld, the 1 GB shared cache, the
 * sandbox and a pile of entitlements -- none of which exist on this board.
 * So the first process here is written from scratch: it talks to the kernel
 * through raw system calls, prints on /dev/console (the only console we
 * have), forks a child to prove that exec works, and then reaps children
 * for as long as it lives.
 *
 * It must never exit: the kernel panics when pid 1 goes away.
 */
#include "polaris.h"

#define CONSOLE "/dev/console"

static void say(int fd, const char *s)
{
	p_puts(fd, s);
}

void _start(void)
{
	static const char alive[] = "POLARIS LAUNCHD ALIVE\n";
	static const char forking[] = "POLARIS LAUNCHD: forking /sbin/hello\n";
	static const char forked[] = "POLARIS LAUNCHD: child forked\n";
	static const char reaped[] = "POLARIS LAUNCHD: child reaped\n";
	static const char failed[] = "POLARIS LAUNCHD: execve failed\n";
	char *argv[2];
	char *envp[2];
	int fd;
	long pid;

	fd = p_open(CONSOLE, PO_WRONLY, 0);
	if (fd < 0) {
		/*
		 * No console is not fatal: the process still has to stay
		 * alive, or the kernel panics with "initproc exited".
		 */
		fd = 1;
	}

	say(fd, alive);
	say(fd, forking);

	pid = p_fork();
	if (pid == 0) {
		argv[0] = (char *)"/sbin/hello";
		argv[1] = 0;
		envp[0] = (char *)"PATH=/sbin:/usr/bin";
		envp[1] = 0;
		if (p_execve("/sbin/hello", argv, envp) < 0) {
			say(fd, failed);
			p_exit(1);
		}
		p_exit(0);
	}

	say(fd, forked);

	for (;;) {
		int status = 0;

		if (p_wait4(-1, &status, 0) > 0) {
			say(fd, reaped);
		}
	}
}
