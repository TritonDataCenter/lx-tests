/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "lxtst.h"

#define TST_NAME	"memcntl"
#define ONE_MB		(1024 * 1024)

static int tc;
static unsigned long pagesize = 0;
static char *data = NULL;

struct memtest {
	void	*addr;
	size_t	len;
	int	flag;
	int	err;
	char	*name;
};

static void
tfail(char *msg)
{
	printf("FAIL %s %d: %s\n", TST_NAME, tc, msg);
	exit(1);
}

static void
test_mlock()
{
	struct memtest tests[] = {
		{0, 0, 0, 0, "mlock len=0"},
		{0, 0, 1, 0, "munlock len=0"},
		{data, 0UL-pagesize, 0, EINVAL, "mlock len overflow"},
		{data, 0UL-pagesize, 1, EINVAL, "munlock len overflow"},
		{data + pagesize + 1, pagesize, 0, ENOMEM,
		    "mlock round up overflow"},
		{data + pagesize + 1, pagesize, 1, ENOMEM,
		    "munlock round up overflow"},
		{data, pagesize, 0, 0, "mlock success"},
		{data, pagesize, 1, 0, "munlock success"},
		{data + 1, pagesize, 0, 0, "mlock success"},
		{data, pagesize, 1, 0, "munlock success"}
	};
	unsigned len = sizeof (tests) / sizeof (tests[0]);
	char e[80];

	for (unsigned i = 0; i < len; i++) {
		errno = 0;
		/* flag indicates mlock vs munlock in this case */
		if (tests[i].flag == 0) {
			mlock(tests[i].addr, tests[i].len);
		} else {
			munlock(tests[i].addr, tests[i].len);
		}
		if (tests[i].err != errno) {
			snprintf(e, sizeof (e), "%s unexpected error %d",
			    tests[i].name, errno);
			tfail(e);
		}

		tc++;
	}
}

static void
test_madvise()
{
	struct memtest tests[] = {
		{0, 0, 0, 0, "madvise len=0"},
		{0, 0, -1, EINVAL, "madvise invalid behavior"},
		{data, 0UL-pagesize, MADV_NORMAL, EINVAL,
		    "madvise len overflow"},
		{data + 1, pagesize, MADV_NORMAL, EINVAL, "madvise offset"},
		{data, pagesize, MADV_NORMAL, 0, "madvise success"}
	};
	unsigned len = sizeof (tests) / sizeof (tests[0]);
	char e[80];

	for (unsigned i = 0; i < len; i++) {
		errno = 0;
		madvise(tests[i].addr, tests[i].len, tests[i].flag);
		if (tests[i].err != errno) {
			snprintf(e, sizeof (e), "%s unexpected error %d",
			    tests[i].name, errno);
			tfail(e);
		}

		tc++;
	}
}

static void
test_mprotect()
{
	struct memtest tests[] = {
		{0, 0, 0, 0, "mprotect len=0"},
		{0, 0, PROT_GROWSUP|PROT_GROWSDOWN, EINVAL,
		    "mprotect invalid prot"},
		{data, 0UL-pagesize, PROT_READ|PROT_WRITE, ENOMEM,
		    "mprotect len overflow"},
		{data + 1, pagesize, PROT_READ|PROT_WRITE, EINVAL,
		    "mprotect unaligned"},
		{data, pagesize, PROT_READ|PROT_WRITE, 0, "mprotect success"}
	};
	unsigned len = sizeof (tests) / sizeof (tests[0]);
	char e[80];

	for (unsigned i = 0; i < len; i++) {
		errno = 0;
		mprotect(tests[i].addr, tests[i].len, tests[i].flag);
		if (tests[i].err != errno) {
			snprintf(e, sizeof (e), "%s unexpected error %d",
			    tests[i].name, errno);
			tfail(e);
		}

		tc++;
	}
}

static void
test_msync()
{
	struct memtest tests[] = {
		{0, 0, 0, 0, "msync len=0"},
		{0, 0, -1, EINVAL, "msync invalid flag"},
		{data, 0UL-pagesize, MS_SYNC, ENOMEM, "msync len overflow"},
		{data + 1, pagesize, MS_SYNC, EINVAL, "msync offset"},
		{data, pagesize, MS_SYNC, 0, "msync success"}
	};
	unsigned len = sizeof (tests) / sizeof (tests[0]);
	char e[80];

	for (unsigned i = 0; i < len; i++) {
		errno = 0;
		msync(tests[i].addr, tests[i].len, tests[i].flag);
		if (tests[i].err != errno) {
			snprintf(e, sizeof (e), "%s unexpected error %d",
			    tests[i].name, errno);
			tfail(e);
		}

		tc++;
	}
}

static void
test_lock_limits(char *p)
{
	struct rlimit l;
	char e[80];
	int am_root = (geteuid() == 0);
	int r;

	/*
	 * We start by locking with whatever the current limit is.
	 */
	if (getrlimit(RLIMIT_MEMLOCK, &l) != 0) {
		snprintf(e, sizeof (e), "getrlimit %d", errno);
		tfail(e);
	}

	if (l.rlim_cur < (10 * ONE_MB)) {
		snprintf(e, sizeof (e),
		    "current locked mem limit is too low %ld",
		    (long)l.rlim_cur);
		tfail(e);
	}

	if (mlockall(MCL_CURRENT) != 0)
		tfail("mlockall");

	tc++;
	if (munlockall() != 0)
		tfail("munlockall");

	tc++;
	if (mlock(p, pagesize) != 0)
		tfail("mlock");

	tc++;
	if (munlock(p, pagesize) != 0)
		tfail("munlock");

	/*
	 * Now we set a pretty low limit, but one we should still pass.
	 * NOTE: if the size of this test program expands, the 8MB limit
	 * may need to increase.
	 */
	l.rlim_max = 10 * ONE_MB;
	l.rlim_cur = 8 * ONE_MB;

	tc++;
	if (setrlimit(RLIMIT_MEMLOCK, &l) != 0) {
		perror("setrlimit");
		exit(1);
	}

	if (mlockall(MCL_CURRENT) != 0)
		tfail("mlockall");

	tc++;
	if (munlockall() != 0)
		tfail("munlockall");

	tc++;
	if (mlock(p, pagesize) != 0)
		tfail("mlock");

	tc++;
	if (munlock(p, pagesize) != 0)
		tfail("munlock");

	/*
	 * Now we set a limit of 0. We should fail if we're running as a
	 * regular user, but pass if we're running as a privileged user.
	 */
	l.rlim_max = 10 * ONE_MB;
	l.rlim_cur = 0;

	tc++;
	if (setrlimit(RLIMIT_MEMLOCK, &l) != 0) {
		perror("setrlimit");
		exit(1);
	}

	r = mlockall(MCL_CURRENT);
	if ((am_root && r != 0) || (!am_root && r == 0))
		tfail("mlockall");

	if (am_root) {
		tc++;
		if (munlockall() != 0)
			tfail("munlockall");
	}

	tc++;
	r = mlock(p, pagesize);
	if ((am_root && r != 0) || (!am_root && r == 0))
		tfail("mlock");

	if (am_root) {
		tc++;
		if (munlock(p, pagesize) != 0)
			tfail("munlock");
	}
}

int
main(int argc, char **argv)
{
	pagesize = (unsigned long)sysconf(_SC_PAGESIZE);
	if (pagesize == -1) {
		test_fail("memcntl sysconf()", strerror(errno));
		exit(EXIT_FAILURE);
	}

	/*
	 * Map four pages and then unmap the trailing two pages.  This should
	 * guarantee that anything that falls off the end of the second page
	 * will land in an unmapped region
	 */
	data = mmap(0, 4 * pagesize, PROT_READ|PROT_WRITE,
	    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (data == NULL) {
		test_fail("memcntl mmap()", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (munmap(data + (pagesize * 2), pagesize * 2) != 0) {
		test_fail("memcntl munmap()", strerror(errno));
		exit(EXIT_FAILURE);
	}

	tc = 1;

	test_mlock();
	test_madvise();
	test_mprotect();
	test_msync();
	test_lock_limits(data);

	return (test_pass(TST_NAME));
}
