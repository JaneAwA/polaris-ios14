/*
 * Our own init for the polaris iOS 14 port.
 *
 * Apple ships launchd, but it needs dyld, the 1 GB shared cache, the
 * sandbox and a pile of entitlements -- none of which exist on this board.
 * So the first process here is written from scratch: it talks to the kernel
 * through raw system calls, prints on /dev/console (the only console we
 * have), starts children to prove that a second process works, and then
 * reaps them for as long as it lives.
 *
 * Two experiments, in this order:
 *
 *   1. vfork().  The child borrows this task and this address space, so
 *      nothing has to be copied and no second pmap has to be built before
 *      execve() gives the child a task of its own.
 *   2. fork().  A real child with a copied address space, which is the path
 *      that has been failing: the child disappears before it prints its
 *      return value.
 *
 * Running both in one boot says whether the problem is "a second process"
 * or specifically "a copied address space".  The payload exits on its own
 * after a moment so that the parent gets control back and the second
 * experiment can run; init itself must never exit, because the kernel
 * panics when pid 1 goes away.
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
	static const char vforking[] = "POLARIS LAUNCHD: vfork /sbin/hello\n";
	static const char forking[] = "POLARIS LAUNCHD: fork /sbin/hello\n";
	static const char failed[] = "POLARIS LAUNCHD: execve failed\n";
	static const char running[] = "POLARIS LAUNCHD: child running\n";
	char *argv[2];
	char *envp[2];
	int fd;
	long pid;
	long r;
	int status;

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

	say(fd, alive);
	say_val(fd, "POLARIS LAUNCHD: my pid = ", p_getpid());

	/* Experiment 1: a child that borrows this task until it execves. */
	say(fd, vforking);
	pid = p_vfork();
	say_val(fd, "POLARIS LAUNCHD: vfork -> ", pid);
	if (pid == 0) {
		/* Reaching this line proves vfork returned to user space. */
		say(fd, running);
		r = p_execve("/sbin/hello", argv, envp);
		say_val(fd, "POLARIS LAUNCHD: vfork execve -> ", r);
		say(fd, failed);
		p_exit(1);
	}

	/* Experiment 2: a real child, i.e. a copied address space. */
	say(fd, forking);
	pid = p_fork();
	say_val(fd, "POLARIS LAUNCHD: fork -> ", pid);
	if (pid == 0) {
		say(fd, running);
		r = p_execve("/sbin/hello", argv, envp);
		say_val(fd, "POLARIS LAUNCHD: fork execve -> ", r);
		say(fd, failed);
		p_exit(1);
	}

	/*
	 * Reaper.  The raw status is the only witness to why a child went
	 * away: low byte is the signal when WIFSIGNALED, otherwise
	 * (code << 8).  Sleep on ECHILD instead of spinning -- cpus=1, and a
	 * busy init starves the console.
	 *
	 * Only the first few failures are printed: this loop runs for the
	 * lifetime of the machine, and a line every 300 ms scrolls everything
	 * else off the panel (an earlier build sat there printing
	 * "wait -> -0xe" forever, which is all the screen showed).
	 */
	{
		int noisy = 3;

		for (;;) {
			status = 0;
			r = p_wait4(-1, &status, 0);
			if (r > 0) {
				say_val(fd, "POLARIS LAUNCHD: reaped pid ", r);
				say_val(fd, "POLARIS LAUNCHD: reaped status ", status);
			} else if (noisy > 0) {
				noisy--;
				say_val(fd, "POLARIS LAUNCHD: wait -> ", r);
			}
			p_sleep_ms(300);
		}
	}
}
