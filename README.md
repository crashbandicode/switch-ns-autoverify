# ns-autoverify

A minimal Atmosphère sysmodule (libnx, C). After **Super Smash Bros. Ultimate** (`01006A800016E000`) crashes, it
runs the **same official content verification that the HOME "Check" button runs**. That clears HOS's false-positive
"Corrupted data has been detected. Please run a check for corrupt data." state the same way a manual check does.
A failed verification changes nothing, so real corruption still shows the dialog.

- Program ID `4200000000004156` ("AV"). Firmware 23.x only. Atmosphère 1.12.x.
- Design and root cause: `smash-training-lobby/artifacts/switch-corrupt-dialog/DESIGN.md`.
- Rollout, rollback and test plan: [`docs/ROLLOUT.md`](docs/ROLLOUT.md).

## What it does

1. **Start-up.**
   - Mount the SD, then honour the `disabled` kill switch and the crash-loop guard (stale `busy` marker →
     `disabled`).
   - Read the firmware; anything but 23.x → exit.
   - Open `pm:dmnt`, `pgl`, `ns:am2` and a pgl shell-event observer. Any failure → exit.
2. **Watch** pgl shell events: application Start / Crash (Exception) / Exit.
3. **Trigger.** A verify is queued when:
   - the tracked application crashed and exited (15 s settle);
   - or at start-up ns reports a non-zero `GetApplicationTerminateResult` for SSBU;
   - or the operator drops a `verify-now` file.
4. **Gate.**
   - Allowlist (SSBU only); no application running (otherwise defer); `observe-only` (log only).
   - At most 4 verifies per run; never again after a buffer that could not be released.
5. **Verify.**
   - `ns` `IApplicationManagerInterface` cmd 1003 `RequestVerifyApplication(id, 0x7, 0x100000-byte TransferMemory)`,
     exactly qlaunch's arguments.
   - Wait in bounded 5 s slices. Cancel if an application starts or after 45 min.
   - PASS needs both `Get` and `GetDetailResult` to be 0.
6. **After.**
   - Snapshot again (ApplicationView raw bytes, terminate result, record).
   - Only if PASS and the operator created `clear-terminate-result`: `ClearApplicationTerminateResult` (cmd 57).
   - Otherwise nothing.

Logs: `sd:/atmosphere/logs/ns-autoverify.log` (64 KiB cap, one rotation). Status line:
`sd:/config/ns-autoverify/status.txt`.

## Build

```
make            # out/stage1/atmosphere/contents/4200000000004156/{exefs.nsp,toolbox.json}   (no boot2.flag)
make stage2     # out/stage2/atmosphere/contents/4200000000004156/flags/boot2.flag
make test       # host tests of the decision logic (system cc), 76 checks
make check-host # type-check main.c against libnx headers with host clang (no devkitPro needed; not a build)
```

The console build needs devkitPro (devkitA64 + libnx + `elf2nso` / `npdmtool` / `build_pfs0`) with `DEVKITPRO` set.
The operator's earlier sysmodule was built with devkitA64 r30 + libnx 4.12.0 on the Yoga host
(`ssbu-switch-remote/tools/build_sysmodule.ps1`). The same toolchain works here.

## Layout

```
source/main.c        libnx side: services, pgl events, verify, logging (no fatalThrow / diagAbort anywhere)
source/policy.{c,h}  pure decision logic (firmware gate, allowlist, verdict, clear rule, gate, crash tracker)
tests/test_policy.c  host tests;  tests/shim/  three tiny headers that let host clang parse libnx
ns-autoverify.json   npdm (services: fsp-srv set:sys pm:dmnt pgl ns:am2; core 3; no debug syscalls)
toolbox.json         ovlSysmodules entry (requires_reboot: false, so Stage 1 can start it by hand)
docs/ROLLOUT.md      staged install, rollback, tests
```
