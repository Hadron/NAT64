#ifndef _JOOL_MOD_OUTGOING_H
#define _JOOL_MOD_OUTGOING_H

/**
 * @file
 * Third step in the packet processing algorithm defined in the RFC.
 * The 3.6 section of RFC 6146 is encapsulated in this module.
 * Infers a tuple (summary) of the outgoing packet, yet to be created.
 *
 * @author Ramiro Nava
 * @author Alberto Leiva
 */

#include "nat64/mod/types.h"


verdict compute_out_tuple(struct tuple *in, struct tuple *out);


#endif /* _JOOL_MOD_OUTGOING_H */
