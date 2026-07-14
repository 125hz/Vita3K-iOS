# Vita3K iOS next-milestone handoff

Use this file when continuing in a new Codex task or Claude Code session. The repository is `https://github.com/125hz/Vita3K-iOS`, the working branch is `ios-port`, and the current completed implementation target is Milestone 22.

## Copy-paste prompt

```text
Continue the Vita3K iOS port in https://github.com/125hz/Vita3K-iOS on branch ios-port. First read ios/README.md, ios/PORTING.md, docs/ios-development.md, ios/NEXT-MILESTONE-HANDOFF.md, .github/workflows/ios.yml, and the latest git log/diff. Inspect the exact physical-device screenshot or diagnostic I provide from the previous milestone.

Implement exactly one bounded next instruction-family or subsystem milestone anchored by the real unsupported CPU instruction, memory fault, or unimplemented HLE NID. Batch coherent compiler instruction families when deterministic CPU-state tests can validate them; never skip an unknown instruction or fabricate execution state. Do not claim the game is playable. Preserve desktop and Android behavior. Keep platform code behind narrow interfaces. Add or update deterministic legal synthetic regression coverage runnable on Windows. Run the portable core build and tests locally.

Extend the existing .github/workflows/ios.yml workflow; do not create a duplicate workflow. It must retain workflow_dispatch, build an unsigned arm64 iphoneos IPA on a GitHub-hosted macOS runner with CODE_SIGNING_ALLOWED=NO and CODE_SIGNING_REQUIRED=NO, package Payload/Vita3K-iOS.app as artifacts/Vita3K-iOS-unsigned.ipa, and upload that IPA plus all legal testing fixtures. Never upload my game, firmware PUPs, keys, certificates, provisioning profiles, or copyrighted extracted files. Do not sign, sideload, or enable JIT; I handle those steps.

Update the milestone number in ios/Info.plist.in, ios/src/AppDelegate.mm, ios/README.md, ios/PORTING.md, and docs/ios-development.md. Commit and push the completed work to ios-port, monitor the Build unsigned iOS IPA Action until it succeeds, download the exact artifact, compute the IPA SHA-256, and give me concise physical-device test steps plus the next diagnostic I should return. Be transparent about all remaining upstream incompatibilities.
```

## Known Milestone 22 boundary

Milestone 21 was accepted on a physical device with Amagami title `PCSG00291` from `patch/eboot.bin`. Preparation still reported `module_start 0x810176B9 via lifecycle export`; the bounded attempt executed ten instructions and made zero HLE calls before reporting `No diagnostic HLE binding exists for NID 0x00000000.`

The rebound ARM import trampoline is `SVC; MOV pc,lr; inline NID`. The interpreter previously used the inline NID only if a diagnostic handler already existed and otherwise fell back to `r12`, which remained zero in this real call. Milestone 22 validates the canonical trampoline regardless of handler availability, reports its real inline NID and upstream name, captures SVC/LR addresses plus `r0`–`r3` and `r12`, checks the module import inventory, and still stops without inventing unimplemented HLE behavior. Install the Milestone 22 artifact, prepare `PCSG00291`, run the one-shot attempt, and use the complete HLE context to implement the correct coherent HLE subsystem next.

## Local Windows verification

```powershell
git switch ios-port
git pull --rebase origin ios-port
cmake -S ios/core -B build-core-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-core-tests --config Release --parallel
ctest --test-dir build-core-tests -C Release --output-on-failure
```

The smoke-test executable also accepts output switches used by CI, including `--emit-m22-install-zip-fixture artifacts/milestone22-inline-hle-diagnostic.zip`. Generated build and artifact directories are not source and should not be committed.

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
