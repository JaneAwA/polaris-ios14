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

/*
 * The screen is the only diagnostic channel, and "child forked" used to be
 * printed whether fork() worked or not -- a failed fork returns -errno and
 * fell straight through to that message.  Print the raw value instead, so a
 * photo tells us which of the two happened.
 */
static void say_num(int fd, long v)
{
	static const char digits[] = "0123456789abcdef";
	char buf[24];
	int i = 23;

	buf[i] = '\0';
	if (v < 0) {
		/* -errno: print the errno, not its two's complement. */
		v = -v;
		buf[--i] = '-';
	}
	do {
		buf[--i] = digits[v & 0xfl];
		v >>= 4;
	} while (v != 0);
	buf[--i] = 'x';
	buf[--i] = '0';
	p_puts(fd, &buf[i]);
}

void polaris_start(void)
{
	static const char alive[] = "POLARIS LAUNCHD ALIVE\n";
	static const char forking[] = "POLARIS LAUNCHD: forking /sbin/hello\n";
	static const char forked[] = "POLARIS LAUNCHD: child forked\n";
	static const char reaped[] = "POLARIS LAUNCHD: child reaped\n";
	static const char failed[] = "POLARIS LAUNCHD: execve failed\n";
	static const char running[] = "POLARIS LAUNCHD: child running\n";
	char *argv[2];
	char *envp[2];
	int fd;
	long pid;
	long r;
	int waits = 0;

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
	say(fd, "POLARIS LAUNCHD: fork -> ");
	say_num(fd, pid);
	say(fd, "\n");

	if (pid == 0) {
		/* Reaching this line proves fork returned to user space. */
		say(fd, running);
		argv[0] = (char *)"/sbin/hello";
		argv[1] = 0;
		envp[0] = (char *)"PATH=/sbin:/usr/bin";
		envp[1] = 0;
		r = p_execve("/sbin/hello", argv, envp);
		say(fd, "POLARIS LAUNCHD: execve -> ");
		say_num(fd, r);
		say(fd, "\n");
		say(fd, failed);
		p_exit(1);
	}

	if (pid < 0) {
		say(fd, "POLARIS LAUNCHD: fork failed\n");
	} else {
		say(fd, forked);
	}

	for (;;) {
		int status = 0;

		r = p_wait4(-1, &status, 0);
		if (r > 0) {
			say(fd, reaped);
		} else if (waits < 3) {
			/*
			 * ECHILD on a machine with no children would spin here
			 * silently, which is exactly how the last boot ended.
			 */
			waits++;
			say(fd, "POLARIS LAUNCHD: wait -> ");
			say_num(fd, r);
			say(fd, "\n");
		}
	}
}
