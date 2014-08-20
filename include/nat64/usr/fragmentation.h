#ifndef _JOOL_USR_FRAGMENTATION_H
#define _JOOL_USR_FRAGMENTATION_H

#include <linux/types.h>
#include "nat64/comm/config_proto.h"


#define FRAGMENTATION_TIMEOUT_OPT 	"toFrag"

int fragmentation_request(__u32 operation, struct fragmentation_config *config);


#endif /* _JOOL_USR_FRAGMENTATION_H */

