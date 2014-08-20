#include "nat64/comm/config_proto.h"
#include <linux/slab.h>
#include "nat64/mod/types.h"


int serialize_general_config(struct response_general *config, unsigned char **buffer_out,
		size_t *buffer_len_out)
{
	struct sessiondb_config *sconfig;
	struct filtering_config *filterconfig;
	struct translate_config *tconfig;
	struct pktqueue_config *pconfig;
	struct fragmentation_config *fragconfig;
	unsigned char *buffer;
	size_t mtus_len;

	mtus_len = config->translate.mtu_plateau_count * sizeof(*config->translate.mtu_plateaus);

	buffer = kmalloc(sizeof(*config) + mtus_len, GFP_KERNEL);
	if (!buffer) {
		log_debug("Could not allocate the configuration structure.");
		return -ENOMEM;
	}

	sconfig = &((struct response_general *) buffer)->sessiondb;
	pconfig = &((struct response_general *) buffer)->pktqueue;
	filterconfig = &((struct response_general *) buffer)->filtering;
	tconfig = &((struct response_general *) buffer)->translate;
	fragconfig = &((struct response_general *) buffer)->fragmentation;

	sconfig->ttl.udp = jiffies_to_msecs(config->sessiondb.ttl.udp);
	sconfig->ttl.tcp_est = jiffies_to_msecs(config->sessiondb.ttl.tcp_est);
	sconfig->ttl.tcp_trans = jiffies_to_msecs(config->sessiondb.ttl.tcp_trans);
	sconfig->ttl.icmp = jiffies_to_msecs(config->sessiondb.ttl.icmp);
	memcpy(filterconfig, &config->filtering, sizeof(*filterconfig));
	memcpy(tconfig, &config->translate, sizeof(*tconfig));
	memcpy(tconfig + 1, config->translate.mtu_plateaus, mtus_len);
	memcpy(pconfig, &config->pktqueue, sizeof(*pconfig));
	memcpy(fragconfig, &config->fragmentation, sizeof(*fragconfig));

	*buffer_out = buffer;
	*buffer_len_out = sizeof(*config) + mtus_len;
	return 0;
}

int deserialize_general_config(void *buffer, __u16 buffer_len, struct response_general *target_out)
{
	struct sessiondb_config *sconfig;
	struct translate_config *tconfig;
	size_t mtus_len;

	memcpy(target_out, buffer, sizeof(*target_out));

	sconfig = &target_out->sessiondb;
	sconfig->ttl.udp = msecs_to_jiffies(sconfig->ttl.udp);
	sconfig->ttl.tcp_est = msecs_to_jiffies(sconfig->ttl.tcp_est);
	sconfig->ttl.tcp_trans = msecs_to_jiffies(sconfig->ttl.tcp_trans);
	sconfig->ttl.icmp = msecs_to_jiffies(sconfig->ttl.icmp);

	tconfig = &target_out->translate;
	mtus_len = tconfig->mtu_plateau_count * sizeof(*tconfig->mtu_plateaus);
	tconfig->mtu_plateaus = kmalloc(mtus_len, GFP_ATOMIC);
	if (!tconfig->mtu_plateaus) {
		log_debug("Could not allocate the config's plateaus.");
		return -ENOMEM;
	}
	memcpy(tconfig->mtu_plateaus, buffer + sizeof(*target_out), mtus_len);

	return 0;
}
