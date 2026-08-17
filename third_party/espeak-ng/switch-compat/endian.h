/* Switch (aarch64) endian shim. newlib has no <endian.h>/<sys/endian.h>;
 * espeak-ng only reads little-endian binary data here, so provide the
 * le*toh/htole* identities plus bswap-based big-endian variants.
 */
#ifndef SWITCH_ENDIAN_COMPAT_H
#define SWITCH_ENDIAN_COMPAT_H

#define __BYTE_ORDER 1234
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define BYTE_ORDER __LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN

#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) (x)
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) (x)

#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) (x)
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) (x)

#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) (x)
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) (x)

#endif /* SWITCH_ENDIAN_COMPAT_H */
