/*
 * lwIP compiler/platform abstraction for the libnfs user-space backend.
 *
 * This is a trimmed down version of the lwIP unix port arch/cc.h that does
 * not depend on the pthread based sys_arch: the user-space backend runs lwIP
 * in NO_SYS mode inside libnfs' own single threaded event loop.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define LWIP_TIMEVAL_PRIVATE 0

#define LWIP_ERRNO_INCLUDE <errno.h>
#define LWIP_ERRNO_STDINCLUDE 1

#define LWIP_PLATFORM_ASSERT(x) do { \
	fprintf(stderr, "Assertion \"%s\" failed at line %d in %s\n", \
		x, __LINE__, __FILE__); \
	fflush(stderr); \
	abort(); \
} while (0)

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)

unsigned int lwip_port_rand(void);
#define LWIP_RAND() (lwip_port_rand())

#define LWIP_NO_UNISTD_H 0

#endif /* LWIP_ARCH_CC_H */
