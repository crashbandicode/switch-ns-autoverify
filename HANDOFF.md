# Handoff: ns-autoverify

- **Objective.** Operator decision 2026-11-17 ("go with 1"): a sysmodule that, after an SSBU crash, runs the same
  official verification as HOME → Check, and clears the flag only through a passing verification.
- **Owner.** A spawned research/implementation worker for primary `c07b3449`.
- **Repo.** `/home/intpa/switch-ns-autoverify` (local git, branch `main`, never pushed).
- **Design.** `/home/intpa/smash-training-lobby/artifacts/switch-corrupt-dialog/DESIGN.md`.

## State (2026-11-17)

- **Implemented:** `source/main.c`, `source/policy.{c,h}`, npdm, toolbox.json, Makefile, `docs/ROLLOUT.md`.
- **Tested on this host:**
  - `make test` → 76 checks, 0 failures.
  - `make check-host` → main.c + policy.c type-check against libnx headers (local source `b156b477`) with
    `-Wall -Wextra -Werror`, exit 0. A deliberately wrong call is rejected, so the check is not vacuous.
- **BUILT on the Yoga (2026-11-17 12:16Z), commit `1838af18`.** Toolchain: devkitA64 GCC 16.1.0, libnx 4.12.0,
  `-Wall -Wextra -Werror`, 0 warnings; host tests 76/0 there too. Built with `tools/build_yoga.ps1`; booking
  `285044ec` released, lane deleted.
  - `exefs.nsp` sha256 `b337410566841c955ad5420d6c2dde4a509e7ce86d4f28e37c55acacc55992ff` (96,554 B).
  - `toolbox.json` sha256 `31a05378b892ea32597f9d295fb9ddb03d6b0f9c33b06a30e79454be03e70da0`.
  - Artifacts: `smash-training-lobby/artifacts/switch-corrupt-dialog/build-1838af18/`.
  - `4e71f3f3` did not build: libnx 4.12.0 spells the pgl observer API differently. Its logs are in
    `build-4e71f3f3/`.
- **Install job:** `/home/intpa/ssbu-switch-remote-work/card-jobs/nsav-s1-1838af18/` (STEPS.md), plus siblings
  `nsav-s1b-noflag`, `nsav-t1-snap`, `nsav-t2-active`, `nsav-off`, `nsav-rollback`. Staged, not run, no go.txt.
  - It needs `own_install-ns-autoverify.patch` applied in `ssbu-switch-remote` first. Without it, own_install.py
    refuses these paths.
  - In a scratch copy the patched suite passed 45/45. All six jobs dry-ran ok on a fake UMS card.
- **Earlier status, superseded by the Yoga build above:** this host has no devkitPro (`/opt/devkitpro`, `~/devkitpro` and `$DEVKITPRO` are all absent), so
  there is no `exefs.nsp` yet. Missing: devkitA64 (aarch64-none-elf gcc + newlib), libnx (built library), and
  `elf2nso`, `npdmtool`, `build_pfs0` (devkitPro `switch-tools`).
  - Options: build on the Yoga host (devkitA64 r30 + libnx 4.12.0, as for `4200000000005352`), or the operator
    approves pulling the `devkitpro/devkita64` container image here (docker/podman are installed; no image is
    present).
  - Expect GCC (devkitA64) diagnostics that clang's check could miss. Run `make` with `-Werror` and fix before any
    install.
- **NOT run on the console.** No card or console access by design. The hardware driver installs and tests per
  `docs/ROLLOUT.md` (Stage 1 without boot2.flag; Stage 2 adds it).

## Risks and unknowns

- Which ns value carries the "needs verify" state on 23.5.0; whether a passing verify alone clears it. T1 resolves
  both, and `clear-terminate-result` is the switch if qlaunch also clears the terminate result.
- `ns` accepting `RequestVerifyApplication` from a non-qlaunch `ns:am2` client: the switchbrew permission mask
  0x7FF says yes; untested.
- GCC/newlib-specific compile issues not visible to the clang check.

## Next commands

```
# on a devkitPro host, from a checkout of this repo:
make test && make && make stage2
sha256sum out/stage1/atmosphere/contents/4200000000004156/exefs.nsp
```
