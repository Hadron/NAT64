#ifndef _JOOL_COMM_CONFIG_PROTO_H
#define _JOOL_COMM_CONFIG_PROTO_H

/**
 * @file
 * Elements visible to both the kernel module and the userspace application, and which they use to
 * communicate with each other.
 *
 * If you see a "__u8", keep in mind it might be intended as a boolean. sizeof(bool) is
 * implementation defined, which is unacceptable because these structures define a communication
 * protocol.
 *
 * @author Miguel Gonzalez
 * @author Alberto Leiva
 */

#include <linux/types.h>
#include "nat64/comm/types.h"


/**
 * ID of Netlink messages Jool listens to.
 * This value was chosen at random, if I remember correctly.
 */
#define MSG_TYPE_JOOL (0x10 + 2)

/**
 * ID of messages intended to return configuration to userspace.
 * ("set config" is intended to be read from the kernel's perspective).
 * This exists as an attempt to match Netlink's conventions; Jool doesn't really care about it.
 */
#define MSG_SETCFG		0x11
/**
 * ID of messages intended to update configuration.
 * ("get config" is intended to be read from the kernel's perspective).
 * This exists as an attempt to match Netlink's conventions; Jool doesn't really care about it.
 *
 * TODO (fine) Looks like nobody is using this. Jool's alignment to Netlink's conventions should
 * probably be rethought.
 */
#define MSG_GETCFG		0x12

enum config_mode {
	/** The current message is talking about the IPv6 pool. */
	MODE_POOL6 = (1 << 1),
	/** The current message is talking about the IPv4 pool. */
	MODE_POOL4 = (1 << 2),
	/** The current message is talking about the Binding Information Bases. */
	MODE_BIB = (1 << 3),
	/** The current message is talking about the session tables. */
	MODE_SESSION = (1 << 4),
	/** The current message is talking about general configuration values. */
	MODE_GENERAL = (1 << 0),
};

#define POOL6_OPS (OP_DISPLAY | OP_COUNT | OP_ADD | OP_REMOVE)
#define POOL4_OPS (OP_DISPLAY | OP_COUNT | OP_ADD | OP_REMOVE)
#define BIB_OPS (OP_DISPLAY | OP_COUNT | OP_ADD | OP_REMOVE)
#define SESSION_OPS (OP_DISPLAY | OP_COUNT)
#define GENERAL_OPS (OP_DISPLAY | OP_UPDATE)

enum config_operation {
	/** The userspace app wants to print the stuff being requested. */
	OP_DISPLAY = (1 << 0),
	/** The userspace app wants to print the number of records in the table being requested. */
	OP_COUNT = (1 << 1),
	/** The userspace app wants to add an element to the table being requested. */
	OP_ADD = (1 << 2),
	/* The userspace app wants to edit some value. */
	OP_UPDATE = (1 << 3),
	/** The userspace app wants to delete an element from the table being requested. */
	OP_REMOVE = (1 << 4),
	/* The userspace app wants to clear some table. */
	OP_FLUSH = (1 << 5),

};

/**
 * @{
 * Allowed modes for the operation mentioned in the name.
 * eg. DISPLAY_MODES = Allowed modes for display operations.
 */
#define DISPLAY_MODES (MODE_POOL6 | MODE_POOL4 | MODE_BIB | MODE_SESSION | MODE_GENERAL)
#define COUNT_MODES (MODE_POOL6 | MODE_POOL4 | MODE_BIB | MODE_SESSION)
#define ADD_MODES (MODE_POOL6 | MODE_POOL4 | MODE_BIB)
#define UPDATE_MODES (MODE_GENERAL)
#define REMOVE_MODES (MODE_POOL6 | MODE_POOL4 | MODE_BIB)
#define FLUSH_MODES (MODE_POOL6 | MODE_POOL4)

/**
 * Prefix to all user-to-kernel messages.
 * Indicates what the rest of the message contains.
 */
struct request_hdr {
	/** Size of the message. Includes header (this one) and payload. */
	__u32 length;
	/** See "enum config_mode". */
	__u8 mode;
	/** See "enum config_operation". */
	__u8 operation;
};

/**
 * Configuration for the "IPv6 Pool" module.
 */
union request_pool6 {
	struct {
		/* Nothing needed here ATM. */
	} display;
	struct {
		/** The prefix the user wants to add to the pool. */
		struct ipv6_prefix prefix;
	} add;
	struct {
		/** The prefix the user wants to remove from the pool. */
		struct ipv6_prefix prefix;
		/* Whether the prefix's sessions should be cleared too (false) or not (true). */
		__u8 quick;
	} remove;
	struct {
		/* Whether the sessions tables should also be cleared (false) or not (true). */
		__u8 quick;
	} flush;
};

/**
 * Configuration for the "IPv4 Pool" module.
 */
union request_pool4 {
	struct {
		/* Nothing needed there ATM. */
	} display;
	struct {
		/** The address the user wants to add to the pool. */
		struct in_addr addr;
	} add;
	struct {
		/** The address the user wants to remove from the pool. */
		struct in_addr addr;
		/* Whether the address's BIB entries and sessions should be cleared too (false) or not (true). */
		__u8 quick;
	} remove;
	struct {
		/* Whether the BIB and the sessions tables should also be cleared (false) or not (true). */
		__u8 quick;
	} flush;
};

/**
 * Configuration for the "BIB" module.
 */
struct request_bib {
	/**
	 * Table the userspace app wants to display or edit.
	 * Actually intended to be a l4_protocol (enum), but enum lengths are implementation-defined.
	 */
	__u8 l4_proto;
	union {
		struct {
			/** If this is false, this is the first chunk the app is requesting. (boolean) */
			__u8 iterate;
			/**
			 * Address the userspace app received in the last chunk. Iteration should contiue
			 * from here.
			 */
			struct ipv4_tuple_address addr4;
		} display;
		struct {
			/* Nothing needed here. */
		} count;
		struct {
			/** The IPv6 transport address of the entry the user wants to add. */
			struct ipv6_tuple_address addr6;
			/** The IPv4 transport address of the entry the user wants to add. */
			struct ipv4_tuple_address addr4;
		} add;
		struct {
			/* Is the value if "addr6" set? (boolean) */
			__u8 addr6_set;
			/** The IPv6 transport address of the entry the user wants to remove. */
			struct ipv6_tuple_address addr6;
			/* Is the value if "addr4" set? (boolean) */
			__u8 addr4_set;
			/** The IPv4 transport address of the entry the user wants to remove. */
			struct ipv4_tuple_address addr4;
		} remove;
		struct {
			/* Nothing needed here. */
		} clear;
	};
};

/**
 * Configuration for the "Session DB"'s tables.
 * Only the "OP_DISPLAY" and "OP_COUNT" operations make sense in this module.
 */
struct request_session {
	/**
	 * Table the userspace app wants to display.
	 * Actually intended to be a l4_protocol (enum), but enum lengths are implementation-defined.
	 */
	__u8 l4_proto;
	union {
		struct {
			/** If this is false, this is the first chunk the app is requesting. (boolean) */
			__u8 iterate;
			/**
			 * Address the userspace app received in the last chunk. Iteration should contiue
			 * from here.
			 */
			struct ipv4_tuple_address addr4;
		} display;
		struct {
			/* Nothing needed here. */
		} count;
	};
};

enum sessiondb_type {
	UDP_TIMEOUT,
	ICMP_TIMEOUT,
	TCP_EST_TIMEOUT,
	TCP_TRANS_TIMEOUT,
};

/**
 * Configuration of the "Session DB" module.
 */
struct sessiondb_config {
	struct {
		/** Maximum number of seconds inactive UDP sessions will remain in the DB. */
		__u64 udp;
		/** Maximum number of seconds inactive ICMP sessions will remain in the DB. */
		__u64 icmp;
		/** Max number of seconds established and inactive TCP sessions will remain in the DB. */
		__u64 tcp_est;
		/** Max number of seconds transitory and inactive TCP sessions will remain in the DB. */
		__u64 tcp_trans;
	} ttl;
};

enum fragmentation_type {
	FRAGMENT_TIMEOUT,
};

/**
 * Time interval to allow arrival of fragments, in milliseconds.
 */
struct fragmentation_config {
	__u64 fragment_timeout;
};

/**
 * Indicators of the respective fields in the pktqueue_config structure.
 */
enum pktqueue_type {
	MAX_PKTS,
};

/**
 * Configuration of the "Packet Queue" module.
 */
struct pktqueue_config {
	__u64 max_pkts;
};

enum filtering_type {
	DROP_BY_ADDR,
	DROP_ICMP6_INFO,
	DROP_EXTERNAL_TCP,
};

/**
 * Configuration for the "Filtering and Updating" module.
 */
struct filtering_config {
	/** Use Address-Dependent Filtering? (boolean) */
	__u8 drop_by_addr;
	/** Filter ICMPv6 Informational packets? (boolean) */
	__u8 drop_icmp6_info;
	/** Drop externally initiated TCP connections? (IPv4 initiated) (boolean) */
	__u8 drop_external_tcp;
};

enum translate_type {
	RESET_TCLASS,
	RESET_TOS,
	NEW_TOS,
	DF_ALWAYS_ON,
	BUILD_IPV4_ID,
	LOWER_MTU_FAIL,
	MTU_PLATEAUS,
	MIN_IPV6_MTU,
};

/**
 * Configuration for the "Translate the packet" module.
 *
 * Several of the fields here are intended to be booleans, but sizeof(bool) is implementation
 * defined, which is unacceptable because this is part of a communication protocol.
 */
struct translate_config {
	/**
	 * "true" if the Traffic Class field of translated IPv6 headers should always be set to zero.
	 * Otherwise it will be copied from the IPv4 header's TOS field.
	 * Boolean.
	 */
	__u8 reset_traffic_class;
	/**
	 * "true" if the Type of Service (TOS) field of translated IPv4 headers should always be set
	 * to "new_tos".
	 * Otherwise it will be copied from the IPv6 header's Traffic Class field.
	 * Boolean.
	 */
	__u8 reset_tos;
	/**
	 * If "reset_tos" is "true", this is the value the translator will always write in the TOS
	 * field of translated IPv4 headers.
	 * If "reset_tos" is "false", then this doesn't do anything.
	 */
	__u8 new_tos;
	/**
	 * If "true", the translator will always set translated IPv4 headers' Don't Fragment (DF)
	 * flags as one.
	 * Otherwise the flag will be set depending on the packet's length.
	 * Boolean.
	 */
	__u8 df_always_on;
	/**
	 * Whether translated IPv4 headers' Identification fields should be computed (Either from the
	 * IPv6 fragment header's Identification field or deduced from the packet's length).
	 * Otherwise it will always be set as zero.
	 * Boolean.
	 */
	__u8 build_ipv4_id;
	/**
	 * "true" if the value for MTU fields of outgoing ICMPv6 fragmentation needed packets should
	 * be set as no less than 1280, regardless of MTU plateaus and whatnot.
	 * See RFC 6145 section 6, second approach.
	 * Boolean.
	 */
	__u8 lower_mtu_fail;
	/** Length of the mtu_plateaus array. */
	__u16 mtu_plateau_count;
	/**
	 * If the translator detects the source of the incoming packet does not implement RFC 1191,
	 * these are the plateau values used to determine a likely path MTU for outgoing ICMPv6
	 * fragmentation needed packets.
	 * The translator is supposed to pick the greatest plateau value that is less than the incoming
	 * packet's Total Length field.
	 */
	__u16 *mtu_plateaus;
	/**
	 * The smallest MTU in the IPv6 side. Jool will ensure that packets traveling from 4 to 6 will
	 * be no bigger than this amount of bytes.
	 */
	__u16 min_ipv6_mtu;
};

enum general_module {
	/** Indicates the presence of a struct sessiondb_config value. */
	SESSIONDB,
	/** Indicates the presence of a struct pktqueue_config value. */
	PKTQUEUE,
	/** Indicates the presence of a struct filtering_config value. */
	FILTERING,
	/** Indicates the presence of a struct translate_config value. */
	TRANSLATE,
	/** Indicates the presence of a struct fragmentation_config value. */
	FRAGMENT,
};

union request_general {
	struct {
		/* Nothing needed here. */
	} display;
	struct {
		__u8 module;
		__u8 type;
		/* The value is given in a variable-sized payload so it's not here. */
	} update;
};

/**
 * A BIB entry, from the eyes of userspace.
 *
 * It's a stripped version of "struct bib_entry" and only used when BIB entries need to travel to
 * userspace. For anything else, use "struct bib_entry".
 *
 * See "struct bib_entry" for documentation on the fields.
 */
struct bib_entry_usr {
	struct ipv4_tuple_address addr4;
	struct ipv6_tuple_address addr6;
	__u8 is_static;
};

/**
 * A session entry, from the eyes of userspace.
 *
 * It's a stripped version of "struct session_entry" and only used when sessions need to travel to
 * userspace. For anything else, use "struct session_entry".
 *
 * See "struct session_entry" for documentation on the fields.
 */
struct session_entry_usr {
	struct ipv6_pair addr6;
	struct ipv4_pair addr4;
	__u64 dying_time;
	__u8 state;
};

/**
 * A copy of the entire running configuration, excluding databases.
 */
struct response_general {
	struct sessiondb_config sessiondb;
	struct pktqueue_config pktqueue;
	struct filtering_config filtering;
	struct translate_config translate;
	struct fragmentation_config fragmentation;
};

/**
 * "struct general_config" has pointers, so if the userspace app wants the configuration,
 * the structure cannot simply be copied to userspace.
 * This translates "config" and its subobjects into a byte array which can then be transformed back
 * using "deserialize_general_config()".
 */
int serialize_general_config(struct response_general *config, unsigned char **buffer_out,
		size_t *buffer_len_out);
/**
 * Reverts the work of serialize_translate_config() by creating "config" out of the byte array
 * "buffer".
 */
int deserialize_general_config(void *buffer, __u16 buffer_len, struct response_general *config);


#endif /* _JOOL_COMM_CONFIG_PROTO_H */
