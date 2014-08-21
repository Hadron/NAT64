#include "nat64/mod/session_db.h"

#include <net/ipv6.h>
#include "nat64/comm/constants.h"
#include "nat64/mod/rbtree.h"
#include "nat64/mod/bib_db.h"
#include "nat64/mod/pool6.h"
#include "nat64/mod/rfc6052.h"
#include "nat64/mod/send_packet.h"

#include "session.c"

/********************************************
 * Structures and private variables.
 ********************************************/

/**
 * Session table definition.
 * Holds red-black trees, one for each indexing need (IPv4 and IPv6).
 */
struct session_table {
	/** Indexes the entries using their IPv6 identifiers. */
	struct rb_root tree6;
	/** Indexes the entries using their IPv4 identifiers. */
	struct rb_root tree4;
	/** Number of session entries in this table. */
	u64 count;
	/**
	 * Lock to sync access. This protects both the trees and the entries, but if you only need to
	 * read the static portion of the entries, you can get away with only incresing their reference
	 * counter.
	 * TODO (info) make those fields (ipv6, ipv4, l4_proto) explicitly constant. Do it on BIB too.
	 */
	spinlock_t lock;
};

/** The session table for UDP connections. */
static struct session_table session_table_udp;
/** The session table for TCP connections. */
static struct session_table session_table_tcp;
/** The session table for ICMP connections. */
static struct session_table session_table_icmp;

struct expire_timer {
	struct timer_list timer;
	struct list_head sessions;
	struct session_table *table;
	size_t timeout_offset;
};

/** Killer of sessions whose expiration date was initialized using "config".ttl.udp. */
static struct expire_timer expirer_udp;
/** Killer of sessions whose expiration date was initialized using "config".ttl.tcp_est. */
static struct expire_timer expirer_tcp_est;
/** Killer of sessions whose expiration date was initialized using "config".ttl.tcp_trans. */
static struct expire_timer expirer_tcp_trans;
/** Killer of sessions whose expiration date was initialized using "config".ttl.icmp. */
static struct expire_timer expirer_icmp;
/** Killer of sessions whose expiration date was initialized using "TCP_INCOMING_SYN". */
static struct expire_timer expirer_syn;

/** Current valid configuration for the Session DB module. */
static struct sessiondb_config *config;

/********************************************
 * Private (helper) functions.
 ********************************************/

/**
 * One-liner to get the session table corresponding to the "l4_proto" protocol.
 *
 * Doesn't care about spinlocks.
 */
static int get_session_table(l4_protocol l4_proto, struct session_table **result)
{
	switch (l4_proto) {
	case L4PROTO_UDP:
		*result = &session_table_udp;
		return 0;
	case L4PROTO_TCP:
		*result = &session_table_tcp;
		return 0;
	case L4PROTO_ICMP:
		*result = &session_table_icmp;
		return 0;
	case L4PROTO_NONE:
		WARN(true, "There is no session table for the 'NONE' protocol.");
		return -EINVAL;
	}

	WARN(true, "Unsupported transport protocol: %u.", l4_proto);
	return -EINVAL;
}

/**
 * Fills "pair" with "tuple"'s contents.
 *
 * Doesn't care about spinlocks.
 */
static void tuple_to_ipv6_pair(struct tuple *tuple, struct ipv6_pair *pair)
{
	pair->remote.address = tuple->src.addr.ipv6;
	pair->remote.l4_id = tuple->src.l4_id;
	pair->local.address = tuple->dst.addr.ipv6;
	pair->local.l4_id = tuple->dst.l4_id;
}

/**
 * Fills "pair" with "tuple"'s contents.
 *
 * Doesn't care about spinlocks.
 */
static void tuple_to_ipv4_pair(struct tuple *tuple, struct ipv4_pair *pair)
{
	pair->remote.address = tuple->src.addr.ipv4;
	pair->remote.l4_id = tuple->src.l4_id;
	pair->local.address = tuple->dst.addr.ipv4;
	pair->local.l4_id = tuple->dst.l4_id;
}

/**
 * Returns a positive integer if session.ipv6 < pair.
 * Returns a negative integer if session.ipv6 > pair.
 * Returns zero if session.ipv6 == pair.
 *
 * Doesn't care about spinlocks.
 */
static int compare_full6(struct session_entry *session, struct ipv6_pair *pair)
{
	int gap;

	gap = ipv6_addr_cmp(&pair->local.address, &session->ipv6.local.address);
	if (gap != 0)
		return gap;

	gap = ipv6_addr_cmp(&pair->remote.address, &session->ipv6.remote.address);
	if (gap != 0)
		return gap;

	gap = pair->local.l4_id - session->ipv6.local.l4_id;
	if (gap != 0)
		return gap;

	gap = pair->remote.l4_id - session->ipv6.remote.l4_id;
	return gap;
}

/**
 * Returns a positive integer if session.ipv4.local < addr.
 * Returns a negative integer if session.ipv4.local > addr.
 * Returns zero if session.ipv4.local == addr.
 *
 * Doesn't care about spinlocks.
 */
static int compare_local4(struct session_entry *session, struct ipv4_tuple_address *addr)
{
	int gap;

	gap = ipv4_addr_cmp(&addr->address, &session->ipv4.local.address);
	if (gap != 0)
		return gap;

	gap = addr->l4_id - session->ipv4.local.l4_id;
	return gap;
}

/**
 * Returns a positive integer if session.ipv4 < pair.
 * Returns a negative integer if session.ipv4 > pair.
 * Returns zero if session.ipv4 == pair.
 *
 * It excludes remote layer-4 IDs from the comparison. See session_allow() to find out why.
 *
 * Doesn't care about spinlocks.
 */
static int compare_addrs4(struct session_entry *session, struct ipv4_pair *pair)
{
	int gap;

	gap = compare_local4(session, &pair->local);
	if (gap != 0)
		return gap;

	gap = ipv4_addr_cmp(&pair->remote.address, &session->ipv4.remote.address);
	return gap;
}

/**
 * Returns a positive integer if session.ipv4 < pair.
 * Returns a negative integer if session.ipv4 > pair.
 * Returns zero if session.ipv4 == pair.
 *
 * Doesn't care about spinlocks.
 */
static int compare_full4(struct session_entry *session, struct ipv4_pair *pair)
{
	int gap;

	gap = compare_addrs4(session, pair);
	if (gap != 0)
		return gap;

	gap = pair->remote.l4_id - session->ipv4.remote.l4_id;
	return gap;
}

/**
 * Returns a positive integer if session.ipv4.local < addr.
 * Returns a negative integer if session.ipv4.local > addr.
 * Returns zero if session.ipv4.local == addr.
 *
 * Doesn't care about spinlocks.
 */
static int compare_local_addr4(struct session_entry *session, struct in_addr *addr)
{
	return ipv4_addr_cmp(addr, &session->ipv4.local.address);
}

/**
 * Sends a probe packet to "session"'s IPv6 endpoint, to trigger a confirmation ACK if the
 * connection is still alive.
 *
 * From RFC 6146 page 30.
 *
 * @param[in] session the established session that has been inactive for too long.
 *
 * Doesn't care about spinlocks, but "session" might.
 */
static void send_probe_packet(struct session_entry *session)
{
	struct sk_buff* skb;
	struct dst_entry *dst;
	struct ipv6hdr *iph;
	struct tcphdr *th;
	int error;

	unsigned int l3_hdr_len = sizeof(*iph);
	unsigned int l4_hdr_len = sizeof(*th);

	skb = alloc_skb(LL_MAX_HEADER + l3_hdr_len + l4_hdr_len, GFP_ATOMIC);
	if (!skb) {
		log_debug("Could now allocate a probe packet.");
		goto fail;
	}

	skb_reserve(skb, LL_MAX_HEADER);
	skb_put(skb, l3_hdr_len + l4_hdr_len);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, l3_hdr_len);

	iph = ipv6_hdr(skb);
	iph->version = 6;
	iph->priority = 0;
	iph->flow_lbl[0] = 0;
	iph->flow_lbl[1] = 0;
	iph->flow_lbl[2] = 0;
	iph->payload_len = cpu_to_be16(l4_hdr_len);
	iph->nexthdr = NEXTHDR_TCP;
	iph->hop_limit = 255;
	iph->saddr = session->ipv6.local.address;
	iph->daddr = session->ipv6.remote.address;

	th = tcp_hdr(skb);
	th->source = cpu_to_be16(session->ipv6.local.l4_id);
	th->dest = cpu_to_be16(session->ipv6.remote.l4_id);
	th->seq = htonl(0);
	th->ack_seq = htonl(0);
	th->res1 = 0;
	th->doff = l4_hdr_len / 4;
	th->fin = 0;
	th->syn = 0;
	th->rst = 0;
	th->psh = 0;
	th->ack = 1;
	th->urg = 0;
	th->ece = 0;
	th->cwr = 0;
	th->window = htons(8192);
	th->check = 0;
	th->urg_ptr = 0;

	th->check = csum_ipv6_magic(&iph->saddr, &iph->daddr, l4_hdr_len, IPPROTO_TCP,
			csum_partial(th, l4_hdr_len, 0));
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	dst = route_ipv6(iph, th, L4PROTO_TCP, 0);
	if (!dst) {
		log_debug("Could not route the probe packet.");
		goto fail;
	}
	skb->dev = dst->dev;
	skb_dst_set(skb, dst);

	error = ip6_local_out(skb);
	if (error) {
		log_debug("The kernel's packet dispatch function returned errcode %d.", error);
		goto fail;
	}

	return;

fail:
	log_debug("Looks like a TCP connection will break or remain idle forever somewhere...");
}

/**
 * Removes all of this database's references towards "session", and drops its refcount accordingly.
 *
 * The only thing it doesn't do is decrement count of "session"'s table! I do that outside because
 * I always want to add up and report that number.
 *
 * @return number of sessions removed from the database. This is always 1, because I have no way to
 *		know if the removal failed (and it shouldn't be possible anyway).
 *
 * "table"'s spinlock must already be held.
 */
static int remove(struct session_entry *session, struct session_table *table)
{
	rb_erase(&session->tree6_hook, &table->tree6);
	rb_erase(&session->tree4_hook, &table->tree4);

	/*
	 * The functions above don't clean the nodes,
	 * and other threads might be holding references to this session.
	 */
	RB_CLEAR_NODE(&session->tree6_hook);
	RB_CLEAR_NODE(&session->tree4_hook);

	list_del(&session->expire_list_hook);
	session_return(session);
	return 1;
}

/**
 * Wrapper for mod_timer().
 *
 * Not holding a spinlock is desirable for performance reasons (mod_timer() syncs itself).
 */
static void schedule_timer(struct timer_list *timer, unsigned long next_time)
{
	unsigned long min_next = jiffies + MIN_TIMER_SLEEP;

	if (time_before(next_time, min_next))
		next_time = min_next;

	mod_timer(timer, next_time);
	log_debug("A timer will awake in %u msecs.", jiffies_to_msecs(timer->expires - jiffies));
}

static unsigned long get_timeout(struct expire_timer *expirer)
{
	unsigned long timeout;

	if (expirer == &expirer_syn)
		return msecs_to_jiffies(1000 * TCP_INCOMING_SYN);

	rcu_read_lock_bh();
	timeout = *(expirer->timeout_offset + (__u64 *) rcu_dereference_bh(config));
	rcu_read_unlock_bh();

	return timeout;
}

int sessiondb_get_timeout(struct session_entry *session, unsigned long *result)
{
	if (!session->expirer) {
		log_debug("The session entry doesn't have an expirer");
		return -EINVAL;
	}

	*result = get_timeout(session->expirer);
	return 0;
}

/**
 * Decides whether "session"'s expiration should cause its destruction or not. It should be called
 * when "session" expires.
 *
 * If "session" should be destroyed, it'll return true.
 * If "session" should not be destroyed, it will update its lifetime and TCP state (if applies) and
 * will return false.
 *
 * @param[in] session The entry whose lifetime just expired.
 *
 * session's table's spinlock must already be held.
 */
static bool session_expire(struct session_entry *session)
{
	switch (session->l4_proto) {
	case L4PROTO_UDP:
	case L4PROTO_ICMP:
		return true;

	case L4PROTO_TCP:
		switch (session->state) {
		case V4_INIT:
			/* TODO (Issue #58) send the stored packet. */
			/* send_icmp_error_message(skb, DESTINATION_UNREACHABLE, ADDRESS_UNREACHABLE); */
			session->state = CLOSED;
			return true;

		case ESTABLISHED:
			send_probe_packet(session);

			session->state = TRANS;
			session->update_time = jiffies;

			list_del(&session->expire_list_hook);
			list_add_tail(&session->expire_list_hook, &expirer_tcp_trans.sessions);
			if (!timer_pending(&expirer_tcp_trans.timer))
				schedule_timer(&expirer_tcp_trans.timer, jiffies + get_timeout(&expirer_tcp_trans));

			return false;

		case V6_INIT:
		case V4_FIN_RCV:
		case V6_FIN_RCV:
		case V4_FIN_V6_FIN_RCV:
		case TRANS:
			session->state = CLOSED;
			return true;

		case CLOSED:
			/* Closed sessions are not supposed to be stored, so this is an error. */
			WARN(true, "Closed state found; removing session entry.");
			return true;
		}

		WARN(true, "Unknown state found (%d); removing session entry.", session->state);
		return true;

	case L4PROTO_NONE:
		WARN(true, "Invalid transport protocol: NONE.");
		return true;
	}

	WARN(true, "Unknown transport protocol: %u.", session->l4_proto);
	return true;
}

/**
 * Called once in a while to kick off the scheduled expired sessions massacre.
 *
 * In that sense, it's a public function, so it requires spinlocks to NOT be held.
 */
static void cleaner_timer(unsigned long param)
{
	struct expire_timer *expirer = (struct expire_timer *) param;
	struct list_head *current_hook, *next_hook;
	struct session_entry *session;
	unsigned long timeout;
	unsigned int s = 0;

	log_debug("===============================================");
	log_debug("Deleting expired sessions...");

	timeout = get_timeout(expirer);

	spin_lock_bh(&expirer->table->lock);

	list_for_each_safe(current_hook, next_hook, &expirer->sessions) {
		session = list_entry(current_hook, struct session_entry, expire_list_hook);

		if (time_before(jiffies, session->update_time + timeout)) {
			/* "list" is sorted by expiration date, so stop on the first unexpired session. */
			expirer->table->count -= s;
			spin_unlock_bh(&expirer->table->lock);
			log_debug("Deleted %u sessions.", s);
			schedule_timer(&expirer->timer, session->update_time + timeout);
			return;
		}

		if (!session_expire(session))
			continue; /* The entry's TTL changed, which doesn't mean the next one isn't expired. */

		s += remove(session, expirer->table);
	}

	expirer->table->count -= s;
	spin_unlock_bh(&expirer->table->lock);
	log_debug("Deleted %u sessions.", s);
}

/*******************************
 * Public functions.
 *******************************/

/**
 * Auxiliar for sessiondb_init(). Encapsulates initialization of an expire_timer structure.
 *
 * Doesn't care about spinlocks (initialization code doesn't share threads).
 */
static void init_expire_timer(struct expire_timer *expirer, struct session_table *table,
		size_t timeout_offset)
{
	init_timer(&expirer->timer);
	expirer->timer.function = cleaner_timer;
	expirer->timer.expires = 0;
	expirer->timer.data = (unsigned long) expirer;

	INIT_LIST_HEAD(&expirer->sessions);
	expirer->table = table;
	expirer->timeout_offset = timeout_offset;
}

int sessiondb_init(void)
{
	struct session_table *tables[] = { &session_table_udp, &session_table_tcp,
			&session_table_icmp };
	int i;
	int error;

	error = session_init();
	if (error)
		return error;

	config = kmalloc(sizeof(*config), GFP_ATOMIC);
	if (!config) {
		log_debug("Could not allocate memory to store the session DB config.");
		return -ENOMEM;
	}

	config->ttl.udp = msecs_to_jiffies(1000 * UDP_DEFAULT);
	config->ttl.icmp = msecs_to_jiffies(1000 * ICMP_DEFAULT);
	config->ttl.tcp_trans = msecs_to_jiffies(1000 * TCP_TRANS);
	config->ttl.tcp_est = msecs_to_jiffies(1000 * TCP_EST);

	for (i = 0; i < ARRAY_SIZE(tables); i++) {
		tables[i]->tree6 = RB_ROOT;
		tables[i]->tree4 = RB_ROOT;
		tables[i]->count = 0;
		spin_lock_init(&tables[i]->lock);
	}

	init_expire_timer(&expirer_udp, &session_table_udp,
			offsetof(struct sessiondb_config, ttl.udp));
	init_expire_timer(&expirer_tcp_est, &session_table_tcp,
			offsetof(struct sessiondb_config, ttl.tcp_est));
	init_expire_timer(&expirer_tcp_trans, &session_table_tcp,
			offsetof(struct sessiondb_config, ttl.tcp_trans));
	init_expire_timer(&expirer_syn, &session_table_tcp, 0);
	init_expire_timer(&expirer_icmp, &session_table_icmp,
			offsetof(struct sessiondb_config, ttl.icmp));

	return 0;
}

/**
 * Auxiliar for sessiondb_destroy(). Wraps the destruction of a session, exposing an API the rbtree
 * module wants.
 *
 * Doesn't care about spinlocks (destructor code doesn't share threads).
 */
static void session_destroy_aux(struct rb_node *node)
{
	session_kfree(rb_entry(node, struct session_entry, tree6_hook));
}

void sessiondb_destroy(void)
{
	struct session_table *tables[] = { &session_table_udp, &session_table_tcp,
			&session_table_icmp };
	int i;

	del_timer_sync(&expirer_udp.timer);
	del_timer_sync(&expirer_tcp_est.timer);
	del_timer_sync(&expirer_tcp_trans.timer);
	del_timer_sync(&expirer_syn.timer);
	del_timer_sync(&expirer_icmp.timer);

	log_debug("Emptying the session tables...");
	/*
	 * The values need to be released only in one of the trees
	 * because both trees point to the same values.
	 */
	for (i = 0; i < ARRAY_SIZE(tables); i++)
		rbtree_clear(&tables[i]->tree6, session_destroy_aux);

	session_destroy();
}

int sessiondb_clone_config(struct sessiondb_config *clone)
{
	rcu_read_lock_bh();
	*clone = *rcu_dereference_bh(config);
	rcu_read_unlock_bh();
	return 0;
}

int sessiondb_set_config(enum sessiondb_type type, size_t size, void *value)
{
	struct sessiondb_config *tmp_config;
	struct sessiondb_config *old_config;
	__u64 value64;
	__u32 max_u32 = 0xFFFFFFFFL; /* Max value in milliseconds */

	if (size != sizeof(__u64)) {
		log_err("Expected an 8-byte integer, got %zu bytes.", size);
		return -EINVAL;
	}
	value64 = *((__u64 *) value);
	if (value64 > max_u32) {
		log_err("Expected a timeout less than %u seconds", max_u32 / 1000);
		return -EINVAL;
	}

	value64 = msecs_to_jiffies(value64);

	tmp_config = kmalloc(sizeof(*tmp_config), GFP_KERNEL);
	if (!tmp_config)
		return -ENOMEM;

	old_config = config;
	*tmp_config = *old_config;

	switch (type) {
	case UDP_TIMEOUT:
		if (value64 < msecs_to_jiffies(1000 * UDP_MIN)) {
			log_err("The UDP timeout must be at least %u seconds.", UDP_MIN);
			goto fail;
		}
		tmp_config->ttl.udp = value64;
		break;
	case ICMP_TIMEOUT:
		tmp_config->ttl.icmp = value64;
		break;
	case TCP_EST_TIMEOUT:
		if (value64 < msecs_to_jiffies(1000 * TCP_EST)) {
			log_err("The TCP est timeout must be at least %u seconds.", TCP_EST);
			goto fail;
		}
		tmp_config->ttl.tcp_est = value64;
		break;
	case TCP_TRANS_TIMEOUT:
		if (value64 < msecs_to_jiffies(1000 * TCP_TRANS)) {
			log_err("The TCP trans timeout must be at least %u seconds.", TCP_TRANS);
			goto fail;
		}
		tmp_config->ttl.tcp_trans = value64;
		break;
	}

	rcu_assign_pointer(config, tmp_config);
	synchronize_rcu_bh();
	kfree(old_config);
	return 0;

fail:
	kfree(tmp_config);
	return -EINVAL;
}

int sessiondb_get_by_ipv4(struct ipv4_pair *pair, l4_protocol l4_proto,
		struct session_entry **result)
{
	struct session_table *table;
	int error;

	if (WARN(!pair, "The session tables cannot contain NULL."))
		return -EINVAL;
	error = get_session_table(l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->lock);
	*result = rbtree_find(pair, &table->tree4, compare_full4, struct session_entry, tree4_hook);
	if (*result)
		session_get(*result);
	spin_unlock_bh(&table->lock);

	return (*result) ? 0 : -ENOENT;
}

int sessiondb_get_by_ipv6(struct ipv6_pair *pair, l4_protocol l4_proto,
		struct session_entry **result)
{
	struct session_table *table;
	int error;

	if (WARN(!pair, "The session tables cannot contain NULL."))
		return -EINVAL;
	error = get_session_table(l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->lock);
	*result = rbtree_find(pair, &table->tree6, compare_full6, struct session_entry, tree6_hook);
	if (*result)
		session_get(*result);
	spin_unlock_bh(&table->lock);

	return (*result) ? 0 : -ENOENT;
}

int sessiondb_get(struct tuple *tuple, struct session_entry **result)
{
	struct ipv6_pair pair6;
	struct ipv4_pair pair4;

	if (WARN(!tuple, "There's no session entry mapped to NULL."))
		return -EINVAL;

	switch (tuple->l3_proto) {
	case L3PROTO_IPV6:
		tuple_to_ipv6_pair(tuple, &pair6);
		return sessiondb_get_by_ipv6(&pair6, tuple->l4_proto, result);
	case L3PROTO_IPV4:
		tuple_to_ipv4_pair(tuple, &pair4);
		return sessiondb_get_by_ipv4(&pair4, tuple->l4_proto, result);
	}

	WARN(true, "Unsupported network protocol: %u.", tuple->l3_proto);
	return -EINVAL;
}

bool sessiondb_allow(struct tuple *tuple)
{
	struct session_table *table;
	struct session_entry *session;
	struct ipv4_pair tuple_pair;
	int error;
	bool result;

	/* Sanity */
	if (WARN(!tuple, "Cannot extract addresses from NULL."))
		return false;
	error = get_session_table(tuple->l4_proto, &table);
	if (error)
		return error;

	/* Action */
	tuple_to_ipv4_pair(tuple, &tuple_pair);

	spin_lock_bh(&table->lock);
	session = rbtree_find(&tuple_pair, &table->tree4, compare_addrs4, struct session_entry,
			tree4_hook);
	result = session ? true : false;
	spin_unlock_bh(&table->lock);

	return result;
}

int sessiondb_add(struct session_entry *session)
{
	struct session_table *table;
	int error;

	/* Sanity */
	if (WARN(!session, "Cannot insert NULL to a session table."))
		return -EINVAL;
	error = get_session_table(session->l4_proto, &table);
	if (error)
		return error;

	/* Action */
	spin_lock_bh(&table->lock);

	error = rbtree_add(session, ipv6, &table->tree6, compare_full6, struct session_entry,
			tree6_hook);
	if (error) {
		spin_unlock_bh(&table->lock);
		return -EEXIST;
	}

	error = rbtree_add(session, ipv4, &table->tree4, compare_full4, struct session_entry,
			tree4_hook);
	if (error) { /* This is not supposed to happen in a perfect world. */
		rb_erase(&session->tree6_hook, &table->tree6);
		spin_unlock_bh(&table->lock);
		/*
		 * Afterthought:
		 * We've analyzed this and indeed it seems to be impossible.
		 * However, maybe we should lower the criticalness of it anyway, to protect ourselves from
		 * future refactors which might break our assumptions...
		 * Hmmmmmmmmmmmmmmmmmmmmmmmmmm.
		 */
		WARN(true, "The session could be indexed by IPv6 but not by IPv4.");
		return -EEXIST;
	}

	session_get(session); /* We have 2 indexes, but really they count as one. */
	table->count++;
	spin_unlock_bh(&table->lock);
	return 0;
}

int sessiondb_for_each(l4_protocol l4_proto, int (*func)(struct session_entry *, void *), void *arg)
{
	struct session_table *table;
	struct rb_node *node;
	int error;

	error = get_session_table(l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->lock);
	for (node = rb_first(&table->tree4); node && !error; node = rb_next(node)) {
		error = func(rb_entry(node, struct session_entry, tree4_hook), arg);
	}

	spin_unlock_bh(&table->lock);
	return error;
}

/**
 * See the function of the same name from the BIB DB module for comments on this.
 *
 * Requires "table"'s spinlock to already be held.
 */
static struct rb_node *find_next_chunk(struct session_table *table,
		struct ipv4_tuple_address *addr, bool starting)
{
	struct rb_node **node, *parent;
	struct session_entry *session;

	if (starting)
		return rb_first(&table->tree4);

	rbtree_find_node(addr, &table->tree4, compare_local4, struct session_entry, tree4_hook, parent,
			node);
	if (*node)
		return rb_next(*node);

	session = rb_entry(parent, struct session_entry, tree4_hook);
	return (compare_local4(session, addr) < 0) ? parent : rb_next(parent);
}

int sessiondb_iterate_by_ipv4(l4_protocol l4_proto, struct ipv4_tuple_address *addr, bool starting,
		int (*func)(struct session_entry *, void *), void *arg)
{
	struct session_table *table;
	struct rb_node *node;
	int error;

	if (WARN(!addr, "The IPv4 address is NULL."))
		return -EINVAL;
	error = get_session_table(l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->lock);
	for (node = find_next_chunk(table, addr, starting); node && !error; node = rb_next(node)) {
		error = func(rb_entry(node, struct session_entry, tree4_hook), arg);
	}

	spin_unlock_bh(&table->lock);
	return error;
}

int sessiondb_count(l4_protocol proto, __u64 *result)
{
	struct session_table *table;
	int error;

	error = get_session_table(proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->lock);
	*result = table->count;
	spin_unlock_bh(&table->lock);
	return 0;
}

int sessiondb_get_or_create_ipv6(struct tuple *tuple, struct bib_entry *bib,
		struct session_entry **session)
{
	struct ipv6_prefix prefix;
	struct in_addr ipv4_dst;
	struct ipv4_pair pair4;
	struct ipv6_pair pair6;
	struct rb_node **node, *parent;
	struct session_table *table;
	int error;

	if (WARN(!tuple, "There's no session entry mapped to NULL."))
		return -EINVAL;

	tuple_to_ipv6_pair(tuple, &pair6);

	error = get_session_table(tuple->l4_proto, &table);
	if (error)
		return error;

	/* Find it */
	spin_lock_bh(&table->lock);
	rbtree_find_node(&pair6, &table->tree6, compare_full6, struct session_entry, tree6_hook,
			parent, node);
	if (*node) {
		*session = rb_entry(*node, struct session_entry, tree6_hook);
		session_get(*session);
		goto end;
	}
	/* The entry doesn't exist, so try to create it. */

	/* Translate address from IPv6 to IPv4 */
	error = pool6_get(&tuple->dst.addr.ipv6, &prefix);
	if (error) {
		log_debug("Errcode %d while obtaining %pI6c's prefix.", error, &tuple->dst.addr.ipv6);
		goto end;
	}

	error = addr_6to4(&tuple->dst.addr.ipv6, &prefix, &ipv4_dst);
	if (error) {
		log_debug("Error code %d while translating the packet's address.", error);
		goto end;
	}

	/*
	 * Create the session entry.
	 *
	 * Fortunately, ICMP errors cannot reach this code because of the requirements in the header
	 * of section 3.5, so we can use the tuple as shortcuts for the packet's fields.
	 */
	pair4.local = bib->ipv4;
	pair4.remote.address = ipv4_dst;
	pair4.remote.l4_id = (tuple->l4_proto != L4PROTO_ICMP) ? tuple->dst.l4_id : bib->ipv4.l4_id;
	*session = session_create(&pair4, &pair6, tuple->l4_proto); /* refcounter = 1*/
	if (!(*session)) {
		log_debug("Failed to allocate a session entry.");
		error = -ENOMEM;
		goto end;
	}

	/* Add it to the database. */
	rb_link_node(&(*session)->tree6_hook, parent, node);
	rb_insert_color(&(*session)->tree6_hook, &table->tree6);

	error = rbtree_add(*session, ipv4, &table->tree4, compare_full4, struct session_entry,
			tree4_hook);
	if (error) {
		WARN(true, "The session entry could be indexed by IPv6, but not by IPv4.");
		rb_erase(&(*session)->tree6_hook, &table->tree6);
		session_kfree(*session);
		goto end;
	}

	/* Tidy up and succeed. */
	session_get(*session); /* refcounter = 2 (the DB's and the one we're about to return) */
	bib_get(bib); /* refcounter++ (because of the new session's reference) */
	(*session)->bib = bib;
	table->count++;
	/* Fall through. */

end:
	spin_unlock_bh(&table->lock);
	return error;
}


int sessiondb_get_or_create_ipv4(struct tuple *tuple, struct bib_entry *bib,
		struct session_entry **session)
{
	struct ipv6_prefix prefix;
	struct in6_addr ipv6_src;
	struct ipv4_pair pair4;
	struct ipv6_pair pair6;
	struct rb_node **node, *parent;
	struct session_table *table;
	int error;

	if (WARN(!tuple, "There's no session entry mapped to NULL."))
		return -EINVAL;

	tuple_to_ipv4_pair(tuple, &pair4);

	error = get_session_table(tuple->l4_proto, &table);
	if (error)
		return error;

	/* Find it */
	spin_lock_bh(&table->lock);
	rbtree_find_node(&pair4, &table->tree4, compare_full4, struct session_entry, tree4_hook,
			parent, node);
	if (*node) {
		*session = rb_entry(*node, struct session_entry, tree4_hook);
		session_get(*session);
		goto end;
	}
	/* The entry doesn't exist, so try to create it. */

	/* Translate address from IPv4 to IPv6 */
	error = pool6_peek(&prefix);
	if (error)
		goto end;

	error = addr_4to6(&tuple->src.addr.ipv4, &prefix, &ipv6_src);
	if (error) {
		log_debug("Error code %d while translating the packet's address.", error);
		goto end;
	}

	/*
	 * Create the session entry.
	 *
	 * Fortunately, ICMP errors cannot reach this code because of the requirements in the header
	 * of section 3.5, so we can use the tuple as shortcuts for the packet's fields.
	 */
	pair6.remote = bib->ipv6;
	pair6.local.address = ipv6_src;
	pair6.local.l4_id = (tuple->l4_proto != L4PROTO_ICMP) ? tuple->src.l4_id : bib->ipv6.l4_id;
	*session = session_create(&pair4, &pair6, tuple->l4_proto); /* refcounter = 1 */
	if (!(*session)) {
		log_debug("Failed to allocate a session entry.");
		error = -ENOMEM;
		goto end;
	}

	/* Add it to the database. */
	rb_link_node(&(*session)->tree4_hook, parent, node);
	rb_insert_color(&(*session)->tree4_hook, &table->tree4);

	error = rbtree_add(*session, ipv6, &table->tree6, compare_full6, struct session_entry,
			tree6_hook);
	if (error) {
		WARN(true, "The session entry could be indexed by IPv4, but not by IPv6.");
		rb_erase(&(*session)->tree4_hook, &table->tree4);
		session_kfree(*session);
		goto end;
	}

	/* Tidy up and succeed. */
	session_get(*session); /* refcounter = 2 (the DB's and the one we're about to return) */
	bib_get(bib); /* refcounter++ (because of the new session's reference) */
	(*session)->bib = bib;
	table->count++;
	/* Fall through. */

end:
	spin_unlock_bh(&table->lock);
	return error;
}

int sessiondb_delete_by_bib(struct bib_entry *bib)
{
	struct session_table *table;
	struct session_entry *root_session, *session;
	struct rb_node *node;
	int error;
	int s = 0;

	/* Sanitize */
	error = get_session_table(bib->l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->lock);

	/* Find the top-most node in the tree whose IPv4 address is addr. */
	root_session = rbtree_find(&bib->ipv4, &table->tree4, compare_local4, struct session_entry,
			tree4_hook);
	if (!root_session)
		goto success; /* "Successfully" deleted zero entries. */

	/* Keep moving left deleting sessions until the local address changes. */
	node = rb_prev(&root_session->tree4_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree4_hook);
		if (compare_local4(session, &bib->ipv4) != 0)
			break;
		s += remove(session, table);

		node = rb_prev(&root_session->tree4_hook);
	}

	/* Keep moving right deleting sessions until the local address changes. */
	node = rb_next(&root_session->tree4_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree4_hook);
		if (compare_local4(session, &bib->ipv4) != 0)
			break;
		s += remove(session, table);

		node = rb_next(&root_session->tree4_hook);
	}

	s += remove(root_session, table);
	table->count -= s;
	/* Fall through. */

success:
	spin_unlock_bh(&table->lock);
	log_debug("Deleted %d sessions.", s);
	return 0;
}

/**
 * Deletes the sessions from the "table" table whose local IPv4 address is "addr".
 * This function is awfully similar to sessiondb_delete_by_bib(). See that for more comments.
 */
static int delete_sessions_by_ipv4(struct session_table *table, struct in_addr *addr)
{
	struct session_entry *root_session, *session;
	struct rb_node *node;
	int s = 0;

	spin_lock_bh(&table->lock);

	root_session = rbtree_find(addr, &table->tree4, compare_local_addr4, struct session_entry,
			tree4_hook);
	if (!root_session)
		goto success;

	node = rb_prev(&root_session->tree4_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree4_hook);
		if (compare_local_addr4(session, addr) != 0)
			break;
		s += remove(session, table);

		node = rb_prev(&root_session->tree4_hook);
	}

	node = rb_next(&root_session->tree4_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree4_hook);
		if (compare_local_addr4(session, addr) != 0)
			break;
		s += remove(session, table);

		node = rb_next(&root_session->tree4_hook);
	}

	s += remove(root_session, table);
	table->count -= s;
	/* Fall through. */

success:
	spin_unlock_bh(&table->lock);
	log_debug("Deleted %d sessions.", s);
	return 0;
}

int sessiondb_delete_by_ipv4(struct in_addr *addr4)
{
	if (WARN(!addr4, "The IPv4 address is NULL"))
		return -EINVAL;

	delete_sessions_by_ipv4(&session_table_tcp, addr4);
	delete_sessions_by_ipv4(&session_table_icmp, addr4);
	delete_sessions_by_ipv4(&session_table_udp, addr4);

	return 0;
}

/**
 * Helper of the set_*_timer functions. Safely updates "session"->dying_time using "ttl" and moves
 * it from its original location to the end of "list".
 */
static void sessiondb_update_timer(struct session_entry *session, struct expire_timer *expirer)
{
	spin_lock_bh(&expirer->table->lock);

	/*
	 * We don't update the session->dying_time when the session isn't part of the DB.
	 *
	 * When this function was called, the spinlock wasn't held.
	 * Ergo, the timer might have remove the entry from the database during that time.
	 * If that happens, we shouldn't update the timer because that'd leave the DB in an
	 * inconsistent state.
	 */
	if (RB_EMPTY_NODE(&session->tree6_hook) || RB_EMPTY_NODE(&session->tree4_hook)) {
		spin_unlock_bh(&expirer->table->lock);
		return;
	}

	session->update_time = jiffies;
	list_del(&session->expire_list_hook);
	list_add_tail(&session->expire_list_hook, &expirer->sessions);

	if (timer_pending(&expirer->timer)) {
		/*
		 * The new session is always the one who's going to expire last.
		 * So if the timer is already set, there should be no reason to edit it.
		 */
		spin_unlock_bh(&expirer->table->lock);
		return;
	}

	spin_unlock_bh(&expirer->table->lock);
	schedule_timer(&expirer->timer, jiffies + get_timeout(expirer));
}

void set_udp_timer(struct session_entry *session)
{
	sessiondb_update_timer(session, &expirer_udp);
}

void set_tcp_est_timer(struct session_entry *session)
{
	sessiondb_update_timer(session, &expirer_tcp_est);
}

void set_tcp_trans_timer(struct session_entry *session)
{
	sessiondb_update_timer(session, &expirer_tcp_trans);
}

void set_icmp_timer(struct session_entry *session)
{
	sessiondb_update_timer(session, &expirer_icmp);
}

/**
 * Marks "session" to be destroyed after TCP_INCOMING_SYN seconds have lapsed.
 */
/*
void set_syn_timer(struct session_entry *session)
{
	__u64 ttl = msecs_to_jiffies(1000 * TCP_INCOMING_SYN);
	sessiondb_update_timer(session, &expirer_syn, ttl);
}
*/

/**
 * Returns 1 if "session"->ipv6.local.address contains "prefix".
 * otherwise returns zero.
 *
 * Doesn't care about spinlocks.
 */
static int sessiondb_ipv6_prefix_equal(struct session_entry *session, struct ipv6_prefix *prefix)
{
	return ipv6_prefix_equal(&prefix->address, &session->ipv6.local.address, prefix->len);
}

/**
 * Used in delete_sessions_by_ipv6_prefix when is searching in the Session tree6,
 * returns zero if "session"->ipv6.local.address is equals to "prefix" or contains the "prefix".
 * Otherwise return the gap of the comparison result.
 */
static int compare_local_prefix6(struct session_entry *session, struct ipv6_prefix *prefix) {
	int gap;

	gap = ipv6_addr_cmp(&prefix->address, &session->ipv6.local.address);
	if (gap == 0)
		return 0;

	if (sessiondb_ipv6_prefix_equal(session, prefix))
		return 0;

	return gap;
}

/**
 * Deletes the sessions from the "table" table whose local IPv6 address contains "prefix".
 * This function is awfully similar to sessiondb_delete_by_bib(). See that for more comments.
 */
static int delete_sessions_by_ipv6_prefix(struct session_table *table, struct ipv6_prefix *prefix)
{
	struct session_entry *root_session, *session;
	struct rb_node *node;
	int s = 0;

	spin_lock_bh(&table->lock);

	root_session = rbtree_find(prefix, &table->tree6, compare_local_prefix6, struct session_entry,
			tree6_hook);
	if (!root_session)
		goto success;

	node = rb_prev(&root_session->tree6_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree6_hook);
		node = rb_prev(&session->tree6_hook);

		if (!sessiondb_ipv6_prefix_equal(session, prefix))
			break;
		s += remove(session, table);
	}

	node = rb_next(&root_session->tree6_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree6_hook);
		node = rb_next(&session->tree6_hook);

		if (!sessiondb_ipv6_prefix_equal(session, prefix))
			break;
		s += remove(session, table);
	}

	s += remove(root_session, table);
	table->count -= s;
	/* Fall through. */

success:
	spin_unlock_bh(&table->lock);
	log_debug("Deleted %d sessions.", s);
	return 0;
}

int sessiondb_delete_by_ipv6_prefix(struct ipv6_prefix *prefix)
{
	if (WARN(!prefix, "The IPv6 prefix is NULL"))
		return -EINVAL;

	delete_sessions_by_ipv6_prefix(&session_table_tcp, prefix);
	delete_sessions_by_ipv6_prefix(&session_table_icmp, prefix);
	delete_sessions_by_ipv6_prefix(&session_table_udp, prefix);

	return 0;
}

static int flush_aux(struct session_table *table)
{
	struct session_entry *root_session, *session;
	struct rb_node *node;
	int s = 0;

	spin_lock_bh(&table->lock);

	node = (&table->tree4)->rb_node;
	if (!node)
		goto success;

	root_session = rb_entry(node, struct session_entry, tree4_hook);
	if (!root_session)
		goto success;

	node = rb_prev(&root_session->tree4_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree4_hook);
		node = rb_prev(&session->tree4_hook);
		s += remove(session, table);
	}

	node = rb_next(&root_session->tree4_hook);
	while (node) {
		session = rb_entry(node, struct session_entry, tree4_hook);
		node = rb_next(&session->tree4_hook);
		s += remove(session, table);
	}

	s += remove(root_session, table);
	table->count -= s;
	/* Fall through. */

success:
	spin_unlock_bh(&table->lock);
	log_debug("Deleted %d sessions.", s);
	return 0;
}

int sessiondb_flush(void)
{

	log_debug("Emptying the session tables...");
	flush_aux(&session_table_udp);
	flush_aux(&session_table_tcp);
	flush_aux(&session_table_icmp);

	return 0;
}
