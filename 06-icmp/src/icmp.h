#ifndef ICMP_H
#define ICMP_H

#include <stdbool.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

/* 由 ipv4_reply 传入已检查的连续 IPv4 报文；不转移 mbuf 所有权。 */
bool icmp_reply(struct rte_mbuf *packet, struct rte_ipv4_hdr *ip);

#endif
