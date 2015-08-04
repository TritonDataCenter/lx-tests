#
# Copyright 2015 Joyent, Inc.
#

TARGETS = \
	uname

CPPFLAGS +=

CFLAGS += -Wall -Werror

LDFLAGS += util.o

all: util.o $(TARGETS)

util.o: util.c
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c util.c

test: all
	@r=0; for i in $(TARGETS); \
	do ./$$i; \
		if [ $$? -ne 0 ]; then r=1; fi; \
	done; \
	if [ $$r -eq 1 ]; then echo "Some tests failed"; fi

clean:
	rm -f *.o $(TARGETS)
