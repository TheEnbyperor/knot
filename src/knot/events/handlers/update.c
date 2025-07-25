/*  Copyright (C) 2025 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include <assert.h>

#include "knot/conf/tools.h"
#include "knot/events/handlers.h"
#include "knot/nameserver/log.h"
#include "knot/nameserver/process_query.h"
#include "knot/query/capture.h"
#include "knot/query/requestor.h"
#include "knot/server/server.h"
#include "knot/updates/ddns.h"
#include "knot/zone/digest.h"
#include "knot/zone/zone.h"
#include "libdnssec/random.h"
#include "libknot/libknot.h"
#include "libknot/quic/quic_conn.h"
#include "libknot/quic/quic.h"
#include "libknot/quic/tls.h"
#include "contrib/net.h"
#include "contrib/time.h"

#define UPDATE_LOG(priority, qdata, fmt...) \
	ns_log(priority, knot_pkt_qname(qdata->query), LOG_OPERATION_UPDATE, \
	       LOG_DIRECTION_IN, (qdata)->params->remote, \
	       (qdata)->params->proto, false, (qdata)->sign.tsig_key.name, fmt)

static void init_qdata_from_request(knotd_qdata_t *qdata,
                                    zone_t *zone,
                                    knot_request_t *req,
                                    knotd_qdata_params_t *params,
                                    knotd_qdata_extra_t *extra)
{
	memset(qdata, 0, sizeof(*qdata));
	qdata->params = params;
	qdata->query = req->query;
	qdata->sign = req->sign;
	qdata->extra = extra;
	memset(extra, 0, sizeof(*extra));
	qdata->extra->zone = zone;
}

#ifdef ENABLE_QUIC
static int ddnsq_alloc_reply(knot_quic_reply_t *r)
{
	r->out_payload->iov_len = KNOT_WIRE_MAX_PKTSIZE;

	return KNOT_EOK;
}

static int ddnsq_send_reply(knot_quic_reply_t *r)
{
	int fd = *(int *)r->sock;
	int ret = net_dgram_send(fd, r->out_payload->iov_base, r->out_payload->iov_len, r->ip_rem);
	if (ret < 0) {
		return knot_map_errno();
	} else if (ret == r->out_payload->iov_len) {
		return KNOT_EOK;
	} else {
		return KNOT_NET_EAGAIN;
	}
}

static void ddnsq_free_reply(knot_quic_reply_t *r)
{
	r->out_payload->iov_len = 0;
}
#endif // ENABLE_QUIC

static int check_prereqs(knot_request_t *request,
                         zone_update_t *update,
                         knotd_qdata_t *qdata)
{
	uint16_t rcode = KNOT_RCODE_NOERROR;
	int ret = ddns_process_prereqs(request->query, update, &rcode);
	if (ret != KNOT_EOK) {
		UPDATE_LOG(LOG_WARNING, qdata, "prerequisites not met (%s)",
		           knot_strerror(ret));
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		return ret;
	}

	ret = ddns_precheck_update(request->query, update, &rcode);
	if (ret != KNOT_EOK) {
		UPDATE_LOG(LOG_WARNING, qdata, "broken update format (%s)",
		           knot_strerror(ret));
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		return ret;
	}

	return KNOT_EOK;
}

static int process_single_update(knot_request_t *request,
                                 zone_update_t *update,
                                 knotd_qdata_t *qdata)
{
	uint16_t rcode = KNOT_RCODE_NOERROR;
	int ret = ddns_process_update(request->query, update, &rcode);
	if (ret != KNOT_EOK) {
		UPDATE_LOG(LOG_WARNING, qdata, "failed to apply (%s)",
		           knot_strerror(ret));
		assert(rcode != KNOT_RCODE_NOERROR);
		knot_wire_set_rcode(request->resp->wire, rcode);
		return ret;
	}

	return KNOT_EOK;
}

static void set_rcodes(list_t *requests, const uint16_t rcode)
{
	ptrnode_t *node;
	WALK_LIST(node, *requests) {
		knot_request_t *req = node->d;
		if (knot_wire_get_rcode(req->resp->wire) == KNOT_RCODE_NOERROR) {
			knot_wire_set_rcode(req->resp->wire, rcode);
		}
	}
}

static int process_bulk(zone_t *zone, list_t *requests, zone_update_t *up)
{
	// Walk all the requests and process.
	ptrnode_t *node;
	WALK_LIST(node, *requests) {
		knot_request_t *req = node->d;
		// Init qdata structure for logging (unique per-request).
		knotd_qdata_params_t params = {
			.proto = flags2proto(req->flags),
			.remote = &req->remote
		};
		knotd_qdata_t qdata;
		knotd_qdata_extra_t extra;
		init_qdata_from_request(&qdata, zone, req, &params, &extra);

		int ret = check_prereqs(req, up, &qdata);
		if (ret != KNOT_EOK) {
			// Skip updates with failed prereqs.
			continue;
		}

		ret = process_single_update(req, up, &qdata);
		if (ret != KNOT_EOK) {
			log_zone_error(zone->name, "DDNS, dropping %zu updates in a bulk",
			               list_size(requests));
			return ret;
		}
	}

	return KNOT_EOK;
}

static int process_normal(conf_t *conf, zone_t *zone, list_t *requests)
{
	assert(requests);

	// Init zone update structure
	zone_update_t up;
	zone_update_flags_t type = (zone->contents == NULL) ? UPDATE_FULL :
	                           UPDATE_INCREMENTAL | UPDATE_NO_CHSET;
	int ret = zone_update_init(&up, zone, type);
	if (ret != KNOT_EOK) {
		set_rcodes(requests, KNOT_RCODE_SERVFAIL);
		return ret;
	}

	// Process all updates.
	ret = process_bulk(zone, requests, &up);
	if (ret == KNOT_EOK && !node_rrtype_exists(up.new_cont->apex, KNOT_RRTYPE_SOA)) {
		ret = KNOT_ESEMCHECK;
	}
	if (ret == KNOT_EOK) {
		ret = zone_update_verify_digest(conf, &up);
	}
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		set_rcodes(requests, KNOT_RCODE_SERVFAIL);
		return ret;
	}

	// Sign update.
	conf_val_t val = conf_zone_get(conf, C_DNSSEC_SIGNING, zone->name);
	bool dnssec_enable = conf_bool(&val);
	val = conf_zone_get(conf, C_ZONEMD_GENERATE, zone->name);
	unsigned digest_alg = conf_opt(&val);
	if (dnssec_enable) {
		if (up.flags & UPDATE_FULL) {
			zone_sign_reschedule_t resch = { 0 };
			zone_sign_roll_flags_t rflags = KEY_ROLL_ALLOW_ALL;
			ret = knot_dnssec_zone_sign(&up, conf, 0, rflags, 0, &resch);
			event_dnssec_reschedule(conf, zone, &resch, false);
		} else {
			ret = knot_dnssec_sign_update(&up, conf);
		}
	} else if (digest_alg != ZONE_DIGEST_NONE) {
		if (zone_update_to(&up) == NULL) {
			ret = zone_update_increment_soa(&up, conf);
		}
		if (ret == KNOT_EOK) {
			ret = zone_update_add_digest(&up, digest_alg, false);
		}
	}
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		set_rcodes(requests, KNOT_RCODE_SERVFAIL);
		return ret;
	}

	// Apply changes.
	ret = zone_update_commit(conf, &up);
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		if (ret == KNOT_EZONESIZE) {
			set_rcodes(requests, KNOT_RCODE_REFUSED);
		} else {
			set_rcodes(requests, KNOT_RCODE_SERVFAIL);
		}
		return ret;
	}

	return KNOT_EOK;
}

static void process_requests(conf_t *conf, zone_t *zone, list_t *requests)
{
	assert(zone);
	assert(requests);

	/* Keep original state. */
	struct timespec t_start = time_now();
	const uint32_t old_serial = zone_contents_serial(zone->contents);

	/* Process authenticated packet. */
	int ret = process_normal(conf, zone, requests);
	if (ret != KNOT_EOK) {
		log_zone_error(zone->name, "DDNS, processing failed (%s)",
		               knot_strerror(ret));
		return;
	}

	/* Evaluate response. */
	const uint32_t new_serial = zone_contents_serial(zone->contents);
	if (new_serial == old_serial) {
		log_zone_info(zone->name, "DDNS, finished, no changes to the zone were made");
		return;
	}

	struct timespec t_end = time_now();
	log_zone_info(zone->name, "DDNS, finished, serial %u -> %u, "
	              "%.02f seconds", old_serial, new_serial,
	              time_diff_ms(&t_start, &t_end) / 1000.0);

	zone_schedule_notify(zone, 1);
}

static int remote_forward(conf_t *conf, knot_request_t *request, conf_remote_t *remote,
                          zone_t *zone)
{
	/* Copy request and assign new ID. */
	knot_pkt_t *query = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, NULL);
	int ret = knot_pkt_copy(query, request->query);
	if (ret != KNOT_EOK) {
		knot_pkt_free(query);
		return ret;
	}
	knot_wire_set_id(query->wire, dnssec_random_uint16_t());

	/* Prepare packet capture layer. */
	const knot_layer_api_t *capture = query_capture_api();
	struct capture_param capture_param = {
		.sink = request->resp
	};

	/* Create requestor instance. */
	knot_requestor_t re;
	ret = knot_requestor_init(&re, capture, &capture_param, NULL);
	if (ret != KNOT_EOK) {
		knot_pkt_free(query);
		return ret;
	}

	/* Create a request. */
	knot_request_flag_t flags = conf->cache.srv_tcp_fastopen ? KNOT_REQUEST_TFO : 0;
	if (request->query->tsig_rr != NULL && request->sign.tsig_key.secret.size == 0) {
		// Put the TSIG back on the wire as it was removed when parsing in pkt copy.
		knot_tsig_append(query->wire, &query->size, query->max_size, query->tsig_rr);
		flags |= KNOT_REQUEST_FWD;
	}
	knot_request_t *req = knot_request_make(NULL, remote, query,
	                                        zone->server->quic_creds, NULL, flags);
	if (req == NULL) {
		knot_requestor_clear(&re);
		knot_pkt_free(query);
		return KNOT_ENOMEM;
	}

	/* Execute the request. */
	int timeout = conf->cache.srv_tcp_remote_io_timeout;
	ret = knot_requestor_exec(&re, req, timeout);

	knot_request_free(req, NULL);
	knot_requestor_clear(&re);

	return ret;
}

static void forward_request(conf_t *conf, zone_t *zone, knot_request_t *request)
{
	/* Read the ddns master or the first master. */
	conf_val_t *remote;
	conf_mix_iter_t iter;
	conf_val_t master_val = conf_zone_get(conf, C_DDNS_MASTER, zone->name);
	if (master_val.code == KNOT_EOK) {
		remote = &master_val;
	} else {
		master_val = conf_zone_get(conf, C_MASTER, zone->name);
		conf_mix_iter_init(conf, &master_val, &iter);
		remote = iter.id;
	}

	/* Get the number of remote addresses. */
	conf_val_t addr = conf_id_get(conf, C_RMT, C_ADDR, remote);
	size_t addr_count = conf_val_count(&addr);
	assert(addr_count > 0);

	/* Try all remote addresses to forward the request to. */
	int ret = KNOT_EOK;
	for (size_t i = 0; i < addr_count; i++) {
		conf_remote_t master = conf_remote(conf, remote, i);

		ret = remote_forward(conf, request, &master, zone);
		if (ret == KNOT_EOK) {
			break;
		}
	}

	/* Restore message ID. */
	knot_wire_set_id(request->resp->wire, knot_wire_get_id(request->query->wire));
	if (request->query->tsig_rr != NULL && request->sign.tsig_key.secret.size == 0) {
		/* Put the remote signature back on the response wire. */
		knot_tsig_append(request->resp->wire, &request->resp->size,
		                 request->resp->max_size, request->resp->tsig_rr);
	}

	/* Set RCODE if forwarding failed. */
	if (ret != KNOT_EOK) {
		knot_wire_set_rcode(request->resp->wire, KNOT_RCODE_SERVFAIL);
		log_zone_error(zone->name, "DDNS, failed to forward updates to the master (%s)",
		               knot_strerror(ret));
	} else {
		log_zone_info(zone->name, "DDNS, updates forwarded to the master");
	}
}

static void forward_requests(conf_t *conf, zone_t *zone, list_t *requests)
{
	assert(zone);
	assert(requests);

	ptrnode_t *node;
	WALK_LIST(node, *requests) {
		knot_request_t *req = node->d;
		forward_request(conf, zone, req);
	}
}

static void send_update_response(conf_t *conf, zone_t *zone, knot_request_t *req)
{
	if (req->resp) {
		// Sign the response if the secret is known.
		if (req->sign.tsig_key.secret.size > 0) {
			knotd_qdata_t qdata;
			knotd_qdata_extra_t extra;
			init_qdata_from_request(&qdata, zone, req, NULL, &extra);
			(void)process_query_sign_response(req->resp, &qdata);
		}

		if (net_is_stream(req->fd) && req->tls_req_ctx.conn != NULL) {
			(void)knot_tls_send_dns(req->tls_req_ctx.conn,
			                        req->resp->wire, req->resp->size);
			knot_tls_conn_block(req->tls_req_ctx.conn, false);
		}
#ifdef ENABLE_QUIC
		else if (req->quic_conn != NULL) {
			assert(!net_is_stream(req->fd));
			uint8_t op_buf[KNOT_WIRE_MAX_PKTSIZE];
			struct iovec out_payload = { .iov_base = op_buf, .iov_len = sizeof(op_buf) };
			knot_quic_reply_t rpl = {
				.ip_rem = &req->remote,
				.ip_loc = &req->source,
				.in_payload = NULL,
				.out_payload = &out_payload,
				.sock = &req->fd,
				.alloc_reply = ddnsq_alloc_reply,
				.send_reply = ddnsq_send_reply,
				.free_reply = ddnsq_free_reply
			};

			void *succ = knot_quic_stream_add_data(req->quic_conn, req->quic_stream,
			                                       req->resp->wire, req->resp->size);
			if (succ != NULL) { // else ENOMEM
				(void)knot_quic_send(req->quic_conn->quic_table, req->quic_conn,
				                     &rpl, 4, KNOT_QUIC_SEND_IGNORE_BLOCKED);
			}
			knot_quic_conn_block(req->quic_conn, false);
		} else // NOTE ties to 'if' below
#else
		assert(req->quic_conn == NULL);
#endif // ENABLE_QUIC

		if (net_is_stream(req->fd)) {
			net_dns_tcp_send(req->fd, req->resp->wire, req->resp->size,
			                 conf->cache.srv_tcp_remote_io_timeout, NULL);
		} else {
			net_dgram_send(req->fd, req->resp->wire, req->resp->size,
			               &req->remote);
		}
	}
}

static void send_update_responses(conf_t *conf, zone_t *zone, list_t *updates)
{
	ptrnode_t *node, *nxt;
	WALK_LIST_DELSAFE(node, nxt, *updates) {
		knot_request_t *req = node->d;
		send_update_response(conf, zone, req);
		knot_request_free(req, NULL);
	}
	ptrlist_free(updates, NULL);
}

static int init_update_responses(list_t *updates)
{
	ptrnode_t *node, *nxt;
	WALK_LIST_DELSAFE(node, nxt, *updates) {
		knot_request_t *req = node->d;
		req->resp = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, NULL);
		if (req->resp == NULL) {
			return KNOT_ENOMEM;
		}

		assert(req->query);
		knot_pkt_init_response(req->resp, req->query);
	}

	return KNOT_EOK;
}

static size_t update_dequeue(zone_t *zone, list_t *updates)
{
	assert(zone);
	assert(updates);

	pthread_mutex_lock(&zone->ddns_lock);

	if (EMPTY_LIST(zone->ddns_queue)) {
		/* Lost race during reload. */
		pthread_mutex_unlock(&zone->ddns_lock);
		return 0;
	}

	*updates = zone->ddns_queue;
	size_t update_count = zone->ddns_queue_size;
	init_list(&zone->ddns_queue);
	zone->ddns_queue_size = 0;

	pthread_mutex_unlock(&zone->ddns_lock);

	return update_count;
}

int event_update(conf_t *conf, zone_t *zone)
{
	assert(zone);

	/* Get list of pending updates. */
	list_t updates;
	size_t update_count = update_dequeue(zone, &updates);
	if (update_count == 0) {
		return KNOT_EOK;
	}

	/* Init updates responses. */
	int ret = init_update_responses(&updates);
	if (ret != KNOT_EOK) {
		/* Send what responses we can. */
		set_rcodes(&updates, KNOT_RCODE_SERVFAIL);
		send_update_responses(conf, zone, &updates);
		return ret;
	}

	bool forward = false;
	if (zone_is_slave(conf, zone)) {
		conf_val_t ddnsmaster = conf_zone_get(conf, C_DDNS_MASTER, zone->name);
		if (ddnsmaster.code != KNOT_EOK || *conf_str(&ddnsmaster) != '\0') {
			forward = true;
		}
	}

	/* Process update list - forward if zone has master, or execute.
	   RCODEs are set. */
	if (forward) {
		log_zone_info(zone->name,
		              "DDNS, forwarding %zu updates", update_count);
		forward_requests(conf, zone, &updates);
	} else {
		log_zone_info(zone->name,
		              "DDNS, processing %zu updates", update_count);
		process_requests(conf, zone, &updates);
	}

	/* Send responses. */
	send_update_responses(conf, zone, &updates);

	return KNOT_EOK;
}
