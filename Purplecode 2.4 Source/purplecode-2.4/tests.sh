#!/bin/sh
set -eu
cd "$(dirname "$0")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for src in examples/*.purple; do ./purplec -S "$src" -o "$tmp/$(basename "${src%.purple}").s"; done
cat > "$tmp/expr.purple" <<'EOF'
fn twice(x) { return x * 2 }
fn add(a, b) { return a + b }
fn main() { return (add(20, 22) - twice(11)) / 2 }
EOF
./purplec -S "$tmp/expr.purple" -o "$tmp/expr.s"
gcc -no-pie "$tmp/expr.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/expr"
set +e
"$tmp/expr"
status=$?
set -e
[ "$status" -eq 10 ] || { echo "expected exit status 10, got $status" >&2; exit 1; }
# Encoding built-ins write their result to the language global $LastHexResult.
./purplec -S examples/blow_fonts_encoding.purple -o "$tmp/encoding.s"
gcc -no-pie "$tmp/encoding.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/encoding"
"$tmp/encoding" > "$tmp/encoding.out"
grep -q '^6B65626162$' "$tmp/encoding.out"
grep -q '^WUVT$' "$tmp/encoding.out"
grep -q '^0100111101001011$' "$tmp/encoding.out"
# HTTPS entry point must be available in hosted native builds (network is not needed here).
./purplec -S examples/net_https_get.purple -o "$tmp/https.s"
gcc -no-pie "$tmp/https.s" runtime/purple_rt.c -lssl -lcrypto -lm -o "$tmp/https"
# Syntax errors must fail compilation, not hang or emit assembly.
printf 'fn main( { return 0 }\n' > "$tmp/bad.purple"
if timeout 3 ./purplec -S "$tmp/bad.purple" -o "$tmp/bad.s" >/dev/null 2>&1; then echo 'expected malformed source to fail' >&2; exit 1; fi
# NYMNOG API runtime smoke test: process/time APIs, file metadata, JS byte loading,
# integer printing, and a harmless Linux getpid syscall.
./purplec -S examples/nymnog_demo.purple -o "$tmp/nymnog_demo.s"
gcc -no-pie "$tmp/nymnog_demo.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/nymnog_demo"
"$tmp/nymnog_demo" > "$tmp/nymnog.out"
grep -q 'Purplecode NYMNOG API demo' "$tmp/nymnog.out"
grep -q '\[Web.JS\] Loaded .*demo.js' "$tmp/nymnog.out"
# The integer printer must print numbers, not dereference them as strings.
cat > "$tmp/print_int.purple" <<'EOF'
fn main() {
    Data.ShellPrint(12345)
    return 0
}
EOF
./purplec -S "$tmp/print_int.purple" -o "$tmp/print_int.s"
gcc -no-pie "$tmp/print_int.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/print_int"
"$tmp/print_int" > "$tmp/print_int.out"
grep -qx '12345' "$tmp/print_int.out"

# Actual JavaScript execution, asset APIs, and metadata-based Controlang alias.
if command -v node >/dev/null 2>&1; then
  cat > "$tmp/jsrun.js" <<'EOF'
console.log('JS_ENGINE_EXECUTED'); process.exit(7);
EOF
  cat > "$tmp/integration.purple" <<'EOF'
#uselib "../examples/kustomlibrary.pl"
fn main() {
  let js: int = Web.JS.Run("PLACEHOLDER")
  Data.ShellPrint.Int(js)
  return 0
}
EOF
  sed "s|PLACEHOLDER|$tmp/jsrun.js|" "$tmp/integration.purple" > "$tmp/integration2.purple"
  sed "s|../examples/kustomlibrary.pl|$PWD/examples/kustomlibrary.pl|" "$tmp/integration2.purple" > "$tmp/integration3.purple"
  ./purplec -S "$tmp/integration3.purple" -o "$tmp/integration.s"
  gcc -no-pie "$tmp/integration.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/integration"
  "$tmp/integration" > "$tmp/integration.out"
  grep -q '^JS_ENGINE_EXECUTED$' "$tmp/integration.out"
  grep -qx '7' "$tmp/integration.out"
  node examples/cache/demo.js > "$tmp/demo-node.out"
  grep -q 'executed in Node.js' "$tmp/demo-node.out"
fi
# Imported metadata aliases both %KustomGUI(...) and KustomGUI(...).
cat > "$tmp/controlang.purple" <<'EOF'
#uselib "../examples/kustomlibrary.pl"
fn main() { %KustomGUI("test window"); return 0 }
EOF
sed "s|../examples/kustomlibrary.pl|$PWD/examples/kustomlibrary.pl|" "$tmp/controlang.purple" > "$tmp/controlang2.purple"
./purplec -S "$tmp/controlang2.purple" -o "$tmp/controlang.s"
grep -q 'call Kustom.GUI.MakeWindow' "$tmp/controlang.s"
# Invalid library metadata must stop compilation.
cat > "$tmp/bad.pl" <<'EOF'
NAME = broken
EOF
cat > "$tmp/badlib.purple" <<'EOF'
#uselib "bad.pl"
fn main() { return 0 }
EOF
if ./purplec -S "$tmp/badlib.purple" -o "$tmp/badlib.s" >/dev/null 2>&1; then
  echo 'expected invalid Purple Lib metadata to fail' >&2; exit 1
fi


# A .pl library's post-header Purplecode body is compiled and called, not merely aliased.
cat > "$tmp/bodylib.pl" <<'EOF'
PLStartMarekPCStartHeader
NAME = BodyLib
VERSION = 1.0
TYPE = Purple Lib
LANGUAGE = Purplecode
CONTROLANG = Demo.Double
PLEndMarekPCEndHeader
fn Demo.Double(x) { return x * 2 }
EOF
cat > "$tmp/bodylib.purple" <<'EOF'
#uselib "bodylib.pl"
fn main() { return %BodyLib(21) }
EOF
./purplec -S "$tmp/bodylib.purple" -o "$tmp/bodylib.s"
gcc -no-pie "$tmp/bodylib.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/bodylib"
set +e; "$tmp/bodylib"; status=$?; set -e
[ "$status" -eq 42 ] || { echo "Purple Lib source body expected exit 42, got $status" >&2; exit 1; }

# Native plugin ABI actually loads a .so, resolves the exported symbol, and calls it.
gcc -shared -fPIC -o "$tmp/libpurple_demo.so" examples/native/demo.c
cat > "$tmp/native.purple" <<EOF
fn main() {
  Controlang.Native.Register("Demo.Double", "$tmp/libpurple_demo.so", "purple_demo_double")
  return Demo.Double(21)
}
EOF
./purplec -S "$tmp/native.purple" -o "$tmp/native.s"
gcc -no-pie "$tmp/native.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/native"
set +e; "$tmp/native"; status=$?; set -e
[ "$status" -eq 42 ] || { echo "native Controlang expected exit 42, got $status" >&2; exit 1; }

# Image.Load decodes PPM, PNG and JPEG into RGB pixels.
for spec in "demo.ppm:2x2:4" "demo.png:3x2:6" "demo.jpg:4x3:12"; do
  file=${spec%%:*}; rest=${spec#*:}; dims=${rest%%:*}; pixels=${rest##*:}
  cat > "$tmp/image.purple" <<EOF
fn main() { let pixels: int = Image.Load("examples/cache/$file"); Data.ShellPrint.Int(pixels); return 0 }
EOF
  ./purplec -S "$tmp/image.purple" -o "$tmp/image.s"
  gcc -no-pie "$tmp/image.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/image"
  "$tmp/image" > "$tmp/image.out"
  grep -q "Decoded examples/cache/$file: $dims RGB24" "$tmp/image.out"
  grep -qx "$pixels" "$tmp/image.out"
done

# CSS parser accepts valid balanced rules and rejects malformed braces.
cat > "$tmp/good.css" <<'EOF'
body { color: purple; background: #101020; }
EOF
cat > "$tmp/css.purple" <<EOF
fn main() { return Web.CSS.Load("$tmp/good.css") }
EOF
./purplec -S "$tmp/css.purple" -o "$tmp/css.s"
gcc -no-pie "$tmp/css.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/css"
set +e; "$tmp/css" >/dev/null; status=$?; set -e
[ "$status" -gt 0 ] || { echo "CSS.Load expected stylesheet byte count, got $status" >&2; exit 1; }
cat > "$tmp/bad.css" <<'EOF'
body { color: purple;
EOF
cat > "$tmp/badcss.purple" <<EOF
fn main() { let x: int = Web.CSS.Load("$tmp/bad.css"); return x }
EOF
./purplec -S "$tmp/badcss.purple" -o "$tmp/badcss.s"
gcc -no-pie "$tmp/badcss.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/badcss"
if "$tmp/badcss" >/dev/null 2>&1; then echo 'expected malformed CSS to fail' >&2; exit 1; fi

# Raw-sector API can read sector 1 of a regular image without root permissions.
dd if=/dev/zero of="$tmp/disk.img" bs=512 count=4 status=none
cat > "$tmp/sector.purple" <<EOF
fn main() { return USB.ReadSector("$tmp/disk.img", 1) }
EOF
./purplec -S "$tmp/sector.purple" -o "$tmp/sector.s"
gcc -no-pie "$tmp/sector.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/sector"
set +e; "$tmp/sector"; status=$?; set -e
[ "$status" -eq 0 ] || { echo "USB.ReadSector expected 0, got $status" >&2; exit 1; }
cat > "$tmp/sectorto.purple" <<EOF
fn main() { return USB.ReadSectorTo("$tmp/disk.img", 1, "$tmp/sector.bin") }
EOF
./purplec -S "$tmp/sectorto.purple" -o "$tmp/sectorto.s"
gcc -no-pie "$tmp/sectorto.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/sectorto"
set +e; "$tmp/sectorto"; status=$?; set -e
[ "$status" -eq 0 ] || { echo "USB.ReadSectorTo expected 0, got $status" >&2; exit 1; }
[ "$(wc -c < "$tmp/sector.bin")" -eq 512 ] || { echo 'USB.ReadSectorTo did not save a 512-byte sector' >&2; exit 1; }


# ELF execution API runs an actual executable and respects SafeCodeOnly policy.
cat > "$tmp/elf-child.c" <<'EOF'
#include <stdio.h>
int main(void) { puts("ELF_CHILD_RAN"); return 23; }
EOF
gcc -o "$tmp/elf-child" "$tmp/elf-child.c"
cat > "$tmp/native-elf-lib.purple" <<EOF
#uselib "$PWD/examples/native_elf_library.pl"
fn main() { return Native.ELF.Run("$tmp/elf-child") }
EOF
./purplec -S "$tmp/native-elf-lib.purple" -o "$tmp/native-elf-lib.s"
gcc -no-pie "$tmp/native-elf-lib.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/native-elf-lib"
printf 'SafeCodeOnly = false\nVirtualMode = false\n' > "$tmp/config.ini"
set +e; PURPLECODE_CONFIG="$tmp/config.ini" "$tmp/native-elf-lib" > "$tmp/native-elf-lib.out"; status=$?; set -e
[ "$status" -eq 23 ] || { echo "ELF Purple Lib wrapper expected exit 23, got $status" >&2; exit 1; }
grep -qx 'ELF_CHILD_RAN' "$tmp/native-elf-lib.out"
cat > "$tmp/elf-run.purple" <<EOF
fn main() { return Program.ELF64.Run("$tmp/elf-child") }
EOF
./purplec -S "$tmp/elf-run.purple" -o "$tmp/elf-run.s"
gcc -no-pie "$tmp/elf-run.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/elf-run"
printf 'SafeCodeOnly = false\nVirtualMode = false\n' > "$tmp/config.ini"
set +e; PURPLECODE_CONFIG="$tmp/config.ini" "$tmp/elf-run" > "$tmp/elf-run.out"; status=$?; set -e
[ "$status" -eq 23 ] || { echo "ELF runner expected exit 23, got $status" >&2; exit 1; }
grep -qx 'ELF_CHILD_RAN' "$tmp/elf-run.out"
printf 'SafeCodeOnly = true\nVirtualMode = false\n' > "$tmp/config.ini"
set +e; PURPLECODE_CONFIG="$tmp/config.ini" "$tmp/elf-run" >/dev/null 2>&1; status=$?; set -e
[ "$status" -eq 255 ] || { echo "SafeCodeOnly should block ELF execution (return -1), got $status" >&2; exit 1; }
# Virtual Mode must never silently fall back to host execution if bwrap is unavailable.
if ! command -v bwrap >/dev/null 2>&1; then
  printf 'SafeCodeOnly = false\nVirtualMode = true\n' > "$tmp/config.ini"
  set +e; PURPLECODE_CONFIG="$tmp/config.ini" "$tmp/elf-run" >/dev/null 2>&1; status=$?; set -e
  [ "$status" -eq 126 ] || { echo "VirtualMode without bwrap should fail closed, got $status" >&2; exit 1; }
fi

# User-wide library lookup: a .pl installed under the user's library root is
# found from an unrelated project directory without an absolute path.
mkdir -p "$tmp/home/.local/share/purplecode/libs/vendor/nested"
cat > "$tmp/home/.local/share/purplecode/libs/vendor/nested/global.pl" <<'EOF'
PLStartMarekPCStartHeader
NAME = GlobalLib
VERSION = 1.0.0
TYPE = Purple Lib
LANGUAGE = Purplecode
PLEndMarekPCEndHeader
fn Global.Answer() { return 42 }
EOF
cat > "$tmp/global-import.purple" <<'EOF'
#uselib "global.pl"
fn main() { return Global.Answer() }
EOF
HOME="$tmp/home" ./purplec -S "$tmp/global-import.purple" -o "$tmp/global-import.s"
gcc -no-pie "$tmp/global-import.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/global-import"
set +e; "$tmp/global-import"; status=$?; set -e
[ "$status" -eq 42 ] || { echo "global Purple Lib import expected 42, got $status" >&2; exit 1; }
python3 tools/purplelibinstaller.py --help > "$tmp/installer-help.txt"
grep -q -- '--githubpage URL' "$tmp/installer-help.txt"
# Exercise the installer without network access by supplying a fake git clone.
mkdir -p "$tmp/fakebin" "$tmp/fake-repo/nested"
cat > "$tmp/fake-repo/nested/installed.pl" <<'EOF'
PLStartMarekPCStartHeader
NAME = InstalledLib
VERSION = 1.0.0
TYPE = Purple Lib
LANGUAGE = Purplecode
PLEndMarekPCEndHeader
fn Installed.Answer() { return 42 }
EOF
cat > "$tmp/fakebin/git" <<'EOF'
#!/bin/sh
for arg do dest="$arg"; done
mkdir -p "$dest"
cp -a "$PURPLE_TEST_REPO/." "$dest/"
EOF
chmod +x "$tmp/fakebin/git"
PATH="$tmp/fakebin:$PATH" PURPLE_TEST_REPO="$tmp/fake-repo" HOME="$tmp/home" \
  python3 tools/purplelibinstaller.py --githubpage github.com/test/purplelib --lib-dir "$tmp/home/.local/share/purplecode/libs" > "$tmp/install.out"
grep -q 'Installed 1 Purple Lib file' "$tmp/install.out"
cat > "$tmp/installed-import.purple" <<'EOF'
#uselib "installed.pl"
fn main() { return Installed.Answer() }
EOF
HOME="$tmp/home" ./purplec -S "$tmp/installed-import.purple" -o "$tmp/installed-import.s"
gcc -no-pie "$tmp/installed-import.s" runtime/purple_rt.c -DPURPLE_HAS_IMAGE -lpng -ljpeg -lssl -lcrypto -lm -o "$tmp/installed-import"
set +e; "$tmp/installed-import"; status=$?; set -e
[ "$status" -eq 42 ] || { echo "installed Purple Lib expected 42, got $status" >&2; exit 1; }
./purplec --help > "$tmp/purplec-help.txt" 2>&1
grep -q -- '--static' "$tmp/purplec-help.txt"
# Purplecode can link directly to a native ELF without leaving a .s output.
./purplec examples/hello.purple -o "$tmp/hello-direct.elf"
file "$tmp/hello-direct.elf" | grep -qi 'ELF 64-bit'
[ ! -e "$tmp/hello-direct.elf.s" ]
set +e; "$tmp/hello-direct.elf" > "$tmp/hello-direct.out"; status=$?; set -e
[ "$status" -eq 0 ] || { echo "direct ELF output expected status 0, got $status" >&2; exit 1; }


# --static must either produce a truly static ELF or fail clearly when static
# dependency archives are unavailable; it must never silently make a dynamic ELF.
cat > "$tmp/static-lib.pl" <<'EOF'
PLStartMarekPCStartHeader
NAME = StaticLib
VERSION = 1.0.0
TYPE = Purple Lib
LANGUAGE = Purplecode
PLEndMarekPCEndHeader
fn Static.Answer() { return 42 }
EOF
cat > "$tmp/static.purple" <<EOF
#uselib "$tmp/static-lib.pl"
fn main() { return Static.Answer() }
EOF
if ./purplec -S "$tmp/static.purple" --static -o "$tmp/static-program" > "$tmp/static.out" 2>&1; then
  file "$tmp/static-program" | grep -qi 'statically linked'
  if readelf -l "$tmp/static-program" 2>/dev/null | grep -q 'INTERP'; then
    echo 'static output unexpectedly contains a dynamic interpreter' >&2; exit 1
  fi
  set +e; "$tmp/static-program"; status=$?; set -e
  [ "$status" -eq 42 ] || { echo "static imported library expected exit 42, got $status" >&2; exit 1; }
else
  grep -q 'static link failed' "$tmp/static.out" || { cat "$tmp/static.out" >&2; echo 'static failure was not explained' >&2; exit 1; }
  [ ! -e "$tmp/static-program" ] || { echo 'failed static link left an output file' >&2; exit 1; }
fi

echo 'Purplecode tests passed.' 


# Direct ELF: signed formatting and signed division/remainder edge cases.
cat > "$tmp/direct_signed.purple" <<'EOF_SIGNED_TEST'
fn main() {
    Data.ShellPrint.Int(-12345)
    Data.ShellPrint.Int(-9223372036854775807 - 1)
    Data.ShellPrint.Int(9223372036854775807)
    Data.ShellPrint.Int(-17 / 5)
    Data.ShellPrint.Int(-17 % 5)
    Data.ShellPrint.Int(17 % -5)
    Data.ShellPrint.Int(0)
    return 0
}
EOF_SIGNED_TEST
./purplec "$tmp/direct_signed.purple" --direct -o "$tmp/direct_signed"
"$tmp/direct_signed" > "$tmp/direct_signed.out"
cat > "$tmp/direct_signed.expected" <<'EOF_SIGNED_EXPECTED'
-12345
-9223372036854775808
9223372036854775807
-3
-2
2
0
EOF_SIGNED_EXPECTED
cmp "$tmp/direct_signed.expected" "$tmp/direct_signed.out"

cat > "$tmp/direct_ops.purple" <<'EOF_OPS'
fn main() {
    Data.ShellPrint.Int(5 & 3)
    Data.ShellPrint.Int(5 | 8)
    Data.ShellPrint.Int(5 ^ 3)
    Data.ShellPrint.Int(2 << 3)
    Data.ShellPrint.Int(-8 >> 1)
    Data.ShellPrint.Int(2 && 3)
    Data.ShellPrint.Int(0 || 7)
    Data.ShellPrint.Int(~0)
    Data.ShellPrint.Int(!0)
    return 0
}
EOF_OPS
./purplec "$tmp/direct_ops.purple" --direct -o "$tmp/direct_ops"
"$tmp/direct_ops" > "$tmp/direct_ops.out"
cat > "$tmp/direct_ops.expected" <<'EOF_OPS_EXPECTED'
1
13
6
16
-4
1
1
-1
1
EOF_OPS_EXPECTED
cmp "$tmp/direct_ops.expected" "$tmp/direct_ops.out"
# The direct backend uses its own linear-scan allocator for local variables.
# Seven simultaneously-live locals force both register assignment and spills.
cat > "$tmp/regalloc.purple" <<'EOF_REGALLOC'
fn main() {
  let a: int = 1
  let b: int = 2
  let c: int = 3
  let d: int = 4
  let e: int = 5
  let f: int = 6
  let g: int = 7
  Data.ShellPrint.Int(a + b + c + d + e + f + g)
  return 0
}
EOF_REGALLOC
./purplec "$tmp/regalloc.purple" --direct --dump-regalloc -o "$tmp/regalloc" 2>"$tmp/regalloc.alloc"
"$tmp/regalloc" > "$tmp/regalloc.out"
grep -qx '28' "$tmp/regalloc.out"
grep -q 'linear-scan register allocation' "$tmp/regalloc.alloc"
grep -q -- '-> rbx' "$tmp/regalloc.alloc"
grep -q 'stack spill' "$tmp/regalloc.alloc"
echo 'Purplecode custom register allocator tests passed.'
echo 'Purplecode direct operator tests passed.'


echo 'Purplecode direct signed-integer tests passed.' 

# Direct ELF hierarchical syscall/runtime coverage: overload printing, process/time,
# filesystem metadata, and the Linux x86-64 syscall bridge without libc.
cat > "$tmp/direct-runtime.purple" <<'EOF_DIRECT_RUNTIME'
fn main() {
  Data.ShellPrint("DIRECT_STRING_OK")
  let message: string = "LOCAL_STRING_OK"
  Data.ShellPrint(message)
  Data.ShellPrint(12345)
  let pid: int = System.Process.ID()
  if pid > 0 { Data.ShellPrint("PID_OK") }
  let now: int = System.Time.Unix()
  if now > 0 { Data.ShellPrint("TIME_OK") }
  let exists: bool = FS.Exists("/etc/passwd")
  if exists { Data.ShellPrint("FS_EXISTS_OK") }
  let size: int = FS.Size("/etc/hostname")
  if size > 0 { Data.ShellPrint("FS_SIZE_OK") }
  LinuxAPI.Call(1, 1, "SYSCALL_WRITE_OK", 16)
  Data.ShellPrint("")
  let mem: int = Memory.Alloc(4096)
  if mem > 0 {
    Memory.Free(mem, 4096)
    Data.ShellPrint("MEMORY_ALLOC_FREE_OK")
  }
  FS.Data.Write("PURPLE_DIRECT_FILE_WRITE_OK", "/tmp/purple-direct-write-test.txt")
  Data.ShellPrint("AFTER_DIRECT_FS_WRITE")
  return 0
}
EOF_DIRECT_RUNTIME
./purplec "$tmp/direct-runtime.purple" --direct -o "$tmp/direct-runtime.elf"
chmod +x "$tmp/direct-runtime.elf"
"$tmp/direct-runtime.elf" > "$tmp/direct-runtime.out"
grep -qx 'DIRECT_STRING_OK' "$tmp/direct-runtime.out"
grep -qx 'LOCAL_STRING_OK' "$tmp/direct-runtime.out"
grep -qx '12345' "$tmp/direct-runtime.out"
grep -qx 'PID_OK' "$tmp/direct-runtime.out"
grep -qx 'TIME_OK' "$tmp/direct-runtime.out"
grep -qx 'FS_EXISTS_OK' "$tmp/direct-runtime.out"
grep -qx 'FS_SIZE_OK' "$tmp/direct-runtime.out"
grep -qx 'SYSCALL_WRITE_OK' "$tmp/direct-runtime.out"
grep -qx 'MEMORY_ALLOC_FREE_OK' "$tmp/direct-runtime.out"
grep -qx 'AFTER_DIRECT_FS_WRITE' "$tmp/direct-runtime.out"
grep -qx 'PURPLE_DIRECT_FILE_WRITE_OK' /tmp/purple-direct-write-test.txt
rm -f /tmp/purple-direct-write-test.txt
echo 'Purplecode direct hierarchical syscall/runtime tests passed.'

# Internal emitter: user functions, recursion, structs (stack + heap + nested), methods, arrays, allocator,
# and loop-carried variables that must survive register reuse.
cat > "$tmp/direct_lang.purple" <<'EOF_DIRECT_LANG'
struct Point { x: int; y: int; }
struct Entity { id: int; pos: Point; name: string; health: int; }
fn Point.sum(self: int, extra: int) { return extra + 1; }
fn fib(n: int) { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }
fn greet() { return "hello from fn"; }
fn main() {
  Data.ShellPrint.Int(fib(20));
  Data.ShellPrint(greet());
  let total: int = 0; let i: int = 0; let keep: int = 7;
  while i < 5 {
    let a: int = i * 2; let b: int = a + 1; let c: int = b + 1; let d: int = c + 1; let e: int = d + 1;
    let f: int = e + keep; total = total + f; i = i + 1;
  }
  Data.ShellPrint.Int(total);
  let s: Point; s.x = 1; s.y = 2; Data.ShellPrint.Int(s.x + s.y);
  let h = Struct.New(Point); h.x = 10; h.y = 20; Data.ShellPrint.Int(h.x + h.y);
  let en: Entity; en.id = 7; en.pos.x = 3; en.pos.y = 4; en.name = "knight";
  Data.ShellPrint(en.name); Data.ShellPrint.Int(en.pos.x * en.pos.y + en.id);
  Data.ShellPrint.Int(h.sum(41));
  let arr = Array.New(5); let k: int = 0;
  while k < Array.Len(arr) { Array.Set(arr, k, k * k); k = k + 1; }
  Data.ShellPrint.Int(Array.Get(arr, 4)); Data.ShellPrint.Int(Array.Get(arr, 9)); Data.ShellPrint.Int(Array.Set(arr, 9, 1));
  Array.Free(arr); Struct.Free(h);
  let p = System.Memory.Alloc(64); Memory.Free(p);
}
EOF_DIRECT_LANG
./purplec "$tmp/direct_lang.purple" --direct -o "$tmp/direct_lang"
"$tmp/direct_lang" > "$tmp/direct_lang.out"
printf '6765\nhello from fn\n75\n3\n30\nknight\n19\n42\n16\n0\n-1\n' > "$tmp/direct_lang.expected"
cmp "$tmp/direct_lang.expected" "$tmp/direct_lang.out"
# The default path must use the internal emitter (no GCC needed) for supported programs.
./purplec "$tmp/direct_lang.purple" -o "$tmp/direct_lang_default" | grep -q 'GoodNative Emitter'
"$tmp/direct_lang_default" | cmp - "$tmp/direct_lang.expected"
echo 'Purplecode direct language tests passed.'

# Internal emitter: File.*, Net.* (loopback UDP + name resolution), encoders, hex print, StructArray, string escapes.
mkdir -p "$tmp/fdir"
cat > "$tmp/direct_rt.purple" <<'EOF_DIRECT_RT'
struct P { a: int; b: int; }
fn main() {
  let buf: int = Memory.Alloc(256);
  File.Write("TMPDIR/fdir/b.txt", "hello");
  File.Append("TMPDIR/fdir/b.txt", " world\n");
  File.Copy("TMPDIR/fdir/b.txt", "TMPDIR/fdir/a.txt");
  Data.ShellPrint.Int(File.Read("TMPDIR/fdir/a.txt", buf, 100));
  Net.Print(buf);
  Data.ShellPrint.Int(File.Count("TMPDIR/fdir"));
  let nm: int = Memory.Alloc(64);
  File.Name("TMPDIR/fdir", 0, nm, 60); Net.Print(nm); Data.ShellPrint("");
  Data.ShellPrint.Int(File.IsDir("TMPDIR/fdir") + File.Exists("TMPDIR/fdir/a.txt") * 10);
  File.Delete("TMPDIR/fdir/a.txt");
  Data.ShellPrint.Int(File.Exists("TMPDIR/fdir/a.txt"));
  Data.ShellPrint(Hexify("Hi!")); Data.ShellPrint(Base64ify("Many hands")); Data.ShellPrint(Binarify("A"));
  Data.ShellPrint.Hex(48879);
  let arr = StructArray.New(P, 3);
  let e = StructArray.At(arr, 2, P);
  e.a = 11; e.b = 22;
  let f = StructArray.At(arr, 2, P);
  Data.ShellPrint.Int(f.a + f.b);
  let a: int = Net.UdpOpen(0);
  let ls: int = Net.Listen(0);
  Data.ShellPrint.Int(Net.Resolve("localhost", buf, 60));
  Net.Print(buf); Data.ShellPrint("");
  Data.ShellPrint("tab:\there");
}
EOF_DIRECT_RT
sed "s#TMPDIR#$tmp#g" "$tmp/direct_rt.purple" > "$tmp/direct_rt2.purple"
./purplec "$tmp/direct_rt2.purple" --direct -o "$tmp/direct_rt"
"$tmp/direct_rt" > "$tmp/direct_rt.out"
printf '12\nhello world\n2\na.txt\n11\n0\n486921\nTWFueSBoYW5kcw==\n01000001\n0xbeef\n33\n9\n127.0.0.1\ntab:\there\n' > "$tmp/direct_rt.expected"
cmp "$tmp/direct_rt.expected" "$tmp/direct_rt.out"
echo 'Purplecode direct runtime (File/Net/encoders) tests passed.'

# GoodNative Emitter 2.2: BlowUI/GUI software renderer written in Purplecode (headless, byte-exact regression).
cat > "$tmp/blow_native.purple" <<'EOF_BLOW'
fn main() {
  Blow.Headless(480, 300)
  Blow.Theme(0)
  Blow.Begin(0)
  Blow.Panel(20, 20, 440, 260, 16)
  Blow.Label(40, 36, "BlowUI native", 2)
  Blow.Muted(40, 70, "pure Purplecode renderer", 1)
  Blow.Button(40, 100, 140, 40, "Click me")
  Blow.Toggle(200, 108, 1)
  Blow.Checkbox(260, 108, 1, "Check")
  Blow.Slider(1, 40, 180, 300, 60, 100)
  Blow.Progress(40, 210, 300, 14, 70, 100)
  Blow.Badge(360, 40, "NEW", 0x34D399)
  Blow.StyledLabel(40, 240, "Bold Italic", "!Bold, Italic, Underline", 0xFFFFFF, 1)
  Blow.End()
  Blow.SimClick(60, 110)
  Blow.Screenshot("TMPDIR/blow.ppm")
  GUI.Window.CreateSized(1, 1, "x")
}
EOF_BLOW
sed "s#TMPDIR#$tmp#g" "$tmp/blow_native.purple" > "$tmp/blow_native2.purple"
./purplec "$tmp/blow_native2.purple" --direct -o "$tmp/blow_native"
env -u DISPLAY "$tmp/blow_native" 2>/dev/null || true
test "$(head -c 15 "$tmp/blow.ppm" | head -2 | tr '\n' ' ')" = "P6 480 300 "
test "$(wc -c < "$tmp/blow.ppm")" = "432015"
echo 'GoodNative BlowUI/GUI renderer tests passed.'
