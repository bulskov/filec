#pragma once
/* filec/path — lexical path manipulation.  No syscalls, no filesystem access:
 * "/a/b/../c" normalises to "/a/c" even if b is a symlink.  Use fs_ functions
 * when the real filesystem matters.
 *
 * STATUS: DRAFT INTERFACE — not implemented yet.  See docs/interface.md.
 *
 * Canonical form: '/' separators on every platform.  Windows-style input
 * ('\\', "C:\\x", "\\\\server\\share", "\\\\?\\C:\\x") is accepted under
 * PATH_WINDOWS and comes out with '/'.  Convert to the native look only for
 * display (path_to_display) — fs_ functions convert at the OS boundary.
 *
 * Every path function takes a style, so Windows rules are testable on Linux
 * and vice versa.  Use PATH_NATIVE in application code.
 *
 * Results marked "view" point into the input: no allocation, valid as long
 * as the input is.  Allocating functions return {NULL, 0} on OOM. */

#include <stdbool.h>
#include <stddef.h>

#include "arena/allocator.h"
#include "seqc/iter.h"
#include "seqc/string.h"

typedef enum
{
    PATH_POSIX,   /* '/' only, case-sensitive, root is "/"                 */
    PATH_WINDOWS, /* '/' and '\\', ASCII case-insensitive, drive/UNC roots */
} path_style_t;

#ifdef _WIN32
#define PATH_NATIVE PATH_WINDOWS
#else
#define PATH_NATIVE PATH_POSIX
#endif

/* --- Anatomy (views) ----------------------------------------------------- */
/*
 *   path             root            parent          file_name  stem     ext
 *   "/a/b.tar.gz"    "/"             "/a"            "b.tar.gz" "b.tar"  "gz"
 *   "C:/x/.bashrc"   "C:/"           "C:/x"          ".bashrc"  ".bashrc" ""
 *   "//srv/share/f"  "//srv/share/"  "//srv/share/"  "f"        "f"      ""
 *   "a"              ""              ""              "a"        "a"      ""
 *   "/"              "/"             "/"             ""         ""       ""
 */

string_t path_root(string_t p, path_style_t style); /* "" if relative */
bool path_is_absolute(string_t p, path_style_t style);
/* Parent of a root is the root. */
string_t path_parent(string_t p, path_style_t style);
/* Last component, "" for a root. */
string_t path_file_name(string_t p, path_style_t style);
string_t path_stem(string_t p, path_style_t style);
/* Without the dot. */
string_t path_extension(string_t p, path_style_t style);

/* Yields string_t: the root first (if any), then each component.
 * Empty components and "." are skipped; ".." is yielded as-is. */
iter_t path_components(string_t p, path_style_t style, allocator_t a);

/* --- Building (allocating) ----------------------------------------------- */

/* join("/a", "b") = "/a/b";  join("/a", "/b") = "/b" (absolute b wins). */
string_t path_join(
    string_t a, string_t b, path_style_t style, allocator_t alloc);

/* Joins every string_t yielded by parts.  Consumes the iterator. */
string_t path_join_iter(iter_t parts, path_style_t style, allocator_t alloc);

/* Canonical separators, no empty or "." components, ".." resolved
 * lexically.  ".." never climbs above a root; leading ".." of a relative
 * path is kept.  Trailing separator removed except on a root.  "" -> "." */
string_t path_normalize(string_t p, path_style_t style, allocator_t alloc);

/* Replace (or add, or with ext = "" remove) the extension. */
string_t path_with_extension(
    string_t p, string_t ext, path_style_t style, allocator_t alloc);

/* Relative path from base to target, both absolute and normalised, e.g.
 * ("/a/b", "/a/c/d") -> "../c/d".  Returns {NULL, 0} if the roots differ. */
string_t path_relative(
    string_t base, string_t target, path_style_t style, allocator_t alloc);

/* Native look for showing to a user: '\\' on Windows, unchanged on POSIX. */
string_t path_to_display(string_t p, path_style_t style, allocator_t alloc);

/* --- Comparing ----------------------------------------------------------- */
/* Component-wise on normalised input: "/foo" is not a prefix of "/foobar".
 * PATH_WINDOWS folds ASCII case only (NTFS's full upcase table is not
 * modelled; neither is macOS Unicode normalisation). */

bool path_equals(string_t a, string_t b, path_style_t style);
int path_compare(string_t a, string_t b, path_style_t style);
bool path_starts_with(string_t p, string_t prefix, path_style_t style);
/* Consistent with path_equals: equal paths hash equal. */
size_t path_hash(string_t p, path_style_t style);
