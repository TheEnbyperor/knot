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

#include <assert.h>
#include <stdint.h>
#include <urcu.h>

#include "contrib/mempattern.h"
#include "libdnssec/random.h"
#include "knot/common/log.h"
#include "knot/conf/conf.h"
#include "knot/dnssec/zone-events.h"
#include "knot/events/handlers.h"
#include "knot/events/replan.h"
#include "knot/nameserver/ixfr.h"
#include "knot/query/layer.h"
#include "knot/query/query.h"
#include "knot/query/requestor.h"
#include "knot/server/server.h"
#include "knot/updates/changesets.h"
#include "knot/zone/adjust.h"
#include "knot/zone/digest.h"
#include "knot/zone/serial.h"
#include "knot/zone/zone.h"
#include "knot/zone/zonefile.h"
#include "libknot/errcode.h"

/*!
 * \brief Refresh event processing.
 *
 * The following diagram represents refresh event processing.
 *
 * \verbatim
 *                               O
 *                               |
 *                         +-----v-----+
 *                         |   BEGIN   |
 *                         +---+---+---+
 *               has SOA       |   |           no SOA
 *         +-------------------+   +------------------------------+
 *         |                                                      |
 *  +------v------+  outdated  +--------------+   error   +-------v------+
 *  |  SOA query  +------------>  IXFR query  +----------->  AXFR query  |
 *  +-----+---+---+            +------+-------+           +----+----+----+
 *  error |   | current               | success        success |    | error
 *        |   +-----+ +---------------+                        |    |
 *        |         | | +--------------------------------------+    |
 *        |         | | |              +----------+  +--------------+
 *        |         | | |              |          |  |
 *        |      +--v-v-v--+           |       +--v--v--+
 *        |      |  DONE   |           |       |  FAIL  |
 *        |      +---------+           |       +--------+
 *        +----------------------------+
 *
 * \endverbatim
 */

#define REFRESH_LOG(priority, data, msg...) \
	ns_log(priority, (data)->zone->name, LOG_OPERATION_REFRESH, LOG_DIRECTION_NONE, \
	       &(data)->remote->addr, 0, false, (data)->remote->key.name, msg)

#define REFRESH_LOG_PROTO(priority, data, msg...) \
	ns_log(priority, (data)->zone->name, LOG_OPERATION_REFRESH, LOG_DIRECTION_NONE, \
	       &(data)->remote->addr, flags2proto((data)->layer->flags), \
	       (data)->layer->flags & KNOT_REQUESTOR_REUSED, (data)->remote->key.name, msg)

#define AXFRIN_LOG(priority, data, msg...) \
	ns_log(priority, (data)->zone->name, LOG_OPERATION_AXFR, LOG_DIRECTION_IN, \
	       &(data)->remote->addr, flags2proto((data)->layer->flags), \
	       (data)->layer->flags & KNOT_REQUESTOR_REUSED, (data)->remote->key.name, msg)

#define IXFRIN_LOG(priority, data, msg...) \
	ns_log(priority, (data)->zone->name, LOG_OPERATION_IXFR, LOG_DIRECTION_IN, \
	       &(data)->remote->addr, flags2proto((data)->layer->flags), \
	       (data)->layer->flags & KNOT_REQUESTOR_REUSED, (data)->remote->key.name, msg)

enum state {
	REFRESH_STATE_INVALID = 0,
	STATE_SOA_QUERY,
	STATE_TRANSFER,
};

enum xfr_type {
	XFR_TYPE_NOTIMP = -2,
	XFR_TYPE_ERROR = -1,
	XFR_TYPE_UNDETERMINED = 0,
	XFR_TYPE_UPTODATE,
	XFR_TYPE_AXFR,
	XFR_TYPE_IXFR,
};

struct refresh_data {
	knot_layer_t *layer;              //!< Used for reading requestor flags.

	// transfer configuration, initialize appropriately:

	zone_t *zone;                     //!< Zone to eventually updated.
	conf_t *conf;                     //!< Server configuration.
	const conf_remote_t *remote;      //!< Remote endpoint.
	const knot_rrset_t *soa;          //!< Local SOA (NULL for AXFR).
	const size_t max_zone_size;       //!< Maximal zone size.
	query_edns_data_t edns;           //!< EDNS data to be used in queries.
	zone_master_fallback_t *fallback; //!< Flags allowing zone_master_try() fallbacks.
	bool fallback_axfr;               //!< Flag allowing fallback to AXFR,
	bool ixfr_by_one;                 //!< Allow only single changeset within IXFR.
	bool ixfr_from_axfr;              //!< Diff computation of incremental update from AXFR allowed.
	uint32_t expire_timer;            //!< Result: expire timer from answer EDNS.

	// internal state, initialize with zeroes:

	int ret;                          //!< Error code.
	enum state state;                 //!< Event processing state.
	enum xfr_type xfr_type;           //!< Transfer type (mostly IXFR versus AXFR).
	bool axfr_style_ixfr;             //!< Master responded with AXFR-style-IXFR.
	knot_rrset_t *initial_soa_copy;   //!< Copy of the received initial SOA.
	struct xfr_stats stats;           //!< Transfer statistics.
	struct timespec started;          //!< When refresh started.
	size_t change_size;               //!< Size of added and removed RRs.

	struct {
		zone_contents_t *zone;    //!< AXFR result, new zone.
	} axfr;

	struct {
		struct ixfr_proc *proc;   //!< IXFR processing context.
		knot_rrset_t *final_soa;  //!< SOA denoting end of transfer.
		list_t changesets;        //!< IXFR result, zone updates.
	} ixfr;

	bool updated;  // TODO: Can we fid a better way to check if zone was updated?
	knot_mm_t *mm; // TODO: This used to be used in IXFR. Remove or reuse.
};

static const uint32_t EXPIRE_TIMER_INVALID = ~0U;

static bool serial_is_current(uint32_t local_serial, uint32_t remote_serial)
{
	return (serial_compare(local_serial, remote_serial) & SERIAL_MASK_GEQ);
}

static time_t bootstrap_next(uint8_t *count)
{
	// Let the increment gradually grow in a sensible way.
	time_t increment = 5 * (*count) * (*count);

	if (increment < 7200) { // two hours
		(*count)++;
	} else {
		increment = 7200;
	}

	// Add a random delay to prevent burst refresh.
	return increment + dnssec_random_uint16_t() % 30;
}

static void limit_timer(conf_t *conf, const knot_dname_t *zone, uint32_t *timer,
                        const char *tm_name, const yp_name_t *low, const yp_name_t *upp)
{
	uint32_t tlow = 0;
	if (low > 0) {
		conf_val_t val1 = conf_zone_get(conf, low, zone);
		tlow = conf_int(&val1);
	}
	conf_val_t val2 = conf_zone_get(conf, upp, zone);
	uint32_t tupp = conf_int(&val2);

	const char *msg = "%s timer trimmed to '%s-%s-interval'";
	if (*timer < tlow) {
		*timer = tlow;
		log_zone_debug(zone, msg, tm_name, tm_name, "min");
	} else if (*timer > tupp) {
		*timer = tupp;
		log_zone_debug(zone, msg, tm_name, tm_name, "max");
	}
}

/*!
 * \brief Modify the expire timer wrt the received EDNS EXPIRE (RFC 7314, section 4)
 *
 * \param data             The refresh data.
 * \param pkt              A received packet to parse.
 * \param strictly_follow  Strictly use EDNS EXPIRE as the expire timer value.
 *                         (false == RFC 7314, section 4, second paragraph,
 *                           true ==                      third paragraph)
 */
static void consume_edns_expire(struct refresh_data *data, knot_pkt_t *pkt, bool strictly_follow)
{
	if (data->zone->is_catalog_flag) {
		data->expire_timer = EXPIRE_TIMER_INVALID;
		return;
	}

	uint8_t *expire_opt = knot_pkt_edns_option(pkt, KNOT_EDNS_OPTION_EXPIRE);
	if (expire_opt != NULL && knot_edns_opt_get_length(expire_opt) == sizeof(uint32_t)) {
		uint32_t edns_expire = knot_wire_read_u32(knot_edns_opt_get_data(expire_opt));
		data->expire_timer = strictly_follow ? edns_expire :
				     MAX(edns_expire, data->zone->timers.next_expire - time(NULL));
	}
}

static void finalize_timers_base(struct refresh_data *data, bool also_expire)
{
	conf_t *conf = data->conf;
	zone_t *zone = data->zone;

	// EDNS EXPIRE -- RFC 7314, section 4, fourth paragraph.
	data->expire_timer = MIN(data->expire_timer, zone_soa_expire(data->zone));
	assert(data->expire_timer != EXPIRE_TIMER_INVALID);

	time_t now = time(NULL);
	const knot_rdataset_t *soa = zone_soa(zone);

	uint32_t soa_refresh = knot_soa_refresh(soa->rdata);
	limit_timer(conf, zone->name, &soa_refresh, "refresh",
	            C_REFRESH_MIN_INTERVAL, C_REFRESH_MAX_INTERVAL);
	zone->timers.next_refresh = now + soa_refresh;
	zone->timers.last_refresh_ok = true;

	if (zone->is_catalog_flag) {
		// It's already zero in most cases.
		zone->timers.next_expire = 0;
	} else if (also_expire) {
		limit_timer(conf, zone->name, &data->expire_timer, "expire",
		            // Limit min if not received as EDNS Expire.
		            data->expire_timer == knot_soa_expire(soa->rdata) ?
			      C_EXPIRE_MIN_INTERVAL : 0,
		            C_EXPIRE_MAX_INTERVAL);
		zone->timers.next_expire = now + data->expire_timer;
	}
}

static void finalize_timers(struct refresh_data *data)
{
	finalize_timers_base(data, true);
}

static void finalize_timers_noexpire(struct refresh_data *data)
{
	finalize_timers_base(data, false);
}

static void fill_expires_in(char *expires_in, size_t size, const struct refresh_data *data)
{
	assert(!data->zone->is_catalog_flag || data->zone->timers.next_expire == 0);
	if (data->zone->timers.next_expire > 0 && data->expire_timer > 0) {
		(void)snprintf(expires_in, size,
		               ", expires in %u seconds", data->expire_timer);
	}
}

static void xfr_log_publish(const struct refresh_data *data,
                            const uint32_t old_serial,
                            const uint32_t new_serial,
                            const uint32_t master_serial,
                            bool has_master_serial,
                            bool axfr_bootstrap)
{
	struct timespec finished = time_now();
	double duration = time_diff_ms(&data->started, &finished) / 1000.0;

	char old_info[32] = "none";
	if (!axfr_bootstrap) {
		(void)snprintf(old_info, sizeof(old_info), "%u", old_serial);
	}

	char master_info[32] = "";
	if (has_master_serial) {
		(void)snprintf(master_info, sizeof(master_info),
		               ", remote serial %u", master_serial);
	}

	char expires_in[32] = "";
	fill_expires_in(expires_in, sizeof(expires_in), data);

	REFRESH_LOG(LOG_INFO, data,
	            "zone updated, %0.2f seconds, serial %s -> %u%s%s",
	            duration, old_info, new_serial, master_info, expires_in);
}

static void xfr_log_read_ms(const knot_dname_t *zone, int ret)
{
	log_zone_error(zone, "failed reading master serial from KASP DB (%s)", knot_strerror(ret));
}

static int axfr_init(struct refresh_data *data)
{
	zone_contents_t *new_zone = zone_contents_new(data->zone->name, true);
	if (new_zone == NULL) {
		return KNOT_ENOMEM;
	}

	data->axfr.zone = new_zone;
	return KNOT_EOK;
}

static void axfr_cleanup(struct refresh_data *data)
{
	zone_contents_deep_free(data->axfr.zone);
	data->axfr.zone = NULL;
}

static void axfr_slave_sign_serial(zone_contents_t *new_contents, zone_t *zone,
                                   conf_t *conf, uint32_t *master_serial)
{
	// Update slave's serial to ensure it's growing and consistent with
	// its serial policy.

	*master_serial = zone_contents_serial(new_contents);

	uint32_t new_serial, lastsigned_serial;
	if (zone->contents != NULL) {
		// Retransfer or AXFR-fallback - increment current serial.
		uint32_t cont_serial = zone_contents_serial(zone->contents);
		new_serial = serial_next(cont_serial, conf, zone->name, SERIAL_POLICY_AUTO, 1);
	} else if (zone_get_lastsigned_serial(zone, &lastsigned_serial) == KNOT_EOK) {
		// Bootstrap - increment stored serial.
		new_serial = serial_next(lastsigned_serial, conf, zone->name, SERIAL_POLICY_AUTO, 1);
	} else {
		// Bootstrap - try to reuse master serial, considering policy.
		new_serial = serial_next(*master_serial, conf, zone->name, SERIAL_POLICY_AUTO, 0);
	}
	zone_contents_set_soa_serial(new_contents, new_serial);
}

static int axfr_finalize(struct refresh_data *data)
{
	zone_contents_t *new_zone = data->axfr.zone;

	conf_val_t val = conf_zone_get(data->conf, C_DNSSEC_SIGNING, data->zone->name);
	bool dnssec_enable = conf_bool(&val);
	uint32_t old_serial = zone_contents_serial(data->zone->contents), master_serial = 0;
	bool bootstrap = (data->zone->contents == NULL);

	if (dnssec_enable) {
		axfr_slave_sign_serial(new_zone, data->zone, data->conf, &master_serial);
	}

	zone_update_t up = { 0 };
	int ret;

	if (data->ixfr_from_axfr && data->axfr_style_ixfr) {
		ret = zone_update_from_differences(&up, data->zone, NULL, new_zone, UPDATE_INCREMENTAL, dnssec_enable, false);
	} else {
		ret = zone_update_from_contents(&up, data->zone, new_zone, UPDATE_FULL);
	}
	if (ret != KNOT_EOK) {
		data->fallback->remote = false;
		return ret;
	}
	// Seized by zone_update. Don't free the contents again in axfr_cleanup.
	data->axfr.zone = NULL;

	ret = zone_update_semcheck(data->conf, &up);
	if (ret == KNOT_EOK) {
		ret = zone_update_verify_digest(data->conf, &up);
	}
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		return ret;
	}

	val = conf_zone_get(data->conf, C_ZONEMD_GENERATE, data->zone->name);
	unsigned digest_alg = conf_opt(&val);

	if (dnssec_enable) {
		zone_sign_reschedule_t resch = { 0 };
		ret = knot_dnssec_zone_sign(&up, data->conf, ZONE_SIGN_KEEP_SERIAL, KEY_ROLL_ALLOW_ALL, 0, &resch);
		event_dnssec_reschedule(data->conf, data->zone, &resch, false);
	} else if (digest_alg != ZONE_DIGEST_NONE) {
		assert(zone_update_to(&up) != NULL);
		ret = zone_update_add_digest(&up, digest_alg, false);
	}
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		data->fallback->remote = false;
		return ret;
	}

	ret = zone_update_commit(data->conf, &up);
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		AXFRIN_LOG(LOG_WARNING, data,
		           "failed to store changes (%s)", knot_strerror(ret));
		data->fallback->remote = false;
		return ret;
	}

	if (dnssec_enable) {
		ret = zone_set_master_serial(data->zone, master_serial);
		if (ret != KNOT_EOK) {
			log_zone_warning(data->zone->name,
			"unable to save master serial, future transfers might be broken");
		}
	}

	finalize_timers(data);
	xfr_log_publish(data, old_serial, zone_contents_serial(data->zone->contents),
	                master_serial, dnssec_enable, bootstrap);

	data->fallback->remote = false;
	zone_set_last_master(data->zone, (const struct sockaddr_storage *)data->remote);

	return KNOT_EOK;
}

static int axfr_consume_rr(const knot_rrset_t *rr, struct refresh_data *data)
{
	assert(rr);
	assert(data);
	assert(data->axfr.zone);

	// zc is stateless structure which can be initialized for each rr
	// the changes are stored only in data->axfr.zone (aka zc.z)
	zcreator_t zc = {
		.z = data->axfr.zone,
		.master = false,
		.ret = KNOT_EOK
	};

	if (rr->type == KNOT_RRTYPE_SOA &&
	    node_rrtype_exists(zc.z->apex, KNOT_RRTYPE_SOA)) {
		return KNOT_STATE_DONE;
	}

	data->ret = zcreator_step(&zc, rr);
	if (data->ret != KNOT_EOK) {
		return KNOT_STATE_FAIL;
	}

	data->change_size += knot_rrset_size(rr);
	if (data->change_size > data->max_zone_size) {
		AXFRIN_LOG(LOG_WARNING, data,
		           "zone size exceeded");
		data->ret = KNOT_EZONESIZE;
		return KNOT_STATE_FAIL;
	}

	return KNOT_STATE_CONSUME;
}

static int axfr_consume_packet(knot_pkt_t *pkt, struct refresh_data *data)
{
	assert(pkt);
	assert(data);

	const knot_pktsection_t *answer = knot_pkt_section(pkt, KNOT_ANSWER);
	int ret = KNOT_STATE_CONSUME;
	for (uint16_t i = 0; i < answer->count && ret == KNOT_STATE_CONSUME; ++i) {
		ret = axfr_consume_rr(knot_pkt_rr(answer, i), data);
	}
	return ret;
}

static int axfr_consume(knot_pkt_t *pkt, struct refresh_data *data, bool reuse_soa)
{
	assert(pkt);
	assert(data);

	// Check RCODE
	if (knot_pkt_ext_rcode(pkt) != KNOT_RCODE_NOERROR) {
		AXFRIN_LOG(LOG_WARNING, data,
		           "server responded with error '%s'",
		           knot_pkt_ext_rcode_name(pkt));
		data->ret = KNOT_EDENIED;
		return KNOT_STATE_FAIL;
	}

	// Initialize with first packet
	if (data->axfr.zone == NULL) {
		data->ret = axfr_init(data);
		if (data->ret != KNOT_EOK) {
			AXFRIN_LOG(LOG_WARNING, data,
			           "failed to initialize (%s)",
			           knot_strerror(data->ret));
			data->fallback->remote = false;
			return KNOT_STATE_FAIL;
		}

		AXFRIN_LOG(LOG_INFO, data, "started");
		xfr_stats_begin(&data->stats);
		data->change_size = 0;
	}

	int next;
	// Process saved SOA if fallback from IXFR
	if (data->initial_soa_copy != NULL) {
		next = reuse_soa ? axfr_consume_rr(data->initial_soa_copy, data) :
		                   KNOT_STATE_CONSUME;
		knot_rrset_free(data->initial_soa_copy, data->mm);
		data->initial_soa_copy = NULL;
		if (next != KNOT_STATE_CONSUME) {
			return next;
		}
	}

	// Process answer packet
	xfr_stats_add(&data->stats, pkt->size + knot_rrset_size(pkt->tsig_rr));
	next = axfr_consume_packet(pkt, data);

	// Finalize
	if (next == KNOT_STATE_DONE) {
		xfr_stats_end(&data->stats);
	}

	return next;
}

/*! \brief Initialize IXFR-in processing context. */
static int ixfr_init(struct refresh_data *data)
{
	struct ixfr_proc *proc = mm_alloc(data->mm, sizeof(*proc));
	if (proc == NULL) {
		return KNOT_ENOMEM;
	}

	memset(proc, 0, sizeof(struct ixfr_proc));
	proc->state = IXFR_START;
	proc->mm = data->mm;

	data->ixfr.proc = proc;
	data->ixfr.final_soa = NULL;

	init_list(&data->ixfr.changesets);

	return KNOT_EOK;
}

/*! \brief Clean up data allocated by IXFR-in processing. */
static void ixfr_cleanup(struct refresh_data *data)
{
	if (data->ixfr.proc == NULL) {
		return;
	}

	knot_rrset_free(data->ixfr.final_soa, data->mm);
	data->ixfr.final_soa = NULL;
	mm_free(data->mm, data->ixfr.proc);
	data->ixfr.proc = NULL;

	changesets_free(&data->ixfr.changesets);
}

static bool ixfr_serial_once(changeset_t *ch, conf_t *conf, uint32_t *master_serial, uint32_t *local_serial)
{
	uint32_t ch_from = changeset_from(ch), ch_to = changeset_to(ch);

	if (ch_from != *master_serial || (serial_compare(ch_from, ch_to) & SERIAL_MASK_GEQ)) {
		return false;
	}

	uint32_t new_from = *local_serial;
	uint32_t new_to = serial_next(new_from, conf, ch->soa_from->owner, SERIAL_POLICY_AUTO, 1);
	knot_soa_serial_set(ch->soa_from->rrs.rdata, new_from);
	knot_soa_serial_set(ch->soa_to->rrs.rdata, new_to);

	*master_serial = ch_to;
	*local_serial = new_to;

	return true;
}

static int ixfr_slave_sign_serial(list_t *changesets, zone_t *zone,
                                  conf_t *conf, uint32_t *master_serial)
{
	uint32_t local_serial = zone_contents_serial(zone->contents), lastsigned;

	if (zone_get_lastsigned_serial(zone, &lastsigned) != KNOT_EOK || lastsigned != local_serial) {
		// this is kind of assert
		return KNOT_ERROR;
	}

	int ret = zone_get_master_serial(zone, master_serial);
	if (ret != KNOT_EOK) {
		log_zone_error(zone->name, "failed to read master serial"
		                           "from KASP DB (%s)", knot_strerror(ret));
		return ret;
	}
	changeset_t *chs;
	WALK_LIST(chs, *changesets) {
		if (!ixfr_serial_once(chs, conf, master_serial, &local_serial)) {
			return KNOT_EINVAL;
		}
	}

	return KNOT_EOK;
}

static int ixfr_finalize(struct refresh_data *data)
{
	conf_val_t val = conf_zone_get(data->conf, C_DNSSEC_SIGNING, data->zone->name);
	bool dnssec_enable = conf_bool(&val);
	uint32_t master_serial = 0, old_serial = zone_contents_serial(data->zone->contents);

	if (dnssec_enable) {
		int ret = ixfr_slave_sign_serial(&data->ixfr.changesets, data->zone, data->conf, &master_serial);
		if (ret != KNOT_EOK) {
			IXFRIN_LOG(LOG_WARNING, data,
			           "failed to adjust SOA serials from unsigned remote (%s)",
			           knot_strerror(ret));
			data->fallback_axfr = false;
			data->fallback->remote = false;
			return ret;
		}
	}

	val = conf_zone_get(data->conf, C_IXFR_BENEVOLENT, data->zone->name);
	zone_update_flags_t strict = conf_bool(&val) ? 0 : UPDATE_STRICT;

	zone_update_t up = { 0 };
	int ret = zone_update_init(&up, data->zone, UPDATE_INCREMENTAL | UPDATE_NO_CHSET | strict);
	if (ret != KNOT_EOK) {
		data->fallback_axfr = false;
		data->fallback->remote = false;
		return ret;
	}

	changeset_t *set;
	WALK_LIST(set, data->ixfr.changesets) {
		ret = zone_update_apply_changeset(&up, set);
		if (ret != KNOT_EOK) {
			uint32_t serial_from = knot_soa_serial(set->soa_from->rrs.rdata);
			uint32_t serial_to = knot_soa_serial(set->soa_to->rrs.rdata);
			zone_update_clear(&up);
			IXFRIN_LOG(LOG_WARNING, data,
			           "serial %u -> %u, failed to apply changes to zone (%s)",
			           serial_from, serial_to, knot_strerror(ret));
			return ret;
		}
	}

	ret = zone_update_semcheck(data->conf, &up);
	if (ret == KNOT_EOK) {
		ret = zone_update_verify_digest(data->conf, &up);
	}
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		data->fallback_axfr = false;
		return ret;
	}

	val = conf_zone_get(data->conf, C_ZONEMD_GENERATE, data->zone->name);
	unsigned digest_alg = conf_opt(&val);

	if (dnssec_enable) {
		ret = knot_dnssec_sign_update(&up, data->conf);
	} else if (digest_alg != ZONE_DIGEST_NONE) {
		if (zone_update_to(&up) == NULL) {
			ret = zone_update_increment_soa(&up, data->conf);
		}
		if (ret == KNOT_EOK) {
			ret = zone_update_add_digest(&up, digest_alg, false);
		}
	}
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		data->fallback_axfr = false;
		data->fallback->remote = false;
		return ret;
	}

	ret = zone_update_commit(data->conf, &up);
	if (ret != KNOT_EOK) {
		zone_update_clear(&up);
		IXFRIN_LOG(LOG_WARNING, data,
		           "failed to store changes (%s)", knot_strerror(ret));
		return ret;
	}

	if (dnssec_enable && !EMPTY_LIST(data->ixfr.changesets)) {
		ret = zone_set_master_serial(data->zone, master_serial);
		if (ret != KNOT_EOK) {
			log_zone_warning(data->zone->name,
			"unable to save master serial, future transfers might be broken");
		}
	}

	finalize_timers(data);
	xfr_log_publish(data, old_serial, zone_contents_serial(data->zone->contents),
	                master_serial, dnssec_enable, false);

	if (old_serial != zone_contents_serial(data->zone->contents)) {
		data->fallback->remote = false;
		zone_set_last_master(data->zone, (const struct sockaddr_storage *)data->remote);
	}

	return KNOT_EOK;
}

/*! \brief Stores starting SOA into changesets structure. */
static int ixfr_solve_start(const knot_rrset_t *rr, struct refresh_data *data)
{
	assert(data->ixfr.final_soa == NULL);
	if (rr->type != KNOT_RRTYPE_SOA) {
		return KNOT_EMALF;
	}

	// Store terminal SOA
	data->ixfr.final_soa = knot_rrset_copy(rr, data->mm);
	if (data->ixfr.final_soa == NULL) {
		return KNOT_ENOMEM;
	}

	// Initialize list for changes
	init_list(&data->ixfr.changesets);

	return KNOT_EOK;
}

/*! \brief Decides what to do with a starting SOA (deletions). */
static int ixfr_solve_soa_del(const knot_rrset_t *rr, struct refresh_data *data)
{
	if (rr->type != KNOT_RRTYPE_SOA) {
		return KNOT_EMALF;
	}

	// Create new changeset.
	changeset_t *change = changeset_new(data->zone->name);
	if (change == NULL) {
		return KNOT_ENOMEM;
	}

	// Store SOA into changeset.
	change->soa_from = knot_rrset_copy(rr, NULL);
	if (change->soa_from == NULL) {
		changeset_free(change);
		return KNOT_ENOMEM;
	}

	// Add changeset.
	add_tail(&data->ixfr.changesets, &change->n);

	return KNOT_EOK;
}

/*! \brief Stores ending SOA into changeset. */
static int ixfr_solve_soa_add(const knot_rrset_t *rr, changeset_t *change, knot_mm_t *mm)
{
	if (rr->type != KNOT_RRTYPE_SOA) {
		return KNOT_EMALF;
	}

	change->soa_to = knot_rrset_copy(rr, NULL);
	if (change->soa_to == NULL) {
		return KNOT_ENOMEM;
	}

	return KNOT_EOK;
}

/*! \brief Adds single RR into remove section of changeset. */
static int ixfr_solve_del(const knot_rrset_t *rr, changeset_t *change, knot_mm_t *mm)
{
	return changeset_add_removal(change, rr, 0);
}

/*! \brief Adds single RR into add section of changeset. */
static int ixfr_solve_add(const knot_rrset_t *rr, changeset_t *change, knot_mm_t *mm)
{
	return changeset_add_addition(change, rr, 0);
}

/*! \brief Decides what the next IXFR-in state should be. */
static int ixfr_next_state(struct refresh_data *data, const knot_rrset_t *rr)
{
	const bool soa = (rr->type == KNOT_RRTYPE_SOA);
	enum ixfr_state state = data->ixfr.proc->state;

	if ((state == IXFR_SOA_ADD || state == IXFR_ADD) &&
	    knot_rrset_equal(rr, data->ixfr.final_soa, true)) {
		data->ixfr_by_one = false; // just one changeset was there, no need to replan IXFR now
		return IXFR_DONE;
	}

	if ((state == IXFR_SOA_ADD || state == IXFR_ADD) &&
	    soa && data->ixfr_by_one) {
		return IXFR_DONE;
	}

	switch (state) {
	case IXFR_START:
		// Final SOA already stored or transfer start.
		return data->ixfr.final_soa ? IXFR_SOA_DEL : IXFR_START;
	case IXFR_SOA_DEL:
		// Empty delete section or start of delete section.
		return soa ? IXFR_SOA_ADD : IXFR_DEL;
	case IXFR_SOA_ADD:
		// Empty add section or start of add section.
		return soa ? IXFR_SOA_DEL : IXFR_ADD;
	case IXFR_DEL:
		// End of delete section or continue.
		return soa ? IXFR_SOA_ADD : IXFR_DEL;
	case IXFR_ADD:
		// End of add section or continue.
		return soa ? IXFR_SOA_DEL : IXFR_ADD;
	default:
		assert(0);
		return IXFR_INVALID;
	}
}

/*!
 * \brief Processes single RR according to current IXFR-in state. The states
 *        correspond with IXFR-in message structure, in the order they are
 *        mentioned in the code.
 *
 * \param rr    RR to process.
 * \param proc  Processing context.
 *
 * \return KNOT_E*
 */
static int ixfr_step(const knot_rrset_t *rr, struct refresh_data *data)
{
	data->ixfr.proc->state = ixfr_next_state(data, rr);
	changeset_t *change = TAIL(data->ixfr.changesets);

	switch (data->ixfr.proc->state) {
	case IXFR_START:
		return ixfr_solve_start(rr, data);
	case IXFR_SOA_DEL:
		return ixfr_solve_soa_del(rr, data);
	case IXFR_DEL:
		return ixfr_solve_del(rr, change, data->mm);
	case IXFR_SOA_ADD:
		return ixfr_solve_soa_add(rr, change, data->mm);
	case IXFR_ADD:
		return ixfr_solve_add(rr, change, data->mm);
	case IXFR_DONE:
		return KNOT_EOK;
	default:
		return KNOT_ERROR;
	}
}

static int ixfr_consume_rr(const knot_rrset_t *rr, struct refresh_data *data)
{
	if (knot_dname_in_bailiwick(rr->owner, data->zone->name) < 0) {
		return KNOT_STATE_CONSUME;
	}

	data->ret = ixfr_step(rr, data);
	if (data->ret != KNOT_EOK) {
		IXFRIN_LOG(LOG_WARNING, data,
		           "failed (%s)", knot_strerror(data->ret));
		return KNOT_STATE_FAIL;
	}

	data->change_size += knot_rrset_size(rr);
	if (data->change_size / 2 > data->max_zone_size) {
		IXFRIN_LOG(LOG_WARNING, data,
		           "transfer size exceeded");
		data->ret = KNOT_EZONESIZE;
		return KNOT_STATE_FAIL;
	}

	if (data->ixfr.proc->state == IXFR_DONE) {
		return KNOT_STATE_DONE;
	}

	return KNOT_STATE_CONSUME;
}

/*!
 * \brief Processes IXFR reply packet and fills in the changesets structure.
 *
 * \param pkt    Packet containing the IXFR reply in wire format.
 * \param adata  Answer data, including processing context.
 *
 * \return KNOT_STATE_CONSUME, KNOT_STATE_DONE, KNOT_STATE_FAIL
 */
static int ixfr_consume_packet(knot_pkt_t *pkt, struct refresh_data *data)
{
	// Process RRs in the message.
	const knot_pktsection_t *answer = knot_pkt_section(pkt, KNOT_ANSWER);
	int ret = KNOT_STATE_CONSUME;
	for (uint16_t i = 0; i < answer->count && ret == KNOT_STATE_CONSUME; ++i) {
		ret = ixfr_consume_rr(knot_pkt_rr(answer, i), data);
	}
	return ret;
}

static enum xfr_type determine_xfr_type(const knot_pktsection_t *answer,
                                        uint32_t zone_serial, const knot_rrset_t *initial_soa)
{
	if (answer->count < 1) {
		return XFR_TYPE_NOTIMP;
	}

	const knot_rrset_t *rr_one = knot_pkt_rr(answer, 0);
	if (initial_soa != NULL) {
		if (rr_one->type == KNOT_RRTYPE_SOA) {
		        return knot_rrset_equal(initial_soa, rr_one, true) ?
		               XFR_TYPE_AXFR : XFR_TYPE_IXFR;
		}
		return XFR_TYPE_AXFR;
	}

	if (answer->count == 1) {
		if (rr_one->type == KNOT_RRTYPE_SOA) {
			return serial_is_current(zone_serial, knot_soa_serial(rr_one->rrs.rdata)) ?
			       XFR_TYPE_UPTODATE : XFR_TYPE_UNDETERMINED;
		}
		return XFR_TYPE_ERROR;
	}

	const knot_rrset_t *rr_two = knot_pkt_rr(answer, 1);
	if (answer->count == 2 && rr_one->type == KNOT_RRTYPE_SOA &&
	    knot_rrset_equal(rr_one, rr_two, true)) {
		return XFR_TYPE_AXFR;
	}

	return (rr_one->type == KNOT_RRTYPE_SOA && rr_two->type != KNOT_RRTYPE_SOA) ?
	       XFR_TYPE_AXFR : XFR_TYPE_IXFR;
}

static int ixfr_consume(knot_pkt_t *pkt, struct refresh_data *data)
{
	assert(pkt);
	assert(data);

	// Check RCODE
	if (knot_pkt_ext_rcode(pkt) != KNOT_RCODE_NOERROR) {
		IXFRIN_LOG(LOG_WARNING, data,
		           "server responded with error '%s'",
		           knot_pkt_ext_rcode_name(pkt));
		data->ret = KNOT_EDENIED;
		return KNOT_STATE_FAIL;
	}

	// Initialize with first packet
	if (data->ixfr.proc == NULL) {
		const knot_pktsection_t *answer = knot_pkt_section(pkt, KNOT_ANSWER);

		uint32_t master_serial;
		data->ret = slave_zone_serial(data->zone, data->conf, &master_serial);
		if (data->ret != KNOT_EOK) {
			xfr_log_read_ms(data->zone->name, data->ret);
			data->fallback_axfr = false;
			data->fallback->remote = false;
			return KNOT_STATE_FAIL;
		}
		data->xfr_type = determine_xfr_type(answer, master_serial,
		                                    data->initial_soa_copy);
		switch (data->xfr_type) {
		case XFR_TYPE_ERROR:
			IXFRIN_LOG(LOG_WARNING, data,
			           "malformed response SOA");
			data->ret = KNOT_EMALF;
			data->xfr_type = XFR_TYPE_IXFR; // unrecognisable IXFR type is the same as failed IXFR
			return KNOT_STATE_FAIL;
		case XFR_TYPE_NOTIMP:
			IXFRIN_LOG(LOG_WARNING, data,
			           "not supported by remote");
			data->ret = KNOT_ENOTSUP;
			data->xfr_type = XFR_TYPE_IXFR;
			return KNOT_STATE_FAIL;
		case XFR_TYPE_UNDETERMINED:
			// Store the SOA and check with next packet
			data->initial_soa_copy = knot_rrset_copy(knot_pkt_rr(answer, 0), data->mm);
			if (data->initial_soa_copy == NULL) {
				data->ret = KNOT_ENOMEM;
				return KNOT_STATE_FAIL;
			}
			xfr_stats_add(&data->stats, pkt->size + knot_rrset_size(pkt->tsig_rr));
			return KNOT_STATE_CONSUME;
		case XFR_TYPE_AXFR:
			IXFRIN_LOG(LOG_INFO, data,
			           "receiving AXFR-style IXFR");
			data->axfr_style_ixfr = true;
			return axfr_consume(pkt, data, true);
		case XFR_TYPE_UPTODATE:
			consume_edns_expire(data, pkt, false);
			finalize_timers(data);
			char expires_in[32] = "";
			fill_expires_in(expires_in, sizeof(expires_in), data);
			IXFRIN_LOG(LOG_INFO, data,
			          "zone is up-to-date%s", expires_in);
			xfr_stats_begin(&data->stats);
			xfr_stats_add(&data->stats, pkt->size + knot_rrset_size(pkt->tsig_rr));
			xfr_stats_end(&data->stats);
			return KNOT_STATE_DONE;
		case XFR_TYPE_IXFR:
			break;
		default:
			assert(0);
			data->ret = KNOT_EPROCESSING;
			return KNOT_STATE_FAIL;
		}

		data->ret = ixfr_init(data);
		if (data->ret != KNOT_EOK) {
			IXFRIN_LOG(LOG_WARNING, data,
			           "failed to initialize (%s)", knot_strerror(data->ret));
			data->fallback_axfr = false;
			data->fallback->remote = false;
			return KNOT_STATE_FAIL;
		}

		IXFRIN_LOG(LOG_INFO, data, "started");
		xfr_stats_begin(&data->stats);
		data->change_size = 0;
	}

	int next;
	// Process saved SOA if existing
	if (data->initial_soa_copy != NULL) {
		next = ixfr_consume_rr(data->initial_soa_copy, data);
		knot_rrset_free(data->initial_soa_copy, data->mm);
		data->initial_soa_copy = NULL;
		if (next != KNOT_STATE_CONSUME) {
			return next;
		}
	}

	// Process answer packet
	xfr_stats_add(&data->stats, pkt->size + knot_rrset_size(pkt->tsig_rr));
	next = ixfr_consume_packet(pkt, data);

	// Finalize
	if (next == KNOT_STATE_DONE) {
		xfr_stats_end(&data->stats);
	}

	return next;
}

static int soa_query_produce(knot_layer_t *layer, knot_pkt_t *pkt)
{
	struct refresh_data *data = layer->data;

	query_init_pkt(pkt);

	data->ret = knot_pkt_put_question(pkt, data->zone->name, KNOT_CLASS_IN,
	                                  KNOT_RRTYPE_SOA);
	if (data->ret != KNOT_EOK) {
		return KNOT_STATE_FAIL;
	}

	return KNOT_STATE_CONSUME;
}

static bool wait4pinned_master(struct refresh_data *data)
{
	// Master pinning not enabled.
	if (data->fallback->pin_tol == 0) {
		return false;
	// Don't restrict refresh from the pinned master.
	} else if (data->fallback->trying_last) {
		return false;
	// Pinned master expected but not yet set, force AXFR (e.g. dropped timers).
	} else if (data->zone->timers.last_master.sin6_family == AF_UNSPEC) {
		data->xfr_type = XFR_TYPE_AXFR;
		return false;
	}

	time_t now = time(NULL);
	// Starting countdown for master transition.
	if (data->zone->timers.master_pin_hit == 0) {
		data->zone->timers.master_pin_hit = now;
		zone_events_schedule_at(data->zone, ZONE_EVENT_REFRESH, now + data->fallback->pin_tol);
	// Switch to a new master.
	} else if (data->zone->timers.master_pin_hit + data->fallback->pin_tol <= now) {
		data->xfr_type = XFR_TYPE_AXFR;
		return false;
	}

	return true;
}

static int soa_query_consume(knot_layer_t *layer, knot_pkt_t *pkt)
{
	struct refresh_data *data = layer->data;

	if (knot_pkt_ext_rcode(pkt) != KNOT_RCODE_NOERROR) {
		REFRESH_LOG_PROTO(LOG_WARNING, data,
		                  "server responded with error '%s'",
		                  knot_pkt_ext_rcode_name(pkt));
		data->ret = KNOT_EDENIED;
		return KNOT_STATE_FAIL;
	}

	const knot_pktsection_t *answer = knot_pkt_section(pkt, KNOT_ANSWER);
	const knot_rrset_t *rr = answer->count == 1 ? knot_pkt_rr(answer, 0) : NULL;
	if (!rr || rr->type != KNOT_RRTYPE_SOA || rr->rrs.count != 1) {
		REFRESH_LOG_PROTO(LOG_WARNING, data, "malformed message");
		conf_val_t val = conf_zone_get(data->conf, C_SEM_CHECKS, data->zone->name);
		if (conf_opt(&val) == SEMCHECKS_SOFT) {
			data->xfr_type = XFR_TYPE_AXFR;
			data->state = STATE_TRANSFER;
			return KNOT_STATE_RESET;
		} else {
			data->ret = KNOT_EMALF;
			return KNOT_STATE_FAIL;
		}
	}

	uint32_t local_serial;
	data->ret = slave_zone_serial(data->zone, data->conf, &local_serial);
	if (data->ret != KNOT_EOK) {
		xfr_log_read_ms(data->zone->name, data->ret);
		data->fallback->remote = false;
		return KNOT_STATE_FAIL;
	}
	uint32_t remote_serial = knot_soa_serial(rr->rrs.rdata);
	bool current = serial_is_current(local_serial, remote_serial);
	bool master_uptodate = serial_is_current(remote_serial, local_serial);

	if (!current) {
		if (wait4pinned_master(data)) {
			REFRESH_LOG_PROTO(LOG_INFO, data,
			                  "remote serial %u, zone is outdated, waiting for pinned master",
			                  remote_serial);
			return KNOT_STATE_DONE;
		}
		REFRESH_LOG(LOG_INFO, data,
		            "remote serial %u, zone is outdated", remote_serial);
		data->state = STATE_TRANSFER;
		return KNOT_STATE_RESET; // continue with transfer
	} else if (master_uptodate) {
		consume_edns_expire(data, pkt, false);
		finalize_timers(data);
		char expires_in[32] = "";
		fill_expires_in(expires_in, sizeof(expires_in), data);
		REFRESH_LOG_PROTO(LOG_INFO, data,
		                  "remote serial %u, zone is up-to-date%s",
		                  remote_serial, expires_in);
		return KNOT_STATE_DONE;
	} else {
		finalize_timers_noexpire(data);
		REFRESH_LOG_PROTO(LOG_INFO, data,
		                  "remote serial %u, remote is outdated", remote_serial);
		return KNOT_STATE_DONE;
	}
}

static int transfer_produce(knot_layer_t *layer, knot_pkt_t *pkt)
{
	struct refresh_data *data = layer->data;

	query_init_pkt(pkt);

	bool ixfr = (data->xfr_type == XFR_TYPE_IXFR);

	data->ret = knot_pkt_put_question(pkt, data->zone->name, KNOT_CLASS_IN,
	                                  ixfr ? KNOT_RRTYPE_IXFR : KNOT_RRTYPE_AXFR);
	if (data->ret != KNOT_EOK) {
		return KNOT_STATE_FAIL;
	}

	if (ixfr) {
		assert(data->soa);
		knot_rrset_t *sending_soa = knot_rrset_copy(data->soa, data->mm);
		uint32_t master_serial;
		data->ret = slave_zone_serial(data->zone, data->conf, &master_serial);
		if (data->ret != KNOT_EOK) {
			data->fallback->remote = false;
			xfr_log_read_ms(data->zone->name, data->ret);
		}
		if (sending_soa == NULL || data->ret != KNOT_EOK) {
			knot_rrset_free(sending_soa, data->mm);
			return KNOT_STATE_FAIL;
		}
		knot_soa_serial_set(sending_soa->rrs.rdata, master_serial);
		knot_pkt_begin(pkt, KNOT_AUTHORITY);
		knot_pkt_put(pkt, KNOT_COMPR_HINT_QNAME, sending_soa, 0);
		knot_rrset_free(sending_soa, data->mm);
	}

	return KNOT_STATE_CONSUME;
}

static int transfer_consume(knot_layer_t *layer, knot_pkt_t *pkt)
{
	struct refresh_data *data = layer->data;

	consume_edns_expire(data, pkt, true);
	if (data->expire_timer < 2) {
		REFRESH_LOG(LOG_WARNING, data,
		            "remote is expired, ignoring");
		return KNOT_STATE_IGNORE;
	}

	data->fallback_axfr = (data->xfr_type == XFR_TYPE_IXFR);

	int next = (data->xfr_type == XFR_TYPE_AXFR) ? axfr_consume(pkt, data, false) :
	                                               ixfr_consume(pkt, data);

	// Transfer completed
	if (next == KNOT_STATE_DONE) {
		// Log transfer even if we still can fail
		uint32_t serial;
		switch (data->xfr_type) {
		case XFR_TYPE_AXFR:
			serial = zone_contents_serial(data->axfr.zone);
			break;
		case XFR_TYPE_IXFR:
			serial = knot_soa_serial(data->ixfr.final_soa->rrs.rdata);
			break;
		case XFR_TYPE_UPTODATE:
			if (slave_zone_serial(data->zone, data->conf, &serial) == KNOT_EOK) {
				break;
			}
			// FALLTHROUGH
		default:
			serial = 0;
		}
		char serial_log[32];
		(void)snprintf(serial_log, sizeof(serial_log),
		               " remote serial %u,", serial);
		xfr_log_finished(data->zone->name,
		                 data->xfr_type == XFR_TYPE_IXFR ||
		                 data->xfr_type == XFR_TYPE_UPTODATE ?
		                 LOG_OPERATION_IXFR : LOG_OPERATION_AXFR,
		                 LOG_DIRECTION_IN, &data->remote->addr,
		                 flags2proto(layer->flags),
		                 data->remote->key.name,
		                 serial_log, &data->stats);

		/*
		 * TODO: Move finialization into finish
		 * callback. And update requestor to allow reset from fallback
		 * as we need IXFR to AXFR failover.
		 */
		if (tsig_unsigned_count(layer->tsig) != 0) {
			data->ret = KNOT_EMALF;
			return KNOT_STATE_FAIL;
		}

		// Finalize and publish the zone
		switch (data->xfr_type) {
		case XFR_TYPE_IXFR:
			data->ret = ixfr_finalize(data);
			break;
		case XFR_TYPE_AXFR:
			data->ret = axfr_finalize(data);
			break;
		default:
			return next;
		}
		if (data->ret == KNOT_EOK) {
			data->updated = true;
		} else {
			next = KNOT_STATE_FAIL;
		}
	}

	return next;
}

static int refresh_begin(knot_layer_t *layer, void *_data)
{
	layer->data = _data;
	struct refresh_data *data = _data;
	data->layer = layer;

	if (data->soa) {
		data->state = STATE_SOA_QUERY;
		data->xfr_type = XFR_TYPE_IXFR;
		data->initial_soa_copy = NULL;
	} else {
		data->state = STATE_TRANSFER;
		data->xfr_type = XFR_TYPE_AXFR;
		data->initial_soa_copy = NULL;
	}

	data->started = time_now();

	return KNOT_STATE_PRODUCE;
}

static int refresh_produce(knot_layer_t *layer, knot_pkt_t *pkt)
{
	struct refresh_data *data = layer->data;
	data->layer = layer;

	switch (data->state) {
	case STATE_SOA_QUERY: return soa_query_produce(layer, pkt);
	case STATE_TRANSFER:  return transfer_produce(layer, pkt);
	default:
		return KNOT_STATE_FAIL;
	}
}

static int refresh_consume(knot_layer_t *layer, knot_pkt_t *pkt)
{
	struct refresh_data *data = layer->data;
	data->layer = layer;

	data->fallback->address = false; // received something, other address not needed

	switch (data->state) {
	case STATE_SOA_QUERY: return soa_query_consume(layer, pkt);
	case STATE_TRANSFER:  return transfer_consume(layer, pkt);
	default:
		return KNOT_STATE_FAIL;
	}
}

static int refresh_reset(knot_layer_t *layer)
{
	return KNOT_STATE_PRODUCE;
}

static int refresh_finish(knot_layer_t *layer)
{
	struct refresh_data *data = layer->data;
	data->layer = layer;

	// clean processing context
	axfr_cleanup(data);
	ixfr_cleanup(data);

	return KNOT_STATE_NOOP;
}

static const knot_layer_api_t REFRESH_API = {
	.begin = refresh_begin,
	.produce = refresh_produce,
	.consume = refresh_consume,
	.reset = refresh_reset,
	.finish = refresh_finish,
};

static size_t max_zone_size(conf_t *conf, const knot_dname_t *zone)
{
	conf_val_t val = conf_zone_get(conf, C_ZONE_MAX_SIZE, zone);
	return conf_int(&val);
}

typedef struct {
	bool force_axfr;
	bool send_notify;
	bool ixfr_by_one;
	bool ixfr_from_axfr;
	bool more_xfr;
} try_refresh_ctx_t;

static int try_refresh(conf_t *conf, zone_t *zone, const conf_remote_t *master,
                       void *ctx, zone_master_fallback_t *fallback)
{
	// TODO: Abstract interface to issue DNS queries. This is almost copy-pasted.

	assert(zone);
	assert(master);
	assert(ctx);
	assert(fallback);

	try_refresh_ctx_t *trctx = ctx;

	knot_rrset_t *soa = NULL;
	if (zone->contents) {
		rcu_read_lock();
		knot_rrset_t tmp = node_rrset(zone->contents->apex, KNOT_RRTYPE_SOA);
		soa = knot_rrset_copy(&tmp, NULL);
		rcu_read_unlock();
		if (soa == NULL) {
			return KNOT_ENOMEM;
		}
	}

	struct refresh_data data = {
		.zone = zone,
		.conf = conf,
		.remote = master,
		.soa = zone->contents && !trctx->force_axfr ? soa : NULL,
		.max_zone_size = max_zone_size(conf, zone->name),
		.edns = query_edns_data_init(conf, master, QUERY_EDNS_OPT_EXPIRE),
		.expire_timer = EXPIRE_TIMER_INVALID,
		.fallback = fallback,
		.fallback_axfr = false, // will be set upon IXFR consume
		.ixfr_by_one = trctx->ixfr_by_one,
		.ixfr_from_axfr = trctx->ixfr_from_axfr,
	};

	knot_requestor_t requestor;
	knot_requestor_init(&requestor, &REFRESH_API, &data, NULL);

	knot_pkt_t *pkt = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, NULL);
	if (pkt == NULL) {
		knot_requestor_clear(&requestor);
		knot_rrset_free(soa, NULL);
		return KNOT_ENOMEM;
	}

	knot_request_flag_t flags = conf->cache.srv_tcp_fastopen ? KNOT_REQUEST_TFO : 0;
	knot_request_t *req = knot_request_make(NULL, master, pkt, zone->server->quic_creds,
	                                        &data.edns, flags);
	if (req == NULL) {
		knot_requestor_clear(&requestor);
		knot_rrset_free(soa, NULL);
		return KNOT_ENOMEM;
	}

	int timeout = conf->cache.srv_tcp_remote_io_timeout;

	int ret;

	// while loop runs 0x or 1x; IXFR to AXFR failover
	while (ret = knot_requestor_exec(&requestor, req, timeout),
	       ret = (data.ret == KNOT_EOK ? ret : data.ret),
	       !(requestor.layer.flags & KNOT_REQUESTOR_IOFAIL) &&
	       data.fallback_axfr && ret != KNOT_EOK) {
		REFRESH_LOG(LOG_WARNING, &data,
		            "fallback to AXFR (%s)", knot_strerror(ret));
		ixfr_cleanup(&data);
		data.ret = KNOT_EOK;
		data.xfr_type = XFR_TYPE_AXFR;
		data.fallback_axfr = false,
		requestor.layer.state = KNOT_STATE_RESET;
		requestor.layer.flags |= KNOT_REQUESTOR_CLOSE;
	}
	knot_request_free(req, NULL);
	knot_requestor_clear(&requestor);
	knot_rrset_free(soa, NULL);

	if (ret == KNOT_EOK) {
		trctx->send_notify = trctx->send_notify || (data.updated && !master->block_notify_after_xfr);
		trctx->force_axfr = false;
		trctx->more_xfr = trctx->more_xfr || (data.updated && data.ixfr_by_one && data.xfr_type == XFR_TYPE_IXFR);
	}

	return ret;
}

int event_refresh(conf_t *conf, zone_t *zone)
{
	assert(zone);

	if (!zone_is_slave(conf, zone)) {
		return KNOT_ENOTSUP;
	}

	try_refresh_ctx_t trctx = { 0 };

	// TODO: Flag on zone is ugly. Event specific parameters would be nice.
	if (zone_get_flag(zone, ZONE_FORCE_AXFR, true)) {
		trctx.force_axfr = true;
		zone->zonefile.retransfer = true;
	}

	conf_val_t val = conf_zone_get(conf, C_IXFR_BY_ONE, zone->name);
	trctx.ixfr_by_one = conf_bool(&val);
	val = conf_zone_get(conf, C_IXFR_FROM_AXFR, zone->name);
	trctx.ixfr_from_axfr = conf_bool(&val);

	int ret = zone_master_try(conf, zone, try_refresh, &trctx, "refresh");
	zone_clear_preferred_master(zone);
	if (ret != KNOT_EOK) {
		const knot_rdataset_t *soa = zone_soa(zone);
		uint32_t next;

		if (soa) {
			next = knot_soa_retry(soa->rdata);
		} else {
			next = bootstrap_next(&zone->zonefile.bootstrap_cnt);
		}

		limit_timer(conf, zone->name, &next, "retry",
		            C_RETRY_MIN_INTERVAL, C_RETRY_MAX_INTERVAL);
		time_t now = time(NULL);
		zone->timers.next_refresh = now + next;
		zone->timers.last_refresh_ok = false;

		char time_str[64] = { 0 };
		struct tm time_gm = { 0 };
		localtime_r(&zone->timers.next_refresh, &time_gm);
		strftime(time_str, sizeof(time_str), KNOT_LOG_TIME_FORMAT, &time_gm);

		char expires_in[32] = "";
		if (!zone->is_catalog_flag) {
			struct refresh_data data = {
				.zone = zone,
				.expire_timer = zone->timers.next_expire - now,
			};
			fill_expires_in(expires_in, sizeof(expires_in), &data);
		}

		log_zone_error(zone->name, "refresh, failed (%s), next retry at %s%s",
		               knot_strerror(ret), time_str, expires_in);
	} else {
		zone->zonefile.bootstrap_cnt = 0;
	}

	/* Reschedule events. */
	replan_from_timers(conf, zone);
	if (trctx.send_notify) {
		zone_schedule_notify(zone, 1);
	}
	if (trctx.more_xfr && ret == KNOT_EOK) {
		zone_events_schedule_now(zone, ZONE_EVENT_REFRESH);
	}

	return ret;
}
