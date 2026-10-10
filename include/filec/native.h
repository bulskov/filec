#pragma once
/* filec/native — conversion between NATIVE paths (what the OS and the user
 * write) and CANONICAL paths (filec/path.h).  All platform path syntax lives
 * here and nowhere else.
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
 * directory are refused: on Windows "C:x" and "C:" (relative to drive C's
 * current directory), and "\x" or "/x" (the root of the current drive).
 * fs_from_native (filec/fs.h) resolves those against the real current
 * directory.
 *
 * The rules in detail (tests/native_test.c checks each):
 *   - The result of path_from_native is normalised.  On Windows ".." never
 *     leaves a drive or share: C:\..\x is /C:/x, \\srv\share\..\x is
 *     /UNC/srv/share/x — unlike path_normalize, which would climb to the
 *     virtual root.
 *   - POSIX: '\' and ':' are ordinary characters in names.
 *   - Windows: only the drive letter is upper-cased; server, share and
 *     other names keep their case.  "\\srv" without a share, and the
 *     device namespace "\\.\..." are PATH_NATIVE_INVALID.
 *   - A NUL byte anywhere is PATH_NATIVE_INVALID in either style.
 *   - "" is ".".
 *   - path_to_native writes a drive root as "C:\" (with the backslash —
 *     "C:" alone would be drive-relative).
 *   - PATH_NATIVE_OOM: out could not grow; out is unchanged. */

#include <stdbool.h>
#include <stddef.h>

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
    PATH_NATIVE_OOM,            /* out's allocator returned NULL        */
} path_native_err_t;

/* Both functions APPEND their result to out, a strbuf you own: create it
 * once, strbuf_clear it between calls, and nothing else is allocated —
 * after the first few calls not even that.  Look at the result with
 * strbuf_view(out); keep it with strbuf_to_string(out, alloc).
 *
 *   - On success they return PATH_NATIVE_OK.
 *   - On failure out is left exactly as it was (no partial result).
 *   - The input must not point into out: an append may move out's buffer.
 *     Copy it first, or convert into a second strbuf.
 *   - out == NULL is PATH_NATIVE_INVALID. */

/* Native -> canonical, normalised (see filec/path.h).  Windows: '/' and '\'
 * are both separators; the drive letter is upper-cased ("c:" -> "/C:"). */
path_native_err_t path_from_native(
    string_t native, path_style_t style, strbuf_t *out);

/* Canonical -> native, for the OS or for showing to a user: '\' and
 * "C:\" on Windows, unchanged on POSIX.  Windows refuses "/" and paths that
 * go through it (PATH_NATIVE_NO_NATIVE_FORM).  The "\\?\" long-path prefix
 * the OS needs is added by the fs backend, not here.  An empty canonical
 * path is PATH_NATIVE_INVALID. */
path_native_err_t path_to_native(
    string_t canonical, path_style_t style, strbuf_t *out);
