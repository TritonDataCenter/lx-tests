/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright 2017, Joyent, Inc.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/file.h>
#include "lxtst.h"

#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/ipc.h>
#include <sys/sem.h>

static int tc;
static char tst_file[80];

static int is_lx = 0;
static int got_sig;
static struct timespec delay;

#define	SEMKEY	(1529)

extern int semtimedop(int, struct sembuf *, size_t, const struct timespec *);

static void
tfail(char *msg)
{
	printf("FAIL sig %d: %s\n", tc, msg);
	(void) unlink(tst_file);
        exit(1);
}

static void
t_err(char *msg, int rc, int en)
{
	char e[80];

	snprintf(e, sizeof (e), "%s %d %d", msg, rc, en);
	tfail(e);
}

static void
handler(int signo, siginfo_t *sip, void *cp)
{
	got_sig = 1;
}

/* Setup for a connection, but don't actually accept it */
static int
tsrv()
{
	struct sockaddr_in addr;
	int fd;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
                t_err("socket", fd, errno);
   
	bzero((char *) &addr, sizeof(addr));
   
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(5001);
   
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
                t_err("bind", -1, errno);

	if (listen(fd, 5) == -1)
                t_err("listen", -1, errno);

	return(fd);
}

/* Handle SIGINT and restart interrupted syscall */
static void
setup_sighand()
{
	struct sigaction act;

	got_sig = 0;

	act.sa_flags = SA_RESTART;
	act.sa_sigaction = handler;
	if (sigaction(SIGINT, &act, NULL) != 0)
                t_err("sigaction", 0, errno);
}

static void
child_accept(int pfd, int should_intr)
{
	struct sockaddr_in addr;
	int fd, cl;
	struct timeval tv;

	setup_sighand();

	tv.tv_sec = 5;
	tv.tv_usec = 0;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
                t_err("socket", fd, errno);
   
	/* Set a timeout so that the signal will not cause restart */
	if (should_intr) {
		if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,
		    sizeof (struct timeval)) < 0)
                	t_err("setsockopt", 0, errno);
	}

	bzero((char *) &addr, sizeof(addr));
  
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(5001);
   
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
               	t_err("bind", 0, errno);

	if (listen(fd, 5) == -1) {
		perror("listen error");
		exit(1);
	}

	/* tell the parent we're ready */
	(void) write(pfd, "\n", 1);

	if ((cl = accept(fd, NULL, NULL)) == -1) {
		if (!should_intr) {
			t_err("accept returned", -1, errno);
		} else if (errno != EINTR) {
			t_err("accept expected EINTR", cl, errno);
		}
	}

	if (!should_intr)
		exit(1);
	return;
}

static void
child_recv(int pfd, int should_intr)
{
	struct sockaddr_in addr;
	int fd;
	struct hostent *server;
	struct timeval tv;
	char buf[80];

	setup_sighand();

	tv.tv_sec = 5;
	tv.tv_usec = 0;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
                t_err("socket", fd, errno);

	/* Set a timeout so that the signal will not cause restart */
	if (should_intr) {
		if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,
		    sizeof (struct timeval)) < 0)
                	t_err("setsockopt", 0, errno);
	}

	server = gethostbyname("localhost");
	if (server == NULL)
               	t_err("gethostbyname", 0, errno);
   
	bzero((char *) &addr, sizeof(addr));
	addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, (char *)&addr.sin_addr.s_addr,
	    server->h_length);
	addr.sin_port = htons(5001);
   
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
               	t_err("connect", 0, errno);

	/* tell the parent we're ready */
	(void) write(pfd, "\n", 1);

	if (recv(fd, buf, sizeof (buf), 0) < 0) {
		if (!should_intr) {
			t_err("recv returned", -1, errno);
		} else if (errno != EINTR) {
			t_err("recv expected EINTR", -1, errno);
		}
	}

	if (!should_intr)
		exit(1);
	return;
}

static void
child_flock(int pfd)
{
	int fd;

	setup_sighand();

	if ((fd = open(tst_file, O_RDWR, 0666)) < 0)
                t_err("open", fd, errno);

	/* tell the parent we're ready */
	(void) write(pfd, "\n", 1);

	if (flock(fd, LOCK_EX) < 0)
               	t_err("flock", fd, errno);
	exit(1);
}

static void
child_fcntl(int pfd)
{
	int fd;
	struct flock fl;

	setup_sighand();

	if ((fd = open(tst_file, O_RDWR, 0666)) < 0)
                t_err("open", fd, errno);

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;

	/* tell the parent we're ready */
	(void) write(pfd, "\n", 1);

	if (fcntl(fd, F_SETLKW, &fl) < 0)
               	t_err("fcntl", fd, errno);
	exit(1);
}

static void
child_sem(int pfd)
{
	int semid;
	struct sembuf ops[1];

	setup_sighand();

	if ((semid = semget(SEMKEY, 1, 0666)) < 0)
                t_err("semget", semid, errno);

	/* tell the parent we're ready */
	(void) write(pfd, "\n", 1);

	ops[0].sem_num = 0;
	ops[0].sem_op = 0;
	ops[0].sem_flg = 0;
	if (semtimedop(semid, ops, 1, NULL) < 0)
               	t_err("semtimedop", -1, errno);
	exit(1);
}

/*
 * Test restarted interrupted accept.
 */
static int
test1()
{
	int pid;
	int pfd[2];
	char buf[80];
	int status;

	tc = 1;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_accept(pfd[1], 0);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child accept syscall should restart from this */
	kill(pid, SIGINT);

	/* wait a bit and terminate the child */
	nanosleep(&delay, NULL);
	kill(pid, SIGKILL);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	close(pfd[0]);
	return (0);
}

/*
 * Test interrupted accept due to socket timeout setting.
 */
static int
test2()
{
	int pid;
	int pfd[2];
	char buf[80];
	int status;

	tc = 2;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_accept(pfd[1], 1);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child accept should get EINTR */
	kill(pid, SIGINT);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	close(pfd[0]);
	return (0);
}

/*
 * Test restarted interrupted recv.
 */
static int
test3()
{
	int pid;
	int fd, pfd[2];
	char buf[80];
	int status;

	tc = 3;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);
	fd = tsrv();

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_recv(pfd[1], 0);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child recv syscall should restart from this */
	kill(pid, SIGINT);

	/* wait a bit and terminate the child */
	nanosleep(&delay, NULL);
	kill(pid, SIGKILL);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	close(fd);
	close(pfd[0]);
	return (0);
}

/*
 * Test interrupted recv due to socket timeout setting.
 */
static int
test4()
{
	int pid;
	int fd, pfd[2];
	char buf[80];
	int status;

	tc = 4;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);
	fd = tsrv();

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_recv(pfd[1], 1);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child recv should get EINTR */
	kill(pid, SIGINT);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	close(fd);
	close(pfd[0]);
	return (0);
}

/*
 * Test restarted flock.
 */
static int
test5()
{
	int pid;
	int fd, pfd[2];
	char buf[80];
	int status;

	tc = 5;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);

	if ((fd = open(tst_file, O_RDWR | O_CREAT, 0666)) < 0)
                t_err("open", fd, errno);
	if (flock(fd, LOCK_EX) < 0)
                t_err("flock", fd, errno);

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_flock(pfd[1]);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child flock should get EINTR */
	kill(pid, SIGINT);

	/* wait a bit and terminate the child */
	nanosleep(&delay, NULL);
	kill(pid, SIGKILL);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	(void) flock(fd, LOCK_UN);
	close(fd);
	(void) unlink(tst_file);
	close(pfd[0]);
	return (0);
}

/*
 * Test restarted fcntl file locking.
 */
static int
test6()
{
	int pid;
	int fd, pfd[2];
	char buf[80];
	int status;
	struct flock fl;

	tc = 6;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);

	if ((fd = open(tst_file, O_RDWR | O_CREAT, 0666)) < 0)
                t_err("open", fd, errno);

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;

	if (fcntl(fd, F_SETLKW, &fl) < 0)
                t_err("fcntl", fd, errno);

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_fcntl(pfd[1]);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child flock should get EINTR */
	kill(pid, SIGINT);

	/* wait a bit and terminate the child */
	nanosleep(&delay, NULL);
	kill(pid, SIGKILL);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	fl.l_type = F_UNLCK;
	(void) fcntl(fd, F_SETLKW, &fl);

	close(fd);
	(void) unlink(tst_file);
	close(pfd[0]);
	return (0);
}

/*
 * Test restarted semtimedop.
 */
static int
test7()
{
	int pid;
	int semid, pfd[2];
	char buf[80];
	int status;
	int val;
	struct sembuf ops[1];

	tc = 7;

	if (pipe(pfd) != 0)
                t_err("pipe", 0, errno);

	if ((semid = semget(SEMKEY, 1, 0666 | IPC_CREAT)) < 0)
                t_err("semget", semid, errno);

	val = 0;
	if (semctl(semid, 0, SETVAL, val) < 0)
                t_err("semget", -1, errno);

	ops[0].sem_num = 0;
	ops[0].sem_op = 1;
	ops[0].sem_flg = SEM_UNDO;
	if (semop(semid, ops, 1) < 0)
                t_err("semop", -1, errno);

	pid = fork();
	if (pid == 0) {
		close(pfd[0]);
		child_sem(pfd[1]);
		exit(0);
	}
	close(pfd[1]);

	/* wait until child is ready */
	if (read(pfd[0], buf, sizeof (buf)) < 0)
                t_err("read", 0, errno);
	nanosleep(&delay, NULL);

	/* child semtimedop should get EINTR */
	kill(pid, SIGINT);

	/* wait a bit and terminate the child */
	nanosleep(&delay, NULL);
	kill(pid, SIGKILL);

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) != 0)
                t_err("waitpid", status, errno);
	ops[0].sem_num = 0;
	ops[0].sem_op = -1;
	ops[0].sem_flg = 0;
	if (semop(semid, ops, 1) < 0)
                t_err("semop", -1, errno);
	if (semctl(semid, 0, IPC_RMID) < 0)
                t_err("semctl rm", -1, errno);

	close(pfd[0]);
	return (0);
}

int
main(int argc, char **argv)
{
	struct utsname nm;

	delay.tv_sec = 0;
	delay.tv_nsec = 1000000;
	snprintf(tst_file, sizeof(tst_file), "lxtmp%d", getpid());

	uname(&nm);
	if (strstr(nm.version, "BrandZ") != NULL)
		is_lx = 1;

	/* The following 7 cases test restartable syscalls */
	test1();	/* socket accept - no timeout - restart */
	test2();	/* socket accept - timeout    - norestart */
	test3();	/* socket recv   - no timeout - restart */
	test4();	/* socket recv   - timeout    - no restart */
	test5();	/* flock - restart */
	test6();	/* fcntl - restart */
	test7();	/* semtimedop - restart */

	return (test_pass("sig"));
}
