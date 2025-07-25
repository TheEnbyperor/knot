libknotd_la_CPPFLAGS = $(AM_CPPFLAGS) $(CFLAG_VISIBILITY) $(libkqueue_CFLAGS) \
                       $(liburcu_CFLAGS) $(lmdb_CFLAGS) $(systemd_CFLAGS) \
                       $(libdbus_CFLAGS) $(gnutls_CFLAGS) -DKNOTD_MOD_STATIC
libknotd_la_LDFLAGS  = $(AM_LDFLAGS) -export-symbols-regex '^knotd_'
libknotd_la_LIBADD   = $(dlopen_LIBS) $(libkqueue_LIBS) $(pthread_LIBS)
libknotd_LIBS        = libknotd.la libknot.la libdnssec.la libzscanner.la \
                       $(libcontrib_LIBS) $(liburcu_LIBS) $(lmdb_LIBS) \
                       $(systemd_LIBS) $(libdbus_LIBS) $(gnutls_LIBS)

if EMBEDDED_LIBNGTCP2
libknotd_la_LIBADD += $(libembngtcp2_LIBS)
endif EMBEDDED_LIBNGTCP2

include_libknotddir = $(includedir)/knot
include_libknotd_HEADERS = \
	knot/include/module.h

libknotd_la_SOURCES = \
	knot/catalog/catalog_db.c		\
	knot/catalog/catalog_db.h		\
	knot/catalog/catalog_update.c		\
	knot/catalog/catalog_update.h		\
	knot/catalog/generate.c			\
	knot/catalog/generate.h			\
	knot/catalog/interpret.c		\
	knot/catalog/interpret.h		\
	knot/conf/base.c			\
	knot/conf/base.h			\
	knot/conf/conf.c			\
	knot/conf/conf.h			\
	knot/conf/confdb.c			\
	knot/conf/confdb.h			\
	knot/conf/confio.c			\
	knot/conf/confio.h			\
	knot/conf/migration.c			\
	knot/conf/migration.h			\
	knot/conf/module.h			\
	knot/conf/module.c			\
	knot/conf/schema.c			\
	knot/conf/schema.h			\
	knot/conf/tools.c			\
	knot/conf/tools.h			\
	knot/ctl/commands.c			\
	knot/ctl/commands.h			\
	knot/ctl/process.c			\
	knot/ctl/process.h			\
	knot/dnssec/context.c			\
	knot/dnssec/context.h			\
	knot/dnssec/ds_query.c			\
	knot/dnssec/ds_query.h			\
	knot/dnssec/kasp/kasp_db.c		\
	knot/dnssec/kasp/kasp_db.h		\
	knot/dnssec/kasp/kasp_zone.c		\
	knot/dnssec/kasp/kasp_zone.h		\
	knot/dnssec/kasp/keystate.c		\
	knot/dnssec/kasp/keystate.h		\
	knot/dnssec/kasp/keystore.c		\
	knot/dnssec/kasp/keystore.h		\
	knot/dnssec/kasp/policy.h		\
	knot/dnssec/key-events.c		\
	knot/dnssec/key-events.h		\
	knot/dnssec/key_records.c		\
	knot/dnssec/key_records.h		\
	knot/dnssec/nsec-chain.c		\
	knot/dnssec/nsec-chain.h		\
	knot/dnssec/nsec3-chain.c		\
	knot/dnssec/nsec3-chain.h		\
	knot/dnssec/policy.c			\
	knot/dnssec/policy.h			\
	knot/dnssec/rrset-sign.c		\
	knot/dnssec/rrset-sign.h		\
	knot/dnssec/zone-events.c		\
	knot/dnssec/zone-events.h		\
	knot/dnssec/zone-keys.c			\
	knot/dnssec/zone-keys.h			\
	knot/dnssec/zone-nsec.c			\
	knot/dnssec/zone-nsec.h			\
	knot/dnssec/zone-sign.c			\
	knot/dnssec/zone-sign.h			\
	knot/events/events.c			\
	knot/events/events.h			\
	knot/events/handlers.h			\
	knot/events/handlers/backup.c		\
	knot/events/handlers/dnskey_sync.c	\
	knot/events/handlers/dnssec.c		\
	knot/events/handlers/ds_check.c		\
	knot/events/handlers/ds_push.c		\
	knot/events/handlers/expire.c		\
	knot/events/handlers/flush.c		\
	knot/events/handlers/freeze_thaw.c	\
	knot/events/handlers/load.c		\
	knot/events/handlers/notify.c		\
	knot/events/handlers/refresh.c		\
	knot/events/handlers/update.c		\
	knot/events/handlers/validate.c		\
	knot/events/replan.c			\
	knot/events/replan.h			\
	knot/nameserver/axfr.c			\
	knot/nameserver/axfr.h			\
	knot/nameserver/chaos.c			\
	knot/nameserver/chaos.h			\
	knot/nameserver/internet.c		\
	knot/nameserver/internet.h		\
	knot/nameserver/ixfr.c			\
	knot/nameserver/ixfr.h			\
	knot/nameserver/log.h			\
	knot/nameserver/notify.c		\
	knot/nameserver/notify.h		\
	knot/nameserver/nsec_proofs.c		\
	knot/nameserver/nsec_proofs.h		\
	knot/nameserver/process_query.c		\
	knot/nameserver/process_query.h		\
	knot/nameserver/query_module.c		\
	knot/nameserver/query_module.h		\
	knot/nameserver/tsig_ctx.c		\
	knot/nameserver/tsig_ctx.h		\
	knot/nameserver/update.c		\
	knot/nameserver/update.h		\
	knot/nameserver/xfr.c			\
	knot/nameserver/xfr.h			\
	knot/query/capture.c			\
	knot/query/capture.h			\
	knot/query/layer.h			\
	knot/query/query.c			\
	knot/query/query.h			\
	knot/query/requestor.c			\
	knot/query/requestor.h			\
	knot/query/tls-requestor.c		\
	knot/query/tls-requestor.h		\
	knot/common/dbus.c			\
	knot/common/dbus.h			\
	knot/common/evsched.c			\
	knot/common/evsched.h			\
	knot/common/fdset.c			\
	knot/common/fdset.h			\
	knot/common/log.c			\
	knot/common/log.h			\
	knot/common/process.c			\
	knot/common/process.h			\
	knot/common/stats.c			\
	knot/common/stats.h			\
	knot/common/systemd.c			\
	knot/common/systemd.h			\
	knot/common/unreachable.c		\
	knot/common/unreachable.h		\
	knot/journal/journal_basic.c		\
	knot/journal/journal_basic.h		\
	knot/journal/journal_metadata.c		\
	knot/journal/journal_metadata.h		\
	knot/journal/journal_read.c		\
	knot/journal/journal_read.h		\
	knot/journal/journal_write.c		\
	knot/journal/journal_write.h		\
	knot/journal/knot_lmdb.c		\
	knot/journal/knot_lmdb.h		\
	knot/journal/serialization.c		\
	knot/journal/serialization.h		\
	knot/server/dthreads.c			\
	knot/server/dthreads.h			\
	knot/server/handler.c			\
	knot/server/handler.h			\
	knot/server/proxyv2.c			\
	knot/server/proxyv2.h			\
	knot/server/server.c			\
	knot/server/server.h			\
	knot/server/tcp-handler.c		\
	knot/server/tcp-handler.h		\
	knot/server/udp-handler.c		\
	knot/server/udp-handler.h		\
	knot/server/xdp-handler.c		\
	knot/server/xdp-handler.h		\
	knot/updates/acl.c			\
	knot/updates/acl.h			\
	knot/updates/apply.c			\
	knot/updates/apply.h			\
	knot/updates/changesets.c		\
	knot/updates/changesets.h		\
	knot/updates/ddns.c			\
	knot/updates/ddns.h			\
	knot/updates/zone-update.c		\
	knot/updates/zone-update.h		\
	knot/worker/pool.c			\
	knot/worker/pool.h			\
	knot/worker/queue.c			\
	knot/worker/queue.h			\
	knot/zone/adds_tree.c			\
	knot/zone/adds_tree.h			\
	knot/zone/adjust.c			\
	knot/zone/adjust.h			\
	knot/zone/backup.c			\
	knot/zone/backup.h			\
	knot/zone/backup_dir.c			\
	knot/zone/backup_dir.h			\
	knot/zone/contents.c			\
	knot/zone/contents.h			\
	knot/zone/digest.c			\
	knot/zone/digest.h			\
	knot/zone/measure.h			\
	knot/zone/measure.c			\
	knot/zone/node.c			\
	knot/zone/node.h			\
	knot/zone/reverse.c			\
	knot/zone/reverse.h			\
	knot/zone/semantic-check.c		\
	knot/zone/semantic-check.h		\
	knot/zone/serial.c			\
	knot/zone/serial.h			\
	knot/zone/timers.c			\
	knot/zone/timers.h			\
	knot/zone/zone-diff.c			\
	knot/zone/zone-diff.h			\
	knot/zone/zone-dump.c			\
	knot/zone/zone-dump.h			\
	knot/zone/zone-load.c			\
	knot/zone/zone-load.h			\
	knot/zone/zone-tree.c			\
	knot/zone/zone-tree.h			\
	knot/zone/zone.c			\
	knot/zone/zone.h			\
	knot/zone/zonedb-load.c			\
	knot/zone/zonedb-load.h			\
	knot/zone/zonedb.c			\
	knot/zone/zonedb.h			\
	knot/zone/zonefile.c			\
	knot/zone/zonefile.h

if ENABLE_QUIC
libknotd_la_SOURCES += \
	knot/query/quic-requestor.c		\
	knot/query/quic-requestor.h		\
	knot/server/quic-handler.c		\
	knot/server/quic-handler.h
endif ENABLE_QUIC

if HAVE_DAEMON
noinst_LTLIBRARIES += libknotd.la
pkgconfig_DATA     += knotd.pc
endif HAVE_DAEMON

KNOTD_MOD_CPPFLAGS = $(AM_CPPFLAGS) $(CFLAG_VISIBILITY)
KNOTD_MOD_LDFLAGS  = $(AM_LDFLAGS) -module -shared -avoid-version

pkglibdir = $(module_instdir)
pkglib_LTLIBRARIES =

include $(srcdir)/knot/modules/authsignal/Makefile.inc
include $(srcdir)/knot/modules/cookies/Makefile.inc
include $(srcdir)/knot/modules/dnsproxy/Makefile.inc
include $(srcdir)/knot/modules/dnstap/Makefile.inc
include $(srcdir)/knot/modules/geoip/Makefile.inc
include $(srcdir)/knot/modules/noudp/Makefile.inc
include $(srcdir)/knot/modules/onlinesign/Makefile.inc
include $(srcdir)/knot/modules/probe/Makefile.inc
include $(srcdir)/knot/modules/queryacl/Makefile.inc
include $(srcdir)/knot/modules/rrl/Makefile.inc
include $(srcdir)/knot/modules/stats/Makefile.inc
include $(srcdir)/knot/modules/synthrecord/Makefile.inc
include $(srcdir)/knot/modules/whoami/Makefile.inc
include $(srcdir)/knot/modules/updateproxy/Makefile.inc
include $(srcdir)/knot/modules/queryignoretsig/Makefile.inc
