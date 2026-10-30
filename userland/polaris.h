/*
 * Raw system call wrappers for the polaris iOS 14 port.
 *
 * There is no libSystem on this board yet (no dyld, no shared cache), so the
 * userland talks to the kernel directly: the number goes into x16, the
 * arguments into x0-x2, and "svc #0x80" does the rest.  On Darwin a failed
 * system call returns the errno in x0 and sets the carry flag, so the wrapper
 * turns that into a negative return value the way every other Unix does.
 */
#ifndef POLARIS_H
#define POLARIS_H

/*
 * ld64 builds the stub machinery of a normal executable and then wants
 * dyld_stub_binder to fill it in.  There is no dyld on this board and
 * nothing is ever stubbed, but the link fails without the symbol, so give
 * it an empty one (weak, so two programs can be linked into one).
 */
__attribute__((weak)) void dyld_stub_binder(void) { }

#define PSYS_exit      1
#define PSYS_fork      2
#define PSYS_read      3
#define PSYS_write     4
#define PSYS_open      5
#define PSYS_close     6
#define PSYS_wait4     7
#define PSYS_getpid   20
#define PSYS_execve   59

#define PO_RDONLY   0x0000
#define PO_WRONLY   0x0001
#define PO_RDWR     0x0002

static inline long psys(long no, long a0, long a1, long a2, int *err)
{
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x16 __asm__("x16") = no;
	long e = 0;

	__asm__ volatile("svc #0x80\n\t"
	    "cset %w1, cs"
	    : "+r"(x0), "=&r"(e)
	    : "r"(x1), "r"(x2), "r"(x16)
	    : "cc", "memory");

	if (err != 0) {
		*err = (int)e;
	}
	return x0;
}

/* A failed call returns -errno; a successful one returns what the kernel gave. */
static inline long psys_ret(long no, long a0, long a1, long a2)
{
	int err = 0;
	long r = psys(no, a0, a1, a2, &err);

	return err ? -r : r;
}

static inline int p_open(const char *path, int flags, int mode)
{
	return (int)psys_ret(PSYS_open, (long)path, flags, mode);
}

static inline long p_read(int fd, void *buf, unsigned long n)
{
	return psys_ret(PSYS_read, fd, (long)buf, (long)n);
}

static inline long p_write(int fd, const void *buf, unsigned long n)
{
	return psys_ret(PSYS_write, fd, (long)buf, (long)n);
}

static inline int p_close(int fd)
{
	return (int)psys_ret(PSYS_close, fd, 0, 0);
}

/*
 * fork() does not return 0 in the child on this kernel.
 *
 * thread_set_child() in osfmk/arm64/status.c hands the child the pid in x0 and
 * the constant 1 in x1; libsyscall is what turns that into the 0 that user
 * code expects.  Reading only x0 makes both processes believe they are the
 * parent, which is exactly what the panel showed ("fork -> 0x2 pid 0x2", then
 * the same wait/exec sequence twice).
 */
static inline long p_fork(void)
{
	register long x0 __asm__("x0") = 0;
	register long x1 __asm__("x1") = 0;
	register long x16 __asm__("x16") = PSYS_fork;
	int e = 0;

	__asm__ volatile("svc #0x80\n\t"
	    "cset %w2, cs"
	    : "+r"(x0), "+r"(x1), "=&r"(e)
	    : "r"(x16)
	    : "cc", "memory");

	if (e != 0) {
		return -x0;
	}
	return (x1 != 0) ? 0 : x0;
}

static inline long p_execve(const char *path, char *const argv[], char *const envp[])
{
	return psys_ret(PSYS_execve, (long)path, (long)argv, (long)envp);
}

static inline long p_wait4(int pid, int *status, int options)
{
	return psys_ret(PSYS_wait4, pid, (long)status, options);
}

/*
 * Lets a probe line say which process printed it.  Without this a child
 * that came back from fork() with the parent's return value is
 * indistinguishable from the parent itself.
 */
static inline long p_getpid(void)
{
	return psys_ret(PSYS_getpid, 0, 0, 0);
}

static inline void p_exit(int code)
{
	psys(PSYS_exit, code, 0, 0, 0);
	for (;;) {
		/* exit() does not return; if it somehow does, keep the pid alive. */
	}
}

static inline unsigned long p_strlen(const char *s)
{
	unsigned long n = 0;

	while (s[n] != '\0') {
		n++;
	}
	return n;
}

static inline void p_puts(int fd, const char *s)
{
	p_write(fd, s, p_strlen(s));
}

#endif /* POLARIS_H */
