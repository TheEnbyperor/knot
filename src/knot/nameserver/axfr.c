/*  Copyright (C) 2024 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <urcu.h>

#include "contrib/mempattern.h"
#include "contrib/sockaddr.h"
#include "knot/nameserver/axfr.h"
#include "knot/nameserver/internet.h"
#include "knot/nameserver/log.h"
#include "knot/nameserver/xfr.h"
#include "libknot/libknot.h"

#define ZONE_NAME(qdata) knot_pkt_qname((qdata)->query)
#define REMOTE(qdata) (qdata)->params->remote
#define PROTO(qdata) (qdata)->params->proto
#define KEY(qdata) (qdata)->sign.tsig_key.name

#define AXFROUT_LOG(priority, qdata, fmt...) \
	ns_log(priority, ZONE_NAME(qdata), LOG_OPERATION_AXFR, \
	       LOG_DIRECTION_OUT, REMOTE(qdata), PROTO(qdata), false, KEY(qdata), fmt)

/* AXFR context. @note aliasing the generic xfr_proc */
struct axfr_proc {
	struct xfr_proc proc;
	trie_it_t *i;
	zone_tree_it_t it;
	unsigned cur_rrset;
};

static int axfr_put_rrsets(knot_pkt_t *pkt, zone_node_t *node,
                           struct axfr_proc *state)
{
	assert(node != NULL);

	/* Append all RRs. */
	for (unsigned i = state->cur_rrset; i < node->rrset_count; ++i) {
		knot_rrset_t rrset = node_rrset_at(node, i);
		if (rrset.type == KNOT_RRTYPE_SOA) {
			continue;
		}

		int ret = knot_pkt_put(pkt, 0, &rrset, KNOT_PF_NOTRUNC | KNOT_PF_ORIGTTL);
		if (ret != KNOT_EOK) {
			/* If something failed, remember the current RR for later. */
			state->cur_rrset = i;
			return ret;
		}
		if (pkt->size > KNOT_WIRE_PTR_MAX) {
			// optimization: once the XFR DNS message is > 16 KiB, compression
			// is limited. Better wrap to next message.
			state->cur_rrset = i + 1;
			return KNOT_ESPACE;
		}
	}

	state->cur_rrset = 0;

	return KNOT_EOK;
}

static int axfr_process_node_tree(knot_pkt_t *pkt, const void *item,
                                  struct xfr_proc *state)
{
	assert(item != NULL);

	struct axfr_proc *axfr = (struct axfr_proc*)state;

	int ret = zone_tree_it_begin((zone_tree_t *)item, &axfr->it); // does nothing if already iterating

	/* Put responses. */
	while (ret == KNOT_EOK && !zone_tree_it_finished(&axfr->it)) {
		zone_node_t *node = zone_tree_it_val(&axfr->it);
		ret = axfr_put_rrsets(pkt, node, axfr);
		if (ret == KNOT_EOK) {
			zone_tree_it_next(&axfr->it);
		}
	}

	/* Finished all nodes. */
	if (ret == KNOT_EOK) {
		zone_tree_it_free(&axfr->it);
	}
	return ret;
}

static void axfr_query_cleanup(knotd_qdata_t *qdata)
{
	struct axfr_proc *axfr = (struct axfr_proc *)qdata->extra->ext;

	zone_tree_it_free(&axfr->it);
	ptrlist_free(&axfr->proc.nodes, qdata->mm);
	mm_free(qdata->mm, axfr);

	/* Allow zone changes (finished). */
	rcu_read_unlock();
}

static void axfr_answer_finished(knotd_qdata_t *qdata, knot_pkt_t *pkt, int state)
{
	struct xfr_proc *xfr = qdata->extra->ext;

	switch (state) {
	case KNOT_STATE_PRODUCE:
		xfr_stats_add(&xfr->stats, pkt->size);
		break;
	case KNOT_STATE_DONE:
		xfr_stats_add(&xfr->stats, pkt->size);
		xfr_stats_end(&xfr->stats);
		xfr_log_finished(ZONE_NAME(qdata), LOG_OPERATION_AXFR, LOG_DIRECTION_OUT,
				 REMOTE(qdata), PROTO(qdata), KEY(qdata), "", &xfr->stats);
		break;
	default:
		break;
	}
}

static knot_layer_state_t axfr_query_check(knotd_qdata_t *qdata)
{
	NS_NEED_ZONE(qdata, KNOT_RCODE_NOTAUTH);
	NS_NEED_AUTH(qdata, ACL_ACTION_TRANSFER);
	NS_NEED_ZONE_CONTENTS(qdata);

	return KNOT_STATE_DONE;
}

static int axfr_query_init(knotd_qdata_t *qdata)
{
	assert(qdata);

	/* Check AXFR query validity. */
	if (axfr_query_check(qdata) == KNOT_STATE_FAIL) {
		if (qdata->rcode == KNOT_RCODE_FORMERR) {
			return KNOT_EMALF;
		} else {
			return KNOT_EDENIED;
		}
	}

	if (zone_get_flag(qdata->extra->zone, ZONE_XFR_FROZEN, false)) {
		qdata->rcode = KNOT_RCODE_REFUSED;
		qdata->rcode_ede = KNOT_EDNS_EDE_NOT_READY;
		return KNOT_ETRYAGAIN;
	}

	/* Create transfer processing context. */
	knot_mm_t *mm = qdata->mm;
	struct axfr_proc *axfr = mm_alloc(mm, sizeof(struct axfr_proc));
	if (axfr == NULL) {
		return KNOT_ENOMEM;
	}
	memset(axfr, 0, sizeof(struct axfr_proc));
	init_list(&axfr->proc.nodes);

	/* Put data to process. */
	xfr_stats_begin(&axfr->proc.stats);
	const zone_contents_t *contents = qdata->extra->contents;
	/* Must be non-NULL for the first message. */
	assert(contents);
	ptrlist_add(&axfr->proc.nodes, contents->nodes, mm);
	/* Put NSEC3 data if exists. */
	if (!zone_tree_is_empty(contents->nsec3_nodes)) {
		ptrlist_add(&axfr->proc.nodes, contents->nsec3_nodes, mm);
	}

	/* Set up cleanup callback. */
	qdata->extra->ext = axfr;
	qdata->extra->ext_cleanup = &axfr_query_cleanup;
	qdata->extra->ext_finished = &axfr_answer_finished;

	/* No zone changes during multipacket answer (unlocked in axfr_answer_cleanup) */
	rcu_read_lock();

	return KNOT_EOK;
}

knot_layer_state_t axfr_process_query(knot_pkt_t *pkt, knotd_qdata_t *qdata)
{
	if (pkt == NULL || qdata == NULL) {
		return KNOT_STATE_FAIL;
	}

	/* AXFR over UDP isn't allowed, respond with NOTIMPL. */
	if (qdata->params->proto == KNOTD_QUERY_PROTO_UDP) {
		qdata->rcode = KNOT_RCODE_NOTIMPL;
		return KNOT_STATE_FAIL;
	}

	/* Initialize on first call. */
	struct axfr_proc *axfr = qdata->extra->ext;
	if (axfr == NULL) {
		int ret = axfr_query_init(qdata);
		switch (ret) {
		case KNOT_EOK:         /* OK */
			AXFROUT_LOG(LOG_INFO, qdata, "started, serial %u",
			            zone_contents_serial(qdata->extra->contents));
			break;
		case KNOT_EDENIED:     /* Not authorized, already logged. */
			return KNOT_STATE_FAIL;
		case KNOT_EMALF:       /* Malformed query. */
			AXFROUT_LOG(LOG_DEBUG, qdata, "malformed query");
			return KNOT_STATE_FAIL;
		case KNOT_ETRYAGAIN:   /* Outgoing AXFR temporarily disabled. */
			AXFROUT_LOG(LOG_INFO, qdata, "outgoing AXFR frozen");
			return KNOT_STATE_FAIL;
		default:
			AXFROUT_LOG(LOG_ERR, qdata, "failed to start (%s)",
			            knot_strerror(ret));
			return KNOT_STATE_FAIL;
		}
	}

	/* Reserve space for TSIG. */
	int ret = knot_pkt_reserve(pkt, knot_tsig_wire_size(&qdata->sign.tsig_key));
	if (ret != KNOT_EOK) {
		return KNOT_STATE_FAIL;
	}

	/* Answer current packet (or continue). */
	ret = xfr_process_list(pkt, &axfr_process_node_tree, qdata);
	switch (ret) {
	case KNOT_ESPACE: /* Couldn't write more, send packet and continue. */
		return KNOT_STATE_PRODUCE; /* Check for more. */
	case KNOT_EOK:    /* Last response. */
		return KNOT_STATE_DONE;
	default:          /* Generic error. */
		AXFROUT_LOG(LOG_ERR, qdata, "failed (%s)", knot_strerror(ret));
		return KNOT_STATE_FAIL;
	}
}
