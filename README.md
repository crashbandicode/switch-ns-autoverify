# ns-autoverify (0.2.2: fast clear) + nsav-probe (0.4.0)

When **Super Smash Bros. Ultimate** (`01006A800016E000`) terminates abnormally, ns records a terminate result and
qlaunch shows "Corrupted data has been detected. Please run a check for corrupt data." at the next launch. A manual
check then takes about 8 minutes and finds nothing. This Atmosphère sysmodule clears that state **immediately**,
but only when **ns itself** says the result is not a data-corruption result.

- Program ID `4200000000004156` ("AV").
- Tested on: Super Smash Bros. Ultimate 13.0.5 · Switch firmware 23.5.0 · Atmosphère 1.12.2
- Operator decision 2026-11-17: no 8-minute verify ("skip the 8 minutes because nothing actually happened"). The
  earlier auto-verify design (0.1.x) is superseded.
- Design and root cause: `smash-training-lobby/artifacts/switch-corrupt-dialog/DESIGN.md` §3B.
- Rollout and test: [`docs/ROLLOUT.md`](docs/ROLLOUT.md). The executable job is
  `ssbu-switch-remote-work/card-jobs/nsav2-p0-probe-cf2390e6/STEPS.md`.

## Sysmodule behaviour

- **When it checks.** After any process exit it re-checks every 1 s for 20 s; otherwise at start-up, on an ns record
  update, and every 10 s. Each check is one cheap `ns` read of SSBU's `GetApplicationTerminateResult` (2100).
- **If the result is non-zero and no application runs**, it reads:
  - `GetRawApplicationTerminateResult` (2101);
  - `IsDataCorruptedResult` (ns:ro getter → `IReadOnlyApplicationRecordInterface` cmd 2) for both values.
- **Decision** (`av_clear_gate`, host-tested):
  - any failed query → keep the dialog (fail closed);
  - ns says data corruption → keep the dialog, **except the 0.2.2 fast path below**;
  - `observe-only` → log only;
  - otherwise → `ClearApplicationTerminateResult` (57), re-read, and log whether it took.
  - A value whose clear did not take is never retried. At most 32 clears per run.
- **0.2.2 fast path: Rom hash-tree results only** (fs 2002-4071..4079; hw6's 0x1FD602 = 2002-4075, a transient bad
  read that the system Check then found clean).
  - **One fast clear per verified epoch**, kept in `sd:/config/ns-autoverify/strike`:
    - exactly `armed` → `fast-clear`: `strike` is first rewritten to `used 0x<value>` and read back (if that fails,
      nothing is cleared), then 57 is called;
    - `used 0x…` → `strike-keep`: the dialog stays for the system Check;
    - missing or garbled → `unarmed-keep` (fail closed).
  - The next launch re-reads the same data, so real corruption fails again and reaches the dialog.
  - **The epoch is re-armed only on positive evidence**, never by a timer or a reboot:
    - `clean-session`: an SSBU process start and exit with no pgl crash event, no flagged state seen during it or in
      the 20 s after the exit, and 2100 = 2101 = 0 at the end of that window;
    - `external-clear`: a flagged state the module saw went to 0 without its own clear (the system Check passing, a
      redownload, or probe Y).
  - A fresh install has no `strike` file. The first clean SSBU session arms it.
  - **Hardware status:** host-tested only. No real 4075 has recurred with 0.2.2 installed; every decision is logged
    for when one does.
- **Wake-ups:** pgl process events, ns `ApplicationRecordUpdateSystemEvent` (a failed launch has no process exit; at
  most one wake per second), and a 10 s poll.
- **Optional manual action:** a `verify-now` file runs the official `RequestVerifyApplication` once. That is the
  only time the 2 MiB buffer is used.
- **Fail-safe:**
  - no `fatalThrow`/`diagAbort`; SD first; any init error or firmware other than 23.x → exit;
  - a stale `busy` marker → auto `disabled`; a `disabled` file is honoured at start and every wake;
  - every wait is bounded.
- **Logs:** `sd:/atmosphere/logs/ns-autoverify.log` (64 KiB cap). Status line: `sd:/config/ns-autoverify/status.txt`.

## nsav-probe (hbmenu NRO)

Run once from the Album before the sysmodule is trusted. It establishes which ns call removes the HOME badge and
dialog.

| Button | Action |
| --- | --- |
| A | Read-only dump: view, flags, 2100, 2101, both classifications, the record. |
| X | `SetApplicationTerminateResult(SSBU, 0x4A8)` (cmd 56), only if the current value is 0. Reproduces the state without a crash. |
| Y | `ClearApplicationTerminateResult` (57). |
| ZL | `SetApplicationTerminateResult(SSBU, 0x1FD602)`, only if 2100 and 2101 both read 0. Test-only; raises the corrupt-data state; needs its own go. |
| + | Exit. |

At start it also classifies `0x0`, `0x4A8`, `0x1F4002`, `0x23F402` and `0x1FD602` (fs 2002-4000 / 4602 / 4075), which
validates the `IsDataCorruptedResult` request layout. Log: `sd:/atmosphere/logs/nsav-probe.log`.

## Build

```
make            # stage1 tree (exefs.nsp + toolbox.json, no boot2.flag) + probe NRO     (needs DEVKITPRO)
make stage2     # flags/boot2.flag only
make test       # host tests of source/policy.c (132 checks)
make check-host # type-check all C against libnx headers with host clang
tools/build_yoga.ps1 -Rev <commit> -Out <dir>   # ledger-booked build on the Yoga (devkitA64 GCC 16.1.0, libnx 4.12.0)
```
