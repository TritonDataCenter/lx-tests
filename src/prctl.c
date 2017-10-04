/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

#include <sys/prctl.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "lxtst.h"

#define	TST_NAME	"prctl"

static void
tfail(char *fmt, ...)
{
	va_list ap;

	printf("FAIL %s: ", TST_NAME);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	fputc('\n', stdout);

	exit(1);
}

static void
terr(const char *fmt, ...)
{
	int rc = errno;
	va_list ap;

	printf("FAIL %s: ", TST_NAME);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	printf(": %s (%d)\n", strerror(rc), rc);
	exit(1);
}

#define	LX_THRNAME_LEN	16
static void
get_set_name(void)
{
	const char *name = "foo";

	char buf[LX_THRNAME_LEN] = { 0 };
	int rc;

	rc = prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);
	if (rc != 0) {
		terr("prctl(PR_SET_NAME, \"%s\", 0, 0, 0) = %d", name, rc);
	}

	rc = prctl(PR_GET_NAME, (unsigned long)buf, 0, 0, 0);
	if (rc != 0) {
		terr("prctl(PR_GET_NAME, buf, 0, 0, 0) = %d", rc);
	}

	if (strcmp(buf, name) != 0) {
		tfail("thread name (\"%s\") does not match what was set "
		    "(\"%s\")", buf, name);
	}

	test_pass(TST_NAME ": get_set_name");
}

int
main(int argc, char **argv)
{
	get_set_name();

	return (0);
}
