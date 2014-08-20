#include "nat64/usr/dns.h"
#include "nat64/usr/types.h"
#include <netdb.h>
#include <stdio.h>

void print_ipv6_tuple(struct ipv6_tuple_address *tuple, bool numeric_hostname, char *separator,
		__u8 l4_proto)
{
	char hostname[NI_MAXHOST], service[NI_MAXSERV];
	char hostaddr[INET6_ADDRSTRLEN];
	struct sockaddr_in6 sa6;
	int err;

	if (numeric_hostname)
		goto print_numeric;

	memset(&sa6, 0, sizeof(struct sockaddr_in6));
	sa6.sin6_family = AF_INET6;
	sa6.sin6_port = htons(tuple->l4_id);
	sa6.sin6_addr = tuple->address;

	err = getnameinfo((const struct sockaddr*) &sa6, sizeof(sa6),
			hostname, sizeof(hostname), service, sizeof(service), 0);
	if (err != 0) {
		log_err("getnameinfo failed: %s", gai_strerror(err));
		goto print_numeric;
	}

	/* Verification because ICMP doesn't use numeric ports, so it makes no sense to have a
	 * translation of the "ICMP id". */
	if (l4_proto != L4PROTO_ICMP)
		printf("%s%s%s", hostname, separator, service);
	else
		printf("%s%s%u", hostname, separator, tuple->l4_id);
	return;

print_numeric:
	inet_ntop(AF_INET6, &tuple->address, hostaddr, sizeof(hostaddr));
	printf("%s%s%u", hostaddr, separator, tuple->l4_id);
}

void print_ipv4_tuple(struct ipv4_tuple_address *tuple, bool numeric_hostname, char *separator,
		__u8 l4_proto)
{
	char hostname[NI_MAXHOST], service[NI_MAXSERV];
	char *hostaddr;
	struct sockaddr_in sa;
	int err;

	if (numeric_hostname)
		goto print_numeric;

	memset(&sa, 0, sizeof(struct sockaddr_in));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(tuple->l4_id);
	sa.sin_addr = tuple->address;

	err = getnameinfo((const struct sockaddr*) &sa, sizeof(sa),
			hostname, sizeof(hostname), service, sizeof(service), 0);
	if (err != 0) {
		log_err("getnameinfo failed: %s", gai_strerror(err));
		goto print_numeric;
	}

	/* Verification because ICMP doesn't use numeric ports, so it makes no sense to have a
	 * translation of the "ICMP id". */
	if (l4_proto != L4PROTO_ICMP)
		printf("%s%s%s", hostname, separator, service);
	else
		printf("%s%s%u", hostname, separator, tuple->l4_id);
	return;

print_numeric:
	hostaddr = inet_ntoa(tuple->address);
	printf("%s%s%u", hostaddr, separator, tuple->l4_id);
}
