/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <isc/types.h>
#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/sockaddr.h>

isc_boolean_t
isc_sockaddr_equal(isc_sockaddr_t *a, isc_sockaddr_t *b)
{
	REQUIRE(a != NULL && b != NULL);

	if (a->length != b->length)
		return (ISC_FALSE);

	/*
	 * We don't just memcmp because the sin_zero field isn't always
	 * zero.
	 */

	if (a->type.sa.sa_family != b->type.sa.sa_family)
		return (ISC_FALSE);
	switch (a->type.sa.sa_family) {
	case AF_INET:
		if (memcmp(&a->type.sin.sin_addr, &b->type.sin.sin_addr,
			   sizeof a->type.sin.sin_addr) != 0)
			return (ISC_FALSE);
		if (a->type.sin.sin_port != b->type.sin.sin_port)
			return (ISC_FALSE);
		break;
	case AF_INET6:
		if (memcmp(&a->type.sin6.sin6_addr, &b->type.sin6.sin6_addr,
			   sizeof a->type.sin6.sin6_addr) != 0)
			return (ISC_FALSE);
		if (a->type.sin6.sin6_port != b->type.sin6.sin6_port)
			return (ISC_FALSE);
		break;
	default:
		if (memcmp(&a->type, &b->type, a->length) != 0)
			return (ISC_FALSE);
	}

	return (ISC_TRUE);
}

unsigned int
isc_sockaddr_hash(isc_sockaddr_t *sockaddr, isc_boolean_t address_only) {
	unsigned int length;
	const unsigned char *s;
	unsigned int h = 0;
	unsigned int g;
	
	/*
	 * Provide a hash value for 'sockaddr'.
	 */

	REQUIRE(sockaddr != NULL);

	if (address_only) {
		switch (sockaddr->type.sa.sa_family) {
		case AF_INET:
			return (ntohl(sockaddr->type.sin.sin_addr.s_addr));
		case AF_INET6:
			s = (unsigned char *)&sockaddr->type.sin6.sin6_addr;
			length = sizeof sockaddr->type.sin6.sin6_addr;
			break;
		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "unknown address family: %d\n",
					 (int)sockaddr->type.sa.sa_family);
			s = (unsigned char *)&sockaddr->type;
			length = sockaddr->length;
		}
	} else {
		s = (unsigned char *)&sockaddr->type;
		length = sockaddr->length;
	}

	while (length > 0) {
		h = ( h << 4 ) + *s;
		if ((g = ( h & 0xf0000000 )) != 0) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
		s++;
		length--;
	}

	return (h);
}

void
isc_sockaddr_fromin(isc_sockaddr_t *sockaddr, struct in_addr *ina,
		    unsigned int port)
{
	memset(sockaddr, 0, sizeof *sockaddr);
	sockaddr->type.sin.sin_family = AF_INET;
#ifdef ISC_NET_HAVESALEN
	sockaddr->type.sin.sin_len = sizeof sockaddr->type.sin;
#endif
	sockaddr->type.sin.sin_addr = *ina;
	sockaddr->type.sin.sin_port = htons(port);
	sockaddr->length = sizeof sockaddr->type.sin;
	ISC_LINK_INIT(sockaddr, link);
}

void
isc_sockaddr_fromin6(isc_sockaddr_t *sockaddr, struct in6_addr *ina6,
		     unsigned int port)
{
	memset(sockaddr, 0, sizeof *sockaddr);
	sockaddr->type.sin6.sin6_family = AF_INET6;
#ifdef ISC_NET_HAVESALEN
	sockaddr->type.sin6.sin6_len = sizeof sockaddr->type.sin6;
#endif
	sockaddr->type.sin6.sin6_addr = *ina6;
	sockaddr->type.sin6.sin6_port = htons(port);
	sockaddr->length = sizeof sockaddr->type.sin6;
	ISC_LINK_INIT(sockaddr, link);
}

void
isc_sockaddr_v6fromin(isc_sockaddr_t *sockaddr, struct in_addr *ina,
		      unsigned int port)
{
	memset(sockaddr, 0, sizeof *sockaddr);
	sockaddr->type.sin6.sin6_family = AF_INET6;
#ifdef ISC_NET_HAVESALEN
	sockaddr->type.sin6.sin6_len = sizeof sockaddr->type.sin6;
#endif
	sockaddr->type.sin6.sin6_addr.s6_addr[10] = 0xff;
	sockaddr->type.sin6.sin6_addr.s6_addr[11] = 0xff;
	memcpy(&sockaddr->type.sin6.sin6_addr.s6_addr[12], ina, 4);
	sockaddr->type.sin6.sin6_port = htons(port);
	sockaddr->length = sizeof sockaddr->type.sin6;
	ISC_LINK_INIT(sockaddr, link);
}

int
isc_sockaddr_pf(isc_sockaddr_t *sockaddr) {

	/*
	 * Get the protocol family of 'sockaddr'.
	 */

#if (AF_INET == PF_INET && AF_INET6 == PF_INET6)
	/*
	 * Assume that PF_xxx == AF_xxx for all AF and PF.
	 */
	return (sockaddr->type.sa.sa_family);
#else
	switch (sockaddr->type.sa.sa_family) {
	case AF_INET:
		return (PF_INET);
	case AF_INET6:
		return (PF_INET);
	default:
		FATAL_ERROR(__FILE__, __LINE__, "unknown address family");
	}
#endif
}
