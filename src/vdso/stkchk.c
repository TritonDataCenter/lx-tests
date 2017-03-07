/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <signal.h>
#include <ucontext.h>
#include <elf.h>
#include <time.h>
#include <string.h>

extern void vdso_init_from_sysinfo_ehdr(uintptr_t);
extern void *vdso_sym(const char *, const char *);
extern long run_stack_func(void *, unsigned, void (*)(uintptr_t, uintptr_t),
    uintptr_t *);


#define	STACK_BUDGET	104
#define	CLOCK_IDENT	CLOCK_REALTIME

static volatile int	segv_blown = 0;
static void		*segv_stk;
static ucontext_t	segv_ctx;
static struct timespec	ts;
static char		*progname = NULL;


static void
pass()
{
	printf("PASS vdso_%s\n", progname);
	exit(0);
}

static void
fail(char *msg)
{
	if (errno != 0) {
		printf("FAIL vdso_%s - %s: %s\n", progname, msg,
		    strerror(errno));
	} else {
		printf("FAIL vdso_%s - %s\n", progname, msg);
	}
	exit(1);
}

static void
segv_handler()
{
	segv_blown = 1;
	setcontext(&segv_ctx);
}

/*
 * Allocate a test stack.  Mark lower-addressed half as PROT_NONE so that any
 * incursions into it will result in SEGV.
 */
static void *
init_test_stack(long size)
{
	void *alloc;

	alloc = mmap(NULL, size, PROT_READ|PROT_WRITE,
	    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (alloc == NULL) {
		fail("mmap");
	}
	if (mprotect(alloc, (size >> 1), PROT_NONE) != 0)
		fail("mprotect");

	return (alloc);
}

static void
init_signal_handler()
{
	void *stkp;
	stack_t stk = { 0 };
	struct sigaction sa = { 0 };

	stkp = mmap(NULL, SIGSTKSZ, PROT_READ|PROT_WRITE,
	    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (stkp == NULL)
		fail("mmap");

	stk.ss_sp = stkp;
	stk.ss_size = SIGSTKSZ;
	if (sigaltstack(&stk, NULL) != 0)
		fail("sigaltstack");

	sa.sa_sigaction  = segv_handler;
	sa.sa_flags = SA_ONSTACK;
	if (sigaction(SIGSEGV, &sa, NULL) != 0)
		fail("sigaction");
}

static void *
init_vdso()
{
	uintptr_t ehdrp = 0;
	void *sym;

	ehdrp = getauxval(AT_SYSINFO_EHDR);
	vdso_init_from_sysinfo_ehdr(ehdrp);
	sym = vdso_sym("LINUX_2.6", "__vdso_clock_gettime");
	if (sym == NULL) {
		errno = 0;
		fail("vdso_sym failure");
	}
	return (sym);
}

int
main(int argc, char *argv[])
{
	void *clock_gettime_sym, *stk_alloc, *stk_run;
	long pagesize;

	progname = basename(argv[0]);

	clock_gettime_sym = init_vdso();

	errno = 0;
	pagesize = sysconf(_SC_PAGESIZE);
	if (errno != 0)
		fail("sysconf");

	stk_alloc = init_test_stack(pagesize * 2);
	init_signal_handler();

	segv_blown = 0;
	getcontext(&segv_ctx);

	if (!segv_blown) {
		uintptr_t args[2] = { CLOCK_IDENT, (uintptr_t)&ts };
		long result;

		stk_run = (void *)((uintptr_t)stk_alloc + pagesize);
		result = run_stack_func(stk_run, STACK_BUDGET,
		    clock_gettime_sym, args);
		pass();
	} else {
		fail("Blew stack budget");
	}

	return (0);
}
