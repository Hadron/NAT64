#ifndef _JOOL_MOD_TYPES_MOD_H
#define _JOOL_MOD_TYPES_MOD_H

#include "nat64/comm/types.h"
#include <linux/netfilter.h>


/**
 * Messages to help us walk through a run. Also covers normal packet drops (bad checksums,
 * bogus addresses, etc) and failed memory allocations (because the kernel already prints those).
 */
#define log_debug(text, ...) pr_debug(text "\n", ##__VA_ARGS__)
/** Responses to events triggered by the user, which might not show signs of life elsehow. */
#define log_info(text, ...) pr_info(text "\n", ##__VA_ARGS__)
/**
 * "I'm dropping a packet because the config's flipped out."
 * These rate limit themselves so the log doesn't get too flooded.
 */
#define log_warn_once(text, ...) \
	do { \
		static bool __logged = false; \
		static unsigned long __last_log; \
		\
		if (!__logged || __last_log < jiffies - msecs_to_jiffies(60 * 1000)) { \
			pr_warn(MODULE_NAME " WARNING (%s): " text "\n", __func__, ##__VA_ARGS__); \
			__logged = true; \
			__last_log = jiffies; \
		} \
	} while (0)
/**
 * "Your configuration cannot be applied, user."
 * log_warn() signals errors while processing packets. log_err() signals errors while processing
 * user requests.
 * I the code found a **programming** error, use WARN() or its variations instead.
 */
#define log_err(text, ...) pr_err(MODULE_NAME " ERROR (%s): " text "\n", __func__, ##__VA_ARGS__)
/**
 * This is intended to be equivalent to WARN(), except it's silent if you're unit testing.
 * Do this when you're testing errors being caught correctly and don't want dumped stacks on the
 * log.
 */
#ifdef UNIT_TESTING
	#define WARN_IF_REAL(condition, format...) condition
#else
	#define WARN_IF_REAL(condition, format...) WARN(condition, format)
#endif


/**
 * An indicator of what a function expects its caller to do with the packet being translated.
 */
typedef enum verdict {
	/** "No problems thus far, processing of the packet can continue." */
	VER_CONTINUE = -1,
	/** "Packet is not meant for translation. Please hand it to the local host." */
	VER_ACCEPT = NF_ACCEPT,
	/**
	 * "Packet is invalid and should be silently dropped."
	 * (Or "packet is invalid and I already sent a ICMP error, so just kill it".)
	 */
	VER_DROP = NF_DROP,
	/*
	 * "Packet is a fragment, and I need more information to be able to translate it, so I'll keep
	 * it for a while. Do not free, access or modify it."
	 */
	VER_STOLEN = NF_STOLEN,
} verdict;

/**
 * A tuple is sort of a summary of a packet; it is a quick accesor for several of its key elements.
 *
 * Keep in mind that the tuple's values do not always come from places you'd normally expect.
 * Unless you know ICMP errors are not involved, if the RFC says "the tuple's source address",
 * then you *MUST* extract the address from the tuple, not from the packet.
 * Conversely, if it says "the packet's source address", then *DO NOT* extract it from the tuple
 * for convenience. See comments inside for more info.
 */
struct tuple {
	/**
	 * Most of the time, this is the packet's _source_ address and layer-4 identifier. When the
	 * packet contains a inner packet, this is the inner packet's _destination_ address and l4 id.
	 */
	struct tuple_addr src;
	/**
	 * Most of the time, this is the packet's _destination_ address and layer-4 identifier. When
	 * the packet contains a inner packet, this is the inner packet's _source_ address and l4 id.
	 */
	struct tuple_addr dst;
	/** The packet's network protocol. */
	l3_protocol l3_proto;
	/**
	 * The packet's transport protocol that counts.
	 *
	 * Most of the time, this is the packet's simple l4-protocol. When the packet contains a inner
	 * packet, this is the inner packet's l4-protocol.
	 * Also, keep in mind that tuples represent whole packets, not fragments. If a packet's
	 * fragment offset is not zero, then its layer 4 protocol will be L4PROTO_NONE, but its tuple's
	 * l4_proto will be something else.
	 */
	l4_protocol l4_proto;

/**
 * By the way: There's code that depends on src.l4_id containing the same value as dst.l4_id when
 * l4_proto == L4PROTO_ICMP (i. e. 3-tuples).
 */
#define icmp_id src.l4_id
};

/**
 * Returns true if "tuple" represents a '3-tuple' (address-address-ICMP id), as defined by the RFC.
 */
static inline bool is_3_tuple(struct tuple *tuple)
{
	return (tuple->l4_proto == L4PROTO_ICMP);
}

/**
 * Returns true if "tuple" represents a '5-tuple' (address-port-address-port-transport protocol),
 * as defined by the RFC.
 */
static inline bool is_5_tuple(struct tuple *tuple)
{
	return !is_3_tuple(tuple);
}

/**
 * Prints "tuple" pretty in the log.
 */
void log_tuple(struct tuple *tuple);

/**
 * @{
 * Returns "true" if the first parameter is the same as the second one, even if they are pointers
 * to different places in memory.
 *
 * @param a struct you want to compare to "b".
 * @param b struct you want to compare to "a".
 * @return (*addr_1) === (*addr_2), with null checks as appropriate.
 */
bool ipv4_addr_equals(struct in_addr *a, struct in_addr *b);
bool ipv6_addr_equals(struct in6_addr *a, struct in6_addr *b);
bool ipv4_tuple_addr_equals(struct ipv4_tuple_address *a, struct ipv4_tuple_address *b);
bool ipv6_tuple_addr_equals(struct ipv6_tuple_address *a, struct ipv6_tuple_address *b);
bool ipv6_prefix_equals(struct ipv6_prefix *a, struct ipv6_prefix *b);
/**
 * @}
 */

/**
 * The kernel has a ipv6_addr_cmp(), but not a ipv4_addr_cmp().
 * Of course, that is because in_addrs are, to most intents and purposes, 32-bit integer values.
 * But the absence of ipv4_addr_cmp() does makes things look asymmetric.
 * So, booya.
 *
 * @return positive if a2 is bigger, negative if a1 is bigger, zero it they're equal.
 */
static inline int ipv4_addr_cmp(const struct in_addr *a1, const struct in_addr *a2)
{
	return memcmp(a1, a2, sizeof(struct in_addr));
}

/**
 * @{
 * Returns true if "type" (which is assumed to have been extracted from a ICMP header) represents
 * a packet involved in a ping.
 */
bool is_icmp6_info(__u8 type);
bool is_icmp4_info(__u8 type);
/**
 * @}
 */

/**
 * @{
 * Returns true if "type" (which is assumed to have been extracted from a ICMP header) represents
 * a packet which is an error response.
 */
bool is_icmp6_error(__u8 type);
bool is_icmp4_error(__u8 type);
/**
 * @}
 */


#endif /* _JOOL_MOD_TYPES_MOD_H */
