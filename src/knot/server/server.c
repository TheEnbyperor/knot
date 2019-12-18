/*  Copyright (C) 2020 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#define __APPLE_USE_RFC_3542

#include <stdlib.h>
#include <assert.h>
#include <netinet/tcp.h>
#include <sys/resource.h>

#include "libknot/errcode.h"
#ifdef ENABLE_XDP
#include "libknot/xdp/af_xdp.h"
#endif
#include "libknot/yparser/ypschema.h"
#include "knot/common/log.h"
#include "knot/common/stats.h"
#include "knot/conf/confio.h"
#include "knot/conf/migration.h"
#include "knot/conf/module.h"
#include "knot/dnssec/kasp/kasp_db.h"
#include "knot/journal/journal_basic.h"
#include "knot/server/server.h"
#include "knot/server/udp-handler.h"
#include "knot/server/tcp-handler.h"
#include "knot/zone/timers.h"
#include "knot/zone/zonedb-load.h"
#include "knot/worker/pool.h"
#include "contrib/net.h"
#include "contrib/sockaddr.h"
#include "contrib/trim.h"

/*! \brief Minimal send/receive buffer sizes. */
enum {
	UDP_MIN_RCVSIZE = 4096,
	UDP_MIN_SNDSIZE = 4096,
	TCP_MIN_RCVSIZE = 4096,
	TCP_MIN_SNDSIZE = sizeof(uint16_t) + UINT16_MAX
};

/*! \brief Unbind interface and clear the structure. */
static void server_deinit_iface(iface_t *iface)
{
	assert(iface);

	/* Free UDP handler. */
	if (iface->fd_udp != NULL) {
		for (int i = 0; i < iface->fd_udp_count; i++) {
			if (iface->fd_udp[i] > -1) {
				close(iface->fd_udp[i]);
			}
		}
		free(iface->fd_udp);
	}

	for (int i = 0; i < iface->fd_xdp_count; i++) {
#ifdef ENABLE_XDP
		knot_xsk_deinit(iface->sock_xdp[i]);
#else
		assert(0);
#endif
	}
	free(iface->fd_xdp);
	free(iface->sock_xdp);

	/* Free TCP handler. */
	if (iface->fd_tcp != NULL) {
		for (int i = 0; i < iface->fd_tcp_count; i++) {
			if (iface->fd_tcp[i] > -1) {
				close(iface->fd_tcp[i]);
			}
		}
		free(iface->fd_tcp);
	}
}

/*! \brief Deinit server interface list. */
static void server_deinit_iface_list(iface_t *ifaces, size_t n)
{
	if (ifaces != NULL) {
		for (size_t i = 0; i < n; i++) {
			server_deinit_iface(ifaces + i);
		}
		free(ifaces);
	}
}

/*! \brief Set lower bound for socket option. */
static bool setsockopt_min(int sock, int option, int min)
{
	int value = 0;
	socklen_t len = sizeof(value);

	if (getsockopt(sock, SOL_SOCKET, option, &value, &len) != 0) {
		return false;
	}

	assert(len == sizeof(value));
	if (value >= min) {
		return true;
	}

	return setsockopt(sock, SOL_SOCKET, option, &min, sizeof(min)) == 0;
}

/*!
 * \brief Enlarge send/receive buffers.
 */
static bool enlarge_net_buffers(int sock, int min_recvsize, int min_sndsize)
{
	return setsockopt_min(sock, SO_RCVBUF, min_recvsize) &&
	       setsockopt_min(sock, SO_SNDBUF, min_sndsize);
}

/*!
 * \brief Enable source packet information retrieval.
 */
static bool enable_pktinfo(int sock, int family)
{
	int level = 0;
	int option = 0;

	switch (family) {
	case AF_INET:
		level = IPPROTO_IP;
#if defined(IP_PKTINFO)
		option = IP_PKTINFO; /* Linux */
#elif defined(IP_RECVDSTADDR)
		option = IP_RECVDSTADDR; /* BSD */
#else
		return false;
#endif
		break;
	case AF_INET6:
		level = IPPROTO_IPV6;
		option = IPV6_RECVPKTINFO;
		break;
	default:
		return false;
	}

	const int on = 1;
	return setsockopt(sock, level, option, &on, sizeof(on)) == 0;
}

/*!
 * Linux 3.15 has IP_PMTUDISC_OMIT which makes sockets
 * ignore PMTU information and send packets with DF=0.
 * Fragmentation is allowed if and only if the packet
 * size exceeds the outgoing interface MTU or the packet
 * encounters smaller MTU link in network.
 * This mitigates DNS fragmentation attacks by preventing
 * forged PMTU information.
 * FreeBSD already has same semantics without setting
 * the option.
 */
static int disable_pmtudisc(int sock, int family)
{
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_OMIT)
	if (family == AF_INET) {
		int action_omit = IP_PMTUDISC_OMIT;
		if (setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, &action_omit,
		    sizeof(action_omit)) != 0) {
			return knot_map_errno();
		}
	}
#endif
	return KNOT_EOK;
}

/*!
 * \brief Enable TCP Fast Open.
 */
static int enable_fastopen(int sock, int backlog)
{
#if defined(TCP_FASTOPEN)
#if __APPLE__
	if (backlog > 0) {
		backlog = 1; // just on-off switch on macOS
	}
#endif
	if (setsockopt(sock, IPPROTO_TCP, TCP_FASTOPEN, &backlog, sizeof(backlog)) != 0) {
		return knot_map_errno();
	}
#endif
	return KNOT_EOK;
}

/*!
 * \brief Create and initialize new interface.
 *
 * Both TCP and UDP sockets will be created for the interface.
 *
 * \param addr              Socket address.
 * \param udp_thread_count  Number of created UDP workers.
 * \param tcp_thread_count  Number of created TCP workers.
 * \param tcp_reuseport     Indication if reuseport on TCP is enabled.
 *
 * \retval Pointer to a new initialized inteface.
 * \retval NULL if error.
 */
static iface_t *server_init_iface(struct sockaddr_storage *addr,
                                  int udp_thread_count, int tcp_thread_count,
                                  int xdp_thread_count, bool tcp_reuseport)
{
	iface_t *new_if = calloc(1, sizeof(*new_if));
	if (new_if == NULL) {
		log_error("failed to initialize interface");
		return NULL;
	}
	memcpy(&new_if->addr, addr, sizeof(*addr));

	/* Convert to string address format. */
	char addr_str[SOCKADDR_STRLEN] = { 0 };
	sockaddr_tostr(addr_str, sizeof(addr_str), addr);

	int udp_socket_count = 1;
	int udp_bind_flags = 0;
	int tcp_socket_count = 1;
	int tcp_bind_flags = 0;
	int xdp_socket_count = 0;

#ifdef ENABLE_REUSEPORT
	udp_socket_count = udp_thread_count;
	udp_bind_flags |= NET_BIND_MULTIPLE;

	if (tcp_reuseport) {
		tcp_socket_count = tcp_thread_count;
		tcp_bind_flags |= NET_BIND_MULTIPLE;
	}
#endif

#ifdef ENABLE_XDP
	xdp_socket_count = xdp_thread_count;
#endif

	new_if->fd_udp = malloc(udp_socket_count * sizeof(int));
	new_if->fd_tcp = malloc(tcp_socket_count * sizeof(int));
	new_if->fd_xdp = malloc(xdp_socket_count * sizeof(int));
	new_if->sock_xdp = calloc(xdp_socket_count, sizeof(*new_if->sock_xdp));
	if (new_if->fd_udp == NULL || new_if->fd_tcp == NULL ||
	    new_if->fd_xdp == NULL || new_if->sock_xdp == NULL) {
		log_error("failed to initialize interface");
		server_deinit_iface(new_if);
		return NULL;
	}

	bool warn_bind = true;
	bool warn_bufsize = true;
	bool warn_pktinfo = true;
	bool warn_flag_misc = true;

	/* Create bound UDP sockets. */
	for (int i = 0; i < udp_socket_count; i++) {
		int sock = net_bound_socket(SOCK_DGRAM, addr, udp_bind_flags);
		if (sock == KNOT_EADDRNOTAVAIL) {
			udp_bind_flags |= NET_BIND_NONLOCAL;
			sock = net_bound_socket(SOCK_DGRAM, addr, udp_bind_flags);
			if (sock >= 0 && warn_bind) {
				log_warning("address %s UDP bound, but required nonlocal bind", addr_str);
				warn_bind = false;
			}
		}

		if (sock < 0) {
			log_error("cannot bind address %s UDP (%s)", addr_str,
			          knot_strerror(sock));
			server_deinit_iface(new_if);
			return NULL;
		}

		if (!enlarge_net_buffers(sock, UDP_MIN_RCVSIZE, UDP_MIN_SNDSIZE) &&
		    warn_bufsize) {
			log_warning("failed to set network buffer sizes for UDP");
			warn_bufsize = false;
		}

		if (sockaddr_is_any(addr) && !enable_pktinfo(sock, addr->ss_family) &&
		    warn_pktinfo) {
			log_warning("failed to enable received packet information retrieval");
			warn_pktinfo = false;
		}

		int ret = disable_pmtudisc(sock, addr->ss_family);
		if (ret != KNOT_EOK && warn_flag_misc) {
			log_warning("failed to disable Path MTU discovery for IPv4/UDP (%s)",
			            knot_strerror(ret));
			warn_flag_misc = false;
		}

		new_if->fd_udp[new_if->fd_udp_count] = sock;
		new_if->fd_udp_count += 1;
	}

	for (int i = 0; i < xdp_socket_count; i++) {
#ifndef ENABLE_XDP
		assert(0);
#else
		int ret = knot_xsk_init(new_if->sock_xdp + i, "enp1s0f1", i, "/bpf-kernel.o"); // FIXME
		if (ret != KNOT_EOK) {
			log_warning("failed to init XDP (%s)", knot_strerror(ret));
		} else {
			new_if->fd_xdp[i] = knot_xsk_get_poll_fd(new_if->sock_xdp[i]);
		}
#endif
	}


	warn_bind = true;
	warn_bufsize = true;
	warn_flag_misc = true;

	/* Create bound TCP sockets. */
	for (int i = 0; i < tcp_socket_count; i++) {
		int sock = net_bound_socket(SOCK_STREAM, addr, tcp_bind_flags);
		if (sock == KNOT_EADDRNOTAVAIL) {
			tcp_bind_flags |= NET_BIND_NONLOCAL;
			sock = net_bound_socket(SOCK_STREAM, addr, tcp_bind_flags);
			if (sock >= 0 && warn_bind) {
				log_warning("address %s TCP bound, but required nonlocal bind", addr_str);
				warn_bind = false;
			}
		}

		if (sock < 0) {
			log_error("cannot bind address %s TCP (%s)", addr_str,
			          knot_strerror(sock));
			server_deinit_iface(new_if);
			return NULL;
		}

		if (!enlarge_net_buffers(sock, TCP_MIN_RCVSIZE, TCP_MIN_SNDSIZE) &&
		    warn_bufsize) {
			log_warning("failed to set network buffer sizes for TCP");
			warn_bufsize = false;
		}

		new_if->fd_tcp[new_if->fd_tcp_count] = sock;
		new_if->fd_tcp_count += 1;

		/* Listen for incoming connections. */
		int ret = listen(sock, TCP_BACKLOG_SIZE);
		if (ret < 0) {
			log_error("failed to listen on TCP interface %s", addr_str);
			server_deinit_iface(new_if);
			return NULL;
		}

		/* TCP Fast Open. */
		ret = enable_fastopen(sock, TCP_BACKLOG_SIZE);
		if (ret < 0 && warn_flag_misc) {
			log_warning("failed to enable TCP Fast Open on %s (%s)",
			            addr_str, knot_strerror(ret));
			warn_flag_misc = false;
		}
	}

	return new_if;
}

/*! \brief Initialize bound sockets according to configuration. */
static int configure_sockets(conf_t *conf, server_t *s)
{
	if (s->state & ServerRunning) {
		return KNOT_EOK;
	}

#ifdef ENABLE_REUSEPORT
	/* Log info if reuseport is used and for what protocols. */
	log_info("using reuseport for UDP%s", conf->cache.srv_tcp_reuseport ? " and TCP" : "");
#endif

	if (conf->cache.srv_xdp_threads > 0) {
		struct rlimit no_limit = { RLIM_INFINITY, RLIM_INFINITY };
		int ret = setrlimit(RLIMIT_MEMLOCK, &no_limit);
		if (ret) {
			return -errno;
		}
	}

	/* Update bound interfaces. */
	conf_val_t listen_val = conf_get(conf, C_SRV, C_LISTEN);
	conf_val_t rundir_val = conf_get(conf, C_SRV, C_RUNDIR);

	size_t nifs = conf_val_count(&listen_val), real_n = 0;
	iface_t *newlist = calloc(nifs, sizeof(*newlist));
	if (newlist == NULL) {
		return KNOT_ENOMEM;
	}

	char *rundir = conf_abs_path(&rundir_val, NULL);
	while (listen_val.code == KNOT_EOK) {
		/* Log interface binding. */
		struct sockaddr_storage addr = conf_addr(&listen_val, rundir);
		char addr_str[SOCKADDR_STRLEN] = { 0 };
		sockaddr_tostr(addr_str, sizeof(addr_str), &addr);
		log_info("binding to interface %s", addr_str);

		/* Create new interface. */
		unsigned size_udp = s->handlers[IO_UDP].handler.unit->size;
		unsigned size_tcp = s->handlers[IO_TCP].handler.unit->size;
		bool tcp_reuseport = conf->cache.srv_tcp_reuseport;
		iface_t *new_if = server_init_iface(&addr, size_udp, size_tcp, conf->cache.srv_xdp_threads, tcp_reuseport);
		if (new_if != NULL) {
			memcpy(&newlist[real_n++], new_if, sizeof(*newlist));
			free(new_if);
		}

		conf_val_next(&listen_val);
	}
	assert(real_n <= nifs);
	nifs = real_n;
	free(rundir);

	/* Publish new list. */
	s->ifaces = newlist;
	s->n_ifaces = nifs;

	/* Set the ID's (thread_id) of both the TCP and UDP threads. */
	unsigned thread_count = 0;
	for (unsigned proto = IO_UDP; proto <= IO_XDP; ++proto) {
		dt_unit_t *tu = s->handlers[proto].handler.unit;
		for (unsigned i = 0; tu != NULL && i < tu->size; ++i) {
			s->handlers[proto].handler.thread_id[i] = thread_count++;
		}
	}

	return KNOT_EOK;
}

int server_init(server_t *server, int bg_workers)
{
	if (server == NULL) {
		return KNOT_EINVAL;
	}

	/* Clear the structure. */
	memset(server, 0, sizeof(server_t));

	/* Initialize event scheduler. */
	if (evsched_init(&server->sched, server) != KNOT_EOK) {
		return KNOT_ENOMEM;
	}

	server->workers = worker_pool_create(bg_workers);
	if (server->workers == NULL) {
		evsched_deinit(&server->sched);
		return KNOT_ENOMEM;
	}

	char *journal_dir = conf_db(conf(), C_JOURNAL_DB);
	conf_val_t journal_size = conf_db_param(conf(), C_JOURNAL_DB_MAX_SIZE, C_MAX_JOURNAL_DB_SIZE);
	conf_val_t journal_mode = conf_db_param(conf(), C_JOURNAL_DB_MODE, C_JOURNAL_DB_MODE);
	knot_lmdb_init(&server->journaldb, journal_dir, conf_int(&journal_size), journal_env_flags(conf_opt(&journal_mode)), NULL);
	free(journal_dir);

	kasp_db_ensure_init(&server->kaspdb, conf());

	char *timer_dir = conf_db(conf(), C_TIMER_DB);
	conf_val_t timer_size = conf_db_param(conf(), C_TIMER_DB_MAX_SIZE, C_MAX_TIMER_DB_SIZE);
	knot_lmdb_init(&server->timerdb, timer_dir, conf_int(&timer_size), 0, NULL);
	free(timer_dir);

	return KNOT_EOK;
}

void server_deinit(server_t *server)
{
	if (server == NULL) {
		return;
	}

	/* Save zone timers. */
	if (server->zone_db != NULL) {
		log_info("updating persistent timer DB");
		int ret = zone_timers_write_all(&server->timerdb, server->zone_db);
		if (ret != KNOT_EOK) {
			log_warning("failed to update persistent timer DB (%s)",
				    knot_strerror(ret));
		}
	}

	/* Free remaining interfaces. */
	server_deinit_iface_list(server->ifaces, server->n_ifaces);

	/* Free threads and event handlers. */
	worker_pool_destroy(server->workers);

	/* Free zone database. */
	knot_zonedb_deep_free(&server->zone_db, true);

	/* Free remaining events. */
	evsched_deinit(&server->sched);

	/* Close persistent timers DB. */
	knot_lmdb_deinit(&server->timerdb);

	/* Close kasp_db. */
	knot_lmdb_deinit(&server->kaspdb);

	/* Close journal database if open. */
	knot_lmdb_deinit(&server->journaldb);
}

static int server_init_handler(server_t *server, int index, int thread_count,
                               runnable_t runnable, runnable_t destructor)
{
	/* Initialize */
	iohandler_t *h = &server->handlers[index].handler;
	memset(h, 0, sizeof(iohandler_t));
	h->server = server;
	h->unit = dt_create(thread_count, runnable, destructor, h);
	if (h->unit == NULL) {
		return KNOT_ENOMEM;
	}

	h->thread_state = calloc(thread_count, sizeof(unsigned));
	if (h->thread_state == NULL) {
		dt_delete(&h->unit);
		return KNOT_ENOMEM;
	}

	h->thread_id = calloc(thread_count, sizeof(unsigned));
	if (h->thread_id == NULL) {
		free(h->thread_state);
		dt_delete(&h->unit);
		return KNOT_ENOMEM;
	}

	return KNOT_EOK;
}

static void server_free_handler(iohandler_t *h)
{
	if (h == NULL || h->server == NULL) {
		return;
	}

	/* Wait for threads to finish */
	if (h->unit) {
		dt_stop(h->unit);
		dt_join(h->unit);
	}

	/* Destroy worker context. */
	dt_delete(&h->unit);
	free(h->thread_state);
	free(h->thread_id);
}

int server_start(server_t *server, bool async)
{
	if (server == NULL) {
		return KNOT_EINVAL;
	}

	/* Start workers. */
	worker_pool_start(server->workers);

	/* Wait for enqueued events if not asynchronous. */
	if (!async) {
		worker_pool_wait(server->workers);
	}

	/* Start evsched handler. */
	evsched_start(&server->sched);

	/* Start I/O handlers. */
	server->state |= ServerRunning;
	for (int proto = IO_UDP; proto <= IO_XDP; ++proto) {
		if (server->handlers[proto].size > 0) {
			int ret = dt_start(server->handlers[proto].handler.unit);
			if (ret != KNOT_EOK) {
				return ret;
			}
		}
	}

	return KNOT_EOK;
}

void server_wait(server_t *server)
{
	if (server == NULL) {
		return;
	}

	evsched_join(&server->sched);
	worker_pool_join(server->workers);

	for (int proto = IO_UDP; proto <= IO_XDP; ++proto) {
		if (server->handlers[proto].size > 0) {
			server_free_handler(&server->handlers[proto].handler);
		}
	}
}

static int reload_conf(conf_t *new_conf)
{
	yp_schema_purge_dynamic(new_conf->schema);

	/* Re-load common modules. */
	int ret = conf_mod_load_common(new_conf);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Re-import zonefile if specified. */
	const char *filename = conf()->filename;
	if (filename != NULL) {
		log_info("reloading configuration file '%s'", filename);

		/* Import the configuration file. */
		ret = conf_import(new_conf, filename, true, false);
		if (ret != KNOT_EOK) {
			log_error("failed to load configuration file (%s)",
			          knot_strerror(ret));
			return ret;
		}
	} else {
		log_info("reloading configuration database '%s'",
		         knot_db_lmdb_get_path(new_conf->db));

		/* Re-load extra modules. */
		for (conf_iter_t iter = conf_iter(new_conf, C_MODULE);
		     iter.code == KNOT_EOK; conf_iter_next(new_conf, &iter)) {
			conf_val_t id = conf_iter_id(new_conf, &iter);
			conf_val_t file = conf_id_get(new_conf, C_MODULE, C_FILE, &id);
			ret = conf_mod_load_extra(new_conf, conf_str(&id), conf_str(&file), false);
			if (ret != KNOT_EOK) {
				conf_iter_finish(new_conf, &iter);
				return ret;
			}
		}
	}

	conf_mod_load_purge(new_conf, false);

	// Migrate from old schema.
	ret = conf_migrate(new_conf);
	if (ret != KNOT_EOK) {
		log_error("failed to migrate configuration (%s)", knot_strerror(ret));
	}

	/* Refresh hostname. */
	conf_refresh_hostname(new_conf);

	return KNOT_EOK;
}

/*! \brief Check if parameter listen has been changed since knotd started. */
static bool listen_changed(conf_t *conf, server_t *server)
{
	assert(server->ifaces);

	conf_val_t listen_val = conf_get(conf, C_SRV, C_LISTEN);
	size_t new_count = conf_val_count(&listen_val);
	size_t old_count = server->n_ifaces;
	if (new_count != old_count) {
		return true;
	}

	conf_val_t rundir_val = conf_get(conf, C_SRV, C_RUNDIR);
	char *rundir = conf_abs_path(&rundir_val, NULL);
	size_t matches = 0;

	/* Find matching interfaces. */
	while (listen_val.code == KNOT_EOK) {
		struct sockaddr_storage addr = conf_addr(&listen_val, rundir);
		bool found = false;
		for (size_t i = 0; i < server->n_ifaces; i++) {
			if (sockaddr_cmp(&addr, &server->ifaces[i].addr, false) == 0) {
				matches++;
				found = true;
				break;
			}
		}

		if (!found) {
			break;
		}
		conf_val_next(&listen_val);
	}

	free(rundir);

	return matches != old_count;
}

/*! \brief Log warnings if config change requires a restart. */
static void warn_server_reconfigure(conf_t *conf, server_t *server)
{
	const char *msg = "changes of %s require restart to take effect";

	static bool warn_tcp_reuseport = true;
	static bool warn_udp = true;
	static bool warn_xdp = true;
	static bool warn_tcp = true;
	static bool warn_bg = true;
	static bool warn_listen = true;

	if (warn_tcp_reuseport && conf->cache.srv_tcp_reuseport != conf_tcp_reuseport(conf)) {
		log_warning(msg, "tcp-reuseport");
		warn_tcp_reuseport = false;
	}

	if (warn_udp && server->handlers[IO_UDP].size != conf_udp_threads(conf)) {
		log_warning(msg, "udp-workers");
		warn_udp = false;
	}

	if (warn_xdp && server->handlers[IO_XDP].size != conf_xdp_threads(conf)) {
		log_warning(msg, "xdp-workers");
		warn_xdp = false;
	}

	if (warn_tcp && server->handlers[IO_TCP].size != conf_tcp_threads(conf)) {
		log_warning(msg, "tcp-workers");
		warn_tcp = false;
	}

	if (warn_bg && conf->cache.srv_bg_threads != conf_bg_threads(conf)) {
		log_warning(msg, "background-workers");
		warn_bg = false;
	}

	if (warn_listen && listen_changed(conf, server)) {
		log_warning(msg, "listen");
		warn_listen = false;
	}
}

int server_reload(server_t *server)
{
	if (server == NULL) {
		return KNOT_EINVAL;
	}

	/* Check for no edit mode. */
	if (conf()->io.txn != NULL) {
		log_warning("reload aborted due to active configuration transaction");
		return KNOT_TXN_EEXISTS;
	}

	conf_t *new_conf = NULL;
	int ret = conf_clone(&new_conf);
	if (ret != KNOT_EOK) {
		log_error("failed to initialize configuration (%s)",
		          knot_strerror(ret));
		return ret;
	}

	yp_flag_t flags = conf()->io.flags;
	bool full = !(flags & CONF_IO_FACTIVE);
	bool reuse_modules = !full && !(flags & CONF_IO_FRLD_MOD);

	/* Reload configuration and modules if full reload or a module change. */
	if (full || !reuse_modules) {
		ret = reload_conf(new_conf);
		if (ret != KNOT_EOK) {
			conf_free(new_conf);
			return ret;
		}

		conf_activate_modules(new_conf, server, NULL, new_conf->query_modules,
		                      &new_conf->query_plan);
	}

	conf_update_flag_t upd_flags = CONF_UPD_FNOFREE;
	if (!full) {
		upd_flags |= CONF_UPD_FCONFIO;
	}
	if (reuse_modules) {
		upd_flags |= CONF_UPD_FMODULES;
	}

	/* Update to the new config. */
	conf_t *old_conf = conf_update(new_conf, upd_flags);

	/* Reload each component if full reload or a specific one if required. */
	if (full || (flags & CONF_IO_FRLD_LOG)) {
		log_reconfigure(conf());
	}
	if (full || (flags & CONF_IO_FRLD_SRV)) {
		server_reconfigure(conf(), server);
		warn_server_reconfigure(conf(), server);
		stats_reconfigure(conf(), server);
	}
	if (full || (flags & (CONF_IO_FRLD_ZONES | CONF_IO_FRLD_ZONE))) {
		server_update_zones(conf(), server);
	}

	/* Free old config needed for module unload in zone reload. */
	conf_free(old_conf);

	if (full) {
		log_info("configuration reloaded");
	} else {
		// Reset confio reload context.
		conf()->io.flags = YP_FNONE;
		if (conf()->io.zones != NULL) {
			trie_clear(conf()->io.zones);
		}
	}

	return KNOT_EOK;
}

void server_stop(server_t *server)
{
	log_info("stopping server");

	/* Stop scheduler. */
	evsched_stop(&server->sched);
	/* Interrupt background workers. */
	worker_pool_stop(server->workers);

	/* Clear 'running' flag. */
	server->state &= ~ServerRunning;
}

static int set_handler(server_t *server, int index, unsigned size, bool use_xdp, runnable_t run)
{
	/* Initialize I/O handlers. */
	int ret = server_init_handler(server, index, size, run, NULL);
	if (ret != KNOT_EOK) {
		return ret;
	}

	server->handlers[index].size = size;
	server->handlers[index].handler.use_xdp = use_xdp;

	return KNOT_EOK;
}

/*! \brief Reconfigure UDP and TCP query processing threads. */
static int configure_threads(conf_t *conf, server_t *server)
{
	int ret = set_handler(server, IO_UDP, conf->cache.srv_udp_threads, false, udp_master);
	if (ret != KNOT_EOK) {
		return ret;
	}

	if (conf->cache.srv_xdp_threads > 0) {
		ret = set_handler(server, IO_XDP, conf->cache.srv_xdp_threads, true, udp_master);
		if (ret != KNOT_EOK) {
			return ret;
		}
	}

	return set_handler(server, IO_TCP, conf->cache.srv_tcp_threads, false, tcp_master);
}

static int reconfigure_journal_db(conf_t *conf, server_t *server)
{
	char *journal_dir = conf_db(conf, C_JOURNAL_DB);
	conf_val_t journal_size = conf_db_param(conf, C_JOURNAL_DB_MAX_SIZE, C_MAX_JOURNAL_DB_SIZE);
	conf_val_t journal_mode = conf_db_param(conf, C_JOURNAL_DB_MODE, C_JOURNAL_DB_MODE);
	int ret = knot_lmdb_reinit(&server->journaldb, journal_dir, conf_int(&journal_size),
	                           journal_env_flags(conf_opt(&journal_mode)));
	if (ret != KNOT_EOK) {
		log_warning("ignored reconfiguration of journal DB (%s)", knot_strerror(ret));
	}
	free(journal_dir);

	return KNOT_EOK; // not "ret"
}

static int reconfigure_kasp_db(conf_t *conf, server_t *server)
{
	char *kasp_dir = conf_db(conf, C_KASP_DB);
	conf_val_t kasp_size = conf_db_param(conf, C_KASP_DB_MAX_SIZE, C_MAX_KASP_DB_SIZE);
	int ret = knot_lmdb_reinit(&server->kaspdb, kasp_dir, conf_int(&kasp_size), 0);
	if (ret != KNOT_EOK) {
		log_warning("ignored reconfiguration of KASP DB (%s)", knot_strerror(ret));
	}
	free(kasp_dir);

	return KNOT_EOK; // not "ret"
}

static int reconfigure_timer_db(conf_t *conf, server_t *server)
{
	char *timer_dir = conf_db(conf, C_TIMER_DB);
	conf_val_t timer_size = conf_db_param(conf, C_TIMER_DB_MAX_SIZE, C_MAX_TIMER_DB_SIZE);
	int ret = knot_lmdb_reconfigure(&server->timerdb, timer_dir, conf_int(&timer_size), 0);
	free(timer_dir);
	return ret;
}

void server_reconfigure(conf_t *conf, server_t *server)
{
	if (conf == NULL || server == NULL) {
		return;
	}

	int ret;

	/* First reconfiguration. */
	if (!(server->state & ServerRunning)) {
		log_info("Knot DNS %s starting", PACKAGE_VERSION);

		if (conf->filename != NULL) {
			log_info("loaded configuration file '%s'",
			         conf->filename);
		} else {
			log_info("loaded configuration database '%s'",
			         knot_db_lmdb_get_path(conf->db));
		}

		/* Configure server threads. */
		if ((ret = configure_threads(conf, server)) != KNOT_EOK) {
			log_error("failed to configure server threads (%s)",
			          knot_strerror(ret));
		}

		/* Configure sockets. */
		if ((ret = configure_sockets(conf, server)) != KNOT_EOK) {
			log_error("failed to configure server sockets (%s)",
			          knot_strerror(ret));
		}
	}

	/* Reconfigure journal DB. */
	if ((ret = reconfigure_journal_db(conf, server)) != KNOT_EOK) {
		log_error("failed to reconfigure journal DB (%s)",
		          knot_strerror(ret));
	}

	/* Reconfigure KASP DB. */
	if ((ret = reconfigure_kasp_db(conf, server)) != KNOT_EOK) {
		log_error("failed to reconfigure KASP DB (%s)",
		          knot_strerror(ret));
	}

	/* Reconfiure Timer DB. */
	if ((ret = reconfigure_timer_db(conf, server)) != KNOT_EOK) {
		log_error("failed to reconfigure Timer DB (%s)",
		          knot_strerror(ret));
	}
}

void server_update_zones(conf_t *conf, server_t *server)
{
	if (conf == NULL || server == NULL) {
		return;
	}

	/* Prevent emitting of new zone events. */
	if (server->zone_db) {
		knot_zonedb_foreach(server->zone_db, zone_events_freeze);
	}

	/* Suspend adding events to worker pool queue, wait for queued events. */
	evsched_pause(&server->sched);
	worker_pool_wait(server->workers);

	/* Reload zone database and free old zones. */
	zonedb_reload(conf, server);

	/* Trim extra heap. */
	mem_trim();

	/* Resume processing events on new zones. */
	evsched_resume(&server->sched);
	if (server->zone_db) {
		knot_zonedb_foreach(server->zone_db, zone_events_start);
	}
}
