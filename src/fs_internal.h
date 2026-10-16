#pragma once

#include "filec/fs.h"
#include "seqc/string.h"

#define FILEC_PATH_BUF_MAX (FS_PATH_MAX + 1)

// shared helper functions
fs_err_t filec_path_cstr(string_t path, char *buf, size_t buf_size);
/* --- Reading directories: what each backend implements -------------------
 * fs_dir_list and fs_dir_iter (fs_common.c) are written once, on top of
 * these. */

typedef struct filec_dir_t filec_dir_t;

/* Opens path for reading; d comes from a (one allocation) and
 * filec_dir_close gives it back.  On failure *out is NULL. */
fs_err_t filec_dir_open(
    string_t path, uint32_t want, allocator_t a, filec_dir_t **out);

/* The next entry, "." and ".." skipped, filled for d's want.  e->name is a
 * view into d, valid until the next filec_dir_next or filec_dir_close.  At
 * the end: *done = true and FS_OK (and the same on every later call).  An
 * entry that vanished between listing and stat is skipped. */
fs_err_t filec_dir_next(filec_dir_t *d, fs_entry_t *e, bool *done);

/* Closes the OS handle and gives d back to its allocator.  NULL: no-op. */
void filec_dir_close(filec_dir_t *d);

/* n bytes from p copied into a, as a string_t that lives as long as a;
 * n == 0 gives "".  false if a cannot allocate. */
bool filec_dup_string(allocator_t a, const char *p, size_t n, string_t *out);
