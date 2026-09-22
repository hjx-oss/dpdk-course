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
import arp_cases


def arguments():
    lab.require_isolation()
    cpu = min(os.sched_getaffinity(0))
    return [str(LESSON / "build/udp_api"), "-l", str(cpu), "-m", "128",
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
    active = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    active.bind(("192.0.2.1", 40001))
    active.settimeout(5)
    with tempfile.TemporaryDirectory(dir=build, prefix="runtime-") as runtime:
        environment = dict(os.environ, RUNTIME_DIRECTORY=runtime)
        with log.open("w") as output:
            process = subprocess.Popen(arguments(), stdout=output, stderr=output, env=environment)
            try:
                wait_ready(process, log)
                greeting, peer = active.recvfrom(2048)
                assert greeting == b"hello-from-dpdk" and peer == ("192.0.2.2", 9002)
                active.sendto(b"ack-from-linux", peer)
                active.close()
                lines = ["PASS active UDP: 9002 -> 192.0.2.1:40001 hello-from-dpdk"]
                lines += checker()
            finally:
                stop(process)
                active.close()
        assert process.returncode == 0, log.read_text()
    result = next(line for line in log.read_text().splitlines() if line.startswith("RESULT "))
    assert "mbufs_in_use=0" in result, result
    print("\n".join(lines))
    print(result)
    (build / "demo.txt").write_text("\n".join(lines + [result]) + "\n")
    return lines, result


def demonstrate():
    # 主动发包实验可能已通过 ARP 学到邻居。
    result = subprocess.run(["ping", "-n", "-c", "3", "-i", "0.2", "-W", "2", "192.0.2.2"],
                            text=True, capture_output=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    (LESSON / "build/ping.txt").write_text(result.stdout)
    lines = ["PASS ping: 3 Echo requests, 3 Echo replies"]
    lines += exchange([b"hello-after-ping"])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
        other.settimeout(3)
        other.sendto(b"hello DPDK 123!", ("192.0.2.2", 9001))
        reply, peer = other.recvfrom(2048)
        assert reply == b"HELLO DPDK 123!" and peer == ("192.0.2.2", 9001)
        lines.append("PASS port 9001: hello DPDK 123! -> HELLO DPDK 123!")
    lines.append("PASS port 9000: payload echoed unchanged")
    entries = arp_cases.neighbor()
    assert entries and entries[0]["lladdr"] == "02:00:00:00:00:02"
    assert "PERMANENT" not in entries[0].get("state", [])
    lines.append("PASS ARP neighbor learned without static entry")
    return lines


if __name__ == "__main__":
    if "--inside" not in sys.argv:
        lab.enter([sys.executable, str(Path(__file__).resolve()), "--inside"], static_neighbor=False)
    run(demonstrate)
