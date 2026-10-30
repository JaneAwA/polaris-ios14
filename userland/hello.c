/*
 * The program our init execs, so that a second exec (new task, new address
 * space, new image) is visible on the screen.
 *
 * It must never exit on its own: when the parent execs this image the
 * process is still pid 1, and the kernel panics if pid 1 goes away.
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
	 * Stay alive without spinning: wait4() on an empty child list returns
	 * immediately, so sleeping is what keeps the CPU free (cpus=1) for
	 * the console to drain.
	 */
	for (;;) {
		p_sleep_ms(500);
	}
}
