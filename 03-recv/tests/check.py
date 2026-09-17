#!/usr/bin/env python3
"""自动验证本版接收程序；veth 只创建在本次新建的网络空间内。"""

import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


LESSON = Path(__file__).resolve().parents[1]
BUILD = LESSON / "build"
RESULTS = BUILD / "results.json"


def run_command(arguments):
    return subprocess.run(arguments, check=True, text=True, capture_output=True)


def wait_for(log_path, text, process):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        output = log_path.read_text(errors="replace")
        if text in output:
            return output
        if process.poll() is not None:
            raise RuntimeError(f"Receiver exited before {text!r}:\n{output}")
        time.sleep(0.02)
    raise RuntimeError(f"Timed out waiting for {text!r}")


def inside_namespace():
    parent_namespace = os.environ["DPDK_CHECK_PARENT_NETNS"]
    if os.readlink("/proc/self/ns/net") == parent_namespace:
        raise RuntimeError("A new network namespace is required")

    run_command(["sysctl", "-w", "net.ipv6.conf.default.disable_ipv6=1"])
    run_command(["ip", "link", "add", "client0", "type", "veth",
                 "peer", "name", "dpdk0"])
    run_command(["ip", "link", "set", "client0", "address",
                 "02:00:00:00:00:01", "up"])
    run_command(["ip", "link", "set", "dpdk0", "address",
                 "02:00:00:00:00:02", "up"])

    cpu = min(os.sched_getaffinity(0))
    command = [str(BUILD / "receive"), "-l", str(cpu), "-m", "128",
               "--no-pci", "--no-huge", "--no-shconf", "--no-telemetry",
               "--vdev=net_af_packet0,iface=dpdk0"]
    cases = []

    with tempfile.TemporaryDirectory(prefix="dpdk-receive-check-", dir=BUILD) as runtime:
        environment = dict(os.environ, RUNTIME_DIRECTORY=runtime)
        log_path = BUILD / "receive.log"

        with log_path.open("w") as log:
            receiver = subprocess.Popen(command, stdout=log, stderr=log,
                                        env=environment)
            try:
                wait_for(log_path, "Ready:", receiver)

                with socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                   socket.htons(0x0003)) as sender:
                    sender.bind(("client0", 0))
                    sender.settimeout(0.2)

                    sent = run_command([sys.executable, str(LESSON / "tests/send_packets.py")])
                    (BUILD / "send.log").write_text(sent.stdout)
                    output = wait_for(log_path, "text: hello-3", receiver)
                    for number in range(1, 4):
                        assert f"RX {number}: bytes=60, EtherType=0x88b5" in output
                        assert f"text: hello-{number}" in output
                    cases.append("three hello frames received with matching contents")

                    # 只收包的程序不应回包；忽略客户端自己的发送副本。
                    try:
                        while True:
                            returned_frame, address = sender.recvfrom(2048)
                            if address[2] != socket.PACKET_OUTGOING:
                                raise AssertionError("Unexpected returned frame")
                    except socket.timeout:
                        pass
                    cases.append("receive-only program sends no reply")

                    header = bytes.fromhex("020000000002 020000000001 88b5")
                    # veth 可以递送未补齐最小以太网帧长的数据，用来检查短载荷。
                    for frame in [header, header + b"A\x00\xffB", header + b"x" * 128]:
                        assert sender.send(frame) == len(frame)
                    output = wait_for(log_path, "RX 6:", receiver)
                    wait_for(log_path, "text: xxxxxxxxxxxxxxxx", receiver)
                    output = log_path.read_text()
                    assert "No bytes after Ethernet header." in output
                    assert "hex : 41 00 ff 42" in output
                    assert "text: A..B" in output
                    cases.append("empty, binary and truncated display preview")

                    # 重复收包跨过多轮 burst；不等待回复，也不更改程序的运行循环。
                    for group in range(10):
                        for number in range(32):
                            frame = (header + f"batch-{group}-{number}".encode()).ljust(60, b"\x00")
                            assert sender.send(frame) == len(frame)
                        expected_count = 6 + (group + 1) * 32
                        wait_for(log_path, f"RX {expected_count}:", receiver)
                    cases.append("320 additional frames received")

                receiver.send_signal(signal.SIGINT)
                assert receiver.wait(timeout=10) == 0
                output = log_path.read_text()
                assert "Stopped. Received 326 packets." in output
                assert "Mbufs still in use: 0" in output
                cases.append("Ctrl+C exits cleanly and all mbufs are returned")
            finally:
                if receiver.poll() is None:
                    receiver.terminate()
                    try:
                        receiver.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        receiver.kill()
                        receiver.wait()

        # 空闲退出和重新打开同一个实验端口，验证资源确实已释放。
        idle_log = BUILD / "idle.log"
        with idle_log.open("w") as log:
            receiver = subprocess.Popen(command, stdout=log, stderr=log,
                                        env=environment)
            try:
                wait_for(idle_log, "Ready:", receiver)
                receiver.send_signal(signal.SIGINT)
                assert receiver.wait(timeout=10) == 0
                output = idle_log.read_text()
                assert "Received 0 packets." in output
                assert "Mbufs still in use: 0" in output
                cases.append("idle exit and reopening the port")
            finally:
                if receiver.poll() is None:
                    receiver.kill()
                    receiver.wait()

        missing_port = subprocess.run(command[:-1], env=environment,
                                      text=True, capture_output=True, timeout=10)
        assert missing_port.returncode != 0
        assert "Port net_af_packet0 not found" in missing_port.stderr
        cases.append("missing port produces a specific error")

    RESULTS.write_text(json.dumps({"status": "passed", "dpdk": "25.11.3",
                                   "received": 326, "mbufs_in_use": 0,
                                   "cases": cases}, ensure_ascii=False, indent=2) + "\n")
    for case in cases:
        print(f"PASS: {case}")


def main():
    if "--inside" in sys.argv:
        inside_namespace()
        return

    RESULTS.unlink(missing_ok=True)
    environment = dict(os.environ)
    environment["DPDK_CHECK_PARENT_NETNS"] = os.readlink("/proc/self/ns/net")
    subprocess.run(["unshare", "--user", "--map-root-user", "--net",
                    sys.executable, str(Path(__file__).resolve()), "--inside"],
                   check=True, env=environment)
    print("PASS: isolated veth test completed")


if __name__ == "__main__":
    main()
