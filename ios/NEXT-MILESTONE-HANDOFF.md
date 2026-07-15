# Vita3K iOS next-milestone handoff

Use this file when continuing in a new Codex task or Claude Code session. The repository is `https://github.com/125hz/Vita3K-iOS`, the working branch is `ios-port`, and the current completed implementation target is Milestone 30. Read `MENU-BOOT-CHECKLIST.md` for the full first-menu critical path.

## Copy-paste prompt

```text
Continue the Vita3K iOS port in https://github.com/125hz/Vita3K-iOS on branch ios-port. First read ios/README.md, ios/PORTING.md, ios/VION-ANALYSIS.md, docs/ios-development.md, ios/NEXT-MILESTONE-HANDOFF.md, .github/workflows/ios.yml, and the latest git log/diff. Inspect the exact physical-device screenshot or diagnostic I provide from the previous milestone.

Implement exactly one bounded next instruction-family or subsystem milestone anchored by the real unsupported CPU instruction, memory fault, or unimplemented HLE NID. Batch coherent compiler instruction families when deterministic CPU-state tests can validate them; never skip an unknown instruction or fabricate execution state. Do not claim the game is playable. Preserve desktop and Android behavior. Keep platform code behind narrow interfaces. Add or update deterministic legal synthetic regression coverage runnable on Windows. Run the portable core build and tests locally.

Extend the existing .github/workflows/ios.yml workflow; do not create a duplicate workflow. It must retain workflow_dispatch, build an unsigned arm64 iphoneos IPA on a GitHub-hosted macOS runner with CODE_SIGNING_ALLOWED=NO and CODE_SIGNING_REQUIRED=NO, package Payload/Vita3K-iOS.app as artifacts/Vita3K-iOS-unsigned.ipa, and upload that IPA plus all legal testing fixtures. Never upload my game, firmware PUPs, keys, certificates, provisioning profiles, or copyrighted extracted files. Do not sign, sideload, or enable JIT; I handle those steps.

Update the milestone number in ios/Info.plist.in, ios/src/AppDelegate.mm, ios/README.md, ios/PORTING.md, and docs/ios-development.md. Commit and push the completed work to ios-port, monitor the Build unsigned iOS IPA Action until it succeeds, download the exact artifact, compute the IPA SHA-256, and give me concise physical-device test steps plus the next diagnostic I should return. Be transparent about all remaining upstream incompatibilities.
```

## Known Milestone 30 boundary

Milestone 29 was accepted on a physical device with Amagami title `PCSG00291` from `patch/eboot.bin`. It advanced to 138 instructions and six HLE calls, retained DSO handle `0x812C7690`, three termination registrations, and the acquired guard `0x812C75D8`. `memalign(0x10, 0x180)` succeeded with guest address `0x90000000`, zero failures, and 384 live/peak bytes. Execution then stopped at `0x81001F82` on `EB00 00CE`, the legal `ADD.W r0, r0, lr, LSL #3` encoding.

Milestone 30 corrects high-register validation across the complete Thumb-2 shifted-register ALU/test family: LR is legal as a source, shifted source, or destination, while SP and PC misuse remains an explicit boundary. It also raises the safe one-shot interpreter ceiling from 256 to 4096 instructions. Install the Milestone 30 artifact, prepare `PCSG00291`, and run the attempt once. It should advance past 138 instructions while preserving all six HLE calls and heap/runtime diagnostics. Return the complete next CPU, memory, HLE, return, or instruction-limit boundary; never skip unknown behavior.

## Local Windows verification

```powershell
git switch ios-port
git pull --rebase origin ios-port
cmake -S ios/core -B build-core-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-core-tests --config Release --parallel
ctest --test-dir build-core-tests -C Release --output-on-failure
```

The smoke-test executable also accepts output switches used by CI, including `--emit-m30-install-zip-fixture artifacts/milestone30-thumb2-lr-shifted-register.zip`. Generated build and artifact directories are not source and should not be committed.

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
