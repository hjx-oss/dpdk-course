#!/usr/bin/env python3
"""在第二集的隔离网络空间中，从 client0 发送三个实验帧。"""

import socket
import time


def main():
    destination_mac = bytes.fromhex("02 00 00 00 00 02")
    source_mac = bytes.fromhex("02 00 00 00 00 01")
    ether_type = bytes.fromhex("88 b5")

    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                       socket.htons(0x88B5)) as sender:
        sender.bind(("client0", 0))

        for packet_number in range(1, 4):
            message = f"hello-{packet_number}".encode("ascii")
            ethernet_frame = destination_mac + source_mac + ether_type + message

            # 补足 60 字节，延续第二集的实验帧内容；这里不包含 FCS。
            ethernet_frame = ethernet_frame.ljust(60, b"\x00")
            sent_bytes = sender.send(ethernet_frame)
            if sent_bytes != len(ethernet_frame):
                raise RuntimeError("Frame was not sent completely")

            print(f"SEND {packet_number}: {sent_bytes} bytes, {message.decode()}")
            time.sleep(0.2)

    print("Sent 3 frames. Check the receiver terminal for RX 1, RX 2 and RX 3.")


if __name__ == "__main__":
    main()
