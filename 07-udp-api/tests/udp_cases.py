#!/usr/bin/env python3
"""真实 veth 收发：正常 UDP、边界报文、无回复分支和资源回收。"""
import json
from pathlib import Path
import socket
import struct
import sys
import time
import demo


def checksum(data):
    data += b"\0" * (len(data) % 2)
    value = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while value >> 16:
        value = (value & 0xffff) + (value >> 16)
    return (~value) & 0xffff


def frame(payload=b"raw-udp", *, destination="192.0.2.2", port=9000,
          fragment=0, ihl=0x45, udp_length=None, ip_length=None,
          bad_ip_checksum=False, bad_udp_checksum=False, zero_checksum=False):
    src, dst = socket.inet_aton("192.0.2.1"), socket.inet_aton(destination)
    length = len(payload) + 8
    udp = struct.pack("!HHHH", 40001, port, length if udp_length is None else udp_length, 0) + payload
    pseudo = src + dst + struct.pack("!BBH", 0, 17, len(udp))
    value = checksum(pseudo + udp) or 0xffff
    value = 0 if zero_checksum else value ^ (1 if bad_udp_checksum else 0)
    udp = udp[:6] + struct.pack("!H", value) + udp[8:]
    ip = struct.pack("!BBHHHBBH4s4s", ihl, 0, 20 + length if ip_length is None else ip_length,
                     1, fragment, 64, 17, 0, src, dst)
    ip = ip[:10] + struct.pack("!H", checksum(ip) ^ (1 if bad_ip_checksum else 0)) + ip[12:]
    ethernet = bytes.fromhex("020000000002 020000000001 0800")
    return (ethernet + ip + udp).ljust(60, b"\0")


def receive(sock, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sock.settimeout(max(.001, deadline - time.monotonic()))
        try:
            packet, address = sock.recvfrom(4096)
        except socket.timeout:
            return None
        if address[2] == socket.PACKET_OUTGOING or len(packet) < 42:
            continue
        if packet[12:14] == b"\x08\x00" and packet[26:30] == socket.inet_aton("192.0.2.2") and packet[34:36] == struct.pack("!H", 9000):
            return packet
    return None


def verify_reply(packet, payload):
    assert packet is not None, "No reply"
    assert packet[:12] == bytes.fromhex("020000000001 020000000002")
    ip = packet[14:34]
    length = struct.unpack("!H", ip[2:4])[0]
    assert checksum(ip) == 0
    assert ip[12:20] == socket.inet_aton("192.0.2.2") + socket.inet_aton("192.0.2.1")
    udp = packet[34:14 + length]
    assert struct.unpack("!HH", udp[:4]) == (9000, 40001)
    assert udp[8:] == payload
    pseudo = ip[12:20] + struct.pack("!BBH", 0, 17, len(udp))
    assert checksum(pseudo + udp) == 0


def cases():
    messages = [b"hello-1", b"hello-2", b"hello-3", b"", b"\x00\xff\x80A", b"x" * 1472]
    results = demo.exchange(messages)
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as raw:
        raw.bind(("client0", 0))
        for zero in [False, True]:
            raw.send(frame(zero_checksum=zero))
            verify_reply(receive(raw, 3), b"raw-udp")
        results.append("PASS source/destination fields and IPv4/UDP reply checksums")
        invalid = {
            "Ethernet header without IPv4 data": frame()[:14],
            "truncated IPv4": frame()[:30],
            "other EtherType": frame()[:12] + b"\x88\xb5" + frame()[14:],
            "other IP": frame(destination="192.0.2.3"),
            "other UDP port": frame(port=9999),
            "IPv4 fragment": frame(fragment=0x2000),
            "IPv4 options": frame(ihl=0x46),
            "short IP length": frame(ip_length=21),
            "IP length beyond received bytes": frame(ip_length=1500),
            "invalid UDP length": frame(udp_length=7),
            "inconsistent UDP length": frame(udp_length=9),
            "bad IPv4 checksum": frame(bad_ip_checksum=True),
            "bad UDP checksum": frame(bad_udp_checksum=True),
        }
        for name, packet in invalid.items():
            raw.send(packet)
            assert receive(raw, .15) is None, f"Unexpected reply: {name}"
            results.append("PASS ignore " + name)
        for n in range(64):
            payload = f"batch-{n}".encode()
            raw.send(frame(payload))
            verify_reply(receive(raw, 3), payload)
        results.append("PASS 64 repeated replies after invalid input")
    return results

