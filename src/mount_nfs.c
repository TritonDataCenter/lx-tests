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
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/vfs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/wait.h>
#include <linux/magic.h>

#include "lxtst.h"

#define TST_NAME	"mount_nfs"
#define	MNT_PNT		"/lxtst_mnt"
#define TST_FNAME	"/lxtst_mnt/lx-tests-mount_nfs-test-file"
#define	FSTYPE		"nfs"

static int tc;
static struct timespec delay = {
	.tv_sec = 0,
	.tv_nsec = 1000000
};

static void
tfail(char *msg)
{
	printf("FAIL %s %d: %s\n", TST_NAME, tc, msg);
	exit(1);
}

static void
t_err(char *msg, int en)
{
	char e[80];

	snprintf(e, sizeof (e), "%s: %d", msg, en);
	tfail(e);
}

static int
rpcbind_running()
{
	DIR *dirp;
	struct dirent *dp;
	int fd, l;
	char buf[128];

	if ((dirp = opendir("/proc")) == NULL)
		return (0);

	do {
		if ((dp = readdir(dirp)) != NULL) {
			if (strcmp(dp->d_name, ".") == 0 ||
			    strcmp(dp->d_name, "..") == 0)
				continue;

			snprintf(buf, sizeof (buf), "/proc/%s/comm",
			    dp->d_name);
			if ((fd = open(buf, O_RDONLY)) >= 0) {
				l = read(fd, buf, sizeof (buf));
				buf[l] = '\0';
				close(fd);
				if (strcmp(buf, "rpcbind\n") == 0) {
					(void) closedir(dirp);
					return (1);
				}
			}
		}
	} while (dp != NULL);

	(void) closedir(dirp);
	return (0);
}

void
lock_it(int fd)
{
	int res, l;
	struct flock lock;
	char buf[80];

	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	lock.l_pid = getpid();

	lock.l_type = F_WRLCK;
	res = fcntl(fd, F_SETLKW, &lock);
	if (res < 0)
		t_err("lock F_SETLKW", errno);

	lseek(fd, 0, SEEK_SET);
	l = read(fd, buf, sizeof (buf));
	if (l < 0)
		t_err("lock read", errno);

	buf[l] = '\0';
	if (l != 0 && atoi(buf) != 0)
		tfail("lock test, got lock when locked");
	(void) snprintf(buf, sizeof (buf), "%d", getpid());
	l = strlen(buf) + 1;
	if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, buf, l) != l)
		tfail("lock test, write lock");
	fdatasync(fd);
}

void
unlock_it(int fd)
{
	int res, l;
	struct flock lock;
	char buf[80];

	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	lock.l_pid = getpid();

	lseek(fd, 0, SEEK_SET);
	l = read(fd, buf, sizeof (buf));
	if (l < 0)
		t_err("lock read", errno);

	buf[l] = '\0';
	if (atoi(buf) != getpid())
		tfail("lock test, unlocked by other process");
	(void) snprintf(buf, sizeof (buf), "%d", 0);
	l = strlen(buf) + 1;
	if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, buf, l) != l)
		tfail("lock test, write lock");
	fdatasync(fd);
	lock.l_type = F_UNLCK;
	res = fcntl(fd, F_SETLK, &lock);
	if (res < 0)
		t_err("unlock F_SETLK", errno);
}

/* Test NFS locking */
static void
do_lock_test()
{
	int fd;
	int pid;
	int status;

	/* cleanup for possible failed previous run */
	unlink(TST_FNAME);

	fd = open(TST_FNAME, O_RDWR | O_CREAT, 0644);
	if (fd < 0)
		t_err("lock test", errno);

	pid = fork();
	if (pid < 0)
		t_err("fork", errno);

	if (pid == 0) {
		/* child */
		lock_it(fd);
		nanosleep(&delay, NULL);
		unlock_it(fd);
		exit(0);
	} else {
		/* parent */
		lock_it(fd);
		nanosleep(&delay, NULL);
		unlock_it(fd);
	}

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
		tfail("lock test; child failed");

	close(fd);
	unlink(TST_FNAME);
}

int
main(int argc, char **argv)
{
	int e;
	struct statfs sfs;
	char *hostaddr;
	char mnt_server[1024];
	char *conf_nfs_server, *conf_nfs_export, *conf_mountd_port;
	char opts[256];
	char em[80];
	int have_rpcbind;

	if (geteuid() != 0)
		return (test_skip(TST_NAME, "not root"));

	if ((conf_nfs_server = getenv("LXTST_CONF_NFS_SERVER")) == NULL ||
	    (conf_nfs_export = getenv("LXTST_CONF_NFS_EXPORT")) == NULL ||
	    (conf_mountd_port = getenv("LXTST_CONF_MOUNTD_PORT")) == NULL)
		return (test_skip(TST_NAME,
		    "no NFS server:fs or mountd port configured"));

	have_rpcbind = rpcbind_running();
	if (!have_rpcbind) {
		printf("%s: rpcbind not running, skipping NFS locking "
		    "tests\n", TST_NAME);
	}

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

	/* NFSv4 */
	tc = 1;
	(void) snprintf(opts, sizeof (opts), "addr=%s,proto=tcp,vers=4",
	    hostaddr);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0) {
		if (errno == EPROTONOSUPPORT) {
			printf("%s: NFSv4 not supported by server, skipping\n",
			    TST_NAME);
			goto no_v4;
		}
		t_err("mount", errno);
	}
	if (statfs(MNT_PNT, &sfs) != 0)  {
		e = errno;
		(void) umount(MNT_PNT);
		t_err("statfs", e);
	}
	if (sfs.f_type != NFS_SUPER_MAGIC) {
		(void) umount(MNT_PNT);
		snprintf(em, sizeof (em), "invalid fs magic: 0x%lx",
		   (unsigned long)sfs.f_type);
		tfail(em);
	}

	if (have_rpcbind) {
		/* Test  NFSv4 locking */
		tc = 2;
		do_lock_test();
	}

	(void) umount(MNT_PNT);

no_v4:
	/* NFSv3, as issued by the mount.nfs helper */
	tc = 3;
	(void) snprintf(opts, sizeof (opts),
		"addr=%s,vers=3,proto=tcp,"
		"mountvers=3,mountproto=tcp,mountport=%s",
		hostaddr, conf_mountd_port);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0)
		t_err("mount", errno);
	if (statfs(MNT_PNT, &sfs) != 0)  {
		e = errno;
		(void) umount(MNT_PNT);
		t_err("statfs", e);
	}
	if (sfs.f_type != NFS_SUPER_MAGIC) {
		(void) umount(MNT_PNT);
		snprintf(em, sizeof (em), "invalid fs magic: 0x%lx",
		   (unsigned long)sfs.f_type);
		tfail(em);
	}

	if (have_rpcbind) {
		/* Test  NFSv3 locking */
		tc = 4;
		do_lock_test();
	}

	(void) umount(MNT_PNT);

	/* NFSv3 with some extra options */
	tc = 5;
	(void) snprintf(opts, sizeof (opts),
		"addr=%s,vers=3,proto=tcp,nolock,bg,"
		"mountvers=3,mountproto=tcp,mountport=%s",
		hostaddr, conf_mountd_port);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) != 0)
		t_err("mount", errno);
	if (statfs(MNT_PNT, &sfs) != 0)  {
		e = errno;
		(void) umount(MNT_PNT);
		t_err("statfs", e);
	}
	if (sfs.f_type != NFS_SUPER_MAGIC) {
		(void) umount(MNT_PNT);
		snprintf(em, sizeof (em), "invalid fs magic: 0x%lx",
		   (unsigned long)sfs.f_type);
		tfail(em);
	}
	(void) umount(MNT_PNT);

	/* Attempt mount from unshared path */
	tc = 6;
	(void) snprintf(mnt_server, sizeof (mnt_server), "%s:%sFOO",
		conf_nfs_server, conf_nfs_export);
	(void) snprintf(opts, sizeof (opts),
		"addr=%s,vers=3,proto=tcp,"
		"mountvers=3,mountproto=tcp,mountport=%s",
		hostaddr, conf_mountd_port);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) == 0 || errno != EACCES)
		t_err("mount", errno);
	(void) snprintf(mnt_server, sizeof (mnt_server), "%s:%s",
		conf_nfs_server, conf_nfs_export);

	/* NFSv3 with sec=none */
	tc = 7;
	(void) snprintf(opts, sizeof (opts),
		"addr=%s,vers=3,proto=tcp,sec=none,"
		"mountvers=3,mountproto=tcp,mountport=%s",
		hostaddr, conf_mountd_port);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) == 0 || errno != EACCES)
		t_err("mount", errno);

	/*
	 * Attempt mount using illumos security (diffie-hellman) name. This is
	 * invalid on Linux.
	 */
	tc = 8;
	(void) snprintf(opts, sizeof (opts),
		"addr=%s,vers=3,proto=tcp,sec=dh,"
		"mountvers=3,mountproto=tcp,mountport=%s",
		hostaddr, conf_mountd_port);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) == 0 || errno != EINVAL)
		t_err("mount", errno);

	/*
	 * Attempt mount from invalid server (localhost)
	 */
	tc = 9;
	(void) snprintf(opts, sizeof (opts),
		"addr=127.0.0.1,vers=3,proto=tcp,"
		"mountvers=3,mountproto=tcp,mountport=%s",
		conf_mountd_port);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) == 0 ||
	    errno != ETIMEDOUT)
		t_err("mount", errno);

	/*
	 * XXX - we cannot currently distinguish these on lx:
	 * Attempt mount with invalid mountd port
	 * Attempt mount from invalid server (localhost)
	(void) snprintf(opts, sizeof (opts),
		"addr=%s,vers=3,proto=tcp,"
		"mountvers=3,mountproto=tcp,mountport=%d",
		hostaddr, atoi(conf_mountd_port) + 1);
	if (mount(mnt_server, MNT_PNT, FSTYPE, 0, opts) == 0 ||
	    errno != EPFNOSUPPORT)
		t_err("mount", errno);

	* XXX
	*/

	(void) rmdir(MNT_PNT);
	return (test_pass(TST_NAME));
}
