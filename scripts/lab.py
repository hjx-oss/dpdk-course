#!/usr/bin/env python3
"""只在本脚本新建的 user/network namespace 中建立实验网口。"""
import os
from pathlib import Path
import subprocess
import sys


def require_isolation():
    parent = os.environ.get("DPDK_COURSE_PARENT_NETNS")
    if not parent or os.readlink("/proc/self/ns/net") == parent:
        raise RuntimeError("请通过 scripts/lab.py 创建独立实验网络")


def enter(command, static_neighbor=False):
    environment = dict(os.environ,
                       DPDK_COURSE_PARENT_NETNS=os.readlink("/proc/self/ns/net"))
    args = ["unshare", "--user", "--map-root-user", "--net", sys.executable,
            str(Path(__file__).resolve()), "--inside"]
    if static_neighbor:
        args.append("--static-neighbor")
    os.execvpe(args[0], args + ["--"] + command, environment)


def setup(static_neighbor):
    require_isolation()
    commands = [
        ["sysctl", "-w", "net.ipv6.conf.default.disable_ipv6=1"],
        ["ip", "link", "add", "client0", "type", "veth", "peer", "name", "dpdk0"],
        ["ip", "link", "set", "lo", "up"],
        ["ip", "link", "set", "client0", "address", "02:00:00:00:00:01", "up"],
        ["ip", "link", "set", "dpdk0", "address", "02:00:00:00:00:02", "up"],
        ["ip", "addr", "add", "192.0.2.1/24", "dev", "client0"],
        ["ethtool", "-K", "client0", "tx", "off", "gso", "off", "gro", "off"],
    ]
    if static_neighbor:
        commands.append(["ip", "neigh", "add", "192.0.2.2", "lladdr",
                         "02:00:00:00:00:02", "nud", "permanent", "dev", "client0"])
    for command in commands:
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    # 192.0.2.2 属于我们的 C 程序，不给 dpdk0 配置这个 IP。


if __name__ == "__main__":
    args = sys.argv[1:]
    inside = "--inside" in args
    static = "--static-neighbor" in args
    command = args[args.index("--") + 1:] if "--" in args else ["bash", "--noprofile", "--norc"]
    if not inside:
        enter(command, static)
    setup(static)
    print("veth: client0 192.0.2.1 <-> dpdk0 / UDP server 192.0.2.2", flush=True)
    os.execvp(command[0], command)
