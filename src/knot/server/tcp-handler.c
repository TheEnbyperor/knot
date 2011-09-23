#include <config.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/sockaddr.h"
#include "common/skip-list.h"
#include "common/fdset.h"
#include "knot/common.h"
#include "knot/server/tcp-handler.h"
#include "knot/server/xfr-handler.h"
#include "libknot/nameserver/name-server.h"
#include "knot/other/error.h"
#include "knot/stat/stat.h"
#include "libknot/util/wire.h"
#include "knot/server/zones.h"

/*! \brief TCP worker data. */
typedef struct tcp_worker_t {
	iohandler_t *ioh; /*!< Shortcut to I/O handler. */
	fdset_t *fdset;   /*!< File descriptor set. */ 
	int pipe[2];      /*!< Master-worker signalization pipes. */
} tcp_worker_t;

/*
 * Forward decls.
 */

/*! \brief Wrapper for TCP send. */
static int xfr_send_cb(int session, sockaddr_t *addr, uint8_t *msg, size_t msglen)
{
	UNUSED(addr);
	return tcp_send(session, msg, msglen);
}

/*!
 * \brief TCP event handler function.
 *
 * Handle single TCP event.
 *
 * \param w Associated I/O event.
 * \param revents Returned events.
 */
static void tcp_handle(tcp_worker_t *w, int fd)
{
	if (fd < 0 || !w || !w->ioh) {
		return;
	}
	
	debug_net("tcp: handling TCP event in thread %p.\n",
		  (void*)pthread_self());

	knot_nameserver_t *ns = w->ioh->server->nameserver;
	xfrhandler_t *xfr_h = w->ioh->server->xfr_h;

	/* Check address type. */
	sockaddr_t addr;
	if (sockaddr_init(&addr, w->ioh->type) != KNOTD_EOK) {
		log_server_error("Socket type %d is not supported, "
				 "IPv6 support is probably disabled.\n",
				 w->ioh->type);
		return;
	}

	/* Receive data. */
	uint8_t qbuf[65535]; /*! \todo This may be problematic. */
	size_t qbuf_maxlen = sizeof(qbuf);
	int n = tcp_recv(fd, qbuf, qbuf_maxlen, &addr);
	if (n <= 0) {
		debug_net("tcp: client disconnected\n");
		fdset_remove(w->fdset, fd);
		close(fd);
		return;
	}

	/* Parse query. */
//	knot_response_t *resp = knot_response_new(qbuf_maxlen);
	size_t resp_len = qbuf_maxlen; // 64K

	/* Parse query. */
	knot_packet_type_t qtype = KNOT_QUERY_NORMAL;

	knot_packet_t *packet =
		knot_packet_new(KNOT_PACKET_PREALLOC_QUERY);
	if (packet == NULL) {
		uint16_t pkt_id = knot_wire_get_id(qbuf);
		knot_ns_error_response(ns, pkt_id, KNOT_RCODE_SERVFAIL,
				  qbuf, &resp_len);
		return;
	}

	int res = knot_ns_parse_packet(qbuf, n, packet, &qtype);
	if (unlikely(res != KNOTD_EOK)) {

		/* Send error response on dnslib RCODE. */
		if (res > 0) {
			uint16_t pkt_id = knot_wire_get_id(qbuf);
			knot_ns_error_response(ns, pkt_id, res,
					  qbuf, &resp_len);
		}

//		knot_response_free(&resp);
		knot_packet_free(&packet);
		return;
	}

	/* Handle query. */
	knot_ns_xfr_t xfr;
	res = KNOTD_ERROR;
	switch(qtype) {

	/* Response types. */
	case KNOT_RESPONSE_NORMAL:
	case KNOT_RESPONSE_AXFR:
	case KNOT_RESPONSE_IXFR:
	case KNOT_RESPONSE_NOTIFY:
		/*! Implement packet handling. */
		break;

	/* Query types. */
	case KNOT_QUERY_NORMAL:
		res = knot_ns_answer_normal(ns, packet, qbuf, &resp_len);
		break;
	case KNOT_QUERY_IXFR:
//		memset(&xfr, 0, sizeof(knot_ns_xfr_t));
//		xfr.type = XFR_TYPE_IOUT;
//		wire_copy = malloc(sizeof(uint8_t) * packet->size);
//		if (!wire_copy) {
//			/*!< \todo Cleanup. */
//			ERR_ALLOC_FAILED;
//			return;
//		}
//		memcpy(wire_copy, packet->wireformat, packet->size);
//		packet->wireformat = wire_copy;
//		xfr.query = packet; /* Will be freed after processing. */
//		xfr.send = xfr_send_cb;
//		xfr.session = fd;
//		memcpy(&xfr.addr, &addr, sizeof(sockaddr_t));
//		xfr_request(xfr_h, &xfr);
//		debug_net("tcp: enqueued IXFR query on fd=%d\n", fd);
//		return;
		debug_net("tcp: IXFR not supported, will answer as AXFR on fd=%d\n", fd);
	case KNOT_QUERY_AXFR:
		memset(&xfr, 0, sizeof(knot_ns_xfr_t));
		xfr.type = XFR_TYPE_AOUT;
		uint8_t *wire_copy = malloc(sizeof(uint8_t) * packet->size);
		if (!wire_copy) {
			/*!< \todo Cleanup. */
			ERR_ALLOC_FAILED;
			return;
		}
		memcpy(wire_copy, packet->wireformat, packet->size);
		packet->wireformat = wire_copy;
		xfr.query = packet;
		xfr.send = xfr_send_cb;
		xfr.session = fd;
		memcpy(&xfr.addr, &addr, sizeof(sockaddr_t));
		xfr_request(xfr_h, &xfr);
		debug_net("tcp: enqueued AXFR query on fd=%d\n", fd);
		return;
	case KNOT_QUERY_NOTIFY:
	case KNOT_QUERY_UPDATE:
		/*! \todo Implement query notify/update. */
		knot_ns_error_response(ns, knot_packet_id(packet),
				       KNOT_RCODE_NOTIMPL, qbuf,
				       &resp_len);
		res = KNOTD_EOK;
		break;
	}

	debug_net("tcp: got answer of size %zd.\n",
		  resp_len);

	knot_packet_free(&packet);

	/* Send answer. */
	if (res == KNOTD_EOK) {
		assert(resp_len > 0);
		res = tcp_send(fd, qbuf, resp_len);

		/* Check result. */
		if (res != (int)resp_len) {
			debug_net("tcp: %s: failed: %d - %d.\n",
				  "socket_send()",
				  res, errno);
		}
	}

	return;
}

static int tcp_accept(int fd)
{
	/* Accept incoming connection. */
	int incoming = accept(fd, 0, 0);

	/* Evaluate connection. */
	if (incoming < 0) {
		if (errno != EINTR) {
			log_server_error("Cannot accept connection "
					 "(%d).\n", errno);
		}
	} else {
		debug_net("tcp: accepted connection fd = %d\n", incoming);
	}

	return incoming;
}

tcp_worker_t* tcp_worker_create()
{
	tcp_worker_t *w = malloc(sizeof(tcp_worker_t));
	if (!w) {
		debug_net("tcp_master: out of memory when creating worker\n");
		return 0;
	}
	
	/* Create signal pipes. */
	memset(w, 0, sizeof(tcp_worker_t));
	if (pipe(w->pipe) < 0) {
		free(w);
		return 0;
	}
	
	/* Create fdset. */
	w->fdset = fdset_new();
	if (!w->fdset) {
		close(w->pipe[0]);
		close(w->pipe[1]);
		free(w);
	}
	
	fdset_add(w->fdset, w->pipe[0], OS_EV_READ);
	
	return w;
}

void tcp_worker_free(tcp_worker_t* w)
{
	if (!w) {
		return;
	}
	
	/* Destroy fdset. */
	fdset_destroy(w->fdset);
	
	/* Close pipe write end and worker. */
	close(w->pipe[0]);
	close(w->pipe[1]);
	free(w);
}

/*
 * Public APIs.
 */

int tcp_send(int fd, uint8_t *msg, size_t msglen)
{

	/*! \brief TCP corking.
	 *  \see http://vger.kernel.org/~acme/unbehaved.txt
	 */
#ifdef TCP_CORK
	int cork = 1;
	setsockopt(fd, SOL_TCP, TCP_CORK, &cork, sizeof(cork));
#endif

	/* Send message size. */
	unsigned short pktsize = htons(msglen);
	int sent = send(fd, &pktsize, sizeof(pktsize), 0);
	if (sent < 0) {
		return KNOTD_ERROR;
	}

	/* Send message data. */
	sent = send(fd, msg, msglen, 0);
	if (sent < 0) {
		return KNOTD_ERROR;
	}

#ifdef TCP_CORK
	/* Uncork. */
	cork = 0;
	setsockopt(fd, SOL_TCP, TCP_CORK, &cork, sizeof(cork));
#endif
	return sent;
}

int tcp_recv(int fd, uint8_t *buf, size_t len, sockaddr_t *addr)
{
	/* Receive size. */
	unsigned short pktsize = 0;
	int n = recv(fd, &pktsize, sizeof(unsigned short), 0);
	if (n < 0) {
		return KNOTD_ERROR;
	}

	pktsize = ntohs(pktsize);

	// Check packet size for NULL
	if (pktsize == 0) {
		return KNOTD_ERROR;
	}

	debug_net("tcp: incoming packet size=%hu on fd=%d\n",
		  pktsize, fd);

	// Check packet size
	if (len < pktsize) {
		return KNOTD_ENOMEM;
	}

	/* Receive payload. */
	n = recv(fd, buf, pktsize, MSG_WAITALL);

	/* Get peer name. */
	if (addr) {
		socklen_t alen = addr->len;
		getpeername(fd, addr->ptr, &alen);
	}

	debug_net("tcp: received packet size=%d on fd=%d\n",
		  n, fd);

	return n;
}

int tcp_loop_master(dthread_t *thread)
{
	iohandler_t *handler = (iohandler_t *)thread->data;
	dt_unit_t *unit = thread->unit;
	tcp_worker_t **workers = handler->data;

	/* Check socket. */
	if (!handler || handler->fd < 0 || !workers) {
		debug_net("tcp_master: failed to initialize\n");
		return KNOTD_EINVAL;
	}

	/* Accept connections. */
	int id = 0;
	debug_net("tcp_master: created with %d workers\n", unit->size - 1);
	while(1) {
		/* Check for cancellation. */
		if (dt_is_cancelled(thread)) {
			break;
		}

		/* Accept client. */
		int client = tcp_accept(handler->fd);
		if (client < 0) {
			continue;
		}

		/* Add to worker in RR fashion. */
		if (write(workers[id]->pipe[1], &client, sizeof(int)) < 0) {
			debug_net("tcp_master: failed to register fd=%d to "
			          "worker=%d\n", client, id);
			close(client);
			continue;
		}
		id = get_next_rr(id, unit->size - 1);
	}

	debug_net("tcp_master: finished\n");
	free(workers);
	
	return KNOTD_EOK;
}

int tcp_loop_worker(dthread_t *thread)
{
	tcp_worker_t *w = thread->data;
	if (!w) {
		return KNOTD_EINVAL;
	}

	/* Accept clients. */
	debug_net("tcp: worker started, backend = %s\n", fdset_method());
	for (;;) {

		/* Cancellation point. */
		if (dt_is_cancelled(thread)) {
			break;
		}

		/* Wait for events. */
		int nfds = fdset_wait(w->fdset);
		if (nfds <= 0) {
			continue;
		}

		/* Process incoming events. */
		debug_net("tcp_worker: registered %d events\n",
		          nfds);
		fdset_it_t it;
		fdset_begin(w->fdset, &it);
		while(1) {
			
			/* Handle incoming clients. */
			if (it.fd == w->pipe[0]) {
				int client = 0;
				if (read(it.fd, &client, sizeof(int)) < 0) {
					continue;
				}

				debug_net("tcp_worker: registered client %d\n",
				          client);
				fdset_add(w->fdset, client, OS_EV_READ);
			} else {
				/* Handle other events. */
				tcp_handle(w, it.fd);
			}
			
			/* Check if next exists. */
			if (fdset_next(w->fdset, &it) != 0) {
				break;
			}
		}
		
	}

	/* Stop whole unit. */
	debug_net("tcp_worker: worker finished\n");
	tcp_worker_free(w);
	return KNOTD_EOK;
}

int tcp_loop_unit(iohandler_t *ioh, dt_unit_t *unit)
{
	if (unit->size < 1) {
		return KNOTD_EINVAL;
	}
	
	/* Create unit data. */
	tcp_worker_t **workers = malloc((unit->size - 1) *
	                                sizeof(tcp_worker_t *));
	if (!workers) {
		debug_net("tcp_master: out of memory\n");
		return KNOTD_EINVAL;
	}

	/* Prepare worker data. */
	unsigned allocated = 0;
	for (unsigned i = 0; i < unit->size - 1; ++i) {
		workers[i] = tcp_worker_create();
		if (workers[i] == 0) {
			break;
		}
		workers[i]->ioh = ioh;
		++allocated;
	}
	
	/* Check allocated workers. */
	if (allocated != unit->size - 1) {
		for (unsigned i = 0; i < allocated; ++i) {
			tcp_worker_free(workers[i]);
		}
	
		free(workers);
		debug_net("tcp_master: out of memory when allocated workers\n");
		return KNOTD_EINVAL;
	}
	
	/* Store worker data. */
	ioh->data = workers;
	
	/* Repurpose workers. */
	for (unsigned i = 0; i < allocated; ++i) {
		dt_repurpose(unit->threads[i + 1], tcp_loop_worker, workers[i]);
	}

	/* Repurpose first thread as master (unit controller). */
	dt_repurpose(unit->threads[0], tcp_loop_master, ioh);

	return KNOTD_EOK;
}
