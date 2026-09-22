/* 使用真实 mbuf 和可控时间检查状态转换，不用等待缓存的三十秒。 */
#include "arp.h"
#include "net_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_eal.h>

static const struct rte_ether_addr own = {{2, 0, 0, 0, 0, 2}};
static const struct rte_ether_addr peer = {{2, 0, 0, 0, 0, 1}};

static struct rte_mbuf *answer(struct rte_mempool *pool, rte_be32_t ip)
{
    struct rte_mbuf *packet = rte_pktmbuf_alloc(pool);
    assert(packet);
    char *data = rte_pktmbuf_append(packet, 60);
    assert(data);
    memset(data, 0, 60);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    eth->src_addr = peer;
    eth->dst_addr = own;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);
    arp->arp_hardware = rte_cpu_to_be_16(1);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = 6;
    arp->arp_plen = 4;
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
    arp->arp_data.arp_sha = peer;
    arp->arp_data.arp_sip = ip;
    arp->arp_data.arp_tha = own;
    arp->arp_data.arp_tip = rte_cpu_to_be_32(SERVER_IP);
    return packet;
}

static void query(struct rte_mempool *pool, rte_be32_t ip, uint64_t now)
{
    struct rte_mbuf *packet = arp_poll(pool, now);
    assert(packet && rte_pktmbuf_pkt_len(packet) == 60);
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    assert(rte_is_broadcast_ether_addr(&eth->dst_addr));
    assert(rte_is_same_ether_addr(&eth->src_addr, &own));
    assert(arp->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST));
    assert(arp->arp_data.arp_tip == ip);
    assert(rte_is_zero_ether_addr(&arp->arp_data.arp_tha));
    const unsigned char *data = (const unsigned char *)eth;
    for (unsigned i = 42; i < 60; i++) {
        assert(data[i] == 0);
    }
    rte_pktmbuf_free(packet);
}

int main(int argc, char **argv)
{
    assert(rte_eal_init(argc, argv) >= 0);
    struct rte_mempool *pool = rte_pktmbuf_pool_create(
        "state_test", 63, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    assert(pool);
    const uint64_t hz = rte_get_timer_hz();
    const uint64_t now = 100 * hz;
    const rte_be32_t ip = rte_cpu_to_be_32(RTE_IPV4(192, 0, 2, 1));
    struct rte_ether_addr found = own;
    arp_init(&own);
    assert(arp_resolve(ip, &found, now) == ARP_PENDING);
    assert(rte_is_same_ether_addr(&found, &own));
    query(pool, ip, now);
    assert(arp_poll(pool, now + hz / 2) == NULL);
    struct rte_mbuf *packet = answer(pool, ip);
    struct rte_arp_hdr *arp = rte_pktmbuf_mtod_offset(
        packet, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));
    arp->arp_data.arp_tha = peer;
    assert(arp_input(packet, now) == ARP_IGNORED);
    arp->arp_data.arp_tha = own;
    assert(arp_input(packet, now) == ARP_LEARNED);
    assert(arp_input(packet, now) == ARP_IGNORED); /* 非等待状态不接受应答。 */
    rte_pktmbuf_free(packet);
    assert(arp_resolve(ip, &found, now + 29 * hz) == ARP_REACHABLE);
    assert(rte_is_same_ether_addr(&found, &peer));
    assert(arp_poll(pool, now + 29 * hz) == NULL);
    puts("PASS cache hit: resolved MAC reused, no new request");
    assert(arp_resolve(ip, &found, now + 30 * hz) == ARP_PENDING);
    query(pool, ip, now + 30 * hz);
    puts("PASS cache expiry: 30 seconds, next lookup requests again");

    arp_init(&own);
    assert(arp_resolve(ip, &found, now) == ARP_PENDING);
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        query(pool, ip, now + attempt * hz);
        assert(arp_resolve(ip, &found, now + attempt * hz) == ARP_PENDING);
        assert(arp_poll(pool, now + attempt * hz + hz / 2) == NULL);
    }
    assert(arp_poll(pool, now + 3 * hz) == NULL);
    assert(arp_resolve(ip, &found, now + 3 * hz) == ARP_FAILED);
    assert(arp_resolve(ip, &found, now + 4 * hz) == ARP_PENDING);
    puts("PASS retries: three attempts, final wait, failure and cooldown");

    arp_init(&own);
    for (unsigned host = 10; host < 26; host++) {
        rte_be32_t target = rte_cpu_to_be_32(RTE_IPV4(192, 0, 2, host));
        assert(arp_resolve(target, &found, now) == ARP_PENDING);
    }
    assert(arp_resolve(ip, &found, now) == ARP_FAILED);
    puts("PASS full table: seventeenth pending neighbor rejected");
    const uint32_t invalid[] = {0, SERVER_IP, RTE_IPV4(192, 0, 2, 0),
                               RTE_IPV4(192, 0, 2, 255), RTE_IPV4(198, 51, 100, 1)};
    arp_init(&own);
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(arp_resolve(rte_cpu_to_be_32(invalid[i]), &found, now) == ARP_FAILED);
    }
    puts("PASS query boundary: only other unicast peers on local subnet");

    /* 内存池耗尽也不能把主循环卡住，失败尝试仍有上限。 */
    struct rte_mbuf *held[63];
    for (unsigned i = 0; i < 63; i++) {
        held[i] = rte_pktmbuf_alloc(pool);
        assert(held[i]);
    }
    assert(arp_resolve(ip, &found, now) == ARP_PENDING);
    for (unsigned i = 0; i <= 3; i++) {
        assert(arp_poll(pool, now + i * hz) == NULL);
    }
    assert(arp_resolve(ip, &found, now + 3 * hz) == ARP_FAILED);
    for (unsigned i = 0; i < 63; i++) {
        rte_pktmbuf_free(held[i]);
    }
    puts("PASS pool exhaustion: bounded attempts and no leaked mbuf");

    arp_init(&own);
    packet = answer(pool, ip);
    arp = rte_pktmbuf_mtod_offset(packet, struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
    assert(arp_input(packet, now) == ARP_REPLY_READY);
    rte_pktmbuf_free(packet);
    assert(arp_resolve(ip, &found, now) == ARP_REACHABLE);
    puts("PASS request learning: sender cached while preparing reply");
    assert(rte_mempool_in_use_count(pool) == 0);
    rte_mempool_free(pool);
    assert(rte_eal_cleanup() == 0);
    puts("PASS state tests: mbufs_in_use=0");
    return 0;
}
