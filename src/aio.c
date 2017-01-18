#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
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
	return (syscall(SYS_io_cancel, cb, ep));
}

int
io_destroy(aio_context_t ctx)
{
	return (syscall(SYS_io_destroy, ctx));
}

/*
 * Test parallel writes with single blocking getevent consumer.
 * This must be the first test since it creates a test file the rest of the
 * test will depend on.
 */
int
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

	/* initialize */
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	/* Setup parallel reads */
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
		struct timespec timeout = { 0, 0 };
		struct io_event event, *ep = &event;
		struct iocb *iop;
		int n;

		rc = io_getevents(ctx, 0, 1, &event, &timeout);
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
 * Test parallel reads with single blocking getevent consumer.
 * tnum == 2; test 0:0 timeout
 * tnum == 3; test NULL timeout
 * tnum == 4; test 0:10000 timeout
 */
int
test2(char *fname, int tnum)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;

	tc = tnum;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	/* initialize */
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	/* Setup parallel reads */
	for (i = 0; i < NPAR; i++) {
		char *buf;
		struct iocb *io;

		buf = (char *)malloc(BLKSIZE);
		io = (struct iocb *)calloc(1, sizeof (struct iocb));
		if (buf == NULL || io == NULL)
			tfail("out of memory");

		io->aio_data = i;
		io->aio_key = 0;
		io->aio_lio_opcode = IOCB_CMD_PREAD;
		io->aio_reqprio = 0;
		io->aio_fildes = fd;
		io->aio_buf = (__u64)buf;
		io->aio_nbytes = BLKSIZE;
		io->aio_offset = i * BLKSIZE;
		io->aio_flags = 0;

		ioq[i] = io;
	}

	/* Submit parallel reads */
	rc = io_submit(ctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	if (tnum == 1) {
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
		rc = io_getevents(ctx, 0, 1, &event, tp);
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

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	close(fd);
	return (0);
}

/* Test parallel reads with blocking getevent consumer on all of them */
int
test5(char *fname)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;
	struct io_event events[NPAR];
	struct timespec timeout = { 0, 0 };

	tc = 5;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	/* initialize */
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	/* Setup parallel reads */
	for (i = 0; i < NPAR; i++) {
		char *buf;
		struct iocb *io;

		buf = (char *)malloc(BLKSIZE);
		io = (struct iocb *)calloc(1, sizeof (struct iocb));
		if (buf == NULL || io == NULL)
			tfail("out of memory");

		io->aio_data = i;
		io->aio_key = 0;
		io->aio_lio_opcode = IOCB_CMD_PREAD;
		io->aio_reqprio = 0;
		io->aio_fildes = fd;
		io->aio_buf = (__u64)buf;
		io->aio_nbytes = BLKSIZE;
		io->aio_offset = i * BLKSIZE;
		io->aio_flags = 0;

		ioq[i] = io;
	}

	/* Submit parallel reads */
	rc = io_submit(ctx, NPAR, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, NPAR, NPAR, events, &timeout);
	if (rc != NPAR)
		t_err("getevents", rc, errno);

	/* validate all IO's */
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
 * Test parallel read submition then cancel.
 */
int
test6(char *fname)
{
	int rc;
	aio_context_t ctx;
	struct iocb **ioq;
	char *buf;
	struct iocb *io;
	struct io_event event;

if (1) return (0);
	tc = 6;
	/* initialize */
	ctx = 0;
	rc = io_setup(1, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * 1);
	if (ioq == NULL)
		tfail("out of memory");

	buf = (char *)malloc(80);
	io = (struct iocb *)calloc(1, sizeof (struct iocb));
	if (buf == NULL || io == NULL)
		tfail("out of memory");

	io->aio_data = 0;
	io->aio_key = 0;
	io->aio_lio_opcode = IOCB_CMD_PREAD;
	io->aio_reqprio = 0;
	io->aio_fildes = 0;		/* XXX doesn't work (stdin read) */
	io->aio_buf = (__u64)buf;
	io->aio_nbytes = 80;
	io->aio_offset = 0;
	io->aio_flags = 0;

	ioq[0] = io;

	/* Submit parallel reads */
	rc = io_submit(ctx, 1, ioq);
	if (rc != 1)
		t_err("submit", rc, errno);

	/* cancel them */
	/* result arg is no longer used in Linux */
	rc = io_cancel(ctx, ioq[0], &event);
	if (rc != 0)
		t_err("cancel", rc, errno);

	rc = io_destroy(ctx);
	if (rc != 0)
		t_err("destroy", rc, errno);

	return (0);
}

/*
 * Test parallel read submition then destroy.
 */
int
test7(char *fname)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;

	tc = 7;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	/* initialize */
	ctx = 0;
	rc = io_setup(NPAR, &ctx);
	if (rc < 0)
		t_err("setup", rc, errno);

	ioq = (struct iocb **)malloc(sizeof (struct iocb *) * NPAR);
	if (ioq == NULL)
		tfail("out of memory");

	/* Setup parallel reads */
	for (i = 0; i < NPAR; i++) {
		char *buf;
		struct iocb *io;

		buf = (char *)malloc(BLKSIZE);
		io = (struct iocb *)calloc(1, sizeof (struct iocb));
		if (buf == NULL || io == NULL)
			tfail("out of memory");

		io->aio_data = i;
		io->aio_key = 0;
		io->aio_lio_opcode = IOCB_CMD_PREAD;
		io->aio_reqprio = 0;
		io->aio_fildes = fd;
		io->aio_buf = (__u64)buf;
		io->aio_nbytes = BLKSIZE;
		io->aio_offset = i * BLKSIZE;
		io->aio_flags = 0;

		ioq[i] = io;
	}

	/* Submit parallel reads */
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
int
test8(char *fname)
{
	int fd, rc, i;
	aio_context_t ctx;
	struct iocb **ioq;
	struct iocb *io;
	struct timespec timeout = { 0, 0 };
	struct io_event event, *ep = &event;
	struct iocb *iop;
	int n;

if (1) return (0);
	tc = 8;
	if ((fd = open(fname, O_RDONLY)) < 0)
		t_err("open", fd, errno);

	/* initialize */
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

	ioq[i] = io;

	/* Submit */
	rc = io_submit(ctx, 1, ioq);
	if (rc != NPAR)
		t_err("submit", rc, errno);

	rc = io_getevents(ctx, 0, 1, &event, &timeout);
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

int
main(int argc, char **argv)
{
	snprintf(tst_file, sizeof(tst_file), "lxtmp%d", getpid());

	test1(tst_file);
	test2(tst_file, 2);
	test2(tst_file, 3);
	test2(tst_file, 4);
	test5(tst_file);
	test6(tst_file);	/* XXX disabled */
	test7(tst_file);
	test8(tst_file);	/* XXX disabled */

	unlink(tst_file);
	return (test_pass("aio"));
}
