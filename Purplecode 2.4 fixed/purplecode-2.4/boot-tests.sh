#!/bin/sh
# Pure-shell / pure-C boot tests for Purplebootcode. No Python.
set -eu
cd "$(dirname "$0")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== Building Multiboot kernels =="
./purplebc examples/boot_hello.purplebc -o "$tmp/kernel.elf"
./purplebc examples/boot_hello.purplebc --format bin -o "$tmp/kernel.bin"

echo "== Validating Multiboot2 headers (pure C) =="
./mbcheck --elf "$tmp/kernel.elf"
./mbcheck --bin "$tmp/kernel.bin"

echo "== Basic ELF checks =="
if command -v readelf >/dev/null 2>&1; then
  readelf -h "$tmp/kernel.elf" | grep -q 'Entry point address'
  readelf -h "$tmp/kernel.elf" | grep -q 'ELF32'
  echo "readelf header OK"
else
  echo "readelf not available – skipping detailed ELF header dump"
fi

[ -s "$tmp/kernel.elf" ]
[ -s "$tmp/kernel.bin" ]
echo "kernel artifacts non-empty"

echo "== Additional boot samples =="
./purplebc examples/boot_disk_read.purplebc -o "$tmp/disk.elf"
./purplebc examples/boot_iso_read.purplebc -o "$tmp/iso-read.elf"
./mbcheck --elf "$tmp/disk.elf"
./mbcheck --elf "$tmp/iso-read.elf"

cat > "$tmp/namespaced.purplebc" <<'EOFAPI'
fn main() {
  Screen.Clear()
  Screen.Write("Purplecode boot API test\n")
  while true {
    CPU.Halt()
  }
  return 0
}
EOFAPI
./purplebc "$tmp/namespaced.purplebc" -o "$tmp/namespaced.elf"
./mbcheck --elf "$tmp/namespaced.elf"

echo "== UEFI (optional, requires clang + lld-link) =="
if [ "${PURPLECODE_TEST_UEFI:-0}" = 1 ] && command -v clang >/dev/null 2>&1 && command -v lld-link >/dev/null 2>&1; then
  ./purplebc examples/uefi_hello.purplebc --format uefi -o "$tmp/BOOTX64.EFI"
  od -An -tx1 -N 2 "$tmp/BOOTX64.EFI" | grep -q '4d 5a'
  echo "UEFI PE produced"
else
  echo "Skipping UEFI PE/COFF test (set PURPLECODE_TEST_UEFI=1 and install clang/lld-link to enable)"
fi

echo ""
echo "All boot-compiler tests passed (no Python involved)."
echo "To actually boot the kernel under QEMU:"
echo "  qemu-system-i386 -kernel build/kernel.elf -serial stdio"
echo "  (or use GRUB: multiboot2 /boot/kernel.elf)"
