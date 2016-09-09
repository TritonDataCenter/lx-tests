/*
 * Copyright 2016 Joyent, Inc.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/vfs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/magic.h>

#include "lxtst.h"

#define TST_NAME	"mount_nfs"
#define	MNT_PNT		"/lxtst_mnt"
#define	FSTYPE		"nfs"

int
main(int argc, char **argv)
{
	struct statfs sfs;
	char *hostaddr;
	char mnt_server[1024];
	char *conf_nfs_server, *conf_nfs_export, *conf_mountd_port;
	char opts[256];

	if (geteuid() != 0)
		return (test_skip(TST_NAME, "not root"));

	if ((conf_nfs_server = getenv("LXTST_CONF_NFS_SERVER")) == NULL ||
	    (conf_nfs_export = getenv("LXTST_CONF_NFS_EXPORT")) == NULL)
		return (test_skip(TST_NAME, "no NFS server:fs configured"));

	/*
	 * If the first character in the server value is a digit, assume
	 * we are configured with an IP address for the server, otherwise
	 * we have to lookup the hostname.
	 */
	if (isdigit(*conf_nfs_server)) {
		hostaddr = conf_nfs_server;
	} else {
		struct hostent *he;
		char msg[128];

		he = gethostbyname(conf_nfs_server);
		if (he == NULL) {
			(void) snprintf(msg, sizeof (msg),
			    "unable to gethostbyname for %s",
			    conf_nfs_server);
			return (test_fail(TST_NAME, msg));
		}

		hostaddr = inet_ntoa(*(struct in_addr *)he->h_addr);
	}

	(void) snprintf(mnt_server, sizeof (mnt_server), "%s:%s",
		conf_nfs_server, conf_nfs_export);

	/*
	 * Cleanup from a previous run.
	 */
	(void) umount(MNT_PNT);
	(void) rmdir(MNT_PNT);

	if (mkdir(MNT_PNT, 0755) != 0)
		return (test_fail(TST_NAME, "mkdir mountpoint failed"));

	/* Test 1 - basic options */
	(void) snprintf(opts, sizeof (opts), "addr=%s,proto=tcp", hostaddr);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0)
		return (test_fail(TST_NAME, "mount nfs 1"));
	if (statfs(MNT_PNT, &sfs) != 0)  {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount nfs 1 - statfs"));
	}
	if (sfs.f_type != NFS_SUPER_MAGIC) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount nfs 1 - fs magic"));
	}
	(void) umount(MNT_PNT);

	/* Test 2 - explicit v4 */
	(void) snprintf(opts, sizeof (opts), "addr=%s,proto=tcp,vers=4",
	    hostaddr);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0)
		return (test_fail(TST_NAME, "mount nfs 2"));
	if (statfs(MNT_PNT, &sfs) != 0)  {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount nfs 2 - statfs"));
	}
	if (sfs.f_type != NFS_SUPER_MAGIC) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount nfs 2 - fs magic"));
	}
	(void) umount(MNT_PNT);

	/* Test 3 - explicit v3 */
	(void) snprintf(opts, sizeof (opts), "addr=%s,proto=tcp,vers=3",
	    hostaddr);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0)
		return (test_fail(TST_NAME, "mount nfs 3"));
	if (statfs(MNT_PNT, &sfs) != 0)  {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount nfs 3 - statfs"));
	}
	if (sfs.f_type != NFS_SUPER_MAGIC) {
		(void) umount(MNT_PNT);
		return (test_fail(TST_NAME, "mount nfs 3 - fs magic"));
	}
	(void) umount(MNT_PNT);

	/* Test 4 - nfs and mountd ports provided - optional test */
	conf_mountd_port = getenv("LXTST_CONF_MOUNTD_PORT");
	if (conf_mountd_port != NULL) {
		/* 2049 is the well-defined NFS port */
		(void) snprintf(opts, sizeof (opts),
			"addr=%s,vers=3,proto=tcp,port=2049,"
			"mountvers=3,mountproto=tcp,mountport=%s",
			hostaddr, conf_mountd_port);
		if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0)
			return (test_fail(TST_NAME, "mount nfs 4"));
		if (statfs(MNT_PNT, &sfs) != 0)  {
			(void) umount(MNT_PNT);
			return (test_fail(TST_NAME, "mount nfs 4 - statfs"));
		}
		if (sfs.f_type != NFS_SUPER_MAGIC) {
			(void) umount(MNT_PNT);
			return (test_fail(TST_NAME, "mount nfs 4 - fs magic"));
		}
		(void) umount(MNT_PNT);
	}

	(void) rmdir(MNT_PNT);
	return (test_pass(TST_NAME));
}
