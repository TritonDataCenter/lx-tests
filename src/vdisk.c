/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/hdreg.h>

#include "lxtst.h"

#define TST_NAME	"vdisk"
#define	DSK_NAME	"/dev/zvol0"

#ifndef O_DIRECT
#define O_DIRECT        00040000        /* direct disk access hint */
#endif

static int tc;

static void
tfail(char *msg)
{
	printf("FAIL %s %d: %s\n", TST_NAME, tc, msg);
	exit(1);
}

static void
tst_fd(int fd)
{
	unsigned long val;
	unsigned long long val64;
	struct hd_geometry g;

	if (ioctl(fd, BLKGETSIZE, &val) == -1)
		tfail("BLKGETSIZE ioctl");

        if (ioctl(fd, BLKGETSIZE64, &val64) == -1)
		tfail("BLKGETSIZE64 ioctl");

        if (ioctl(fd, BLKSSZGET, &val) == -1)
		tfail("BLKSSZGET ioctl");

        if (ioctl(fd, HDIO_GETGEO, &g) == -1)
		tfail("HDIO_GETGEO ioctl");
}

int
main(int argc, char *argv[])
{
	int fd;
	struct stat sb;

	tc = 1;
	if ((fd = open("/dev/zfsds0", O_RDONLY)) < 0)
		tfail("/dev/zfs - open failed");

	tst_fd(fd);

	close(fd);

	if (stat(DSK_NAME, &sb) != 0) {
		printf("%s: no virtual disk configured, skipping remaining "
		    "tests\n", TST_NAME);
		return (test_pass(TST_NAME));
	}

	if (geteuid() != 0) {
		printf("%s: not root, skipping remaining tests\n", TST_NAME);
		return (test_pass(TST_NAME));
	}

	tc = 2;
	if ((sb.st_mode & S_IFMT) != S_IFBLK)
		tfail("not a block device");

	if ((fd = open(DSK_NAME, O_RDWR | O_EXCL | O_NONBLOCK | O_DIRECT,
	    0)) < 0)
		tfail("/dev/zvol0 - open failed");

	tst_fd(fd);

	close(fd);

	return (test_pass(TST_NAME));
}
