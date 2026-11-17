# ns-autoverify 0.2 (fast clear): rollout, rollback, tests

The executable, step-by-step version for the hardware driver is
`/home/intpa/ssbu-switch-remote-work/card-jobs/nsav2-p0-probe-cf2390e6/STEPS.md`. This file explains why the steps
are what they are. All console work runs under the operator's card rules: backup, readback, receipt. If a dialog is
left showing, the operator's standing rule allows HOME → Check → Done.

## Order: P0 probe → Stage 1 → Stage 2

1. **P0, the probe NRO** (no sysmodule). It answers the unknowns before anything persistent is installed:
   - Does ns's `IsDataCorruptedResult` work with the assumed layout (in u32, out u8)? The reference table covers
     `0x0`, `0x4A8` and two fs corruption results.
   - Does setting the terminate result to `0x4A8` (cmd 56) reproduce the HOME "!" badge and dialog, with no crash
     needed?
   - Does `ClearApplicationTerminateResult` (57) alone remove both?
   - If not (outcomes O2/O3), the dumps show which `ApplicationView` bytes or record fields stay set. The next call
     is then researched; nothing is tried blindly. The sysmodule stays uninstalled until O1.
2. **Stage 1:** `exefs.nsp` + `toolbox.json` + `observe-only`, **no `boot2.flag`**. Started by hand in ovlSysmodules
   (A starts it and, on this card, creates the flag; Y deletes the flag). Then a listing must prove the flag is
   absent. Tests:
   - observe (probe X → `observed`);
   - active (`nsav2-active` → `cleared`, no badge, no dialog);
   - a real crash;
   - the kill switch.
3. **Stage 2:** only `flags/boot2.flag`, in its own later session, after Stage 1 passed. Reboot, then HOME must be
   reached.

## Program ID `4200000000004156`

- **Homebrew range:** `0x42…`.
- **Card convention:** the operator's own `4200000000005352` "SR" and `4200000000004150` "AP"; `0x4156` = "AV".
- **Free:** absent from every local card listing and receipt; the driver re-checks with a listing before the
  install.

## Footprint and fail-safes

- **Small at boot:** 192 KiB static heap and a 64 KiB stack (boot2 aborts the boot if an SD sysmodule cannot be
  created).
- **Normal path:** a handful of small IPCs, no buffer.
- **Crash-loop guard:** a `busy` marker is written around init, each clear and each manual verify. If one is found at
  start → `disabled` and exit.
- **What it may write to ns:**
  - `ClearApplicationTerminateResult` for SSBU, only after ns positively classified both results as not data
    corruption;
  - `RequestVerifyApplication`, only on a manual `verify-now`.

## Rollback

- **Soft:** `nsav2-off` (the `disabled` file; exit within 60 s and at every later start). `nsav2-rearm` undoes it.
- **Full:** `nsav2-rollback` (module files and the probe), then reboot.
- **If HOME is not reachable:** power-cycle once (the stale-marker guard). Otherwise hekate UMS: delete
  `atmosphere/contents/4200000000004156/`.

## Real-corruption safety

The gate keeps the dialog whenever ns classifies the terminate result or the raw result as data corruption, and
whenever any query fails. The host tests cover every branch. On the console, P0's reference table shows ns
classifying fs corruption results as corrupt. A real corrupt-content crash cannot be produced safely: the console is
internet-blocked, so there would be no redownload. So that case is covered by ns's own classification plus the
fail-closed gate, not by a live test.
