# 第七课：UDP 应用接口

承接第六课，将“UDP 模块立即回显”改成“协议接收 → 应用队列 → 应用处理 → 发送队列 → ARP 解析 → 组包发送”。仍使用单个 lcore 顺序执行，下一课再引入核间交接。

## 编译和运行

以下命令在克隆后的仓库根目录执行。目录可以放在任意位置；环境安装见根目录 README。

```bash
source ./env.sh
make -C 07-udp-api
bash 07-udp-api/run.sh
make -C 07-udp-api check
```

`run.sh` 自动建立独立 user/network namespace 和 veth，启动应用和 Linux 客户端；实验结束后退出该网络空间。它不改物理网卡、宿主路由、驱动绑定或大页。

应看到：

- DPDK 应用先从 `192.0.2.2:9002` 主动发出 `hello-from-dpdk`，Linux 的 `192.0.2.1:40001` 收到并回送确认。
- 发到 `9000` 的内容原样回显；发到 `9001` 的 `hello DPDK 123!` 返回 `HELLO DPDK 123!`。
- 系统 ping 正常往返，ARP 无需静态邻居项，最后 `mbufs_in_use=0`。

`check` 除真实 veth 收发外，还检查 API 参数、端口冲突、两个端口的接收隔离、空数据报、截断、队列满、环形复用、ARP 失败、包池耗尽和退出清理。

## 手动运行与参数

从仓库根目录进入实验网络：

```bash
source ./env.sh
python3 scripts/lab.py -- bash --noprofile --norc
CPU=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
./07-udp-api/build/udp_api -l "$CPU" -m 128 \
  --no-pci --no-huge --no-shconf --no-telemetry \
  --vdev=net_af_packet0,iface=dpdk0,qpairs=1
```

这里的 shell 已在独立实验网络内。另一个普通终端不在同一网络空间；完整的双端演示建议直接使用 `run.sh`，它会先绑定客户端端口，再启动 DPDK 程序。

| 参数 | 用途 |
|---|---|
| `-l "$CPU"` | 选当前进程允许使用的一个逻辑 CPU |
| `-m 128` | 请求 128 MB EAL 内存 |
| `--no-pci` | 不探测或接管物理 PCI 网卡 |
| `--no-huge` | 软件实验使用普通内存，无需配置宿主大页 |
| `--no-shconf` | 不建立供多个 DPDK 进程共享的配置 |
| `--no-telemetry` | 关闭本课用不到的遥测服务 |
| `--vdev=net_af_packet0,iface=dpdk0,qpairs=1` | AF_PACKET 驱动接入本实验的 dpdk0，使用一对 RX/TX 队列 |

程序无额外参数时运行三个应用端口。保留第五课的查询模式：在上述命令末尾加 `-- --arp-query 192.0.2.1`，只做 ARP 查询并退出；查询 `192.0.2.99` 可观察有限重试后失败。按 Ctrl+C 会关闭应用、清理排队数据、停止端口并检查 mbuf 回收。

## 应用接口

`app.c` 只通过 `udp.h` 收发应用数据，不读取以太网/IP/UDP 头，不调用 DPDK 收发函数。

| 接口 | 本课语义 |
|---|---|
| `udp_socket()` | 分配本库的端点槽位，返回编号；不是 Linux fd |
| `udp_bind(fd, port)` | 绑定本机 192.0.2.2 的显式非零端口，重复端口报 EADDRINUSE |
| `udp_sendto(fd, data, length, peer)` | 将数据和目标地址复制进待发队列；成功返回数据长度，包括 0 |
| `udp_recvfrom(fd, data, capacity, &peer)` | 一次取一个数据报，返回复制字节数和来源地址；小缓冲区截断后丢弃余下部分 |
| `udp_take_error(fd)` | 取走最近一次异步发送错误；无错误返回 0；多个错误可能合并 |
| `udp_close(fd)` | 关闭端点，丢弃未取走和未发出的数据，释放槽位 |

同步失败返回 `-1` 并设置 `errno`：空接收队列或满发送队列为 `EAGAIN`，未绑定为 `EINVAL`，无效编号为 `EBADF`，超过 1472 字节为 `EMSGSIZE`，不支持的目标网段为 `ENETUNREACH`。0 字节 UDP 数据报不是连接关闭。

`udp_sendto` 成功只代表进入本地队列。后来 ARP 失败或待发超过四秒时通过 `udp_take_error` 返回 `EHOSTUNREACH`；组包时包池不足返回 `ENOBUFS`；TX 不接收时返回 `EIO`。这些错误不是对方已收到或没收到的确认。

## 源码入口与所有权

- `src/app.c` / `app.h`：9000 回显、9001 ASCII 大写、9002 主动发送与接收确认。
- `src/udp.h`：应用可见接口，IP 和端口采用主机字节序。
- `src/udp.c`：端点表、收发队列、端口查找、数据报复制、等待 ARP、新建 UDP 帧。
- `src/udp_internal.h`：主循环与协议模块使用的内部接口。
- `src/ipv4.c` / `ipv4.h`：公共检查，区分丢弃、已复制到应用队列、可立即回复三种结果。
- `src/main.c`：一轮执行 RX → 协议输入 → 应用处理 → UDP 待发 → ARP 轮询；统一提交 TX 和回收 mbuf。
- `src/arp.c` / `src/icmp.c`：沿用第六课功能。

收到的 mbuf 在 UDP 数据被复制后由 main 释放。待收、待发队列持有固定大小的数据副本，不持有 mbuf。MAC 已知后才分配发送 mbuf：`udp_poll` 交给 main，TX 接收成功后归驱动，未接收则由 main 释放。

## 当前边界

最多 4 个端点，每个端点的 RX、TX 队列各容纳 8 个数据报。每个数据报最多 1472 字节；不做 IP 分片/重组、广播、多播或跨网段路由。每个端点的 TX 按 FIFO 等待，队头未解析会暂时阻挡该端点后续数据；其他端点继续被轮询。每条待发数据有独立的四秒期限。

接口非阻塞，仅支持单线程顺序调用；队列使用普通数组，没有锁，也不是跨核 `rte_ring`。这是本项目的应用 API，不是 socket Hook，也不具备 POSIX socket 的全部语义。应用示例对排队失败打印错误并放弃该次回复，不保证重试或可靠交付。

参考：[DPDK IPv4/UDP 校验 API](https://doc.dpdk.org/api-25.11/rte__ip4_8h.html)、[Linux UDP 数据报语义](https://man7.org/linux/man-pages/man7/udp.7.html)。
