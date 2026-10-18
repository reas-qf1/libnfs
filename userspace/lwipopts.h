/*
 * lwIP configuration for the libnfs user-space TCP/IP backend.
 *
 * lwIP runs in NO_SYS mode (no threads, no sys_arch) and is driven entirely
 * from libnfs' event loop. Raw TCP/UDP packets are exchanged with the kernel
 * through a TUN device using io_uring.
 */

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* No OS: single threaded, no mboxes/semaphores. */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0

/* Protocols */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        0
#define LWIP_ETHERNET                   0
#define LWIP_ICMP                       1
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_SNMP                       0
#define LWIP_IGMP                       0
#define LWIP_STATS                      0
#define LWIP_TIMERS                     1

/* We implement our own TCP client backend; the sequential/socket APIs are
 * not needed. */
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0

/* Memory: use the C library allocator. */
#define MEM_LIBC_MALLOC                 1
#define MEMP_MEM_MALLOC                 1
#define MEM_USE_POOLS                   0
#define MEM_ALIGNMENT                   8
#define MEMP_NUM_PBUF                   64
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                32
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                128
#define MEMP_NUM_REASSDATA              4
#define MEMP_NUM_SYS_TIMEOUT            16
#define PBUF_POOL_SIZE                  128
#define PBUF_POOL_BUFSIZE               1600

/* TCP tuning. NFS does large (up to 1 MiB) reads/writes. */
#define TCP_MSS                         1460
#define TCP_WND                         (32 * TCP_MSS)
#define TCP_SND_BUF                     (32 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_LISTEN_BACKLOG              0
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1

/* Only hand single, complete packets to the netif output function. */
#define LWIP_NETIF_TX_SINGLE_PBUF       1

/* Reassemble incoming fragments; fragment our own if needed. */
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAXAGE                 3
#define IP_REASS_MAX_PBUFS              16

/* Software checksums are mandatory since we speak raw IP. */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_UDP              1

#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_REMOVE_CALLBACK      0

/* No loopback interface, no hostname plumbing. */
#define LWIP_NETIF_LOOPBACK             0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_NETIF_HOSTNAME             0

/* We do not need the lwIP assert machinery to pull in extra hooks. */
#define LWIP_NOASSERT_ON_ERROR          1

#endif /* LWIP_LWIPOPTS_H */
