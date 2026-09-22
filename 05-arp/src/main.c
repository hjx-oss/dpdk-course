#include "arp.h"
#include "udp.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>

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

    argc -= result;
    argv += result;
    rte_be32_t query_ip = 0;
    bool query_mode = argc != 1;
    if (query_mode
        && (argc != 3 || strcmp(argv[1], "--arp-query") != 0
            || inet_pton(AF_INET, argv[2], &query_ip) != 1)) {
        rte_exit(EXIT_FAILURE, "Usage: program [EAL options] -- --arp-query IPv4\n");
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

    struct rte_ether_addr local_mac;
    result = rte_eth_macaddr_get(port_id, &local_mac);
    if (result < 0) {
        rte_exit(EXIT_FAILURE, "Cannot read port MAC.\n");
    }

    arp_init(&local_mac);

    if (signal(SIGINT, handle_signal) == SIG_ERR
        || signal(SIGTERM, handle_signal) == SIG_ERR) {
        rte_exit(EXIT_FAILURE, "Cannot install signal handler.\n");
    }

    uint64_t received_total = 0;
    uint64_t sent_total = 0;
    uint64_t ignored_total = 0;
    uint64_t learned_total = 0;
    bool query_failed = false;
    uint64_t unsent_total = 0;
    puts("Ready: ARP + UDP 192.0.2.2:9000 on dpdk0. Press Ctrl+C to stop.");

    while (!stop_requested) {
        uint64_t now = rte_get_timer_cycles();
        struct rte_mbuf *packets[BURST_SIZE];
        uint16_t received_count;

        received_count = rte_eth_rx_burst(port_id, 0, packets, BURST_SIZE);
        received_total += received_count;

        for (uint16_t index = 0; index < received_count; index++) {
            struct rte_mbuf *packet = packets[index];

            bool reply_ready = false;
            if (rte_pktmbuf_linearize(packet) == 0
                && rte_pktmbuf_pkt_len(packet) >= sizeof(struct rte_ether_hdr)) {
                struct rte_ether_hdr *ethernet;
                ethernet = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
                if (ethernet->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                    enum arp_result action = arp_input(packet, now);
                    if (action == ARP_LEARNED) {
                        learned_total++;
                        rte_pktmbuf_free(packet);
                        continue;
                    }
                    reply_ready = action == ARP_REPLY_READY;
                } else if (ethernet->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                    reply_ready = udp_reply(packet);
                }
            }
            if (!reply_ready) {
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
        /* 查询不阻塞收包，收到应答后的下一圈即可读到结果。 */
        if (query_mode) {
            struct rte_ether_addr peer_mac;
            enum arp_state state = arp_resolve(query_ip, &peer_mac, now);
            if (state == ARP_REACHABLE) {
                char mac_text[RTE_ETHER_ADDR_FMT_SIZE];
                rte_ether_format_addr(mac_text, sizeof(mac_text), &peer_mac);
                printf("NEIGHBOR %s -> %s\n", argv[2], mac_text);
                stop_requested = 1;
            } else if (state == ARP_FAILED) {
                printf("NEIGHBOR_FAILED %s\n", argv[2]);
                query_failed = true;
                stop_requested = 1;
            }
        }

        struct rte_mbuf *request = NULL;
        if (!stop_requested) {
            request = arp_poll(packet_pool, now);
        }
        if (request != NULL) {
            uint16_t sent_count = rte_eth_tx_burst(port_id, 0, &request, 1);
            if (sent_count == 0) {
                unsent_total++;
                rte_pktmbuf_free(request);
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
           " learned=%" PRIu64 " unsent=%" PRIu64 " mbufs_in_use=%u\n",
           received_total, sent_total, ignored_total, learned_total, unsent_total, buffers_in_use);
    rte_mempool_free(packet_pool);

    result = rte_eal_cleanup();
    return buffers_in_use == 0 && result == 0 && !query_failed ? EXIT_SUCCESS : EXIT_FAILURE;
}
