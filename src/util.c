/*
 * Copyright 2015 Joyent, Inc.
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
