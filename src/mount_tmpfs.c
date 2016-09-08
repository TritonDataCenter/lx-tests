/*
 * Copyright 2016 Joyent, Inc.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <stdio.h>
#include <sys/vfs.h>
#include <linux/magic.h>

#include "lxtst.h"

#define TST_NAME	"mount_tmpfs"
#define	MNT_PNT		"/lxtst_mnt"
#define	FSTYPE		"tmpfs"
#define TEN_MB		1310720
#define	UID		1000
#define	GID		47

int
main(int argc, char **argv)
{
	char opts[128];
	struct statfs sfs;
	struct stat sb;

	if (geteuid() != 0)
		return (test_skip(TST_NAME, "not root"));

	/*
	 * Cleanup from a previous run.
	 */
	(void) umount(MNT_PNT);
	(void) rmdir(MNT_PNT);

	if (mkdir(MNT_PNT, 0755) != 0)
		return (test_fail(TST_NAME, "mkdir mountpoint failed"));

	/* Test 1 - no options */
	if (mount("swap", MNT_PNT, FSTYPE, 0, NULL) != 0)
		return (test_fail(TST_NAME, "mount tmpfs 1"));
	if (statfs(MNT_PNT, &sfs) != 0)  {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 1 - statfs"));
	}
	if (sfs.f_type != TMPFS_MAGIC) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 1 - fs magic"));
	}
	(void) umount(MNT_PNT);

	/* Test 2 - size option */
	(void) snprintf(opts, sizeof (opts), "size=10m");
	if (mount("swap", MNT_PNT, FSTYPE, 0, opts) != 0)
		return (test_fail(TST_NAME, "mount tmpfs 2 - mount"));
	if (statfs(MNT_PNT, &sfs) != 0)  {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 2 - statfs"));
	}
	if ((sfs.f_blocks * 512) != TEN_MB) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 2 - size"));
	}
	(void) umount(MNT_PNT);

	/* Test 3 - size & uid options */
	(void) snprintf(opts, sizeof (opts), "size=10m,uid=%d", UID);
	if (mount("swap", MNT_PNT, FSTYPE, 0, opts) != 0)
		return (test_fail(TST_NAME, "mount tmpfs 3"));
	if (stat(MNT_PNT, &sb) != 0) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 3 - stat"));
	}
	if (sb.st_uid != UID || sb.st_gid != 0) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 3 - uid/gid"));
	}
	(void) umount(MNT_PNT);

	/* Test 4 - size, uid & gid options */
	(void) snprintf(opts, sizeof (opts), "size=10m,uid=%d,gid=%d",
	    UID, GID);
	if (mount("swap", MNT_PNT, FSTYPE, 0, opts) != 0)
		return (test_fail(TST_NAME, "mount tmpfs 4"));
	if (stat(MNT_PNT, &sb) != 0) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 4 - stat"));
	}
	if (sb.st_uid != UID || sb.st_gid != GID) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount tmpfs 4 - uid/gid"));
	}
	(void) umount(MNT_PNT);

	(void) rmdir(MNT_PNT);
	return (test_pass(TST_NAME));
}
