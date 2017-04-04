/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright 2017, Joyent, Inc.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/vfs.h>
#include <string.h>
#include <errno.h>
#include <linux/magic.h>
#define _GNU_SOURCE
#include <dirent.h>
#include <sys/syscall.h>

#include "lxtst.h"

#define TST_NAME	"sysfs"
#define MNT_PNT		"/sys"

typedef struct l_dirent {
	long           d_ino;
	off_t          d_off;
	unsigned short d_reclen;
	char           d_name[];
} l_dirent_t;

#define BUF_SIZE 1024

static int tc;

typedef struct dexpect {
	char	d_type;
	char	*d_name;
} dexpect_t;

dexpect_t topdir[] = {
	{DT_DIR, "block"},
	{DT_DIR, "bus"},
	{DT_DIR, "class"},
	{DT_DIR, "devices"},
	{DT_DIR, "fs"},
	{0, NULL}
};

dexpect_t blockdir[] = {
	{DT_LNK, "zfsds0"},
	{0, NULL}
};

dexpect_t classdir[] = {
	{DT_DIR, "net"},
	{0, NULL}
};

dexpect_t netdir[] = {
	{DT_LNK, "eth0"},
	{DT_LNK, "lo"},
	{0, NULL}
};

dexpect_t devicesdir[] = {
	{DT_DIR, "system"},
	{DT_DIR, "virtual"},
	{0, NULL}
};

dexpect_t netlodir[] = {
	{DT_REG, "address"},
	{DT_REG, "addr_len"},
	{DT_REG, "flags"},
	{DT_REG, "ifindex"},
	{DT_REG, "mtu"},
	{DT_REG, "tx_queue_len"},
	{DT_REG, "type"},
	{0, NULL}
};

static void
tfail(char *msg)
{
	printf("FAIL %s %d: %s\n", TST_NAME, tc, msg);
	exit(1);
}

/*
 * Verify we have the same inode for a subdir and '.' in that subdir across
 * one of the subdirs we know changes "class" within our implementation of
 * sysfs. Obviously this is a regression test since our code used to be
 * broken this way.
 */
static int
check_inode(char *path, char *subdir)
{
	int fd, nread;
	char buf[BUF_SIZE];
	l_dirent_t *d;
	int bpos;
	char pdir[1024];
	long subdir_ino;

	fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd == -1)
		tfail("open");

	nread = syscall(SYS_getdents, fd, buf, BUF_SIZE);
	if (nread == -1)
		tfail("getdents");

	for (bpos = 0; bpos < nread;) {
		d = (l_dirent_t *) (buf + bpos);
		if (strcmp(d->d_name, subdir) == 0) {
			subdir_ino = d->d_ino;
			break;
		}
		bpos += d->d_reclen;
	}
	close(fd);

	snprintf(pdir, sizeof (pdir), "%s/%s", path, subdir);
	fd = open(pdir, O_RDONLY | O_DIRECTORY);
	if (fd == -1)
		tfail("open");

	nread = syscall(SYS_getdents, fd, buf, BUF_SIZE);
	if (nread == -1)
		tfail("getdents");

	for (bpos = 0; bpos < nread;) {
		d = (l_dirent_t *) (buf + bpos);
		if (strcmp(d->d_name, ".") == 0) {
			if (d->d_ino != subdir_ino) {
				char e[128];

				snprintf(e, sizeof (e),
				    "got inode 0x%x for \"%s/%s\", but got "
				    "0x%x for \"%s/.\"",
				    (unsigned int)subdir_ino, path, subdir,
				    (unsigned int)d->d_ino, pdir);
				tfail(e);
			}
			break;
		}
		bpos += d->d_reclen;
	}
	close(fd);

	return (0);
}

static char
get_dtype(char *name, dexpect_t *ents)
{
	int i;

	for (i = 0; ents[i].d_name != NULL; i++) {
		if (strcmp(name, ents[i].d_name) == 0)
			return (ents[i].d_type);
	}

	return (-1);
}

/*
 * Check the d_type of entries in the directory against the expected 'ents'.
 */
static void
check_dir(char *path, dexpect_t *ents)
{
	int fd, nread;
	char buf[BUF_SIZE];
	l_dirent_t *d;
	int bpos;
	char d_type, dt;

	fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd == -1)
		tfail("open");

	for ( ; ; ) {
		nread = syscall(SYS_getdents, fd, buf, BUF_SIZE);
		if (nread == -1)
			tfail("getdents");

		if (nread == 0)
			break;

		for (bpos = 0; bpos < nread;) {
			d = (l_dirent_t *) (buf + bpos);
			d_type = *(buf + bpos + d->d_reclen - 1);
			if ((dt = get_dtype(d->d_name, ents)) != -1 &&
			    dt != d_type) {
				char e[128];

				snprintf(e, sizeof (e),
				    "expected d_type %d for \"%s\", but got %d",
				    dt, d->d_name, d_type);
				tfail(e);
			}
			bpos += d->d_reclen;
		}
	}
}

int
main(int argc, char **argv)
{
	struct statfs sfs;
	char e[80];

	/* check sysfs magic number */
	tc = 1;
	if (statfs(MNT_PNT, &sfs) != 0)  {
		return (test_fail(TST_NAME, "1 - statfs"));
	}
	if (sfs.f_type != SYSFS_MAGIC) {
		snprintf(e, sizeof (e), "expected magic 0x%x, got 0x%x",
		    (unsigned int)SYSFS_MAGIC, (unsigned int)sfs.f_type);
		/*
		tfail(e);
		*/ 
		printf("%s\n", e);
	}

	/*
	 * check inode consistency across a spot we know transitions internally
	 */
	tc = 2;
	if (check_inode(MNT_PNT, "block") != 0)  {
		tfail("inodes don't match");
	}

	tc = 3;
	check_dir(MNT_PNT, topdir);

	tc = 4;
	check_dir(MNT_PNT "/block", blockdir);

	tc = 5;
	check_dir(MNT_PNT "/class", classdir);

	tc = 6;
	check_dir(MNT_PNT "/class/net", netdir);

	tc = 7;
	check_dir(MNT_PNT "/devices", devicesdir);

	tc = 8;
	check_dir(MNT_PNT "/devices/virtual/net/lo", netlodir);

	return (test_pass(TST_NAME));
}
