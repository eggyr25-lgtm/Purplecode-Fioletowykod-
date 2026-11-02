# Milestone 1 1.2 release notes

## Usability
- Rewrote the main README as a step-by-step guide: requirements, build, first UEFI app, emulator workflow, API reference, common errors, and target differences.
- Added `examples/uefi_diagnostics.purplebc`, a console smoke-test application using only APIs implemented by the current UEFI target.
- Added a PE/COFF artifact check for the diagnostics example to `boot-tests.sh`.
- Rewrote boot status and native-backend notes to distinguish actual features from future work and avoid implying that hosted APIs work in UEFI.

## Scope reminder
This release improves the developer experience and test coverage; it does not magically add firmware services that the implementation does not yet provide. UEFI filesystem protocols, GOP pixel drawing, input, hardware drivers, and a complete UEFI OS remain future work. The UEFI target still uses Clang/LLD for final code generation.
