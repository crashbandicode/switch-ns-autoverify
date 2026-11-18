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
  - The card tool now allows these paths: `ssbu-switch-remote@0b21b00c` (primary-authorized 12:45Z).
    - 48 tests OK, including the nsav lifecycle and a TR regression, on ums/mtp/ftp.
    - Differential vs the previous HEAD: TR install receipts, INSTALL.md and card tree identical.
    - All 8 jobs dry-ran ok with the committed tool.
  - STEPS finalized as probe-first, with an abort/rollback point after every step.
  - The old `nsav-*` jobs (0.1 auto-verify) are marked SUPERSEDED.
- **Superseded builds:** `build-1838af18` (0.1.1 auto-verify) and `build-4e71f3f3` (failed). Do not install either.

## Update 2026-11-17 13:1xZ: hardware probe 12:47Z, then 0.2.1 / probe 0.3.0 (`5d126774`)

- **Probe 0.2.0 on hardware** (`artifacts/switch-corrupt-dialog/hw2/RESULT.md`, analysed in `hw2/ANALYSIS.md`).
  - What worked: `nsInitialize`, the classify layout (0x4a8 not corrupt; fs results corrupt), and 2101.
  - `Set(0x4A8)` changed only `raw` (2101), and `last_event` 16→12. 2100 stayed 0 and the view was unchanged.
  - Clear zeroed `raw`.
  - The badge was not checked after the Set. No real flagged state has been dumped yet.
- **Sysmodule 0.2.1:** acts on term OR raw, each classified by ns; a clear counts as taken only when both read 0.
- **Probe 0.3.0:** adds `last_updated` and the ZR scope-0 verify experiment.
- **Build `5d126774`** on the Yoga, 0 warnings, 63/0:
  - `exefs.nsp` `229fd13749b44cab12d7b052674f8b11df5201131937d900889ea656427b2c72` (97,332 B);
  - `nsav-probe.nro` `99c11d2501e403d6ea2f19944d24c68897d6b6b658dbe59ecdb3546a5c3fdc98` (241,720 B).
- **Next:** the controlled real-crash test, job `card-jobs/nsav3-crash-5d126774/STEPS.md`. The nsav2 jobs are
  superseded.

## Update 2026-11-18 04:41Z: 0.2.2 design posted, waiting for the primary's GO (nsav-devin)

- **Trigger:** hw6-natural (`smash-training-lobby/artifacts/switch-corrupt-dialog/hw6-natural/RESULT.md`).
  - 0.2.1 kept `term=raw=0x1fd602` (ns: corrupt).
  - The system Check then found no corruption.
- **Decode:** 0x1fd602 = 2002-4075 `RomHierarchicalSha256HashVerificationFailed`, a SHA-256 hash-tree mismatch on a read. The
  FS server raises 4535, and the RomFs layer renames it to 4075. With the Check passing, it was most likely a transient bad read.
- **H2 now likely:** 2100 is non-zero only for corrupt-class results, and flags bit 13 (0x2000) is set with it. If so, 0.2.1's
  non-corrupt clear path can never fire on a real dialog.
- **Design** (inbox `phase23/INBOX-opus-rc6-integration.md`, `## 04:41Z … NSAV 0.2.2 DESIGN`):
  - fast-clear 2002-4071..4079 once per "verified epoch" (`config/ns-autoverify/strike` file);
  - a repeat inside the epoch → `strike-keep` for the system Check;
  - an observed external clear (the Check) resets the epoch;
  - wait on ns `ApplicationRecordUpdateSystemEvent`, with a 10 s poll;
  - probe 0.4.0 adds ZL = Set(0x1FD602) when both values are 0.
  - A targeted ExeFS verify was rejected: Code is not mountable via fsp-srv, and the failing section is unknown.
- **erpt plan:** the 265 UUIDs new since 11-16 12:44Z are in `hw6-natural/erpt-new-since-1116-1244Z.txt`; parse them with
  `hw6-natural/erpt_parse.py`.
- **Next after GO:** implement in policy/main/probe plus host tests, then the Yoga build, then card jobs
  `ssbu-switch-remote-work/card-jobs/nsav6-*` (S0 install with observe-only, T1 reproduce, T2 fast clear, T3 strike +
  system Check, T4 crash-trigger, rollback). The driver is switch-hw-devin-2. Nothing pushed.

## Update 2026-11-18 05:1xZ: 0.2.2 implemented, built and staged (nsav-devin, after the primary's 04:44Z GO)

- **Commit `c4356b4`** (local `main`, not pushed). Epoch rules as in the README:
  - armed only by `clean-session` or `external-clear`;
  - missing or garbled → `unarmed-keep`.
- **Checks:** host tests 132/0; `make check-host` exit 0.
- **Yoga build `build-c4356b4/`:** GCC 16.1.0 / libnx 4.12.0, 0 warnings.
  - `exefs.nsp` `989090c01d7363b4bee8b83de4bb4066f7dec614e413b9efa59324818142678d` (101,457 B);
  - `toolbox.json` `31a05378…`;
  - `nsav-probe.nro` 0.4.0 `f58ff99890c40fdab7a34a732e6b4eaebe2b60b9a5672f188c7511bdc49f244f` (241,720 B).
- **Jobs** (`ssbu-switch-remote-work/card-jobs/`): master `nsav6-s1-c4356b4/STEPS.md`, plus `nsav6-noflag`, `-off`,
  `-rearm`, `-probe-out`, `-rollback` and `-s2-boot2` (Stage 2, needs its own go).
  - All 8 steps dry-ran ok on a scratch card with `ssbu-switch-remote@70e7aa3` (ums), replacing a fake 0.2.1.
  - The card tool can't remove `config/ns-autoverify/strike`; the rollback leaves it (inert).
- **Size:** about +322 net product lines (main +175, policy +92, policy.h +38, probe +17) and +149 test lines, against
  the approved ~150. Flagged to the primary.
- **Hardware scope:** (a) no regression and no action, plus the session/epoch log lines; (b) the 0.2.1 clear
  (probe X) and the kill switch. **The 4075 fast path is host-tested only.**
- **Next:** the driver runs `nsav6-s1-c4356b4` when the console queue allows. Then read `$E=…/hw7/p` against the
  pass table.

## Unknowns (P0 resolves them)

- The `IsDataCorruptedResult` (2) and `GetRawApplicationTerminateResult` (2101) request layouts are assumed, with no
  libnx wrapper. A wrong layout fails closed: the dialog is kept.
- Whether the terminate result alone drives the badge and dialog, and whether 57 alone clears them.
- Whether ns classifies `0x4A8` as not corrupt.

## Next

The driver runs P0 per STEPS.md. If O1 → Stage 1. If O2/O3/O4/O5 → report the probe log here; the next ns call is
researched from the dumps.
