/*
 * msvc_compat.h - POSIX/GCC compatibility shims for building VICE with MSVC.
 *
 * Included at the end of the MSVC config.h, so it is seen by every
 * translation unit that includes vice.h. Only active under _MSC_VER.
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 */

#ifndef VICE_MSVC_COMPAT_H
#define VICE_MSVC_COMPAT_H

#ifdef _MSC_VER

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <basetsd.h>

/* Pull in the Windows headers up front. SDL (via vice_sdl.h) and others
 * include <windows.h> late, after VICE sources have defined short macros
 * such as `opcode_t` or `p0` that clash with identifiers in the Windows SDK
 * headers. MinGW's headers happen not to trip over these; MSVC's do. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define VICE_MSVC_UNDEF_LEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef VICE_MSVC_UNDEF_LEAN
#undef WIN32_LEAN_AND_MEAN
#undef VICE_MSVC_UNDEF_LEAN
#endif
/* rpcndr.h: VICE uses `small` as an identifier */
#ifdef small
#undef small
#endif

/* GCC attributes: drop them */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* ssize_t */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

/* strings.h */
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* sys/stat.h mode tests (spelled exactly like arch/shared/archdep_stat.h so
 * the redefinition there is benign) */
#ifndef S_ISDIR
#define S_ISDIR(_m)  (((_m) & S_IFMT) == _S_IFDIR)
#define S_ISFIFO(_m) (((_m) & S_IFMT) == _S_IFIFO)
#define S_ISCHR(_m)  (((_m) & S_IFMT) == _S_IFCHR)
#define S_ISBLK(_m)  (((_m) & S_IFMT) == _S_IFBLK)
#define S_ISREG(_m)  (((_m) & S_IFMT) == _S_IFREG)
#endif

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

#endif /* _MSC_VER */

#endif /* VICE_MSVC_COMPAT_H */
