#!/usr/bin/env python3
"""验证 ICMP 校验和、长度、标识符、序号和完整正文。"""
import socket
import struct
import time
from udp_cases import checksum
from arp_cases import CLIENT, SERVER, CLIENT_IP, SERVER_IP


def request(payload=b"icmp-data", *, kind=8, code=0, bad_checksum=False,
            protocol=1, fragment=0, ihl=0x45, destination=SERVER_IP,
            ip_length=None, bad_ip_checksum=False):
    message = struct.pack("!BBHHH", kind, code, 0, 0x1234, 7) + payload
    value = checksum(message) ^ (1 if bad_checksum else 0)
    message = message[:2] + struct.pack("!H", value) + message[4:]
    length = 20 + len(message) if ip_length is None else ip_length
    ip = struct.pack("!BBHHHBBH4s4s", ihl, 0, length, 9, fragment, 32,
                     protocol, 0, CLIENT_IP, destination)
    ip = ip[:10] + struct.pack("!H", checksum(ip) ^ (1 if bad_ip_checksum else 0)) + ip[12:]
    return (SERVER + CLIENT + b"\x08\x00" + ip + message).ljust(60, b"\0")


def receive(raw, timeout=.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        raw.settimeout(max(.001, end - time.monotonic()))
        try:
            frame, address = raw.recvfrom(4096)
        except socket.timeout:
            return None
        if address[2] != socket.PACKET_OUTGOING and len(frame) >= 42:
            if frame[12:14] == b"\x08\x00" and frame[23] == 1 and frame[26:30] == SERVER_IP:
                return frame
    return None


def verify(frame, payload):
    assert frame is not None, "ICMP reply missing"
    assert frame[:14] == CLIENT + SERVER + b"\x08\x00"
    ip = frame[14:34]
    size = struct.unpack("!H", ip[2:4])[0]
    assert size == 28 + len(payload)
    assert ip[12:20] == SERVER_IP + CLIENT_IP and ip[8] == 64
    assert checksum(ip) == 0
    message = frame[34:14 + size]
    assert message[:2] == b"\0\0"
    assert struct.unpack("!HH", message[4:8]) == (0x1234, 7)
    assert message[8:] == payload and checksum(message) == 0
    assert len(frame) == max(60, 14 + size)
    assert frame[14 + size:] == b"\0" * (len(frame) - 14 - size)


def cases():
    results = []
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as raw:
        raw.bind(("client0", 0))
        for payload in [b"", b"x", b"odd", bytes(range(255)), b"x" * 1472]:
            raw.send(request(payload))
            verify(receive(raw, 3), payload)
        raw.send(request(b"x")[:43] + b"z" * 30)
        verify(receive(raw, 3), b"x")
        results.append("PASS ICMP: empty, odd, binary, MTU and padding")
        results.append("PASS ICMP: identifier, sequence, payload and both checksums")
        invalid = {
            "Ethernet without IPv4": request()[:14],
            "truncated IP": request()[:30],
            "ICMP too short": request(ip_length=24),
            "Echo reply input": request(kind=0),
            "other ICMP type": request(kind=3),
            "nonzero ICMP code": request(code=1),
            "ICMP checksum": request(bad_checksum=True),
            "IP checksum": request(bad_ip_checksum=True),
            "other IPv4 protocol": request(protocol=6),
            "IPv4 fragment": request(fragment=0x2000),
            "IPv4 options": request(ihl=0x46),
            "other destination": request(destination=socket.inet_aton("192.0.2.3")),
            "IP length too small": request(ip_length=19),
            "IP length too large": request(ip_length=1500),
        }
        for name, frame in invalid.items():
            raw.send(frame)
            assert receive(raw) is None, name
            results.append("PASS ignore " + name)
        for _ in range(32):
            raw.send(request())
            verify(receive(raw, 3), b"icmp-data")
        results.append("PASS 32 ICMP replies after invalid input")
    return results
