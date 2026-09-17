# DPDK：逐步搭建用户态协议栈

从接收一个以太网帧开始，逐集加入 UDP、ARP、ICMP、多核和 TCP。每集目录包含本集完整源码和运行方法；下一集承接上一集的程序继续增加功能。

## 目录

```text
dpdk-course/
├── README.md
├── env.sh
├── scripts/                  安装和隔离实验网络
├── 03-recv/
│   ├── src/main.c            第三集：接收报文
│   ├── README.md
│   ├── Makefile
│   └── tests/
└── 04-udp/
    ├── src/
    │   ├── main.c            初始化、收包、调用 UDP、发包和退出
    │   ├── udp.c             检查请求并原地构造回复
    │   └── udp.h             模块接口
    ├── README.md
    ├── Makefile
    ├── run.sh
    └── tests/
```

代码需要 Linux。

## 1. 下载仓库

```bash
git clone https://github.com/hjx-oss/dpdk-course.git
cd dpdk-course
```

也可以下载 ZIP 并解压。在项目根目录打开 **Bash**；目录名称和位置可以自行选择。后面的命令均从根目录执行。

## 2. 配置环境

验证环境为 Ubuntu 22.04、x86_64、DPDK 25.11.3。先安装编译器、Python、构建依赖和网络工具：

```bash
sudo apt update
sudo apt install -y build-essential pkg-config python3 python3-venv \
  libnuma-dev curl ca-certificates xz-utils iproute2 ethtool util-linux
```

公用服务器已经具备这些依赖时，直接跳过系统安装。下面的脚本只把构建工具、DPDK 源码和安装产物放进本仓库的 `.deps/`：

```bash
bash scripts/setup.sh
source ./env.sh
pkg-config --modversion libdpdk
```

最后一条应输出 `25.11.3`。安装脚本依次完成：建立 Python 工具环境 → 下载并核对 DPDK SHA-256 → 配置 AF_PACKET 驱动 → 编译 → 安装。默认使用 4 个编译任务，可以通过 `DPDK_BUILD_JOBS` 调整。

新开终端时重新执行 `source ./env.sh`。已有同版本 DPDK 的朋友，也可以先将 `DPDK_INSTALL_PREFIX` 设置为自己的安装前缀，再加载 `env.sh`；该目录下应有 `include/` 和 `lib/pkgconfig/libdpdk.pc`。

确认系统允许普通用户建立隔离实验网络：

```bash
unshare --user --map-root-user --net true
```

命令应正常退出。如果提示不允许，请使用允许 user namespace 的 Linux 虚拟机，或请管理员提供实验环境；不需要修改物理网卡或关闭整机安全策略。

## 3. 跑第三集：接收报文

```bash
source ./env.sh
make -C 03-recv
make -C 03-recv check
```

测试会创建独立网络空间和一对 veth，发送报文，核对接收输出，然后检查退出和缓冲区回收。预期最后出现 `PASS: isolated veth test completed`。

## 4. 跑第四集：UDP 回显

```bash
bash 04-udp/run.sh
```

这条命令编译程序，在独立实验网络中启动服务端，用普通 UDP socket 发送三条消息，核对回复后退出：

```text
PASS hello-1: reply matches
PASS hello-2: reply matches
PASS hello-3: reply matches
RESULT received=3 sent=3 ignored=0 unsent=0 mbufs_in_use=0
```

需要分别编译或运行完整检查时：

```bash
source ./env.sh
make -C 04-udp
make -C 04-udp check
```

完整检查还覆盖空载荷、二进制、1472 字节载荷、头部长度、校验和、非本机请求、分片拒绝、连续收发和空闲退出。详细边界见 [第四集说明](04-udp/README.md)。

## 实验里的包怎么走

```text
同一个独立网络命名空间

Python UDP socket
192.0.2.1:40000
      │
   client0 ══════ veth ══════ dpdk0
                                 │ AF_PACKET
                           DPDK UDP 程序
                           192.0.2.2:9000
```

`192.0.2.2` 由 C 程序判断，不配置到 Linux 的 `dpdk0` 上。第四集暂时给客户端设置静态邻居项，让它知道服务端的 MAC；第五集再加入 ARP。回复的目的 MAC、IP 和端口直接取自收到的请求。

脚本只在新建的 user/network namespace 中创建和配置 `client0 / dpdk0`，不更改宿主机物理网口、默认路由、驱动绑定或大页设置。AF_PACKET 仍经过 Linux 软件网络，这套实验验证功能，不测物理网卡旁路性能。[DPDK AF_PACKET 文档](https://doc.dpdk.org/guides-25.11/nics/af_packet.html)

## 各集代码

| 集数 | 本集增加的能力 | 代码与运行说明 |
|---|---|---|
| 03 | 收包、查看内容、归还 mbuf | [03-recv](03-recv/README.md) |
| 04 | 识别 UDP 请求并回显 | [04-udp](04-udp/README.md) |
