#ifndef _JOOL_MOD_RFC6052_H
#define _JOOL_MOD_RFC6052_H

/**
 * @file
 * The algorithm defined in RFC 6052 (http://tools.ietf.org/html/rfc6052).
 *
 * @author Ramiro Nava
 * @author Alberto Leiva
 */

#include <linux/types.h>
#include <linux/in.h>
#include <linux/in6.h>
#include "nat64/comm/types.h"


/**
 * Translates "src" into a IPv4 address and returns it as "dst".
 *
 * In other words, removes "prefix" from "src". The result will be 32 bits of address.
 * You want to extract "prefix" from the IPv6 pool somehow.
 *
 * @return error status.
 */
int addr_6to4(struct in6_addr *src, struct ipv6_prefix *prefix, struct in_addr *dst);

/**
 * Translates "src" into a IPv6 address and returns it as "dst.
 *
 * In other words, adds "prefix" to "src". The result will be 128 bits of address.
 * You want to extract "prefix" from the IPv6 pool somehow.
 *
 * @return error status.
 */
int addr_4to6(struct in_addr *src, struct ipv6_prefix *prefix, struct in6_addr *dst);


#endif /* _JOOL_MOD_RFC6052_H */
