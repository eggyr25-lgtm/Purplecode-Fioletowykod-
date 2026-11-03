# Purplecode 2.4

Security-hardening release focused on the image builder and the hosted networking runtime.

## Security fixes

### Critical – Image builder memory safety (`tools/purpleimage.c`)
The PBFIMG1 JSON manifest was previously built into a fixed 4 KiB buffer. `snprintf` return values were added to the length without checking whether the output had been truncated. A sufficiently long injected path (or many inject files) could advance the write cursor past the allocation and corrupt the heap.

**Fix:** The manifest is now built with a growable buffer. Every append checks the required size and reallocates (doubling) as needed. Long paths and large file lists are handled safely.

### High – Shell command injection in ISO builder (`tools/purpleimage.c`)
ISO creation used `system()` with single-quoted paths for `mkdir`, `cp`, `grub-mkrescue`, and cleanup. A crafted filename containing a single quote could break out of the quoting and execute arbitrary shell commands.

**Fix:** All external commands now go through a small `run(argv[])` helper that does `fork` + `execvp`. No shell is involved, so metacharacters in paths are inert.

### Medium – Static HTTP server path / symlink escape (`runtime/purple_net.inc`)
`Net.HttpServe` rejected `".."` in the request URI but still opened paths with ordinary `fopen` after simple string concatenation. A symlink inside the served directory that pointed outside it could expose files outside the intended tree.

**Fix:** The server opens the root directory once with `O_DIRECTORY` and performs all subsequent opens with `openat(..., O_NOFOLLOW)`. Symlink escapes are refused. Directory → `index.html` fallback is preserved.

### Medium – Unbounded HTTP client response buffer (`runtime/purple_net.inc`)
The plain HTTP client grew its response buffer without an upper limit and without a socket read timeout. A malicious or stuck server could force unbounded memory growth or hang the client indefinitely.

**Fix:**
- Hard response size limit of 16 MiB.
- 30-second `SO_RCVTIMEO` on the socket.

## Other changes
- README and version branding updated to 2.4.
- No language or bytecode changes; this is a pure hardening / tooling release.

## Upgrade notes
- Drop-in replacement for 2.3 for normal use.
- If you relied on extremely large HTTP responses (> 16 MiB) from the plain HTTP client, you will now get a failure instead of unbounded growth. Adjust the limit in `runtime/purple_net.inc` if you need a different ceiling.
- ISO builds no longer go through a shell; behaviour is otherwise identical.

## Credits
Security review and patches applied against the 2.3 tree.

## Additional hardening (post-2.4 polish)

### Medium – Remaining symlink escape via intermediate directory symlinks
The original 2.4 `openat(..., O_NOFOLLOW)` only protected the final path component. A directory symlink as an intermediate component (e.g. `root/lnk → /etc` then request `/lnk/hostname`) still escaped the tree.

**Fix:** Path components are now walked one-by-one with `O_NOFOLLOW` (and `O_DIRECTORY` for non-final components) via a new `openat_nofollow_walk` helper. No symlink is ever followed.

### Medium – Missing receive timeout on accepted server connections
The static HTTP server was single-threaded and performed blocking `recv` with no timeout after `accept`. A slow client could hang the entire server.

**Fix:** `SO_RCVTIMEO` (15 s) is now set on every accepted client socket, matching the client-side timeout practice.

### Truncation safety in `purpleimage`
Several `snprintf` calls into `PATH_MAX` buffers did not check the return value. Silent truncation could produce incorrect paths.

**Fix:** A checked `xsnprintf` macro now aborts with a clear error on truncation or failure.

### Documentation and packaging hygiene
- `libjpeg` / `libjpeg-turbo` development headers are now explicitly listed in the README requirements (Arch package `libjpeg-turbo`).
- Prebuilt binaries and `kernel.elf` are no longer shipped in the source distribution; build them with `make`.
- Remaining “2.3” branding, help text, Server/User-Agent headers, and the hello example message updated to 2.4.
