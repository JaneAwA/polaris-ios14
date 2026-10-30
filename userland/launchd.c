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
 *   1. vfork() followed by execve().  The child borrows this task and this
 *      address space, so nothing has to be copied and the kernel only builds
 *      a task when the child execs -- the one path that has been getting as
 *      far as printing "HELLO pid" before the number itself.
 *   2. fork() with no execve() at all.  The child keeps this code and this
 *      stack, so it separates "a second process cannot touch its stack"
 *      from "something execve() builds is wrong".
 *
 * init itself must never exit, because the kernel panics when pid 1 goes
 * away, so both children are waited for and then the process just reaps.
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
	/*
	 * Static, not automatic: an earlier build died on the very first
	 * write to this buffer, which is what a broken user stack looks
	 * like.  With the buffer in .bss the number still comes out, so
	 * "the process cannot print" stops being indistinguishable from
	 * "the process cannot touch its stack".
	 */
	static char buf[24];
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
	static const char forking[] = "POLARIS LAUNCHD: fork (no exec)\n";
	static const char child_alive[] = "POLARIS CHILD: alive, my pid = ";
	static const char failed[] = "POLARIS LAUNCHD: execve failed\n";
	static const char running[] = "POLARIS LAUNCHD: child running\n";
	char *argv[2];
	char *envp[2];
	int fd;
	int cfd;
	long pid;
	long r;
	int status;

	fd = p_open(CONSOLE, PO_WRONLY, 0);
	cfd = fd;
	if (fd < 0) {
		/*
		 * No console is not fatal: the process still has to stay
		 * alive, or the kernel panics with "initproc exited".
		 */
		fd = 1;
	} else {
		/*
		 * Pin the console onto the three descriptors every program
		 * assumes.  open() hands out 3 here, so a child that execs and
		 * writes to fd 1 -- which is what /sbin/hello does -- would be
		 * writing to a descriptor that was never opened: EBADF, no
		 * bytes, no message, exit status 0.  That looks exactly like
		 * "the second process died", and it is why the panel showed a
		 * hello that never printed a single character.
		 */
		(void)p_dup2(fd, 0);
		(void)p_dup2(fd, 1);
		(void)p_dup2(fd, 2);
		if (fd > 2) {
			(void)p_close(fd);
		}
		fd = 1;
	}

	say(fd, alive);
	say_val(fd, "POLARIS LAUNCHD: my pid = ", p_getpid());
	say_val(fd, "POLARIS LAUNCHD: console open = ", cfd);

	argv[0] = (char *)"/sbin/hello";
	argv[1] = 0;
	envp[0] = (char *)"PATH=/sbin:/usr/bin";
	envp[1] = 0;

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

	/*
	 * Experiment 2: fork() without execve().
	 *
	 * The child gets a copy of this address space and keeps running this
	 * very code, stack bytes included.  If it manages to print a number,
	 * a copied address space and its stack are fine, and whatever breaks
	 * belongs to what execve() builds -- which is a much smaller place to
	 * look than "the second process".
	 */
	say(fd, forking);
	pid = p_fork();
	say_val(fd, "POLARIS LAUNCHD: fork -> ", pid);
	if (pid == 0) {
		/*
		 * say_num() prints out of .bss, so a child that manages to
		 * print its pid has said nothing at all about the stack it
		 * inherited.  This frame is the first thing in the whole
		 * program that forces a stack page in: one marker before the
		 * first byte, one between the two writes, one after.
		 */
		volatile unsigned char stack_probe[512];
		unsigned long sp;

		__asm__ volatile("mov %0, sp" : "=r"(sp));
		say(fd, child_alive);
		say_num(fd, p_getpid());
		say(fd, "\n");
		say_val(fd, "POLARIS CHILD: sp = ", (long)sp);
		say_val(fd, "POLARIS CHILD: frame = ", (long)(unsigned long)stack_probe);
		stack_probe[0] = 0x11;
		say(fd, "POLARIS CHILD: stack byte 0 written\n");
		stack_probe[511] = 0x22;
		say(fd, "POLARIS CHILD: stack byte 511 written\n");
		say_val(fd, "POLARIS CHILD: probe = ",
		    (long)(stack_probe[0] + stack_probe[511]));
		p_sleep_ms(150);
		p_exit(0);
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
