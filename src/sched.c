/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include "lxtst.h"

#ifndef SCHED_BATCH
#define	SCHED_BATCH	3
#endif
#ifndef SCHED_IDLE
#define	SCHED_IDLE	5
#endif

static int chld_tid;
static int chld_done;

static int
thr(void *a)
{
	/* thread */
	chld_tid = syscall(186);
	while (chld_done == 0)
		usleep(1000);
        return (0);
}

/*
 * On Linux (and lx) a process is in SCHED_OTHER (a round-robin scheduling
 * class) by default.
 */

int
main(int argc, char **argv)
{
	int am_root;
	int res;
	int pid;
	int status;
	pthread_t tid;
	void *rv;
	struct sched_param sp;
	struct timespec tp;

	am_root = (geteuid() == 0);

	/* Test 1 - getscheduler for self */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 1", "get: incorrect class"));

	/* Test 2 - getparam for self */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_OTHER))
		return (test_fail("sched 2", "get param: incorrect"));

	/* Test 3 - re-set scheduler to SCHED_OTHER for self */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_OTHER, &sp);
	if (res < 0)
		return (test_fail("sched 3", "set: self failed"));

	/* Test 4 - setscheduler with invalid priority for self */
	sp.sched_priority = sched_get_priority_max(SCHED_OTHER) + 1;
	errno = 0;
	res = sched_setscheduler(0, SCHED_OTHER, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 4", "set invalid priority: passed"));

	/* Test 5 - setscheduler with invalid priority for self */
	sp.sched_priority = sched_get_priority_min(SCHED_OTHER) - 1;
	errno = 0;
	res = sched_setscheduler(0, SCHED_OTHER, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 5", "set to SCHED_OTHER: passed"));

	/* Test 6 - get RR interval - pass even though we're SCHED_OTHER */
	errno = 0;
	res = sched_rr_get_interval(0, &tp);
	if (res < 0 || tp.tv_sec != 0)
		return (test_fail("sched 6", "get interval: failed"));

	/*
	 * Now test against another process.
	 */

	pid = fork();
	if (pid < 0)
		return (test_fail("sched", "fork failed"));

	if (pid == 0) {
		/* child */
		pause();
	}

	/* parent */

	/* Test 7 - getscheduler for child */
	res = sched_getscheduler(pid);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 7", "get: incorrect class"));

	/* Test 8 - getparam for child */
	res = sched_getparam(pid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_OTHER))
		return (test_fail("sched 8", "get param: incorrect"));

	kill(pid, 9);
	if (waitpid(pid,  &status, WNOHANG) == -1)
		return (test_fail("sched", "waitpid failed"));

	/*
	 * Now test against another thread.
	 */

	if (pthread_create(&tid, NULL, (void *(*)(void *))thr, NULL) != 0)
		return (test_fail("sched", "pthread_create failed"));

	/* main thread */
	while (chld_tid == 0)
		usleep(1000);

	/* Test 9 - getscheduler for thread */
	res = sched_getscheduler(chld_tid);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 9", "get: incorrect class"));

	/* Test 10 - getparam for thread */
	res = sched_getparam(chld_tid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_OTHER))
		return (test_fail("sched 10", "get param: incorrect"));

	/* Test 11 - change current thread */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_BATCH, &sp);
	if (res < 0)
		return (test_fail("sched 11", "set: set self to BATCH failed"));

	/* Test 12 - getscheduler for current thread */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_BATCH)
		return (test_fail("sched 12", "get: incorrect class"));

	/* Test 13 - other thread shouldn't change after test 11 */
	res = sched_getscheduler(chld_tid);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 13", "get: incorrect class"));

	/* Test 14 change current thread back to OTHER */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_OTHER, &sp);
	if (res < 0)
		return (test_fail("sched 14", "set: set self to OTHER failed"));

	/* Test 15 - set scheduler to SCHED_BATCH for thread */
	sp.sched_priority = 0;
	res = sched_setscheduler(chld_tid, SCHED_BATCH, &sp);
	if (res < 0)
		return (test_fail("sched 15", "set: set thrd to BATCH failed"));

	/* Test 16 - getscheduler for thread */
	res = sched_getscheduler(chld_tid);
	if (res < 0 || res != SCHED_BATCH)
		return (test_fail("sched 16", "get: incorrect class"));

	/* Test 17 - getparam for thread */
	res = sched_getparam(chld_tid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_BATCH))
		return (test_fail("sched 17", "get param: incorrect"));

	/* Test 18 - current thread shouldn't change after test 15 */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 18", "get: incorrect class"));

	/*
	 * Now switch to SCHED_IDLE.
	 */

	/* Test 19 - set scheduler to SCHED_IDLE for thread */
	sp.sched_priority = 0;
	res = sched_setscheduler(chld_tid, SCHED_IDLE, &sp);
	if (res < 0)
		return (test_fail("sched 19", "set: set self to IDLE failed"));

	/* Test 20 - getscheduler for thread */
	res = sched_getscheduler(chld_tid);
	if (res < 0 || res != SCHED_IDLE)
		return (test_fail("sched 20", "get: incorrect class"));

	/* Test 21 - current thread shouldn't change after test 19 */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 21", "get: incorrect class"));

	chld_done = 1;
	pthread_join(tid, &rv);

	/*
	 * Now, test against a non-existent process. We can't use the one we
	 * just killed since we might race and it might still be around!
	 */

	/* Test 22 - getscheduler for none */
	res = sched_getscheduler(99999);
	if (res == 0 || errno != ESRCH)
		return (test_fail("sched 22", "get: incorrect errno"));

	/* Test 23 - getparam for none */
	res = sched_getparam(99999, &sp);
	if (res == 0 || errno != ESRCH)
		return (test_fail("sched 23", "get param: incorrect errno"));

	/* Test 24 - setscheduler to SCHED_OTHER for none */
	sp.sched_priority = sched_get_priority_max(SCHED_OTHER);
	res = sched_setscheduler(99999, SCHED_OTHER, &sp);
	if (res == 0 || errno != ESRCH)
		return (test_fail("sched 24", "set: incorrect errno"));

	/*
	 * Now test against another process which know we can access but can't
	 * change as a regular user. Use 'init'.
	 */

	/* Test 25 - getscheduler for init - this is allowed */
	res = sched_getscheduler(1);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 25", "get: incorrect class"));

	/* Test 26 - getparam for init */
	res = sched_getparam(1, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_OTHER))
		return (test_fail("sched 26", "get param: incorrect"));

	/* Test 27 - try to setscheduler for init */
	sp.sched_priority = sched_get_priority_max(SCHED_OTHER);
	errno = 0;
	res = sched_setscheduler(1, SCHED_OTHER, &sp);
	if (am_root) {
		if (res < 0)
			return (test_fail("sched 27", "set: failed"));
	} else {
		if (res == 0 || errno != EPERM)
			return (test_fail("sched 27", "set: incorrect errno"));
	}

	/*
	 * Now switch to SCHED_BATCH.
	 */

	/* Test 28 - set scheduler to SCHED_BATCH for self */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_BATCH, &sp);
	if (res < 0)
		return (test_fail("sched 28", "set: set self to BATCH failed"));

	/* Test 29 - getscheduler for self */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_BATCH)
		return (test_fail("sched 29", "get: incorrect class"));

	/* Test 30 - getparam for self */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_BATCH))
		return (test_fail("sched 30", "get param: incorrect"));

	/*
	 * Now switch to SCHED_IDLE.
	 */

	/* Test 31 - set scheduler to SCHED_IDLE for self */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_IDLE, &sp);
	if (res < 0)
		return (test_fail("sched 31", "set: set self to IDLE failed"));

	/* Test 32 - getscheduler for self */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_IDLE)
		return (test_fail("sched 32", "get: incorrect class"));

	/* Test 33 - getparam for self */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_IDLE))
		return (test_fail("sched 33", "get param: incorrect"));

	/* Test 34 - set self back to SCHED_OTHER should fail after IDLE */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_OTHER, &sp);
	if (am_root) {
		if (res < 0)
			return (test_fail("sched 34", "leave IDLE failed"));
	} else {
		if (res == 0 || errno != EPERM)
			return (test_fail("sched 34", "leave IDLE worked"));
	}

	/*
	 * Now test switching to SCHED_RR, which we pretend works if you're
	 * root.
	 */

	/* Test 35 - setscheduler to SCHED_RR for self */
	sp.sched_priority = sched_get_priority_min(SCHED_RR);
	res = sched_setscheduler(0, SCHED_RR, &sp);
	if (am_root) {
		if (res < 0)
			return (test_fail("sched 35", "set SCHED_RR: failed"));
	} else {
		if (res == 0 || errno != EPERM)
			return (test_fail("sched 35", "set SCHED_RR: worked"));
	}

	/*
	 * NOTE: We're done testing unless we're root
	 */
	if (!am_root)
		return (test_pass("sched"));

	/* Test 36 - getparam for self, confirm new priority from test 14 */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_min(SCHED_RR))
		return (test_fail("sched 36", "get param: incorrect"));

	/* Test 37 - setparam for self */
	sp.sched_priority = sched_get_priority_max(SCHED_RR);
	res = sched_setparam(0, &sp);
	if (res < 0)
		return (test_fail("sched 37", "set param: failed"));

	/* Test 39 - getparam for self, confirm new priority from test 16 */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_RR))
		return (test_fail("sched 39", "get param: incorrect"));

	/* Test 40 - setparam with invalid priority for self */
	sp.sched_priority = sched_get_priority_max(SCHED_RR) + 1;
	errno = 0;
	res = sched_setparam(0, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 40", "set invalid priority: passed"));

	/* Test 41 - setparam with invalid priority for self */
	sp.sched_priority = sched_get_priority_min(SCHED_RR) - 1;
	errno = 0;
	res = sched_setparam(0, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 41", "set invalid priority: passed"));

	/* Test 42 - getparam for self, confirm priority hasn't changed */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_RR))
		return (test_fail("sched 42", "get param: incorrect"));

	/* Test 43 - get RR interval */
	res = sched_rr_get_interval(0, &tp);
	if (res < 0 || tp.tv_sec != 0 || tp.tv_nsec == 0)
		return (test_fail("sched 43", "get interval: incorrect"));

	/*
	 * Now switch back to SCHED_OTHER and test switching a child to
	 * SCHED_RR.
	 */

	/* Test 44 - re-set scheduler to SCHED_OTHER for self */
	sp.sched_priority = 0;
	res = sched_setscheduler(0, SCHED_OTHER, &sp);
	if (res < 0)
		return (test_fail("sched 44", "set: reset self failed"));

	pid = fork();
	if (pid < 0)
		return (test_fail("sched", "fork failed"));

	if (pid == 0) {
		/* child */
		pause();
	}

	/* parent */

	/* Test 45 - getscheduler for child */
	res = sched_getscheduler(pid);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 45", "get: incorrect class"));

	/* Test 46 - setscheduler to SCHED_RR for child */
	sp.sched_priority = sched_get_priority_min(SCHED_RR);
	res = sched_setscheduler(pid, SCHED_RR, &sp);
	if (res < 0)
		return (test_fail("sched 46", "set SCHED_RR: failed"));

	/* Test 47 - getscheduler for child */
	res = sched_getscheduler(pid);
	if (res < 0 || res != SCHED_RR)
		return (test_fail("sched 47", "get: incorrect class"));

	/* Test 48 - getparam for child */
	res = sched_getparam(pid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_min(SCHED_RR))
		return (test_fail("sched 48", "get param: incorrect"));

	/* Test 49 - setparam for child */
	sp.sched_priority = sched_get_priority_max(SCHED_RR);
	res = sched_setparam(pid, &sp);
	if (res < 0)
		return (test_fail("sched 49", "set param: failed"));

	/* Test 50 - getparam for child, confirm new priority from test 28 */
	res = sched_getparam(pid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_RR))
		return (test_fail("sched 50", "get param: incorrect"));

	/* Test 51 - invalid setparam for child */
	sp.sched_priority = sched_get_priority_max(SCHED_RR) + 1;
	res = sched_setparam(pid, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 51", "set param: passed"));

	/* Test 52 - getparam for child, confirm priority didn't change */
	res = sched_getparam(pid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_RR))
		return (test_fail("sched 52", "get param: incorrect"));

	/* Test 53 - get RR interval for child */
	res = sched_rr_get_interval(pid, &tp);
	if (res < 0 || tp.tv_sec != 0 || tp.tv_nsec != 100000000)
		return (test_fail("sched 53", "get interval: incorrect"));

	kill(pid, 9);
	if (waitpid(pid,  &status, WNOHANG) == -1)
		return (test_fail("sched", "waitpid failed"));

	/*
	 * Now test RR against another thread.
	 */

	chld_tid = chld_done = 0;
	if (pthread_create(&tid, NULL, (void *(*)(void *))thr, NULL) != 0)
		return (test_fail("sched", "pthread_create failed"));

	/* main thread */
	while (chld_tid == 0)
		usleep(1000);

	/* Test 54 - getscheduler for thread */
	res = sched_getscheduler(chld_tid);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 54", "get: incorrect class"));

	/* Test 55 - setscheduler to SCHED_RR for thread */
	sp.sched_priority = sched_get_priority_min(SCHED_RR);
	res = sched_setscheduler(chld_tid, SCHED_RR, &sp);
	if (res < 0)
		return (test_fail("sched 55", "set SCHED_RR: failed"));

	/* Test 56 - getscheduler for thread */
	res = sched_getscheduler(chld_tid);
	if (res < 0 || res != SCHED_RR)
		return (test_fail("sched 56", "get: incorrect class"));

	/* Test 57 - getparam for thread */
	res = sched_getparam(chld_tid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_min(SCHED_RR))
		return (test_fail("sched 57", "get param: incorrect"));

	/* Test 58 - scheduler for self shouldn't change after 55 */
	res = sched_getscheduler(0);
	if (res < 0 || res != SCHED_OTHER)
		return (test_fail("sched 58", "get: incorrect class"));

	/* Test 59 - setparam for child */
	sp.sched_priority = sched_get_priority_max(SCHED_RR);
	res = sched_setparam(chld_tid, &sp);
	if (res < 0)
		return (test_fail("sched 59", "set param: failed"));

	/* Test 60 - getparam for thread, confirm new priority from test 50 */
	res = sched_getparam(chld_tid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_RR))
		return (test_fail("sched 60", "get param: incorrect"));

	/* Test 61 - invalid setparam for thread */
	sp.sched_priority = sched_get_priority_max(SCHED_RR) + 1;
	res = sched_setparam(chld_tid, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 61", "set param: passed"));

	/* Test 62 - getparam for thread, confirm priority didn't change */
	res = sched_getparam(chld_tid, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_RR))
		return (test_fail("sched 62", "get param: incorrect"));

	/* Test 63 - get RR interval for thread */
	res = sched_rr_get_interval(chld_tid, &tp);
	if (res < 0 || tp.tv_sec != 0 || tp.tv_nsec == 0)
		return (test_fail("sched 63", "get interval: incorrect"));

	chld_done = 1;
	pthread_join(tid, &rv);

	/*
	 * Now switch self to SCHED_FIFO, which we pretend works.
	 */

	/* Test 64 - setscheduler to SCHED_FIFO for self */
	sp.sched_priority = sched_get_priority_min(SCHED_FIFO);
	res = sched_setscheduler(0, SCHED_FIFO, &sp);
	if (res < 0)
		return (test_fail("sched 64", "set SCHED_FIFO: failed"));

	/* Test 65 - getparam for self, confirm new priority from test 39 */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_min(SCHED_FIFO))
		return (test_fail("sched 65", "get param: incorrect"));

	/* Test 66 - setparam for self */
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
	res = sched_setparam(0, &sp);
	if (res < 0)
		return (test_fail("sched 66", "set param: failed"));

	/* Test 67 - getparam for self, confirm new priority from test 41 */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_FIFO))
		return (test_fail("sched 67", "get param: incorrect"));

	/* Test 68 - invalid setparam for self */
	sp.sched_priority = sched_get_priority_min(SCHED_FIFO) - 1;
	res = sched_setparam(0, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 68", "set param: passed"));

	/* Test 69 - invalid setparam for self */
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO) + 1;
	res = sched_setparam(0, &sp);
	if (res == 0 || errno != EINVAL)
		return (test_fail("sched 69", "set param: passed"));

	/* Test 70 - getparam for self, confirm unchanged priority */
	res = sched_getparam(0, &sp);
	if (res < 0 || sp.sched_priority != sched_get_priority_max(SCHED_FIFO))
		return (test_fail("sched 70", "get param: incorrect"));

	return (test_pass("sched"));
}
