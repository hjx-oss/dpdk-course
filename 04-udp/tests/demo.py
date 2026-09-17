#!/usr/bin/env python3
"""普通 UDP socket 发送三条消息，自动核对 DPDK 回包。"""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

LESSON = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LESSON.parent / "scripts"))
import lab


def arguments():
    lab.require_isolation()
    cpu = min(os.sched_getaffinity(0))
    return [str(LESSON / "build/udp_echo"), "-l", str(cpu), "-m", "128",
            "--no-pci", "--no-huge", "--no-shconf", "--no-telemetry",
            "--vdev=net_af_packet0,iface=dpdk0,qpairs=1"]


def wait_ready(process, log):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if "Ready:" in log.read_text(errors="replace"):
            return
        if process.poll() is not None:
            break
        time.sleep(.02)
    raise RuntimeError("UDP server did not start:\n" + log.read_text())


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()  # 只清理本脚本创建且尚未退出的子进程。
            process.wait()
            raise RuntimeError("Our UDP server did not exit")


def exchange(payloads):
    lines = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.bind(("192.0.2.1", 40000))
        client.settimeout(3)
        for payload in payloads:
            client.sendto(payload, ("192.0.2.2", 9000))
            returned, address = client.recvfrom(2048)
            assert address == ("192.0.2.2", 9000) and returned == payload
            text = payload.decode("ascii") if payload and all(32 <= b < 127 for b in payload) and len(payload) < 40 else f"{len(payload)} bytes"
            lines.append(f"PASS {text}: reply matches")
    return lines


def run(checker):
    lab.require_isolation()
    build = LESSON / "build"
    build.mkdir(exist_ok=True)
    log = build / "server.log"
    with tempfile.TemporaryDirectory(dir=build, prefix="runtime-") as runtime:
        environment = dict(os.environ, RUNTIME_DIRECTORY=runtime)
        with log.open("w") as output:
            process = subprocess.Popen(arguments(), stdout=output, stderr=output, env=environment)
            try:
                wait_ready(process, log)
                lines = checker()
            finally:
                stop(process)
        assert process.returncode == 0, log.read_text()
    result = next(line for line in log.read_text().splitlines() if line.startswith("RESULT "))
    assert "mbufs_in_use=0" in result, result
    print("\n".join(lines))
    print(result)
    (build / "demo.txt").write_text("\n".join(lines + [result]) + "\n")
    return lines, result


if __name__ == "__main__":
    if "--inside" not in sys.argv:
        lab.enter([sys.executable, str(Path(__file__).resolve()), "--inside"], static_neighbor=True)
    run(lambda: exchange([b"hello-1", b"hello-2", b"hello-3"]))
