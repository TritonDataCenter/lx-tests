/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

#ifndef _LXTST_H
#define _LXTST_H

int test_pass(const char *);
int test_fail(const char *, const char *);
int test_skip(const char *, const char *);

#endif /* _LXTST_H */
