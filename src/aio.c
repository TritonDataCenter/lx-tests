/*
 * Copyright 2017 Joyent, Inc.
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
#include <sys/eventfd.h>
#include <linux/aio_abi.h>
#include "lxtst.h"

#define _GNU_SOURCE
#include <sys/syscall.h>

#define	BLKSIZE		(512)
#define	NPAR		(4)
#define	FILE_BLOCKS	(NPAR)
#define BLOCK_TAG	"test data in block %d"

static int tc;
static char tst_file[80];

static int is_lx = 0;
static aio_context_t gctx;
static int gztot;
static int state;
static int evfd;
static struct timespec delay;

static void
tfail(char *msg)
{
        printf("FAIL aio %d: %s\n", tc, msg);
	unlink(tst_file);
        exit(1);
}

static void
t_err(char *msg, int rc, int en)
{
	char e[80];

	snprintf(e, sizeof (e), "%s %d %d", msg, rc, en);
	tfail(e);
}

int
io_setup(int nr, aio_context_t *ctxp)
{
	return (syscall(SYS_io_setup, nr, ctxp));
}

int
io_submit(aio_context_t ctx, long nr, struct iocb *cbpp[])
{
	return (syscall(SYS_io_submit, ctx, nr, cbpp));
}

int
io_getevents(aio_context_t ctx, long minnr, long nr, struct io_event *ep,
    struct timespec *tp)
{
	return (syscall(SYS_io_getevents, ctx, minnr, nr, ep, tp));
}

int
io_cancel(aio_context_t ctx, struct iocb *cb, struct io_event *ep)
{
	return (syscall(SYS_io_cancel, ctx, cb, ep));
}

int
io_destroy(aio_context_t ctx)
{
	return (syscall(SYS_io_destroy, ctx));
}

static struct iocb *
mk_cb(int fd, int op, size_t offset, long data)
{
	struct iocb *io;
	char *buf;

	buf = (char *)malloc(BLKSIZE);
	io = (struct iocb *)calloc(1, sizeof (struct iocb));
	if (buf == NULL || io == NULL)
		tfail("out of memory");

	io->aio_data = data;
	io->aio_key = 0;
	io->aio_lio_opcode = op;
	io->aio_reqprio = 0;
	io->aio_fildes = fd;
	io->aio_buf = (__u64)buf;
	io->aio_nbytes = BLKSIZE;
	io->aio_offset = offset;
	io->aio_flags = 0;

	return (io);
}

static int
setup_sock()
{
	struct sockaddr_in addr;
	int fd;
	struct hostent *server;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket error");
		exit(-1);
	}

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
tsrv()
{
	struct sockaddr_in addr;
	int fd, cl;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket error");
		exit(-1);
	}
   
	bzero((char *) &addr, sizeof(addr));
   
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(5001);
   
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind error");
		exit(-1);
	}

	state = 1;
	if (listen(fd, 5) == -1) {
		perror("listen error");
		exit(-1);
	}

	if ((cl = accept(fd, NULL, NULL)) == -1) {
		perror("accept error");
		exit(1);
	}

	while (state < 2)
                nanosleep(&delay, NULL);

	close(cl);
	close(fd);
}

/* Run a test case in a different process */
static int
run_as_proc(int tstcase, int (*tf)(), void *arg)
{
	int pid;
	int status;
	int res;

	tc = tstcase;
	pid = fork();
	if (pid < 0)
		t_err("fork", pid, errno);

	if (pid == 0) {
		/* child */
		res = tf(arg);
		exit(res);
	}

	waitpid(pid, &status, 0);
	if (WEXITSTATUS(status) == 0)
		return (0);
	printf("FAIL aio %d, status: %d\n", tc, WEXITSTATUS(status));
	exit(1);
}

/*
 * Test parallel writes with single blocking getevent consumer.
 * This must be the first test since it creates a test file the rest of the
 * test will depend on.
 */
static int
test1(char *fname)
{
	int rand_fd, fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;

	tc = 1;
	if ((rand_fd = open("/dev/urandom", O_RDONLY)) < 0) {
		perror("open /dev/urandom");
		exit(1);
	}

	if ((fd = open(fname, O_WRONLY | O_CREAT, 0666)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < NPAR; i++) {
		char *buf;
		struct iocb *io;

		buf = (char *)malloc(BLKSIZE);
		io = (struct iocb *)calloc(1, sizeof (struct iocb));
		if (buf == NULL || io == NULL)
			tfail("out of memory");

		if ((rc = read(rand_fd, buf, BLKSIZE)) != BLKSIZE)
			t_err("read", rc, errno);
		snprintf(buf, BLKSIZE, BLOCK_TAG, i);

		io->aio_data = i;
		io->aio_key = 0;
		io->aio_lio_opcode = IOCB_CMD_PWRITE;
		io->aio_reqprio = 0;
		io->aio_fildes = fd;
		io->aio_buf = (__u64)buf;
		io->aio_nbytes = BLKSIZE;
		io->aio_offset = i * BLKSIZE;
		io->aio_flags = 0;

		ioq[i] = io;
	}

	close(rand_fd);

	/* Submit parallel writes */
	rc = io_submit(ctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	/* Validate IO one at a time until completed */
	for (i = 0; i < NPAR; i++) {
		struct io_event event, *ep = &event;
		struct iocb *iop;
		int n;

		rc = io_getevents(ctx, 1, 1, &event, NULL);
		if (rc != 1)
			t_err("getevents", rc, errno);

		iop = (struct iocb *)ep->obj;

		n = (int)ep->data;

		if (n < 0 || n >= NPAR)
			tfail("unexpected data tag");

		if (ep->res != BLKSIZE)
			tfail("unexpected res");

		if (iop->aio_nbytes != BLKSIZE)
			tfail("unexpected nbytes");

		if (iop->aio_offset != (n * BLKSIZE))
			tfail("unexpected offset");

		free((void *)iop->aio_buf);
		free(iop);
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Test multiple reads with single getevent consumer.
 * tnum == 2; test 0:0 timeout - poll
 * tnum == 3; test NULL timeout - block
 * tnum == 4; test 0:10000 timeout - wait a bit
 */
static int
test2(char *fname, int tnum)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;

	tc = tnum;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < NPAR; i++) {
		ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
	}

	rc = io_submit(ctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	if (tnum == 2) {
		/* Test getevents with valid (but no-op) arguments */
		rc = io_getevents(ctx, 0, 0, NULL, NULL);
		if (rc != 0)
			t_err("empty getevents", rc, errno);
	}

	/* Handle IO one at a time until completed */
	for (i = 0; i < NPAR; i++) {
		struct timespec timeout = { 0, 0 };
		struct timespec *tp;
		struct io_event event, *ep = &event;
		struct iocb *iop;
		char *ebuf;
		int n, len;
		char tag[80];

		switch (tnum) {
		case 2:	tp = &timeout;
			break;
		case 3: tp = NULL;
			break;
		default: tp = &timeout;
			tp->tv_nsec = 10000;
			break;
		}

		/* tnum 3 is blocking */
		if (tnum == 3) {
			rc = io_getevents(ctx, 1, 1, &event, tp);
			if (rc != 1)
				t_err("getevents", rc, errno);
		} else {
retry:
			rc = io_getevents(ctx, 1, 1, &event, tp);
			if (rc == 0)	/* timeout */
				goto retry;
			if (rc != 1)
				t_err("getevents", rc, errno);
		}

		iop = (struct iocb *)ep->obj;
		ebuf = (char *)iop->aio_buf;

		n = (int)ep->data;
		snprintf(tag, sizeof (tag), BLOCK_TAG, n);
		len = strlen(tag);

		if (n < 0 || n >= NPAR)
			tfail("unexpected data tag");

		if (ep->res != BLKSIZE)
			tfail("unexpected res");

		if (iop->aio_nbytes != BLKSIZE)
			tfail("unexpected nbytes");

		if (iop->aio_offset != (n * BLKSIZE))
			tfail("unexpected offset");

		if (strncmp(ebuf, tag, len) != 0)
			tfail("unexpected block tag");

		free((void *)iop->aio_buf);
		free(iop);
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/* Test parallel reads with blocking getevent consumer on all of them */
static int
test5(char *fname)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event events[NPAR];

	tc = 5;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < NPAR; i++) {
		ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
	}

	/* Submit parallel reads */
	rc = io_submit(ctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, NPAR, NPAR, events, NULL);
	if (rc != NPAR)
		t_err("getevents", rc, errno);

	for (i = 0; i < NPAR; i++) {
		struct io_event *ep = &events[i];
		struct iocb *iop;
		char *ebuf;
		int n, len;
		char tag[80];

		iop = (struct iocb *)ep->obj;
		ebuf = (char *)iop->aio_buf;

		n = (int)ep->data;
		snprintf(tag, sizeof (tag), BLOCK_TAG, n);
		len = strlen(tag);

		if (n < 0 || n >= NPAR)
			tfail("unexpected data tag");

		if (ep->res != BLKSIZE)
			tfail("unexpected res");

		if (iop->aio_nbytes != BLKSIZE)
			tfail("unexpected nbytes");

		if (iop->aio_offset != (n * BLKSIZE))
			tfail("unexpected offset");

		if (strncmp(ebuf, tag, len) != 0)
			tfail("unexpected block tag");

		free((void *)iop->aio_buf);
		free(iop);
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Test submitting lots of CBs then cancel last one.
 */
static int
test6(char *fname)
{
	int i, fd, rc;
	int n = 1024;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event event;

	tc = 6;
	/* initialize */
	ctx = 0;
	rc = io_setup(n, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * n);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < n; i++) {
		ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, 0, (long)i);
	}
	i--;

	rc = io_submit(ctx, n, ioq);
	if (rc != n)
		t_err("submit", rc, errno);

	/* cancel last one */
	/* result arg is no longer used in Linux */
	rc = io_cancel(ctx, ioq[i], &event);
	if (rc == -1 && errno != EAGAIN && errno != EINVAL)
		t_err("cancel", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

/*
 * Test read submition then destroy.
 */
static int
test7(char *fname)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;

	tc = 7;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < NPAR; i++) {
		ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
	}

	rc = io_submit(ctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Test fsync.
 */
static int
test8(char *fname)
{
	int fd, rc;
	aio_context_t ctx;
	struct iocb **ioq;
	struct iocb *io;
	struct io_event event, *ep = &event;
	struct iocb *iop;
	int n;

	tc = 8;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(1, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	io = (struct iocb *)calloc(1, sizeof (struct iocb));
	if (io == NULL)
		tfail("out of memory");

	io->aio_data = 16;
	io->aio_key = 0;
	io->aio_lio_opcode = IOCB_CMD_FSYNC;
	io->aio_reqprio = 0;
	io->aio_fildes = fd;
	io->aio_flags = 0;

	ioq[0] = io;

	rc = io_submit(ctx, 1, ioq);
	if (rc != 1)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 1, 1, &event, NULL);
	if (rc != 1)
		t_err("getevents", rc, errno);

	iop = (struct iocb *)ep->obj;

	n = (int)ep->data;

	if (n != 16)
		tfail("unexpected data tag");

	if (ep->res != 0)
		tfail("unexpected res");

	free((void *)iop->aio_buf);
	free(iop);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/* Test read with NULL buffer (we've seen traces that do this) */
static int
test9(char *fname)
{
	int fd, rc;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event event, *ep = &event;
	struct iocb *iop;
	int n;
	struct iocb *io;

	tc = 9;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * 1);
	if (ioq == NULL)
		tfail("out of memory");

	io = mk_cb(fd, IOCB_CMD_PREAD, 0, (long)47);
	free((void *)io->aio_buf);
	io->aio_buf = (__u64)NULL;
	io->aio_nbytes = 0;
	io->aio_offset = 47;
	ioq[0] = io;

	rc = io_submit(ctx, 1, ioq);
	if (rc != 1)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 1, 1, &event, NULL);
	if (rc != 1)
		t_err("getevents", rc, errno);

	iop = (struct iocb *)ep->obj;

	n = (int)ep->data;

	if (n != 47)
		tfail("unexpected data tag");

	if (ep->res != 0)
		tfail("unexpected res");

	if (iop->aio_nbytes != 0)
		tfail("unexpected nbytes");

	if (iop->aio_offset != 47)
		tfail("unexpected offset");

	free((void *)iop->aio_buf);
	free(iop);


	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/* Test parallel reads on different fd's */
static int
test10(char *fname)
{
	int fd0, fd1, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event events[NPAR];

	tc = 10;
	if ((fd0 = open(fname, O_RDONLY)) < 0)
		t_err("open", fd0, errno);

	if ((fd1 = open("aio.c", O_RDONLY)) < 0)
		t_err("open", fd1, errno);

	ctx = 0;
	rc = io_setup(2, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * 2);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < 2; i++) {
		struct iocb *io;

		if (i == 0) {
			io = mk_cb(fd0, IOCB_CMD_PREAD, 0, (long)i);
		} else {
			io = mk_cb(fd1, IOCB_CMD_PREAD, 0, (long)i);
		}

		ioq[i] = io;
	}

	rc = io_submit(ctx, 2, ioq);
	if (rc != 2)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 2, 2, events, NULL);
	if (rc != 2)
		t_err("getevents", rc, errno);

	/* validate both IO's */
	for (i = 0; i < 2; i++) {
		struct io_event *ep = &events[i];
		struct iocb *iop;
		char *ebuf;
		int n, len;
		char tag[80];

		iop = (struct iocb *)ep->obj;
		ebuf = (char *)iop->aio_buf;

		n = (int)ep->data;
		if (n < 0 || n >= 2)
			tfail("unexpected data tag");

		if (ep->res != BLKSIZE)
			tfail("unexpected res");

		if (iop->aio_nbytes != BLKSIZE)
			tfail("unexpected nbytes");

		if (iop->aio_offset != 0)
			tfail("unexpected offset");

		if (n == 0) {
			snprintf(tag, sizeof (tag), BLOCK_TAG, n);
			len = strlen(tag);
		} else {
			snprintf(tag, sizeof (tag), "/*\n * Copyright 20");
			len = strlen(tag);
		}

		if (strncmp(ebuf, tag, len) != 0)
			tfail("unexpected block tag");

		free((void *)iop->aio_buf);
		free(iop);
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd0);
	close(fd1);
	return (0);
}

/*
 * Test read which results in an error on the first CB, first with invalid
 * fd, then with invalid cmd.
 */
static int
test11(char *fname)
{
	int fd, rc;
	aio_context_t ctx;
	struct iocb **ioq;
	struct iocb *io;

	tc = 11;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	io = mk_cb(fd, IOCB_CMD_PREAD, 0, 0L);
	io->aio_fildes = 0;	/* pipe/socket is an error */
	ioq[0] = io;

	rc = io_submit(ctx, 1, ioq);
	if (rc != -1 || errno != EINVAL) {
		int err = errno;
		printf("expected submit EINVAL, got %d %d\n", rc, err);
		t_err("submit", rc, err);
	}

	io->aio_fildes = fd;
	io->aio_lio_opcode = 99;
	ioq[0] = io;

	rc = io_submit(ctx, 1, ioq);
	if (rc != -1 || errno != EINVAL) {
		int err = errno;
		printf("expected submit EINVAL, got %d %d\n", rc, err);
		t_err("submit", rc, err);
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Test reads which result in an error at syscall time on a CB which is not
 * the first. The first CB should go through.
 */
static int
test12(char *fname)
{
	int fd, rc;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event events[NPAR];
	struct iocb *io;

	tc = 12;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	io = mk_cb(fd, IOCB_CMD_PREAD, 0, 0L);
	ioq[0] = io;
	io = mk_cb(fd, IOCB_CMD_PREAD, BLKSIZE, 0L);
	io->aio_fildes = 0;	/* pipe/socket is an error */
	ioq[1] = io;

	rc = io_submit(ctx, 2, ioq);
	if (rc != 1)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 1, 2, events, NULL);
	if (rc != 1)
		t_err("getevents", rc, errno);

	struct io_event *ep = &events[0];
	struct iocb *iop;

	iop = (struct iocb *)ep->obj;
	if (ep->res != BLKSIZE)
		tfail("unexpected res");
	free((void *)iop->aio_buf);

	free(iop);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Test reads which result in an error at I/O time on a CB due to invalid
 * buffer.
 */
static int
test13(char *fname)
{
	int i, fd, rc;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event events[NPAR];
	struct iocb *io;

	tc = 13;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	io = mk_cb(fd, IOCB_CMD_PREAD, 0, 0L);
	ioq[0] = io;
	io = mk_cb(fd, IOCB_CMD_PREAD, BLKSIZE, 0L);
	free((void *)io->aio_buf);
	io->aio_buf = (__u64)2048;
	ioq[1] = io;

	rc = io_submit(ctx, 2, ioq);
	if (rc != 2)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 2, 2, events, NULL);
	if (rc != 2)
		t_err("getevents", rc, errno);

	for (i = 0; i < 2; i++) {
		struct io_event *ep = &events[i];
		struct iocb *iop;

		iop = (struct iocb *)ep->obj;
		if (iop->aio_offset == BLKSIZE) {
			if (ep->res != -EFAULT)
				tfail("unexpected res");
		} else {
			if (ep->res != BLKSIZE)
				tfail("unexpected res");
			free((void *)iop->aio_buf);
		}

		free(iop);
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/* Test destroy without ever issuing an IO */
static int
test14(char *fname)
{
	int rc;
	aio_context_t ctx;

	tc = 14;
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

/* Test too many CBs */
static int
test15(char *fname)
{
	int i, rc;
	aio_context_t ctx[5];

	tc = 15;
	for (i = 0; i < 5; i++) {
		ctx[i] = 0;
		rc = io_setup((16 * 1024), &ctx[i]);
		if (rc < 0) {
			if (i != 4 || errno != EAGAIN)
				t_err("setup", rc, errno);
		}
	}

	for (i = 0; i < 4; i++) {
		rc = io_destroy(ctx[i]);
		if (rc < 0)
			t_err("destroy", rc, errno);
	}
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

/* Test two reads using two different contexts */
static int
test16(char *fname)
{
	int fd, rc, i;
	aio_context_t ctx0, ctx1;
	struct iocb **ioq0, **ioq1;

	tc = 16;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx0 = 0;
	rc = io_setup(NPAR, &ctx0);
	if (rc < 0)
		t_err("setup", rc, errno);

	ctx1 = 0;
	rc = io_setup(NPAR, &ctx1);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq0 = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	ioq1 = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq0 == NULL || ioq1 == NULL)
		tfail("out of memory");

	for (i = 0; i < 1; i++) {
		ioq0[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
		ioq1[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
	}

	rc = io_submit(ctx0, 1, ioq0);
	if (rc != 1)
		t_err("submit", rc, errno);
	rc = io_submit(ctx1, 1, ioq1);
	if (rc != 1)
		t_err("submit", rc, errno);

	for (i = 0; i < 2; i++) {
		struct io_event event, *ep = &event;
		struct iocb *iop;
		char *ebuf;
		int n, len;
		char tag[80];

		if (i == 0) {
			rc = io_getevents(ctx1, 1, 1, ep, NULL);
		} else {
			rc = io_getevents(ctx0, 1, 1, ep, NULL);
		}
		if (rc != 1)
			t_err("getevents", rc, errno);

		iop = (struct iocb *)ep->obj;
		ebuf = (char *)iop->aio_buf;

		n = (int)ep->data;
		snprintf(tag, sizeof (tag), BLOCK_TAG, n);
		len = strlen(tag);

		if (n < 0 || n >= NPAR)
			tfail("unexpected data tag");

		if (ep->res != BLKSIZE)
			tfail("unexpected res");

		if (iop->aio_nbytes != BLKSIZE)
			tfail("unexpected nbytes");

		if (iop->aio_offset != (n * BLKSIZE))
			tfail("unexpected offset");

		if (strncmp(ebuf, tag, len) != 0)
			tfail("unexpected block tag");

		free((void *)iop->aio_buf);
		free(iop);
	}

	rc = io_destroy(ctx0);
	if (rc != 0)
		t_err("destroy", rc, errno);

	rc = io_destroy(ctx1);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

static void
test17_val()
{
	struct io_event event, *ep = &event;
	struct iocb *iop;
	char *ebuf;
	int rc, n, len;
	char tag[80];

	rc = io_getevents(gctx, 1, 1, ep, NULL);
	if (rc != 1)
		t_err("getevents", rc, errno);

	iop = (struct iocb *)ep->obj;
	ebuf = (char *)iop->aio_buf;

	n = (int)ep->data;
	snprintf(tag, sizeof (tag), BLOCK_TAG, n);
	len = strlen(tag);

	if (n < 0 || n >= NPAR)
		tfail("unexpected data tag");

	if (ep->res != BLKSIZE)
		tfail("unexpected res");

	if (iop->aio_nbytes != BLKSIZE)
		tfail("unexpected nbytes");

	if (iop->aio_offset != (n * BLKSIZE))
		tfail("unexpected offset");

	if (strncmp(ebuf, tag, len) != 0)
		tfail("unexpected block tag");

	free((void *)iop->aio_buf);
	free(iop);
}

static void
t17()
{
	state = 1;
	test17_val();
}

/*
 * Test two reads using the same context but a getevent from two diff. threads
 * Thread b will block on getevent before we submit any IO.
 */
static int
test17(char *fname)
{
	int fd, rc;
	struct iocb **ioq;
	pthread_t tid;

	tc = 17;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	gctx = 0;
	rc = io_setup(NPAR, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	/* start thread and block for event before submit */
	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t17, (void *)NULL);
	while (state == 0)
		nanosleep(&delay, NULL);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	ioq[0] = mk_cb(fd, IOCB_CMD_PREAD, 0 * BLKSIZE, (long)0);
	ioq[1] = mk_cb(fd, IOCB_CMD_PREAD, 1 * BLKSIZE, (long)1);

	rc = io_submit(gctx, 2, ioq);
	if (rc != 2)
		t_err("submit", rc, errno);

	test17_val();
	pthread_join(tid, NULL);

	rc = io_destroy(gctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

static void
t18()
{
	struct io_event events[NPAR];
	int rc;

	state = 1;
	rc = io_getevents(gctx, NPAR, NPAR, events, NULL);
	/* We're being interrupted - should get 0 or 1 event */
	if (rc != 0 && rc != 1)
		t_err("getevents", rc, errno);
}

/*
 * Test 2 threads, thread B blocks in io_getevents waiting for events, thread A
 * then destroys the context.
 */
static int
test18(char *fname)
{
	int fd, rc;
	struct iocb **ioq;
	pthread_t tid;

	tc = 18;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	gctx = 0;
	rc = io_setup(NPAR, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	ioq[0] = mk_cb(fd, IOCB_CMD_PREAD, 0 * BLKSIZE, (long)0);

	rc = io_submit(gctx, 1, ioq);
	if (rc != 1)
		t_err("submit", rc, errno);

	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t18, (void *)NULL);

	while (state == 0)
		nanosleep(&delay, NULL);

	rc = io_destroy(gctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	pthread_join(tid, NULL);

	close(fd);
	return (0);
}

static void
t19()
{
	struct io_event events[128];

	state = 1;
	(void) io_getevents(gctx, 128, 128, events, NULL);
	exit(0);
}

/*
 * Similar to test 18.
 *
 * Test 2 threads; thread B blocks in io_getevents waiting for events, thread A
 * then destroys the context. Thread B then exits the process while thread A is
 * still in io_destroy.
 */
static int
test19(char *fname)
{
	int rc;
	pthread_t tid;

	tc = 19;
	gctx = 0;
	rc = io_setup(128, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t19, (void *)NULL);

	while (state == 0)
		nanosleep(&delay, NULL);

	rc = io_destroy(gctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	/* The thread never returns, it exits, so no pthread_join */
	return (0);
}

static void
t20()
{
	struct io_event events[128];

	state = 1;
	(void) io_getevents(gctx, 128, 128, events, NULL);
}

/*
 * Another variation on the above.
 *
 * Test 2 threads; thread B blocks in io_getevents waiting for events, thread A
 * then exits the process while thread B is blocked in io_getevents.
 */
static int
test20(char *fname)
{
	int rc;
	pthread_t tid;

	tc = 20;
	gctx = 0;
	rc = io_setup(128, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t20, (void *)NULL);

	while (state == 0)
		nanosleep(&delay, NULL);

	exit(0);
}

static void
t21()
{
	struct io_event events[NPAR];

	state = 1;
	(void) io_getevents(gctx, NPAR, NPAR, events, NULL);
}

/*
 * This is test21 and test22.
 *
 * Test 2 threads, thread B blocks in io_getevents waiting for events,
 * based flag, thread A then:
 *    0 - kills the process with a signal.
 *    1 - aborts
 */
static int
test21(void *a)
{
	int flag = (int)(unsigned long)a;
	int rc;
	pthread_t tid;

	if (flag == 0) {
		tc = 21;
	} else {
		tc = 22;
	}
	gctx = 0;
	rc = io_setup(NPAR, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t21, (void *)NULL);

	while (state == 0)
		nanosleep(&delay, NULL);

	if (flag == 0) {
		kill(getpid(), SIGKILL);
	} else {
		abort();
	}
	return (0);
}

/*
 * Timeout testing with getevents min_nr > submitted. Submit 1, wait for 4 with
 * a 0:0 timeout. This is like a poll and should return the 1 event.
 */
static int
test23(char *fname)
{
	aio_context_t ctx;
	int fd, rc, retries;
	struct iocb **ioq;
	struct timespec timeout = { 0, 0 };
	struct io_event events[NPAR];

	tc = 23;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	ioq[0] = mk_cb(fd, IOCB_CMD_PREAD, 0 * BLKSIZE, (long)0);

	rc = io_submit(ctx, 1, ioq);
	if (rc != 1)
		t_err("submit", rc, errno);

	for (retries = 0; retries < 5; retries++) {
		rc = io_getevents(ctx, NPAR, NPAR, events, &timeout);
		if (rc == 1)
			break;
		nanosleep(&delay, NULL);
		continue;
	}
	if (rc != 1)
		t_err("getevents", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Timeout testing with getevents min_nr > submitted. Nothing submitted, wait
 * for 4 with a 0:0 timeout. This is like a poll and should return 0 events.
 */
static int
test24(char *fname)
{
	aio_context_t ctx;
	int rc, retries;
	struct io_event events[NPAR];
	struct timespec timeout = { 0, 0 };

	tc = 24;
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	for (retries = 0; retries < 5; retries++) {
		rc = io_getevents(ctx, NPAR, NPAR, events, &timeout);
		if (rc != 0)
			t_err("getevents", rc, errno);
		nanosleep(&delay, NULL);
		continue;
	}
	if (rc != 0)
		t_err("getevents", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

/*
 * Timeout testing with getevents min_nr == 0. Nothing submitted, wait
 * with a NULL timeout. This is also like a poll and should return 0 events,
 * even though timeout is NULL
 */
static int
test25(char *fname)
{
	aio_context_t ctx;
	int rc;
	struct io_event events[NPAR];

	tc = 25;
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	rc = io_getevents(ctx, 0, NPAR, events, NULL);
	if (rc != 0)
		t_err("getevents", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

/*
 * Similar to test13, but use a socket fd, which should result in an error at
 * I/O time on the CB.
 */
static int
test26(char *fname)
{
	int i, fd0, fd1, rc;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event events[NPAR];
	struct iocb *io;
	pthread_t tid;

	tc = 26;
	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))tsrv, (void *)NULL);

	while (state == 0)
                nanosleep(&delay, NULL);

	if ((fd0 = open(fname, O_RDONLY)) < 0)
		t_err("open", fd0, errno);

	fd1 = setup_sock();

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < 2; i++) {
		if (i == 0) {
			io = mk_cb(fd0, IOCB_CMD_PREAD, BLKSIZE, 0L);
		} else {
			io = mk_cb(fd1, IOCB_CMD_PREAD, BLKSIZE, 0L);
		}
		ioq[i] = io;
	}

	rc = io_submit(ctx, 2, ioq);
	if (rc != 2)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 2, 2, events, NULL);
	if (rc != 2)
		t_err("getevents", rc, errno);

	for (i = 0; i < 2; i++) {
		struct io_event *ep = &events[i];
		struct iocb *iop;

		iop = (struct iocb *)ep->obj;
		if (iop->aio_fildes == fd1) {
			if (ep->res != -ESPIPE)
				tfail("unexpected res");
		} else {
			if (ep->res != BLKSIZE)
				tfail("unexpected res");
			free((void *)iop->aio_buf);
		}

		free(iop);
	}

	state = 2;
	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	pthread_join(tid, NULL);

	close(fd0);
	close(fd1);
	return (0);
}

static void
t27()
{
	int rc;
	struct io_event events[NPAR];
	struct timespec timeout = { 0, 10000 };

	state = 1;
	while (gztot != NPAR) {
		struct io_event *ep = &events[0];
		struct iocb *iop;

		rc = io_getevents(gctx, 1, 1, events, &timeout);
		if (rc < 0)
			t_err("getevents", rc, errno);
		if (rc == 1) {

			iop = (struct iocb *)ep->obj;
			free((void *)iop->aio_buf);
			free(iop);
			gztot++;
		}
                nanosleep(&delay, NULL);
	}
}

/*
 * Similar to test 17; test two threads getting multiple events off the same
 * context.
 */
static int
test27(char *fname)
{
	int fd, rc, i;
	struct iocb **ioq;
	struct io_event events[NPAR];
	struct timespec timeout = { 0, 10000 };
	pthread_t tid;

	tc = 27;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	gctx = 0;
	rc = io_setup(NPAR, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	gztot = 0;
	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t27, (void *)NULL);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < NPAR; i++) {
		ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
	}

	rc = io_submit(gctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	/* wait for thread to be running */
	while (state == 0)
		nanosleep(&delay, NULL);

	while (gztot != NPAR) {
		struct io_event *ep = &events[0];
		struct iocb *iop;

		rc = io_getevents(gctx, 1, 1, events, &timeout);
		if (rc < 0)
			t_err("getevents", rc, errno);
		if (rc == 1) {
			iop = (struct iocb *)ep->obj;
			free((void *)iop->aio_buf);
			free(iop);
			gztot++;
		}
                nanosleep(&delay, NULL);
	}

	pthread_join(tid, NULL);

	rc = io_destroy(gctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/*
 * Test the extremely broken behavior from libaio io_getevents() which assumes
 * it can reference through the context as if it were a pointer to a
 * well-defined structure in the process address space.
 */
static int
test28(char *fname)
{
	aio_context_t ctx;
	int rc, i;
	char *p;

	tc = 28;
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	p = (char *)ctx;
	for (i = 0; i < 32; p++, i++) {
		if (*p != '\0' && is_lx) {
			printf("unexpected ctx element (%d): 0x%x\n", i, *p);
		}
	}

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

static void
t29()
{
	int rc;
	struct io_event events[3 * NPAR];

	state = 1;
	while (gztot != 0) {
		int i;
		uint64_t u;

		if ((rc = read(evfd, &u, sizeof (u))) != sizeof (u))
			t_err("eventfd read", rc, errno);

		rc = io_getevents(gctx, 1, 3 * NPAR, events, NULL);
		if (rc < 0)
			t_err("getevents", rc, errno);

		if (rc < u)
			tfail("getevents did not get everything");

		for (i = 0; i < rc; i++) {
			struct io_event *ep = &events[i];
			struct iocb *iop;

			iop = (struct iocb *)ep->obj;
			if (ep->res != BLKSIZE)
				tfail("unexpected res");
			free((void *)iop->aio_buf);
			free(iop);
		}

		gztot -= rc;
	}
}

/*
 * Test eventfd notification with one thread waiting to get events based on
 * eventfd notification. Loop 3 times submitting with a short delay to test
 * re-reading eventfd.
 */
static int
test29(char *fname)
{
	int fd, rc, i, j;
	struct iocb **ioq;
	pthread_t tid;

	tc = 29;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((evfd = eventfd(0, 0)) < 0)
		t_err("eventfd", evfd, errno);

	gztot = 3 * NPAR;
	state = 0;
	pthread_create(&tid, NULL, (void *(*)(void *))t29, (void *)NULL);

	/* wait for thread to be running */
	while (state < 1)
                nanosleep(&delay, NULL);

	gctx = 0;
	rc = io_setup(3 * NPAR, &gctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (j = 0 ; j < 3; j++) {
		for (i = 0; i < NPAR; i++) {
			ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE,
			    (long)i);
			ioq[i]->aio_flags = IOCB_FLAG_RESFD;
			ioq[i]->aio_resfd = evfd;
		}

		rc = io_submit(gctx, NPAR, ioq);
		if (rc != NPAR)
			t_err("submit", rc, errno);

		/* let reader do some work */
		nanosleep(&delay, NULL);
	}

	pthread_join(tid, NULL);

	rc = io_destroy(gctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	close(evfd);
	return (0);
}

/*
 * Test eventfd notification with an invalid fd in one of the control blocks.
 */
static int
test30(char *fname)
{
	int fd, rc, i;
	struct iocb **ioq;
	aio_context_t ctx;

	tc = 30;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	if ((evfd = eventfd(0, 0)) < 0)
		t_err("eventfd", evfd, errno);

	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	for (i = 0; i < NPAR; i++) {
		ioq[i] = mk_cb(fd, IOCB_CMD_PREAD, i * BLKSIZE, (long)i);
		ioq[i]->aio_flags = IOCB_FLAG_RESFD;
		if (i == 2) {
			ioq[i]->aio_resfd = 0;
		} else {
			ioq[i]->aio_resfd = evfd;
		}
	}

	/* only 2 out of NPAR should be submitted since 3rd has bad aio_resfd */
	rc = io_submit(ctx, NPAR, ioq);
	if (rc != 2)
		t_err("submit", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	close(evfd);
	return (0);
}

/*
 * Test creating a large number of contexts up to our current max.
 */
static int
test31(char *fname)
{
	int rc, i;
	aio_context_t ctx, ctxa[512];

	tc = 31;

	for (i = 0; i < 512; i++) {
		ctxa[i] = 0;
		rc = io_setup(NPAR, &ctxa[i]);
		if (rc < 0)
			t_err("setup", rc, errno);
	}

	/* this should fail */
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc == 0 || errno != ENOMEM)
		tfail("io_setup of 513 context's succeeded");

	for (i = 0; i < 512; i++) {
		rc = io_destroy(ctxa[i]);
		if (rc != 0)
			t_err("destroy", rc, errno);
	}

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

	test1(tst_file);
	test2(tst_file, 2);
	test2(tst_file, 3);
	test2(tst_file, 4);
	test5(tst_file);
	test6(tst_file);
	test7(tst_file);
	if (is_lx)	/* fsync fails on Linux with EINVAL  */
		test8(tst_file);
	test9(tst_file);
	test10(tst_file);
	test11(tst_file);
	test12(tst_file);
	test13(tst_file);
	test14(tst_file);
	test15(tst_file);
	test16(tst_file);
	test17(tst_file);
	test18(tst_file);
	run_as_proc(19, test19, tst_file);
	run_as_proc(20, test20, tst_file);
	run_as_proc(21, test21, (void *)0);
	run_as_proc(22, test21, (void *)1);
	test23(tst_file);
	test24(tst_file);
	test25(tst_file);
	test26(tst_file);
	test27(tst_file);
	test28(tst_file);
	test29(tst_file);
	test30(tst_file);
	if (is_lx)	/* Linux can do more, but we don't care  */
		test31(tst_file);

	unlink(tst_file);
	return (test_pass("aio"));
}
