# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?= -W -Wall -fno-common -g -ggdb -std=gnu99 -O2
	SHOBJ_LDFLAGS ?= -shared
else
	SHOBJ_CFLAGS ?= -W -Wall -dynamic -fno-common -g -ggdb -std=gnu99 -O2
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup -macosx_version_min 10.12
endif

ifeq ($(USE_MONOTONIC_CLOCK),1)
	SHOBJ_CFLAGS += -DUSE_MONOTONIC_CLOCK
endif

.SUFFIXES: .c .so .xo .o

all: ratelimit.so

.c.xo:
	$(CC) -I. $(CFLAGS) $(SHOBJ_CFLAGS) -fPIC -c $< -o $@

ratelimit.xo: redismodule.h

ratelimit.so: ratelimit.xo
	$(LD) -o $@ $< $(SHOBJ_LDFLAGS) $(LIBS) -lc

clean:
	rm -rf *.xo *.so
