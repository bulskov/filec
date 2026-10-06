# filec — interface design (draft)

This document records *why* the interfaces look the way they do, and which
questions are still open. It is a draft: change it as the implementation
teaches us things.

## Conventions

| Convention | Reason |
|---|---|
| `path_` / `fs_` prefixes, `_t` types, Allman braces, 80 columns | Same style as `seqc` / `arena_allocation`. |
| `string_t` from seqc for every string in and out | One string type across the library family; paths need not be NUL-terminated. |
| Library-specific errors: `fs_err_t { kind, os_code }` | Portable `kind` to branch on, raw `errno` / `GetLastError()` for diagnostics. |
| Allocation only through a passed `allocator_t` | Same rule as seqc. The caller decides lifetimes (typically one arena per job). |
| Independent of threadc | Cancellation is a C11 `atomic_bool *`; filec must be usable without threadc. |

## path — lexical, no OS

### Canonical form

Inside the program, paths always use `/`. Windows input with `\` is accepted
under `PATH_WINDOWS` and normalised to `/`. Conversion to the native look
happens only:

- at the OS boundary, inside `fs` (to UTF-16 and `\\?\` long-path form), and
- for display, with `path_to_display`.

Benefits: one representation to compare, hash (`path_hash`, consistent with
`path_equals`) and test.

### Style parameter instead of `#ifdef`

Every function takes a `path_style_t`. `PATH_NATIVE` picks the platform's
style. This means the whole Windows rule set — drive letters, UNC roots,
`\\?\` prefixes, case-insensitive comparison — is unit tested in Linux CI, and
POSIX rules on Windows.

### Root forms to support under `PATH_WINDOWS`

| Input | Root (canonical) | Note |
|---|---|---|
| `C:\x` | `C:/` | drive absolute |
| `C:x` | `C:` | drive *relative* — not absolute; decide whether to reject |
| `\x` | `/` | root of current drive — not absolute on Windows |
| `\\server\share\x` | `//server/share/` | UNC |
| `\\?\C:\x` | `C:/` | long-path prefix stripped on input |
| `\\?\UNC\server\share\x` | `//server/share/` | long UNC |

### Views vs allocation

Anatomy functions (`root`, `parent`, `file_name`, `stem`, `extension`) return
**views** into the input — no allocation. Anything that builds a new string
takes an `allocator_t` and returns `{NULL, 0}` on OOM.

### Known limitations (deliberate)

- `PATH_WINDOWS` folds ASCII case only. NTFS uses a per-volume upcase table;
  modelling it is out of scope.
- No Unicode normalisation (NFC/NFD). Matters on macOS; out of scope for now.
- `..` is resolved lexically. `/a/link/..` is `/a`, even if `link` points
  elsewhere. Use `fs_real_path` when that matters.

## fs — the OS boundary

### Path buffers (decided: stack buffer, or caller-provided buffer)

Every `fs` call must convert its path: NUL-terminate on POSIX, UTF-8 → UTF-16
plus `\\?\` prefix on Windows. Rather than passing an allocator to every call:

- **Default:** the plain functions use a stack buffer of `FS_PATH_MAX` (4096)
  bytes. Longer paths fail with `FS_ERR_NAME_TOO_LONG`. This matches Linux's
  own `PATH_MAX`.
- **Caller-provided buffer:** for longer paths, the caller passes an
  `fs_pathbuf_t { ptr, size }` and decides where it lives — stack, arena,
  static. Windows needs about `2 * len + 16` bytes for UTF-16 plus prefix.

The plain function is a thin wrapper that calls the buffer variant with a
stack buffer, so there is one implementation per operation.

**Open:** the header sketches `_buf` variants only for `fs_dir_open` and
`fs_stat` (the hot paths). Options:

1. `_buf` variant for every path-taking function — explicit, but doubles the
   API surface.
2. `_buf` variants only where long paths are realistic (listing, stat, open,
   copy, rename), plain functions elsewhere.
3. No variants; instead one optional `fs_pathbuf_t *` parameter (NULL = stack)
   on every function — one API, slightly noisier calls.

### `want` / `valid` metadata masks

Platforms deliver different metadata for free:

| | names | kind | size, mtime, attrs |
|---|---|---|---|
| Linux `getdents64` | ✓ | usually (`d_type`, may be `DT_UNKNOWN`) | ✗ needs `statx` per entry |
| Windows `FindFirstFileExW` | ✓ | ✓ | ✓ |

So the caller says what it **wants**, the backend fills in what it got for
free plus what was asked for, and sets **valid** accordingly. Callers must
check `valid`, never assume. A file manager can then paint names immediately
and fetch sizes lazily on Linux, while Windows gets everything in one pass.

### Batch API first, iterator on top

`fs_dir_read` fills a caller array — the fast, explicit primitive with an
error per call. `fs_dir_iter` wraps it as a seqc `iter_t` for convenience.

`iter_t` has no error channel, so the iterator writes errors into a
caller-provided `fs_err_t` and stops (the "side channel" approach):

```c
fs_err_t err = {0};
iter_t it = fs_dir_iter(path, FS_F_SIZE, &err, alloc);
vec_extend(v, iter_filter(it, is_big, NULL));
if (err.kind != FS_OK) { /* the loop stopped early */ }
```

If the pattern shows up in many places, extend seqc with a fallible iterator
(a `failed()` slot like `peek`, propagated by adaptors, reported by
terminals). We own seqc, so that is an option — but start with the side
channel, which costs nothing.

### Timestamps: `mtime` and `btime`, never `ctime`

POSIX `ctime` = *metadata change* time; Windows "creation time" = *birth*.
Same name, different meaning — a classic bug. The API uses `mtime` (content
modified) and `btime` (birth; `statx` `STATX_BTIME`, not every Linux
filesystem has it — check `valid`). All times are Unix epoch nanoseconds.

### Files: a handle, positional I/O only

`fs_file_t` is `{ intptr_t h }` — an fd or a `HANDLE`, no allocation. There is
no seek and no hidden file position: `fs_read_at` / `fs_write_at` (`pread` /
`ReadFile` with `OVERLAPPED` offset). Concurrent reads of one file are then
safe, and there is no shared cursor to get wrong.

### Links

`FS_KIND_LINK` covers symlinks *and* Windows junctions / reparse points. The
difference is a Windows detail most callers do not care about; add a flag if
one ever does.

### Renames are atomic and honest

`fs_rename` works within one volume only and returns `FS_ERR_CROSS_DEVICE`
otherwise — the caller (e.g. a file manager's job system) decides to copy +
delete. Without `FS_REPLACE` the "does the target exist" check is atomic
(`renameat2(RENAME_NOREPLACE)` / `MoveFileExW` without
`MOVEFILE_REPLACE_EXISTING`), so there is no check-then-act race.

### Trash

Linux follows the XDG trash spec: `$XDG_DATA_HOME/Trash` for the home volume,
`$topdir/.Trash-$uid` (or `.Trash/$uid`) on other volumes, with a
`.trashinfo` file per item. Windows uses the Recycle Bin. This is a sizeable
piece of work on Linux — schedule it after the basics.

### Watching

- Not recursive. inotify is not; Windows could be, but identical semantics on
  every platform matter more.
- Events are **hints**. On `FS_W_OVERFLOW`, or whenever in doubt, re-read the
  directory.
- `fs_watch_wait` blocks (meant for its own thread); `fs_watch_wake` unblocks
  it from any thread for shutdown. `add`/`remove` are safe from any thread
  while another thread waits.

### Recursive operations — deferred

Recursive copy / delete / walk are left out on purpose. They involve policy
(conflicts, undo, progress across many files) that belongs to the caller for
now. Revisit later, mostly as a platform optimisation question (e.g. Windows
fast paths for whole-tree operations). An `fs_walk` iterator is a likely
first addition once the needs are clear.

## Open questions

- Path buffers: which of the three options above for `_buf` variants?
- `C:x` (drive-relative) and `\x` (current-drive root) on Windows: support,
  or reject as `FS_ERR_INVALID`?
- `fs_volumes` on Linux: which mounts count as "pseudo" (proc, sysfs, cgroup,
  tmpfs?, overlay?) and are hidden?
- Does `fs_entry_t` need `atime`, owner/group, or link target?
  (Probably not in the entry; separate calls.)
- macOS backend: later, but keep `src/posix/` free of Linux-only assumptions
  where it is cheap (`statx`, `copy_file_range`, `renameat2`, inotify are
  Linux-only and need fallbacks).
