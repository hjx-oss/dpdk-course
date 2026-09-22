#ifndef UDP_H
#define UDP_H

#include <stdbool.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

/* IPv4 层先检查报文；本函数检查 UDP 并改成回复，不转移 mbuf 所有权。 */
bool udp_reply(struct rte_mbuf *packet, struct rte_ipv4_hdr *ip);

#endif
