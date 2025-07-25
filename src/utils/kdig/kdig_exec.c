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

#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "utils/kdig/kdig_exec.h"
#include "utils/common/exec.h"
#include "utils/common/msg.h"
#include "utils/common/netio.h"
#include "utils/common/sign.h"
#include "libknot/libknot.h"
#include "contrib/json.h"
#include "contrib/sockaddr.h"
#include "contrib/time.h"
#include "contrib/ucw/lists.h"

#if USE_DNSTAP
#include "contrib/dnstap/convert.h"
#include "contrib/dnstap/message.h"
#include "contrib/dnstap/writer.h"
#include "libknot/probe/data.h"

static int write_dnstap(dt_writer_t           *writer,
                        const bool            is_query,
                        const uint8_t         *wire,
                        const size_t          wire_len,
                        net_t                 *net,
                        const struct timespec *mtime)
{
	Dnstap__Message       msg;
	Dnstap__Message__Type msg_type;
	int                   ret;
	int                   protocol = 0;

	if (writer == NULL) {
		return KNOT_EOK;
	}

	if (net->local == NULL) {
		net_set_local_info(net);
	}

	msg_type = is_query ? DNSTAP__MESSAGE__TYPE__TOOL_QUERY :
	                      DNSTAP__MESSAGE__TYPE__TOOL_RESPONSE;

	if (net->socktype == SOCK_DGRAM) {
		protocol = IPPROTO_UDP;
	} else if (net->socktype == SOCK_STREAM) {
		protocol = IPPROTO_TCP;
	}

	ret = dt_message_fill(&msg, msg_type, net->local_info->ai_addr,
	                      net->srv->ai_addr, protocol,
	                      wire, wire_len, mtime);
	if (ret != KNOT_EOK) {
		return ret;
	}

	return dt_writer_write(writer, (const ProtobufCMessage *)&msg);
}

static float get_query_time(const Dnstap__Dnstap *frame)
{
	if (!frame->message->has_query_time_sec ||
	    !frame->message->has_query_time_nsec ||
	    !frame->message->has_response_time_sec ||
	    !frame->message->has_response_time_sec) {
		return 0;
	}

	struct timespec from = {
		.tv_sec = frame->message->query_time_sec,
		.tv_nsec = frame->message->query_time_nsec
	};

	struct timespec to = {
		.tv_sec = frame->message->response_time_sec,
		.tv_nsec = frame->message->response_time_nsec
	};

	return time_diff_ms(&from, &to);
}

static void fill_remote_addr(net_t *net, Dnstap__Message *message, bool is_initiator)
{
	if (!message->has_socket_family || !message->has_socket_protocol) {
		return;
	}

	if ((message->response_address.data == NULL && is_initiator) ||
	     message->query_address.data == NULL) {
		return;
	}

	struct sockaddr_storage ss = { 0 };
	int family = dt_family_decode(message->socket_family);
	knot_probe_proto_t proto = dt_protocol_decode(message->socket_protocol);

	ProtobufCBinaryData *addr = NULL;
	uint32_t port = 0;
	if (is_initiator) {
		addr = &message->response_address;
		port = message->response_port;
	} else {
		addr = &message->query_address;
		port = message->query_port;
	}

	sockaddr_set_raw(&ss, family, addr->data, addr->len);
	sockaddr_port_set(&ss, port);

	get_addr_str(&ss, proto, &net->remote_str);
}

static int process_dnstap(const query_t *query)
{
	dt_reader_t *reader = query->dt_reader;

	if (query->dt_reader == NULL) {
		return -1;
	}

	bool first_message = true;

	for (;;) {
		Dnstap__Dnstap      *frame = NULL;
		Dnstap__Message     *message = NULL;
		ProtobufCBinaryData *wire = NULL;
		bool                is_query;
		bool                is_initiator;

		// Read next message.
		int ret = dt_reader_read(reader, &frame);
		if (ret == KNOT_EOF) {
			break;
		} else if (ret != KNOT_EOK) {
			ERR("can't read dnstap message");
			break;
		}

		// Check for dnstap message.
		if (frame->type == DNSTAP__DNSTAP__TYPE__MESSAGE) {
			message = frame->message;
		} else {
			WARN("ignoring non-dnstap message");
			dt_reader_free_frame(reader, &frame);
			continue;
		}

		// Check for the type of dnstap message.
		if (message->has_response_message) {
			wire = &message->response_message;
			is_query = false;
		} else if (message->has_query_message) {
			wire = &message->query_message;
			is_query = true;
		} else {
			WARN("dnstap frame contains no message");
			dt_reader_free_frame(reader, &frame);
			continue;
		}

		// Ignore query message if requested.
		if (is_query && !query->style.show_query) {
			dt_reader_free_frame(reader, &frame);
			continue;
		}

		// Get the message role.
		is_initiator = dt_message_role_is_initiator(message->type);

		// Create dns packet based on dnstap wire data.
		knot_pkt_t *pkt = knot_pkt_new(wire->data, wire->len, NULL);
		if (pkt == NULL) {
			ERR("can't allocate packet");
			dt_reader_free_frame(reader, &frame);
			break;
		}

		// Parse packet and reconstruct required data.
		ret = knot_pkt_parse(pkt, KNOT_PF_NOCANON);
		if (ret == KNOT_EOK || ret == KNOT_ETRAIL) {
			time_t timestamp = 0;
			float  query_time = 0.0;
			net_t  net_ctx = { 0 };

			if (ret == KNOT_ETRAIL) {
				WARN("malformed message (%s)", knot_strerror(ret));
			}

			if (is_query) {
				if (message->has_query_time_sec) {
					timestamp = message->query_time_sec;
				}
			} else {
				if (message->has_response_time_sec) {
					timestamp = message->response_time_sec;
				}
				query_time = get_query_time(frame);
			}

			// Prepare connection information string.
			fill_remote_addr(&net_ctx, message, is_initiator);

			if (first_message) {
				first_message = false;
			} else {
				printf("\n");
			}

			print_packet(pkt, &net_ctx, pkt->size, query_time, timestamp,
			             is_query ^ is_initiator, &query->style);

			net_clean(&net_ctx);
		} else {
			ERR("can't print dnstap message");
		}

		knot_pkt_free(pkt);
		dt_reader_free_frame(reader, &frame);
	}

	return 0;
}
#endif // USE_DNSTAP

static int add_query_edns(knot_pkt_t *packet, const query_t *query, uint16_t max_size)
{
	/* Initialize OPT RR. */
	knot_rrset_t opt_rr;
	int ret = knot_edns_init(&opt_rr, max_size, 0,
	                         query->edns > -1 ? query->edns : 0, &packet->mm);
	if (ret != KNOT_EOK) {
		return ret;
	}

	if (query->flags.do_flag) {
		knot_edns_set_do(&opt_rr);
	}

	/* Append NSID. */
	if (query->nsid) {
		ret = knot_edns_add_option(&opt_rr, KNOT_EDNS_OPTION_NSID,
		                           0, NULL, &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}
	}

	/* Append Zone version. */
	if (query->zoneversion) {
		ret = knot_edns_add_option(&opt_rr, KNOT_EDNS_OPTION_ZONEVERSION,
		                           0, NULL, &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}
	}

	/* Append EDNS-client-subnet. */
	if (query->subnet.family != AF_UNSPEC) {
		uint16_t size = knot_edns_client_subnet_size(&query->subnet);
		uint8_t data[size];

		ret = knot_edns_client_subnet_write(data, size, &query->subnet);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}

		ret = knot_edns_add_option(&opt_rr, KNOT_EDNS_OPTION_CLIENT_SUBNET,
		                           size, data, &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}
	}

	/* Append a cookie option if present. */
	if (query->cc.len > 0) {
		uint16_t size = knot_edns_cookie_size(&query->cc, &query->sc);
		uint8_t data[size];

		ret = knot_edns_cookie_write(data, size, &query->cc, &query->sc);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}

		ret = knot_edns_add_option(&opt_rr, KNOT_EDNS_OPTION_COOKIE,
		                           size, data, &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}
	}

	/* Append EDNS Padding. */
	int padding = query->padding;
	if (padding != -3 && query->alignment > 0) {
		padding = knot_edns_alignment_size(packet->size,
		                                   knot_rrset_size(&opt_rr),
		                                   query->alignment);
	} else if (query->padding == -2 || (query->padding == -1 && query->tls.enable)) {
		padding = knot_pkt_default_padding_size(packet, &opt_rr);
	}
	if (padding > -1) {
		uint8_t zeros[padding];
		memset(zeros, 0, sizeof(zeros));

		ret = knot_edns_add_option(&opt_rr, KNOT_EDNS_OPTION_PADDING,
		                           padding, zeros, &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}
	}

	/* Append custom EDNS options. */
	node_t *node;
	WALK_LIST(node, query->edns_opts) {
		ednsopt_t *opt = (ednsopt_t *)node;
		ret = knot_edns_add_option(&opt_rr, opt->code, opt->length,
		                           opt->data, &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_clear(&opt_rr, &packet->mm);
			return ret;
		}
	}

	/* Add prepared OPT to packet. */
	ret = knot_pkt_put(packet, KNOT_COMPR_HINT_NONE, &opt_rr, KNOT_PF_FREE);
	if (ret != KNOT_EOK) {
		knot_rrset_clear(&opt_rr, &packet->mm);
	}

	return ret;
}

static bool do_padding(const query_t *query)
{
	return (query->padding != -3) &&                       // Disabled padding.
	       (query->padding > -1 || query->alignment > 0 || // Explicit padding.
	        query->padding == -2 ||                        // Default padding.
	        (query->padding == -1 && query->tls.enable));  // TLS automatic.
}

static bool use_edns(const query_t *query)
{
	return query->edns > -1 || query->udp_size > -1 || query->nsid ||
	       query->zoneversion || query->subnet.family != AF_UNSPEC ||
	       query->flags.do_flag || query->cc.len > 0 || do_padding(query) ||
	       !ednsopt_list_empty(&query->edns_opts);
}

static knot_pkt_t *create_query_packet(const query_t *query)
{
	// Set packet buffer size.
	uint16_t max_size;
	if (query->udp_size < 0) {
		if (use_edns(query)) {
			max_size = DEFAULT_EDNS_SIZE;
		} else {
			max_size = DEFAULT_UDP_SIZE;
		}
	} else {
		max_size = query->udp_size;
	}

	// Create packet skeleton.
	knot_pkt_t *packet = create_empty_packet(max_size);
	if (packet == NULL) {
		return NULL;
	}

	// Set ID = 0 for packet send over HTTPS
	// Due HTTP cache it is convenient to set the query ID to 0 - GET messages has same header then
#if defined(LIBNGHTTP2) || defined(ENABLE_QUIC)
	if (query->https.enable || query->quic.enable) {
		knot_wire_set_id(packet->wire, 0);
	}
#endif

	// Set flags to wireformat.
	if (query->flags.aa_flag) {
		knot_wire_set_aa(packet->wire);
	}
	if (query->flags.tc_flag) {
		knot_wire_set_tc(packet->wire);
	}
	if (query->flags.rd_flag) {
		knot_wire_set_rd(packet->wire);
	}
	if (query->flags.ra_flag) {
		knot_wire_set_ra(packet->wire);
	}
	if (query->flags.z_flag) {
		knot_wire_set_z(packet->wire);
	}
	if (query->flags.ad_flag) {
		knot_wire_set_ad(packet->wire);
	}
	if (query->flags.cd_flag) {
		knot_wire_set_cd(packet->wire);
	}

	// Set NOTIFY opcode.
	if (query->notify) {
		knot_wire_set_opcode(packet->wire, KNOT_OPCODE_NOTIFY);
	}

	// Set packet question if available.
	knot_dname_t *qname = NULL;
	if (query->owner != NULL) {
		qname = knot_dname_from_str_alloc(query->owner);
		if (qname == NULL) {
			ERR("'%s' is not a valid domain name", query->owner);
			knot_pkt_free(packet);
			return NULL;
		}

		int ret = knot_pkt_put_question(packet, qname, query->class_num,
		                                query->type_num);
		if (ret != KNOT_EOK) {
			knot_dname_free(qname, NULL);
			knot_pkt_free(packet);
			return NULL;
		}
	}

	// For IXFR query or NOTIFY query with SOA serial, add a proper section.
	if (query->serial >= 0) {
		if (query->notify) {
			knot_pkt_begin(packet, KNOT_ANSWER);
		} else {
			knot_pkt_begin(packet, KNOT_AUTHORITY);
		}

		// SOA rdata in wireformat.
		uint8_t wire[22] = { 0x0 };

		// Create rrset with SOA record.
		knot_rrset_t *soa = knot_rrset_new(qname,
		                                   KNOT_RRTYPE_SOA,
		                                   query->class_num,
		                                   0,
		                                   &packet->mm);
		knot_dname_free(qname, NULL);
		if (soa == NULL) {
			knot_pkt_free(packet);
			return NULL;
		}

		// Fill in blank SOA rdata to rrset.
		int ret = knot_rrset_add_rdata(soa, wire, sizeof(wire), &packet->mm);
		if (ret != KNOT_EOK) {
			knot_rrset_free(soa, &packet->mm);
			knot_pkt_free(packet);
			return NULL;
		}

		// Set SOA serial.
		knot_soa_serial_set(soa->rrs.rdata, query->serial);

		ret = knot_pkt_put(packet, KNOT_COMPR_HINT_NONE, soa, KNOT_PF_FREE);
		if (ret != KNOT_EOK) {
			knot_rrset_free(soa, &packet->mm);
			knot_pkt_free(packet);
			return NULL;
		}

		free(soa);
	} else {
		knot_dname_free(qname, NULL);
	}

	// Begin additional section
	knot_pkt_begin(packet, KNOT_ADDITIONAL);

	// Create EDNS section if required.
	if (use_edns(query)) {
		int ret = add_query_edns(packet, query, max_size);
		if (ret != KNOT_EOK) {
			ERR("can't set up EDNS section");
			knot_pkt_free(packet);
			return NULL;
		}
	}

	return packet;
}

static bool check_reply_id(const knot_pkt_t *reply,
                           const knot_pkt_t *query)
{
	uint16_t query_id = knot_wire_get_id(query->wire);
	uint16_t reply_id = knot_wire_get_id(reply->wire);

	if (reply_id != query_id) {
		WARN("reply ID (%u) is different from query ID (%u)",
		     reply_id, query_id);
		return false;
	}

	return true;
}

static void check_reply_qr(const knot_pkt_t *reply)
{
	if (!knot_wire_get_qr(reply->wire)) {
		WARN("response QR bit not set");
	}
}

static void check_reply_question(const knot_pkt_t *reply,
                                 const knot_pkt_t *query)
{
	if (knot_wire_get_qdcount(reply->wire) < 1) {
		WARN("response doesn't have question section");
		return;
	}

	if (!knot_dname_is_equal(knot_pkt_wire_qname(reply), knot_pkt_wire_qname(query)) ||
	    knot_pkt_qclass(reply) != knot_pkt_qclass(query) ||
	    knot_pkt_qtype(reply)  != knot_pkt_qtype(query)) {
		WARN("query/response question sections are different");
		return;
	}
}

static int64_t first_serial_check(const knot_pkt_t *reply, const knot_pkt_t *query)
{
	const knot_pktsection_t *answer = knot_pkt_section(reply, KNOT_ANSWER);

	if (answer->count <= 0) {
		return -1;
	}

	const knot_rrset_t *first = knot_pkt_rr(answer, 0);

	if (first->type != KNOT_RRTYPE_SOA) {
		return -1;
	} else {
		if (!knot_dname_is_case_equal(first->owner, knot_pkt_qname(query))) {
			WARN("leading SOA owner not matching the requested zone name");
		}

		return knot_soa_serial(first->rrs.rdata);
	}
}

static bool finished_xfr(const uint32_t serial, const knot_pkt_t *reply,
                         const knot_pkt_t *query, const size_t msg_count, bool is_ixfr)
{
	const knot_pktsection_t *answer = knot_pkt_section(reply, KNOT_ANSWER);

	if (answer->count <= 0) {
		return false;
	}

	const knot_rrset_t *last = knot_pkt_rr(answer, answer->count - 1);

	if (last->type != KNOT_RRTYPE_SOA) {
		return false;
	} else if (answer->count == 1 && msg_count == 1) {
		return is_ixfr;
	} else {
		if (!knot_dname_is_case_equal(last->owner, knot_pkt_qname(query))) {
			WARN("final SOA owner not matching the requested zone name");
		}

		return knot_soa_serial(last->rrs.rdata) == serial;
	}
}

static int sign_query(knot_pkt_t *pkt, const query_t *query, sign_context_t *ctx)
{
	if (query->tsig_key.name == NULL) {
		return KNOT_EOK;
	}

	int ret = sign_context_init_tsig(ctx, &query->tsig_key);
	if (ret != KNOT_EOK) {
		return ret;
	}

	ret = sign_packet(pkt, ctx);
	if (ret != KNOT_EOK) {
		sign_context_deinit(ctx);
		return ret;
	}

	return KNOT_EOK;
}

static void net_close_keepopen(net_t *net, const query_t *query)
{
	if (!query->keepopen) {
		net_close(net);
	}
}

static int process_query_packet(const knot_pkt_t      *query,
                                net_t                 *net,
                                const query_t         *query_ctx,
                                const bool            ignore_tc,
                                const sign_context_t  *sign_ctx,
                                const style_t         *style)
{
	struct timespec	t_start, t_query, t_end;
	time_t		timestamp;
	knot_pkt_t	*reply = NULL;
	uint8_t		in[MAX_PACKET_SIZE];
	int		in_len = 0;
	int		ret;

	// Get start query time.
	timestamp = time(NULL);
	t_start = time_now();

	// Connect to the server if not already connected.
	if (net->sockfd < 0) {
		ret = net_connect(net);
		if (ret != KNOT_EOK) {
			return -1;
		}
	}

	// Send query packet.
	ret = net_send(net, query->wire, query->size);
	if (ret != KNOT_EOK) {
		net_close(net);
		return -1;
	}

	// Get stop query time and start reply time.
	t_query = time_now();

#if USE_DNSTAP
	struct timespec t_query_full = time_diff(&t_start, &t_query);
	t_query_full.tv_sec += timestamp;

	// Make the dnstap copy of the query.
	write_dnstap(query_ctx->dt_writer, true, query->wire, query->size,
	             net, &t_query_full);
#endif // USE_DNSTAP

	// Print query packet if required.
	if (style->show_query && style->format != FORMAT_JSON) {
		// Create copy of query packet for parsing.
		knot_pkt_t *q = knot_pkt_new(query->wire, query->size, NULL);
		if (q != NULL) {
			if (knot_pkt_parse(q, KNOT_PF_NOCANON) == KNOT_EOK) {
				print_packet(q, net, query->size,
				             time_diff_ms(&t_start, &t_query),
				             timestamp, false, style);
			} else {
				ERR("can't print query packet");
			}
			knot_pkt_free(q);
		} else {
			ERR("can't print query packet");
		}

		printf("\n");
	}

	// Loop over incoming messages, unless reply id is correct or timeout.
	while (true) {
		reply = NULL;

		// Receive a reply message.
		in_len = net_receive(net, in, sizeof(in));
		t_end = time_now();
		if (in_len <= 0) {
			goto fail;
		}

#if USE_DNSTAP
		struct timespec t_end_full = time_diff(&t_start, &t_end);
		t_end_full.tv_sec += timestamp;

		// Make the dnstap copy of the response.
		write_dnstap(query_ctx->dt_writer, false, in, in_len, net,
		             &t_end_full);
#endif // USE_DNSTAP

		// Create reply packet structure to fill up.
		reply = knot_pkt_new(in, in_len, NULL);
		if (reply == NULL) {
			ERR("internal error (%s)", knot_strerror(KNOT_ENOMEM));
			goto fail;
		}

		// Parse reply to the packet structure.
		ret = knot_pkt_parse(reply, KNOT_PF_NOCANON);
		if (ret == KNOT_ETRAIL) {
			WARN("malformed reply packet (%s)", knot_strerror(ret));
		} else if (ret != KNOT_EOK) {
			ERR("malformed reply packet from %s", net->remote_str);
			goto fail;
		}

		// Compare reply header id.
		if (check_reply_id(reply, query)) {
			break;
		// Check for timeout.
		} else if (time_diff_ms(&t_query, &t_end) > 1000 * net->wait) {
			goto fail;
		}

		knot_pkt_free(reply);
	}

	// Check for TC bit and repeat query with TCP if required.
	if (knot_wire_get_tc(reply->wire) != 0 &&
	    ignore_tc == false && net->socktype == SOCK_DGRAM) {
		printf("\n");
		WARN("truncated reply from %s, retrying over TCP\n",
		     net->remote_str);
		knot_pkt_free(reply);
		net_close_keepopen(net, query_ctx);

		net->socktype = SOCK_STREAM;

		return process_query_packet(query, net, query_ctx, true,
		                            sign_ctx, style);
	}

	// Check for question sections equality.
	check_reply_question(reply, query);

	// Check QR bit
	check_reply_qr(reply);

	// Print reply packet.
	if (style->format != FORMAT_JSON) {
		// Intentionaly start-end because of QUIC can have receive time.
		print_packet(reply, net, in_len, time_diff_ms(&t_start, &t_end),
		             timestamp, true, style);
	} else {
		knot_pkt_t *q = knot_pkt_new(query->wire, query->size, NULL);
		(void)knot_pkt_parse(q, KNOT_PF_NOCANON);
		print_packets_json(q, reply, net, timestamp, style);
		knot_pkt_free(q);
	}

	// Verify signature if a key was specified.
	if (sign_ctx->digest != NULL) {
		ret = verify_packet(reply, sign_ctx);
		if (ret != KNOT_EOK) {
			WARN("reply verification for %s (%s)",
			     net->remote_str, knot_strerror(ret));
		}
	}

	// Check for BADCOOKIE RCODE and repeat query with the new cookie if required.
	if (knot_pkt_ext_rcode(reply) == KNOT_RCODE_BADCOOKIE && query_ctx->badcookie > 0) {
		printf("\n");
		WARN("bad cookie from %s, retrying with the received one\n",
		     net->remote_str);
		net_close_keepopen(net, query_ctx);

		// Prepare new query context.
		query_t new_ctx = *query_ctx;

		uint8_t *opt = knot_pkt_edns_option(reply, KNOT_EDNS_OPTION_COOKIE);
		if (opt == NULL) {
			ERR("bad cookie, missing EDNS section");
			goto fail;
		}

		const uint8_t *data = knot_edns_opt_get_data(opt);
		uint16_t data_len = knot_edns_opt_get_length(opt);
		ret = knot_edns_cookie_parse(&new_ctx.cc, &new_ctx.sc, data, data_len);
		if (ret != KNOT_EOK) {
			ERR("bad cookie, missing EDNS cookie option");
			goto fail;
		}
		knot_pkt_free(reply);

		// Restore the original client cookie.
		new_ctx.cc = query_ctx->cc;

		new_ctx.badcookie--;

		knot_pkt_t *new_query = create_query_packet(&new_ctx);
		ret = process_query_packet(new_query, net, &new_ctx, ignore_tc,
		                           sign_ctx, style);
		knot_pkt_free(new_query);

		return ret;
	}

	knot_pkt_free(reply);
	net_close_keepopen(net, query_ctx);

	return 0;

fail:
	if (style->format != FORMAT_JSON) {
		// Intentionaly start-end because of QUIC can have receive time.
		print_packet(reply, net, in_len, time_diff_ms(&t_start, &t_end),
		             timestamp, true, style);
	} else {
		knot_pkt_t *q = knot_pkt_new(query->wire, query->size, NULL);
		(void)knot_pkt_parse(q, KNOT_PF_NOCANON);
		print_packets_json(q, reply, net, timestamp, style);
		knot_pkt_free(q);
	}

	knot_pkt_free(reply);
	net_close(net);

	return -1;
}

static int process_query(const query_t *query, net_t *net)
{
	node_t     *server;
	knot_pkt_t *out_packet;
	int        ret;

	// Create query packet.
	out_packet = create_query_packet(query);
	if (out_packet == NULL) {
		ERR("can't create query packet");
		return -1;
	}

	// Sign the query.
	sign_context_t sign_ctx = { 0 };
	ret = sign_query(out_packet, query, &sign_ctx);
	if (ret != KNOT_EOK) {
		ERR("can't sign the packet (%s)", knot_strerror(ret));
		return -1;
	}

	// Reuse previous connection if available.
	if (net->sockfd >= 0) {
		DBG("Querying for owner(%s), class(%u), type(%u), reused connection",
		    query->owner, query->class_num, query->type_num);

		ret = process_query_packet(out_packet, net, query, query->ignore_tc,
		                           &sign_ctx, &query->style);
		goto finish;
	}

	// Get connection parameters.
	int socktype = get_socktype(query->protocol, query->type_num);
	int flags = query->fastopen ? NET_FLAGS_FASTOPEN : NET_FLAGS_NONE;

	// Loop over server list to process query.
	WALK_LIST(server, query->servers) {
		srv_info_t *remote = (srv_info_t *)server;
		int iptype = get_iptype(query->ip, remote);

		DBG("Querying for owner(%s), class(%u), type(%u), server(%s), "
		    "port(%s), protocol(%s)", query->owner, query->class_num,
		    query->type_num, remote->name, remote->service,
		    get_sockname(socktype));

		// Loop over the number of retries.
		for (size_t i = 0; i <= query->retries; i++) {
			// Initialize network structure for current server.
			ret = net_init(query->local, remote, iptype, socktype,
			               query->wait, flags,
			               (struct sockaddr *)&query->proxy.src,
			               (struct sockaddr *)&query->proxy.dst,
			               net);
			if (ret != KNOT_EOK) {
				if (ret == KNOT_NET_EADDR) {
					// Requested address family not available.
					goto next_server;
				}
				continue;
			}

			// Loop over all resolved addresses for remote.
			while (net->srv != NULL) {
				ret = net_init_crypto(net, &query->tls, &query->https,
				                      &query->quic);
				if (ret != 0) {
					ERR("failed to initialize crypto context (%s)",
					    knot_strerror(ret));
					break;
				}

				ret = process_query_packet(out_packet, net,
				                           query,
				                           query->ignore_tc,
				                           &sign_ctx,
				                           &query->style);
				// If error try next resolved address.
				if (ret != 0) {
					net->srv = net->srv->ai_next;
					if (net->srv != NULL && query->style.show_query) {
						printf("\n");
					}

					continue;
				}

				break;
			}

			// Success.
			if (ret == 0) {
				goto finish;
			}

			if (i < query->retries) {
				DBG("retrying server %s@%s(%s)",
				    remote->name, remote->service,
				    get_sockname(socktype));

				if (query->style.show_query) {
					printf("\n");
				}
			}

			net_clean(net);
		}

		ERR("failed to query server %s@%s(%s)",
		    remote->name, remote->service, get_sockname(socktype));

		// If not last server, print separation.
		if (server->next->next && query->style.show_query) {
			printf("\n");
		}
next_server:
		continue;
	}
finish:
	if (!query->keepopen || net->sockfd < 0) {
		net_clean(net);
	}
	sign_context_deinit(&sign_ctx);
	knot_pkt_free(out_packet);

	if (ret == KNOT_NET_EADDR) {
		WARN("no servers to query");
	}

	return ret;
}

static int process_xfr_packet(const knot_pkt_t      *query,
                              net_t                 *net,
                              const query_t         *query_ctx,
                              const sign_context_t  *sign_ctx,
                              const style_t         *style)
{
	struct timespec t_start, t_query, t_end;
	time_t		timestamp;
	knot_pkt_t      *reply = NULL;
	uint8_t         in[MAX_PACKET_SIZE];
	int             in_len;
	int             ret;
	int64_t         serial = 0;
	size_t          total_len = 0;
	size_t          msg_count = 0;
	size_t          rr_count = 0;
	jsonw_t         *w = NULL;

	// Get start query time.
	timestamp = time(NULL);
	t_start = time_now();

	// Connect to the server if not already connected.
	if (net->sockfd < 0) {
		ret = net_connect(net);
		if (ret != KNOT_EOK) {
			return -1;
		}
	}

	// Send query packet.
	ret = net_send(net, query->wire, query->size);
	if (ret != KNOT_EOK) {
		net_close(net);
		return -1;
	}

	// Get stop query time and start reply time.
	t_query = time_now();

#if USE_DNSTAP
	struct timespec t_query_full = time_diff(&t_start, &t_query);
	t_query_full.tv_sec += timestamp;

	// Make the dnstap copy of the query.
	write_dnstap(query_ctx->dt_writer, true, query->wire, query->size,
	             net, &t_query_full);
#endif // USE_DNSTAP

	// Print query packet if required.
	if (style->show_query && style->format != FORMAT_JSON) {
		// Create copy of query packet for parsing.
		knot_pkt_t *q = knot_pkt_new(query->wire, query->size, NULL);
		if (q != NULL) {
			if (knot_pkt_parse(q, KNOT_PF_NOCANON) == KNOT_EOK) {
				print_packet(q, net, query->size,
				             time_diff_ms(&t_start, &t_query),
				             timestamp, false, style);
			} else {
				ERR("can't print query packet");
			}
			knot_pkt_free(q);
		} else {
			ERR("can't print query packet");
		}

		printf("\n");
	}

	// Loop over reply messages unless first and last SOA serials differ.
	while (true) {
		reply = NULL;

		usleep(query_ctx->msgdelay * 1000LU);

		// Receive a reply message.
		in_len = net_receive(net, in, sizeof(in));
		t_end = time_now();
		if (in_len <= 0) {
			goto fail;
		}

#if USE_DNSTAP
		struct timespec t_end_full = time_diff(&t_start, &t_end);
		t_end_full.tv_sec += timestamp;

		// Make the dnstap copy of the response.
		write_dnstap(query_ctx->dt_writer, false, in, in_len, net,
		             &t_end_full);
#endif // USE_DNSTAP

		// Create reply packet structure to fill up.
		reply = knot_pkt_new(in, in_len, NULL);
		if (reply == NULL) {
			ERR("internal error (%s)", knot_strerror(KNOT_ENOMEM));
			goto fail;
		}

		// Parse reply to the packet structure.
		ret = knot_pkt_parse(reply, KNOT_PF_NOCANON);
		if (ret == KNOT_ETRAIL) {
			WARN("malformed reply packet (%s)", knot_strerror(ret));
		} else if (ret != KNOT_EOK) {
			ERR("malformed reply packet from %s", net->remote_str);
			goto fail;
		}

		// Compare reply header id.
		if (check_reply_id(reply, query) == false) {
			ERR("reply ID mismatch from %s", net->remote_str);
			goto fail;
		}

		// Print leading transfer information.
		if (msg_count == 0) {
			if (style->format != FORMAT_JSON) {
				print_header_xfr(query, style);
			} else {
				knot_pkt_t *q = knot_pkt_new(query->wire, query->size, NULL);
				(void)knot_pkt_parse(q, KNOT_PF_NOCANON);
				w = print_header_xfr_json(q, timestamp, style);
				knot_pkt_free(q);
			}
		}

		// Check for error reply.
		if (knot_pkt_ext_rcode(reply) != KNOT_RCODE_NOERROR) {
			ERR("server replied with error '%s'",
			    knot_pkt_ext_rcode_name(reply));
			goto fail;
		}

		// The first message has a special treatment.
		if (msg_count == 0) {
			// Verify 1. signature if a key was specified.
			if (sign_ctx->digest != NULL) {
				ret = verify_packet(reply, sign_ctx);
				if (ret != KNOT_EOK) {
					style_t tsig_style = {
						.format = style->format,
						.style = style->style,
						.show_tsig = true
					};
					if (style->format != FORMAT_JSON) {
						print_data_xfr(reply, &tsig_style);
					}

					ERR("reply verification for %s (%s)",
					    net->remote_str, knot_strerror(ret));
					goto fail;
				}
			}

			// Read first SOA serial.
			serial = first_serial_check(reply, query);

			if (serial < 0) {
				ERR("first answer record from %s isn't SOA",
				    net->remote_str);
				goto fail;
			}

			// Check for question sections equality.
			check_reply_question(reply, query);

			// Check QR bit
			check_reply_qr(reply);
		}

		msg_count++;
		rr_count += knot_wire_get_ancount(reply->wire);
		total_len += in_len;

		// Print reply packet.
		if (style->format != FORMAT_JSON) {
			print_data_xfr(reply, style);
		} else {
			print_data_xfr_json(w, reply, timestamp);
		}

		// Fail to continue if TC is set.
		if (knot_wire_get_tc(reply->wire)) {
			ERR("truncated reply");
			goto fail;
		}

		// Check for finished transfer.
		if (finished_xfr(serial, reply, query, msg_count, query_ctx->serial != -1)) {
			knot_pkt_free(reply);
			break;
		}

		knot_pkt_free(reply);
		reply = NULL;
	}

	// Print full transfer information.
	t_end = time_now();
	if (style->format != FORMAT_JSON) {
		print_footer_xfr(total_len, msg_count, rr_count, net,
		                 time_diff_ms(&t_query, &t_end), timestamp, style);
	} else {
		print_footer_xfr_json(&w, style);
	}

	net_close_keepopen(net, query_ctx);

	return 0;

fail:
	// Print partial transfer information.
	t_end = time_now();
	if (style->format != FORMAT_JSON) {
		print_data_xfr(reply, style);
		print_footer_xfr(total_len, msg_count, rr_count, net,
		                 time_diff_ms(&t_query, &t_end), timestamp, style);
	} else {
		print_data_xfr_json(w, reply, timestamp);
		print_footer_xfr_json(&w, style);
	}

	knot_pkt_free(reply);
	net_close(net);
	free(w);

	return -1;
}

static int process_xfr(const query_t *query, net_t *net)
{
	knot_pkt_t *out_packet;
	int        ret;

	// Create query packet.
	out_packet = create_query_packet(query);
	if (out_packet == NULL) {
		ERR("can't create query packet");
		return -1;
	}

	// Sign the query.
	sign_context_t sign_ctx = { 0 };
	ret = sign_query(out_packet, query, &sign_ctx);
	if (ret != KNOT_EOK) {
		ERR("can't sign the packet (%s)", knot_strerror(ret));
		return -1;
	}

	// Reuse previous connection if available.
	if (net->sockfd >= 0) {
		DBG("Querying for owner(%s), class(%u), type(%u), reused connection",
		    query->owner, query->class_num, query->type_num);

		ret = process_xfr_packet(out_packet, net, query,
		                         &sign_ctx, &query->style);
		goto finish;
	}

	// Get connection parameters.
	int socktype = get_socktype(query->protocol, query->type_num);
	int flags = query->fastopen ? NET_FLAGS_FASTOPEN : NET_FLAGS_NONE;

	// Use the first nameserver from the list.
	srv_info_t *remote = HEAD(query->servers);
	int iptype = get_iptype(query->ip, remote);

	DBG("Querying for owner(%s), class(%u), type(%u), server(%s), "
	    "port(%s), protocol(%s)", query->owner, query->class_num,
	    query->type_num, remote->name, remote->service,
	    get_sockname(socktype));

	// Initialize network structure.
	ret = net_init(query->local, remote, iptype, socktype, query->wait, flags,
	               (struct sockaddr *)&query->proxy.src,
	               (struct sockaddr *)&query->proxy.dst,
	               net);
	if (ret != KNOT_EOK) {
		sign_context_deinit(&sign_ctx);
		knot_pkt_free(out_packet);
		return -1;
	}

	// Loop over all resolved addresses for remote.
	while (net->srv != NULL) {
		ret = net_init_crypto(net, &query->tls, &query->https,
		                      &query->quic);
		if (ret != 0) {
			ERR("failed to initialize crypto context (%s)",
			    knot_strerror(ret));
			break;
		}

		ret = process_xfr_packet(out_packet, net,
		                         query,
		                         &sign_ctx,
		                         &query->style);
		// If error try next resolved address.
		if (ret != 0) {
			net->srv = (net->srv)->ai_next;
			continue;
		}

		break;
	}

	if (ret != 0) {
		ERR("failed to query server %s@%s(%s)",
		    remote->name, remote->service, get_sockname(socktype));
	}
finish:
	if (!query->keepopen || net->sockfd < 0) {
		net_clean(net);
	}
	sign_context_deinit(&sign_ctx);
	knot_pkt_free(out_packet);

	return ret;
}

int kdig_exec(const kdig_params_t *params)
{
	node_t *n;
	net_t net = { .sockfd = -1 };

	if (params == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	bool success = true;

	// Loop over query list.
	WALK_LIST(n, params->queries) {
		query_t *query = (query_t *)n;

		int ret = -1;
		switch (query->operation) {
		case OPERATION_QUERY:
			ret = process_query(query, &net);
			break;
		case OPERATION_XFR:
			ret = process_xfr(query, &net);
			break;
#if USE_DNSTAP
		case OPERATION_LIST_DNSTAP:
			ret = process_dnstap(query);
			break;
#endif // USE_DNSTAP
		default:
			ERR("unsupported operation");
			break;
		}

		// All operations must succeed.
		if (ret != 0) {
			success = false;
		}

		// If not last query, print separation.
		if (n->next->next && params->config->style.format == FORMAT_FULL) {
			printf("\n");
		}
	}

	if (net.sockfd >= 0) {
		net_close(&net);
		net_clean(&net);
	}

	return success ? KNOT_EOK : KNOT_ERROR;
}
