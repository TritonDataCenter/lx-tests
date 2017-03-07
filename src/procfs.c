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
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/vfs.h>
#include <string.h>
#include <errno.h>
#include <linux/magic.h>

#include "lxtst.h"

#define TST_NAME	"procfs"
#define MNT_PNT		"/proc"

char *writable[] = {
	"/proc/self/loginuid",
	"/proc/self/oom_score_adj",
	"/proc/sys/kernel/core_pattern",
	"/proc/sys/kernel/shmall",
	"/proc/sys/kernel/shmmax",
	"/proc/sys/net/core/somaxconn",
	"/proc/sys/net/ipv4/ip_local_port_range",
	"/proc/sys/net/ipv4/tcp_fin_timeout",
	"/proc/sys/net/ipv4/tcp_keepalive_intvl",
	"/proc/sys/net/ipv4/tcp_keepalive_time",
	"/proc/sys/net/ipv4/tcp_sack",
	"/proc/sys/net/ipv4/tcp_window_scaling",
	"/proc/sys/vm/overcommit_memory",
	"/proc/sys/vm/swappiness",
	NULL
};

/* Validate /proc/mounts looks reasonable */
static int
check_mounts()
{
	FILE *fp;
	char line[128], *f1, *f2;
	int res = -1;

	if ((fp = fopen("/proc/mounts", "r")) == NULL)
		return (-1);

	while (fgets(line, sizeof (line), fp) != NULL) {

		f1 = strtok(line, " ");
		f2 = strtok(NULL, " ");

		if (f1 == NULL || f2 == NULL) {
			fclose(fp);
			return (-1);
		}

		if (strcmp(f2, "/") == 0 && strcmp(f1, "/dev/zfsds0") == 0) {
			res = 0;
			break;
		}
	}

	fclose(fp);
	return (res);
}

/*
 * Validate that some fields in /proc/self/stat look reasonable:
 *	startstack
 *	exit signal
 */
static int
check_self_stat()
{
	FILE *fp;
	char line[1024];
	char *f;
	int cnt;
	int res = 0;
	long long val;

	if ((fp = fopen("/proc/self/stat", "r")) == NULL)
		return (-1);

	if (fgets(line, sizeof (line), fp) == NULL || strlen(line) == 0)
		goto bad;

	f = strtok(line, " ");
	for (cnt = 1; f != NULL; cnt++) {
		if (cnt == 28) {
			/*
			 * startstack - field 28
			 * We previously reported USRSTACK, which is USERLIMIT,
			 * which is 0xfffffd7fffe00000. See the definition for
			 * LX_MAXSTACK64 is the lx kernel source.
			 */
			errno = 0;
			val = atoll(f);
			if (errno != 0 || val > 0x7ffffff00000) {
				res = -1;
				break;
			}
		} else if (cnt == 38) {
			/*
			 * exit signal - field 38
			 * Some apps explicitly use this field to determine if
			 * this is a process.
			 */
			errno = 0;
			val = atoll(f);
			if (errno != 0 || val != 17) {
				res = -1;
				break;
			}
		}
		f = strtok(NULL, " ");
	}

bad:
	fclose(fp);
	return (res);
}

/* Validate /proc/self/mountinfo looks reasonable */
static int
check_self_mountinfo()
{
	FILE *fp;
	char line[128], *lp, *f, *fmp, *fdev;
	int i;
	int res = -1;

	if ((fp = fopen("/proc/self/mountinfo", "r")) == NULL)
		return (-1);

	while (fgets(line, sizeof (line), fp) != NULL) {
		lp = line;
		fmp = fdev = NULL;
		i = 0;
		while ((f = strtok(lp, " ")) != NULL) {
			if (i == 3) {
				fmp = f;
			} else if (i == 8) {
				fdev = f;
			}
			lp = NULL;
			i++;
		}

		if (fmp != NULL && strcmp(fmp, "/") == 0 &&
		    fdev != NULL && strcmp(fdev, "/dev/zfsds0") == 0) {
			res = 0;
			break;
		}
	}

	fclose(fp);
	return (res);
}

/* Validate /proc/devices looks reasonable - these are hardcoded in procfs */
static int
check_devices()
{
	FILE *fp;
	char line[128], *f1, *f2;
	int res = 0, found = 0;
	long val;

	if ((fp = fopen("/proc/devices", "r")) == NULL)
		return (-1);

	while (fgets(line, sizeof (line), fp) != NULL) {

		f1 = strtok(line, " \n");
		f2 = strtok(NULL, " \n");

		if (f1 == NULL || f2 == NULL)
			continue;

		errno = 0;
		val = atol(f1);
		if (errno != 0)
			continue;

		if (strcmp(f2, "/dev/tty") == 0) {
			found++;
			if (val != 5) {
				res = -1;
				break;
			}
		} else if (strcmp(f2, "/dev/console") == 0) {
			found++;
			if (val != 5) {
				res = -1;
				break;
			}
		} else if (strcmp(f2, "/dev/ptmx") == 0) {
			found++;
			if (val != 5) {
				res = -1;
				break;
			}
		} else if (strcmp(f2, "ptm") == 0) {
			found++;
			if (val != 5) {
				res = -1;
				break;
			}
		} else if (strcmp(f2, "pts") == 0) {
			found++;
			if (val != 136) {
				res = -1;
				break;
			}
		} else if (strcmp(f2, "zvol") == 0) {
			found++;
			if (val != 203) {
				res = -1;
				break;
			}
		}
	}

	if (found != 6)
		res = -1;

	fclose(fp);
	return (res);
}

/* Validate files in the 'writable' list look writable. */
static int
check_writable()
{
	int i;
	struct stat sb;

	for (i = 0; writable[i] != NULL; i++) {
		if (stat(writable[i], &sb) != 0 || !(sb.st_mode & 0600))
			return (-1);
	}

	return (0);
}

int
main(int argc, char **argv)
{
	struct statfs sfs;

	/* Test 1 - is /proc proc? */
	if (statfs(MNT_PNT, &sfs) != 0)  {
		return (test_fail(TST_NAME, "procfs 1 - statfs"));
	}
	if (sfs.f_type != PROC_SUPER_MAGIC) {
		return (test_fail(TST_NAME, "procfs 1 - fs magic"));
	}

	/* Test 2 - /proc/mounts should exist and have a root entry */
	if (check_mounts() != 0)  {
		return (test_fail(TST_NAME, "procfs 2 - mounts"));
	}

	/* Test 3 - /proc/self/stat entry */
	if (check_self_stat() != 0)  {
		return (test_fail(TST_NAME, "procfs 3 - self/stat"));
	}

	/* Test 4 - /proc/self/mountinfo entry */
	if (check_self_mountinfo() != 0)  {
		return (test_fail(TST_NAME, "procfs 4 - self/mountinfo"));
	}

	/* Test 5 - /proc/devices entry */
	if (check_devices() != 0)  {
		return (test_fail(TST_NAME, "procfs 5 - devices"));
	}

	/* Test 6 - are various files writable? */
	if (check_writable() != 0)  {
		return (test_fail(TST_NAME, "procfs 6 - writable files"));
	}

	return (test_pass(TST_NAME));
}
