/*
   Copyright (C) 2010 by Ronnie Sahlberg <ronniesahlberg@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Network backend for libnfs.
 *
 * libnfs only ever needs a small set of BSD-socket operations. Historically
 * these were issued directly from lib/socket.c. This header exports those
 * operations under the nfs_net_ prefix; here they are implemented on top of a
 * user-space TCP/IP stack (lwIP over a TUN device, driven by io_uring).
 */

#ifndef _NFS_NET_H_
#define _NFS_NET_H_

#include <sys/types.h>

#ifndef WIN32
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#else
#include "win32_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The stack is brought up lazily on the first nfs_net_socket() call.
 */

int nfs_net_socket(int domain, int type, int protocol);
int nfs_net_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int nfs_net_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t nfs_net_send(int fd, const void *buf, size_t len, int flags);
ssize_t nfs_net_recv(int fd, void *buf, size_t len, int flags);
ssize_t nfs_net_recvfrom(int fd, void *buf, size_t len, int flags,
			 struct sockaddr *src_addr, socklen_t *addrlen);
int nfs_net_close(int fd);
int nfs_net_getsockopt(int fd, int level, int optname, void *optval,
		       socklen_t *optlen);
int nfs_net_setsockopt(int fd, int level, int optname, const void *optval,
		       socklen_t optlen);
int nfs_net_dup2(int oldfd, int newfd);
int nfs_net_set_nonblocking(int fd);
int nfs_net_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int nfs_net_getaddrinfo(const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res);
void nfs_net_freeaddrinfo(struct addrinfo *res);

#ifdef __cplusplus
}
#endif

#endif /* !_NFS_NET_H_ */
