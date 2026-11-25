# libv90modem 0.6.0: audit fixes, refactor and CPU work — design

Date: 2026-11-22. Library at 0.5.0 (`v0.5.0` = bac3846). Worktree
`/home/georgica/audit-wt`, branch `audit`.

## 1. Intent

The owner asked to "audit, optimize and refactor this whole lib" and left
the priorities to us. Five audits were run on bac3846 (evidence, read in
full for this design):

- `build/audit/answering.md` (call control, start-up, x2, K56flex; cited "ans")
- `build/audit/calling.md` (analogue role, V.92; "call")
- `build/audit/dsp.md` (shared DSP, V.34/V.90 pumps; "dsp")
- `build/audit/v42-api.md` (V.42, API, portability, memory; "v42")
- `build/audit/legacy-tests.md` (V.22bis/V.32bis/V.21, tests, CI; "leg")

Reproducers: `build/scratch-answering/lost.c`, `build/scratch-v42-api/`
(`reent.c`, `apitest.c`, `p1test.c`, `abort.c`, `det.c`, `fuzz_v42.c`,
`fuzz2.c`, `bench42.c`), `build/scratch-legacy-tests/fuzz/s1hang.c`,
`build/scratch-dsp/tmp/x2_click.rec`. CPU prototype:
`build/scratch-dsp/prof/proto.diff`.

This work turns those findings into 0.6.0: bugs fixed test-first, the
duplicated code merged with byte-identical output, the hot paths made
cheaper with bit-identical output, and a safety net that proves each of
these on every commit.

## 2. Goals

1. A safety net, built first, that proves byte identity of every test
   program's output, the 106 recorded calls' full replay traces, and the
   benches' transmit and received-bit hashes, in one command of about
   3-4 minutes, plus a per-phase command for the ARMv5 suite, the port
   matrix and the cost counts.
2. Every Critical/Important finding of the five reports fixed or ruled on,
   each behind a test that failed before the fix; every behaviour change
   listed in `CHANGELOG.md`.
3. "Every wait bounded" and "no signed overflow" hold again in every
   engine and in V.42.
4. The duplicated V.34 upstream receiver (4 copies), the 780-line
   `answer.c` dispatcher and the 1384-line `v90a.c` restructured, with
   output byte-identical.
5. `v90m_mem_size()` about 12.5 KB smaller (union of the calling engines).
6. x2 data mode from 82.5 M toward 55 M ARM instructions a second, V.90
   answering from 39.0 M toward 32.5 M, all bit-identical, measured on
   ARMv5; and a measured answer to "two x2 calls with V.42 per box?".
7. 0.6.0 prepared (docs, CHANGELOG, figures, port matrix, replay) but not
   tagged or pushed.

## 3. Non-goals

- New modulations or features (V.92 quick connect, x2/K56 calling side).
- Output-changing CPU savings (shorter filters, fewer feedback taps,
  decimated estimates, a 16-state x2 upstream): call P4/P7, dsp "Not
  bit-identical".
- API 1.0 stabilisation beyond SONAME and symbol hiding (v42 §5).
- Pushing, tagging, merging.

## 4. Decisions

### 4.1 Binding decisions of the controller (with reasons)

| # | Decision | Reason |
|---|---|---|
| C1 | Phases in this order: (0) safety net; (1) bug and robustness fixes, each a stated behaviour change in CHANGELOG; (2) refactor, byte-identical; (3) CPU, bit-identical; (4) 0.6.0 prep, no push/tag. | Fixes change behaviour and must be judged one by one; refactors and optimisations must change nothing, which is only provable once the behaviour is settled and the net exists. |
| C2 | Phase 0 first, from leg's "safety net": scripted replay proof with stored golden outputs (106 recordings × flag sets incl. `-k`, full traces, not just rate lines), golden outputs of every test program, bench transmit and received-bit hashes, one per-commit command (~3-4 min on 16 threads: parallel host check, golden diff, replay proof, UBSan, short ARM suite on armv7hf, no-float/no-alloc scan), one per-phase command (ARMv5 check-arm, full port matrix incl. riscv64 and big-endian, cost counts, crd-scan). Tests link a library archive built once. CI: fast jobs per push (parallel builds, armv7hf short suite, cross-target output comparison, a big-endian target); ARMv5 nightly; the four slowest programs split by case. | Today the replay proof is prose (leg F1), the fixture check compares only the rate line (dsp test gap 2), CI never compares outputs across targets and has no big-endian target (leg top 4), and the ARMv5 job takes 175 min (leg top 3). A refactor of this size is unsafe without it. |
| C3 | Phase 1 fixes listed in §5, each test-first. LOST-vs-FAILED fixed by the single-end-reason refactor (ans R1). Out of scope: calling-side echo-delay measurement (call R1), API 1.0. A read-only audit of the V.92 digital side (`v92_p34d`, `v92_dec`, `v92_design`, `v92_info`) opens Phase 1; its Important findings are fixed in Phase 1. | The reports cover everything except that V.92 digital area (call §"v92/ coverage"). The end-reason refactor removes the cause (four flags reconstructed by priority) instead of patching one symptom. |
| C4 | Phase 2 refactors: one V.34 upstream receiver `v34up`; `answer.c` one file per scheme behind a phase table; `orig_t` engines in a union; `v90a.c` split; `sintab` once; one V.34 scrambler and one PP/S/S-bar; shared scaling helper; low-risk V.42 refactors. Each task names the proof of identity. | The four copies (ans R3) would otherwise each receive every Phase 3 optimisation; the dispatcher (ans R2) and v90a (call F2) are where the Phase 1 fixes were hardest to place. |
| C5 | Phase 3 optimisations (dsp P1-P5 and the estimated ones, call P1, v42 perf 0/1, ans P1) after `v34up`; portable code only (a builtin only behind a C99 fallback with identical results, `noinline` only through a macro); before/after ARMv5 cost per task; the final task measures x2 with V.42/V.42bis and states whether two heavy calls per box fit; docs keep "one per box" unless measured. | Each optimisation lands once in shared code; portability is a project rule; a CPU claim needs a count. |
| C6 | Memory pins change only in Phase 2 (union); every other phase keeps `v90m_mem_size()` 118824 (x86-64) / 118512 (ARMv5) and `V90M_MEM_SIZE`, `V90M_EC_MEM_SIZE`. CPU figures in README/limits.md updated in Phase 4. | One reviewed change of the ABI-visible sizes, not one per task. |
| C7 | Commits on `audit`, author George Partene <partene.george@gmail.com>, each message ending with exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No push, tag or merge. | Owner's rule. |
| C8 | Nothing copied from D-Modem or courier-emu as code or prose. | Licence (GPL-2.0-only vs AGPL / unknown). |

### 4.2 Decisions made in this design (with reasons)

| # | Decision | Reason |
|---|---|---|
| D1 | **Golden storage.** (a) Every test program's output (host, full, after the port-matrix TIMING filter) is committed as text in `test/golden/out/` (about 105 KB, measured from leg's host run). (b) Bench hashes in `test/golden/bench.txt` (a few lines). (c) The 7 fixtures' full replay traces in `test/fixtures/golden/` (about 0.2 MB; the recordings are already public). (d) The 106 private recordings: a sha256 manifest of every output (`test/golden/replay.sha256`, about 640 runs) and of the recordings themselves (`test/golden/recordings.sha256`) is committed; the full traces (13 MB) are regenerated on demand from any git ref by `tools/replay-proof.sh diff REF` (baseline ref `v0.5.0`). | The recordings are private calls and their traces are 13 MB: not for the repository. Hashes detect any change; the regenerated traces explain it. Everything else is small text, reviewable in a diff, and lets CI check it. |
| D2 | **Callbacks.** While a modem is inside `v90m_process_*`: `v90m_reset` on it returns `V90M_ESTATE` and changes nothing; `v90m_destroy` on it is deferred: the process call finishes the current sample, writes silence for the rest of the block (linear 0; mu-law 0xFF, A-law 0xD5), frees the object (when owned) and returns `V90M_OK`; `v90m_init` on its memory is documented as not allowed. `v90m_retrain`, `v90m_cleardown`, `v90m_get_*` and all `v90m_ec_*` stay allowed. | v42 B1: a reset to the other role runs `answer_tick` over an `orig_t` (UBSan division by zero), destroy is a use-after-free. Refusing reset is simple and testable; destroy on ENDED is the natural host idiom, so it must work. |
| D3 | **End codes.** After a connection, any loss is `V90M_END_LOST`; retrain exhaustion or a failed retrain on the answering side (V.90/V.92/V.34) is `V90M_END_RETRAIN_FAILED`, as on the calling side; an answering V.8 with nothing in common is `V90M_END_NO_COMMON`; a start-up failure stays `V90M_END_FAILED`. | ans B1, B4: one meaning per code across roles. |
| D4 | **`retraining` and `renegotiations`.** `retraining` is 0 during V.34/x2/K56 renegotiations and 1 during retrains; the `renegotiations` counter counts them for every modulation that has one. `echo_delay` stats decided by phase before modulation. | ans B2, B3: the flag must mean what docs/answering.md says. |
| D5 | **Phase 2 bounds.** `p2d` and `p2ans`: an exchange clock not reset by `WAIT_REV1 → INFO0` / `WAIT_BREV1 → INFO0`, limit 20 s; `p2d` RECOVER entries at most 4 (as p2ans's `restarts >= 4`). `answer.c` backstop per non-data phase: 30 s for `ANS_P2`, `ANS_RETRAIN` and V.34's Phase 2; 60 s for `ANS_P3`, `ANS_P3X`. Calling: INFO0d repeats in `P2L_A_WAIT_REV1` (and `P2L_WAIT_REV1` for V.34) count into `restarts`; a retrain's Phase 2 in `v90a` and `v34orig` is limited to 30 s (`V90A_ERR_RETRAINS`, end `V90M_END_RETRAIN_FAILED`). | ans R-1, call B2. Limits sit well above a normal Phase 2 (3-5 s) and the inner limit fires before the backstop; the replay proof shows no recording is affected. |
| D6 | **Clocks.** Monotone counters (`answer_t.t`, `orig_t.t`, `v22bis_t.t`) saturate at `INT32_MAX`. Clocks used in differences or modulo (`x2d`, `k56d`, `p34d`, `v34dx` `now`/`t`; `v90a.now/t`; `v90arx.n`, `.neq`) become `uint32_t` with differences through one fully defined helper `clk_diff()` (no implementation-defined conversion). | ans R-2, call B5, leg B2. Same sizes (no pin change), identical output below 2^31 samples, defined beyond. |
| D7 | **`tx_level`** valid range 0..16384 (`V90M_TX_LEVEL_MAX`), else `V90M_EINVAL`; if any modulation's scaling wraps int16 below 16384 the bound is the largest multiple of 1024 that does not wrap. | v42 B4: 16384 × 181/128 = 23168 fits int16 with margin. |
| D8 | **NULL policy.** Every public function checks its object pointer except the per-bit functions `v90m_ec_tx_bit`, `v90m_ec_rx_bit`, `v90m_async_getbit`, `v90m_async_putbit` (documented: must not be NULL). int → `V90M_EINVAL`, size_t/unsigned long → 0, void → no effect, `v90m_ec_state(NULL)` → `V90M_EINVAL`. | v42 B8; one rule, stated once in docs/api.md and the header. |
| D9 | **LAPM no-progress watchdog, on by default.** V(A) not advanced over N400 consecutive timer-recovery entries while I frames are outstanding releases the link with `V90M_EC_R_N400`. Ruling: a deliberate deviation from V.42 / HDLC, which restart the retransmission count on every recovery; the project rule "every wait bounded" wins. Recorded in docs/error-control.md. | v42 R1. |
| D10 | **SONAME** is `libv90modem.so.MAJOR.MINOR` while MAJOR is 0 (`.so.0.5` during the work, `.so.0.6` at release), `.so.MAJOR` from 1.0. `v90m__*` names in the static library are renamed `v90m__i_*` like every other internal. | v42 B5, v42 refactor 6. |
| D11 | **x2 click.** Success = after a 0.75 ms click (6 full-scale samples) in x2 data mode, the upstream bit errors are 0 from 2 s after the click on. The investigation tries, in order, bounding `er/ei` to ±2^15 before the updates, bounding the per-symbol change of the carrier `freq` integrator and the `t_integ` excursion; each must leave golden outputs and the replay manifest unchanged. If none succeeds, a watchdog ends an x2/K56 call whose upstream MSE stays above p34d's `mse_limit` for the rate for 2 s with `V90M_END_LOST`. | dsp 2.1: the cause is open; a bounded investigation with a defined fallback keeps the plan executable without a human. |
| D12 | **K56 heavy upstream** (ans §5): a new sim client option first, then the B1 retry in `k56d` (Phase 1), moved verbatim into `v34up_retry` in Phase 2. Ring of 192 symbols (768 octets) inside `k56d_t`; while catching up, 3 stored symbols decoded per symbol period with equaliser updates frozen (`QRX_CARRIER | QRX_TIMING`). Hypotheses C/D not implemented. | k56d_t (21.5 KB) stays below the union's largest member (v92d_t, 45.1 KB): no pin change. Three per period clears ~100 stored symbols in ~50 periods (~16 ms) without tripling a 10 ms block. |
| D13 | **Per-commit ARM run** is armv7hf, TEST_SHORT, minus the programs of `tools/ports-slow.txt` (QUICK); the full armv7hf short suite runs per phase. If `make verify` measures over 5 min, more programs move to the per-phase list, slowest first, until it is under. | leg estimates the full armv7hf short suite at 6-8 min after the splits; the 3-4 min target needs a cut, and the cut is made by measurement. |
| D14 | **Short builds off ARM**: `test/test.h` takes `-DTEST_FORCE_SHORT` so a host or big-endian build runs exactly the ARM short set; cross-target comparison is against the host short build of the same commit. | leg: a host short build prints what armv5 prints. |
| D15 | **Big-endian CI target**: s390x (Debian `gcc-s390x-linux-gnu` + `qemu-s390x-static`), short suite, compared with the host short build. | Debian ships it; Bootlin's s390x is already in the port matrix, so local and CI agree. |
| D16 | **Memory outside Phase 2.** A Phase 1 or Phase 3 change that would change `v90m_mem_size()` on x86-64 or ARMv5 is not made: K56's ring lives in the union member; the partial-CPd merge reuses v92aseq's working copy; dsp P8 (a stored reciprocal) is skipped and recorded if it would change the size. The inner union of `v90a_t` (call F1, ~6 KB) is deferred. | C6. |
| D17 | **New `V90M_MEM_SIZE`** after the union: the smallest multiple of 4096 that is at least the largest `v90m_mem_size()` of the port matrix plus 2048. | Keeps the bound on every target with headroom like today's (118824 vs 122880). |
| D18 | **"Two x2 calls per box"** is claimed in the docs only if 2 × the measured x2 data-mode cost with V.42bis running (worse of 32000 and 56000, ARMv5 count) is at most 110 M; otherwise the docs keep "one per box" and state the measured figure. | C5; leaves ~12 M of the ~122 M for the host side. |
| D19 | **Where figures and audit results go:** `docs/history/audit-0.6.md` (created in Phase 0): baseline and per-task cost counts, verify timings, the V.92 digital-side audit, the x2-with-V.42 verdict. | Repository history, like PROGRESS.md; build/ is ignored by git. |
| D20 | **Golden re-record rule.** A Phase 1 commit may re-record only the golden files its change explains, and lists each in the commit body with the reason. Phase 2 and 3 commits re-record nothing, except Phase 2's union task for lines that print `v90m_mem_size()` (none survive the TIMING filter today). | Keeps the net honest: a re-record is a reviewed statement, not a way to make a check pass. |

## 5. Findings to fix (ranked)

Phase 1 (behaviour changes, test-first). "Ref" is the report and its
finding id.

| Rank | Finding | Severity | Ref | Fix |
|---|---|---|---|---|
| 1 | Answering reports `V90M_END_FAILED` for every lost connection except K56 (`answer.c:94` clears `connected`; `api/v90modem.c:237`). Reproduced: v22, v34, v90, v21, v32 all `end=1`. | Important | ans B1 | `answer_t.end` + `fail(a, why)`, `end_of()` a table (ans R1); D3 |
| 2 | Phase 2 unbounded on the answering side: repeated INFO0a/INFO0c resets `t` (`p2d.c:180-183`, `p2ans.c:364-368`); `p2d` RECOVER loop has no counter (`p2d.c:212-219`); no phase deadline in `answer.c` (`:456-466`, `:536-571`). | Important | ans R-1 | D5 |
| 3 | A retrain's Phase 2 unbounded on the calling side (`p2call.c:689-694`, `:389-394`; `v90a.c:1226-1231`; `orig.c:449,459`). | Important | call B2 | D5 |
| 4 | int32 sample clocks overflow after 74.6 h, also after `V90M_EV_ENDED` (`answer.c:367`, `x2d.c:386`, `k56d.c:404`, `p34d.c:392`, `v34dx.c:576`, `orig.c:307`, `v90a.c:1224`, `v90arx.c:758`, `:671`, `v22bis.c:135`). | Important | ans R-2, call B5, leg B2 | D6 |
| 5 | V.22bis waits forever in `V22S_S1_RX` (`v22bis.c:175-176`); a 1200 Hz tone at -21 dBm0 holds a connected call 600 s. | Important | leg B1 | 1 s limit; then `retrain_send()` when retraining, back to WAIT/TRAIN in start-up |
| 6 | Reset/destroy from a callback: UB / use-after-free (`api/v90modem.c:270-309`). | Important | v42 B1 | D2 |
| 7 | V.42bis P1 above `V42COMP_MAX_CODEWORDS` agreed but not run (`v42/v42.c:83-107`, `:173-176`). | Important | v42 B2 | clamp in `v42ec_init`; a failed C-INIT releases with `V42EC_R_COMPRESSION` |
| 8 | `v90m_ec_abort` ignored in DETECT/ASYNC, late in CONNECTED (`api/v90modem.c:486`, `v42.c:243`). | Important | v42 B3 | `v42ec_abort()` → DISCONNECTED/LOCAL at once in every state |
| 9 | `tx_level` unbounded: `INT_MAX` overflows (`answer.c:143`), >23170 wraps int16. | Important | v42 B4 | D7 |
| 10 | SONAME `.so.0` across ABI changes (`Makefile:37-38`). | Important | v42 B5 | D10 |
| 11 | Partial CPd refused (`v92aseq.c:87`, `:114`; `v90a.c:1040-1042`): every far-end renegotiation that sends only changed parts becomes a retrain. | Important | call B1 | merge present parts into the stored CPd; all three required only for the first CPd of a Phase 4 |
| 12 | JM with only PCM-analogue selects V.90 on the calling side (`v8msg.c:116`). | Important | call B3 | calling side takes PCM only with `V8_PCM_DIGITAL`; else V.34 |
| 13 | K56 client that ignores our MP (64 states, non-linear, expanded shaping) cannot be decoded. | Important (gap) | ans top 4, §5 | D12 |
| 14 | One 0.75 ms click ruins the x2 upstream for the rest of the call (mse 16 → ~300). | Important | dsp 2.1, 2.2 | D11 |
| 15 | V.92 digital side not audited in depth. | — | call (coverage) | read-only audit, Important findings fixed in Phase 1 |
| 16 | LAPM can stay CONNECTED for ever without progress (`v42_lapm.c:1049`). | Minor | v42 R1 | D9 |
| 17 | Originator in detection ignores HDLC flags (`v42.c:426-440`). | Minor | v42 B6 | run `v42hdlc_rx_bit` in DETECT; `flag_run >= FLAGS_SEEN` → `want_hdlc` |
| 18 | `retraining` set in V.34/x2/K56 renegotiations (`answer.c:568,607,639`); stale `echo_delay` after a V.92 → V.90 retrain (`api/v90modem.c:354`). | Minor | ans B2, B3 | D4 |
| 19 | End-code inconsistencies across roles (`answer.c:228,507,543`; V.8 no common). | Minor | ans B4 | D3 |
| 20 | K56 tone B during a renegotiation not followed (`k56d.c:427-439`). | Minor | ans R-3 | watch tone B in `K56D_RENEG` → `K56D_RETRAIN` |
| 21 | `max_up_bps` < 2400 with x2/K56 kills the call after the point of no return. | Minor | ans R-6 | `check_cfg` → `V90M_EINVAL` |
| 22 | `answer_v90_cfg` re-runs `k56_offer`/`x2_offer` after the first tick. | Minor | ans R7 | guard: only before the first tick |
| 23 | One data frame lost at every Rd (`v90a.c:686-704`, `:672`). | Minor | call B4 | at Rd detection deliver the older slot if it is not Rd |
| 24 | `clock_q8` int64 overflow on a hostile L2 (`p2call.c:643-658`). | Minor | call B6 | saturate when `|num| > den * 20` before the multiply |
| 25 | Tone B during our own cleardown turns it into a retrain (`v90a.c:1282,1334-1336`). | Minor | call R3 | ignore `rt_due` while `clearing`; end `V90A_ERR_HANGUP` |
| 26 | `v22rx` int32 products can overflow under extreme input (`v22rx.c:237`, `:251-252`). | Minor | leg B3 | int64 products |
| 27 | `v42ec_tick` casts `unsigned ms` to int (`v42.c:510,520`). | Minor | v42 B7 | clamp to 1 000 000 once |
| 28 | NULL handling inconsistent with docs. | Minor | v42 B8 | D8 |
| 29 | Break option not validated. | Minor | v42 B10 | `V90M_EINVAL` for anything but `V90M_BRK_*` |
| 30 | XID response below our proposal clamped up (`v42_lapm.c:500-501`). | Minor | v42 R3 | take the responder's smaller k / N401 |
| 31 | V.42 endpoint config not validated. | Minor | v42 R4 | one `check_ec_cfg` → `V90M_EINVAL`, ranges in the header |
| 32 | `xid_resp[64]` sized by hand; `V42_XID_TRACE` static global. | Minor | v42 R6, R7 | static check; trace removed |
| 33 | `V90M_MOD_BELL212A` without `V90M_MOD_V22` accepted silently. | Minor | v42 B11 | `V90M_ENOTSUP` |
| 34 | Static library leaks `v90m__answer`, `v90m__orig`, `v90m__ec`. | Minor | v42 ref 6 | D10 |
| 35 | `test_v92real` passes with 0 checks when its data is missing; cwd-dependent data paths. | Minor | leg F7 | `-DTEST_DATA_DIR`, a missing file fails (Phase 0) |
| 36 | Wall-clock assertion in `v34/test/test_bench.c:95`. | Minor | leg F8 | report only (Phase 0) |

Phase 2 (byte-identical refactors): ans R1 (as part of rank 1), R2, R3,
R4, R5, R6, R7, R8; call F1, F2, F3, F4, F5, F7; dsp 3.1, 3.2, 3.3, 3.4,
3.5, 3.7; leg F5; v42 refactors 1-5, 7; v42 R5.

Phase 3 (bit-identical CPU): dsp P1, P2, P3, P3b, P4, P5, P6, P7, P8, P9,
P10, P11, P12, P13, P14, P15; call P1, P4 (bit-identical trim only), P5,
P6, P8; ans P1, P3; v42 perf 0, 1. Measured targets from dsp: x2 82.46 M
→ 59.77 M (P1-P5), about 53.5-54.5 M with the rest.

## 6. Deferred (out of this plan), with reasons

| Item | Ref | Why deferred |
|---|---|---|
| Echo-delay measurement on the calling side | call R1 | A feature; it can only be validated against a real server, which the calling side has never met. |
| API 1.0 (size fields in structs, `int` for enum members, fixed-width counters, `v90m_strerror`, Windows export macro, numeric version) | v42 §5 | Owner's scope ruling: only SONAME and symbols now. |
| V.90 Phase 2 recovery (9.2.1.2.5/6) | ans R-4 | A feature, not a bound; the Phase 2 bounds (D5) make the failure defined. |
| Inner union of `v90a_t` (~6 KB) | call F1 | Needs proof that every Phase 3 entry inits its upstream (V.92 → V.90 fallback); marginal gain; would change pins twice. |
| Calling side quantises the PCM upstream to G.711 in `process_linear` | call B8 | Effect unmeasured; needs a codec-on-network-clock simulation first. |
| CM always offers LAPM | call B7 | Documented in Phase 4 (calling hosts run V.42); a cfg flag would change the ABI again. |
| `v90dil_acc_add` overflow by one | call R4 | Unreachable (≤ ~50 counts); clamping changes output, widening changes memory. |
| V.14 overrun counter | v42 B9 | Would grow `v90m_async_mem_size`; the 511-octet doc fix is made. |
| V.32bis holding a steady in-band tone | leg R2 | Needs live evidence; data still flows. |
| `v90_find_sync` needing exactly 17 ones | dsp 2.4 | Awareness only; matches V.34 MP detection. |
| HDLC resync rate limit; byte-wise HDLC rewrite | v42 perf 3, perf 0 | Bounded today; the rewrite touches x2's resync logic. |
| Test-helper deduplication | leg F4 | Leg's own advice: not while the library moves; the duplicated copies are part of the net. |
| `divmod3` halving, interpolator ×4 unroll in v92_mod/pcmtx alone | call P2, P3 | Prototyped: output identical, saving 0.2 M; not worth a change beyond the shared interpolator task. |
| Output-changing CPU (filter lengths, NB, estimate decimation, redesign interval, 16-state x2) | call P4, P7; dsp | Non-goal (bit identity). |
| V.22bis/V.32bis/Bell recordings as fixtures | leg test gap 4 | Needs live callers from v90d (owner action). |
| win32 run under wine in CI | leg CI | Large install for small gain; the win32 build stays. |

## 7. Safety net (Phase 0)

### 7.1 Build

- `T_SRC` compiled once into `$(BUILD)/libtest.a` (host) and
  `$(BUILD)/arm/libtest.a`; each test links `test/test_X.c` against it with
  the same `T_CFLAGS` (no LTO, so the same object code; leg F2). The
  sub-Makefiles (`v34`, `v90`, `v92`, `v42`, `v32`) do the same with their
  `ALL_SRC`.
- One parallel runner for everything: the sub-Makefiles print their
  program lists (`print-tests`), `tools/par-run.sh` runs all programs of
  all directories in one pool, longest first from `tools/test-times.txt`,
  output printed in the fixed order (leg F3). `make check PAR=N` uses it
  on the host.
- `test_p34`, `test_orig_v92`, `test_orig_seeds`, `test_orig_bell` take a
  case number (`prog N`, `prog --cases` prints the count; no argument runs
  all, unchanged output). The ARM runner runs each case as its own job.

### 7.2 Golden outputs and hashes

- `tools/out-filter.sh`: the TIMING filter of `tools/check-ports.sh`,
  shared by both.
- `tools/golden.sh record|check BUILD`: every program's filtered output vs
  `test/golden/out/<dir>/<prog>.txt`; prints `golden: N programs identical`
  or the first differing lines of each.
- `tools/bench-hashes.sh record|check`: `rec_v90`/`bench_v90`,
  `bench_v90 -x` (x2), `-k` (K56), `bench_v34`, `bench_v90a`, `bench_v90c`
  (V.90 and V.92), `bench_bell` (103, 212) on the host; transmit hash and a
  new received-bit hash each, vs `test/golden/bench.txt`.
- `v42/test/test_v42_golden.c`: hashes of V.42bis and V.44 encoder output
  on fixed inputs and of a scripted back-to-back LAPM session's bits, so
  V.42 refactors and optimisations have an identity proof.
- `test/fixtures/check.sh`: full trace of each fixture vs
  `test/fixtures/golden/<name>.txt`, fixtures replayed in parallel.

### 7.3 Replay proof

`tools/replay-proof.sh check|record|diff REF`:
- RECS (default `/home/georgica/sync-build/live-050`), checked against
  `test/golden/recordings.sha256` (106 lines) first.
- Flag sets: base `""`, v92 `-9`, x2 `-x`, x2-v92 `-x -9`, k56 `-k`,
  k56-v92 `-k -9`; 636 runs, `nproc` at a time; each run's `.txt`, `.bits`
  (via a new `v90m-replay -b FILE` option) and `.rc`.
- `check`: sha256 of every output vs `test/golden/replay.sha256`.
- `diff REF`: builds `v90m-replay` from `git archive REF` under
  `build/replay-ref/REF`, replays, and diffs the full traces of every file
  whose hash differs.
- Without recordings it fails, unless `V90M_NO_RECS=1` (CI), when it says
  `replay proof: skipped (V90M_NO_RECS)`.

### 7.4 Commands

`make verify` (per commit; `tools/verify.sh`), stages in parallel, each
printing one line, then `VERIFY PASS (<seconds> s)`:
1. host gcc `make -j16 check PAR=16`, then `tools/golden.sh check`;
2. `tools/replay-proof.sh check`;
3. `tools/bench-hashes.sh check`;
4. `make -j16 check-ubsan PAR=16` (now including the fixtures);
5. armv7hf (`/home/georgica/toolchains/armv7-eabihf--musl--stable-2023.08-1`)
   short suite minus `tools/ports-slow.txt`, outputs compared with a host
   `-DTEST_FORCE_SHORT` build (`tools/short-cmp.sh`);
6. `make check-fixed` (armv5 objects of `CORE_SRC`: no `__aeabi_f*`,
   `__aeabi_d*` or other soft-float helper, no `malloc`/`calloc`/
   `realloc`/`free` outside `api/v90modem.o`) and `make check-symbols`.

`make verify-phase` (`tools/verify-phase.sh`), about an hour:
1. `make check-arm` with the armv5 uClibc toolchain, PAR=16, full short set;
2. `tools/check-ports.sh build/ports` over every target (riscv64-musl,
   s390x-glibc, aarch64be-musl, mips32, powerpc among them; m68k
   report-only, README "Ports");
3. the armv7hf short suite in full;
4. `bench-v90 bench-x2 bench-k56 bench-v34 bench-v90a bench-v90c
   bench-v92c bench-bell` (host = qemu hashes);
5. `cost-x2 cost-k56 cost-v34 cost-v90a cost-v90c cost-v92c cost-bell`,
   written to `build/costs.txt`;
6. `make crd-scan RECS=…`: `0 CRd detections`.

### 7.5 CI (`.github/workflows/ci.yml`)

Per push: host gcc and clang (`-j`, `PAR=$(nproc)`, golden, bench
hashes, `V90M_NO_RECS=1`), UBSan (`-j`), armv7hf short with comparison
(Debian `gcc-arm-linux-gnueabihf`), s390x short with comparison, win32
build. ARMv5 armel full short suite: `schedule` (nightly),
`workflow_dispatch`, tags `v*`.

## 8. Refactor shapes (Phase 2)

- `modem/v34up.{c,h}` as ans R3 (`v34up_t`, `v34up_sync_pp`,
  `v34up_known`, `v34up_dec4`, `v34up_dec16`, `v34up_data_begin`,
  `v34up_data`, `V34UP_*`), plus `v34up_retry` (D12) and the data-mode
  scaling helper (dsp 3.4); `p34d`, `v34dx`, `x2d`, `k56d` keep their
  policies.
- `modem/ans_scheme.h` as ans R2 (`ans_scheme_t`), files `ans_legacy.c`,
  `ans_v90.c`, `ans_v34.c`, `ans_x2.c`, `ans_k56.c`; shared retrain
  bookkeeping and a per-scheme `rt_policy {limit, forget}` (ans R8).
- `orig_t`: `union { struct { v22bis_t v22; v32_t v32; } leg; v34orig_t v34;
  v90a_t v90; } e;` (Annex A needs V.22 and V.32 together).
- `v90a.c` → `v90a.c`, `v90a_up90.c`, `v90a_up92.c`, `v90a_down.c`,
  `v90a_watch.c`, `v90a_priv.h` (call F2).
- `modem/sintab.c` + `extern` (and `v34_interp`); one V.34 scrambler
  (v34/'s), one PP/S/S-bar generator with a scale; `x2_crc16` →
  `info_crc`; `info1c_*` aliases; `p2common.{c,h}` (ans R4, call F5);
  `modem/fixmath.h` (`isqrt` variants after a differential test, call F4 /
  leg F5); `seqhunt` (call F3); relative includes (ans R6); dead code (ans
  R7, call F7, dsp 3.5, 3.7).
- V.42/API: X-macro field lists for cfg/status copies, `comp_out.h` with a
  compile-time bound (v42 R5), a compression ops table, `after()` split by
  role, one version source, a named `V90M_ALIGN` probe.

## 9. Risks

| Risk | Mitigation |
|---|---|
| The replay covers only answering V.8/V.34/V.90/V.92 handshakes (95 of 106 end unconnected; no V.22bis/V.32bis/Bell; no calling side). | Golden outputs of every simulator test (which carry bit and error counts, timings, SNR) and the bench hashes cover the rest; the calling side's benches gain a received-bit hash. |
| A golden re-record hides an unintended change. | D20; the reviewer checks every re-recorded file against the commit's stated reason. |
| Bounds (Phase 2, S1, watchdogs) fire on a legitimate slow peer. | Limits several times the normal duration; replay proof on 106 real calls; a test with a slow-but-legitimate peer (Review Focus). |
| Phase 3 "exact" arithmetic is not exact in a corner (rounding of negative division, ties in the Viterbi). | Each rewrite carries an exhaustive or large randomised differential test against the old expression, plus all identity proofs. P3b is made only if identity holds, else recorded as skipped. |
| A builtin or attribute breaks a port (MSVC, old gcc). | `modem/compiler.h` wrappers with C99 fallbacks; the fallback path is compiled and tested in the suite (`-DV90M_NO_BUILTINS`); win32 build in CI. |
| The per-commit command exceeds 4 min and is skipped in practice. | D13: measured, the slow set moved to per phase. |
| Phase 1 fixes interact (end codes vs bounds vs clocks). | Order: end reasons before the bounds, so every new bound uses `fail(a, why)`. |
| Memory pins move outside Phase 2. | D16; `test_api` pins the size on x86-64 and ARMv5 in every task. |
| The x2 click root cause is not found. | D11's watchdog fallback; the call ends defined instead of carrying garbage. |
