# ns-autoverify (0.2: fast clear) + nsav-probe

When **Super Smash Bros. Ultimate** (`01006A800016E000`) terminates abnormally, ns records a terminate result and
qlaunch shows "Corrupted data has been detected. Please run a check for corrupt data." at the next launch. A manual
check then takes about 8 minutes and finds nothing. This Atmosphère sysmodule clears that state **immediately**,
but only when **ns itself** says the result is not a data-corruption result.

- Program ID `4200000000004156` ("AV"). Firmware 23.x only. Atmosphère 1.12.x.
- Operator decision 2026-11-17: no 8-minute verify ("skip the 8 minutes because nothing actually happened"). The
  earlier auto-verify design (0.1.x) is superseded.
- Design and root cause: `smash-training-lobby/artifacts/switch-corrupt-dialog/DESIGN.md` §3B.
- Rollout and test: [`docs/ROLLOUT.md`](docs/ROLLOUT.md). The executable job is
  `ssbu-switch-remote-work/card-jobs/nsav2-p0-probe-cf2390e6/STEPS.md`.

## Sysmodule behaviour

- **When it checks.** After any process exit it re-checks every 1 s for 20 s; otherwise at start-up and every 60 s.
  Each check is one cheap `ns` read of SSBU's `GetApplicationTerminateResult` (2100).
- **If the result is non-zero and no application runs**, it reads:
  - `GetRawApplicationTerminateResult` (2101);
  - `IsDataCorruptedResult` (ns:ro getter → `IReadOnlyApplicationRecordInterface` cmd 2) for both values.
- **Decision** (`av_clear_gate`, host-tested):
  - any failed query → keep the dialog (fail closed);
  - ns says data corruption → keep the dialog;
  - `observe-only` → log only;
  - otherwise → `ClearApplicationTerminateResult` (57), re-read, and log whether it took.
  - A value whose clear did not take is never retried. At most 32 clears per run.
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
| + | Exit. |

At start it also classifies `0x0`, `0x4A8`, `0x1F4002` and `0x23F402` (fs 2002-4000 / 4602), which validates the
`IsDataCorruptedResult` request layout. Log: `sd:/atmosphere/logs/nsav-probe.log`.

## Build

```
make            # stage1 tree (exefs.nsp + toolbox.json, no boot2.flag) + probe NRO     (needs DEVKITPRO)
make stage2     # flags/boot2.flag only
make test       # host tests of source/policy.c (56 checks)
make check-host # type-check all C against libnx headers with host clang
tools/build_yoga.ps1 -Rev <commit> -Out <dir>   # ledger-booked build on the Yoga (devkitA64 GCC 16.1.0, libnx 4.12.0)
```
