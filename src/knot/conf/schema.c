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

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include "knot/conf/schema.h"
#include "knot/conf/confio.h"
#include "knot/conf/tools.h"
#include "knot/common/log.h"
#include "knot/updates/acl.h"
#include "knot/zone/zone-load.h"
#include "libknot/rrtype/opt.h"
#include "libdnssec/tsig.h"
#include "libdnssec/key.h"

#define HOURS(x)	((x) * 3600)
#define DAYS(x)		((x) * HOURS(24))

#define KILO(x)		(1024LLU * (x))
#define MEGA(x)		(KILO(1024) * (x))
#define GIGA(x)		(MEGA(1024) * (x))
#define TERA(x)		(GIGA(1024) * (x))

#define VIRT_MEM_TOP_32BIT	MEGA(500)
#define VIRT_MEM_LIMIT(x)	(((sizeof(void *) < 8) && ((x) > VIRT_MEM_TOP_32BIT)) \
				 ? VIRT_MEM_TOP_32BIT : (x))

static const knot_lookup_t keystore_backends[] = {
	{ KEYSTORE_BACKEND_PEM,    "pem" },
	{ KEYSTORE_BACKEND_PKCS11, "pkcs11" },
	{ 0, NULL }
};

static const knot_lookup_t tsig_key_algs[] = {
	{ DNSSEC_TSIG_HMAC_MD5,    "hmac-md5" },
	{ DNSSEC_TSIG_HMAC_SHA1,   "hmac-sha1" },
	{ DNSSEC_TSIG_HMAC_SHA224, "hmac-sha224" },
	{ DNSSEC_TSIG_HMAC_SHA256, "hmac-sha256" },
	{ DNSSEC_TSIG_HMAC_SHA384, "hmac-sha384" },
	{ DNSSEC_TSIG_HMAC_SHA512, "hmac-sha512" },
	{ 0, NULL }
};

static const knot_lookup_t dnssec_key_algs[] = {
	{ DNSSEC_KEY_ALGORITHM_RSA_SHA1,          "rsasha1" },
	{ DNSSEC_KEY_ALGORITHM_RSA_SHA1_NSEC3,    "rsasha1-nsec3-sha1" },
	{ DNSSEC_KEY_ALGORITHM_RSA_SHA256,        "rsasha256" },
	{ DNSSEC_KEY_ALGORITHM_RSA_SHA512,        "rsasha512" },
	{ DNSSEC_KEY_ALGORITHM_ECDSA_P256_SHA256, "ecdsap256sha256" },
	{ DNSSEC_KEY_ALGORITHM_ECDSA_P384_SHA384, "ecdsap384sha384" },
	{ DNSSEC_KEY_ALGORITHM_ED25519,           "ed25519" },
#ifdef HAVE_ED448
	{ DNSSEC_KEY_ALGORITHM_ED448,             "ed448" },
#endif
	{ 0, NULL }
};

static const knot_lookup_t unsafe_operation[] = {
	{ UNSAFE_NONE,       "none" },
	{ UNSAFE_KEYSET,     "no-check-keyset" },
	{ UNSAFE_DNSKEY,     "no-update-dnskey" },
	{ UNSAFE_NSEC,       "no-update-nsec" },
	{ UNSAFE_EXPIRED,    "no-update-expired" },
	{ 0, NULL }
};

static const knot_lookup_t cds_cdnskey[] = {
	{ CDS_CDNSKEY_NONE,      "none" },
	{ CDS_CDNSKEY_EMPTY,     "delete-dnssec" },
	{ CDS_CDNSKEY_ROLLOVER,  "rollover" },
	{ CDS_CDNSKEY_ALWAYS,    "always" },
	{ CDS_CDNSKEY_DOUBLE_DS, "double-ds" },
	{ 0, NULL }
};

static const knot_lookup_t dnskey_mgmt[] = {
	{ DNSKEY_MGMT_FULL,        "full" },
	{ DNSKEY_MGMT_INCREMENTAL, "incremental" },
	{ 0, NULL }
};

static const knot_lookup_t cds_digesttype[] = {
	{ DNSSEC_KEY_DIGEST_SHA256,   "sha256" },
	{ DNSSEC_KEY_DIGEST_SHA384,   "sha384" },
	{ 0, NULL }
};

const knot_lookup_t acl_actions[] = {
	{ ACL_ACTION_QUERY,    "query" },
	{ ACL_ACTION_NOTIFY,   "notify" },
	{ ACL_ACTION_TRANSFER, "transfer" },
	{ ACL_ACTION_UPDATE,   "update" },
	{ 0, NULL }
};

static const knot_lookup_t acl_update_owner[] = {
	{ ACL_UPDATE_OWNER_KEY,  "key" },
	{ ACL_UPDATE_OWNER_ZONE, "zone" },
	{ ACL_UPDATE_OWNER_NAME, "name" },
	{ 0, NULL }
};

static const knot_lookup_t acl_update_owner_match[] = {
	{ ACL_UPDATE_MATCH_SUBEQ,   "sub-or-equal" },
	{ ACL_UPDATE_MATCH_EQ,      "equal" },
	{ ACL_UPDATE_MATCH_SUB,     "sub" },
	{ ACL_UPDATE_MATCH_PATTERN, "pattern" },
	{ 0, NULL }
};

static const knot_lookup_t acl_protocol[] = {
	{ ACL_PROTOCOL_UDP,  "udp" },
	{ ACL_PROTOCOL_TCP,  "tcp" },
	{ ACL_PROTOCOL_TLS,  "tls" },
	{ ACL_PROTOCOL_QUIC, "quic" },
	{ 0, NULL }
};

static const knot_lookup_t serial_policies[] = {
	{ SERIAL_POLICY_INCREMENT,  "increment" },
	{ SERIAL_POLICY_UNIXTIME,   "unixtime" },
	{ SERIAL_POLICY_DATESERIAL, "dateserial" },
	{ 0, NULL }
};

static const knot_lookup_t semantic_checks[] = {
	{ SEMCHECKS_OFF,  "off" },
	{ SEMCHECKS_OFF,  "false" },
	{ SEMCHECKS_ON,   "on" },
	{ SEMCHECKS_ON,   "true" },
	{ SEMCHECKS_SOFT, "soft" },
	{ 0, NULL }
};

static const knot_lookup_t zone_digest[] = {
	{ ZONE_DIGEST_NONE,   "none" },
	{ ZONE_DIGEST_SHA384, "zonemd-sha384" },
	{ ZONE_DIGEST_SHA512, "zonemd-sha512" },
	{ ZONE_DIGEST_REMOVE, "remove" },
	{ 0, NULL }
};

static const knot_lookup_t journal_content[] = {
	{ JOURNAL_CONTENT_NONE,    "none" },
	{ JOURNAL_CONTENT_CHANGES, "changes" },
	{ JOURNAL_CONTENT_ALL,     "all" },
	{ 0, NULL }
};

static const knot_lookup_t zonefile_load[] = {
	{ ZONEFILE_LOAD_NONE,  "none" },
	{ ZONEFILE_LOAD_DIFF,  "difference" },
	{ ZONEFILE_LOAD_DIFSE, "difference-no-serial" },
	{ ZONEFILE_LOAD_WHOLE, "whole" },
	{ 0, NULL }
};

static const knot_lookup_t log_severities[] = {
	{ LOG_UPTO(LOG_CRIT),    "critical" },
	{ LOG_UPTO(LOG_ERR),     "error" },
	{ LOG_UPTO(LOG_WARNING), "warning" },
	{ LOG_UPTO(LOG_NOTICE),  "notice" },
	{ LOG_UPTO(LOG_INFO),    "info" },
	{ LOG_UPTO(LOG_DEBUG),   "debug" },
	{ 0, NULL }
};

static const knot_lookup_t journal_modes[] = {
	{ JOURNAL_MODE_ROBUST, "robust" },
	{ JOURNAL_MODE_ASYNC,  "asynchronous" },
	{ 0, NULL }
};

static const knot_lookup_t catalog_roles[] = {
	{ CATALOG_ROLE_NONE,      "none" },
	{ CATALOG_ROLE_INTERPRET, "interpret" },
	{ CATALOG_ROLE_GENERATE,  "generate" },
	{ CATALOG_ROLE_MEMBER,    "member" },
	{ 0, NULL }
};

static const knot_lookup_t dbus_events[] = {
	{ DBUS_EVENT_NONE,            "none" },
	{ DBUS_EVENT_RUNNING,         "running" },
	{ DBUS_EVENT_ZONE_UPDATED,    "zone-updated" },
	{ DBUS_EVENT_KEYS_UPDATED,    "keys-updated" },
	{ DBUS_EVENT_ZONE_SUBMISSION, "ksk-submission" },
	{ DBUS_EVENT_ZONE_INVALID,    "dnssec-invalid" },
	{ 0, NULL }
};

static const yp_item_t desc_module[] = {
	{ C_ID,      YP_TSTR, YP_VNONE, YP_FNONE, { check_module_id } },
	{ C_FILE,    YP_TSTR, YP_VNONE },
	{ C_COMMENT, YP_TSTR, YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_server[] = {
	{ C_IDENT,                YP_TSTR,  YP_VNONE },
	{ C_VERSION,              YP_TSTR,  YP_VNONE },
	{ C_NSID,                 YP_THEX,  YP_VNONE },
	{ C_RUNDIR,               YP_TSTR,  YP_VSTR = { RUN_DIR } },
	{ C_USER,                 YP_TSTR,  YP_VNONE },
	{ C_PIDFILE,              YP_TSTR,  YP_VSTR = { "knot.pid" } },
	{ C_UDP_WORKERS,          YP_TINT,  YP_VINT = { 1, CONF_MAX_UDP_WORKERS, YP_NIL } },
	{ C_TCP_WORKERS,          YP_TINT,  YP_VINT = { 1, CONF_MAX_TCP_WORKERS, YP_NIL } },
	{ C_BG_WORKERS,           YP_TINT,  YP_VINT = { 1, CONF_MAX_BG_WORKERS, YP_NIL } },
	{ C_ASYNC_START,          YP_TBOOL, YP_VNONE },
	{ C_TCP_IDLE_TIMEOUT,     YP_TINT,  YP_VINT = { 1, INT32_MAX, 10, YP_STIME } },
	{ C_TCP_IO_TIMEOUT,       YP_TINT,  YP_VINT = { 0, INT32_MAX, 500 } },
	{ C_TCP_RMT_IO_TIMEOUT,   YP_TINT,  YP_VINT = { 0, INT32_MAX, 5000 } },
	{ C_TCP_MAX_CLIENTS,      YP_TINT,  YP_VINT = { 0, INT32_MAX, YP_NIL } },
	{ C_TCP_REUSEPORT,        YP_TBOOL, YP_VNONE },
	{ C_TCP_FASTOPEN,         YP_TBOOL, YP_VNONE },
	{ C_QUIC_MAX_CLIENTS,     YP_TINT,  YP_VINT = { 128, INT32_MAX, 10000 } },
	{ C_QUIC_OUTBUF_MAX_SIZE, YP_TINT,  YP_VINT = { MEGA(1), SSIZE_MAX, MEGA(100), YP_SSIZE } },
	{ C_QUIC_IDLE_CLOSE,      YP_TINT,  YP_VINT = { 1, INT32_MAX, 4, YP_STIME } },
	{ C_RMT_POOL_LIMIT,       YP_TINT,  YP_VINT = { 0, INT32_MAX, 0 } },
	{ C_RMT_POOL_TIMEOUT,     YP_TINT,  YP_VINT = { 1, INT32_MAX, 5, YP_STIME } },
	{ C_RMT_RETRY_DELAY,      YP_TINT,  YP_VINT = { 0, INT32_MAX, 0 } },
	{ C_SOCKET_AFFINITY,      YP_TBOOL, YP_VNONE },
	{ C_UDP_MAX_PAYLOAD,      YP_TINT,  YP_VINT = { KNOT_EDNS_MIN_DNSSEC_PAYLOAD,
	                                                KNOT_EDNS_MAX_UDP_PAYLOAD,
	                                                1232, YP_SSIZE } },
	{ C_UDP_MAX_PAYLOAD_IPV4, YP_TINT,  YP_VINT = { KNOT_EDNS_MIN_DNSSEC_PAYLOAD,
	                                                KNOT_EDNS_MAX_UDP_PAYLOAD,
	                                                1232, YP_SSIZE } },
	{ C_UDP_MAX_PAYLOAD_IPV6, YP_TINT,  YP_VINT = { KNOT_EDNS_MIN_DNSSEC_PAYLOAD,
	                                                KNOT_EDNS_MAX_UDP_PAYLOAD,
	                                                1232, YP_SSIZE } },
	{ C_CERT_FILE,            YP_TSTR,  YP_VNONE, YP_FNONE },
	{ C_KEY_FILE,             YP_TSTR,  YP_VNONE, YP_FNONE },
	{ C_ECS,                  YP_TBOOL, YP_VNONE },
	{ C_ANS_ROTATION,         YP_TBOOL, YP_VNONE },
	{ C_AUTO_ACL,             YP_TBOOL, YP_VNONE },
	{ C_PROXY_ALLOWLIST,      YP_TNET,  YP_VNONE, YP_FMULTI},
	{ C_DBUS_EVENT,           YP_TOPT,  YP_VOPT = { dbus_events, DBUS_EVENT_NONE }, YP_FMULTI },
	{ C_DBUS_INIT_DELAY,      YP_TINT,  YP_VINT = { 0, INT32_MAX, 1, YP_STIME } },
	{ C_LISTEN,               YP_TADDR, YP_VADDR = { 53 }, YP_FMULTI, { check_listen } },
	{ C_LISTEN_QUIC,          YP_TADDR, YP_VADDR = { 853 }, YP_FMULTI, { check_listen } },
	{ C_LISTEN_TLS,           YP_TADDR, YP_VADDR = { 853 }, YP_FMULTI, { check_listen } },
	{ C_COMMENT,              YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_xdp[] = {
	{ C_LISTEN,               YP_TADDR, YP_VADDR = { 53 }, YP_FMULTI, { check_xdp_listen } },
	{ C_UDP,                  YP_TBOOL, YP_VBOOL = { true } },
	{ C_TCP,                  YP_TBOOL, YP_VNONE },
	{ C_QUIC,                 YP_TBOOL, YP_VNONE },
	{ C_QUIC_PORT,            YP_TINT,  YP_VINT = { 1, 65535, 853 } },
	{ C_TCP_MAX_CLIENTS,      YP_TINT,  YP_VINT = { 1024, INT32_MAX, 1000000 } },
	{ C_TCP_INBUF_MAX_SIZE,   YP_TINT,  YP_VINT = { MEGA(1), SSIZE_MAX, MEGA(100), YP_SSIZE } },
	{ C_TCP_OUTBUF_MAX_SIZE,  YP_TINT,  YP_VINT = { MEGA(1), SSIZE_MAX, MEGA(100), YP_SSIZE } },
	{ C_TCP_IDLE_CLOSE,       YP_TINT,  YP_VINT = { 1, INT32_MAX, 10, YP_STIME } },
	{ C_TCP_IDLE_RESET,       YP_TINT,  YP_VINT = { 1, INT32_MAX, 20, YP_STIME } },
	{ C_TCP_RESEND,           YP_TINT,  YP_VINT = { 1, INT32_MAX, 5, YP_STIME } },
	{ C_ROUTE_CHECK,          YP_TBOOL, YP_VNONE },
	{ C_RING_SIZE,            YP_TINT,  YP_VINT = { 4, 32768, 2048 } },
	{ C_BUSYPOLL_BUDGET,      YP_TINT,  YP_VINT = { 0, UINT16_MAX, 0 } },
	{ C_BUSYPOLL_TIMEOUT,     YP_TINT,  YP_VINT = { 1, UINT16_MAX, 20 } },
	{ C_COMMENT,              YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_control[] = {
	{ C_LISTEN,  YP_TSTR, YP_VSTR = { "knot.sock" } },
	{ C_BACKLOG, YP_TINT, YP_VINT = { 0, UINT16_MAX, 5 } },
	{ C_TIMEOUT, YP_TINT, YP_VINT = { 0, INT32_MAX / 1000, 5, YP_STIME } },
	{ C_COMMENT, YP_TSTR, YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_log[] = {
	{ C_TARGET,  YP_TSTR, YP_VNONE },
	{ C_SERVER,  YP_TOPT, YP_VOPT = { log_severities, 0 } },
	{ C_CTL,     YP_TOPT, YP_VOPT = { log_severities, 0 } },
	{ C_ZONE,    YP_TOPT, YP_VOPT = { log_severities, 0 } },
	{ C_QUIC,    YP_TOPT, YP_VOPT = { log_severities, 0 } },
	{ C_ANY,     YP_TOPT, YP_VOPT = { log_severities, 0 } },
	{ C_COMMENT, YP_TSTR, YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_stats[] = {
	{ C_TIMER,   YP_TINT,  YP_VINT = { 1, UINT32_MAX, 0, YP_STIME } },
	{ C_FILE,    YP_TSTR,  YP_VSTR = { "stats.yaml" } },
	{ C_APPEND,  YP_TBOOL, YP_VNONE },
	{ C_COMMENT, YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_database[] = {
	{ C_STORAGE,             YP_TSTR,  YP_VSTR = { STORAGE_DIR } },
	{ C_JOURNAL_DB,          YP_TSTR,  YP_VSTR = { "journal" } },
	{ C_JOURNAL_DB_MODE,     YP_TOPT,  YP_VOPT = { journal_modes, JOURNAL_MODE_ROBUST } },
	{ C_JOURNAL_DB_MAX_SIZE, YP_TINT,  YP_VINT = { MEGA(1), VIRT_MEM_LIMIT(TERA(100)),
	                                               VIRT_MEM_LIMIT(GIGA(20)), YP_SSIZE } },
	{ C_KASP_DB,             YP_TSTR,  YP_VSTR = { "keys" } },
	{ C_KASP_DB_MAX_SIZE,    YP_TINT,  YP_VINT = { MEGA(5), VIRT_MEM_LIMIT(GIGA(100)),
	                                               MEGA(500), YP_SSIZE } },
	{ C_TIMER_DB,            YP_TSTR,  YP_VSTR = { "timers" } },
	{ C_TIMER_DB_MAX_SIZE,   YP_TINT,  YP_VINT = { MEGA(1), VIRT_MEM_LIMIT(GIGA(100)),
	                                               MEGA(100), YP_SSIZE } },
	{ C_CATALOG_DB,          YP_TSTR,  YP_VSTR = { "catalog" } },
	{ C_CATALOG_DB_MAX_SIZE, YP_TINT,  YP_VINT = { MEGA(5), VIRT_MEM_LIMIT(GIGA(100)),
	                                               VIRT_MEM_LIMIT(GIGA(20)), YP_SSIZE } },
	{ C_COMMENT,             YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_keystore[] = {
	{ C_ID,        YP_TSTR,  YP_VNONE },
	{ C_BACKEND,   YP_TOPT,  YP_VOPT = { keystore_backends, KEYSTORE_BACKEND_PEM },
	                         CONF_IO_FRLD_ZONES },
	{ C_CONFIG,    YP_TSTR,  YP_VSTR = { "keys" }, CONF_IO_FRLD_ZONES },
	{ C_KEY_LABEL, YP_TBOOL, YP_VNONE },
	{ C_COMMENT,   YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_key[] = {
	{ C_ID,      YP_TDNAME, YP_VNONE },
	{ C_ALG,     YP_TOPT,   YP_VOPT = { tsig_key_algs, DNSSEC_TSIG_HMAC_SHA256 } },
	{ C_SECRET,  YP_TB64,   YP_VNONE },
	{ C_COMMENT, YP_TSTR,   YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_remote[] = {
	{ C_ID,               YP_TSTR,  YP_VNONE, CONF_IO_FREF },
	{ C_ADDR,             YP_TADDR, YP_VADDR = { 53, 853 }, YP_FMULTI },
	{ C_VIA,              YP_TADDR, YP_VNONE, YP_FMULTI },
	{ C_QUIC,             YP_TBOOL, YP_VNONE },
	{ C_TLS,              YP_TBOOL, YP_VNONE },
	{ C_KEY,              YP_TREF,  YP_VREF = { C_KEY }, YP_FNONE, { check_ref } },
	{ C_CERT_KEY,         YP_TB64,  YP_VNONE, YP_FMULTI, { check_cert_pin } },
	{ C_BLOCK_NOTIFY_XFR, YP_TBOOL, YP_VNONE },
	{ C_NO_EDNS,          YP_TBOOL, YP_VNONE },
	{ C_AUTO_ACL,         YP_TBOOL, YP_VBOOL = { true } },
	{ C_COMMENT,          YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_remotes[] = {
	{ C_ID,               YP_TSTR,  YP_VNONE, CONF_IO_FREF },
	{ C_RMT,              YP_TREF,  YP_VREF = { C_RMT }, YP_FMULTI, { check_ref } },
	{ C_COMMENT,          YP_TSTR,  YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_acl[] = {
	{ C_ID,                 YP_TSTR,   YP_VNONE, CONF_IO_FREF },
	{ C_ADDR,               YP_TNET,   YP_VNONE, YP_FMULTI },
	{ C_KEY,                YP_TREF,   YP_VREF = { C_KEY }, YP_FMULTI, { check_ref } },
	{ C_RMT,                YP_TREF,   YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI, { check_ref } },
	{ C_ACTION,             YP_TOPT,   YP_VOPT = { acl_actions, ACL_ACTION_QUERY }, YP_FMULTI },
	{ C_PROTOCOL,           YP_TOPT,   YP_VOPT = { acl_protocol, ACL_PROTOCOL_NONE }, YP_FMULTI },
	{ C_DENY,               YP_TBOOL,  YP_VNONE },
	{ C_UPDATE_TYPE,        YP_TDATA,  YP_VDATA = { 0, NULL, rrtype_to_bin, rrtype_to_txt },
	                                   YP_FMULTI, },
	{ C_UPDATE_OWNER,       YP_TOPT,   YP_VOPT = { acl_update_owner, ACL_UPDATE_OWNER_NONE } },
	{ C_UPDATE_OWNER_MATCH, YP_TOPT,   YP_VOPT = { acl_update_owner_match, ACL_UPDATE_MATCH_SUBEQ } },
	{ C_UPDATE_OWNER_NAME,  YP_TDATA,  YP_VDATA = { 0, NULL, rdname_to_bin, rdname_to_txt },
	                                   YP_FMULTI, },
	{ C_CERT_KEY,           YP_TB64,   YP_VNONE, YP_FMULTI, { check_cert_pin } },
	{ C_COMMENT,            YP_TSTR,   YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_submission[] = {
	{ C_ID,           YP_TSTR, YP_VNONE },
	{ C_PARENT,       YP_TREF, YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI, { check_ref } },
	{ C_CHK_INTERVAL, YP_TINT, YP_VINT = { 1, UINT32_MAX, HOURS(1), YP_STIME } },
	{ C_TIMEOUT,      YP_TINT, YP_VINT = { 0, UINT32_MAX, 0, YP_STIME },
	                           CONF_IO_FRLD_ZONES },
	{ C_PARENT_DELAY, YP_TINT, YP_VINT = { 0, UINT32_MAX, 0, YP_STIME } },
	{ C_COMMENT,      YP_TSTR, YP_VNONE },
	{ NULL }
};

static const yp_item_t desc_dnskey_sync[] = {
	{ C_ID,           YP_TSTR, YP_VNONE },
	{ C_RMT,          YP_TREF, YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI, { check_ref } },
	{ C_CHK_INTERVAL, YP_TINT, YP_VINT = { 1, UINT32_MAX, 60, YP_STIME } },
	{ NULL }
};

static const yp_item_t desc_policy[] = {
	{ C_ID,                  YP_TSTR,  YP_VNONE, CONF_IO_FREF },
	{ C_KEYSTORE,            YP_TREF,  YP_VREF = { C_KEYSTORE }, CONF_IO_FRLD_ZONES,
	                                   { check_ref_dflt } },
	{ C_MANUAL,              YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_SINGLE_TYPE_SIGNING, YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_ALG,                 YP_TOPT,  YP_VOPT = { dnssec_key_algs,
	                                               DNSSEC_KEY_ALGORITHM_ECDSA_P256_SHA256 },
	                                   CONF_IO_FRLD_ZONES },
	{ C_KSK_SIZE,            YP_TINT,  YP_VINT = { 0, UINT16_MAX, YP_NIL, YP_SSIZE },
	                                   CONF_IO_FRLD_ZONES },
	{ C_ZSK_SIZE,            YP_TINT,  YP_VINT = { 0, UINT16_MAX, YP_NIL, YP_SSIZE },
	                                   CONF_IO_FRLD_ZONES },
	{ C_KSK_SHARED,          YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_DNSKEY_TTL,          YP_TINT,  YP_VINT = { 0, INT32_MAX, YP_NIL, YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_ZONE_MAX_TTL,        YP_TINT,  YP_VINT = { 0, INT32_MAX, YP_NIL, YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_KEYTAG_MODULO,       YP_TSTR,  YP_VSTR = { "0/1" }, YP_FNONE, { check_modulo } }, \
	{ C_KSK_LIFETIME,        YP_TINT,  YP_VINT = { 0, UINT32_MAX, 0, YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_ZSK_LIFETIME,        YP_TINT,  YP_VINT = { 0, UINT32_MAX, DAYS(30), YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_DELETE_DELAY,        YP_TINT,  YP_VINT = { 0, UINT32_MAX, 0, YP_STIME } },
	{ C_PROPAG_DELAY,        YP_TINT,  YP_VINT = { 0, INT32_MAX, HOURS(1), YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_RRSIG_LIFETIME,      YP_TINT,  YP_VINT = { 1, INT32_MAX, DAYS(14), YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_RRSIG_REFRESH,       YP_TINT,  YP_VINT = { 1, INT32_MAX, YP_NIL, YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_RRSIG_PREREFRESH,    YP_TINT,  YP_VINT = { 0, INT32_MAX, HOURS(1), YP_STIME, DAYS(1) },
	                                   CONF_IO_FRLD_ZONES },
	{ C_REPRO_SIGNING,       YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_NSEC3,               YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_NSEC3_ITER,          YP_TINT,  YP_VINT = { 0, UINT16_MAX, 0 }, CONF_IO_FRLD_ZONES },
	{ C_NSEC3_OPT_OUT,       YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_NSEC3_SALT_LEN,      YP_TINT,  YP_VINT = { 0, UINT8_MAX, 8 }, CONF_IO_FRLD_ZONES },
	{ C_NSEC3_SALT_LIFETIME, YP_TINT,  YP_VINT = { -1, UINT32_MAX, DAYS(30), YP_STIME },
	                                   CONF_IO_FRLD_ZONES },
	{ C_SIGNING_THREADS,     YP_TINT,  YP_VINT = { 1, UINT16_MAX, 1 } },
	{ C_KSK_SBM,             YP_TREF,  YP_VREF = { C_SBM }, CONF_IO_FRLD_ZONES,
	                                   { check_ref } },
	{ C_DS_PUSH,             YP_TREF,  YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI | CONF_IO_FRLD_ZONES,
	                                   { check_ref } },
	{ C_DNSKEY_SYNC,         YP_TREF,  YP_VREF = { C_DNSKEY_SYNC }, CONF_IO_FRLD_ZONES,
	                                   { check_ref } },
	{ C_CDS_CDNSKEY,         YP_TOPT,  YP_VOPT = { cds_cdnskey, CDS_CDNSKEY_ROLLOVER },
	                                   CONF_IO_FRLD_ZONES },
	{ C_CDS_DIGESTTYPE,      YP_TOPT,  YP_VOPT = { cds_digesttype, DNSSEC_KEY_DIGEST_SHA256 },
	                                   CONF_IO_FRLD_ZONES },
	{ C_DNSKEY_MGMT,         YP_TOPT,  YP_VOPT = { dnskey_mgmt, DNSKEY_MGMT_FULL },
	                                   CONF_IO_FRLD_ZONES },
	{ C_OFFLINE_KSK,         YP_TBOOL, YP_VNONE, CONF_IO_FRLD_ZONES },
	{ C_UNSAFE_OPERATION,    YP_TOPT,  YP_VOPT = { unsafe_operation, UNSAFE_NONE }, YP_FMULTI },
	{ C_COMMENT,             YP_TSTR,  YP_VNONE },
	{ NULL }
};

#define ZONE_ITEMS(FLAGS) \
	{ C_STORAGE,             YP_TSTR,  YP_VSTR = { STORAGE_DIR }, FLAGS }, \
	{ C_FILE,                YP_TSTR,  YP_VNONE, FLAGS }, \
	{ C_MASTER,              YP_TREF,  YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI | CONF_REF_EMPTY, \
	                                   { check_ref } }, \
	{ C_DDNS_MASTER,         YP_TREF,  YP_VREF = { C_RMT }, YP_FNONE, { check_ref_empty } }, \
	{ C_NOTIFY,              YP_TREF,  YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI | CONF_REF_EMPTY, \
	                                   { check_ref } }, \
	{ C_NOTIFY_DELAY,        YP_TINT,  YP_VINT  = { -1, UINT32_MAX, 0, YP_STIME } }, \
	{ C_ACL,                 YP_TREF,  YP_VREF = { C_ACL }, YP_FMULTI, { check_ref } }, \
	{ C_MASTER_PIN_TOL,      YP_TINT,  YP_VINT = { 0, UINT32_MAX, 0, YP_STIME } }, \
	{ C_PROVIDE_IXFR,        YP_TBOOL, YP_VBOOL = { true } }, \
	{ C_SEM_CHECKS,          YP_TOPT,  YP_VOPT = { semantic_checks, SEMCHECKS_OFF }, FLAGS }, \
	{ C_DEFAULT_TTL,         YP_TINT,  YP_VINT = { 1, INT32_MAX, DEFAULT_TTL, YP_STIME }, FLAGS }, \
	{ C_ZONEFILE_SYNC,       YP_TINT,  YP_VINT = { -1, INT32_MAX, 0, YP_STIME } }, \
	{ C_ZONEFILE_LOAD,       YP_TOPT,  YP_VOPT = { zonefile_load, ZONEFILE_LOAD_WHOLE } }, \
	{ C_JOURNAL_CONTENT,     YP_TOPT,  YP_VOPT = { journal_content, JOURNAL_CONTENT_CHANGES }, FLAGS }, \
	{ C_JOURNAL_MAX_USAGE,   YP_TINT,  YP_VINT = { KILO(40), SSIZE_MAX, MEGA(100), YP_SSIZE } }, \
	{ C_JOURNAL_MAX_DEPTH,   YP_TINT,  YP_VINT = { 2, SSIZE_MAX, 20 } }, \
	{ C_IXFR_BENEVOLENT,     YP_TBOOL, YP_VNONE }, \
	{ C_IXFR_BY_ONE,         YP_TBOOL, YP_VNONE }, \
	{ C_IXFR_FROM_AXFR,      YP_TBOOL, YP_VNONE }, \
	{ C_ZONE_MAX_SIZE,       YP_TINT,  YP_VINT = { 0, SSIZE_MAX, SSIZE_MAX, YP_SSIZE }, FLAGS }, \
	{ C_ADJUST_THR,          YP_TINT,  YP_VINT = { 1, UINT16_MAX, 1 } }, \
	{ C_DNSSEC_SIGNING,      YP_TBOOL, YP_VNONE, FLAGS }, \
	{ C_DNSSEC_VALIDATION,   YP_TBOOL, YP_VNONE, FLAGS }, \
	{ C_DNSSEC_POLICY,       YP_TREF,  YP_VREF = { C_POLICY }, FLAGS, { check_ref_dflt } }, \
	{ C_DS_PUSH,             YP_TREF,  YP_VREF = { C_RMT, C_RMTS }, YP_FMULTI | CONF_REF_EMPTY | FLAGS, \
	                                   { check_ref } }, \
	{ C_REVERSE_GEN,         YP_TDNAME,YP_VNONE, YP_FMULTI | FLAGS | CONF_IO_FRLD_ZONES }, \
	{ C_SERIAL_POLICY,       YP_TOPT,  YP_VOPT = { serial_policies, SERIAL_POLICY_INCREMENT } }, \
	{ C_SERIAL_MODULO,       YP_TSTR,  YP_VSTR = { "0/1" }, YP_FNONE, { check_modulo_shift } }, \
	{ C_ZONEMD_GENERATE,     YP_TOPT,  YP_VOPT = { zone_digest, ZONE_DIGEST_NONE }, FLAGS }, \
	{ C_ZONEMD_VERIFY,       YP_TBOOL, YP_VNONE, FLAGS }, \
	{ C_REFRESH_MIN_INTERVAL,YP_TINT,  YP_VINT = { 2, UINT32_MAX, 2, YP_STIME } }, \
	{ C_REFRESH_MAX_INTERVAL,YP_TINT,  YP_VINT = { 2, UINT32_MAX, UINT32_MAX, YP_STIME } }, \
	{ C_RETRY_MIN_INTERVAL,  YP_TINT,  YP_VINT = { 1, UINT32_MAX, 1, YP_STIME } }, \
	{ C_RETRY_MAX_INTERVAL,  YP_TINT,  YP_VINT = { 1, UINT32_MAX, UINT32_MAX, YP_STIME } }, \
	{ C_EXPIRE_MIN_INTERVAL, YP_TINT,  YP_VINT = { 3, UINT32_MAX, 3, YP_STIME } }, \
	{ C_EXPIRE_MAX_INTERVAL, YP_TINT,  YP_VINT = { 3, UINT32_MAX, UINT32_MAX, YP_STIME } }, \
	{ C_CATALOG_ROLE,        YP_TOPT,  YP_VOPT = { catalog_roles, CATALOG_ROLE_NONE }, FLAGS }, \
	{ C_CATALOG_TPL,         YP_TREF,  YP_VREF = { C_TPL }, YP_FMULTI | FLAGS, { check_ref, check_catalog_tpl } }, \
	{ C_CATALOG_ZONE,        YP_TDNAME,YP_VNONE, FLAGS | CONF_IO_FRLD_ZONES }, \
	{ C_CATALOG_GROUP,       YP_TSTR,  YP_VNONE, FLAGS | CONF_IO_FRLD_ZONES, { check_catalog_group } }, \
	{ C_MODULE,              YP_TDATA, YP_VDATA = { 0, NULL, mod_id_to_bin, mod_id_to_txt }, \
	                                   YP_FMULTI | FLAGS, { check_modref } }, \
	{ C_COMMENT,             YP_TSTR,  YP_VNONE }, \

static const yp_item_t desc_template[] = {
	{ C_ID,                  YP_TSTR,  YP_VNONE, CONF_IO_FREF },
	{ C_GLOBAL_MODULE,       YP_TDATA, YP_VDATA = { 0, NULL, mod_id_to_bin, mod_id_to_txt },
	                                   YP_FMULTI | CONF_IO_FRLD_MOD, { check_modref } },
	ZONE_ITEMS(CONF_IO_FRLD_ZONES)
	{ NULL }
};

static const yp_item_t desc_zone[] = {
	{ C_DOMAIN, YP_TDNAME, YP_VNONE, CONF_IO_FRLD_ZONE },
	{ C_TPL,    YP_TREF,   YP_VREF = { C_TPL }, CONF_IO_FRLD_ZONE, { check_ref } },
	ZONE_ITEMS(CONF_IO_FRLD_ZONE)
	{ NULL }
};

const yp_item_t conf_schema[] = {
	{ C_MODULE,   YP_TGRP, YP_VGRP = { desc_module }, YP_FMULTI | CONF_IO_FRLD_ALL |
	                                                  CONF_IO_FCHECK_ZONES, { load_module } },
	{ C_SRV,      YP_TGRP, YP_VGRP = { desc_server }, CONF_IO_FRLD_SRV, { check_server } },
	{ C_XDP,      YP_TGRP, YP_VGRP = { desc_xdp }, CONF_IO_FRLD_SRV, { check_xdp } },
	{ C_CTL,      YP_TGRP, YP_VGRP = { desc_control } },
	{ C_LOG,      YP_TGRP, YP_VGRP = { desc_log }, YP_FMULTI | CONF_IO_FRLD_LOG },
	{ C_STATS,    YP_TGRP, YP_VGRP = { desc_stats }, CONF_IO_FRLD_SRV },
	{ C_DB,       YP_TGRP, YP_VGRP = { desc_database }, CONF_IO_FRLD_SRV, { check_database } },
	{ C_KEYSTORE, YP_TGRP, YP_VGRP = { desc_keystore }, YP_FMULTI, { check_keystore } },
	{ C_KEY,      YP_TGRP, YP_VGRP = { desc_key }, YP_FMULTI, { check_key } },
	{ C_RMT,      YP_TGRP, YP_VGRP = { desc_remote }, YP_FMULTI, { check_remote } },
	{ C_RMTS,     YP_TGRP, YP_VGRP = { desc_remotes }, YP_FMULTI, { check_remotes } },
	{ C_ACL,      YP_TGRP, YP_VGRP = { desc_acl }, YP_FMULTI, { check_acl } },
	{ C_SBM,      YP_TGRP, YP_VGRP = { desc_submission }, YP_FMULTI },
	{ C_DNSKEY_SYNC, YP_TGRP, YP_VGRP = { desc_dnskey_sync }, YP_FMULTI, { check_dnskey_sync } },
	{ C_POLICY,   YP_TGRP, YP_VGRP = { desc_policy }, YP_FMULTI, { check_policy } },
	{ C_TPL,      YP_TGRP, YP_VGRP = { desc_template }, YP_FMULTI, { check_template } },
	{ C_ZONE,     YP_TGRP, YP_VGRP = { desc_zone }, YP_FMULTI | CONF_IO_FZONE, { check_zone } },
	{ C_INCL,     YP_TSTR, YP_VNONE, CONF_IO_FDIFF_ZONES | CONF_IO_FRLD_ALL, { include_file } },
	{ C_CLEAR,    YP_TSTR, YP_VNONE, CONF_IO_FDIFF_ZONES | CONF_IO_FRLD_ALL, { clear_conf } },
	{ NULL }
};
