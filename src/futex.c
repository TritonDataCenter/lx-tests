/*
 * Copyright 2016 Joyent, Inc.
 */

/*
 * Currently these tests are focused on PI futexes as exposed via the pthread
 * API. I have confirmed the expected underlying futex(2) calls via both
 * strace and DTrace.
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <linux/futex.h>
#include <sys/time.h>
#include "lxtst.h"

#ifndef SCHED_BATCH
#define	SCHED_BATCH	3
#endif

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>

pthread_mutexattr_t attr;
pthread_mutex_t m;

static int val;

/* child and parent counters */
static int c_cnt, p_cnt;

/*
 * This is used for a shared state machine, along with a nanosleep, to ensure
 * the two threads are the expected state for the specific test case.
 */
static int state;

static int chld_id;
static int par_id;
static int tc;
static struct timespec ts;
static pthread_t tid;

static int
futex(int *uaddr, int futex_op, const struct timespec *timeout)
{
	return (syscall(SYS_futex, uaddr, futex_op, 0, timeout));
}

static int
tfail(char *msg)
{
	printf("FAIL futex %d: %s\n", tc, msg);
	exit(1);
}

static void
advance_state(int expect)
{
	char buf[80];

	if (state != expect) {
		snprintf(buf, sizeof (buf), "state out of sync, "
		   "expected %d, got %d", 0, state);
		tfail(buf);
	}

	state++;
}

static void
thr1()
{
	char buf[80];
	int *pm = (int *)&m;
	int i;

	chld_id = syscall(SYS_gettid);
	advance_state(0);

	errno = 0;
	if (pthread_mutex_lock(&m) != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	while (state == 1)
		nanosleep(&ts, NULL);

	for (i = 0; i < 10; i++) {
		if (*pm == (chld_id | FUTEX_WAITERS))
			break;
		nanosleep(&ts, NULL);
	}

	if (i == 10 && *pm != (chld_id | FUTEX_WAITERS)) {
		snprintf(buf, sizeof (buf), "expected (b) 0x%x, got 0x%x",
		    chld_id | FUTEX_WAITERS, *pm);
		tfail(buf);
	}

	val++;

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}

	advance_state(2);
}

static void
thr2()
{
	char buf[80];
	int *pm = (int *)&m;

	chld_id = syscall(SYS_gettid);
	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d",  errno);
		tfail(buf);
	}

	if (*pm != chld_id) {
		snprintf(buf, sizeof (buf),
		    "expected (b) 0x%x, got 0x%x", chld_id, *pm);
		tfail(buf);
	}
	advance_state(0);
	/* exit while holding robust mutex */
	return;
}

/*
 * Thread exits while holding mutex.
 */
static void
thr_hold_exit()
{
	char buf[80];
	int *pm = (int *)&m;

	chld_id = syscall(SYS_gettid);
	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	if (*pm != chld_id) {
		snprintf(buf, sizeof (buf), "expected (a) 0x%x, got 0x%x",
		    chld_id, *pm);
		tfail(buf);
	}
	advance_state(0);
	/* exit while holding mutex */
}

/*
 * Take a mutex and hold it for a second before releasing it.
 */
static void
thr4()
{
	char buf[80];
	struct timespec csleep;

	chld_id = syscall(SYS_gettid);
	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	csleep.tv_sec = 1;
	csleep.tv_nsec = 0;

	advance_state(0);
	nanosleep(&csleep, NULL);

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}
}

/*
 * Thread exits while holding mutex and after ensuring another thread is
 * waiting on the mutex.
 */
static void
thr6()
{
	char buf[80];
	int *pm = (int *)&m;
	int i;

	chld_id = syscall(SYS_gettid);
	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	if (*pm != chld_id) {
		snprintf(buf, sizeof (buf),
		    "expected (a) 0x%x, got 0x%x", chld_id, *pm);
		tfail(buf);
	}

	advance_state(0);

	/* Wait for parent to be ready to queue on the mutex */
	while (state == 1)
		nanosleep(&ts, NULL);

	/* Wait for parent to enqueue */
	for (i = 0; i < 10; i++) {
		if (*pm == (chld_id | FUTEX_WAITERS))
			break;
		nanosleep(&ts, NULL);
	}

	if (i == 10 && *pm != (chld_id | FUTEX_WAITERS)) {
		snprintf(buf, sizeof (buf), "expected (b) 0x%x, got 0x%x",
		    chld_id | FUTEX_WAITERS, *pm);
		tfail(buf);
	}

	/* exit while holding robust mutex */
}

/*
 * Stress test a normal mutex with two threads at different priorities.
 */
static void
worker_balance(int is_chld)
{
	char buf[80];

	 while (val < 100000) {
		errno = 0;
		if (pthread_mutex_lock(&m) != 0 && errno != 0) {
			snprintf(buf, sizeof (buf), "lock errno %d", errno);
			tfail(buf);
		}

		val++;
		if (is_chld) {
			c_cnt++;
		} else {
			p_cnt++;
		}

		errno = 0;
		if (pthread_mutex_unlock(&m) != 0 && errno != 0) {
			snprintf(buf, sizeof (buf), "unlock errno %d", errno);
			tfail(buf);
		}
        }

	if (c_cnt == 0 || p_cnt == 0) {
		snprintf(buf, sizeof (buf), "unbalanced locking c: %d p: %d",
		    c_cnt, p_cnt);
		tfail(buf);
	}
}

static int
thr_balance(int n)
{
	struct sched_param sp;

	chld_id = syscall(SYS_gettid);
	/* put ourselves into a lower priority class */
	sp.sched_priority = 0;
        (void) sched_setscheduler(0, SCHED_BATCH, &sp);

	worker_balance(1);
	return (0);
}

/* Test EFAULT for invalid mutex address */
static int
test0()
{
	char buf[80];

	tc = 0;
	if (futex((int *)5000, FUTEX_LOCK_PI_PRIVATE, NULL) == 0 ||
	    errno != EFAULT) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}
	return (0);
}

/*
 * Test a normal mutex where the syscall is required:
 * 1) the child takes the mutex in user-land 
 * 2) the parent validates that the mutex value is the child tid
 * 3) the parent tries to lock the mutex, resulting in futex syscall
 * 4) the child releases the mutex
 * 5) the parent is able to successfully take and release the mutex
 */
static int
test1()
{
	char buf[80];
	int *pm = (int *)&m;

	tc = 1;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr1, (void *)NULL);

	while (state == 0)
		nanosleep(&ts, NULL);

	if (*pm != chld_id) {
		snprintf(buf, sizeof (buf), "expected (a) 0x%x, got 0x%x",
		    chld_id, *pm);
		tfail(buf);
	}

	advance_state(1);

	errno = 0;
	if (pthread_mutex_lock(&m) != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	while (state < 3)
		nanosleep(&ts, NULL);

	if (*pm != (par_id | FUTEX_WAITERS)) {
		snprintf(buf, sizeof (buf), "expected (c) 0x%x, got 0x%x",
		    par_id | FUTEX_WAITERS, *pm);
		tfail(buf);
	}

	val++;

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}

	if (*pm != 0) {
		snprintf(buf, sizeof (buf), "expected (d) 0x%x, got 0x%x",
		    0, *pm);
		tfail(buf);
	}
	pthread_join(tid, NULL);
	return (0);
}

/*
 * Test a robust mutex with the following error flow:
 * 1) the child takes the mutex in user-land 
 * 2) the child exits while holding the mutex
 * 3) the parent validates that the mutex value is FUTEX_OWNER_DIED
 * 4) the parent is able to successfully take and release the mutex
 */
static int
test2()
{
	char buf[80];
	int *pm = (int *)&m;

	tc = 2;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
		tfail("pthread_mutexattr_setrobust");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr2, (void *)NULL);

	while (state < 1)
		nanosleep(&ts, NULL);

	/* wait for child to exit while holding the mutex */
	pthread_join(tid, NULL);

	if (*pm != FUTEX_OWNER_DIED) {
		snprintf(buf, sizeof (buf), "expected (a) 0x%x, got 0x%x",
		    FUTEX_OWNER_DIED, *pm);
		tfail(buf);
	}

	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	if (*pm != par_id) {
		snprintf(buf, sizeof (buf), "expected (c) 0x%x, got 0x%x",
		    par_id, *pm);
		tfail(buf);
	}


	val++;

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}

	if (*pm != 0) {
		snprintf(buf, sizeof (buf), "expected (d) 0x%x, got 0x%x",
		    0, *pm);
		tfail(buf);
	}
	return (0);
}

/*
 * Test a non-robust mutex with the following error flow:
 * 1) the child takes the mutex in user-land 
 * 2) the child exits while holding the mutex
 * 3) the parent validates that the mutex value is still the dead tid.
 */
static int
test3()
{
	char buf[80];
	int *pm = (int *)&m;

	tc = 3;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr_hold_exit,
	    (void *)NULL);

	while (state < 1)
		nanosleep(&ts, NULL);

	/* wait for child to exit while holding the mutex */
	pthread_join(tid, NULL);

	if (*pm != chld_id) {
		snprintf(buf, sizeof (buf), "expected (b) 0x%x, got 0x%x",
		    chld_id, *pm);
		tfail(buf);
	}
	return (0);
}

/*
 * Test a mutex lock that times out.
 */
static int
test4()
{
	char buf[80];
	struct timespec psleep;
	int *pm = (int *)&m;
	int r;

	tc = 4;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr4, (void *)NULL);

	while (state < 1)
		nanosleep(&ts, NULL);

	if (*pm != chld_id) {
		snprintf(buf, sizeof (buf), "expected (a) 0x%x, got 0x%x",
		    chld_id, *pm);
		tfail(buf);
	}

	clock_gettime(CLOCK_REALTIME , &psleep);
	psleep.tv_nsec += 1000000;

	if ((r = pthread_mutex_timedlock(&m, &psleep)) != ETIMEDOUT) {
		snprintf(buf, sizeof (buf), "timedlock %d", r);
		tfail(buf);
	}

	if (*pm != (chld_id | FUTEX_WAITERS)) {
		snprintf(buf, sizeof (buf), "expected (b) 0x%x, got 0x%x",
		    chld_id | FUTEX_WAITERS, *pm);
		tfail(buf);
	}

	return (0);
}

/*
 * Child exits while holding futex. Use pthread trylock to force futex_trylock.
 */
static int
test5()
{
	char buf[80];
	int *pm = (int *)&m;

	tc = 5;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
		tfail("pthread_mutexattr_setrobust");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr_hold_exit,
	    (void *)NULL);

	while (state < 1)
		nanosleep(&ts, NULL);

	/* wait for child to exit while holding the mutex */
	pthread_join(tid, NULL);

	if (*pm != FUTEX_OWNER_DIED) {
		snprintf(buf, sizeof (buf), "expected (a) 0x%x, got 0x%x",
		    FUTEX_OWNER_DIED, *pm);
		tfail(buf);
	}

	errno = 0;
	if (pthread_mutex_trylock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "trylock errno %d", errno);
		tfail(buf);
	}

	val++;

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}
	return (0);
}

/*
 * Child exits while holding mutex and after we're enqueued on it.
 */
static int
test6()
{
	char buf[80];
	int *pm = (int *)&m;

	tc = 6;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
		tfail("pthread_mutexattr_setrobust");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr6, (void *)NULL);

	/* Wait for child to take the mutex */
	while (state < 1)
		nanosleep(&ts, NULL);

	/* Tell child to proceed - i.e. exit */
	advance_state(1);

	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	val++;

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}

	if (*pm != 0) {
		snprintf(buf, sizeof (buf), "expected (c) 0x%x, got 0x%x",
		    0, *pm);
		tfail(buf);
	}

	return (0);
}

/* Test EDEADLK for taking futex twice */
static int
test7()
{
	char buf[80];
	int *pm = (int *)&m;

	tc = 7;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
		tfail("pthread_mutexattr_setrobust");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	errno = 0;
	if (pthread_mutex_lock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	if (*pm != par_id) {
		snprintf(buf, sizeof (buf), "expected (c) 0x%x, got 0x%x",
		    par_id, *pm);
		tfail(buf);
	}

	if (futex((int *)&m, FUTEX_LOCK_PI_PRIVATE, NULL) == 0 ||
	    errno != EDEADLK) {
		snprintf(buf, sizeof (buf), "lock errno %d", errno);
		tfail(buf);
	}

	errno = 0;
	if (pthread_mutex_unlock(&m) != 0 && errno != 0) {
		snprintf(buf, sizeof (buf), "unlock errno %d", errno);
		tfail(buf);
	}

	return (0);
}

/* Test that threads at different priorities are not starved on the mutex */
static int
test_balance()
{
	tc = 99;
	c_cnt = p_cnt = 0;
	state = 0;
	if (pthread_mutexattr_init(&attr) != 0)
		tfail("pthread_mutexattr_init");

	if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != 0)
		tfail("pthread_mutexattr_setprotocol");

	if (pthread_mutex_init(&m, &attr) != 0)
		tfail("pthread_mutex_init");

	pthread_create(&tid, NULL, (void *(*)(void *))thr_balance,
	    (void *)NULL);

	worker_balance(0);
	return (0);
}

int
main(int argc, char **argv)
{
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;

	par_id = syscall(SYS_gettid);

	test0();
	test1();
	test2();
	test3();
	test4();
	test5();
	test6();
	test7();
	test_balance();
	return (test_pass("futex"));
}
