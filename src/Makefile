#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

#
# Copyright 2016 Joyent, Inc.
#

TESTS = \
	aio \
	clone \
	futex \
	mount_nfs \
	mount_tmpfs \
	procfs \
	sched \
	socket \
	uname \
	vdisk

SUBDIRS = vdso

COMMON_OBJS = util.o

CFLAGS += -Wall -Werror

aio: LDFLAGS += -lpthread
sched: LDFLAGS += -lpthread
clone: LDFLAGS += -lpthread
futex: LDFLAGS += -lpthread

all: $(TESTS) $(SUBDIRS)
	@for d in $(SUBDIRS); do $(MAKE) -C $$d all; done

$(TESTS): %: %.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $< $(COMMON_OBJS) -o $@ $(LDFLAGS)

test: $(TESTS)
	@for d in $(SUBDIRS); do $(MAKE) -C $$d test; done
	@r=0; for i in $(TESTS); \
	do ./$$i; \
		if [ $$? -ne 0 ]; then r=1; fi; \
	done; \
	if [ $$r -eq 1 ]; then echo "Some tests failed"; fi

clean:
	-for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done
	rm -f $(TESTS) $(COMMON_OBJS)

.PHONY: test clean
