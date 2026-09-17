#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define MBUF_COUNT 4095
#define RX_DESCRIPTOR_COUNT 128
#define BURST_SIZE 32
#define PREVIEW_BYTES 16

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    /* 信号处理函数只做标记，由主循环退出后统一清理资源。 */
    stop_requested = 1;
}

static void port_init(uint16_t port_id, struct rte_mempool *packet_pool)
{
    struct rte_eth_conf port_config = {0};
    int result;

    /* 这一版只接收：配置 1 个 RX 队列，0 个 TX 队列。 */
    result = rte_eth_dev_configure(port_id, 1, 0, &port_config);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot configure port: %s\n", rte_strerror(-result));
    }

    /* RX 队列用这个池中的 mbuf 存放收到的报文。 */
    result = rte_eth_rx_queue_setup(
        port_id, 0, RX_DESCRIPTOR_COUNT, rte_socket_id(), NULL, packet_pool);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot set up RX queue: %s\n", rte_strerror(-result));
    }

    result = rte_eth_dev_start(port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start port: %s\n", rte_strerror(-result));
    }
}

static void print_packet(const struct rte_mbuf *packet, uint64_t packet_number)
{
    uint32_t packet_length = rte_pktmbuf_pkt_len(packet);
    struct rte_ether_hdr header_copy;
    const struct rte_ether_hdr *ethernet_header;

    /* 报文可能短于以太网头，也可能分段存放；read 会检查范围。 */
    ethernet_header = rte_pktmbuf_read(packet, 0, sizeof(header_copy), &header_copy);
    if (ethernet_header == NULL) {
        printf("RX %" PRIu64 ": bytes=%" PRIu32 ", incomplete Ethernet header\n",
               packet_number,
               packet_length);
        return;
    }

    char source_mac[RTE_ETHER_ADDR_FMT_SIZE];
    char destination_mac[RTE_ETHER_ADDR_FMT_SIZE];

    rte_ether_format_addr(source_mac, sizeof(source_mac), &ethernet_header->src_addr);
    rte_ether_format_addr(
        destination_mac, sizeof(destination_mac), &ethernet_header->dst_addr);

    printf("RX %" PRIu64 ": bytes=%" PRIu32 ", EtherType=0x%04x\n",
           packet_number,
           packet_length,
           rte_be_to_cpu_16(ethernet_header->ether_type));
    printf("  %s -> %s\n", source_mac, destination_mac);

    uint32_t preview_length = packet_length - sizeof(header_copy);
    if (preview_length > PREVIEW_BYTES) {
        preview_length = PREVIEW_BYTES;
    }
    if (preview_length == 0) {
        puts("  No bytes after Ethernet header.");
        return;
    }

    uint8_t preview_copy[PREVIEW_BYTES];
    const uint8_t *preview;

    preview = rte_pktmbuf_read(packet, sizeof(header_copy), preview_length, preview_copy);
    if (preview == NULL) {
        puts("  Cannot read packet data.");
        return;
    }

    printf("  hex :");
    for (uint32_t index = 0; index < preview_length; index++) {
        printf(" %02x", preview[index]);
    }

    /* 网络数据不保证以 '\0' 结尾；按长度打印，非可打印字节显示为点。 */
    printf("\n  text: ");
    for (uint32_t index = 0; index < preview_length; index++) {
        uint8_t byte = preview[index];
        putchar(byte >= 32 && byte <= 126 ? byte : '.');
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    /* 每行立即出现在终端，也方便核对重定向保存的运行日志。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int result = rte_eal_init(argc, argv);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize EAL.\n");
    }

    /* 名字对应启动参数 --vdev=net_af_packet0,iface=dpdk0。 */
    uint16_t port_id;
    result = rte_eth_dev_get_port_by_name("net_af_packet0", &port_id);
    if (result < 0) {
        rte_exit(
            EXIT_FAILURE,
            "Port net_af_packet0 not found. Use --vdev=net_af_packet0,iface=dpdk0\n");
    }

    struct rte_mempool *packet_pool;
    packet_pool = rte_pktmbuf_pool_create(
        "packet_pool", MBUF_COUNT, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (packet_pool == NULL) {
        rte_exit(
            EXIT_FAILURE, "Cannot create packet pool: %s\n", rte_strerror(rte_errno));
    }

    port_init(port_id, packet_pool);

    if (signal(SIGINT, handle_signal) == SIG_ERR) {
        rte_exit(EXIT_FAILURE, "Cannot install SIGINT handler.\n");
    }
    if (signal(SIGTERM, handle_signal) == SIG_ERR) {
        rte_exit(EXIT_FAILURE, "Cannot install SIGTERM handler.\n");
    }

    uint64_t total_received = 0;
    puts("Ready: receive on dpdk0. Press Ctrl+C to stop.");

    while (!stop_requested) {
        /* 数组里放的是报文指针；received_count 是本次实际收到的数量。 */
        struct rte_mbuf *packets[BURST_SIZE];
        uint16_t received_count;

        received_count = rte_eth_rx_burst(port_id, 0, packets, BURST_SIZE);

        for (uint16_t index = 0; index < received_count; index++) {
            total_received++;
            print_packet(packets[index], total_received);

            /* 看完就归还；本版没有把报文交给发送队列或其他线程。 */
            rte_pktmbuf_free(packets[index]);
        }
    }

    printf("Stopped. Received %" PRIu64 " packets.\n", total_received);

    /* 先停止并关闭端口，再释放它使用的缓冲池。 */
    result = rte_eth_dev_stop(port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot stop port: %s\n", rte_strerror(-result));
    }

    result = rte_eth_dev_close(port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot close port: %s\n", rte_strerror(-result));
    }

    unsigned buffers_in_use = rte_mempool_in_use_count(packet_pool);
    printf("Mbufs still in use: %u\n", buffers_in_use);
    if (buffers_in_use != 0) {
        rte_exit(EXIT_FAILURE, "Some received packets were not released.\n");
    }

    rte_mempool_free(packet_pool);

    result = rte_eal_cleanup();
    if (result < 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
