# ns-autoverify: safe rollout, rollback and test plan

The console is remote, and a sysmodule that faults at boot could need physical access to recover. Every step below is
for the **hardware driver**, under the operator's card rules: backup, readback, receipt. Nothing here has been run.
The author has no console or card access.

## Program ID: `4200000000004156`

- **Homebrew range:** the `0x42…` prefix is the community homebrew-sysmodule range. Nintendo's system modules
  (`0100000000000xxx`) and applications (`01…` with a high byte of 0x01) are elsewhere.
- **Same convention as this card:** the operator's own sysmodules on this card use `0x42` + two ASCII letters:
  - `4200000000005352` = "SR", switch-remote (parked)
  - `4200000000004150` = "AP", sys-autopilot 1.5.0
  - `0x4156` = "AV" (autoverify) follows the same pattern.
- **Local collision check:** across every receipt, listing and doc under `ssbu-switch-remote{,-work}`,
  `smash-training-lobby/artifacts`, `ssbu-mod-playbook` and `ssbu-switch-remote-research`, the only
  `atmosphere/contents/<id>` values are `01006A800016E000`, `4200000000005352` and `4200000000004150`.
  `4200000000004156` appears nowhere.
- It is not one of the well-known public homebrew IDs:
  - ldn_mitm `4200000000000010`
  - sys-ftpd `420000000000000E`
  - nx-ovlloader `420000000007E51A`
  - sys-botbase `430000000000000B`
  - sys-clk `00FF0000636C6BFF`
  - MissionControl `010000000000BD00`
  - emuiibo `0100000000000352`
- **Precondition for the driver:** an FTP listing of `atmosphere/contents/` shows no `4200000000004156` folder before
  the install.

## Footprint and fail-safes (why a boot fault is unlikely, and bounded if it happens)

- **Memory.** About 0.2 MiB static heap plus a 64 KiB stack plus code at process creation. boot2 aborts the boot only
  when an SD sysmodule cannot be created for lack of memory, and this one is small. The 2 MiB verify buffer is taken
  with `svcSetHeapSize` only while a verify runs, and released afterwards.
- **No aborts.** No `fatalThrow` / `diagAbort`. `__appInit` is empty. Every service is opened in `main()`, and any
  failure logs and exits.
- **Unsupported firmware.** Any firmware except 23.x → log `unsupported-firmware` and exit.
- **SD first.** If the SD cannot be mounted, the process exits at once; without it the kill switch cannot be honoured.
- **Crash-loop guard.** A `busy` marker is written before service init and before each verify, and removed after.
  - If a later start finds the marker (the previous run faulted, was killed, or lost power mid-verify), it creates
    `disabled` and exits.
  - Worst case: one fatal, then a normal boot with the module disabled.
  - The guard does not cover a fault between process start and the SD mount (only `sm` and `fs` init). There, recovery
    needs the SD removed or hekate UMS.
- **Bounded work.**
  - Idle wake-up every 60 s, or on a pgl event.
  - At most 32 events drained per wake.
  - A verify is hard-capped at 45 min, then cancelled.
  - At most 4 verifies per run.
  - Each wait is bounded; a failing wait exits rather than spinning.
- **Default ns mutations: none.** In its default configuration the module's only call that changes ns state is the
  exact `RequestVerifyApplication(appid, 0x7, 0x100000)` qlaunch's "Check" makes. So ns behaves exactly as if Check had
  been pressed, including keeping the dialog when it finds corruption.
  - `ClearApplicationTerminateResult` is called only after a PASS, and only if the operator creates
    `config/ns-autoverify/clear-terminate-result`, which should happen only when T1 shows that qlaunch's Check clears
    it too.

## Files the driver may touch (all optional)

| Path | Effect |
| --- | --- |
| `sd:/config/ns-autoverify/disabled` | Kill switch. Read at start (exit) and every ≤60 s (exit). Auto-created after an interrupted run. |
| `sd:/config/ns-autoverify/observe-only` | Never verify; log the snapshot and "would verify". |
| `sd:/config/ns-autoverify/verify-now` | One-shot manual trigger, consumed on read; deferred while an application runs. |
| `sd:/config/ns-autoverify/clear-terminate-result` | Also clear ns's terminate result after a PASS (see T1). |
| `sd:/config/ns-autoverify/status.txt` | Read-only for the driver: `state=idle\|observed\|verifying\|pass\|fail\|cancelled\|timeout\|error\|disabled\|unsupported-firmware …` |
| `sd:/atmosphere/logs/ns-autoverify.log` (+ `.1`) | Read-only for the driver: capped at 64 KiB, one rotation. |

## Stage 1: installed without `boot2.flag`, started by hand

1. **Card session.** Pre-listing and backup per the card rules.
   - ADD `atmosphere/contents/4200000000004156/exefs.nsp` and `toolbox.json` from `out/stage1/` (nothing else).
   - ADD an empty `config/ns-autoverify/observe-only`.
   - Read back both, with sha256 receipts.
2. **Start it on demand** from ovlSysmodules (Tesla overlay; `toolbox.json` has `requires_reboot: false`).
   - Follow the procedure already documented for sys-autopilot (`ssbu-switch-remote/DESIGN.md` §12): **A, then Y
     once**. On this card's ovlSysmodules, A starts it and also CREATES `flags/boot2.flag`; Y deletes the flag.
   - Then FTP-list `atmosphere/contents/4200000000004156/` and confirm **no `flags/` folder**. This is mandatory: a
     flag left behind silently turns Stage 1 into Stage 2.
3. **Read** `status.txt` (expect `state=idle … term=0x… flags=0x…`) and the log's `snapshot[start]` line.
4. **Run the tests T1-T3 below.** Stop the module from ovlSysmodules between sessions, or leave it; it is idle-cheap.

Stage 1 passes when:
- T1 and T2 are green;
- T3 is green or explicitly waived by the operator;
- no fatal / creport for `4200000000004156` exists in `atmosphere/crash_reports` or `fatal_reports`.

## Stage 2: add `boot2.flag`

1. **Card session.** ADD only `atmosphere/contents/4200000000004156/flags/boot2.flag` (from `out/stage2/`), with
   readback.
2. **Reboot once.** The visual check that HOME is reached is the boot-safety proof. Then read `status.txt`
   (`state=idle`) and the log (`ns-autoverify 0.1.0 start` within about 70 s of boot).
3. **Repeat T2** once from a cold boot.

## Rollback (any stage)

- **Soft (no reboot):** FTP-create `config/ns-autoverify/disabled`. The module exits within 60 s and on every later
  start.
- **Full:** with DBI FTP, delete `atmosphere/contents/4200000000004156/` (the whole folder: exefs.nsp, toolbox.json,
  flags/). Optionally also delete `config/ns-autoverify/` and `atmosphere/logs/ns-autoverify.log*`. Then reboot.
- **If HOME is not reachable** (boot fault before the SD mount):
  - Power-cycle once; the stale-marker guard disables the module on the next boot.
  - If that fails, it needs local hands: hekate UMS, or remove the SD, then delete the folder above. The same recovery
    is recorded for `4200000000005352`.

## Test plan

### T1: observe-only, find which ns value carries the flag (resolves DESIGN unknowns U1-U3)

- **Trigger:** with the module running in `observe-only`, the next real SSBU crash.
  - The campaign hits these regularly (e.g. LEAD-L2's Data Abort).
  - The deterministic repro is CRASH-0900: L+R+A during the fighter load, swap On, cold fighter.
  - No public ns call is known to set the "needs verify" view state directly. `SetApplicationTerminateResult`
    (cmd 56) only writes the terminate result. Using it as a simulator is deliberately not built: it would add an ns
    mutation to the test tooling before we know it reproduces the real state.
- **Evidence:**
  - log lines `event crash application` → `event exit application crashed=1` → after 15 s,
    `decision reason=crash gate=observe` → `snapshot[observe]`;
  - the HOME still shows the "!" badge plus the dialog.
- **Then, as today:** HOME → Check → "No corrupted data was found" → Done. FTP-create `verify-now`, which in
  observe-only takes one more `snapshot[observe]` after the manual check.
- **Diff** `snapshot[start]` (clean) vs `snapshot[observe]` (flagged) vs the post-Check snapshot:
  - which bytes of the 0x50 `view`, which `term` and which record `attributes` change;
  - whether the manual Check zeroes `term`.
- **Decide:**
  - If Check zeroes `term` but our verify alone (T2) does not → create `clear-terminate-result`.
  - Record the result in `smash-training-lobby/artifacts/switch-corrupt-dialog/DESIGN.md` §1.3.

### T2: auto-verify clears it and SSBU launches

1. Delete `observe-only`. Restart the module, or keep the running instance; the file is read on every decision.
2. Crash SSBU as in T1. Do **not** press anything on the HOME dialog.
3. Watch `status.txt`:
   - `verifying permille=…` about 15 s after the crash exit;
   - then `pass … flags_before=… flags_after=…` after about 8-9 min (the manual check took 8 min 14 s).
4. Stills:
   - HOME tile without the "!" badge;
   - launch SSBU from HOME → no "Corrupted data" dialog → title screen.
   - A receipt is not proof; the still is.
5. **Pass:** `pass`, the badge is gone, the launch has no dialog.
   - `pass` but badge or dialog still present → set `clear-terminate-result` per T1 and repeat.
   - Still present after that → the carrier is something else; stop and report the snapshots.
6. **Race variant:**
   - Launch SSBU while `verifying`. Expect `cancelled`, the dialog still shown (safe), and the next exit re-queues the
     verify.
   - The driver must not press HOME's own Check while `status=verifying`.

### T3: Stage 1 restart-safety

- Stop the module in ovlSysmodules during a verify. Restart it → expect `state=disabled cause=stale-busy-marker`.
- Delete `disabled` and restart → `idle`. This proves the crash-loop guard without any fault injection.

### T4: real corruption still surfaces

**Not safely testable on SSBU, and must not be attempted there.**
- The console is internet-blocked, so a corrupted SSBU install could not be redownloaded.
- `CorruptContentForDebug` / `CorruptApplicationForDebug` (ns 1004/1001) are debug-gated, with an unknown retail
  effect, and are not used.

What is proven without touching SSBU:
- **(a) Design.** In the default configuration the module's only ns mutation is the qlaunch-identical verify request.
  If ns finds corruption, ns keeps the flag exactly as after a manual Check that finds corruption. The module never
  clears anything after a non-PASS verdict, and a PASS requires both `Get` and `GetDetailResult` to be zero.
- **(b) Host tests.** `make test` (65 checks) covers every non-PASS verdict → no clear, the allowlist and the gating.

An optional on-console negative test needs explicit operator approval for a card write and a disposable title:
1. Choose a small title whose NSP is kept on the PC so DBI can reinstall it, never SSBU.
2. Build a test variant: `make EXTRA_CFLAGS=-DAV_TEST_EXTRA_PROGRAM_ID=0x<id>ULL`. Never leave it installed.
3. Back up, then flip one ciphertext byte in one of that title's NCAs under `Nintendo/Contents/registered/`.
4. FTP-create `verify-now` containing the test title's 16-hex-digit program id on one line.
   - Without the test build, the allowlist refuses any id but SSBU's; the log shows `gate=refuse`.
   - Expect `fail` with non-zero `get_rc`/`detail_rc`; the HOME Check / Redownload path still offered.
5. Delete the test title, reinstall the clean NSP, and reinstall the normal build.
