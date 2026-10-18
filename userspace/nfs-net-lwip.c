/*
 * libnfs network backend on top of the lwIP user-space TCP/IP stack.
 *
 * lwIP is used through its raw (callback) API, in NO_SYS mode, so that the
 * whole stack executes inside libnfs' single threaded event loop. Data
 * movement to/from the kernel happens through a TUN device driven by
 * io_uring (see nfs-lwip-port.c).
 *
 * libnfs expects a non-blocking BSD socket. This file emulates just the
 * subset of the socket API that lib/socket.c uses on top of lwIP.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"

#include "nfs-net.h"
#include "nfs-lwip.h"

/* Synthetic fd offset. Zero is reserved because libnfs uses 0 as the
 * "no previous fd" sentinel when reconnecting. */
#define LWIP_FD_BASE	4096
#define LWIP_MAX_CONN	64

struct lwip_conn {
	int in_use;
	int type;		/* SOCK_STREAM or SOCK_DGRAM */

	int connecting;
	int connected;
	int peer_closed;
	int so_error;

	struct tcp_pcb *pcb;
	struct udp_pcb *upcb;

	ip_addr_t raddr;
	u16_t rport;

	ip_addr_t udp_src;
	u16_t udp_src_port;

	unsigned char *rx;
	size_t rx_len;
	size_t rx_pos;
	size_t rx_cap;
};

static struct lwip_conn conns[LWIP_MAX_CONN];

static struct lwip_conn *conn_get(int fd)
{
	int idx;

	if (fd < LWIP_FD_BASE || fd >= LWIP_FD_BASE + LWIP_MAX_CONN) {
		return NULL;
	}
	idx = fd - LWIP_FD_BASE;
	if (!conns[idx].in_use) {
		return NULL;
	}
	return &conns[idx];
}

static void conn_rx_reset(struct lwip_conn *c)
{
	c->rx_len = 0;
	c->rx_pos = 0;
}

static int conn_rx_reserve(struct lwip_conn *c, size_t extra)
{
	size_t need;

	if (c->rx_pos > 0 && c->rx_pos == c->rx_len) {
		conn_rx_reset(c);
	}
	if (c->rx_pos > 0) {
		memmove(c->rx, c->rx + c->rx_pos, c->rx_len - c->rx_pos);
		c->rx_len -= c->rx_pos;
		c->rx_pos = 0;
	}

	need = c->rx_len + extra;
	if (need <= c->rx_cap) {
		return 0;
	}

	{
		size_t cap = c->rx_cap ? c->rx_cap : 16384;
		unsigned char *p;

		while (cap < need) {
			cap *= 2;
		}
		p = realloc(c->rx, cap);
		if (p == NULL) {
			return -1;
		}
		c->rx = p;
		c->rx_cap = cap;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* TCP callbacks                                                      */
/* ------------------------------------------------------------------ */

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct lwip_conn *c = (struct lwip_conn *)arg;

	if (p == NULL) {
		/* Remote side closed the connection. */
		c->peer_closed = 1;
		return ERR_OK;
	}

	if (err != ERR_OK) {
		pbuf_free(p);
		return err;
	}

	if (conn_rx_reserve(c, p->tot_len) != 0) {
		return ERR_MEM;
	}

	pbuf_copy_partial(p, c->rx + c->rx_len, p->tot_len, 0);
	c->rx_len += p->tot_len;
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
	struct lwip_conn *c = (struct lwip_conn *)arg;

	c->pcb = NULL;
	c->connecting = 0;
	c->connected = 0;
	c->peer_closed = 1;
	c->so_error = (err == ERR_ABRT) ? ECONNABORTED : ECONNRESET;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct lwip_conn *c = (struct lwip_conn *)arg;

	(void)pcb;
	c->connecting = 0;
	if (err == ERR_OK) {
		c->connected = 1;
	} else {
		c->so_error = ECONNREFUSED;
	}
	return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* UDP callback                                                       */
/* ------------------------------------------------------------------ */

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
			const ip_addr_t *addr, u16_t port)
{
	struct lwip_conn *c = (struct lwip_conn *)arg;

	(void)pcb;

	if (p == NULL) {
		return;
	}

	if (conn_rx_reserve(c, p->tot_len) == 0) {
		pbuf_copy_partial(p, c->rx + c->rx_len, p->tot_len, 0);
		c->rx_len += p->tot_len;
		c->udp_src = *addr;
		c->udp_src_port = port;
	}
	pbuf_free(p);
}

/* ------------------------------------------------------------------ */
/* Backend operations                                                 */
/* ------------------------------------------------------------------ */

static int nfs_net_ensure_init(void)
{
	static int inited;

	if (inited) {
		return 0;
	}
	if (nfs_lwip_port_init() != 0) {
		return -1;
	}
	inited = 1;
	return 0;
}

int nfs_net_socket(int domain, int type, int protocol)
{
	int i;

	if (nfs_net_ensure_init() != 0) {
		return -1;
	}

	(void)domain;
	(void)protocol;

	if (type != SOCK_STREAM && type != SOCK_DGRAM) {
		errno = ESOCKTNOSUPPORT;
		return -1;
	}

	for (i = 0; i < LWIP_MAX_CONN; i++) {
		if (!conns[i].in_use) {
			break;
		}
	}
	if (i == LWIP_MAX_CONN) {
		errno = EMFILE;
		return -1;
	}

	memset(&conns[i], 0, sizeof(conns[i]));
	conns[i].in_use = 1;
	conns[i].type = type;

	return LWIP_FD_BASE + i;
}

static void conn_destroy(struct lwip_conn *c)
{
	if (c->pcb != NULL) {
		tcp_arg(c->pcb, NULL);
		tcp_recv(c->pcb, NULL);
		tcp_err(c->pcb, NULL);
		tcp_abort(c->pcb);
		c->pcb = NULL;
	}
	if (c->upcb != NULL) {
		udp_remove(c->upcb);
		c->upcb = NULL;
	}
	free(c->rx);
	c->rx = NULL;
	c->in_use = 0;
}

static int sockaddr_to_ip(const struct sockaddr *addr, ip_addr_t *ip, u16_t *port)
{
	uint32_t a;

	if (addr->sa_family != AF_INET) {
		return -1;
	}
	a = ntohl(((const struct sockaddr_in *)addr)->sin_addr.s_addr);
	IP_ADDR4(ip, (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff,
		 a & 0xff);
	*port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
	return 0;
}

int nfs_net_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	struct lwip_conn *c = conn_get(fd);
	ip_addr_t ip;
	u16_t port;

	(void)addrlen;

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}
	if (sockaddr_to_ip(addr, &ip, &port) != 0) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	c->raddr = ip;
	c->rport = port;
	c->so_error = 0;

	if (c->type == SOCK_DGRAM) {
		if (c->upcb == NULL) {
			c->upcb = udp_new();
			if (c->upcb == NULL) {
				errno = ENOMEM;
				return -1;
			}
			udp_recv(c->upcb, udp_recv_cb, c);
		}
		if (udp_connect(c->upcb, &ip, port) != ERR_OK) {
			errno = EIO;
			return -1;
		}
		c->connected = 1;
		return 0;
	}

	if (c->pcb != NULL) {
		tcp_abort(c->pcb);
		c->pcb = NULL;
	}

	c->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
	if (c->pcb == NULL) {
		errno = ENOMEM;
		return -1;
	}
	tcp_arg(c->pcb, c);
	tcp_recv(c->pcb, tcp_recv_cb);
	tcp_err(c->pcb, tcp_err_cb);

	c->connecting = 1;
	c->connected = 0;
	c->peer_closed = 0;

	if (tcp_connect(c->pcb, &ip, port, tcp_connected_cb) != ERR_OK) {
		tcp_abort(c->pcb);
		c->pcb = NULL;
		c->connecting = 0;
		errno = ECONNREFUSED;
		return -1;
	}

	errno = EINPROGRESS;
	return -1;
}

int nfs_net_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	struct lwip_conn *c = conn_get(fd);
	ip_addr_t ip;
	u16_t port;

	(void)addrlen;

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}
	if (sockaddr_to_ip(addr, &ip, &port) != 0) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	if (c->type == SOCK_DGRAM) {
		if (c->upcb == NULL) {
			c->upcb = udp_new();
			if (c->upcb == NULL) {
				errno = ENOMEM;
				return -1;
			}
			udp_recv(c->upcb, udp_recv_cb, c);
		}
		if (udp_bind(c->upcb, IP_ANY_TYPE, port) != ERR_OK) {
			errno = EADDRINUSE;
			return -1;
		}
		return 0;
	}

	/* libnfs only ever binds UDP sockets. */
	errno = EOPNOTSUPP;
	return -1;
}

ssize_t nfs_net_send(int fd, const void *buf, size_t len, int flags)
{
	struct lwip_conn *c = conn_get(fd);

	(void)flags;

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}

	if (c->type == SOCK_DGRAM) {
		struct pbuf *p;
		err_t err;

		if (c->upcb == NULL || !c->connected) {
			errno = ENOTCONN;
			return -1;
		}
		if (len > 0xffff) {
			len = 0xffff;
		}
		p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
		if (p == NULL) {
			errno = ENOBUFS;
			return -1;
		}
		pbuf_take(p, buf, (u16_t)len);
		err = udp_send(c->upcb, p);
		pbuf_free(p);
		if (err != ERR_OK) {
			errno = EIO;
			return -1;
		}
		return (ssize_t)len;
	}

	if (c->pcb == NULL || !c->connected) {
		errno = c->so_error ? c->so_error : ENOTCONN;
		return -1;
	}

	{
		u16_t sndbuf = tcp_sndbuf(c->pcb);
		u16_t n;
		err_t err;

		if (sndbuf == 0) {
			errno = EAGAIN;
			return -1;
		}
		n = (len > sndbuf) ? sndbuf : (u16_t)len;
		err = tcp_write(c->pcb, buf, n, TCP_WRITE_FLAG_COPY);
		if (err == ERR_MEM) {
			errno = EAGAIN;
			return -1;
		}
		if (err != ERR_OK) {
			errno = EIO;
			return -1;
		}
		tcp_output(c->pcb);
		return (ssize_t)n;
	}
}

ssize_t nfs_net_recv(int fd, void *buf, size_t len, int flags)
{
	struct lwip_conn *c = conn_get(fd);
	size_t n;

	(void)flags;

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}
	if (c->rx_len == 0) {
		if (c->peer_closed) {
			return 0;
		}
		errno = EAGAIN;
		return -1;
	}

	n = (len > c->rx_len) ? c->rx_len : len;
	memcpy(buf, c->rx + c->rx_pos, n);
	c->rx_pos += n;
	c->rx_len -= n;
	if (c->rx_len == 0) {
		conn_rx_reset(c);
	}
	return (ssize_t)n;
}

ssize_t nfs_net_recvfrom(int fd, void *buf, size_t len, int flags,
			     struct sockaddr *src_addr, socklen_t *addrlen)
{
	struct lwip_conn *c = conn_get(fd);

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}
	if (c->rx_len == 0) {
		errno = EAGAIN;
		return -1;
	}

	if (src_addr != NULL && addrlen != NULL &&
	    *addrlen >= sizeof(struct sockaddr_in)) {
		struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;

		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_port = htons(c->udp_src_port);
		sin->sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(&c->udp_src));
		*addrlen = sizeof(*sin);
	}

	return nfs_net_recv(fd, buf, len, flags);
}

int nfs_net_close(int fd)
{
	struct lwip_conn *c = conn_get(fd);

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}
	conn_destroy(c);
	return 0;
}

int nfs_net_getsockopt(int fd, int level, int optname, void *optval,
			   socklen_t *optlen)
{
	struct lwip_conn *c = conn_get(fd);

	if (c == NULL) {
		errno = EBADF;
		return -1;
	}

	if (level != SOL_SOCKET) {
		return 0;
	}

	switch (optname) {
	case SO_TYPE:
		if (optlen != NULL && *optlen >= sizeof(int)) {
			*(int *)optval = c->type;
			*optlen = sizeof(int);
		}
		return 0;
	case SO_ERROR:
		if (optlen != NULL && *optlen >= sizeof(int)) {
			*(int *)optval = c->so_error;
			*optlen = sizeof(int);
		}
		c->so_error = 0;
		return 0;
	default:
		return 0;
	}
}

int nfs_net_setsockopt(int fd, int level, int optname, const void *optval,
			   socklen_t optlen)
{
	(void)fd;
	(void)level;
	(void)optname;
	(void)optval;
	(void)optlen;
	return 0;
}

int nfs_net_dup2(int oldfd, int newfd)
{
	struct lwip_conn *oldc;
	int oldidx;
	int newidx;

	if (oldfd == newfd) {
		return newfd;
	}

	oldc = conn_get(oldfd);
	if (oldc == NULL) {
		errno = EBADF;
		return -1;
	}
	if (newfd < LWIP_FD_BASE || newfd >= LWIP_FD_BASE + LWIP_MAX_CONN) {
		errno = EBADF;
		return -1;
	}

	oldidx = oldfd - LWIP_FD_BASE;
	newidx = newfd - LWIP_FD_BASE;

	if (conns[newidx].in_use) {
		conn_destroy(&conns[newidx]);
	}
	conns[newidx] = conns[oldidx];
	memset(&conns[oldidx], 0, sizeof(conns[oldidx]));
	return newfd;
}

int nfs_net_set_nonblocking(int fd)
{
	(void)fd;
	/* lwIP raw callbacks are inherently non-blocking. */
	return 0;
}

int nfs_net_getaddrinfo(const char *node, const char *service,
			    const struct addrinfo *hints, struct addrinfo **res)
{
	return getaddrinfo(node, service, hints, res);
}

void nfs_net_freeaddrinfo(struct addrinfo *res)
{
	freeaddrinfo(res);
}

/* ------------------------------------------------------------------ */
/* poll()                                                             */
/* ------------------------------------------------------------------ */

static short conn_revents(struct lwip_conn *c, short events)
{
	short r = 0;

	if (c == NULL) {
		return POLLNVAL;
	}

	if (c->type == SOCK_DGRAM) {
		if (events & POLLIN) {
			r |= POLLIN;
		}
		if (events & POLLOUT) {
			r |= POLLOUT;
		}
		return r;
	}

	if (events & POLLIN) {
		if (c->rx_len > 0 || c->peer_closed) {
			r |= POLLIN;
		}
	}
	if (events & POLLOUT) {
		if (c->connecting) {
			if (c->connected || c->so_error) {
				r |= POLLOUT;
			}
		} else if (c->connected && c->pcb != NULL &&
			   tcp_sndbuf(c->pcb) > 0) {
			r |= POLLOUT;
		}
	}
	if (c->so_error != 0 && !c->connecting) {
		r |= POLLERR;
	}
	return r;
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int nfs_net_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	int64_t start = now_ms();

	for (;;) {
		nfds_t i;
		int ready = 0;
		int to = -1;
		int lt;

		nfs_lwip_port_service();

		for (i = 0; i < nfds; i++) {
			fds[i].revents =
				conn_revents(conn_get(fds[i].fd), fds[i].events);
			if (fds[i].revents != 0) {
				ready++;
			}
		}
		if (ready > 0) {
			return ready;
		}

		if (timeout >= 0) {
			to = (int)(timeout - (now_ms() - start));
			if (to < 0) {
				to = 0;
			}
		}

		lt = nfs_lwip_port_timeout();
		if (lt >= 0 && (to < 0 || lt < to)) {
			to = lt;
		}

		if (to == 0) {
			if (timeout >= 0 && now_ms() - start >= timeout) {
				return 0;
			}
			continue;
		}

		if (nfs_lwip_port_wait(to) != 0) {
			return -1;
		}
	}
}
