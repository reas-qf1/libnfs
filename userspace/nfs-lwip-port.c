/*
 * libnfs user-space network backend: lwIP port driven by io_uring.
 *
 * The kernel is only used as a rudimentary "wire": a TUN device carries raw
 * IPv4 packets between the kernel routing table and this process. All TCP/IP
 * work is done in user space by lwIP, which runs single threaded (NO_SYS) as
 * part of libnfs' event loop.
 *
 * io_uring is used for all I/O on the TUN device:
 *   - a pool of reads is kept outstanding; each completion is turned into a
 *     pbuf and handed to lwIP,
 *   - outgoing packets are copied into a pool of buffers and written back
 *     asynchronously, preserving order.
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
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <liburing.h>

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/mem.h"

#include "nfs-lwip.h"

#define NFS_LWIP_TUN_IFACE	"tun0"
#define NFS_LWIP_IP		"10.0.0.2"
#define NFS_LWIP_NETMASK	"255.255.255.0"
#define NFS_LWIP_GW		"10.0.0.1"

#define RX_SLOTS		16
#define RX_SLOT_SZ		2048
#define TX_SLOTS		48
#define TX_SLOT_SZ		2048

#define RING_ENTRIES		256

#define UD_KIND_RX		1u
#define UD_KIND_TX		2u
#define UD_MAKE(kind, idx)	(((uint64_t)(kind) << 32) | (uint32_t)(idx))
#define UD_KIND(ud)		((uint32_t)((ud) >> 32))
#define UD_IDX(ud)		((uint32_t)((ud) & 0xffffffffu))

static int tun_fd = -1;
static struct io_uring ring;
static int ring_ready;
static struct netif us_netif;
static int port_ready;

static unsigned char rx_buf[RX_SLOTS][RX_SLOT_SZ];
static int rx_armed[RX_SLOTS];

static unsigned char tx_buf[TX_SLOTS][TX_SLOT_SZ];
static int tx_len[TX_SLOTS];
static int tx_busy[TX_SLOTS];
static unsigned int tx_queue[TX_SLOTS];
static int tx_qhead;
static int tx_qtail;
static int tx_qcount;
static int tx_inflight = -1;

static int lwip_debug;

/* ------------------------------------------------------------------ */
/* lwIP platform hooks                                                */
/* ------------------------------------------------------------------ */

unsigned int lwip_port_rand(void)
{
	return (unsigned int)rand();
}

u32_t sys_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u32_t)((u32_t)ts.tv_sec * 1000u + (u32_t)(ts.tv_nsec / 1000000));
}

/* ------------------------------------------------------------------ */
/* TUN driver                                                         */
/* ------------------------------------------------------------------ */

static int open_tun(const char *ifname)
{
	struct ifreq ifr;
	int fd;

	fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
	if (fd == -1) {
		fprintf(stderr, "libnfs-lwip: cannot open /dev/net/tun: %s\n",
			strerror(errno));
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
		fprintf(stderr, "libnfs-lwip: TUNSETIFF(%s) failed: %s\n",
			ifname, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

static void submit_next_tx(void);

/*
 * SQEs are only prepared (not submitted) as packets flow; a single
 * io_uring_submit() is issued at the end of each completion-processing round so
 * one syscall drains a whole batch of re-armed reads and queued writes.
 */
static void submit_pending(void)
{
	if (io_uring_sq_ready(&ring) > 0) {
		io_uring_submit(&ring);
	}
}

static void submit_rx(int idx)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&ring);
	if (sqe == NULL) {
		return;
	}
	io_uring_prep_read(sqe, tun_fd, rx_buf[idx], RX_SLOT_SZ, 0);
	io_uring_sqe_set_data64(sqe, UD_MAKE(UD_KIND_RX, idx));
	rx_armed[idx] = 1;
}

static err_t low_level_output(struct netif *netif, struct pbuf *p,
			      const ip4_addr_t *ipaddr)
{
	int i;

	(void)netif;
	(void)ipaddr;

	if (p->tot_len > TX_SLOT_SZ) {
		return ERR_MEM;
	}

	for (i = 0; i < TX_SLOTS; i++) {
		if (!tx_busy[i]) {
			break;
		}
	}
	if (i == TX_SLOTS) {
		return ERR_MEM;
	}

	pbuf_copy_partial(p, tx_buf[i], p->tot_len, 0);
	tx_len[i] = p->tot_len;
	tx_busy[i] = 1;

	tx_queue[tx_qtail] = (unsigned int)i;
	tx_qtail = (tx_qtail + 1) % TX_SLOTS;
	tx_qcount++;

	submit_next_tx();
	return ERR_OK;
}

static void submit_next_tx(void)
{
	struct io_uring_sqe *sqe;
	unsigned int idx;

	if (tx_inflight >= 0 || tx_qcount == 0) {
		return;
	}

	sqe = io_uring_get_sqe(&ring);
	if (sqe == NULL) {
		return;
	}

	idx = tx_queue[tx_qhead];
	tx_qhead = (tx_qhead + 1) % TX_SLOTS;
	tx_qcount--;

	io_uring_prep_write(sqe, tun_fd, tx_buf[idx], tx_len[idx], 0);
	io_uring_sqe_set_data64(sqe, UD_MAKE(UD_KIND_TX, idx));
	tx_inflight = (int)idx;
}

static err_t tun_netif_init(struct netif *netif)
{
	netif->name[0] = 'l';
	netif->name[1] = 'w';
	netif->mtu = 1500;
	netif->output = low_level_output;
	netif->flags = NETIF_FLAG_LINK_UP;
	return ERR_OK;
}

/* ------------------------------------------------------------------ */
/* io_uring completion processing                                     */
/* ------------------------------------------------------------------ */

static void handle_rx_cqe(int idx, int res)
{
	struct pbuf *p;

	rx_armed[idx] = 0;

	if (res > 0) {
		p = pbuf_alloc(PBUF_RAW, (u16_t)res, PBUF_POOL);
		if (p != NULL) {
			pbuf_take(p, rx_buf[idx], (u16_t)res);
			if (us_netif.input(p, &us_netif) != ERR_OK) {
				pbuf_free(p);
			}
		}
	} else if (res < 0 &&
		   res != -EAGAIN && res != -ENOBUFS && res != -EINTR) {
		if (lwip_debug) {
			fprintf(stderr, "libnfs-lwip: tun read error %d\n", res);
		}
	}

	if (!rx_armed[idx]) {
		submit_rx(idx);
	}
}

static void process_completions(void)
{
	for (;;) {
		struct io_uring_cqe *cqe;
		uint64_t ud;
		int ret;

		ret = io_uring_peek_cqe(&ring, &cqe);
		if (ret != 0) {
			break;
		}

		ud = io_uring_cqe_get_data64(cqe);
		if (UD_KIND(ud) == UD_KIND_RX) {
			handle_rx_cqe((int)UD_IDX(ud), cqe->res);
		} else {
			unsigned int idx = UD_IDX(ud);

			tx_busy[idx] = 0;
			tx_inflight = -1;
			submit_next_tx();
		}
		io_uring_cqe_seen(&ring, cqe);
	}

	/*
	 * Outgoing packets (ACKs in particular) are sent right away. Re-armed
	 * reads, on the other hand, are left for the next nfs_lwip_port_wait(),
	 * where io_uring_wait_cqe() submits and waits in a single syscall.
	 */
	if (tx_inflight >= 0) {
		submit_pending();
	}
}

/* ------------------------------------------------------------------ */
/* Public port interface                                              */
/* ------------------------------------------------------------------ */

static void ip_from_env(const char *name, const char *dflt, ip4_addr_t *out)
{
	const char *s = getenv(name);

	if (s == NULL || s[0] == '\0') {
		s = dflt;
	}
	ip4addr_aton(s, out);
}

int nfs_lwip_port_init(void)
{
	const char *ifname;
	ip4_addr_t ip, netmask, gw;
	int i;

	if (port_ready) {
		return 0;
	}

	lwip_debug = getenv("LIBNFS_LWIP_DEBUG") != NULL;

	ifname = getenv("LIBNFS_TUN");
	if (ifname == NULL || ifname[0] == '\0') {
		ifname = NFS_LWIP_TUN_IFACE;
	}

	tun_fd = open_tun(ifname);
	if (tun_fd == -1) {
		return -1;
	}

	if (io_uring_queue_init(RING_ENTRIES, &ring, 0) < 0) {
		fprintf(stderr, "libnfs-lwip: io_uring_queue_init failed: %s\n",
			strerror(errno));
		close(tun_fd);
		tun_fd = -1;
		return -1;
	}
	ring_ready = 1;

	lwip_init();

	ip_from_env("LIBNFS_LWIP_IP", NFS_LWIP_IP, &ip);
	ip_from_env("LIBNFS_LWIP_NETMASK", NFS_LWIP_NETMASK, &netmask);
	ip_from_env("LIBNFS_LWIP_GW", NFS_LWIP_GW, &gw);

	if (netif_add(&us_netif, &ip, &netmask, &gw, NULL, tun_netif_init,
		      ip_input) == NULL) {
		fprintf(stderr, "libnfs-lwip: netif_add failed\n");
		return -1;
	}
	netif_set_default(&us_netif);
	netif_set_up(&us_netif);
	netif_set_link_up(&us_netif);

	for (i = 0; i < RX_SLOTS; i++) {
		tx_busy[i] = 0;
		rx_armed[i] = 0;
		submit_rx(i);
	}
	submit_pending();

	port_ready = 1;

	if (lwip_debug) {
		fprintf(stderr, "libnfs-lwip: up on %s, ip %s\n", ifname,
			ip4addr_ntoa(&ip));
	}
	return 0;
}

void nfs_lwip_port_fini(void)
{
	if (!port_ready) {
		return;
	}
	if (ring_ready) {
		io_uring_queue_exit(&ring);
		ring_ready = 0;
	}
	if (tun_fd != -1) {
		close(tun_fd);
		tun_fd = -1;
	}
	port_ready = 0;
}

void nfs_lwip_port_service(void)
{
	if (!port_ready) {
		return;
	}
	process_completions();
	sys_check_timeouts();
}

int nfs_lwip_port_timeout(void)
{
	u32_t t;

	if (!port_ready) {
		return -1;
	}
	t = sys_timeouts_sleeptime();
	if (t == SYS_TIMEOUTS_SLEEPTIME_INFINITE) {
		return -1;
	}
	return (int)t;
}

int nfs_lwip_port_wait(int timeout_ms)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	int ret;

	if (!port_ready) {
		return -1;
	}

	if (timeout_ms < 0) {
		ret = io_uring_wait_cqe(&ring, &cqe);
	} else {
		ts.tv_sec = timeout_ms / 1000;
		ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
		ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
	}

	(void)cqe;

	if (ret < 0 && ret != -ETIME && ret != -EINTR) {
		return -1;
	}

	process_completions();
	sys_check_timeouts();
	return 0;
}

struct netif *nfs_lwip_port_netif(void)
{
	return &us_netif;
}
