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
	char buf[24];
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
	long w;
	int fd;

	fd = p_open("/dev/console", PO_WRONLY, 0);
	if (fd < 0) {
		fd = 1;
	}
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
	 * Leave on purpose, but not instantly: give the console time to put
	 * these lines on the panel, then hand control back to init.
	 */
	p_sleep_ms(400);
	p_exit(0);
}
