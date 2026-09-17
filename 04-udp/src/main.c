#include "udp.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#define MBUF_COUNT 4095
#define DESCRIPTOR_COUNT 128
#define BURST_SIZE 32

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void port_init(uint16_t port_id, struct rte_mempool *packet_pool)
{
    struct rte_eth_conf port_config = {0};
    int result;

    /* 第三集只有 RX；现在要回包，因此再配置一条 TX 队列。 */
    result = rte_eth_dev_configure(port_id, 1, 1, &port_config);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot configure port: %s\n", rte_strerror(-result));
    }

    result = rte_eth_rx_queue_setup(
        port_id, 0, DESCRIPTOR_COUNT, rte_socket_id(), NULL, packet_pool);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot set up RX queue: %s\n", rte_strerror(-result));
    }

    result = rte_eth_tx_queue_setup(
        port_id, 0, DESCRIPTOR_COUNT, rte_socket_id(), NULL);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot set up TX queue: %s\n", rte_strerror(-result));
    }

    result = rte_eth_dev_start(port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start port: %s\n", rte_strerror(-result));
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    int result = rte_eal_init(argc, argv);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot initialize EAL.\n");
    }

    uint16_t port_id;
    result = rte_eth_dev_get_port_by_name("net_af_packet0", &port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Port net_af_packet0 not found.\n");
    }

    struct rte_mempool *packet_pool;
    packet_pool = rte_pktmbuf_pool_create(
        "packet_pool", MBUF_COUNT, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (packet_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create packet pool: %s\n", rte_strerror(rte_errno));
    }

    port_init(port_id, packet_pool);

    if (signal(SIGINT, handle_signal) == SIG_ERR
        || signal(SIGTERM, handle_signal) == SIG_ERR) {
        rte_exit(EXIT_FAILURE, "Cannot install signal handler.\n");
    }

    uint64_t received_total = 0;
    uint64_t sent_total = 0;
    uint64_t ignored_total = 0;
    uint64_t unsent_total = 0;
    puts("Ready: UDP 192.0.2.2:9000 on dpdk0. Press Ctrl+C to stop.");

    while (!stop_requested) {
        struct rte_mbuf *packets[BURST_SIZE];
        uint16_t received_count;

        received_count = rte_eth_rx_burst(port_id, 0, packets, BURST_SIZE);
        received_total += received_count;

        for (uint16_t index = 0; index < received_count; index++) {
            struct rte_mbuf *packet = packets[index];

            if (!udp_reply(packet)) {
                ignored_total++;
                rte_pktmbuf_free(packet);
                continue;
            }

            /* 这里只提交一个回复；返回 1 才表示驱动接收了这个 mbuf。 */
            uint16_t sent_count = rte_eth_tx_burst(port_id, 0, &packet, 1);
            if (sent_count == 0) {
                unsent_total++;
                rte_pktmbuf_free(packet);
            } else {
                sent_total++;
            }
        }
    }

    /* 已提交成功的包由驱动负责回收，应用不能再次 free。 */
    result = rte_eth_dev_stop(port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot stop port: %s\n", rte_strerror(-result));
    }
    result = rte_eth_dev_close(port_id);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot close port: %s\n", rte_strerror(-result));
    }

    unsigned buffers_in_use = rte_mempool_in_use_count(packet_pool);
    printf("RESULT received=%" PRIu64 " sent=%" PRIu64 " ignored=%" PRIu64
           " unsent=%" PRIu64 " mbufs_in_use=%u\n",
           received_total, sent_total, ignored_total, unsent_total, buffers_in_use);
    rte_mempool_free(packet_pool);

    result = rte_eal_cleanup();
    return buffers_in_use == 0 && result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
