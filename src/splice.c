/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>
#include "lxtst.h"

#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/syscall.h>

extern ssize_t splice(int, loff_t *, int, loff_t *, size_t, unsigned int);
#define	SPLICE_F_MOVE		0x01
#define	SPLICE_F_NONBLOCK	0x02
#define	SPLICE_F_MORE		0x04
#define	SPLICE_F_GIFT		0x08

static int tc;
static int server_fd, server_acc_fd;

#define	DFLT_PIPE_SIZE	(64 * 1024)

#define DFILE_NAME	"/tmp/lx-tst-splice.dat"
#define TMP_FILE	"/tmp/lx-tst-splice.out"
#define LONG_STR \
    "This is a long string which we use to create a large test data file."

static void
tfail(char *msg)
{
        printf("FAIL splice %d: %s\n", tc, msg);
	unlink(DFILE_NAME);
        exit(1);
}

static void
t_err(char *msg, int rc, int en)
{
	char e[80];

	snprintf(e, sizeof (e), "%s %d %d", msg, rc, en);
	tfail(e);
}

static int
sock_client()
{
	struct sockaddr_in addr;
	int fd;
	struct hostent *server;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket error");
		exit(-1);
	}
	
	server = gethostbyname("localhost");
	if (server == NULL) {
		fprintf(stderr,"ERROR, no such host\n");
		exit(0);
	}
   
	bzero((char *) &addr, sizeof(addr));
	addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, (char *)&addr.sin_addr.s_addr,
	    server->h_length);
	addr.sin_port = htons(5001);
   
	/* Now connect to the server */
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("connect error");
		exit(-1);
	}

	return (fd);
}

static void
sock_fill()
{
	int rc, sfd, fd;
	ssize_t len;
	char buf[64 * 1024];

	sfd = sock_client();

	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	/* Fill the socket from our data file */
	while ((len = read(fd, buf, sizeof (buf))) != 0) {
		if ((rc = write(sfd, buf, len)) != len)
			t_err("write", rc, errno);
	}

	close(fd);
	close(sfd);
}

static int
sock_serv()
{
	struct sockaddr_in addr;
	int cl;

	if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket error");
		exit(-1);
	}
   
	bzero((char *) &addr, sizeof(addr));
   
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(5001);
   
	if (bind(server_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind error");
		exit(-1);
	}

	if (listen(server_fd, 5) == -1) {
		perror("listen error");
		exit(-1);
	}

	if ((cl = accept(server_fd, NULL, NULL)) == -1) {
		perror("accept error");
		exit(1);
	}
	server_acc_fd = cl;

	return (cl);
}

static void
create_data_file(int fsize)
{
	int fd;
	int tot = 0, len = strlen(LONG_STR);

	if ((fd = open(DFILE_NAME, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", fd, errno);

	while (tot < fsize) {
		len = (tot + len) > fsize ? (fsize - tot) : len;
		if (write(fd, LONG_STR, len) != len)
			t_err("write", fd, errno);
		tot += len;
	}

	close(fd);
}

static int
validate_data()
{
	int res;
	char cmd[512];

	snprintf(cmd, sizeof (cmd), "/usr/bin/diff %s %s >/dev/null 2>&1",
	    DFILE_NAME, TMP_FILE);
	res = system(cmd);
	return (res == 0);
}

/*
 * Test a splice from a data file into a pipe. The file must completely
 * fit into the pipe.
 */
static void
test1(int file_size)
{
	int rc, fd, tfd, pfd[2];
	ssize_t s, len;
	char buf[64 * 1024];

	tc = 1;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	s = splice(fd, NULL, pfd[1], NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != file_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size, (int)s);
		tfail(buf);
	}

	close(fd);
	close(pfd[1]);

	while ((len = read(pfd[0], buf, sizeof (buf))) != 0) {
		(void) write(tfd, buf, len);
	}

	close(pfd[0]);
	if (!validate_data())
		tfail("file comparison failed");
	unlink(TMP_FILE);
}

/*
 * Test a splice from a pipe into a data file.
 */
static void
test2(int file_size)
{
	int rc, fd, tfd, pfd[2];
	ssize_t s, len;
	char buf[64 * 1024];

	tc = 2;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	/* First fill the pipe from our data file */
	while ((len = read(fd, buf, sizeof (buf))) != 0) {
		if ((rc = write(pfd[1], buf, len)) != len)
			t_err("write", rc, errno);
	}
	close(fd);
	close(pfd[1]);

	s = splice(pfd[0], NULL, tfd, NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != file_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size, (int)s);
		tfail(buf);
	}

	close(tfd);
	close(pfd[0]);

	if (!validate_data())
		tfail("file comparison failed");
	unlink(TMP_FILE);
}

/*
 * Test a splice from a data file, using an offset, into a pipe. The
 * file must completely fit into the pipe.
 */
static void
test3(int file_size)
{
	int rc, fd, tfd, pfd[2];
	ssize_t s, len;
	loff_t off_in;
	char buf[64 * 1024];

	tc = 3;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	/* splice with an offset 128 bytes in */
	off_in = 128;
	s = splice(fd, &off_in, pfd[1], NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != (file_size - 128)) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size - 128, (int)s);
		tfail(buf);
	}

	close(fd);
	close(pfd[1]);

	while ((len = read(pfd[0], buf, sizeof (buf))) != 0) {
		(void) write(tfd, buf, len);
	}

	close(pfd[0]);
	if (validate_data())
		tfail("file comparison succeeded");
	unlink(TMP_FILE);
}

/*
 * Test a splice from a pipe into a data file, using an offset.
 */
static void
test4(int file_size)
{
	int rc, fd, tfd, pfd[2];
	ssize_t s, len;
	loff_t off_out;
	off_t rc_off;
	char buf[64 * 1024], tbuf[32];
	struct stat sb;

	tc = 4;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((tfd = open(TMP_FILE, O_RDWR | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	/* First fill the pipe from our data file */
	while ((len = read(fd, buf, sizeof (buf))) != 0) {
		if ((rc = write(pfd[1], buf, len)) != len)
			t_err("write", rc, errno);
	}
	close(fd);
	close(pfd[1]);

	/*
	 * Next put some data into the tmp file. This will be partially
	 * overwritten by the splice.
	 */
	snprintf(buf, sizeof (buf),
	    "The quick brown fox jumped over the lazy dog\n");
	len = strlen(buf);
	(void) write(tfd, buf, len);
	if ((rc_off = lseek(tfd, 0, SEEK_SET)) != 0)
		t_err("lseek", rc_off, errno);

	/* splice with an offset 16 bytes in */
	off_out = 16;
	s = splice(pfd[0], NULL, tfd, &off_out, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != file_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size, (int)s);
		tfail(buf);
	}

	close(pfd[0]);

	if ((rc = fstat(tfd, &sb)) < 0)
		t_err("fstat", rc, errno);
	if ((file_size + 16) != sb.st_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size + 16, (int)sb.st_size);
		tfail(buf);
	}

	/* Validate the beginning of the file */
	strncpy(buf, "The quick brown fox jumped", sizeof (buf));
	strncpy(buf + 16, LONG_STR, sizeof (buf) - 16);
	buf[(int)(sizeof (tbuf) - 1)] = '\0';
	if ((rc_off = lseek(tfd, 0, SEEK_SET)) != 0)
		t_err("lseek", rc_off, errno);
	if ((len = read(tfd, tbuf, sizeof (tbuf))) != sizeof (tbuf))
		t_err("read", len, errno);
	tbuf[(int)(sizeof (tbuf) - 1)] = '\0';
	if (strcmp(buf, tbuf) != 0)
		tfail("file header comparison failed");
	close(tfd);

	unlink(TMP_FILE);
}

/*
 * Test a splice from a socket into a pipe. The file must completely
 * fit into the pipe.
 */
static void
test5(int file_size)
{
	pthread_t tid;
	int rc, sfd, tfd, pfd[2];
	ssize_t s, len;
	char buf[64 * 1024];

	tc = 5;

	/* Setup a socket and write our data file into it. */
	pthread_create(&tid, NULL, (void *(*)(void *))sock_fill, (void *)NULL);
	sfd = sock_serv();
	pthread_join(tid, NULL);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	s = splice(sfd, NULL, pfd[1], NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != file_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size, (int)s);
		tfail(buf);
	}

	close(sfd);
	close(pfd[1]);

	while ((len = read(pfd[0], buf, sizeof (buf))) != 0) {
		(void) write(tfd, buf, len);
	}

	close(pfd[0]);
	if (!validate_data())
		tfail("file comparison failed");

	close(server_fd);
	unlink(TMP_FILE);
}

/*
 * Test a splice from a pipe into a socket.
 */
static void
test6(int file_size)
{
	pthread_t tid;
	int rc, fd, tfd, sfd, pfd[2];
	ssize_t s, len;
	char buf[64 * 1024];

	tc = 6;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	/* First fill the pipe from our data file */
	while ((len = read(fd, buf, sizeof (buf))) != 0) {
		if ((rc = write(pfd[1], buf, len)) != len)
			t_err("write", rc, errno);
	}
	close(fd);
	close(pfd[1]);

	/* Setup a socket. */
	pthread_create(&tid, NULL, (void *(*)(void *))sock_serv, (void *)NULL);
	sfd = sock_client();
	pthread_join(tid, NULL);

	s = splice(pfd[0], NULL, sfd, NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != file_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size, (int)s);
		tfail(buf);
	}
	close(pfd[0]);
	close(sfd);

	/* Read the data out of the other end of the socket. */
	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	while ((len = read(server_acc_fd, buf, sizeof (buf))) != 0) {
		if ((rc = write(tfd, buf, len)) != len)
			t_err("write", rc, errno);
	}

	close(tfd);

	if (!validate_data())
		tfail("file comparison failed");

	close(server_acc_fd);
	close(server_fd);
	unlink(TMP_FILE);
}

/*
 * Test a splice from a pipe into a pipe. The file must completely
 * fit into the pipe.
 */
static void
test7(int file_size)
{
	int rc, fd, tfd, afd[2], bfd[2];
	ssize_t s, len;
	char buf[64 * 1024];

	tc = 7;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((rc = pipe(afd)) != 0)
		t_err("pipe", rc, errno);

	if ((rc = pipe(bfd)) != 0)
		t_err("pipe", rc, errno);

	/* First fill the pipe from our data file */
	while ((len = read(fd, buf, sizeof (buf))) != 0) {
		if ((rc = write(afd[1], buf, len)) != len)
			t_err("write", rc, errno);
	}
	close(fd);
	close(afd[1]);

	s = splice(afd[0], NULL, bfd[1], NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != file_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)file_size, (int)s);
		tfail(buf);
	}

	close(afd[0]);
	close(bfd[1]);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	while ((len = read(bfd[0], buf, sizeof (buf))) != 0) {
		(void) write(tfd, buf, len);
	}

	close(tfd);
	close(bfd[0]);
	if (!validate_data())
		tfail("file comparison failed");
	unlink(TMP_FILE);
}

/*
 * Test a splice from a really large data file into a pipe. The file
 * must be bigger than what can completely fit into a default sized pipe
 * so that the splice returns after only writing part of the data.
 */
static void
test8(int file_size)
{
	int rc, fd, tfd, pfd[2];
	ssize_t s, len;
	char buf[64 * 1024];
	struct stat sb;

	tc = 8;
	if ((fd = open(DFILE_NAME, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((tfd = open(TMP_FILE, O_WRONLY | O_CREAT, 0644)) < 0)
		t_err("open", tfd, errno);

	if ((rc = pipe(pfd)) != 0)
		t_err("pipe", rc, errno);

	s = splice(fd, NULL, pfd[1], NULL, (2 * file_size), SPLICE_F_MOVE);
	if (s < 0)
		t_err("splice", s, errno);

	if (s != DFLT_PIPE_SIZE) {
		tfail("splice wrote the whole file");
	}

	close(fd);
	close(pfd[1]);

	while ((len = read(pfd[0], buf, sizeof (buf))) != 0) {
		(void) write(tfd, buf, len);
	}

	close(pfd[0]);

	if ((rc = fstat(tfd, &sb)) < 0)
		t_err("fstat", rc, errno);
	if (s != sb.st_size) {
		snprintf(buf, sizeof (buf), "expected %d, got %d",
		    (int)s, (int)sb.st_size);
		tfail(buf);
	}


	close(tfd);
	unlink(TMP_FILE);
}

int
main(int argc, char **argv)
{
	/* Create a 64k data file which will completely fit into a Linux pipe */
	create_data_file(64 * 1024);

	test1(64 * 1024);
	test2(64 * 1024);
	test3(64 * 1024);
	test4(64 * 1024);
	test5(64 * 1024);
	test6(64 * 1024);
	test7(64 * 1024);

	unlink(DFILE_NAME);

	/*
	 * Create a 128k data file. This won't completely fit into a
	 * default-sized Linux pipe.
	 */
	create_data_file(128 * 1024);

	test8(128 * 1024);

	unlink(DFILE_NAME);

	return (test_pass("splice"));
}
