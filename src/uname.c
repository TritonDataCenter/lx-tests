/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2015, Joyent, Inc.
 */

#include <sys/utsname.h>
#include <string.h>
#include "lxtst.h"

int
main(int argc, char **argv)
{
	struct utsname buf;
	int res;

	res = uname(&buf);
	if (res != 0 || strcmp(buf.version, "BrandZ virtual linux") != 0) {
		return (test_fail("uname", "incorrect version"));
	}

	return (test_pass("uname"));
}
