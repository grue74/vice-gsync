/*
 * unistd.h - minimal stand-in for MSVC builds of VICE.
 *
 * Only on the include path for MSVC builds. Provides the handful of POSIX
 * declarations VICE's Windows code paths expect from MinGW's unistd.h.
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 */

#ifndef VICE_MSVC_UNISTD_H
#define VICE_MSVC_UNISTD_H

#include <io.h>
#include <process.h>
#include <direct.h>
#include <stdlib.h>

#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 0
#define F_OK 0
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#endif
