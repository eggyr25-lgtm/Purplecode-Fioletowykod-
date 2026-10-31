# Purplecode Milestone 1 1.3 release notes

## UEFI graphics and input

- Added GOP discovery via UEFI Boot Services `LocateProtocol`.
- Added `GUI.Width()`, `GUI.Height()`, `GUI.Pixel(x, y, color)`, and `GUI.FillRect(x, y, width, height, color)`.
- Added a non-blocking `Input.ReadKey()` client for UEFI Simple Text Input.
- Added a read-only UEFI Block I/O protocol client: `Disk.BlockSize()`, `Disk.BlockCount()`, and `Disk.ReadByte(lba, offset)`.
- Added `examples/uefi_gop_demo.purplebc`, a colorful framebuffer smoke test.
- Expanded `boot-tests.sh` to compile the GOP demo into an x86-64 PE32+ EFI application.
- Rewrote the boot status guide with API semantics, build instructions, driver boundaries, and explicit unimplemented hardware support.

## Important boundaries

These are firmware protocol clients, not native GPU or USB drivers. GOP drawing depends on a usable framebuffer mode provided by firmware. Direct PCI, USB host controller/HID, NVMe/AHCI, network, audio, native GPU drivers, UEFI filesystem access, and UEFI Block I/O remain unimplemented. The UEFI backend still uses Clang/LLD for machine-code generation. Build tests do not replace QEMU/OVMF visual and input testing.
