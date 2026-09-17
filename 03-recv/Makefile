CC ?= cc
CFLAGS := -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)

all: build/receive

build/receive: src/main.c Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) src/main.c -o $@ $(DPDK_LIBS)

check: all
	python3 tests/check.py

clean:
	rm -rf build

.PHONY: all check clean
