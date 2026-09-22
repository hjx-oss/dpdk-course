#include "udp.h"
#include "net_config.h"

#include <netinet/in.h>
#include <stdio.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

bool udp_reply(struct rte_mbuf *packet, struct rte_ipv4_hdr *ip)
{
    /* IPv4 层已经检查版本、目的地址、分片、总长度和头部校验和。 */
    uint16_t ip_length = rte_be_to_cpu_16(ip->total_length);
    if (ip_length < sizeof(*ip) + sizeof(struct rte_udp_hdr)) {
        return false;
    }
    struct rte_ether_hdr *ethernet = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
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
