CC ?= cc
CFLAGS := -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)
SOURCES := src/main.c src/arp.c src/udp.c src/ipv4.c src/icmp.c
HEADERS := $(wildcard src/*.h)

all: build/icmp_udp

build/icmp_udp: $(SOURCES) $(HEADERS) Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) $(SOURCES) -o $@ $(DPDK_LIBS)

build/arp_state: tests/arp_state.c src/arp.c $(HEADERS) Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -Isrc tests/arp_state.c src/arp.c -o $@ $(DPDK_LIBS)

check: all build/arp_state
	python3 tests/check.py

clean:
	rm -rf build

.PHONY: all check clean
