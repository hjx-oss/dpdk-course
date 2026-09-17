# 第四集：从 main 开始搭建用户态协议栈

本集实现一个 UDP 回显程序。它通过 DPDK 收到以太网帧，检查 IPv4 和 UDP 头，把合法请求改成回复，再从同一网口发回。

## 三个源码文件

| 文件 | 做什么 |
|---|---|
| [src/main.c](src/main.c) | EAL、包池、一个 RX 和一个 TX 队列、收发循环、退出 |
| [src/udp.h](src/udp.h) | 声明 `udp_reply()`，说明 mbuf 所有权 |
| [src/udp.c](src/udp.c) | 检查头部和长度、交换地址和端口、重新计算校验和 |

第三集的收包循环现在多了两件事：调用 `udp_reply()`，再把可回复的包交给 `rte_eth_tx_burst()`。本集用一个工作核把这条路径跑通；后续再增加多核、队列交接和其他协议模块。

## 运行前准备

需要 Linux、DPDK 25.11.3，以及允许普通用户创建 user/network namespace 的环境。首次使用先按[仓库首页](../README.md#2-配置环境)安装依赖。下面所有命令都在克隆后的**仓库根目录**执行，不要求仓库放在某个固定路径。

## 1. 一键运行

```bash
bash 04-udp/run.sh
```

脚本会加载 `env.sh`、编译 `build/udp_echo`，然后建立隔离网络、启动 UDP 服务端，用普通 UDP socket 发送三条消息，核对回复，最后停止服务端。成功时输出：

```text
PASS hello-1: reply matches
PASS hello-2: reply matches
PASS hello-3: reply matches
RESULT received=3 sent=3 ignored=0 unsent=0 mbufs_in_use=0
```

这是一次自动演示，检查结束就退出，不是常驻服务。`run.sh` 无需附加参数，也没有用于配置 CPU、IP 或端口的参数接口。它内部如何选择启动参数，可看 [tests/demo.py 的 arguments()](tests/demo.py)。

## 2. 手动启动、发包和停止

### 编译并进入实验网络

```bash
source ./env.sh
make -C 04-udp
python3 scripts/lab.py --static-neighbor
```

第三条命令会打开一个新的 Bash，工作目录不变。**后续启动和发包都在这个 Bash 中执行**；另开普通终端不会自动进入同一个网络空间。

`lab.py` 创建一对 veth：客户端是 `client0`，程序通过 AF_PACKET 驱动连接 `dpdk0`。客户端 IP 为 `192.0.2.1`；`192.0.2.2:9000` 由我们的 C 程序处理，`192.0.2.2` 不配置给 Linux 的 `dpdk0`。

`--static-neighbor` 是 **lab.py 的选项**：为客户端添加 `192.0.2.2 → 02:00:00:00:00:02` 的静态邻居项。本集还没有 ARP 模块，客户端必须先知道目标 MAC，才能把 UDP 请求发过来。

### 启动服务端

先选取一个当前进程允许使用的 CPU，并把 DPDK 运行时文件放到本项目的构建目录中：

```bash
DPDK_CPU=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
export RUNTIME_DIRECTORY="$(mktemp -d "$PWD/04-udp/build/runtime.XXXXXX")"
```

然后启动程序：

```bash
./04-udp/build/udp_echo \
  -l "$DPDK_CPU" \
  -m 128 \
  --no-pci \
  --no-huge \
  --no-shconf \
  --no-telemetry \
  --vdev=net_af_packet0,iface=dpdk0,qpairs=1 \
  > 04-udp/build/manual-server.log 2>&1 &
udp_pid=$!
trap 'kill -INT "$udp_pid" 2>/dev/null; wait "$udp_pid" 2>/dev/null' EXIT
```

末尾的 `&` 让程序在后台运行，这样同一终端还能发送测试消息；`udp_pid` 保存这次启动的进程号，退出这个 Bash 时也会通知它停止。日志重定向和这些 Shell 语句都不是 DPDK 参数。

```bash
cat 04-udp/build/manual-server.log
```

看到下面这行再发包；若尚未出现，稍后再次查看日志：

```text
Ready: UDP 192.0.2.2:9000 on dpdk0. Press Ctrl+C to stop.
```

本例在后台运行，停止方式见下方；如果去掉重定向和 `&`，程序在前台运行，才直接用 Ctrl+C 停止。

### 发一条 UDP 消息

仍在同一个实验 Bash 中执行：

```bash
python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
    client.bind(("192.0.2.1", 40000))
    client.settimeout(3)
    client.sendto(b"hello-dpdk", ("192.0.2.2", 9000))
    data, address = client.recvfrom(2048)
    print("reply:", data, "from:", address)
    assert data == b"hello-dpdk"
    assert address == ("192.0.2.2", 9000)
PY
```

成功时输出 `reply: b'hello-dpdk' from: ('192.0.2.2', 9000)`。这说明消息经过 veth 到达 DPDK 程序，并由 UDP 模块构造回复返回客户端。

### 停止服务端并退出实验网络

```bash
kill -INT "$udp_pid"
wait "$udp_pid"
trap - EXIT
cat 04-udp/build/manual-server.log
exit
```

`kill -INT` 只通知刚才保存的那个进程正常退出。日志末尾的 `RESULT` 中，`received` 是收到的包数，`sent` 是提交给 TX 成功的包数，`ignored` 是未处理的包数，`unsent` 是 TX 未接收的包数；`mbufs_in_use=0` 表示停止端口后，包池里的 mbuf 已全部归还。`sent` 本身不证明对端收到，客户端实际收到回复才完成本次验证。

最后的 `exit` 返回原来的 Shell；实验进程都退出后，这个网络空间及其中的 veth 会自动销毁。

## 3. 启动参数说明

这些参数交给 `main.c` 中的 `rte_eal_init(argc, argv)`。本集**没有自定义业务命令行参数**，不需要在末尾加 `-- --ip ... --port ...`。

| 参数 | 本集用它做什么 |
|---|---|
| `-l "$DPDK_CPU"` | 指定运行线程使用的逻辑 CPU 编号。这里只有一个收发循环，收包、处理、发包都在同一个工作核上执行。 |
| `-m 128` | 启动时为 DPDK 预分配 128 MB 内存，不是包的大小或数量。 |
| `--no-pci` | 禁用 PCI 总线探测；本实验使用软件网口。 |
| `--no-huge` | 使用普通匿名内存，不要求配置系统大页。 |
| `--no-shconf` | 不创建用于 DPDK 多进程共享的配置文件；本实验不启动 secondary 进程。 |
| `--no-telemetry` | 关闭 DPDK 遥测服务，本实验不需要它。 |
| `--vdev=net_af_packet0,iface=dpdk0,qpairs=1` | 创建一个 AF_PACKET 软件设备，连接当前网络空间中的 `dpdk0`，使用一对 RX/TX 队列。 |

最后一个参数拆开看：

- `net_af_packet0`：DPDK 设备名称。`main.c` 按这个名字寻找端口，不能只改命令里的名字。
- `iface=dpdk0`：连接的 Linux 网口名称，由前面的 `lab.py` 创建。
- `qpairs=1`：一条接收队列加一条发送队列，不代表两个 CPU 核。

`DPDK_CPU` 选取的是进程允许使用的 CPU，不保证它空闲或被独占。若要手动指定，可在启动前设置 `DPDK_CPU=编号`；本集不要通过增加 `-l` 中的 CPU 数量期待程序自动变成多核收发。若 CPU 编号超出该 DPDK 构建支持的 lcore 编号范围，可将 `-l "$DPDK_CPU"` 换成 `--lcores="0@$DPDK_CPU"`，把 DPDK 的 lcore 0 映射到该 CPU。

`RUNTIME_DIRECTORY` 是环境变量，指定 DPDK 运行时文件的存放位置，不是程序的 IP/端口配置。参数定义见 [DPDK EAL 文档](https://doc.dpdk.org/guides-25.11/linux_gsg/linux_eal_parameters.html)和 [AF_PACKET 驱动文档](https://doc.dpdk.org/guides-25.11/nics/af_packet.html)。

### IP、UDP 端口和队列在哪里配置

| 配置 | 当前值 | 修改位置 |
|---|---|---|
| 服务端 IP | `192.0.2.2` | `src/udp.c` 中的 `SERVER_IP` |
| 服务端 UDP 端口 | `9000` | `src/udp.c` 中的 `SERVER_PORT` |
| 客户端 IP、veth MAC、静态邻居项 | `192.0.2.1` 及上述固定 MAC | `scripts/lab.py` |
| 测试客户端的源端口 | `40000` | 本文客户端的 `bind()`；自动演示在 `tests/demo.py` 中配置 |
| RX/TX 队列 | 各一条，队列编号均为 `0` | `src/main.c` 的 `port_init()` 和收发循环 |

更改服务端 IP 时，还要同步修改客户端目标地址、静态邻居项和测试预期；更改 UDP 端口时，要同步修改客户端目标端口与测试预期。`main.c` 的 `Ready` 提示文字也要保持一致。改完源码后重新执行 `make -C 04-udp`。

## 4. 修改代码后跑完整测试

回到仓库根目录的普通 Bash 中执行：

```bash
source ./env.sh
make -C 04-udp check
```

它会自行创建另一套隔离实验网络，覆盖普通回显、空载荷、二进制、1472 字节载荷、非法头部与校验和、非本机请求、分片拒绝、连续收发，以及空闲退出和 mbuf 回收。测试通过会输出对应的 `PASS`，服务端退出时 `mbufs_in_use=0`。

只编译用 `make -C 04-udp`；一键看三条消息用 `bash 04-udp/run.sh`；回归检查用 `make -C 04-udp check`。

## 常见问题

- **提示缺少 `libdpdk` 或动态库**：先确认完成首页安装，再在当前 Bash 中执行 `source ./env.sh`。
- **`unshare` 提示不允许**：当前系统限制了普通用户创建实验网络，需使用允许 user namespace 的 Linux 环境。
- **找不到 `dpdk0`**：启动程序前没有进入 `lab.py` 创建的 Bash，或在另一个普通终端中启动了程序。
- **`Port net_af_packet0 not found`**：检查 `--vdev` 名称是否与源码一致，并确认安装了 AF_PACKET 驱动。
- **收不到回复**：先看服务端是否出现 `Ready`，再核对静态邻居项、目标 `192.0.2.2:9000`，以及客户端是否在同一实验网络中。

实验只配置新建网络空间里的 veth，不修改宿主物理网口和路由。AF_PACKET 仍经过 Linux 软件网络；这里验证收发功能，不测试物理网卡旁路性能。

## 包的所有权

- RX 成功后，应用负责这些 mbuf。
- `udp_reply()` 只修改内容，不释放、不转交 mbuf；返回 `true` 表示可提交回复。
- 不处理的报文和 TX 未接收的报文由 `main` 释放。
- TX 返回 1 后，驱动接管这个 mbuf，应用不能再次释放或使用它。

## 支持边界

本集接收无 VLAN 的 Ethernet + IPv4 + UDP，IPv4 头固定为 20 字节，不处理 IP 选项或分片。检查目的 IP、UDP 目的端口、长度关系与校验和；IPv4 UDP 校验和为零的请求按协议允许接收。回复重新计算 IP 和 UDP 校验和。

这里只回显本机收到的 UDP 内容，不进行路由、ARP、ICMP、TCP、重组、重传或 socket Hook。遇到暂不支持的报文，归还缓冲区而不回复。
