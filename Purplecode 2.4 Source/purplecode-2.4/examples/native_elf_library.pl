PLStartMarekPCStartHeader
FORMAT = PurpleLib/2.0
NAME = NativeELF
NAMESPACE = Native.ELF
VERSION = 1.0.0
TYPE = Purple Lib
LANGUAGE = Purplecode
DESCRIPTION = Explicit ELF execution helper; native execution is governed by PURPLECODE CONFIG
AUTHOR = Purplecode Examples
LICENSE = MIT
ABI_VERSION = 2
EXPORTS = Native.ELF.Run
CAPABILITIES = process.execute
ELF_MODULES = native-tools/**
FEATURES = trusted-elf-execution,virtual-mode-aware
ICON_VECTOR = <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="12" fill="#663399"/><path d="M18 15h28v34H18z" fill="none" stroke="#fff" stroke-width="4"/><path d="M25 25h14M25 32h14M25 39h9" stroke="#fff" stroke-width="3"/></svg>
PLEndMarekPCEndHeader

// The runtime checks SafeCodeOnly and VirtualMode when this is called.
fn Native.ELF.Run(path) {
    return Program.ELF64.Run(path)
}
