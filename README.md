# git-file-sha256

List every commit touching a single file and print the `sha256sum` of the file content at that commit.

```sh
git-file-sha256 path/to/file
```

Output: `<commit> <sha256>` per line, newest first (same order as `git log --follow`).

## Build

Requires a C compiler and `git` on `PATH` (hashing is internal, no `sha256sum` needed).

```sh
make
./git-file-sha256 path/to/file
```

## How it works

No checkouts. Efficient plumbing:

1. One `git log --follow --raw --abbrev=40 --format=%H -- <file>` to get `(commit, blob)` pairs. `--raw` yields the blob SHA directly, so renames tracked by `--follow` need no per-commit path resolution.
2. One persistent `git cat-file --batch` streams all blob contents (avoids one git spawn per commit).
3. Internal SHA-256 over each blob, streamed in 64 KiB chunks. Binary-safe, no temp files, no external hasher.

Paths for `git log -- <file>` are relative to the repo, as usual. Must be run inside a git repository.
