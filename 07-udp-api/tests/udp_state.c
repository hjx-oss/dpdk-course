#include "udp.h"
#include "udp_internal.h"
#include "arp.h"
#include "ipv4.h"
#include "net_config.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_udp.h>

static struct rte_mempool *pool;
static const struct rte_ether_addr local = {{2, 0, 0, 0, 0, 2}};
static const struct rte_ether_addr remote = {{2, 0, 0, 0, 0, 1}};
static struct udp_address peer = {RTE_IPV4(192, 0, 2, 1), 40000};

static void learn(uint64_t now)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    assert(m);
    char *data = rte_pktmbuf_append(m, 60);
    assert(data);
    memset(data, 0, 60);
    struct rte_ether_hdr *eth = (void *)data;
    eth->src_addr = remote;
    memset(eth->dst_addr.addr_bytes, 255, 6);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);
    struct rte_arp_hdr *arp = (void *)(eth + 1);
    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = 6;
    arp->arp_plen = 4;
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
    arp->arp_data.arp_sha = remote;
    arp->arp_data.arp_sip = rte_cpu_to_be_32(peer.ip);
    arp->arp_data.arp_tip = rte_cpu_to_be_32(SERVER_IP);
    assert(arp_input(m, now) == ARP_REPLY_READY);
    rte_pktmbuf_free(m);
}

static enum ipv4_result receive(uint16_t port, const void *bytes, size_t length)
{
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    assert(m);
    size_t size = 14 + 20 + 8 + length;
    char *data = rte_pktmbuf_append(m, size);
    assert(data);
    memset(data, 0, size);
    struct rte_ether_hdr *eth = (void *)data;
    struct rte_ipv4_hdr *ip = (void *)(eth + 1);
    struct rte_udp_hdr *udp = (void *)(ip + 1);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    ip->version_ihl = 0x45;
    ip->total_length = rte_cpu_to_be_16(28 + length);
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = rte_cpu_to_be_32(peer.ip);
    ip->dst_addr = rte_cpu_to_be_32(SERVER_IP);
    ip->hdr_checksum = rte_ipv4_cksum(ip);
    udp->src_port = rte_cpu_to_be_16(peer.port);
    udp->dst_port = rte_cpu_to_be_16(port);
    udp->dgram_len = rte_cpu_to_be_16(8 + length);
    if (length) memcpy(udp + 1, bytes, length);
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
    enum ipv4_result result = ipv4_input(m);
    rte_pktmbuf_free(m);
    return result;
}

int main(int argc, char **argv)
{
    assert(rte_eal_init(argc, argv) >= 0);
    pool = rte_pktmbuf_pool_create("api_test", 127, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, 0);
    assert(pool);
    arp_init(&local);
    udp_init(pool, &local);
    int a = udp_socket(), b = udp_socket();
    assert(a >= 0 && b >= 0 && a != b);
    assert(udp_sendto(a, "x", 1, peer) == -1 && errno == EINVAL);
    assert(udp_bind(a, 9000) == 0);
    assert(udp_bind(b, 9000) == -1 && errno == EADDRINUSE);
    assert(udp_bind(b, 9001) == 0);
    assert(udp_bind(b, 9002) == -1 && errno == EINVAL);
    char data[UDP_MAX_PAYLOAD];
    struct udp_address from;
    assert(udp_recvfrom(a, data, sizeof(data), &from) == -1 && errno == EAGAIN);
    assert(udp_sendto(a, data, sizeof(data) + 1, peer) == -1 && errno == EMSGSIZE);
    struct udp_address outside = {RTE_IPV4(198, 51, 100, 1), 80};
    assert(udp_sendto(a, data, 1, outside) == -1 && errno == ENETUNREACH);
    puts("PASS API: invalid fd, bind conflict, empty queue and argument errors");

    assert(receive(9000, "abcdef", 6) == IPV4_CONSUMED);
    assert(receive(9001, "other", 5) == IPV4_CONSUMED);
    assert(udp_recvfrom(a, data, 2, &from) == 2 && memcmp(data, "ab", 2) == 0);
    assert(from.ip == peer.ip && from.port == peer.port);
    assert(udp_recvfrom(a, data, sizeof(data), NULL) == -1 && errno == EAGAIN);
    assert(udp_recvfrom(b, data, sizeof(data), NULL) == 5 && memcmp(data, "other", 5) == 0);
    assert(receive(9000, NULL, 0) == IPV4_CONSUMED);
    assert(udp_recvfrom(a, NULL, 0, NULL) == 0);
    assert(receive(9999, "x", 1) == IPV4_DROP);
    puts("PASS RX: port isolation, peer address, truncation and empty datagram");

    for (int i = 0; i < 8; i++) assert(receive(9000, &i, sizeof(i)) == IPV4_CONSUMED);
    assert(receive(9000, "full", 4) == IPV4_DROP);
    for (int i = 0; i < 8; i++) {
        int got = -1;
        assert(udp_recvfrom(a, &got, sizeof(got), NULL) == (ssize_t)sizeof(got) && got == i);
    }
    for (int i = 0; i < 64; i++) {
        assert(receive(9000, &i, sizeof(i)) == IPV4_CONSUMED);
        int got = -1;
        assert(udp_recvfrom(a, &got, sizeof(got), NULL) == (ssize_t)sizeof(got) && got == i);
    }
    puts("PASS RX: bounded queue, FIFO order and repeated wraparound");

    uint64_t now = rte_get_timer_cycles();
    struct udp_address absent = {RTE_IPV4(192, 0, 2, 99), 40000};
    for (int i = 0; i < 8; i++) assert(udp_sendto(a, "wait", 4, absent) == 4);
    assert(udp_sendto(a, "full", 4, peer) == -1 && errno == EAGAIN);
    int owner = -1;
    assert(udp_poll(now, &owner) == NULL);
    for (int i = 0; i < 4; i++) {
        struct rte_mbuf *request = arp_poll(pool, now + i * rte_get_timer_hz());
        assert((request != NULL) == (i < 3));
        if (request) rte_pktmbuf_free(request);
    }
    assert(udp_poll(now + 3 * rte_get_timer_hz(), &owner) == NULL);
    assert(udp_take_error(a) == EHOSTUNREACH && udp_take_error(a) == 0);
    assert(udp_close(a) == 0);
    assert(udp_sendto(a, "x", 1, peer) == -1 && errno == EBADF);
    puts("PASS TX: full queue, three ARP attempts, async failure and close with pending data");

    a = udp_socket();
    assert(udp_bind(a, 9000) == 0);
    arp_init(&local);
    learn(now);
    strcpy(data, "copy-me");
    assert(udp_sendto(a, data, 7, peer) == 7);
    memset(data, 'X', 7);
    struct rte_mbuf *packet = udp_poll(rte_get_timer_cycles(), &owner);
    assert(packet && owner == a);
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (void *)(eth + 1);
    struct rte_udp_hdr *udp = (void *)(ip + 1);
    assert(rte_is_same_ether_addr(&eth->src_addr, &local));
    assert(rte_is_same_ether_addr(&eth->dst_addr, &remote));
    assert(rte_be_to_cpu_16(ip->total_length) == 35 && rte_ipv4_cksum(ip) == 0);
    assert(rte_be_to_cpu_16(udp->src_port) == 9000 && rte_be_to_cpu_16(udp->dst_port) == peer.port);
    assert(rte_ipv4_udptcp_cksum_verify(ip, udp) == 0 && memcmp(udp + 1, "copy-me", 7) == 0);
    const uint8_t *bytes = rte_pktmbuf_mtod(packet, const uint8_t *);
    for (unsigned i = 49; i < 60; i++) assert(bytes[i] == 0);
    rte_pktmbuf_free(packet);
    puts("PASS TX: copied payload, ARP hit, fresh headers, checksums and zero padding");

    struct rte_mbuf *held[127];
    unsigned count = 0;
    while (count < 127 && (held[count] = rte_pktmbuf_alloc(pool)) != NULL) count++;
    assert(count == 127);
    assert(udp_sendto(a, "no-buffer", 9, peer) == 9);
    assert(udp_poll(rte_get_timer_cycles(), &owner) == NULL);
    assert(udp_take_error(a) == ENOBUFS);
    while (count) rte_pktmbuf_free(held[--count]);
    udp_report_error(a, EIO);
    assert(udp_take_error(a) == EIO && udp_take_error(a) == 0);
    assert(udp_sendto(a, "pending", 7, absent) == 7);
    assert(receive(9000, "unread", 6) == IPV4_CONSUMED);
    assert(udp_close(a) == 0 && udp_close(b) == 0);
    for (int i = 0; i < 4; i++) assert(udp_socket() >= 0);
    assert(udp_socket() == -1 && errno == EMFILE);
    udp_shutdown();
    assert(rte_mempool_in_use_count(pool) == 0);
    puts("PASS API: pool exhaustion, TX error delivery, socket limit and shutdown; mbufs_in_use=0");
    rte_mempool_free(pool);
    assert(rte_eal_cleanup() == 0);
    return 0;
}
