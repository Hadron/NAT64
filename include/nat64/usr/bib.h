#ifndef _JOOL_USR_BIB_H
#define _JOOL_USR_BIB_H

#include "nat64/comm/types.h"


int bib_display(bool use_tcp, bool use_udp, bool use_icmp, bool numeric_hostname);
int bib_count(bool use_tcp, bool use_udp, bool use_icmp);

int bib_add(bool use_tcp, bool use_udp, bool use_icmp, struct ipv6_tuple_address *ipv6,
		struct ipv4_tuple_address *ipv4);

int bib_remove_ipv6(bool use_tcp, bool use_udp, bool use_icmp, struct ipv6_tuple_address *ipv6);
int bib_remove_ipv4(bool use_tcp, bool use_udp, bool use_icmp, struct ipv4_tuple_address *ipv4);


#endif /* _JOOL_USR_BIB_H */
