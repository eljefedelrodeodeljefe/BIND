/*
 * Copyright (C) 2000  Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 * INTERNET SOFTWARE CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: sdb.c,v 1.1 2000/08/21 22:15:27 bwelling Exp $ */

/*
 * $Id: sdb.c,v 1.1 2000/08/21 22:15:27 bwelling Exp $
 */

#include <config.h>

#include <isc/buffer.h>
#include <isc/lex.h>
#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/once.h>
#include <isc/print.h>
#include <isc/region.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/fixedname.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatatype.h>
#include <dns/result.h>
#include <dns/sdb.h>
#include <dns/types.h>

#include "rdatalist_p.h"
#include "sdb_p.h"

typedef struct sdbimp {
	const char			*drivername;
	dns_sdblookupfunc_t		lookup;
	dns_sdbauthorityfunc_t		authority;
	dns_sdbcreatefunc_t		create;
	dns_sdbdestroyfunc_t		destroy;
	void				*driverdata;
} sdbimp_t;

struct dns_sdb {
	/* Unlocked */
	dns_db_t			common;
	char				*zone;
	sdbimp_t			*implementation;
	void				*dbdata;
	isc_mutex_t			lock;
	/* Locked */
	unsigned int			references;

};

struct dns_sdblookup {
	/* Unlocked */
	unsigned int			magic;
	dns_sdb_t			*sdb;
	ISC_LIST(dns_rdatalist_t)	lists;
	ISC_LIST(isc_buffer_t)		buffers;
	isc_mutex_t			lock;
	/* Locked */
	unsigned int			references;
};

typedef struct sdb_rdatasetiter {
	dns_rdatasetiter_t		common;
	dns_rdatalist_t			*current;
} sdb_rdatasetiter_t;

#define SDB_MAGIC		0x5344422d	/* SDB- */
#define VALID_SDB(sdb)		((sdb) != NULL && \
				 (sdb)->common.impmagic == SDB_MAGIC)

#define SDBLOOKUP_MAGIC		ISC_MAGIC('S','D','B','L')
#define VALID_SDBLOOKUP(sdbl)	ISC_MAGIC_VALID(sdbl, SDBLOOKUP_MAGIC)
#define VALID_SDBNODE(sdbn)	VALID_SDBLOOKUP(sdbn)

/* These values are taken from RFC 1537 */
#define SDB_DEFAULT_REFRESH	(60 * 60 * 8)
#define SDB_DEFAULT_RETRY	(60 * 60 * 2)
#define SDB_DEFAULT_EXPIRE	(60 * 60 * 24 * 7)
#define SDB_DEFAULT_MINIMUM	(60 * 60 * 24)

/* This is a reasonable value */
#define SDB_DEFAULT_TTL		(60 * 60 * 24)

#define MAXSDBIMP 10

static int dummy;

static sdbimp_t imps[MAXSDBIMP];
static int nimps = 0;
static isc_mutex_t implock;
static isc_once_t once;

static isc_result_t findrdataset(dns_db_t *db, dns_dbnode_t *node,
				 dns_dbversion_t *version,
				 dns_rdatatype_t type, dns_rdatatype_t covers,
				 isc_stdtime_t now, dns_rdataset_t *rdataset,
				 dns_rdataset_t *sigrdataset);

static void detachnode(dns_db_t *db, dns_dbnode_t **targetp);


static void list_tordataset(dns_rdatalist_t *rdatalist,
			    dns_db_t *db, dns_dbnode_t *node,
			    dns_rdataset_t *rdataset);


static void rdatasetiter_destroy(dns_rdatasetiter_t **iteratorp);
static isc_result_t rdatasetiter_first(dns_rdatasetiter_t *iterator);
static isc_result_t rdatasetiter_next(dns_rdatasetiter_t *iterator);
static void rdatasetiter_current(dns_rdatasetiter_t *iterator,
				 dns_rdataset_t *rdataset);

static dns_rdatasetitermethods_t rdatasetiter_methods = {
	rdatasetiter_destroy,
	rdatasetiter_first,
	rdatasetiter_next,
	rdatasetiter_current
};

static void
initialize(void) {
	RUNTIME_CHECK(isc_mutex_init(&implock) == ISC_R_SUCCESS);
}

/*
 * Functions used by implementors of simple databases
 */
isc_result_t
dns_sdb_register(const char *drivername, dns_sdblookupfunc_t lookup,
		 dns_sdbauthorityfunc_t authority, dns_sdbcreatefunc_t create,
		 dns_sdbdestroyfunc_t destroy, void *driverdata)
{
	int i, slot;
	RUNTIME_CHECK(isc_once_do(&once, initialize) == ISC_R_SUCCESS);

	REQUIRE(drivername != NULL);
	REQUIRE(lookup != NULL);
	REQUIRE(authority != NULL);

	LOCK(&implock);
	for (i = 0; i < nimps; i++) {
		if (imps[i].drivername == NULL)
			slot = i;
		else if (strcmp(drivername, imps[i].drivername) == 0) {
			UNLOCK(&implock);
			return (ISC_R_EXISTS);
		}
	}
	if (i == nimps) {
		if (nimps < MAXSDBIMP)
			slot = nimps++;
		else {
			UNLOCK(&implock);
			return (ISC_R_NOSPACE);
		}
	}
	imps[slot].drivername = drivername;
	imps[slot].lookup = lookup;
	imps[slot].authority = authority;
	imps[slot].create = create;
	imps[slot].destroy = destroy;
	imps[slot].driverdata = driverdata;
	UNLOCK(&implock);

	return (ISC_R_SUCCESS);
}

void
dns_sdb_unregister(const char *drivername) {
	int i;
	LOCK(&implock);
	for (i = 0; i < nimps; i++) {
		if (imps[i].drivername != NULL &&
		    strcmp(imps[i].drivername, drivername) == 0)
			imps[i].drivername = NULL;
	}
	UNLOCK(&implock);

}

isc_result_t
dns_sdb_putrr(dns_sdblookup_t *lookup, const char *type, dns_ttl_t ttl,
	      const char *data)
{
	dns_rdatalist_t *rdatalist;
	dns_rdata_t *rdata;
	dns_rdatatype_t typeval;
	isc_consttextregion_t r;
	isc_buffer_t b;
	isc_buffer_t *rdatabuf;
	isc_lex_t *lex;
	isc_result_t result;
	unsigned int size;
	isc_mem_t *mctx;

	REQUIRE(VALID_SDBLOOKUP(lookup));
	REQUIRE(type != NULL);
	REQUIRE(data != NULL);

	mctx = lookup->sdb->common.mctx;

	r.base = type;
	r.length = strlen(type);
	result = dns_rdatatype_fromtext(&typeval, (isc_textregion_t *)&r);
	if (result != ISC_R_SUCCESS)
		return (result);

	rdatalist = ISC_LIST_HEAD(lookup->lists);
	while (rdatalist != NULL) {
		if (rdatalist->type == typeval)
			break;
		rdatalist = ISC_LIST_NEXT(rdatalist, link);
	}

	if (rdatalist == NULL) {
		rdatalist = isc_mem_get(mctx, sizeof(dns_rdatalist_t));
		if (rdatalist == NULL)
			return (ISC_R_NOMEMORY);
		rdatalist->rdclass = lookup->sdb->common.rdclass;
		rdatalist->type = typeval;
		rdatalist->covers = 0;
		rdatalist->ttl = ttl;
		ISC_LIST_INIT(rdatalist->rdata);
		ISC_LINK_INIT(rdatalist, link);
		ISC_LIST_APPEND(lookup->lists, rdatalist, link);
	} else 
		if (rdatalist->ttl != ttl)
			return (DNS_R_BADTTL);

	rdata = isc_mem_get(mctx, sizeof(dns_rdata_t));
	if (rdata == NULL)
		return (ISC_R_NOMEMORY);
	dns_rdata_init(rdata);

	lex = NULL;
	result = isc_lex_create(mctx, 64, &lex);
	if (result != ISC_R_SUCCESS)
		goto failure;

	size = 64;
	do {
		isc_buffer_init(&b, data, strlen(data));
		isc_buffer_add(&b, strlen(data));

		result = isc_lex_openbuffer(lex, &b);
		if (result != ISC_R_SUCCESS)
			goto failure;

		rdatabuf = NULL;
		result = isc_buffer_allocate(mctx, &rdatabuf, size);
		if (result != ISC_R_SUCCESS)
			goto failure;
		result = dns_rdata_fromtext(rdata, rdatalist->rdclass,
					    rdatalist->type, lex,
					    dns_rootname, ISC_FALSE,
					    rdatabuf, NULL);
		if (result != ISC_R_SUCCESS)
			isc_buffer_free(&rdatabuf);
		size *= 2;
	} while (result == ISC_R_NOSPACE);

	if (result != ISC_R_SUCCESS)
		goto failure;

	ISC_LIST_APPEND(rdatalist->rdata, rdata, link);
	ISC_LIST_APPEND(lookup->buffers, rdatabuf, link);

	if (lex != NULL)
		isc_lex_destroy(&lex);

	return (ISC_R_SUCCESS);

 failure:

 	if (rdatabuf != NULL)
		isc_buffer_free(&rdatabuf);
	if (lex != NULL)
		isc_lex_destroy(&lex);
	isc_mem_put(mctx, rdata, sizeof(dns_rdata_t));

	return (result);
}

isc_result_t
dns_sdb_putsoa(dns_sdblookup_t *lookup, const char *mname, const char *rname,
	       isc_uint32_t serial)
{
	char str[2 * DNS_NAME_MAXTEXT + 5 * (sizeof("2147483647")) + 7];
	int n;

	REQUIRE(mname != NULL);
	REQUIRE(rname != NULL);

	n = snprintf(str, sizeof str, "%s %s %u %u %u %u %u",
		     mname, rname, serial,
		     SDB_DEFAULT_REFRESH, SDB_DEFAULT_RETRY,
		     SDB_DEFAULT_EXPIRE, SDB_DEFAULT_MINIMUM);
	if (n >= (int)sizeof(str) || n < 0)
		return (ISC_R_NOSPACE);
	return (dns_sdb_putrr(lookup, "SOA", SDB_DEFAULT_TTL, str));
}

/*
 * DB routines
 */

static void
attach(dns_db_t *source, dns_db_t **targetp) {
	dns_sdb_t *sdb = (dns_sdb_t *) source;

	REQUIRE(VALID_SDB(sdb));

	LOCK(&sdb->lock);
	REQUIRE(sdb->references > 0);
	sdb->references++;
	UNLOCK(&sdb->lock);

	*targetp = source;
}

static void
destroy(dns_sdb_t *sdb) {
	isc_mem_t *mctx;
	sdbimp_t *imp = sdb->implementation;

	mctx = sdb->common.mctx;

	if (imp->destroy != NULL)
		imp->destroy(sdb->zone, imp->driverdata, &sdb->dbdata);

	isc_mem_free(mctx, sdb->zone);
	isc_mutex_destroy(&sdb->lock);

	sdb->common.magic = 0;
	sdb->common.impmagic = 0;

	dns_name_free(&sdb->common.origin, mctx);

	isc_mem_put(mctx, sdb, sizeof(dns_sdb_t));
	isc_mem_detach(&mctx);
}

static void
detach(dns_db_t **dbp) {
	dns_sdb_t *sdb = (dns_sdb_t *)(*dbp);
	isc_boolean_t need_destroy = ISC_FALSE;

	REQUIRE(VALID_SDB(sdb));
	LOCK(&sdb->lock);
	REQUIRE(sdb->references > 0);
	sdb->references--;
	if (sdb->references == 0)
		need_destroy = ISC_TRUE;
	UNLOCK(&sdb->lock);

	if (need_destroy)
		destroy(sdb);

	*dbp = NULL;
}

static isc_result_t
beginload(dns_db_t *db, dns_addrdatasetfunc_t *addp, dns_dbload_t **dbloadp) {
	UNUSED(db);
	UNUSED(addp);
	UNUSED(dbloadp);
	return (ISC_R_NOTIMPLEMENTED);
}

static isc_result_t
endload(dns_db_t *db, dns_dbload_t **dbloadp) {
	UNUSED(db);
	UNUSED(dbloadp);
	return (ISC_R_NOTIMPLEMENTED);
}

static isc_result_t
dump(dns_db_t *db, dns_dbversion_t *version, const char *filename) {
	UNUSED(db);
	UNUSED(version);
	UNUSED(filename);
	return (ISC_R_NOTIMPLEMENTED);
}

static void
currentversion(dns_db_t *db, dns_dbversion_t **versionp) {
	REQUIRE(versionp != NULL && *versionp == NULL);

	UNUSED(db);

	*versionp = (void *) &dummy;
	return;
}

static isc_result_t
newversion(dns_db_t *db, dns_dbversion_t **versionp) {
	UNUSED(db);
	UNUSED(versionp);

	return (ISC_R_NOTIMPLEMENTED);
}

static void
attachversion(dns_db_t *db, dns_dbversion_t *source, 
	      dns_dbversion_t **targetp)
{
	REQUIRE(source != NULL && source == (void *) &dummy);

	UNUSED(db);
	UNUSED(source);
	UNUSED(targetp);
	return;
}

static void
closeversion(dns_db_t *db, dns_dbversion_t **versionp, isc_boolean_t commit) {
	REQUIRE(versionp != NULL && *versionp == (void *) &dummy);
	REQUIRE(commit == ISC_FALSE);

	UNUSED(db);
	UNUSED(commit);

	*versionp = NULL;
}

static isc_result_t
createnode(dns_sdb_t *sdb, dns_sdbnode_t **nodep) {
	dns_sdbnode_t *node;
	isc_result_t result;

	node = isc_mem_get(sdb->common.mctx, sizeof(dns_sdbnode_t));
	if (node == NULL)
		return (ISC_R_NOMEMORY);

	node->sdb = NULL;
	attach((dns_db_t *)sdb, (dns_db_t **)&node->sdb);
	ISC_LIST_INIT(node->lists);
	ISC_LIST_INIT(node->buffers);
	result = isc_mutex_init(&node->lock);
	if (result != ISC_R_SUCCESS) {
		isc_mem_put(sdb->common.mctx, sdb, sizeof(dns_sdb_t));
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed: %s",
				 isc_result_totext(result));
		isc_mem_put(sdb->common.mctx, node, sizeof(dns_sdbnode_t));
		return (ISC_R_UNEXPECTED);
	}
	node->references = 1;
	node->magic = SDBLOOKUP_MAGIC;

	*nodep = node;
	return (ISC_R_SUCCESS);
}

static void
destroynode(dns_sdbnode_t *node) {
	dns_rdatalist_t *list;
	dns_rdata_t *rdata;
	isc_buffer_t *b;
	dns_sdb_t *sdb;

	while (!ISC_LIST_EMPTY(node->lists)) {
		list = ISC_LIST_HEAD(node->lists);
		while (!ISC_LIST_EMPTY(list->rdata)) {
			rdata = ISC_LIST_HEAD(list->rdata);
			ISC_LIST_UNLINK(list->rdata, rdata, link);
			isc_mem_put(node->sdb->common.mctx, rdata,
				    sizeof(dns_rdata_t));
		}
		ISC_LIST_UNLINK(node->lists, list, link);
		isc_mem_put(node->sdb->common.mctx, list,
			    sizeof(dns_rdatalist_t));
	}

	while (!ISC_LIST_EMPTY(node->buffers)) {
		b = ISC_LIST_HEAD(node->buffers);
		ISC_LIST_UNLINK(node->buffers, b, link);
		isc_buffer_free(&b);
	}

	isc_mutex_destroy(&node->lock);
	sdb = node->sdb;
	node->magic = 0;
	isc_mem_put(node->sdb->common.mctx, node, sizeof(dns_sdbnode_t));
	detach((dns_db_t **)&sdb);
}

static isc_result_t
findnode(dns_db_t *db, dns_name_t *name, isc_boolean_t create,
	 dns_dbnode_t **nodep)
{
	dns_sdb_t *sdb = (dns_sdb_t *)db;
	dns_sdbnode_t *node = NULL;
	isc_result_t result;
	isc_buffer_t b;
	char namestr[DNS_NAME_MAXTEXT + 1];
	isc_boolean_t isorigin;
	sdbimp_t *imp;

	REQUIRE(VALID_SDB(sdb));
	REQUIRE(create == ISC_FALSE);
	REQUIRE(nodep != NULL && *nodep == NULL);

	UNUSED(name);

	isc_buffer_init(&b, namestr, sizeof(namestr));
	result = dns_name_totext(name, ISC_TRUE, &b);
	if (result != ISC_R_SUCCESS)
		return (result);
	isc_buffer_putuint8(&b, 0);

	result = createnode(sdb, &node);
	if (result != ISC_R_SUCCESS)
		return (result);

	imp = sdb->implementation;

	isorigin = dns_name_equal(name, &sdb->common.origin);

	result = imp->lookup(sdb->zone, namestr, sdb->dbdata, node);
	if (result != ISC_R_SUCCESS && !isorigin) {
		destroynode(node);
		return (result);
	}

	if (isorigin) {
		result = imp->authority(sdb->zone, sdb->dbdata, node);
		if (result != ISC_R_SUCCESS) {
			destroynode(node);
			return (result);
		}
	}
	
	*nodep = node;
	return (ISC_R_SUCCESS);
}

static isc_result_t
find(dns_db_t *db, dns_name_t *name, dns_dbversion_t *version,
     dns_rdatatype_t type, unsigned int options, isc_stdtime_t now,
     dns_dbnode_t **nodep, dns_name_t *foundname,
     dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	dns_sdb_t *sdb = (dns_sdb_t *)db;
	dns_dbnode_t *node = NULL;
	dns_fixedname_t fname;
	dns_rdataset_t xrdataset;
	dns_name_t *xname;
	unsigned int nlabels, olabels;
	isc_result_t result;
	unsigned int i;

	REQUIRE(VALID_SDB(sdb));
	REQUIRE(nodep == NULL || *nodep == NULL);
	REQUIRE(version == NULL || version == (void *) &dummy);

	UNUSED(options);
	UNUSED(sigrdataset);

	if (!dns_name_issubdomain(name, &db->origin))
		return (DNS_R_NXDOMAIN);

	olabels = dns_name_countlabels(&db->origin);
	nlabels = dns_name_countlabels(name);

	dns_fixedname_init(&fname);
	xname = dns_fixedname_name(&fname);

	if (rdataset == NULL)
		rdataset = &xrdataset;

	result = DNS_R_NXDOMAIN;

	for (i = olabels; i <= nlabels; i++) {
		/*
		 * Unless this is an explicit lookup at the origin, don't
		 * look at the origin.
		 */
		if (i == olabels && i != nlabels)
			continue;

		/*
		 * Look up the next label.
		 */
		dns_name_getlabelsequence(name, nlabels - i, i, xname);
		result = findnode(db, xname, ISC_FALSE, &node);
		if (result != ISC_R_SUCCESS) {
			result = DNS_R_NXDOMAIN;
			continue;
		}

		/*
		 * Look for a DNAME at the current label, unless this is
		 * the qname.
		 */
		if (i < nlabels) {
			result = findrdataset(db, node, version,
					      dns_rdatatype_dname,
					      0, now, rdataset, sigrdataset);
			if (result == ISC_R_SUCCESS) {
				if (type != dns_rdatatype_dname)
					result = DNS_R_DNAME;
				break;
			}
		}

		/*
		 * Look for an NS at the current label, unless this is the
		 * origin.
		 */
		if (i != olabels) {
			result = findrdataset(db, node, version,
					      dns_rdatatype_ns,
					      0, now, rdataset, sigrdataset);
			if (result == ISC_R_SUCCESS) {
				if (type != dns_rdatatype_ns)
					result = DNS_R_DELEGATION;
				break;
			}
		}

		/*
		 * If the current name is not the qname, add another label
		 * and try again.
		 */
		if (i < nlabels) {
			destroynode(node);
			node = NULL;
			continue;
		}

		/*
		 * If we're looking for ANY, we're done.
		 */
		if (type == dns_rdatatype_any) {
			result = ISC_R_SUCCESS;
			break;
		}

		/*
		 * Look for the qtype.
		 */
		result = findrdataset(db, node, version, type,
				      0, now, rdataset, sigrdataset);
		if (result == ISC_R_SUCCESS)
			break;

		/*
		 * Look for a CNAME
		 */
		if (type != dns_rdatatype_cname) {
			result = findrdataset(db, node, version,
					      dns_rdatatype_cname,
					      0, now, rdataset, sigrdataset);
			if (result == ISC_R_SUCCESS) {
				result = DNS_R_CNAME;
				break;
			}
		}

		result = DNS_R_NXRRSET;
		break;
	}

	if (rdataset == &xrdataset && dns_rdataset_isassociated(rdataset))
		dns_rdataset_disassociate(rdataset);

	if (foundname != NULL) {
		isc_result_t xresult;

		xresult = dns_name_concatenate(xname, NULL, foundname, NULL);
		if (xresult != ISC_R_SUCCESS) {
			destroynode(node);
			if (dns_rdataset_isassociated(rdataset))
				dns_rdataset_disassociate(rdataset);
			return (DNS_R_BADDB);
		}
	}

	if (nodep != NULL)
		*nodep = node;
	else if (node != NULL)
		detachnode(db, &node);

	return (result);
}

static isc_result_t
findzonecut(dns_db_t *db, dns_name_t *name, unsigned int options,
	    isc_stdtime_t now, dns_dbnode_t **nodep, dns_name_t *foundname,
	    dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	UNUSED(db);
	UNUSED(name);
	UNUSED(options);
	UNUSED(now);
	UNUSED(nodep);
	UNUSED(foundname);
	UNUSED(rdataset);
	UNUSED(sigrdataset);

	return (ISC_R_NOTIMPLEMENTED);
}

static void
attachnode(dns_db_t *db, dns_dbnode_t *source, dns_dbnode_t **targetp) {
	dns_sdb_t *sdb = (dns_sdb_t *)db;
	dns_sdbnode_t *node = (dns_sdbnode_t *)source;

	REQUIRE(VALID_SDB(sdb));
	LOCK(&node->lock);
	INSIST(node->references > 0);
	node->references++;
	INSIST(node->references != 0);		/* Catch overflow. */
	UNLOCK(&node->lock);

	*targetp = source;
}

static void
detachnode(dns_db_t *db, dns_dbnode_t **targetp) {
	dns_sdb_t *sdb = (dns_sdb_t *)db;
	dns_sdbnode_t *node;
	isc_boolean_t need_destroy = ISC_FALSE;

	REQUIRE(VALID_SDB(sdb));
	REQUIRE(targetp != NULL && *targetp != NULL);

	node = (dns_sdbnode_t *)(*targetp);

	LOCK(&node->lock);
	INSIST(node->references > 0);
	node->references--;
	if (node->references == 0)
		need_destroy = ISC_TRUE;
	UNLOCK(&node->lock);

	if (need_destroy)
		destroynode(node);

	*targetp = NULL;
}

static isc_result_t
expirenode(dns_db_t *db, dns_dbnode_t *node, isc_stdtime_t now) {
	UNUSED(db);
	UNUSED(node);
	UNUSED(now);
	INSIST(0);
	return (ISC_R_UNEXPECTED);
}

static void
printnode(dns_db_t *db, dns_dbnode_t *node, FILE *out) {
	UNUSED(db);
	UNUSED(node);
	UNUSED(out);
	return;
}

static isc_result_t
createiterator(dns_db_t *db, isc_boolean_t relative_names,
	       dns_dbiterator_t **iteratorp)
{
	UNUSED(db);
	UNUSED(relative_names);
	UNUSED(iteratorp);
	return (ISC_R_NOTIMPLEMENTED);
}

static isc_result_t
findrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	     dns_rdatatype_t type, dns_rdatatype_t covers,
	     isc_stdtime_t now, dns_rdataset_t *rdataset,
	     dns_rdataset_t *sigrdataset)
{
	dns_rdatalist_t *list;
	dns_sdbnode_t *sdbnode = (dns_sdbnode_t *)node;

	REQUIRE(VALID_SDBNODE(node));

	UNUSED(db);
	UNUSED(version);
	UNUSED(covers);
	UNUSED(now);
	UNUSED(sigrdataset);

	if (type == dns_rdatatype_sig)
		return (ISC_R_NOTIMPLEMENTED);

	list = ISC_LIST_HEAD(sdbnode->lists);
	while (list != NULL) {
		if (list->type == type)
			break;
		list = ISC_LIST_NEXT(list, link);
	}
	if (list == NULL)
		return (ISC_R_NOTFOUND);

	list_tordataset(list, db, node, rdataset);

	return (ISC_R_SUCCESS);
}

static isc_result_t
allrdatasets(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	     isc_stdtime_t now, dns_rdatasetiter_t **iteratorp)
{
	sdb_rdatasetiter_t *iterator;

	REQUIRE(version == NULL || version == &dummy);
	
	UNUSED(version);
	UNUSED(now);

	iterator = isc_mem_get(db->mctx, sizeof(sdb_rdatasetiter_t));
	if (iterator == NULL)
		return (ISC_R_NOMEMORY);

	iterator->common.magic = DNS_RDATASETITER_MAGIC;
	iterator->common.methods = &rdatasetiter_methods;
	iterator->common.db = db;
	iterator->common.node = NULL;
	attachnode(db, node, &iterator->common.node);
	iterator->common.version = version;
	iterator->common.now = now;

	*iteratorp = (dns_rdatasetiter_t *)iterator;

	return (ISC_R_SUCCESS);
}

static isc_result_t
addrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	    isc_stdtime_t now, dns_rdataset_t *rdataset, unsigned int options,
	    dns_rdataset_t *addedrdataset)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(now);
	UNUSED(rdataset);
	UNUSED(options);
	UNUSED(addedrdataset);

	return (ISC_R_NOTIMPLEMENTED);
}

static isc_result_t
subtractrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		 dns_rdataset_t *rdataset, dns_rdataset_t *newrdataset)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(rdataset);
	UNUSED(newrdataset);

	return (ISC_R_NOTIMPLEMENTED);
}

static isc_result_t
deleterdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	       dns_rdatatype_t type, dns_rdatatype_t covers)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(type);
	UNUSED(covers);

	return (ISC_R_NOTIMPLEMENTED);
}

static isc_boolean_t
issecure(dns_db_t *db) {
	UNUSED(db);

	return (ISC_FALSE);
}

static unsigned int
nodecount(dns_db_t *db) {
	UNUSED(db);

	return (0);
}

static isc_boolean_t
ispersistent(dns_db_t *db) {
	UNUSED(db);
	return (ISC_TRUE);
}


static dns_dbmethods_t sdb_methods = {
	attach,
	detach,
	beginload,
	endload,
	dump,
	currentversion,
	newversion,
	attachversion,
	closeversion,
	findnode,
	find,
	findzonecut,
	attachnode,
	detachnode,
	expirenode,
	printnode,
	createiterator,
	findrdataset,
	allrdatasets,
	addrdataset,
	subtractrdataset,
	deleterdataset,
	issecure,
	nodecount,
	ispersistent
};

isc_result_t
dns_sdb_create(isc_mem_t *mctx, dns_name_t *origin, dns_dbtype_t type,
	       dns_rdataclass_t rdclass, unsigned int argc, char *argv[],
	       dns_db_t **dbp)
{
	dns_sdb_t *sdb;
	isc_result_t result;
	char *sdbtype;
	char zonestr[DNS_NAME_MAXTEXT + 1];
	isc_buffer_t b;
	sdbimp_t *imp;
	int i;

	if (argc < 1)
		return (ISC_R_NOTIMPLEMENTED);
	sdbtype = argv[0];

	LOCK(&implock);
	for (i = 0; i < nimps; i++)
		if (imps[i].drivername != NULL &&
		    strcmp(sdbtype, imps[i].drivername) == 0)
			break;
	if (i == nimps) {
		UNLOCK(&implock);
		return (ISC_R_NOTFOUND);
	}
	UNLOCK(&implock);
	imp = &imps[i];

	if (type != dns_dbtype_zone)
		return (ISC_R_NOTIMPLEMENTED);

	sdb = isc_mem_get(mctx, sizeof(dns_sdb_t));
	if (sdb == NULL)
		return (ISC_R_NOMEMORY);
	memset(sdb, 0, sizeof(dns_sdb_t));

	dns_name_init(&sdb->common.origin, NULL);
	sdb->common.attributes = 0;
	sdb->common.methods = &sdb_methods;
	sdb->common.rdclass = rdclass;
	sdb->common.mctx = NULL;
	sdb->implementation = imp;

	isc_mem_attach(mctx, &sdb->common.mctx);

	result = isc_mutex_init(&sdb->lock);
	if (result != ISC_R_SUCCESS) {
		isc_mem_put(mctx, sdb, sizeof(dns_sdb_t));
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed: %s",
				 isc_result_totext(result));
		isc_mem_detach(&sdb->common.mctx);
		return (ISC_R_UNEXPECTED);
	}

	result = dns_name_dupwithoffsets(origin, mctx, &sdb->common.origin);
	if (result != ISC_R_SUCCESS) {
		destroy(sdb);
		return (result);
	}
	isc_buffer_init(&b, zonestr, sizeof(zonestr));
	result = dns_name_totext(origin, ISC_TRUE, &b);
	if (result != ISC_R_SUCCESS) {
		destroy(sdb);
		return (result);
	}
	isc_buffer_putuint8(&b, 0);
	sdb->zone = isc_mem_strdup(mctx, zonestr);
	if (sdb->zone == NULL) {
		destroy(sdb);
		return (ISC_R_NOMEMORY);
	}
	sdb->dbdata = NULL;
	if (imp->create != NULL) {
		result = imp->create(sdb->zone, argc - 1, argv + 1,
				     imp->driverdata, &sdb->dbdata);
		if (result != ISC_R_SUCCESS) {
			destroy(sdb);
			return (result);
		}
	}

	sdb->references = 1;

	sdb->common.magic = DNS_DB_MAGIC;
	sdb->common.impmagic = SDB_MAGIC;

	*dbp = (dns_db_t *)sdb;

	return (ISC_R_SUCCESS);
}


/*
 * Rdataset Methods
 */

static void
disassociate(dns_rdataset_t *rdataset) {
        dns_dbnode_t *node = rdataset->private5;
	dns_sdbnode_t *sdbnode = (dns_sdbnode_t *) node;
	dns_db_t *db = (dns_db_t *) sdbnode->sdb;

	detachnode(db, &node);
	isc__rdatalist_disassociate(rdataset);
}

static dns_rdatasetmethods_t methods = {
	disassociate,
	isc__rdatalist_first,
	isc__rdatalist_next,
	isc__rdatalist_current,
	isc__rdatalist_clone,
	isc__rdatalist_count
};

static void
list_tordataset(dns_rdatalist_t *rdatalist,
		dns_db_t *db, dns_dbnode_t *node,
		dns_rdataset_t *rdataset)
{
	/* This should never fail. */
	RUNTIME_CHECK(dns_rdatalist_tordataset(rdatalist, rdataset) ==
		      ISC_R_SUCCESS);

	rdataset->methods = &methods;
	dns_db_attachnode(db, node, &rdataset->private5);
}

/*
 * Rdataset Iterator Methods
 */

static void
rdatasetiter_destroy(dns_rdatasetiter_t **iteratorp) {
	sdb_rdatasetiter_t *sdbiterator = (sdb_rdatasetiter_t *)(*iteratorp);
	detachnode(sdbiterator->common.db, &sdbiterator->common.node);
	isc_mem_put(sdbiterator->common.db->mctx, sdbiterator,
		    sizeof(sdb_rdatasetiter_t));
	*iteratorp = NULL;
}

static isc_result_t
rdatasetiter_first(dns_rdatasetiter_t *iterator) {
	sdb_rdatasetiter_t *sdbiterator = (sdb_rdatasetiter_t *)iterator;
	dns_sdbnode_t *sdbnode = (dns_sdbnode_t *)iterator->node;

	if (ISC_LIST_EMPTY(sdbnode->lists))
		return (ISC_R_NOMORE);
	sdbiterator->current = ISC_LIST_HEAD(sdbnode->lists);
	return (ISC_R_SUCCESS);
}

static isc_result_t
rdatasetiter_next(dns_rdatasetiter_t *iterator) {
	sdb_rdatasetiter_t *sdbiterator = (sdb_rdatasetiter_t *)iterator;

	sdbiterator->current = ISC_LIST_NEXT(sdbiterator->current, link);
	if (sdbiterator->current == NULL)
		return (ISC_R_NOMORE);
	else
		return (ISC_R_SUCCESS);
}

static void
rdatasetiter_current(dns_rdatasetiter_t *iterator, dns_rdataset_t *rdataset) {
	sdb_rdatasetiter_t *sdbiterator = (sdb_rdatasetiter_t *)iterator;

	list_tordataset(sdbiterator->current, iterator->db, iterator->node,
			rdataset);

}

