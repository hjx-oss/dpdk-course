#!/usr/bin/env python3
"""在独立 veth 中观察主动 ARP：真实应答、广播字段和定时重试。"""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import demo


def experiment(target, present):
    demo.lab.require_isolation()
    log = demo.LESSON / ('build/active-query.log' if present else 'build/active-missing.log')
    frames = []
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as raw:
        raw.bind(('client0', 0))
        raw.settimeout(.05)
        with tempfile.TemporaryDirectory(dir=demo.LESSON / 'build', prefix='runtime-') as runtime:
            with log.open('w') as output:
                process = subprocess.Popen(demo.arguments() + ['--', '--arp-query', target],
                    stdout=output, stderr=output, env=dict(os.environ, RUNTIME_DIRECTORY=runtime))
                try:
                    deadline = time.monotonic() + 12
                    while process.poll() is None:
                        assert time.monotonic() < deadline, 'ARP query timed out'
                        try:
                            frame, address = raw.recvfrom(4096)
                        except socket.timeout:
                            continue
                        if address[2] != socket.PACKET_OUTGOING and len(frame) >= 42:
                            if frame[12:14] == b'\x08\x06' and frame[20:22] == b'\0\1':
                                if frame[38:42] == socket.inet_aton(target):
                                    frames.append((time.monotonic(), frame))
                    process.wait(timeout=2)
                finally:
                    demo.stop(process)
    text = log.read_text()
    assert process.returncode == (0 if present else 1), text
    assert len(frames) == (1 if present else 3), (len(frames), text)
    for _, frame in frames:
        assert len(frame) == 60 and frame[:6] == b'\xff' * 6
        assert frame[6:12] == bytes.fromhex('020000000002')
        assert struct.unpack('!HHBBH', frame[14:22]) == (1, 0x800, 6, 4, 1)
        assert frame[22:32] == bytes.fromhex('020000000002') + socket.inet_aton('192.0.2.2')
        assert frame[32:38] == b'\0' * 6 and frame[42:] == b'\0' * 18
    if present:
        assert 'NEIGHBOR 192.0.2.1 -> 02:00:00:00:00:01' in text, text
        line = 'PASS active query: broadcast request, Linux reply, MAC learned'
    else:
        intervals = [frames[i + 1][0] - frames[i][0] for i in range(2)]
        assert all(.8 <= delta <= 4 for delta in intervals), intervals
        assert 'NEIGHBOR_FAILED 192.0.2.99' in text, text
        line = 'PASS missing peer: three requests at one-second intervals, then failed'
    result = next(line for line in text.splitlines() if line.startswith('RESULT '))
    assert 'mbufs_in_use=0' in result, result
    display = [line] + [s for s in text.splitlines() if s.startswith(('NEIGHBOR', 'RESULT '))]
    print('\n'.join(display))
    return display


def checks():
    lines = experiment('192.0.2.1', True) + experiment('192.0.2.99', False)
    (demo.LESSON / 'build/active.txt').write_text('\n'.join(lines) + '\n')
    return lines


if __name__ == '__main__':
    if '--inside' not in sys.argv:
        demo.lab.enter([sys.executable, str(Path(__file__).resolve()), '--inside'])
    checks()
