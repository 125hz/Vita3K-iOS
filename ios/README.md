# Vita3K iOS bootstrap

This directory is an experimental, device-only iOS application for a future Vita3K port. It builds an unsigned IPA containing a UIKit/Metal host, file-backed logging, sandbox storage, and the first cross-compiled slice of upstream Vita3K code. It now loads and runs the `module_start` of a deliberately tiny synthetic Vita ELF inside a bounded guest-thread context, but it does **not** yet execute ordinary Vita homebrew or games.

The separation is intentional: upstream's current Apple target is a macOS desktop application using Qt, Cocoa, SDL, MoltenVK, and desktop-oriented dependency builds. Those pieces cannot simply be linked into an iOS application.

## Current result

- `arm64` iPhone/iPad application bundle
- unsigned IPA produced by GitHub Actions
- UIKit lifecycle and an `MTKView` rendering host
- unified logging plus `Documents/vita3k.log`
- a `vita3k_ios_core` static library containing Vita3K's ARM instruction encoder and NID database
- on-device self-tests proving the upstream core slice is linked and executing
- sandbox directories under `Documents/Vita3K`, with discovery of legal `.vpk`, `.self`, `.elf`, `.bin`, and `.sfo` import candidates
- bounded header probing for Vita SELF/ELF, VPK/ZIP, and PARAM.SFO candidates before any guest-memory mapping
- a portable 4 GiB Vita guest-address-space reservation with one-page commit, read/write, decommit, and protection diagnostics
- validated ELF `PT_LOAD` segment plans with 32-bit address-overflow and file-range checks
- checked guest-segment mapping with file-byte copying, BSS zero-fill, final host-page protection, readback, and unmapping diagnostics
- batch mapping that merges permissions when adjacent Vita segments share a 16 KiB iPhone host page
- loading and full byte readback of the first valid fixed-address plain Vita ELF in `Documents/Vita3K/imports`
- extraction of the Vita module name/NID and inventory of `PT_SCE_RELA` relocation payloads
- bounded application of Vita relocation formats 0–9 and common ARM/Thumb relocation codes, with verified writes to protected guest pages
- bounded parsing of long/short import records, export records, and their function/variable/TLS NID tables
- a bounded ARM-mode execution harness with register/PC state, MOVW, MOVT, ADD-immediate, SVC, and BX decoding
- a reusable NID-to-HLE dispatcher seam, proven by one synthetic SVC call and verified return-register state
- validated extraction of `module_start` from Vita module metadata, including executable-range and temporary-stack checks
- a bounded single-thread runner implementing minimal `sceKernelGetThreadId` and `sceKernelExitThread` semantics
- an eight-instruction synthetic module that observes its guest thread UID and exits with a verified status
- a narrow C++ `CoreBridge` seam for additional core integration
- no signing credentials or provisioning profiles in CI

See [PORTING.md](PORTING.md) for the integration backlog and known blockers. See [docs/ios-development.md](../docs/ios-development.md) for the complete Windows-first workflow.

## Build contract

The iOS shell is selected before upstream desktop dependencies are configured:

```bash
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DVITA3K_BUILD_IOS=ON \
  -DVITA3K_IOS_LINK_CORE=ON

cmake --build build-ios --config Release --target Vita3KiOS -- \
  -sdk iphoneos CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
```

This command requires macOS and Xcode; the GitHub Actions workflow runs it remotely for Windows contributors.

`VITA3K_IOS_LINK_CORE=ON` is now required. The port can reserve guest address space, map a fixed-address plain `ET_SCE_EXEC` file, apply checked relocations, validate its module-info header, inventory import/export NIDs, and execute a synthetic `module_start` through a single guest-thread context. SELF decoding, relocatable ELF rebasing, imported-stub rewriting, thread scheduling, broader production HLE coverage, the full CPU backend, renderer, audio, and input remain tracked in `PORTING.md`.

Milestone 7 reads the real `module_start` field rather than treating ELF `e_entry` as executable code, starts a bounded synthetic guest thread with the Vita module ABI registers, dispatches `sceKernelGetThreadId`, and stops through `sceKernelExitThread`. The acceptance program loads NIDs directly into `r12` before `SVC`; normal compiled import stubs are not rewritten yet. This is intentionally not advertised as a general ARMv7 interpreter: conditional execution, Thumb/Thumb-2, loads/stores, general branches/calls, floating point/NEON, exceptions, atomics, and most data-processing instructions remain unsupported. Only the first valid fixed-address plain ELF is retained in guest memory; relocatable ELF and SELF containers remain probe-only.
