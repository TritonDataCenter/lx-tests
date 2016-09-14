#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

#
# Copyright 2016 Joyent, Inc.
#

PROG	= stkchk
OBJECTS	= stkchk.o subr.o parse.o
CFLAGS	= -std=c99 -D_GNU_SOURCE

$(PROG): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@

all: $(PROG)

test: all
	./$(PROG)

clean:
	rm -f $(PROG) $(OBJECTS)
.PHONY: clean test
