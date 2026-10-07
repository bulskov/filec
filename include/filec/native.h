#pragma once
/* filec/native — conversion between NATIVE paths (what the OS and the user
 * write) and CANONICAL paths (filec/path.h).  All platform path syntax lives
 * here and nowhere else.
 *
 * STATUS: DRAFT INTERFACE — not implemented yet.  See docs/interface.md.
 *
 * Pure string functions, no syscalls: every style is converted and tested
 * on every platform (Windows rules in Linux CI and vice versa).  Use
 * PATH_STYLE_NATIVE in application code.
 *
 *   style    native                        canonical
 *   POSIX    /home/me/x                    /home/me/x
 *   WINDOWS  C:\Users\me  or  C:/Users/me  /C:/Users/me
 *   WINDOWS  \\server\share\x              /UNC/server/share/x
 *   WINDOWS  \\?\C:\very\long\path         /C:/very/long/path
 *   WINDOWS  \\?\UNC\server\share\x        /UNC/server/share/x
 *   both     a/b  (relative)               a/b
 *
 * Native forms that cannot be converted without knowing the current
 * directory are refused: on Windows "C:x" (relative to drive C's current
 * directory) and "\x" (the root of the current drive).  fs_from_native
 * (filec/fs.h) resolves those against the real current directory. */

#include <stdbool.h>
#include <stddef.h>

#include "arena/allocator.h"
#include "seqc/string.h"

typedef enum
{
    PATH_STYLE_POSIX,
    PATH_STYLE_WINDOWS,
} path_style_t;

#ifdef _WIN32
#define PATH_STYLE_NATIVE PATH_STYLE_WINDOWS
#else
#define PATH_STYLE_NATIVE PATH_STYLE_POSIX
#endif

typedef enum
{
    PATH_NATIVE_OK = 0,
    PATH_NATIVE_DRIVE_RELATIVE, /* "C:x": needs drive C's current dir  */
    PATH_NATIVE_ROOT_RELATIVE,  /* "\x": needs the current drive       */
    PATH_NATIVE_NO_NATIVE_FORM, /* to_native: the virtual root "/" on
                                   Windows, or a path through it        */
    PATH_NATIVE_INVALID,        /* malformed: "\\server" without share,
                                   a NUL byte, ...                      */
    PATH_NATIVE_OOM,            /* the allocator returned NULL          */
} path_native_err_t;

/* Native -> canonical, normalised (see filec/path.h).  Returns {NULL, 0} and
 * sets *err on failure; err may be NULL.  Windows: '/' and '\' are both
 * separators; the drive letter is upper-cased ("c:" -> "/C:"). */
string_t path_from_native(
    string_t native,
    path_style_t style,
    allocator_t alloc,
    path_native_err_t *err);

/* Canonical -> native, for the OS or for showing to a user: '\' and
 * "C:\" on Windows, unchanged on POSIX.  Windows refuses "/" and paths that
 * go through it (PATH_NATIVE_NO_NATIVE_FORM).  The "\\?\" long-path prefix
 * the OS needs is added by the fs backend, not here. */
string_t path_to_native(
    string_t canonical,
    path_style_t style,
    allocator_t alloc,
    path_native_err_t *err);
