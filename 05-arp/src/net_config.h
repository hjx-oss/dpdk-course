#ifndef NET_CONFIG_H
#define NET_CONFIG_H

#include <rte_ip.h>

/* ARP 和 UDP 必须使用同一个本机 IPv4 地址。 */
#define SERVER_IP RTE_IPV4(192, 0, 2, 2)
#define SERVER_PORT 9000
#define NETMASK RTE_IPV4(255, 255, 255, 0)

#endif
