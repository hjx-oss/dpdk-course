#include "icmp.h"

#include <string.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_icmp.h>

bool icmp_reply(struct rte_mbuf *packet, struct rte_ipv4_hdr *ip)
{
    uint16_t ip_length = rte_be_to_cpu_16(ip->total_length);
    uint16_t icmp_length = ip_length - sizeof(*ip);
    if (icmp_length < sizeof(struct rte_icmp_hdr)) {
        return false;
    }
    struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(ip + 1);
    if (icmp->icmp_type != RTE_ICMP_TYPE_ECHO_REQUEST || icmp->icmp_code != 0) {
        return false;
    }
    if (rte_raw_cksum(icmp, icmp_length) != UINT16_MAX) {
        return false;
    }

    /* 回显保留 identifier、sequence 和正文，只改变类型及校验和。 */
    icmp->icmp_type = RTE_ICMP_TYPE_ECHO_REPLY;
    icmp->icmp_cksum = 0;
    icmp->icmp_cksum = (uint16_t)~rte_raw_cksum(icmp, icmp_length);

    struct rte_ether_hdr *ethernet = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    struct rte_ether_addr peer_mac = ethernet->src_addr;
    ethernet->src_addr = ethernet->dst_addr;
    ethernet->dst_addr = peer_mac;
    rte_be32_t peer_ip = ip->src_addr;
    ip->src_addr = ip->dst_addr;
    ip->dst_addr = peer_ip;
    ip->time_to_live = 64;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    /* IP total_length 不含以太网填充；回复尾部统一重新补零。 */
    uint16_t frame_length = sizeof(*ethernet) + ip_length;
    uint16_t excess = rte_pktmbuf_pkt_len(packet) - frame_length;
    if (rte_pktmbuf_trim(packet, excess) < 0) {
        return false;
    }
    uint16_t minimum_frame = RTE_ETHER_MIN_LEN - RTE_ETHER_CRC_LEN;
    if (frame_length < minimum_frame) {
        uint16_t padding_length = minimum_frame - frame_length;
        char *padding = rte_pktmbuf_append(packet, padding_length);
        if (padding == NULL) {
            return false;
        }
        memset(padding, 0, padding_length);
    }
    return true;
}
