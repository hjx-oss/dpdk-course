# 第五集：ARP 主动查询、应答与邻居表

承接第四集的 UDP 回显，加入双向 ARP 地址解析：回答客户端的询问，也主动广播询问目标 IP、处理应答、保存 MAC。邻居表支持命中、过期、有限重试和失败返回。

## 源码入口

| 文件 | 职责 |
|---|---|
| [src/main.c](src/main.c) | EAL、包池和端口初始化；按以太网类型调用模块；统一发送与释放；每圈轮询 ARP 查询 |
| [src/net_config.h](src/net_config.h) | 本机 IP、UDP 端口与子网掩码 |
| [src/arp.h](src/arp.h)、[src/arp.c](src/arp.c) | 构造广播请求、检查请求与应答；维护邻居表、有效期与重试 |
| [src/udp.h](src/udp.h)、[src/udp.c](src/udp.c) | 检查 UDP 请求并原样回显正文 |

代码包含本集需要的全部模块，不依赖其他集的业务源码。收包、处理、发包仍在一个工作核上执行。

## 运行准备

需要 Linux、DPDK 25.11.3 和允许普通用户创建 user/network namespace 的环境。先按[仓库首页](../README.md#2-配置环境)安装依赖。以下命令均从**仓库根目录的 Bash** 执行，仓库可以放在任意目录。

## 1. 一键运行

```bash
bash 05-arp/run.sh
```

不需要额外参数。脚本加载环境、编译程序、创建隔离 veth、启动服务端，验证后停止自己的进程并退出。依次验证客户端自动学习 MAC 后完成 UDP 回显、DPDK 主动查询客户端 MAC、查询不存在的地址后结束重试。输出包括：

```text
PASS start: no static neighbor entry
PASS hello-1: reply matches
PASS hello-2: reply matches
PASS hello-3: reply matches
PASS learned: 192.0.2.2 -> 02:00:00:00:00:02
RESULT received=4 sent=4 ignored=0 learned=0 unsent=0 mbufs_in_use=0
PASS active query: broadcast request, Linux reply, MAC learned
NEIGHBOR 192.0.2.1 -> 02:00:00:00:00:01
RESULT received=1 sent=1 ignored=0 learned=1 unsent=0 mbufs_in_use=0
PASS missing peer: three requests at one-second intervals, then failed
NEIGHBOR_FAILED 192.0.2.99
RESULT received=0 sent=3 ignored=0 learned=0 unsent=0 mbufs_in_use=0
```

`received` 统计所有接收帧，包括 ARP。`sent` 是 TX 成功接收的帧数，客户端收到回复才证明通信完成；`mbufs_in_use=0` 表示正常停止端口后缓冲区全部归还。这些数字来自本次自动演示，不是性能测试。`learned` 统计收到 ARP 应答后只学习地址的帧；收到请求时也会学习合法的发送者地址，但该帧按回复路径计数。

`NEIGHBOR_FAILED` 是故意查询不存在的 `192.0.2.99` 的预期结果；测试脚本确认它以失败退出码结束、只发三次请求，才报告通过。

## 2. 手动运行

### 编译、建立实验网络

```bash
source ./env.sh
make -C 05-arp
python3 scripts/lab.py
```

最后一条打开新的实验 Bash。**之后的启动与发包都在这个 Bash 中进行**，另开普通终端不会自动进入同一网络空间。

不加 `--static-neighbor`：本集由 ARP 自动获得目标 MAC。`client0` 的 IP 为 `192.0.2.1`，另一端 `dpdk0` 通过 AF_PACKET 交给 DPDK 程序。程序处理 `192.0.2.2`，这个地址不配置到 Linux 的 `dpdk0` 上。

### 启动服务端

选择进程允许使用的 CPU，把运行时文件留在本集构建目录：

```bash
DPDK_CPU=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
export RUNTIME_DIRECTORY="$(mktemp -d "$PWD/05-arp/build/runtime.XXXXXX")"
./05-arp/build/arp_udp \
  -l "$DPDK_CPU" \
  -m 128 \
  --no-pci \
  --no-huge \
  --no-shconf \
  --no-telemetry \
  --vdev=net_af_packet0,iface=dpdk0,qpairs=1 \
  > 05-arp/build/manual-server.log 2>&1 &
server_pid=$!
trap 'kill -INT "$server_pid" 2>/dev/null; wait "$server_pid" 2>/dev/null' EXIT
```

`&` 让服务端在后台运行，同一个终端可以继续发包。`server_pid` 只保存本次进程，退出实验 Bash 时通知它正常停止。

```bash
cat 05-arp/build/manual-server.log
```

看到 `Ready:` 后再继续；若尚未出现，稍后再次查看。如果程序报错退出，先按下方参数和网口名称检查。

### 用普通 socket 发送 UDP

```bash
python3 - <<'PYCLIENT'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
    client.bind(("192.0.2.1", 40000))
    client.settimeout(3)
    client.sendto(b"hello-dpdk", ("192.0.2.2", 9000))
    data, address = client.recvfrom(2048)
    print("reply:", data, "from:", address)
    assert data == b"hello-dpdk"
    assert address == ("192.0.2.2", 9000)
PYCLIENT
ip neigh show dev client0
```

第一段应打印 `reply: b'hello-dpdk' from: ('192.0.2.2', 9000)`。邻居表中应出现 `192.0.2.2 lladdr 02:00:00:00:00:02`，状态可能为 `REACHABLE`、`STALE` 等，随时间变化；它不是手动设置的 `PERMANENT` 项。

### 停止并退出

```bash
kill -INT "$server_pid"
wait "$server_pid"
trap - EXIT
cat 05-arp/build/manual-server.log
exit
```

停止时查看 `RESULT`，确认 `mbufs_in_use=0`。最后一条返回原来的 Shell；实验进程都退出后，独立网络空间和其中的 veth 自动销毁。

## 3. 程序启动参数


下表列出 EAL 参数和可选的应用参数；EAL 参数先由 `rte_eal_init(argc, argv)` 处理。默认模式持续处理收到的报文；可在它们后面加 `-- --arp-query IPv4地址`，查询一次 MAC，成功或失败后退出。单独的 `--` 分开 EAL 参数与应用参数。

| 参数 | 本集用它做什么 |
|---|---|
| `-- --arp-query 192.0.2.1` | 可选的应用参数，主动查询客户端 MAC；不加则持续作为回显服务端运行。 |
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


## 4. 单独观察 DPDK 主动查询

不需要手动配置网络的完整验证：

```bash
source ./env.sh
make -C 05-arp
python3 05-arp/tests/resolve.py
```

脚本会在新建的隔离网络里，分别查询存在的 `.1` 和不存在的 `.99`，捕获广播帧、核对字段、统计次数与间隔。日志在本集 `build/active-query.log` 和 `build/active-missing.log`。

如果要直接运行 C 程序，先停止上一段手动实验的服务端；以下从仓库根目录重新打开一套实验网络，不与另一个收发进程共享端口：

```bash
python3 scripts/lab.py
# 后续命令在新打开的实验 Bash 内执行。
DPDK_CPU=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
export RUNTIME_DIRECTORY="$(mktemp -d "$PWD/05-arp/build/runtime.XXXXXX")"
./05-arp/build/arp_udp \
  -l "$DPDK_CPU" -m 128 \
  --no-pci --no-huge --no-shconf --no-telemetry \
  --vdev=net_af_packet0,iface=dpdk0,qpairs=1 \
  -- --arp-query 192.0.2.1
exit
```

程序应打印 `NEIGHBOR 192.0.2.1 -> 02:00:00:00:00:01` 后正常退出。把末尾地址改成 `192.0.2.99`，会打印 `NEIGHBOR_FAILED` 并以退出码 1 结束；测试脚本把这个预期失败视为通过。`inet_pton()` 只检查地址文字，是否同网段、是否本机等约束由 ARP 模块检查。

## 5. 修改配置与检查

- 服务端 IP 与 UDP 端口在 `src/net_config.h`：`SERVER_IP` 为 `192.0.2.2`，`SERVER_PORT` 为 `9000`，`NETMASK` 为 `255.255.255.0`。
- 本机 MAC 由 `main.c` 调用 `rte_eth_macaddr_get()` 读取，实验网口名称和 MAC 在根目录 `scripts/lab.py` 中创建。
- 更改 IP 或端口时，同步修改客户端目标、自动测试预期和 `main.c` 的 Ready 提示，再重新编译。
- `run.sh` 是无参数的自动演示；需要自行选择 CPU 或直接观察日志时，使用上面的手动启动命令。

完整检查：

```bash
source ./env.sh
make -C 05-arp check
```

检查主动广播请求与真实 Linux 应答、无人应答时的三次重试，以及 ARP 应答字段与填充、错误地址和格式、UDP 长度与校验和、空正文、二进制、最大普通 MTU 载荷、错误输入后的连续收发、空闲退出和端口重新打开。

状态测试还覆盖缓存命中不发包、三十秒后重新查询、表满、地址边界和包池耗尽。这部分使用真实 DPDK mbuf，但主动推进传入的计时器刻度，不需要实际等待三十秒。真实重试间隔则由 `tests/resolve.py` 在 veth 上捕获帧并核对。

## 包的所有权

- RX 后 mbuf 归应用。`arp_input()` 返回 `ARP_IGNORED` 或 `ARP_LEARNED` 时，`main` 释放这张包；返回 `ARP_REPLY_READY` 时，包已改成回复，交给 TX。
- UDP / IPv4 入口仍用布尔返回值表示是否准备好了回复。
- `arp_poll()` 返回新分配的 ARP 请求，所有权交给 `main`；返回空指针表示本圈没有请求可发送。
- TX 接收成功后由驱动接管；TX 未接收的包仍由 `main` 释放。邻居表只保存地址和状态，不保存 mbuf 指针。

## 当前支持范围

ARP 支持以太网/IPv4 格式：应答发给本机的请求，并能主动查询配置网段内的其他单播主机。收到应答只接受匹配当前 PENDING 查询且目的 MAC 为本机的帧；请求中的合法发送者也可学习。两端各自维护自己的邻居表。

本程序的策略是十六项固定容量、三十秒有效期、一秒查询间隔、最多三次尝试。第三次尝试后再等一秒才失败；失败记录保留一秒后可清理。分配或 TX 失败也算一次尝试，避免无间隔重试。策略由 `src/arp.c` 常量设置，不是 ARP 协议强制值。表满明确返回失败，不覆盖仍有效的记录。

当前只有一个线程访问邻居表；不实现无偿 ARP 更新、完整的地址冲突检测、ARP 身份认证、跨网段路由或待发应用数据队列。第七课把 UDP 主动发送接到查询结果，并处理数据等待与超时回收。

IPv4 只支持无 VLAN、20 字节 IP 头，不支持 IP 选项和分片。UDP 只回显本机指定端口；IPv4 UDP 校验和为零的请求可接收，回复重新计算校验和。本集还不处理 ICMP，ping 不会收到 Echo 回复。不做路由或 TCP。

veth 与 AF_PACKET 仍经过 Linux 软件网络；本实验验证协议功能，不代表物理网卡旁路性能。脚本只配置新建网络空间，不修改宿主机物理网口、路由、驱动绑定或大页设置。

## 排查入口

- 找不到 DPDK 头文件或动态库：确认安装完成，并在当前 Bash 执行 `source ./env.sh`。
- 找不到 `dpdk0`：确认进入 `lab.py` 创建的实验 Bash，且在同一 Bash 启动服务端。
- `unshare` 不允许：使用允许 user namespace 的 Linux 环境，不需要改物理网络。
- 邻居项一直 `INCOMPLETE`：检查服务端 Ready、目标 IP、AF_PACKET 网口及 ARP 模块。
- 邻居项已学习但应用超时：检查后续 IPv4、UDP 分支和目标端口。

协议定义见 [RFC 826：ARP](https://www.rfc-editor.org/rfc/rfc826)。
