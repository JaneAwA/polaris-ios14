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
 * Hex, with the sign where a human expects it.
 *
 * The first version wrote the '-' into the top of the buffer and then
 * prepended the digits, so -ECHILD came out as "0xa-" and read like a
 * stray trailing dash.  Same bug made a failed fork look like a positive
 * return value.
 */
static void say_num(int fd, long v)
{
	static const char digits[] = "0123456789abcdef";
	char buf[24];
	int i = 23;
	int neg = 0;

	buf[i] = '\0';
	if (v < 0) {
		/* -errno: print the errno, not its two's complement. */
		v = -v;
		neg = 1;
	}
	do {
		buf[--i] = digits[v & 0xfl];
		v >>= 4;
	} while (v != 0);
	buf[--i] = 'x';
	buf[--i] = '0';
	if (neg) {
		buf[--i] = '-';
	}
	p_puts(fd, &buf[i]);
}

/* Print "label <value>" on one line; used by every probe below. */
static void say_val(int fd, const char *label, long v)
{
	say(fd, label);
	say_num(fd, v);
	say(fd, "\n");
}

void polaris_start(void)
{
	static const char alive[] = "POLARIS LAUNCHD ALIVE\n";
	static const char forking[] = "POLARIS LAUNCHD: forking /sbin/hello\n";
	static const char forked[] = "POLARIS LAUNCHD: child forked\n";
	static const char reaped[] = "POLARIS LAUNCHD: child reaped\n";
	static const char failed[] = "POLARIS LAUNCHD: execve failed\n";
	static const char running[] = "POLARIS LAUNCHD: child running\n";
	static const char direct[] = "POLARIS LAUNCHD: no waitable child, exec direct\n";
	char *argv[2];
	char *envp[2];
	int fd;
	long pid;
	long me;
	long r;
	int waits = 0;
	int tried_direct = 0;
	int had_child = 0;

	fd = p_open(CONSOLE, PO_WRONLY, 0);
	if (fd < 0) {
		/*
		 * No console is not fatal: the process still has to stay
		 * alive, or the kernel panics with "initproc exited".
		 */
		fd = 1;
	}

	argv[0] = (char *)"/sbin/hello";
	argv[1] = 0;
	envp[0] = (char *)"PATH=/sbin:/usr/bin";
	envp[1] = 0;

	me = p_getpid();
	say(fd, alive);
	say_val(fd, "POLARIS LAUNCHD: my pid = ", me);
	say(fd, forking);

	pid = p_fork();
	say(fd, "POLARIS LAUNCHD: fork -> ");
	say_num(fd, pid);
	say(fd, " pid ");
	say_num(fd, p_getpid());
	say(fd, "\n");

	if (pid == 0) {
		/* Reaching this line proves fork returned to user space. */
		say(fd, running);
		r = p_execve("/sbin/hello", argv, envp);
		say_val(fd, "POLARIS LAUNCHD: execve -> ", r);
		say(fd, failed);
		p_exit(1);
	}

	if (pid < 0) {
		say(fd, "POLARIS LAUNCHD: fork failed\n");
	} else {
		had_child = 1;
		say(fd, forked);
	}

	for (;;) {
		int status = 0;

		r = p_wait4(-1, &status, 0);
		if (r > 0) {
			say(fd, reaped);
			continue;
		}

		/*
		 * ECHILD here means there is no child to wait for at all, which
		 * is what a fork that never produced one looks like from the
		 * outside.  Do not sit in that loop forever -- exec the payload
		 * directly so one boot still tells us whether a second exec
		 * works.  Once a child has existed, ECHILD is the ordinary end
		 * state of a reaper, so leave init in place instead.
		 */
		if (waits < 3) {
			waits++;
			say_val(fd, "POLARIS LAUNCHD: wait -> ", r);
		}
		if (!had_child && waits >= 3 && !tried_direct) {
			tried_direct = 1;
			say(fd, direct);
			r = p_execve("/sbin/hello", argv, envp);
			say_val(fd, "POLARIS LAUNCHD: direct execve -> ", r);
		}
	}
}
