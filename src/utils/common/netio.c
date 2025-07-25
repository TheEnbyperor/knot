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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/types.h>   // OpenBSD
#include <netinet/tcp.h> // TCP_FASTOPEN
#include <sys/socket.h>

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#include "utils/common/netio.h"
#include "utils/common/msg.h"
#include "utils/common/tls.h"
#include "libknot/libknot.h"
#include "libknot/quic/tls_common.h"
#include "contrib/net.h"
#include "contrib/proxyv2/proxyv2.h"
#include "contrib/sockaddr.h"

static knot_probe_proto_t get_protocol(const net_t *net)
{
#ifdef ENABLE_QUIC
	if (net->quic.params.enable) {
		return KNOT_PROBE_PROTO_QUIC;
	} else
#endif
#ifdef LIBNGHTTP2
	if (net->https.params.enable) {
		return KNOT_PROBE_PROTO_HTTPS;
	} else
#endif
	if (net->tls.params != NULL && net->tls.params->enable) {
		return KNOT_PROBE_PROTO_TLS;
	} else if (net->socktype == PROTO_TCP) {
		return KNOT_PROBE_PROTO_TCP;
	} else {
		assert(net->socktype == PROTO_UDP);
		return KNOT_PROBE_PROTO_UDP;
	}
}

static const char *get_protocol_str(const knot_probe_proto_t proto)
{
	switch (proto) {
	case KNOT_PROBE_PROTO_UDP:
		return "UDP";
	case KNOT_PROBE_PROTO_QUIC:
		return "QUIC";
	case KNOT_PROBE_PROTO_TCP:
		return "TCP";
	case KNOT_PROBE_PROTO_TLS:
		return "TLS";
	case KNOT_PROBE_PROTO_HTTPS:
		return "HTTPS";
	default:
		return "UNKNOWN";
	}
}

srv_info_t *srv_info_create(const char *name, const char *service)
{
	if (name == NULL || service == NULL) {
		DBG_NULL;
		return NULL;
	}

	// Create output structure.
	srv_info_t *server = calloc(1, sizeof(srv_info_t));

	// Check output.
	if (server == NULL) {
		return NULL;
	}

	// Fill output.
	server->name = strdup(name);
	server->service = strdup(service);

	if (server->name == NULL || server->service == NULL) {
		srv_info_free(server);
		return NULL;
	}

	// Return result.
	return server;
}

void srv_info_free(srv_info_t *server)
{
	if (server == NULL) {
		DBG_NULL;
		return;
	}

	free(server->name);
	free(server->service);
	free(server);
}

int get_iptype(const ip_t ip, const srv_info_t *server)
{
	bool unix_socket = (server->name[0] == '/');

	switch (ip) {
	case IP_4:
		return AF_INET;
	case IP_6:
		return AF_INET6;
	default:
		return unix_socket ? AF_UNIX : AF_UNSPEC;
	}
}

int get_socktype(const protocol_t proto, const uint16_t type)
{
	switch (proto) {
	case PROTO_TCP:
		return SOCK_STREAM;
	case PROTO_UDP:
		return SOCK_DGRAM;
	default:
		if (type == KNOT_RRTYPE_AXFR || type == KNOT_RRTYPE_IXFR) {
			return SOCK_STREAM;
		} else {
			return SOCK_DGRAM;
		}
	}
}

const char *get_sockname(const int socktype)
{
	switch (socktype) {
	case SOCK_STREAM:
		return "TCP";
	case SOCK_DGRAM:
		return "UDP";
	default:
		return "UNKNOWN";
	}
}

static int get_addr(const srv_info_t *server,
                    const int        iptype,
                    const int        socktype,
                    struct addrinfo  **info)
{
	struct addrinfo hints;

	// Set connection hints.
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = iptype;
	hints.ai_socktype = socktype;

	// Get connection parameters.
	int ret = getaddrinfo(server->name, server->service, &hints, info);
	switch (ret) {
	case 0:
		return 0;
#ifdef EAI_ADDRFAMILY	/* EAI_ADDRFAMILY isn't implemented in FreeBSD/macOS anymore. */
	case EAI_ADDRFAMILY:
		break;
#else			/* FreeBSD, macOS, and likely others return EAI_NONAME instead. */
	case EAI_NONAME:
		if (iptype != AF_UNSPEC) {
			break;
		}
		/* FALLTHROUGH */
#endif	/* EAI_ADDRFAMILY */
	default:
		ERR("%s for %s@%s", gai_strerror(ret), server->name, server->service);
	}
	return -1;
}

void get_addr_str(const struct sockaddr_storage *ss,
                  const knot_probe_proto_t      protocol,
                  char                          **dst)
{
	char addr_str[SOCKADDR_STRLEN] = { 0 };
	const char *proto_str = get_protocol_str(protocol);

	// Get network address string and port number.
	sockaddr_tostr(addr_str, sizeof(addr_str), ss);

	// Calculate needed buffer size
	size_t buflen = strlen(addr_str) + strlen(proto_str) + 3 /* () */;

	// Free previous string if any and write result
	free(*dst);
	*dst = malloc(buflen);
	if (*dst != NULL) {
		int ret = snprintf(*dst, buflen, "%s(%s)", addr_str, proto_str);
		if (ret <= 0 || ret >= buflen) {
			**dst = '\0';
		}
	}
}

int net_init(const srv_info_t      *local,
             const srv_info_t      *remote,
             const int             iptype,
             const int             socktype,
             const int             wait,
             const net_flags_t     flags,
             const struct sockaddr *proxy_src,
             const struct sockaddr *proxy_dst,
             net_t                 *net)
{
	if (remote == NULL || net == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	// Clean network structure.
	memset(net, 0, sizeof(*net));
	net->sockfd = -1;

	if (iptype == AF_UNIX) {
		struct addrinfo *info = calloc(1, sizeof(struct addrinfo));
		info->ai_addr = calloc(1, sizeof(struct sockaddr_storage));
		info->ai_addrlen = sizeof(struct sockaddr_un);
		info->ai_socktype = socktype;
		info->ai_family = iptype;
		int ret = sockaddr_set_raw((struct sockaddr_storage *)info->ai_addr,
		                           AF_UNIX, (const uint8_t *)remote->name,
		                           strlen(remote->name));
		if (ret != KNOT_EOK) {
			free(info->ai_addr);
			free(info);
			return ret;
		}
		net->remote_info = info;
	} else {
		// Get remote address list.
		if (get_addr(remote, iptype, socktype, &net->remote_info) != 0) {
			net_clean(net);
			return KNOT_NET_EADDR;
		}
	}

	// Set current remote address.
	net->srv = net->remote_info;

	// Get local address if specified.
	if (local != NULL) {
		if (get_addr(local, iptype, socktype, &net->local_info) != 0) {
			net_clean(net);
			return KNOT_NET_EADDR;
		}
	}

	// Store network parameters.
	net->sockfd = -1;
	net->iptype = iptype;
	net->socktype = socktype;
	net->wait = wait;
	net->local = local;
	net->remote = remote;
	net->flags = flags;
	net->proxy.src = proxy_src;
	net->proxy.dst = proxy_dst;

	if ((bool)(proxy_src == NULL) != (bool)(proxy_dst == NULL) ||
	    (proxy_src != NULL && proxy_src->sa_family != proxy_dst->sa_family)) {
		net_clean(net);
		return KNOT_EINVAL;
	}

	return KNOT_EOK;
}

int net_init_crypto(net_t                 *net,
                    const tls_params_t    *tls_params,
                    const https_params_t  *https_params,
                    const quic_params_t   *quic_params)
{
	if (net == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	if (tls_params == NULL || !tls_params->enable) {
		return KNOT_EOK;
	}

	tls_ctx_deinit(&net->tls);
#ifdef LIBNGHTTP2
	// Prepare for HTTPS.
	if (https_params != NULL && https_params->enable) {
		int ret = tls_ctx_init(&net->tls, tls_params,
		                       GNUTLS_NONBLOCK, net->wait);
		if (ret != KNOT_EOK) {
			net_clean(net);
			return ret;
		}
		https_ctx_deinit(&net->https);
		ret = https_ctx_init(&net->https, &net->tls, https_params);
		if (ret != KNOT_EOK) {
			net_clean(net);
			return ret;
		}
	} else
#endif //LIBNGHTTP2
#ifdef ENABLE_QUIC
	if (quic_params != NULL && quic_params->enable) {
		int ret = tls_ctx_init(&net->tls, tls_params,
		                       GNUTLS_NONBLOCK | GNUTLS_ENABLE_EARLY_DATA |
		                       GNUTLS_NO_END_OF_EARLY_DATA, net->wait);
		if (ret != KNOT_EOK) {
			net_clean(net);
			return ret;
		}
		quic_ctx_deinit(&net->quic);
		ret = quic_ctx_init(&net->quic, &net->tls, quic_params);
		if (ret != KNOT_EOK) {
			net_clean(net);
			return ret;
		}
	} else
#endif //ENABLE_QUIC
	{
		int ret = tls_ctx_init(&net->tls, tls_params,
		                       GNUTLS_NONBLOCK, net->wait);
		if (ret != KNOT_EOK) {
			net_clean(net);
			return ret;
		}
	}

	return KNOT_EOK;
}

/*!
 * Connect with TCP Fast Open.
 */
static int fastopen_connect(int sockfd, const struct addrinfo *srv)
{
#if defined( __FreeBSD__)
	const int enable = 1;
	return setsockopt(sockfd, IPPROTO_TCP, TCP_FASTOPEN, &enable, sizeof(enable));
#elif defined(__APPLE__)
	// connection is performed lazily when first data are sent
	struct sa_endpoints ep = {0};
	ep.sae_dstaddr = srv->ai_addr;
	ep.sae_dstaddrlen = srv->ai_addrlen;
	int flags =  CONNECT_DATA_IDEMPOTENT|CONNECT_RESUME_ON_READ_WRITE;

	return connectx(sockfd, &ep, SAE_ASSOCID_ANY, flags, NULL, 0, NULL, NULL);
#elif defined(__linux__)
	// connect() will be called implicitly with sendto(), sendmsg()
	return 0;
#else
	errno = ENOTSUP;
	return -1;
#endif
}

/*!
 * Sends data with TCP Fast Open.
 */
static int fastopen_send(int sockfd, const struct msghdr *msg, int timeout)
{
#if defined(__FreeBSD__) || defined(__APPLE__)
	return sendmsg(sockfd, msg, 0);
#elif defined(__linux__)
	int ret = sendmsg(sockfd, msg, MSG_FASTOPEN);
	if (ret == -1 && errno == EINPROGRESS) {
		struct pollfd pfd = {
			.fd = sockfd,
			.events = POLLOUT,
			.revents = 0,
		};
		if (poll(&pfd, 1, 1000 * timeout) != 1) {
			errno = ETIMEDOUT;
			return -1;
		}
		ret = sendmsg(sockfd, msg, 0);
	}
	return ret;
#else
	errno = ENOTSUP;
	return -1;
#endif
}

static char *net_get_remote(const net_t *net)
{
	if (net->tls.params->sni != NULL) {
		return net->tls.params->sni;
	} else if (net->tls.params->hostname != NULL) {
		return net->tls.params->hostname;
	} else if (strchr(net->remote_str, ':') == NULL) {
		char *at = strchr(net->remote_str, '@');
		if (at != NULL && strncmp(net->remote->name, net->remote_str,
		                          at - net->remote_str)) {
			return net->remote->name;
		}
	}
	return NULL;
}

int net_connect(net_t *net)
{
	if (net == NULL || net->srv == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	// Set remote information string.
	get_addr_str((struct sockaddr_storage *)net->srv->ai_addr,
	             get_protocol(net), &net->remote_str);

	// Create socket.
	int sockfd = socket(net->srv->ai_family, net->socktype, 0);
	if (sockfd == -1) {
		WARN("can't create socket for %s", net->remote_str);
		return KNOT_NET_ESOCKET;
	}

	// Initialize poll descriptor structure.
	struct pollfd pfd = {
		.fd = sockfd,
		.events = POLLOUT,
		.revents = 0,
	};

	// Set non-blocking socket.
	if (fcntl(sockfd, F_SETFL, O_NONBLOCK) == -1) {
		WARN("can't set non-blocking socket for %s", net->remote_str);
		return KNOT_NET_ESOCKET;
	}

	// Bind address to socket if specified.
	if (net->local_info != NULL) {
		if (bind(sockfd, net->local_info->ai_addr,
		         net->local_info->ai_addrlen) == -1) {
			WARN("can't assign address %s", net->local->name);
			return KNOT_NET_ESOCKET;
		}
	} else {
		// Ensure source port is always randomized (even for TCP).
		struct sockaddr_storage local = { .ss_family = net->srv->ai_family };
		(void)bind(sockfd, (struct sockaddr *)&local, sockaddr_len(&local));
	}

	int ret = 0;
	if (net->socktype == SOCK_STREAM) {
		int  cs = 1, err;
		socklen_t err_len = sizeof(err);
		bool fastopen = net->flags & NET_FLAGS_FASTOPEN;

#ifdef TCP_NODELAY
		(void)setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &cs, sizeof(cs));
#endif

		// Establish a connection.
		if (net->tls.params == NULL || !fastopen) {
			if (fastopen) {
				ret = fastopen_connect(sockfd, net->srv);
			} else {
				ret = connect(sockfd, net->srv->ai_addr, net->srv->ai_addrlen);
			}
			if (ret != 0 && errno != EINPROGRESS) {
				WARN("can't connect to %s", net->remote_str);
				net_close(net);
				return KNOT_NET_ECONNECT;
			}

			// Check for connection timeout.
			if (!fastopen && poll(&pfd, 1, 1000 * net->wait) != 1) {
				WARN("connection timeout for %s", net->remote_str);
				net_close(net);
				return KNOT_NET_ECONNECT;
			}

			// Check if NB socket is writeable.
			cs = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &err_len);
			if (cs < 0 || err != 0) {
				WARN("can't connect to %s", net->remote_str);
				net_close(net);
				return KNOT_NET_ECONNECT;
			}
		}

		if (net->tls.params != NULL) {
#ifdef LIBNGHTTP2
			if (net->https.params.enable) {
				// Establish HTTPS connection.
				char *remote = net_get_remote(net);
				ret = tls_ctx_setup_remote_endpoint(&net->tls, &doh_alpn, 1, NULL,
				        remote);
				if (ret != 0) {
					net_close(net);
					return ret;
				}
				if (remote && net->https.authority == NULL) {
					net->https.authority = strdup(remote);
				}
				ret = https_ctx_connect(&net->https, sockfd, fastopen,
				        (struct sockaddr_storage *)net->srv->ai_addr);
			} else
#endif //LIBNGHTTP2
			{
				// Establish TLS connection.
				ret = tls_ctx_setup_remote_endpoint(&net->tls, &dot_alpn, 1,
				        knot_tls_priority(true), net_get_remote(net));
				if (ret != 0) {
					net_close(net);
					return ret;
				}
				ret = tls_ctx_connect(&net->tls, sockfd, fastopen,
				        (struct sockaddr_storage *)net->srv->ai_addr);
			}
			if (ret != KNOT_EOK) {
				net_close(net);
				return ret;
			}
		}
	}
#ifdef ENABLE_QUIC
	else if (net->socktype == SOCK_DGRAM) {
		if (net->quic.params.enable) {
			// Establish QUIC connection.
			ret = net_cmsg_ecn_enable(sockfd, net->srv->ai_family);
			if (ret != KNOT_EOK && ret != KNOT_ENOTSUP) {
				net_close(net);
				return ret;
			}
			ret = tls_ctx_setup_remote_endpoint(&net->tls,
			        &doq_alpn, 1, knot_tls_priority(false), net_get_remote(net));
			if (ret != 0) {
				net_close(net);
				return ret;
			}
			ret = quic_ctx_connect(&net->quic, sockfd,
			        (struct addrinfo *)net->srv);
			if (ret != KNOT_EOK) {
				net_close(net);
				return ret;
			}
		}
	}
#endif

	// Store socket descriptor.
	net->sockfd = sockfd;

	return KNOT_EOK;
}

int net_set_local_info(net_t *net)
{
	if (net == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	socklen_t local_addr_len = sizeof(struct sockaddr_storage);

	struct addrinfo *new_info = calloc(1, sizeof(*new_info) + local_addr_len);
	if (new_info == NULL) {
		return KNOT_ENOMEM;
	}

	new_info->ai_addr = (struct sockaddr *)(new_info + 1);
	new_info->ai_family = net->srv->ai_family;
	new_info->ai_socktype = net->srv->ai_socktype;
	new_info->ai_protocol = net->srv->ai_protocol;
	new_info->ai_addrlen = local_addr_len;

	if (getsockname(net->sockfd, new_info->ai_addr,	&local_addr_len) == -1) {
		WARN("can't get local address");
		free(new_info);
		return KNOT_NET_ESOCKET;
	}

	if (net->local_info != NULL) {
		if (net->local == NULL) {
			free(net->local_info);
		} else {
			freeaddrinfo(net->local_info);
		}
	}

	net->local_info = new_info;

	get_addr_str((struct sockaddr_storage *)net->local_info->ai_addr,
	             get_protocol(net), &net->local_str);

	return KNOT_EOK;
}

int net_send(const net_t *net, const uint8_t *buf, const size_t buf_len)
{
	if (net == NULL || buf == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

#ifdef ENABLE_QUIC
	// Send data over QUIC.
	if (net->quic.params.enable) {
		int ret = quic_send_dns_query((quic_ctx_t *)&net->quic,
		                              net->sockfd, net->srv, buf, buf_len);
		if (ret != KNOT_EOK) {
			WARN("can't send query to %s", net->remote_str);
			return KNOT_NET_ESEND;
		}
	} else
#endif
	// Send data over UDP.
	if (net->socktype == SOCK_DGRAM) {
		char proxy_buf[PROXYV2_HEADER_MAXLEN];
		struct iovec iov[2] = {
			{ .iov_base = proxy_buf, .iov_len = 0 },
			{ .iov_base = (void *)buf, .iov_len = buf_len }
		};

		struct msghdr msg = {
			.msg_name = net->srv->ai_addr,
			.msg_namelen = net->srv->ai_addrlen,
			.msg_iov = &iov[1],
			.msg_iovlen = 1
		};

		if (net->proxy.src != NULL && net->proxy.src->sa_family != 0) {
			int ret = proxyv2_write_header(proxy_buf, sizeof(proxy_buf),
			                               SOCK_DGRAM, net->proxy.src,
			                               net->proxy.dst);
			if (ret < 0) {
				WARN("can't send proxied query to %s", net->remote_str);
				return KNOT_NET_ESEND;
			}
			iov[0].iov_len = ret;
			msg.msg_iov--;
			msg.msg_iovlen++;
		}

		ssize_t total = iov[0].iov_len + iov[1].iov_len;

		if (sendmsg(net->sockfd, &msg, 0) != total) {
			WARN("can't send query to %s", net->remote_str);
			return KNOT_NET_ESEND;
		}
#ifdef LIBNGHTTP2
	// Send data over HTTPS
	} else if (net->https.params.enable) {
		int ret = https_send_dns_query((https_ctx_t *)&net->https, buf, buf_len);
		if (ret != KNOT_EOK) {
			WARN("can't send query to %s", net->remote_str);
			return KNOT_NET_ESEND;
		}
#endif //LIBNGHTTP2
	// Send data over TLS.
	} else if (net->tls.params != NULL) {
		int ret = tls_ctx_send((tls_ctx_t *)&net->tls, buf, buf_len);
		if (ret != KNOT_EOK) {
			WARN("can't send query to %s", net->remote_str);
			return KNOT_NET_ESEND;
		}
	// Send data over TCP.
	} else {
		bool fastopen = net->flags & NET_FLAGS_FASTOPEN;

		char proxy_buf[PROXYV2_HEADER_MAXLEN];
		uint16_t pktsize = htons(buf_len); // Leading packet length bytes.
		struct iovec iov[3] = {
			{ .iov_base = proxy_buf, .iov_len = 0 },
			{ .iov_base = &pktsize, .iov_len = sizeof(pktsize) },
			{ .iov_base = (void *)buf, .iov_len = buf_len }
		};

		struct msghdr msg = {
			.msg_name = net->srv->ai_addr,
			.msg_namelen = net->srv->ai_addrlen,
			.msg_iov = &iov[1],
			.msg_iovlen = 2
		};

		if (net->srv->ai_addr->sa_family == AF_UNIX) {
			msg.msg_name = NULL;
		}

		if (net->proxy.src != NULL && net->proxy.src->sa_family != 0) {
			int ret = proxyv2_write_header(proxy_buf, sizeof(proxy_buf),
			                               SOCK_STREAM, net->proxy.src,
			                               net->proxy.dst);
			if (ret < 0) {
				WARN("can't send proxied query to %s", net->remote_str);
				return KNOT_NET_ESEND;
			}
			iov[0].iov_len = ret;
			msg.msg_iov--;
			msg.msg_iovlen++;
		}

		ssize_t total = iov[0].iov_len + iov[1].iov_len + iov[2].iov_len;

		int ret = 0;
		if (fastopen) {
			ret = fastopen_send(net->sockfd, &msg, net->wait);
		} else {
			ret = sendmsg(net->sockfd, &msg, 0);
		}
		if (ret != total) {
			WARN("can't send query to %s", net->remote_str);
			return KNOT_NET_ESEND;
		}
	}

	return KNOT_EOK;
}

int net_receive(const net_t *net, uint8_t *buf, const size_t buf_len)
{
	if (net == NULL || buf == NULL) {
		DBG_NULL;
		return KNOT_EINVAL;
	}

	// Initialize poll descriptor structure.
	struct pollfd pfd = {
		.fd = net->sockfd,
		.events = POLLIN,
		.revents = 0,
	};

#ifdef ENABLE_QUIC
	// Receive data over QUIC.
	if (net->quic.params.enable) {
		int ret = quic_recv_dns_response((quic_ctx_t *)&net->quic, buf,
		                                 buf_len, net->srv);
		if (ret < 0) {
			WARN("can't receive reply from %s", net->remote_str);
			return KNOT_NET_ERECV;
		}
		return ret;
	} else
#endif
	// Receive data over UDP.
	if (net->socktype == SOCK_DGRAM) {
		struct sockaddr_storage from;
		memset(&from, '\0', sizeof(from));

		// Receive replies unless correct reply or timeout.
		while (true) {
			socklen_t from_len = sizeof(from);

			// Wait for datagram data.
			if (poll(&pfd, 1, 1000 * net->wait) != 1) {
				WARN("response timeout for %s",
				     net->remote_str);
				return KNOT_NET_ETIMEOUT;
			}

			// Receive whole UDP datagram.
			ssize_t ret = recvfrom(net->sockfd, buf, buf_len, 0,
			                       (struct sockaddr *)&from, &from_len);
			if (ret <= 0) {
				WARN("can't receive reply from %s",
				     net->remote_str);
				return KNOT_NET_ERECV;
			}

			// Compare reply address with the remote one.
			if (from_len > sizeof(from) ||
			    memcmp(&from, net->srv->ai_addr, from_len) != 0) {
				char *src = NULL;
				get_addr_str(&from, get_protocol(net), &src);
				WARN("unexpected reply source %s", src);
				free(src);
				continue;
			}

			return ret;
		}
#ifdef LIBNGHTTP2
	// Receive data over HTTPS.
	} else if (net->https.params.enable) {
		int ret = https_recv_dns_response((https_ctx_t *)&net->https, buf, buf_len);
		if (ret < 0) {
			WARN("can't receive reply from %s", net->remote_str);
			return KNOT_NET_ERECV;
		}
		return ret;
#endif //LIBNGHTTP2
	// Receive data over TLS.
	} else if (net->tls.params != NULL) {
		int ret = tls_ctx_receive((tls_ctx_t *)&net->tls, buf, buf_len);
		if (ret < 0) {
			WARN("can't receive reply from %s", net->remote_str);
			return KNOT_NET_ERECV;
		}
		return ret;
	// Receive data over TCP.
	} else {
		uint32_t total = 0;

		uint16_t msg_len = 0;
		// Receive TCP message header.
		while (total < sizeof(msg_len)) {
			if (poll(&pfd, 1, 1000 * net->wait) != 1) {
				WARN("response timeout for %s",
				     net->remote_str);
				return KNOT_NET_ETIMEOUT;
			}

			// Receive piece of message.
			ssize_t ret = recv(net->sockfd, (uint8_t *)&msg_len + total,
				           sizeof(msg_len) - total, 0);
			if (ret <= 0) {
				WARN("can't receive reply from %s",
				     net->remote_str);
				return KNOT_NET_ERECV;
			}
			total += ret;
		}

		// Convert number to host format.
		msg_len = ntohs(msg_len);
		if (msg_len > buf_len) {
			return KNOT_ESPACE;
		}

		total = 0;

		// Receive whole answer message by parts.
		while (total < msg_len) {
			if (poll(&pfd, 1, 1000 * net->wait) != 1) {
				WARN("response timeout for %s",
				     net->remote_str);
				return KNOT_NET_ETIMEOUT;
			}

			// Receive piece of message.
			ssize_t ret = recv(net->sockfd, buf + total, msg_len - total, 0);
			if (ret <= 0) {
				WARN("can't receive reply from %s",
				     net->remote_str);
				return KNOT_NET_ERECV;
			}
			total += ret;
		}

		return total;
	}

	return KNOT_NET_ERECV;
}

void net_close(net_t *net)
{
	if (net == NULL) {
		DBG_NULL;
		return;
	}

#ifdef ENABLE_QUIC
	if (net->quic.params.enable) {
		quic_ctx_close(&net->quic);
	}
#endif
	tls_ctx_close(&net->tls);
	close(net->sockfd);
	net->sockfd = -1;
}

void net_clean(net_t *net)
{
	if (net == NULL) {
		DBG_NULL;
		return;
	}

	free(net->local_str);
	free(net->remote_str);
	net->local_str = NULL;
	net->remote_str = NULL;

	if (net->local_info != NULL) {
		if (net->local == NULL) {
			free(net->local_info);
		} else {
			freeaddrinfo(net->local_info);
		}
		net->local_info = NULL;
	}

	if (net->remote_info != NULL) {
		if (net->remote_info->ai_addr->sa_family == AF_UNIX) {
			free(net->remote_info->ai_addr);
			free(net->remote_info);
		} else {
			freeaddrinfo(net->remote_info);
		}
		net->remote_info = NULL;
	}

#ifdef LIBNGHTTP2
	https_ctx_deinit(&net->https);
#endif
#ifdef ENABLE_QUIC
	quic_ctx_deinit(&net->quic);
#endif
	tls_ctx_deinit(&net->tls);
}
