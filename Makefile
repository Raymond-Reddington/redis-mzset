# Build the mzset Redis module.
#
# Linux:   make
# macOS:   make macos
#
# Requires: gcc/clang, redismodule.h (bundled).
# Redis Modules do NOT build on native Windows -- use WSL, Docker, or a Linux VM.

CC       ?= gcc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -fPIC
LDFLAGS  ?= -shared
TARGET   := mzset.so

SRCS := encoding.c hashtable.c skiplist.c mzset.c module.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean test macos

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c mzset.h redismodule.h
	$(CC) $(CFLAGS) -c $< -o $@

macos: LDFLAGS := -bundle -undefined dynamic_lookup
macos: all

test: $(TARGET)
	@bash tests/test.sh

clean:
	rm -f $(OBJS) $(TARGET) tests/test_encoding
