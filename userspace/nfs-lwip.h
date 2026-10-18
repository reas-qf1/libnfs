/*
 * Internal interface between the lwIP port (TUN + io_uring driver) and the
 * libnfs network backend implementation.
 */

#ifndef _NFS_LWIP_H_
#define _NFS_LWIP_H_

struct netif;

/* Bring up lwIP, open the TUN device and initialize io_uring. Returns 0 on
 * success. Sets errno on failure. */
int nfs_lwip_port_init(void);

/* Tear everything down. */
void nfs_lwip_port_fini(void);

/* Process any pending io_uring completions and run lwIP timers. Never blocks. */
void nfs_lwip_port_service(void);

/* Number of milliseconds until the next lwIP timer is due. -1 means no timer
 * is pending. */
int nfs_lwip_port_timeout(void);

/* Block for up to timeout_ms (-1 == indefinitely) waiting for a packet from
 * the TUN device, then process completions and timers. Returns 0 on success. */
int nfs_lwip_port_wait(int timeout_ms);

/* The lwIP network interface, valid after nfs_lwip_port_init(). */
struct netif *nfs_lwip_port_netif(void);

#endif /* !_NFS_LWIP_H_ */
