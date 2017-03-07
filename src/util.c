/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */

#include <stdio.h>
#include "lxtst.h"

int
test_pass(char *name)
{
	printf("PASS %s\n", name);
	return (0);
}

int
test_fail(char *name, char *msg)
{
	printf("FAIL %s: %s\n", name, msg);
	return (1);
}

int
test_skip(char *name, char *why)
{
	printf("SKIP %s: %s\n", name, why);
	return (0);
}

