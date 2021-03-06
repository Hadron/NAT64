#ifndef _JOOL_MOD_POOL6_H
#define _JOOL_MOD_POOL6_H

/**
 * @file
 * The pool of IPv6 prefixes.
 *
 * @author Alberto Leiva
 */

#include <linux/types.h>
#include <linux/in6.h>
#include "nat64/comm/types.h"
#include "nat64/comm/config_proto.h"

/**
 * Readies the rest of this module for future use.
 *
 * @param pref_strs array of strings denoting the prefixes the pool should start with.
 * @param pref_count size of the "pref_strs" array.
 * @return result status (< 0 on error).
 */
int pool6_init(char *pref_strs[], int pref_count);
/**
 * Frees resources allocated by the pool.
 */
void pool6_destroy(void);
/**
 * Removes all prefixes from the pool.
 */
int pool6_flush(void);

/**
 * Returns (in "prefix") the pool's prefix corresponding to "addr".
 *
 * Because you're not actually borrowing the prefix,
 * - you don't have to return it, and
 * - this function can also be described as a way to infer "addr"'s actual network prefix.
 */
int pool6_get(struct in6_addr *addr, struct ipv6_prefix *prefix);
/**
 * Returns (in "result") any prefix from the pool.
 */
int pool6_peek(struct ipv6_prefix *result);
/**
 * Returns whether "addr"'s network prefix belongs to the pool.
 */
bool pool6_contains(struct in6_addr *addr);

/**
 * Adds "prefix" to the pool. A copy is stored, not "prefix" itself.
 */
int pool6_add(struct ipv6_prefix *prefix);
/**
 * Removes "prefix" from the pool.
 */
int pool6_remove(struct ipv6_prefix *prefix);

/**
 * Executes the "func" function with the "arg" argument on every prefix in the pool.
 */
int pool6_for_each(int (*func)(struct ipv6_prefix *, void *), void * arg);
/**
 * Copies the current number of prefixes in the pool to "result".
 */
int pool6_count(__u64 *result);

#endif /* _JOOL_MOD_POOL6_H */
