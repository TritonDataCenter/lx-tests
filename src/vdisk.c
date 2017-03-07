/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <linux/hdreg.h>

#include "lxtst.h"

#define TST_NAME	"vdisk"
#define	DSK_NAME	"/dev/zvol0"

#ifndef O_DIRECT
#define O_DIRECT        00040000        /* direct disk access hint */
#endif

int
main(int argc, char *argv[])
{
	int fd;
	unsigned long val;
	unsigned long long val64;
	struct hd_geometry g;
	struct stat sb;

	if (stat(DSK_NAME, &sb) != 0)
		return (test_skip(TST_NAME, "no virtual disk configured"));

	if (geteuid() != 0)
		return (test_skip(TST_NAME, "not root"));

	/* Test 1 - is it a block device? */
	if ((sb.st_mode & S_IFMT) != S_IFBLK) {
		return (test_fail(TST_NAME, "vdisk 1 - not a block device"));
	}

	/* Test 2 - ioctls */
	if ((fd = open(DSK_NAME, O_RDWR | O_EXCL | O_NONBLOCK | O_DIRECT,
	    0)) < 0)
		return (test_fail(TST_NAME, "vdisk - open failed"));

	if (ioctl(fd, BLKGETSIZE, &val) == -1)
		return (test_fail(TST_NAME, "vdisk 2 - BLKGETSIZE ioctl"));

        if (ioctl(fd, BLKGETSIZE64, &val64) == -1)
		return (test_fail(TST_NAME, "vdisk 2 - BLKGETSIZE64 ioctl"));

        if (ioctl(fd, BLKSSZGET, &val) == -1)
		return (test_fail(TST_NAME, "vdisk 2 - BLKSSZGET ioctl"));

        if (ioctl(fd, HDIO_GETGEO, &g) == -1)
		return (test_fail(TST_NAME, "vdisk 2 - HDIO_GETGEO ioctl"));

	close(fd);

	return (test_pass(TST_NAME));
}
