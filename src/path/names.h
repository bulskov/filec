#pragma once
/* Internal to filec: the in-place normaliser shared by path.c and native.c.
 *
 * Names are appended to out one at a time and normalised on the way: empty
 * names and "." are dropped, ".." truncates out back to the previous '/'.
 * floor is the length of out that must never be touched — what the caller
 * already had, plus a root such as "/C:".  Nothing is allocated except
 * out's own growth. */

#include <stdbool.h>
#include <stddef.h>

#include "seqc/string.h"

/* Appends the names of src, split at '/' (and at '\' too with
 * backslash_sep), normalised.  With absolute every name is written as
 * "/name" and ".." stops at the floor; without, names are joined by '/' and
 * leading ".." stay.  SEQC_OOM leaves out partly written: the caller
 * truncates back to its own mark. */
seqc_status_t filec_append_names(
    strbuf_t *out,
    string_t src,
    bool backslash_sep,
    size_t floor,
    bool absolute);

/* The final touch when no names were left after floor: "/" for an
 * absolute path, "." for a relative one.  Otherwise nothing. */
seqc_status_t filec_finish_names(strbuf_t *out, size_t floor, bool absolute);
