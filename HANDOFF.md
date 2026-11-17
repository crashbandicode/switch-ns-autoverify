# Handoff: ns-autoverify

- **Objective** (operator, relayed by primary `c07b3449` at 12:16Z on 2026-11-17): a fast clear of SSBU's
  false-positive corrupt-data state, with no 8-minute verify, gated by ns `IsDataCorruptedResult`. Probe first.
- **Repo:** `/home/intpa/switch-ns-autoverify` (local git, branch `main`, never pushed).
- **Design:** `/home/intpa/smash-training-lobby/artifacts/switch-corrupt-dialog/DESIGN.md` §3B.

## State

- **0.2.0 built on the Yoga at `cf2390e6`.** devkitA64 GCC 16.1.0, libnx 4.12.0, `-Werror`, 0 warnings. Host tests
  56/0 (local and Yoga). `make check-host` exit 0.
  - `exefs.nsp` `f0a88f519a24f70be310bb10ee17efe43dc9afc451f83e976f11e8430d84851b` (97,252 B).
  - `toolbox.json` `31a05378b892ea32597f9d295fb9ddb03d6b0f9c33b06a30e79454be03e70da0`.
  - `nsav-probe.nro` `e490aa57873cce0a8750e0f933f18f752b0bb215bc1c1d7a60b74268c71d3759` (237,624 B).
  - Artifacts: `smash-training-lobby/artifacts/switch-corrupt-dialog/build-cf2390e6/`. Booking `ceb8b4ee`
    released, lane deleted.
- **Jobs** (staged, not run, no go.txt): `ssbu-switch-remote-work/card-jobs/nsav2-*`. The master is
  `nsav2-p0-probe-cf2390e6/STEPS.md`.
  - They need `own_install-ns-autoverify-v2.patch` applied in `ssbu-switch-remote`. In a scratch copy the patched
    suite passed 45/45, and all 8 jobs dry-ran ok on a fake card.
  - The old `nsav-*` jobs (0.1 auto-verify) are marked SUPERSEDED.
- **Superseded builds:** `build-1838af18` (0.1.1 auto-verify) and `build-4e71f3f3` (failed). Do not install either.

## Unknowns (P0 resolves them)

- The `IsDataCorruptedResult` (2) and `GetRawApplicationTerminateResult` (2101) request layouts are assumed, with no
  libnx wrapper. A wrong layout fails closed: the dialog is kept.
- Whether the terminate result alone drives the badge and dialog, and whether 57 alone clears them.
- Whether ns classifies `0x4A8` as not corrupt.

## Next

The driver runs P0 per STEPS.md. If O1 → Stage 1. If O2/O3/O4/O5 → report the probe log here; the next ns call is
researched from the dumps.
