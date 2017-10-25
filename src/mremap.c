/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include "lxtst.h"

#define _GNU_SOURCE 
#include <sys/mman.h>
#include <linux/mman.h>

extern void *mremap (void *__addr, size_t __old_len, size_t __new_len,
    int __flags, ...);

#define	ONEMB	(1024 * 1024)

static int tc;

static void
tfail(char *msg)
{
        printf("FAIL mremap %d: %s\n", tc, msg);
        exit(1);
}

/*
static void
t_err(char *msg, int rc, int en)
{
	char e[80];

	snprintf(e, sizeof (e), "%s %d %d", msg, rc, en);
	tfail(e);
}
*/

static void
run_test(int testcase, void (*tp)())
{
	int pid;
	int status;

	tc = testcase;;

	pid = fork();
	if (pid < 0)
		tfail("fork failed");

	if (pid == 0) {
		tp();
	}

	if (waitpid(pid, &status, 0) < 0)
		tfail("waitpid");

	if (!WIFEXITED(status))
		tfail("abnormal exit");

	if (WEXITSTATUS(status) != 0)
		tfail("error exit");
}

/*
 * Anonymous mapping. Expand it, shrink it, then expand it big.
 */
static void
test1()
{
	void *a;

	a = mmap(NULL, 20 * ONEMB, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED)
		tfail("mmap failed");

	a = mremap(a, 20 * ONEMB, 21 * ONEMB, MREMAP_MAYMOVE);
	if (a == MAP_FAILED)
		tfail("mremap failed");

	a = mremap(a, 21 * ONEMB, 20 * ONEMB, MREMAP_MAYMOVE);
	if (a == MAP_FAILED)
		tfail("mremap failed");

	a = mremap(a, 20 * ONEMB, 96 * ONEMB, MREMAP_MAYMOVE);
	if (a == MAP_FAILED)
		tfail("mremap failed");

	exit(0);
}

/*
 * Anonymous mapping. Remap to a fixed address.
 */
static void
test2()
{
	void *a, *tmp, *res;

	a = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED)
		tfail("mmap failed");

	/* Use mmap to get a valid fixed destination address */
	tmp = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (tmp == MAP_FAILED)
		tfail("mmap failed");
	munmap(tmp, 4096);

	res = mremap(a, 4096, 4096, MREMAP_MAYMOVE|MREMAP_FIXED, tmp);
	if (res == MAP_FAILED)
		tfail("mremap failed");

	if (res != tmp)
		tfail("mremap not at fixed address");

	exit(0);
}

/*
 * Similar to test 1, but using odd sizes (not multiples of a pagesize).
 */
static void
test3()
{
	void *a;

	a = mmap(NULL, 5000, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED)
		tfail("mmap failed");

	a = mremap(a, 5000, 10000, MREMAP_MAYMOVE);
	if (a == MAP_FAILED)
		tfail("mremap failed");

	a = mremap(a, 10000, 700000, MREMAP_MAYMOVE);
	if (a == MAP_FAILED)
		tfail("mremap failed");

	a = mremap(a, 700000, 10000000, MREMAP_MAYMOVE);
	if (a == MAP_FAILED)
		tfail("mremap failed");

	exit(0);
}

int
main(void)
{
	run_test(1, test1);
	run_test(2, test2);
	run_test(3, test3);

	return (test_pass("mremap"));
}
