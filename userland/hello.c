/*
 * The child process our launchd execs, so that a fork + execve round trip
 * through the kernel is visible on the screen.
 */
#include "polaris.h"

void polaris_start(void)
{
	static const char hello[] = "POLARIS HELLO FROM USER SPACE\n";
	int fd;

	fd = p_open("/dev/console", PO_WRONLY, 0);
	if (fd < 0) {
		fd = 1;
	}
	p_puts(fd, hello);
	p_exit(7);
}
