#include "arp.h"
#include "net_config.h"

#include <rte_arp.h>
#include <rte_cycles.h>
#include <string.h>

#define ARP_CAPACITY 16
#define ARP_CACHE_SECONDS 30
#define ARP_RETRY_SECONDS 1
#define ARP_MAX_ATTEMPTS 3

struct arp_entry {
    rte_be32_t ip;
    struct rte_ether_addr mac;
    enum arp_state state;
    unsigned attempts;
    uint64_t deadline;
};

static struct arp_entry neighbors[ARP_CAPACITY];
static struct rte_ether_addr port_mac;

void arp_init(const struct rte_ether_addr *local_mac)
{
    memset(neighbors, 0, sizeof(neighbors));
    port_mac = *local_mac;
}

static bool valid_peer(rte_be32_t ip)
{
    uint32_t address = rte_be_to_cpu_32(ip);
    uint32_t host = address & ~NETMASK;
    return (address & NETMASK) == (SERVER_IP & NETMASK) && host != 0 &&
           host != ~NETMASK && address != SERVER_IP;
}

static void expire_entries(uint64_t now)
{
    for (unsigned index = 0; index < ARP_CAPACITY; index++) {
        struct arp_entry *entry = &neighbors[index];
        if ((entry->state == ARP_REACHABLE || entry->state == ARP_FAILED) &&
            now >= entry->deadline) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static struct arp_entry *find_entry(rte_be32_t ip)
{
    for (unsigned index = 0; index < ARP_CAPACITY; index++) {
        if (neighbors[index].state != ARP_EMPTY && neighbors[index].ip == ip) {
            return &neighbors[index];
        }
    }
    return NULL;
}

static struct arp_entry *empty_entry(void)
{
    for (unsigned index = 0; index < ARP_CAPACITY; index++) {
        if (neighbors[index].state == ARP_EMPTY) {
            return &neighbors[index];
        }
    }
    return NULL;
}

enum arp_state arp_resolve(rte_be32_t ip, struct rte_ether_addr *mac, uint64_t now)
{
    expire_entries(now);
    if (!valid_peer(ip)) {
        return ARP_FAILED;
    }
    struct arp_entry *entry = find_entry(ip);

    if (entry == NULL) {
        entry = empty_entry();
        if (entry == NULL) {
            return ARP_FAILED;
        }
        memset(entry, 0, sizeof(*entry));
        entry->ip = ip;
        entry->state = ARP_PENDING;
        entry->deadline = now;
    }

    if (entry->state == ARP_REACHABLE) {
        *mac = entry->mac;
    }
    return entry->state;
}

static struct rte_mbuf *make_request(struct rte_mempool *pool, rte_be32_t ip)
{
    struct rte_mbuf *packet = rte_pktmbuf_alloc(pool);
    if (packet == NULL) {
        return NULL;
    }
    char *data = rte_pktmbuf_append(packet, RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN);
    if (data == NULL) {
        rte_pktmbuf_free(packet);
        return NULL;
    }

    memset(data, 0, RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN);
    struct rte_ether_hdr *ethernet = (struct rte_ether_hdr *)data;
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(ethernet + 1);
    memset(ethernet->dst_addr.addr_bytes, 0xff, RTE_ETHER_ADDR_LEN);
    ethernet->src_addr = port_mac;
    ethernet->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp->arp_plen = sizeof(rte_be32_t);
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);

    arp->arp_data.arp_sha = port_mac;
    arp->arp_data.arp_sip = rte_cpu_to_be_32(SERVER_IP);
    arp->arp_data.arp_tip = ip;
    return packet;
}

struct rte_mbuf *arp_poll(struct rte_mempool *pool, uint64_t now)
{
    expire_entries(now);
    for (unsigned index = 0; index < ARP_CAPACITY; index++) {
        struct arp_entry *entry = &neighbors[index];
        if (entry->state != ARP_PENDING || now < entry->deadline) {
            continue;
        }

        if (entry->attempts >= ARP_MAX_ATTEMPTS) {
            entry->state = ARP_FAILED;
            entry->deadline = now + rte_get_timer_hz();
            continue;
        }

        entry->attempts++;
        entry->deadline = now + ARP_RETRY_SECONDS * rte_get_timer_hz();
        return make_request(pool, entry->ip);
    }
    return NULL;
}

enum arp_result arp_input(struct rte_mbuf *packet, uint64_t now)
{
    if (rte_pktmbuf_linearize(packet) < 0) {
        return ARP_IGNORED;
    }
    const uint16_t header_length =
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    if (rte_pktmbuf_pkt_len(packet) < header_length) {
        return ARP_IGNORED;
    }

    struct rte_ether_hdr *ethernet = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(ethernet + 1);
    if (ethernet->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
        return ARP_IGNORED;
    }

    if (arp->arp_hardware != rte_cpu_to_be_16(RTE_ARP_HRD_ETHER) ||
        arp->arp_protocol != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
        arp->arp_hlen != RTE_ETHER_ADDR_LEN || arp->arp_plen != sizeof(rte_be32_t) ||
        arp->arp_data.arp_tip != rte_cpu_to_be_32(SERVER_IP)) {
        return ARP_IGNORED;
    }

    if ((!rte_is_broadcast_ether_addr(&ethernet->dst_addr) &&
         !rte_is_same_ether_addr(&ethernet->dst_addr, &port_mac)) ||
        !rte_is_valid_assigned_ether_addr(&arp->arp_data.arp_sha) ||
        !rte_is_same_ether_addr(&ethernet->src_addr, &arp->arp_data.arp_sha)) {
        return ARP_IGNORED;
    }

    uint16_t operation = rte_be_to_cpu_16(arp->arp_opcode);
    if (operation != RTE_ARP_OP_REQUEST && operation != RTE_ARP_OP_REPLY) {
        return ARP_IGNORED;
    }
    struct rte_ether_addr peer_mac = arp->arp_data.arp_sha;
    rte_be32_t peer_ip = arp->arp_data.arp_sip;
    expire_entries(now);
    struct arp_entry *entry = find_entry(peer_ip);

    if (operation == RTE_ARP_OP_REPLY &&
        (entry == NULL || entry->state != ARP_PENDING ||
         !rte_is_same_ether_addr(&ethernet->dst_addr, &port_mac) ||
         !rte_is_same_ether_addr(&arp->arp_data.arp_tha, &port_mac))) {
        return ARP_IGNORED;
    }

    if (valid_peer(peer_ip)) {
        if (entry == NULL) {
            entry = empty_entry();
        }
        if (entry != NULL) {
            entry->ip = peer_ip;
            entry->mac = peer_mac;

            entry->state = ARP_REACHABLE;
            entry->attempts = 0;
            entry->deadline = now + ARP_CACHE_SECONDS * rte_get_timer_hz();
        }
    }
    if (operation == RTE_ARP_OP_REPLY) {
        return ARP_LEARNED;
    }

    ethernet->dst_addr = peer_mac;
    ethernet->src_addr = port_mac;
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
    arp->arp_data.arp_tha = peer_mac;
    arp->arp_data.arp_tip = peer_ip;
    arp->arp_data.arp_sha = port_mac;
    arp->arp_data.arp_sip = rte_cpu_to_be_32(SERVER_IP);

    uint16_t excess = rte_pktmbuf_pkt_len(packet) - header_length;
    if (rte_pktmbuf_trim(packet, excess) < 0) {
        return ARP_IGNORED;
    }
    const uint16_t padding_length = RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN - header_length;

    char *padding = rte_pktmbuf_append(packet, padding_length);
    if (padding == NULL) {
        return ARP_IGNORED;
    }
    memset(padding, 0, padding_length);
    return ARP_REPLY_READY;
}
