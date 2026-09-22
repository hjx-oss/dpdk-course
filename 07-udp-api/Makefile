CC ?= cc
CFLAGS := -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)
SOURCES := src/main.c src/arp.c src/udp.c src/ipv4.c src/icmp.c src/app.c
HEADERS := $(wildcard src/*.h)

all: build/udp_api

build/udp_api: $(SOURCES) $(HEADERS) Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) $(SOURCES) -o $@ $(DPDK_LIBS)

build/arp_state: tests/arp_state.c src/arp.c $(HEADERS) Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -Isrc tests/arp_state.c src/arp.c -o $@ $(DPDK_LIBS)

build/udp_state: tests/udp_state.c src/udp.c src/arp.c src/ipv4.c src/icmp.c $(HEADERS) Makefile
	mkdir -p build
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -Isrc tests/udp_state.c src/udp.c src/arp.c src/ipv4.c src/icmp.c -o $@ $(DPDK_LIBS)

check: all build/arp_state build/udp_state
	python3 tests/check.py

clean:
	rm -rf build

.PHONY: all check clean
