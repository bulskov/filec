# filec

Portable paths and filesystem access for C11 — POSIX and Win32 backends.

> **Status: design draft.** The interfaces in
> [`include/filec/path.h`](include/filec/path.h) and
> [`include/filec/fs.h`](include/filec/fs.h) are proposals; nothing is
> implemented yet. The reasoning behind them is in
> [`docs/interface.md`](docs/interface.md).

## Intent

`filec` is one of a small family of reusable C11 libraries:

| Library | Role |
|---|---|
| [arena_allocation](https://github.com/bulskov/arena_allocation) | memory: `allocator_t` and arenas |
| [seqc](https://github.com/bulskov/seqc) | collections, `string_t`, `iter_t` |
| [ctt](https://github.com/bulskov/ctt) | unit tests |
| [threadc](https://github.com/bulskov/threadc) | threads, locks, time |
| **filec** | paths and filesystem |

It was started for fskim (a fast, handmade file manager), so it has to be
*fast* and *honest* about the filesystem — but it has no knowledge of fskim and
should be useful anywhere.

It has two parts:

- **`path`** — pure string logic on `string_t`: root, parent, file name,
  extension, join, normalise, relative, compare. No syscalls. Every function
  takes a path style (`PATH_POSIX` / `PATH_WINDOWS`), so Windows path rules are
  tested on Linux and vice versa.
- **`fs`** — the OS boundary: directory listing, stat, positional file I/O,
  mkdir / rename / remove / copy with progress and cancel, trash, locations,
  volumes, and change watching.

Goals:

- **Speed where it matters.** Directory listing asks for exactly the metadata
  you want (`want` / `valid` bitmasks), so each platform can use its fast path.
- **No hidden allocation.** Everything that allocates takes an `allocator_t`;
  path conversion uses a stack buffer or a buffer you provide.
- **Same semantics everywhere.** Where platforms differ (case, links, watch
  recursion, timestamps) the API picks one meaning and documents it.
- **Library-specific errors.** `fs_err_t { kind, os_code }` — a portable kind
  to branch on, the raw OS code for diagnostics.
- **Independent of threadc.** Cancellation uses a plain C11 `atomic_bool`.

## Planned layout

```
include/filec/path.h     lexical paths — no OS code
include/filec/fs.h       filesystem
src/path/                path implementation
src/posix/               Linux first; macOS later
src/win32/               Win32 backend
tests/                   ctt tests; path tests run both styles on every OS
```

## Dependencies

- `seqc` (and through it `arena_allocation`) — `string_t`, `iter_t`, `vec_t`,
  `allocator_t`.
- `ctt` — tests only.

Fetched with CMake `FetchContent` using the dependency names `arena`, `seqc`,
`ctt`, so a parent project's declarations win and diamond dependencies resolve
to one copy.

## Suggested order of work

1. `path` — pure logic, test-heavy, both styles. A good place to start.
2. `fs` reading: `fs_dir_*`, `fs_stat`, files.
3. `fs` changing: mkdir, remove, rename, copy, trash.
4. Watching, locations, volumes.
5. Win32 backend for all of the above.

## License

MIT — see [LICENSE](LICENSE).
