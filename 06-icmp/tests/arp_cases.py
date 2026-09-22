#!/usr/bin/env python3
"""ARP 应答字段、动态邻居学习和拒绝分支。"""
import json
import socket
import struct
import subprocess
import time

CLIENT = bytes.fromhex("020000000001")
SERVER = bytes.fromhex("020000000002")
CLIENT_IP = socket.inet_aton("192.0.2.1")
SERVER_IP = socket.inet_aton("192.0.2.2")


def neighbor():
    output = subprocess.check_output(["ip", "-j", "neigh", "show", "dev", "client0"], text=True)
    return [item for item in json.loads(output) if item["dst"] == "192.0.2.2"]


def request(*, hardware=1, protocol=0x0800, hlen=6, plen=4, opcode=1,
            target=SERVER_IP, source=CLIENT, ethernet_source=CLIENT,
            ethernet_target=b"\xff" * 6):
    arp = struct.pack("!HHBBH", hardware, protocol, hlen, plen, opcode)
    arp += source + CLIENT_IP + b"\0" * 6 + target
    return (ethernet_target + ethernet_source + b"\x08\x06" + arp).ljust(60, b"\0")


def receive(raw, timeout=.2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        raw.settimeout(max(.001, end - time.monotonic()))
        try:
            frame, address = raw.recvfrom(4096)
        except socket.timeout:
            return None
        if address[2] != socket.PACKET_OUTGOING and len(frame) >= 42:
            if frame[12:14] == b"\x08\x06" and frame[20:22] == b"\0\2":
                return frame
    return None


def verify(frame):
    assert frame is not None, "ARP reply missing"
    assert len(frame) == 60
    assert frame[:14] == CLIENT + SERVER + b"\x08\x06"
    assert struct.unpack("!HHBBH", frame[14:22]) == (1, 0x0800, 6, 4, 2)
    assert frame[22:42] == SERVER + SERVER_IP + CLIENT + CLIENT_IP
    assert frame[42:] == b"\0" * 18


def cases():
    results = []
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as raw:
        raw.bind(("client0", 0))
        for ethernet_target in [b"\xff" * 6, SERVER]:
            raw.send(request(ethernet_target=ethernet_target))
            verify(receive(raw, 3))
        # 较短的软件帧也应补齐尾部；旧填充不能原样反射出去。
        raw.send(request()[:42])
        verify(receive(raw, 3))
        raw.send(request()[:42] + b"x" * 40)
        verify(receive(raw, 3))
        results.append("PASS ARP reply: opcode, addresses and zero padding")
        invalid = {
            "truncated ARP": request()[:30],
            "other hardware": request(hardware=2),
            "other protocol": request(protocol=0x86dd),
            "wrong MAC length": request(hlen=5),
            "wrong IP length": request(plen=16),
            "ARP reply input": request(opcode=2),
            "other target IP": request(target=socket.inet_aton("192.0.2.3")),
            "other target MAC": request(ethernet_target=bytes.fromhex("020000000003")),
            "zero sender MAC": request(source=b"\0" * 6),
            "multicast sender": request(source=b"\xff" * 6),
            "sender MAC mismatch": request(ethernet_source=bytes.fromhex("020000000003")),
        }
        for name, frame in invalid.items():
            raw.send(frame)
            assert receive(raw) is None, name
            results.append("PASS ignore " + name)
        raw.send(request())
        verify(receive(raw, 3))
    return results
