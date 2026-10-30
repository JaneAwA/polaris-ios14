/*
 * The program our init starts, so that a second process (new task, new
 * address space, new image) is visible on the screen.
 *
 * It prints, waits long enough for the writes to reach the console, and then
 * exits on purpose: after a vfork() the parent is blocked until this process
 * execs or exits, and after a fork() it is waiting in wait4(), so init only
 * gets to run its next experiment once this one is gone.
 */
#include "polaris.h"

static void say(int fd, const char *s)
{
	p_puts(fd, s);
}

static void say_num(int fd, long v)
{
	static const char digits[] = "0123456789abcdef";
	/* Static buffer: see the note in launchd.c -- the first write to an
	 * automatic buffer is the first thing that dies on a bad stack. */
	static char buf[24];
	int i = 23;
	int neg = 0;

	buf[i] = '\0';
	if (v < 0) {
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
	static const char hello[] = "POLARIS HELLO FROM USER SPACE\n";
	static const char mark[] = "POLARIS HELLO MARK\n";
	long w;
	int fd;
	int probe;

	fd = p_open("/dev/console", PO_WRONLY, 0);
	if (fd < 0) {
		/*
		 * No console from open(): fall back to whichever descriptor
		 * survived execve().  init pins the console onto 0/1/2 before
		 * it forks, so 1 is expected to be the one that answers; the
		 * loop is there because "nothing was printed" from this side
		 * of the panel looks the same whether the write failed with
		 * EBADF or the process never ran at all.  The marker is
		 * written before any other output, so a single line on the
		 * panel tells both that this image is alive and which
		 * descriptor carried the bytes.
		 */
		for (probe = 1; probe <= 5; probe++) {
			if (p_write(probe, mark, sizeof(mark) - 1) ==
			    (long)(sizeof(mark) - 1)) {
				fd = probe;
				break;
			}
		}
	}
	if (fd < 0) {
		fd = 1;
	}

	/* Negative means the open failed; the value is the descriptor used. */
	say_val(fd, "POLARIS HELLO fd = ", fd);
	say(fd, "POLARIS HELLO pid ");
	say_num(fd, p_getpid());
	say(fd, "\n");

	/*
	 * Print the write() result as well: a short or negative count here
	 * tells a console that refused the bytes apart from a process that
	 * died before it got the chance to send them.
	 */
	w = p_write(fd, hello, sizeof(hello) - 1);
	say_val(fd, "POLARIS HELLO write -> ", w);

	/*
	 * Every print so far came out of .text and .bss, so none of them
	 * proves that this process can touch the stack that execve() built
	 * for it.  Do it explicitly, with a marker on each side of the
	 * first store: whichever line is the last one on the panel names
	 * the instruction that kills the process.
	 */
	{
		volatile unsigned char stack_probe[512];
		unsigned long sp;

		__asm__ volatile("mov %0, sp" : "=r"(sp));
		say_val(fd, "POLARIS HELLO sp = ", (long)sp);
		say_val(fd, "POLARIS HELLO frame = ",
		    (long)(unsigned long)stack_probe);
		stack_probe[0] = 0x33;
		say(fd, "POLARIS HELLO: stack byte 0 written\n");
		stack_probe[511] = 0x44;
		say(fd, "POLARIS HELLO: stack byte 511 written\n");
		say_val(fd, "POLARIS HELLO probe = ",
		    (long)(stack_probe[0] + stack_probe[511]));
	}

	/*
	 * Leave on purpose, but not instantly: give the console time to put
	 * these lines on the panel, then hand control back to init.
	 */
	p_sleep_ms(400);
	p_exit(0);
}
