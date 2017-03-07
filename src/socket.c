/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#define	__USE_GNU	1
#include <sys/types.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <stropts.h>
#include <sys/select.h>
#include <sys/un.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "lxtst.h"

static int
get_type(int fd)
{
	int type;
	socklen_t len = sizeof (type);

	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0) {
		printf("FAIL getsockopt\n");
		exit(1);
	}

	return ((int)type);
}

/*
 * Test for correct socket type with various calls to create the socket.
 */
static int
test1()
{
	int fds[2];

	if ((fds[0] = socket(AF_UNIX, SOCK_SEQPACKET, 0)) < 0)
		return (test_fail("socket 1a", "create SOCK_SEQPACKET"));
	if (get_type(fds[0]) != SOCK_SEQPACKET)
		return (test_fail("socket 1a", "type SOCK_SEQPACKET"));
	close(fds[0]);

	if ((fds[0] = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return (test_fail("socket 1b", "create SOCK_STREAM"));
	if (get_type(fds[0]) != SOCK_STREAM)
		return (test_fail("socket 1b", "type SOCK_STREAM"));
	close(fds[0]);

	if ((fds[0] = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
		return (test_fail("socket 1c", "create SOCK_DGRAM"));
	if (get_type(fds[0]) != SOCK_DGRAM)
		return (test_fail("socket 1c", "type SOCK_DGRAM"));
	close(fds[0]);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
		return (test_fail("socket 1d", "create SOCK_STREAM"));
	if (get_type(fds[0]) != SOCK_STREAM)
		return (test_fail("socket 1d", "type SOCK_STREAN"));
	if (get_type(fds[1]) != SOCK_STREAM)
		return (test_fail("socket 1d", "type SOCK_STREAN"));
	close(fds[0]);
	close(fds[1]);

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) < 0)
		return (test_fail("socket 1e", "create SOCK_DGRAM"));
	if (get_type(fds[0]) != SOCK_DGRAM)
		return (test_fail("socket 1e", "type SOCK_DGRAM"));
	if (get_type(fds[1]) != SOCK_DGRAM)
		return (test_fail("socket 1e", "type SOCK_DGRAM"));
	close(fds[0]);
	close(fds[1]);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) < 0)
		return (test_fail("socket 1f", "create SOCK_SEQPACKET"));
	if (get_type(fds[0]) != SOCK_SEQPACKET)
		return (test_fail("socket 1f", "type SOCK_SEQPACKET"));
	if (get_type(fds[1]) != SOCK_SEQPACKET)
		return (test_fail("socket 1f", "type SOCK_SEQPACKET"));
	close(fds[0]);
	close(fds[1]);

	return (0);
}

/* Test cmsg pid cred passing from children for AF_UNIX/SOCK_SEQPACKET socket */
static int
test2()
{
	int i;
	int stat;
	int enable = 1;
	int len;
	int fds[2];
	char ctl[256];
	char buf[80];
	char *p;
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr* cmsg;

        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) < 0)
		return (test_fail("socket 2", "socketpair"));

	if (setsockopt(fds[0], SOL_SOCKET, SO_PASSCRED, &enable,
	    sizeof (enable)) < 0)
		return (test_fail("socket 2", "setsockopt"));

	for (i = 0; i < 2; i++) {
		if (fork() == 0) {
			snprintf(buf, sizeof (buf), "child %d", getpid());
			iov.iov_base = buf;
			iov.iov_len = strlen(buf);

			msg.msg_name = NULL;
			msg.msg_namelen = 0;
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
			msg.msg_flags = 0;

			if (sendmsg(fds[1], &msg, 0) < 0)
		                exit(1);
			exit(0);
		}
	}

	/* parent */
	for (i = 0; i < 2; i++) {
		iov.iov_base = buf;
		iov.iov_len = sizeof (buf);

		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = &ctl;
		msg.msg_controllen = sizeof (ctl);
		msg.msg_flags = 0;
		bzero(ctl, sizeof (ctl));

		if ((len = recvmsg(fds[0], &msg, 0)) < 0)
			return (test_fail("socket 2", "recvmsg"));

		buf[len] = '\0';
		if (msg.msg_controllen == 0)
			return (test_fail("socket 2", "controllen == 0"));

		cmsg = CMSG_FIRSTHDR(&msg);
		if (cmsg == NULL)
			return (test_fail("socket 2", "no cmsg"));

		p = strchr(buf, ' ');
		if (p == NULL)
			return (test_fail("socket 2", "malformed data"));

		p++;
		if (atoi(p) != ((struct ucred *)CMSG_DATA(cmsg))->pid)
			return (test_fail("socket 2", "incorrect cmsg pid"));
	}

	close(fds[0]);
	close(fds[1]);

	for (i = 0; i < 2; i++) {
		wait(&stat);
		if (WEXITSTATUS(stat) != 0)
			return (test_fail("socket 2", "child error"));
	}

	return (0);
}

int
main(int argc, char **argv)
{
	/* Test 1 - socket types */
	if (test1() != 0)
		return (1);

	/* Test 2 - seqpacket cred passing */
	if (test2() != 0)
		return (1);

	return (test_pass("socket"));
}
