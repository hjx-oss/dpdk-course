#include "ipv4.h"
#include "icmp.h"
#include "net_config.h"
#include "udp.h"

#include <netinet/in.h>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>

bool ipv4_reply(struct rte_mbuf *packet)
{
    if (rte_pktmbuf_linearize(packet) < 0) {
        return false;
    }
    const uint32_t minimum_length = sizeof(struct rte_ether_hdr)
                                    + sizeof(struct rte_ipv4_hdr);
    if (rte_pktmbuf_pkt_len(packet) < minimum_length) {
        return false;
    }

    struct rte_ether_hdr *ethernet = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    if (ethernet->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        return false;
    }
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(ethernet + 1);
    if (ip->version_ihl != 0x45
        || ip->dst_addr != rte_cpu_to_be_32(SERVER_IP)) {
        return false;
    }
    uint16_t fragments = rte_be_to_cpu_16(ip->fragment_offset);
    if ((fragments & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
        return false;
    }
    uint16_t ip_length = rte_be_to_cpu_16(ip->total_length);
    if (ip_length < sizeof(*ip)
        || sizeof(*ethernet) + ip_length > rte_pktmbuf_pkt_len(packet)) {
        return false;
    }
    if (rte_ipv4_cksum(ip) != 0) {
        return false;
    }

    /* 两种协议共用前面的 IPv4 检查，各自检查自己的头部。 */
    if (ip->next_proto_id == IPPROTO_UDP) {
        return udp_reply(packet, ip);
    }
    if (ip->next_proto_id == IPPROTO_ICMP) {
        return icmp_reply(packet, ip);
    }
    return false;
}
