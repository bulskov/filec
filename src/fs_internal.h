#pragma once

#include "filec/fs.h"
#include "seqc/string.h"

#define FILEC_PATH_BUF_MAX (FS_PATH_MAX + 1)

// shared helper functions
fs_err_t filec_path_cstr(string_t path, char *buf, size_t buf_size);