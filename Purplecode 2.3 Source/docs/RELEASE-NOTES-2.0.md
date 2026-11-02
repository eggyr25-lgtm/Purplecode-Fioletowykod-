# Purplecode 2.0 NextGen (NG)

This release rebrands the project as **Purplecode 2.0 NextGen (NG)** and continues the freestanding Multiboot improvements that began in 1.9.

## Freestanding Multiboot backend

- Up to **64** named locals (`let`), stored in fixed slots and usable across calls (was 8)
- String literal / fixup limit raised to **256** (was 64)
- Native emitter diagnostics now include `file:line:column` and clearer messages
- `Memory.Write8` / `Memory.Read8` in addition to 32-bit forms
- `if` supports `==` and `!=` against integer literals, with optional `else`
- Locals loaded by name in `if` conditions and memory operations
- Integer literal assignment: `let x: int = 42`

## Documentation

- Language reference and boot status updated for 2.0 NextGen (NG)
- Capability contract remains in `docs/BOOT-CODE-STATUS.md`

## Compatibility

- `examples/boot_hello.purplebc` unchanged and still the smoke test
- Hosted `purplec` path unchanged in this release
- Prior 1.x release notes are retained for history
