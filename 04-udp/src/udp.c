#include "udp.h"

#include <netinet/in.h>
#include <stdio.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

#define SERVER_IP RTE_IPV4(192, 0, 2, 2)
#define SERVER_PORT 9000

bool udp_reply(struct rte_mbuf *packet)
{
    /* 后面要直接读写三个头部，先保证字节连续，且至少容得下这些头。 */
    if (rte_pktmbuf_linearize(packet) < 0) {
        return false;
    }

    const uint32_t minimum_length = sizeof(struct rte_ether_hdr)
                                  + sizeof(struct rte_ipv4_hdr)
                                  + sizeof(struct rte_udp_hdr);
    if (rte_pktmbuf_pkt_len(packet) < minimum_length) {
        return false;
    }

    struct rte_ether_hdr *ethernet = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
    if (ethernet->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        return false;
    }

    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(ethernet + 1);
    /* 本集只接收固定 20 字节 IPv4 头；选项和分片留到相应课程。 */
    if (ip->version_ihl != 0x45 || ip->next_proto_id != IPPROTO_UDP) {
        return false;
    }
    if (ip->dst_addr != rte_cpu_to_be_32(SERVER_IP)) {
        return false;
    }
    uint16_t fragments = rte_be_to_cpu_16(ip->fragment_offset);
    if ((fragments & (RTE_IPV4_HDR_MF_FLAG | RTE_IPV4_HDR_OFFSET_MASK)) != 0) {
        return false;
    }

    uint16_t ip_length = rte_be_to_cpu_16(ip->total_length);
    if (ip_length < sizeof(*ip) + sizeof(struct rte_udp_hdr)
        || sizeof(*ethernet) + ip_length > rte_pktmbuf_pkt_len(packet)) {
        return false;
    }
    if (rte_ipv4_cksum(ip) != 0) {
        return false;
    }

    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
    uint16_t udp_length = rte_be_to_cpu_16(udp->dgram_len);
    if (udp->dst_port != rte_cpu_to_be_16(SERVER_PORT)
        || udp_length < sizeof(*udp)
        || udp_length != ip_length - sizeof(*ip)) {
        return false;
    }
    /* IPv4 的 UDP 校验和为 0 表示未提供；非零时必须检查。 */
    if (udp->dgram_cksum != 0 && rte_ipv4_udptcp_cksum_verify(ip, udp) != 0) {
        return false;
    }

    /* 回显内容不变，只把三个层次的源和目的对调。 */
    struct rte_ether_addr peer_mac = ethernet->src_addr;
    ethernet->src_addr = ethernet->dst_addr;
    ethernet->dst_addr = peer_mac;

    rte_be32_t peer_ip = ip->src_addr;
    ip->src_addr = ip->dst_addr;
    ip->dst_addr = peer_ip;
    ip->time_to_live = 64;

    rte_be16_t peer_port = udp->src_port;
    udp->src_port = udp->dst_port;
    udp->dst_port = peer_port;

    /* 头部已经变化。先清零旧值，再用新头部重新计算。 */
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);
    udp->dgram_cksum = 0;
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);

    printf("UDP echo: payload=%u bytes, %u -> %u\n",
           (unsigned)(udp_length - sizeof(*udp)),
           rte_be_to_cpu_16(udp->src_port),
           rte_be_to_cpu_16(udp->dst_port));
    return true;
}
