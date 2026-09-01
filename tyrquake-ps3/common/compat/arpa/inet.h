#ifndef PS3_COMPAT_ARPA_INET_H
#define PS3_COMPAT_ARPA_INET_H

/*
 * Minimal stand-in for <arpa/inet.h>, which the ps3dev/PSL1GHT toolchain
 * doesn't provide (confirmed independently by dragonfly-quake-ps3's port,
 * which had to build its own networking stub for the same reason).
 *
 * We don't need real sockets here: net_dgrm.c (real UDP networking) is
 * deliberately excluded from this build -- see the Makefile comment next
 * to NQ_SRCS. The *only* remaining user of this header is net_main.c's
 * NET_AdrToString(), which calls ntohs() purely to pretty-print a port
 * number for console/debug output (loopback connections only, since
 * that's the only net driver we build).
 *
 * PS3 (PPU) is big-endian, and network byte order is *defined* as
 * big-endian -- so on this platform, ntohs/htons are true no-ops, not
 * approximations. This is the correct answer here, not a workaround.
 */
#include <stdint.h>

static inline uint16_t ntohs(uint16_t v) { return v; }
static inline uint16_t htons(uint16_t v) { return v; }
static inline uint32_t ntohl(uint32_t v) { return v; }
static inline uint32_t htonl(uint32_t v) { return v; }

#endif /* PS3_COMPAT_ARPA_INET_H */
