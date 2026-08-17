/* config.h - hand-written for Nintendo Switch (libnx + newlib).
 * espeak-ng normally generates this via autoconf; the Switch port builds the
 * library directly with clang/ld.lld, so this file pins the platform features.
 */
#ifndef ESPEAK_NG_CONFIG_H
#define ESPEAK_NG_CONFIG_H

/* No fork/vfork on Switch (single-process homebrew). */
/* #undef HAVE_FORK */
/* #undef HAVE_VFORK */
/* #undef HAVE_WORKING_FORK */
/* #undef HAVE_WORKING_VFORK */

/* No dynamic loading. */
/* #undef HAVE_DLFCN_H */

/* Standard newlib headers available on Switch. */
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LOCALE_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1

/* Functions provided by newlib. */
#define HAVE_DUP2 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_MALLOC 1
#define HAVE_MEMCHR 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_MKDIR 1
#define HAVE_MKSTEMP 1
#define HAVE_POW 1
#define HAVE_REALLOC 1
#define HAVE_SETLOCALE 1
#define HAVE_SQRT 1
#define HAVE_STRCHR 1
#define HAVE_STRCOLL 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STDC_HEADERS 1

/* getopt is only used by the CLI binary, which the Switch build does not
 * compile; leave it undefined to avoid pulling in newlib's getopt. */
/* #undef HAVE_GETOPT_H */
/* #undef HAVE_GETOPT_LONG */

/* Optional libraries are all disabled. */
/* #undef HAVE_LIBSONIC */
/* #undef HAVE_SONIC_H */
/* #undef HAVE_PCAUDIOLIB_AUDIO_H */
/* #undef HAVE_VALGRIND_MEMCHECK_H */

/* Endianness headers handled by the compat/ shim when newlib lacks them. */
/* #undef HAVE_ENDIAN_H */
/* #undef HAVE_SYS_ENDIAN_H */

#define STDC_HEADERS 1

#define PACKAGE "espeak-ng"
#define PACKAGE_BUGREPORT "https://github.com/espeak-ng/espeak-ng/issues"
#define PACKAGE_NAME "eSpeak NG"
#define PACKAGE_STRING "eSpeak NG 1.52.0"
#define PACKAGE_TARNAME "espeak-ng"
#define PACKAGE_URL "https://github.com/espeak-ng/espeak-ng"
#define PACKAGE_VERSION "1.52.0"
#define VERSION "1.52.0"

/* Default data directory; the app also calls espeak_ng_InitializePath() with
 * the romfs path, so this is only a fallback. */
#define PATH_ESPEAK_DATA "romfs:/espeak-ng-data"

#endif /* ESPEAK_NG_CONFIG_H */
