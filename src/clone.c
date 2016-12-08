/*
 * Copyright 2016 Joyent, Inc.
 */


#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "lxtst.h"

#define	__USE_GNU	1
#include <sched.h>
#include <pthread.h>

#define	STACK_SIZE	65536

static int
c1(void *a)
{
	if (chdir("/tmp") != 0) {
		perror("child chdir failed\n");
		exit(1);
	}
	umask(0751);
	exit(0);
}

static int
c2(void *a)
{
	int i;
	char buf[256];

	/* keep checking until parent changes our cwd and umask */
	for (i = 0; i < 10000; i++) {
		getcwd(buf, sizeof (buf));
		if (strcmp(buf, "/tmp") == 0 && umask(0777) == 0751) {
			exit (0);
		}
		usleep(1000);
	}

	exit (1);
}

static int
c1a(void *a)
{
	if (chdir("/tmp") != 0) {
		perror("child chdir failed\n");
		exit(1);
	}
	exit(0);
}

static int
c2a(void *a)
{
	int i;
	char buf[256];

	/* keep checking until sibling changes our cwd */
	for (i = 0; i < 10000; i++) {
		getcwd(buf, sizeof (buf));
		if (strcmp(buf, "/tmp") == 0) {
			exit (0);
		}
		usleep(1000);
	}

	exit (1);
}

/* cd the group to a file (not a dir) - should fail */
static int
c3(void *a)
{
	char buf[256];

	if (chdir("/proc/self/cmdline") != 0) {
		/* should not have changed our cwd */
		getcwd(buf, sizeof (buf));
		if (strcmp(buf, "/") != 0)
			exit(1);
		if (errno == ENOTDIR)
			exit (0);
		perror("child chdir failed with wrong errno\n");
	}
	exit(1);
}


/* emulate chromium sandboxing for parent */
static int
c4(void *a)
{
	if (chdir("/proc/self/fd") != 0) {
		exit(1);
        }
	if (chroot("/proc/self/fd") != 0) {
		exit(2);
	}
	if (chdir("/") != 0) {
		exit(3);
	}

	exit(0);
}

static int
thr(void *a)
{
	/* thread */
	pause();
        return (0);
}


/* clone with unsupported flag subset (CLONE_VM) - should fail */
static int
test1()
{
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (clone(c1, top, CLONE_VM | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (0);
	return (1);
}

/* clone with supported flag subset (CLONE_FS), but no stack - should fail */
static int
test2()
{
	if (clone(c1, NULL, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (0);
	return (1);
}

/* clone with supported and unsupported flag subset - should fail */
static int
test3()
{
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (clone(c1, top, CLONE_VM | CLONE_FS | SIGCHLD, NULL, NULL, NULL,
	    NULL) < 0)
		return (0);
	return (1);
}

/*
 * clone with supported flag subset (CLONE_FS) - validate that child changed
 * our cwd and umask.
 */
static int
test4()
{
	int stat;
	char buf[256];
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (chdir("/") != 0)
		return (2);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (3);
	umask(0777);

	if (clone(c1, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (4);

	wait(&stat);
	if (WEXITSTATUS(stat) != 0)
		return (5);

	/* child should have changed our cwd */
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/tmp") != 0)
		return (6);

	/* child also should have changed our umask */
	if (umask(0777) != 0751)
		return (7);

	return (0);
}

/*
 * inverse of test4, validate that parent changed changed child's cwd and umask.
 */
static int
test5()
{
	int stat, i;
	char buf[256];
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (chdir("/") != 0)
		return (2);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (3);
	umask(0777);

	if (clone(c2, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (4);

	for (i = 0; i < 10000; i++) {
		if (chdir("/tmp") != 0) {
			return (5);
		}
		umask(0751);

		if (waitpid(-1, &stat, WNOHANG) > 0) {
			if (WEXITSTATUS(stat) != 0)
				return (6);
			break;
		}
		usleep(1000);
	}
	if (i == 10000)
		return (7);

	return (0);
}

/*
 * Locking stress test, clone with supported flag subset (CLONE_FS).
 *
 * The child does the work to modify both processes, but the parent immediately
 * exits.
 *
 * Note, this test does no validation but it could panic the system or leave
 * things in a bad state if we hit an actual kernel bug.
 */
static int
test6worker()
{
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		exit(1);
	top = stack + STACK_SIZE;

	if (clone(c1, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		exit(2);

	exit(0);
}

static int
test6()
{
	int i;
	int pid;

	for (i = 0; i < 500; i++) {
		pid = fork();
		if (pid < 0) {
			printf("FAIL clone 6, fork failed\n");
			exit(1);
		}

		if (pid == 0) {
			/* child */
			test6worker();
			exit(0);
		}
	}

	return (0);
}

/*
 * like tests 4/5, but with 3 processes. Don't check umask since racy.
 */
static int
test7()
{
	int stat, i, found;
	char buf[256];
	char *s1, *t1;
	char *s2, *t2;

	if ((s1 = malloc(STACK_SIZE)) == NULL)
		return (1);
	t1 = s1 + STACK_SIZE;

	if ((s2 = malloc(STACK_SIZE)) == NULL)
		return (1);
	t2 = s2 + STACK_SIZE;

	if (chdir("/") != 0)
		return (2);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (3);

	if (clone(c2a, t1, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (4);
	usleep(1000);
	if (clone(c1a, t2, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (5);

	found = 0;
	for (i = 0; i < 10000; i++) {
		if (waitpid(-1, &stat, WNOHANG) > 0) {
			if (WEXITSTATUS(stat) != 0)
				return (6);
			found++;
			if (found == 2)
				break;
		}
		usleep(1000);
	}
	if (i == 10000)
		return (7);

	/* child should have changed our cwd */
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/tmp") != 0)
		return (8);

	return (0);
}

/*
 * clone with supported flag subset (CLONE_FS) - validate that child failed
 * to change our cwd.
 */
static int
test8()
{
	int stat;
	char buf[256];
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (chdir("/") != 0)
		return (2);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (3);

	if (clone(c3, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (4);

	wait(&stat);
	if (WEXITSTATUS(stat) != 0)
		return (5);

	/* child should not have changed our cwd */
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (6);

	return (0);
}

/*
 * This is another stress test
 * clone with supported flag subset (CLONE_FS), but then immediately exec
 * native app.
 */
static int
test9worker()
{
	char buf[256];
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (chdir("/") != 0)
		return (2);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (3);

	if (clone(c1, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (4);

	execl("/native/usr/bin/pwd", "/native/usr/bin/pwd", NULL);

	return (5);
}

static int
test9()
{
	int i;
	int pid;

	close(1);
	close(2);
	for (i = 0; i < 500; i++) {
		pid = fork();
		if (pid < 0) {
			printf("FAIL clone 6, fork failed\n");
			exit(1);
		}

		if (pid == 0) {
			/* child */
			test9worker();
			exit(0);
		}
	}

	return (0);
}

/*
 * Multi-threaded app clones with supported flag subset (CLONE_FS), but then
 * immediately execs native app.
 */
static int
test10()
{
	char buf[256];
	char *stack, *top;
	pthread_t tid;

	close(1);
	close(2);
	if (pthread_create(&tid, NULL, (void *(*)(void *))thr, NULL) != 0)
		return (1);

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (2);
	top = stack + STACK_SIZE;

	if (chdir("/") != 0)
		return (3);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (4);

	if (clone(c1, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (5);

	execl("/native/usr/bin/pwd", "/native/usr/bin/pwd", NULL);
	return (6);
}

/*
 * Multi-threaded app clones with supported flag subset (CLONE_FS), but then
 * immediately tries to exec invalid app.
 */
static int
test11()
{
	char buf[256];
	char *stack, *top;
	pthread_t tid;

	if (pthread_create(&tid, NULL, (void *(*)(void *))thr, NULL) != 0)
		return (1);

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (2);
	top = stack + STACK_SIZE;

	if (chdir("/") != 0)
		return (3);
	getcwd(buf, sizeof (buf));
	if (strcmp(buf, "/") != 0)
		return (4);

	if (clone(c1, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (5);

	execl("/native/dev/arp", "/native/dev/arp", NULL);
	return (0);
}

/*
 * root-only; test chromium sandboxing where child chroots us into a directory
 * that will be gone when we try to open it.
 */
static int
test12()
{
	int stat;
	char *stack, *top;

	if ((stack = malloc(STACK_SIZE)) == NULL)
		return (1);
	top = stack + STACK_SIZE;

	if (clone(c4, top, CLONE_FS | SIGCHLD, NULL, NULL, NULL, NULL) < 0)
		return (2);

	wait(&stat);
	if (WEXITSTATUS(stat) != 0)
		return (3);

	/* child should have sandboxed us into nonexistent dir */
	if (open("/", 0, O_RDONLY) < 0)
		return (0);
	return (4);
}

static int
run(int tstcase, int (*tc)())
{
	int pid;
	int status;
	int res;

	pid = fork();
	if (pid < 0) {
		printf("FAIL clone %d, initial fork failed\n", tstcase);
		exit(1);
	}

	if (pid == 0) {
		/* child */
		res = tc();
		exit(res);
	}

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) == 0)
		return (0);
	printf("FAIL clone %d, status: %d\n", tstcase, WEXITSTATUS(status));
	exit(1);
}


/*
 * main is the top-level driver of the clone test. We fork a child process for
 * each test and that child process will then clone another process.
 */
int
main(int argc, char **argv)
{
	int am_root = (geteuid() == 0);

	run(1, test1);
	run(2, test2);
	run(3, test3);
	run(4, test4);
	run(5, test5);
	run(6, test6);
	run(7, test7);
	run(8, test8);
	run(9, test9);
	run(10, test10);
	run(11, test11);

	if (!am_root)
		return (test_pass("clone"));

	run(12, test12);

	return (test_pass("clone"));
}
