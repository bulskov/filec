#pragma once
/* filec/path — lexical manipulation of CANONICAL paths.  No syscalls, no
 * filesystem access, no platform: "/a/b/../c" normalises to "/a/c" even if
 * b is a symlink.  Use the fs_ functions when the real filesystem matters.
 *
 * The canonical form — one syntax on every platform:
 *
 *   - '/' is the only separator.
 *   - A path is absolute exactly when it starts with '/'; the only root is
 *     "/".  Everything else is relative.
 *   - Windows drives and shares are ordinary first components:
 *
 *         C:\Users\me          ->  /C:/Users/me
 *         \\server\share\x     ->  /UNC/server/share/x
 *
 *     so on Windows "/" is a virtual root whose children are the drives.
 *
 * Getting there is not this module's job: filec/native.h converts between
 * native and canonical paths, and fs_ functions take and return canonical
 * paths.  So this module never sees '\', "C:" roots or "\\?\" prefixes.
 *
 * NORMALISED paths have no empty components ("//"), no "." components, no
 * ".." except leading ones in a relative path, and no trailing '/' (except
 * the root "/").  The empty relative path is ".".  Everything filec hands
 * out is normalised; the functions below expect normalised input unless
 * they say otherwise (path_normalize and path_join accept anything).
 *
 * Two kinds of result:
 *
 *   - Anatomy functions return VIEWS into the input (or string literals):
 *     no allocation, valid as long as the input is.
 *   - Building functions APPEND a normalised path to out, a strbuf you own,
 *     like filec/native.h.  Create it once, strbuf_clear it between calls;
 *     nothing else is allocated.  Look at the result with strbuf_view(out),
 *     keep it with strbuf_to_string(out, alloc).
 *
 * Every building function returns a path_err_t, and:
 *   - on failure leaves out exactly as it was (no partial result);
 *   - treats what out already holds as text it must not touch — ".." in the
 *     result never reaches back into it;
 *   - needs input that does not point into out: an append may move out's
 *     buffer.  Build into a second strbuf instead;
 *   - returns PATH_INVALID for out == NULL. */

#include <stdbool.h>
#include <stddef.h>

#include "arena/allocator.h"
#include "seqc/iter.h"
#include "seqc/string.h"

typedef enum
{
    PATH_OK = 0,
    PATH_INVALID, /* out == NULL, or input the function cannot use       */
    PATH_OOM,     /* out's allocator returned NULL; out is unchanged     */
} path_err_t;

/* How components compare.  Paths themselves are platform-neutral; whether
 * "A" and "a" name the same file is not, so comparisons ask. */
typedef enum
{
    PATH_CASE_SENSITIVE,   /* byte for byte (Linux)                       */
    PATH_CASE_INSENSITIVE, /* ASCII letters folded (Windows, macOS default) */
} path_case_t;

#if defined(_WIN32) || defined(__APPLE__)
#define PATH_CASE_NATIVE PATH_CASE_INSENSITIVE
#else
#define PATH_CASE_NATIVE PATH_CASE_SENSITIVE
#endif

/* --- Anatomy (views) ----------------------------------------------------- */
/*
 *   path               parent      file_name   stem        extension
 *   "/a/b.tar.gz"      "/a"        "b.tar.gz"  "b.tar"     "gz"
 *   "/x/.bashrc"       "/x"        ".bashrc"   ".bashrc"   ""
 *   "/C:/Users"        "/C:"       "Users"     "Users"     ""
 *   "/C:"              "/"         "C:"        "C:"        ""
 *   "/"                "/"         ""          ""          ""
 *   "a/b"              "a"         "b"         "b"         ""
 *   "a"                "."         "a"         "a"         ""
 *   "."                "."         ""          ""          ""
 */

bool path_is_absolute(string_t p);

/* p without its last component.  The parent of "/" is "/"; of a single
 * relative component (and of ".") it is ".".  Purely lexical: the parent
 * of ".." is ".", even though the real parent is "../..". */
string_t path_parent(string_t p);

/* The last component; "" for "/" and ".". */
string_t path_file_name(string_t p);

/* file_name without its extension. */
string_t path_stem(string_t p);

/* What follows the last '.' of file_name, without the dot.  "" when there
 * is no dot, when the only dot is the first character (".bashrc"), or when
 * the name ends in a dot ("a."). */
string_t path_extension(string_t p);

/* Yields string_t: "/" first if p is absolute, then each component.
 * "." yields nothing.  path_join_iter(path_components(p)) gives p back. */
iter_t path_components(string_t p, allocator_t a);

/* --- Building (append to out, normalised results) ----------------------- */

/* The normalised form of any canonical path: empty and "." components
 * dropped, ".." resolved lexically.  ".." never climbs above "/"; a
 * relative path keeps its leading ".."s.  "" -> "."
 *   "/a//./b/../c/" -> "/a/c"      "a/../.." -> ".."      "/.." -> "/" */
path_err_t path_normalize(string_t p, strbuf_t *out);

/* a, then b.  An absolute b replaces a.  Accepts unnormalised input:
 *   join("/a", "b") = "/a/b"     join("/a/b", "../c") = "/a/c"
 *   join("/a", "/b") = "/b"      join("/a", "") = "/a" */
path_err_t path_join(string_t a, string_t b, strbuf_t *out);

/* Joins every string_t yielded by parts, left to right, as path_join.
 * Always consumes the iterator, also on failure.  No parts gives ".". */
path_err_t path_join_iter(iter_t parts, strbuf_t *out);

/* p with its extension replaced (or added, or with ext = "" removed).
 * PATH_INVALID when p has no file name to change ("/", "."). */
path_err_t path_with_extension(string_t p, string_t ext, strbuf_t *out);

/* The relative path that leads from base to target, both absolute:
 *   ("/a/b", "/a/c/d") -> "../c/d"     ("/a", "/a") -> "."
 * Every absolute path shares the root "/", so there is always an answer —
 * on Windows it may lead through the virtual root ("../../D:/x"), which has
 * no native form.  PATH_INVALID if base or target is relative. */
path_err_t path_relative(
    string_t base, string_t target, path_case_t cs, strbuf_t *out);

/* --- Comparing -------------------------------------------------------------
 */
/* Component by component, so "/foo" is not a prefix of "/foobar", and a
 * directory sorts directly before its contents: "/a" < "/a/b" < "/a-b". */

bool path_equals(string_t a, string_t b, path_case_t cs);
int path_compare(string_t a, string_t b, path_case_t cs);
bool path_starts_with(string_t p, string_t prefix, path_case_t cs);

/* Consistent with path_equals for the same cs: equal paths hash equal. */
size_t path_hash(string_t p, path_case_t cs);
