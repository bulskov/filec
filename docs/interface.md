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

## path — canonical paths, no platform

### Two layers: canonical core, native conversion at the boundary

The path core (`filec/path.h`) knows exactly one syntax, the **canonical
form**, and no platform at all: no style parameter, no `#ifdef`, no `\`, no
drive roots, no `\\?\` prefixes. Every function — parent, file name,
extension, join, normalise, relative, compare — works on that one form, so
its tests are identical on every platform.

All platform syntax lives in one place, `filec/native.h`, which converts
between native and canonical paths:

- **in** — `path_from_native`, when a path comes from the OS or a user;
- **out** — `path_to_native`, for the OS or for display.

`native.h` is pure string code too, with a style parameter
(`PATH_STYLE_POSIX` / `PATH_STYLE_WINDOWS`), so the whole Windows rule set is
tested in Linux CI and the POSIX rules on Windows. `fs` takes and returns
canonical paths and converts internally.

### The canonical form

- `/` is the only separator.
- Absolute exactly when it starts with `/`; the only root is `/`.
- Windows drives and shares become ordinary first components:

| Native (Windows) | Canonical |
|---|---|
| `C:\Users\me`, `C:/Users/me`, `c:\Users\me` | `/C:/Users/me` |
| `\\server\share\x` | `/UNC/server/share/x` |
| `\\?\C:\very\long` | `/C:/very/long` |
| `\\?\UNC\server\share\x` | `/UNC/server/share/x` |
| `C:x` (drive-relative), `\x` (current drive's root) | refused by `path_from_native`; `fs_from_native` resolves them against the real current directory |

On Windows `/` is therefore a **virtual root** whose children are the drives —
"This PC" in Explorer terms. `path_parent("/C:")` is `/`, so navigating up
works the same way on every platform; only `path_to_native` refuses `/` (and
relative paths that lead through it), because it has no native form.

### Normalised paths

No empty components, no `.`, no `..` except leading ones in a relative path,
no trailing `/` except the root, and `.` for the empty relative path.
Everything filec hands out is normalised, and the anatomy functions expect
normalised input; `path_normalize` and `path_join` accept anything.

### Comparison is the one platform-dependent question

Whether `A` and `a` name the same file depends on the platform (and on macOS
the volume), not on the path. So `path_equals`, `path_compare`,
`path_starts_with`, `path_hash` and `path_relative` take a `path_case_t`
(`PATH_CASE_SENSITIVE` / `PATH_CASE_INSENSITIVE`, `PATH_CASE_NATIVE`).
Comparison is component by component: `/foo` is not a prefix of `/foobar`, and
a directory sorts directly before its contents (`/a` < `/a/b` < `/a-b`) — the
order a file tree wants.

### Views vs allocation

Anatomy functions (`parent`, `file_name`, `stem`, `extension`) return
**views** into the input — no allocation.

Anything that builds a new path (`normalize`, `join`, `join_iter`,
`with_extension`, `relative`, and `path_from_native` / `path_to_native`)
**appends to a `strbuf_t` the caller owns** and returns an error code:

- The caller holds the memory, so the caller knows it is responsible for it.
  One strbuf, cleared between calls, serves a whole directory listing; after
  the first few calls nothing allocates.
- On failure the strbuf is left exactly as it was, so there are no partial
  results and no cleanup at the call site.
- The functions need no temporaries: they normalise in place in `out`, with
  `strbuf_truncate` for `..` and the length at entry (or the end of a root)
  as the floor. An earlier version returned a fresh allocation per result;
  making that correct for any allocator meant freeing every temporary on
  every path, which was exactly the bookkeeping arenas exist to avoid.
- The rule that comes with it: the input must not point into `out`, because
  an append may move the buffer.

### Known limitations (deliberate)

- `PATH_CASE_INSENSITIVE` folds ASCII only. NTFS uses a per-volume upcase
  table; modelling it is out of scope.
- No Unicode normalisation (NFC/NFD). Matters on macOS; out of scope for now.
- `..` is resolved lexically. `/a/link/..` is `/a`, even if `link` points
  elsewhere, and the parent of `..` is `.`. Use `fs_real_path` when that
  matters.

## fs — the OS boundary

### Paths in: absolute only, converted in a stack buffer (decided)

Every `fs` call takes **canonical, normalised, absolute** paths. A relative
path is `FS_ERR_INVALID`: the current directory is process-wide state, and a
file manager runs many threads. `fs_from_native` resolves what a user types.

Every call converts its paths to the native form — NUL-terminated on POSIX,
UTF-16 with the `\\?\` prefix on Windows — in a **stack buffer**. A
canonical path longer than `FS_PATH_MAX` (4096) bytes is
`FS_ERR_NAME_TOO_LONG`.

An earlier draft added `_buf` variants (or an `fs_pathbuf_t` argument) for
longer paths. Dropped: Linux refuses paths over `PATH_MAX` (4096) in the
kernel anyway, and on Windows 4096 UTF-8 bytes are at most 4096 UTF-16
units — about 8 KB of stack with the prefix. Longer Windows paths exist but
are rare; they get an honest error instead of doubling the API.

### Errors: returned, results through out-parameters (decided)

Every function that can fail returns `fs_err_t { kind, os_code }`, like
`path_err_t` and `path_native_err_t`; results come back through
out-parameters. `FS_NODISCARD` (`warn_unused_result` on GCC/Clang) makes an
ignored error a warning — the useful part of C++'s `expected`. A generic
result type would need a struct per result type and macros, and without
lambdas there is no chaining to gain. `fs_ok(e)` is the short test.

### Memory: the caller decides, and is responsible (decided)

- Results that are strings (`fs_read_link`, `fs_real_path`, `fs_cwd`,
  `fs_location`, `fs_from_native`, `fs_read_all`) **append to a `strbuf_t`**
  the caller owns, as in `path`; on failure it is unchanged.
- Results that are lists (`fs_dir_list`, `fs_volumes`) **append to a
  `vec_t`** the caller owns, and their strings are **allocated from an
  allocator the caller passes**: they live as long as it. filec never frees
  what it hands out — pass an arena, reset it when the view goes.
- What filec needs only while it works (an iterator's state, a watcher)
  comes from the caller's allocator and goes back to it.
- Streams hand out **views**, valid until the next step: names in
  `fs_dir_iter` and in watch events. That keeps a long stream in constant
  memory; copy what you keep.

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

### Listing: the whole of it, or a stream (decided)

Two calls, because there are two lifetimes:

- `fs_dir_list(path, want, a, vec)` — the whole listing, the names in `a`:
  what a file manager's directory view keeps, sorts and shows.
- `fs_dir_iter(path, want, a, &err)` — a seqc `iter_t` in constant memory,
  names valid until the next step: for walking, counting, searching.

Both are written once (`fs_common.c`) on top of an internal per-backend
`filec_dir_open` / `_next` / `_close` that hands out one entry at a time;
the OS batches underneath anyway (`readdir` over `getdents64`,
`FIND_FIRST_EX_LARGE_FETCH`), so a public batch API would save no system
call.

`iter_t` has no error channel (a terminal consumes the iterator, so there
is nothing left to ask), so `fs_dir_iter` follows seqc's convention for
sources that can fail: the error goes to a caller-provided `fs_err_t`, and
a directory that cannot be opened gives a failed iterator — which since
seqc 3.3 is safe with every adaptor and terminal:

```c
fs_err_t err;
size_t n = iter_count(iter_filter(fs_dir_iter(path, FS_F_KIND, a, &err),
                                  is_dir, NULL));
if (!fs_ok(err)) { /* the listing stopped early */ }
```

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
`$topdir/.Trash/$uid` or `$topdir/.Trash-$uid` on other volumes, with a
`.trashinfo` file per item so any file manager can restore it. macOS uses
`~/.Trash` (or `$topdir/.Trashes/$uid`), Windows the Recycle Bin.

It never copies across volumes, and where there is no trash it says
`FS_ERR_UNSUPPORTED` and deletes **nothing**. That matters on Windows, where
`SHFileOperationW` with `FOF_ALLOWUNDO` silently deletes for good on a drive
without a Recycle Bin (network, removable): only fixed drives are let
through.

### Watching

- Not recursive. inotify is not; Windows could be, but identical semantics on
  every platform matter more.
- Events are **hints**. On `FS_W_OVERFLOW`, or whenever in doubt, re-read the
  directory.
- `fs_watch_wait` blocks (meant for its own thread); `fs_watch_wake` unblocks
  it from any thread for shutdown. `add`/`remove` are safe from any thread
  while another thread waits.
- What each system says differs: inotify and `ReadDirectoryChangesW` give
  every kind with the entry's name; macOS kqueue only that a directory's
  entries changed (name `""`: re-read it), and not a file rewritten in
  place. FSEvents would do better on macOS — a later improvement.

### Recursive operations — deferred

Recursive copy / delete / walk are left out on purpose. They involve policy
(conflicts, undo, progress across many files) that belongs to the caller for
now. Revisit later, mostly as a platform optimisation question (e.g. Windows
fast paths for whole-tree operations). An `fs_walk` iterator is a likely
first addition once the needs are clear.

## Open questions

- `fs_volumes` on Linux: which mounts count as "pseudo" (proc, sysfs, cgroup,
  tmpfs?, overlay?) and are hidden?
- Does `fs_entry_t` need `atime`, owner/group, or link target?
  (Probably not in the entry; separate calls.)
- macOS backend: later, but keep `src/posix/` free of Linux-only assumptions
  where it is cheap (`statx`, `copy_file_range`, `renameat2`, inotify are
  Linux-only and need fallbacks).
