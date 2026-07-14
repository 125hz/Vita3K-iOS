# Vita3K iOS next-milestone handoff

Use this file when continuing in a new Codex task or Claude Code session. The repository is `https://github.com/125hz/Vita3K-iOS`, the working branch is `ios-port`, and the current completed implementation target is Milestone 24.

## Copy-paste prompt

```text
Continue the Vita3K iOS port in https://github.com/125hz/Vita3K-iOS on branch ios-port. First read ios/README.md, ios/PORTING.md, ios/VION-ANALYSIS.md, docs/ios-development.md, ios/NEXT-MILESTONE-HANDOFF.md, .github/workflows/ios.yml, and the latest git log/diff. Inspect the exact physical-device screenshot or diagnostic I provide from the previous milestone.

Implement exactly one bounded next instruction-family or subsystem milestone anchored by the real unsupported CPU instruction, memory fault, or unimplemented HLE NID. Batch coherent compiler instruction families when deterministic CPU-state tests can validate them; never skip an unknown instruction or fabricate execution state. Do not claim the game is playable. Preserve desktop and Android behavior. Keep platform code behind narrow interfaces. Add or update deterministic legal synthetic regression coverage runnable on Windows. Run the portable core build and tests locally.

Extend the existing .github/workflows/ios.yml workflow; do not create a duplicate workflow. It must retain workflow_dispatch, build an unsigned arm64 iphoneos IPA on a GitHub-hosted macOS runner with CODE_SIGNING_ALLOWED=NO and CODE_SIGNING_REQUIRED=NO, package Payload/Vita3K-iOS.app as artifacts/Vita3K-iOS-unsigned.ipa, and upload that IPA plus all legal testing fixtures. Never upload my game, firmware PUPs, keys, certificates, provisioning profiles, or copyrighted extracted files. Do not sign, sideload, or enable JIT; I handle those steps.

Update the milestone number in ios/Info.plist.in, ios/src/AppDelegate.mm, ios/README.md, ios/PORTING.md, and docs/ios-development.md. Commit and push the completed work to ios-port, monitor the Build unsigned iOS IPA Action until it succeeds, download the exact artifact, compute the IPA SHA-256, and give me concise physical-device test steps plus the next diagnostic I should return. Be transparent about all remaining upstream incompatibilities.
```

## Known Milestone 24 boundary

Milestone 23 was accepted on a physical device with Amagami title `PCSG00291` from `patch/eboot.bin`. It dispatched `__cxa_set_dso_handle_main` once, retained DSO handle `0x812C7690`, and advanced to 16 instructions before stopping at Thumb-2 `F05F 0B00` (`MOVS.W r11, #0`) at `0x810175BE`. The look-ahead began with `F8DD A000` (`LDR.W r10, [sp]`), followed by already-supported `MOVW`, high-register `MOV`, and `MOVT` construction.

Milestone 24 implements complete Thumb-2 modified-immediate ALU and wide immediate single-register memory families, including exact flags, signed loads, access widths, addressing/writeback modes, invalid encodings, and overflow/fault behavior. Install the Milestone 24 artifact, prepare `PCSG00291`, and run the one-shot attempt once. It must advance through both captured instructions while preserving the successful libc state. Use the complete next CPU, memory, HLE, return, or budget boundary to choose another broad, coherent family or subsystem.

## Local Windows verification

```powershell
git switch ios-port
git pull --rebase origin ios-port
cmake -S ios/core -B build-core-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-core-tests --config Release --parallel
ctest --test-dir build-core-tests -C Release --output-on-failure
```

The smoke-test executable also accepts output switches used by CI, including `--emit-m24-install-zip-fixture artifacts/milestone24-thumb2-runtime-families.zip`. Generated build and artifact directories are not source and should not be committed.

## GitHub Actions and artifact verification

```powershell
git add -A
git commit -m "Implement Vita3K iOS core milestone N"
git push origin ios-port

gh run list --repo 125hz/Vita3K-iOS --branch ios-port --workflow "Build unsigned iOS IPA"
gh run watch RUN_ID --repo 125hz/Vita3K-iOS --exit-status
gh run download RUN_ID --repo 125hz/Vita3K-iOS --name ARTIFACT_NAME --dir test-artifacts\milestoneN
Get-FileHash test-artifacts\milestoneN\Vita3K-iOS-unsigned.ipa -Algorithm SHA256
```

The existing Action is authoritative. It builds an unsigned physical-device IPA and includes legal synthetic fixtures. Do not add repository secrets or signing material.

## Milestone discipline

- One coherent, deterministically tested instruction-family or subsystem batch per milestone.
- Never skip an unsupported instruction to discover later boundaries; preserve exact guest state.
- Keep the attempt interpreter-only and bounded until interpreter boot is stable.
- A blue Metal clear is not guest rendering.
- Host touch/controller capture is not Vita guest input HLE.
- Keep Amagami, `fontpkg.pup`, `preinstall.pup`, `psvupdat.pup`, keys, and extracted proprietary files local.
- The user handles IPA signing, sideloading, and JIT testing.
