/*  Copyright (C) 2023 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#pragma once

#include "libdnssec/keystore.h"
#include "contrib/spinlock.h"
#include "contrib/time.h"
#include "knot/conf/conf.h"
#include "knot/dnssec/kasp/kasp_zone.h"
#include "knot/dnssec/kasp/policy.h"

typedef struct {
	size_t rrsig_count;
	knot_time_t expire;

	knot_spin_t lock;
} zone_sign_stats_t;

/*!
 * \brief DNSSEC signing context.
 */
typedef struct {
	knot_time_t now;

	knot_lmdb_db_t *kasp_db;
	knot_kasp_zone_t *zone;
	knot_kasp_policy_t *policy;
	dnssec_keystore_t *keystore;
	unsigned keystore_type;

	char *kasp_zone_path;

	zone_sign_stats_t *stats;

	bool rrsig_drop_existing;
	bool keep_deleted_keys;
	bool keytag_conflict;
	bool validation_mode;

	unsigned dbus_event;

	key_records_t offline_records;
	knot_time_t offline_next_time;
} kdnssec_ctx_t;

/*!
 * \brief Initialize DNSSEC signing context.
 *
 * \param conf         Configuration.
 * \param ctx          Signing context to be initialized.
 * \param zone_name    Name of the zone.
 * \param kaspdb       Key and signature policy database.
 * \param from_module  Module identifier if initialized from a module.
 */
int kdnssec_ctx_init(conf_t *conf, kdnssec_ctx_t *ctx, const knot_dname_t *zone_name,
                     knot_lmdb_db_t *kaspdb, const conf_mod_id_t *from_module);

/*!
 * \brief Initialize DNSSEC validating context.
 *
 * \param conf    Configuration.
 * \param ctx     Signing context to be initialized.
 * \param zone    Zone contents to be validated.
 * \param threads The number of threads when conf is not available (0 for default).
 *
 * \return KNOT_E*
 */
int kdnssec_validation_ctx(conf_t *conf, kdnssec_ctx_t *ctx, const zone_contents_t *zone,
                           uint16_t threads);

/*!
 * \brief Save the changes in ctx (in kasp zone).
 */
int kdnssec_ctx_commit(kdnssec_ctx_t *ctx);

/*!
 * \brief Cleanup DNSSEC signing context.
 */
void kdnssec_ctx_deinit(kdnssec_ctx_t *ctx);
