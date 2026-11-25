# The 0.6.0 audit: figures and results

Where the audit's measurements go (spec D19): the safety net's timings,
the baseline cost counts and every Phase 3 task's before and after, the
V.92 digital-side audit, the x2-with-V.42 verdict.

## Safety net (Phase 0)

`make verify` (tools/verify.sh, per commit) and `make verify-phase`
(tools/verify-phase.sh), on the 16-thread development machine, 2026-11-22.

### make verify: about 6.3 min, the big-endian suite included

Measured at the Task 4 fix-round commit (the run `build/verify-r2.txt` of
that tree): `VERIFY PASS (378 s)`. Its stages run at once; the host,
armv7hf and big-endian ones set the time:

| stage | time | line |
|---|---|---|
| host | 378 s | golden: 123 programs identical; split: 4 programs, cases concatenate to the whole |
| replay | 13 s | replay proof: 636 runs identical |
| bench | 1 s | bench hashes: 17 identical |
| ubsan | 253 s | ALL UBSAN CHECKS PASS (the fixtures too) |
| armv7hf | 370 s | 8 of 210 runs skipped (tools/ports-slow.txt); short-cmp: 113 programs identical |
| bigend | 376 s | aarch64be-musl, the same short set: 8 of 210 runs skipped; short-cmp: 113 programs identical |
| fixed | 2 s | fixed: 90 objects, 0 float helpers, allocation only in api/v90modem.o; symbols: only v90m_* (627 internal) |

Before the big-endian stage and the split-check in the host stage (Task 4's
first commit, same machine): 224 s. The run without the four split
programs (still in `tools/ports-slow.txt`) took 190 s; with them, 228 s
(D13: under 300 s, so they left the list; their cases run in parallel).
The controller accepted the 6-7 min per commit for the per-commit
cross-ABI proof. The armv7hf short suite in full took 3:44 (before the
split, 4:32).

### make verify-phase: measured at the release proof

The port matrix and the costs run only at the end of the plan (the
release proof), and once more only if a phase touches portability-sensitive
code. The one run so far, at Task 4's first commit, with every target's
whole suite and the stages one after another, took 22468 s (6 h 14 min;
not the hour the spec estimated): ports 14360 s, costs 6588 s, armv5
1274 s, armv7hf 214 s, benches 31 s, crd 1 s; `VERIFY-PHASE PASS`. Since
then (Task 4 fix round 1) the costs run beside the other stages without
the per-tick counts (`COST_TICKS=0`), and the port matrix runs the whole
suites on host gcc, armv5, s390x and riscv64 and QUICK on the rest (m68k
reported only until Task 5b; since then gating, below), slowest targets first, 2 targets at once with 8 programs
each. That configuration's time, measured at the release proof: 15230 s
(below, "The release proof").

### m68k: a GCC wrong-code bug; gating again (Task 5b)

m68k-uclibc's twelve failing programs (all on `modem/qamrx`) were put down
to qemu-m68k at ports-2. The audit found GCC 12.3.0 for m68k at fault
(with the frame pointer omitted it reads an incoming argument 8 bytes off
around a 64-bit division libcall), in two places: `qamrx_init`'s `dcar`
(the twelve failures) and `v90arx_clock`'s `ppm_q8` (the answering V.90
clock a value that depends on the binary's link layout on m68k, which no
test saw). Both
expressions are rewritten to forms gcc compiles right, with the same value
on every input (checked for all 2^32); an asm scan of the whole tree at
-O1/-O2/-Os/-O3 finds no other site. The matrix builds m68k with
`-fno-omit-frame-pointer` too, and `make verify-phase` gates on m68k like
the other targets (a target that did not run fails the gate). Details:
`docs/history/PROGRESS.md`, "m68k: a GCC wrong-code bug, not qemu".

| check | before | after |
|---|---|---|
| test_qamrx, m68k, no extra flag | 17 of 18 checks fail | 0 failures, output identical to host gcc |
| v90arx_clock(ppm_q8), m68k | 4290672329 for every ppm | the host's values |
| m68k QUICK suite (check-ports.sh) | 12 programs differ | exit 0, 203 of 211 programs identical to host gcc, 8 skipped (QUICK), 8 lines with uClibc's printf rounding |

## Baseline (v0.5.0 code)

`build/costs.txt` of that `make verify-phase` run (armv5 uClibc compiler,
`ARM_CC=$A5`, gcc -O2; ARM instructions counted under qemu, per second of
line in data mode), in M instructions per second; compared with the dsp
audit's counts ("How it was measured") and the README's table.

| | now | dsp audit | README |
|---|---|---|---|
| x2 answering (`cost-x2`, second 15 to 16) | 82.57 | 82.46 | 81.9 (32000) |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.20 | 39.04 | 38.9 |
| K56flex answering (`cost-k56`, second 20 to 21) | 39.27 | | 39.0 |
| V.34 answering (`cost-v34`, second 10 to 11) | 40.53 | | |
| V.90 downstream receiver (`cost-v90a`), 0 / 100 ppm | 44.62 / 48.97 | | |
| V.90 calling (`cost-v90c`), 0 / 100 ppm | 55.11 / 59.31 | | 54.6 / 58.8 |
| V.92 calling (`cost-v92c`), 0 / 100 ppm | 69.95 / 74.32 | 74.00 (100 ppm) | 69.4 / 73.9 |
| Bell 103 / Bell 212A calling (`cost-bell`, first second) | 9.86 / 4.43 | | 9.84 / 4.43 |

Every figure is the same as the earlier ones or less than 1 % above them
(x2 +0.1 % on the dsp audit's, V.90 +0.4 %, V.92 calling at 100 ppm
+0.4 %; V.90 calling +0.9 % on the README's);
where the difference comes from (code since those measurements, or the
counting) was not looked into. These are the figures Phase 3 measures
against.

The slow-tick lines of `cost-v90c` and `cost-v92c` count the ticks that
were slowest on the host (`bench_v90c -w`), which a loaded machine picks
differently from run to run; they are not part of the baseline, and
verify-phase no longer counts them (`COST_TICKS=0`).

## V.92 digital side (Task 6)

Read-only audit of the answering modem's V.92 path: `v92/v92_p34d.{c,h}`,
`v92/v92_dec.{c,h}`, `v92/v92_design.{c,h}`, `v92/v92_info.{c,h}`, the
parts of `v92/v92_rx.c` and `v92/v92_seq.c` they call with received data
(the Ru/Su detector, the training receiver, the Ja / CPt / CPu / SUVu
parsers), their tests, and `modem/answer.c`'s V.92 phases (`start_v92`,
`ANS_P3X` / `ANS_V92`, `answer_cleardown`, `answer_snr_db10`). The calling
audit left these files out (`build/audit/calling.md`, "v92/ coverage").
Tree 29b5f2e. Experiments in a scratch copy (`git archive HEAD` into
`build/scratch-v92d/`, not in the repository), clang UBSan
(`-fsanitize=undefined`): `test_answer_v92`, `test_v92real`, `test_p2_v92`
and the `v92/test` programs pass with no report (231, 19, 103 checks;
dec 368, design 198, e2e 258, info 201, rx 25463, seq 528). The
reproducers below are scratch programs built from `test/test_v92call.c`
and `test/test_answer_v92.c` (two simulated knobs added to the scratch
`sim/v92a`: the caller misses its first n CPds; the caller stops hearing
us), plus direct drivers of `v92_dsn_*`, `v92_trn_*` and `v92_p6_tick`.

### Summary

1. **Our cleardown becomes a retrain when the caller does not answer Rd**
   (Important, high, reproduced). `v92d_cleardown` runs a whole rate
   renegotiation; if the caller's Ru-bar never comes, `V92D_RENEG` times
   out into `retrain(p, 1)` and `answer.c` starts Phase 2 again. In the
   end-to-end run the host's hang-up gave `ANS_RETRAIN` 8 s later and
   `ANS_FAILED` with `cleared = 0` 18 s later; a caller that is still there
   could even be trained back into data mode. (T21.1)
2. **Repeated CPds go out back to back** (Important, medium-high,
   reproduced). Once the repeat of 9.6.1.1.3 starts, CPd' follows CPd'
   with no SUVd between, until an acknowledge comes; 9.6.1.1.2 has a
   single CPd followed by SUVd (Figures 12 to 14 draw no repeat) and a
   real Agere engine cleared down on back-to-back CPds (BinModem). (T21.2)
3. **Signed overflow on received samples near full scale, and in the CPd
   design on a hostile channel estimate** (Important, high, reproduced
   under UBSan). `v92_p6_tick` multiplies int16 samples in int arithmetic
   (`v92_rx.c:22-27`); a caller at about 30000 peak trips it in Phase 3.
   The design's conjugate gradients overflow int32 / int64
   (`v92_design.c:190, 215, 216, 221, 260`) on a channel estimate with a
   long low-pass tail, which the caller's training signal controls.
   (T21.3, T21.4)
4. **Our quality-driven rate drop counts every earlier drop twice**
   (Important, high by reading). The ceiling is the rate now in use, which
   already carries the earlier drops, less 4000 times all drops: 4000,
   8000, 12000 bit/s down in turn, and PCM upstream is switched off for the
   rest of the call after the fourth drop from 48000. The path has no
   test (`v92/README.md` says so). (T21.5)
5. **The upstream decoder has no gain margin** (Important; high that it
   is fragile, low how many callers it hits). A caller whose data mode
   level is 15 % (1.2 dB) off the level the CPd design assumed fails B1u
   (`V92D_ERR_B1`) and the call falls back to V.90 after a retrain. Real
   implementations disagree on the reference of G by up to 9 dB
   (BinModem). (T21.7)

Counts: Bugs 3 Important (T21.1, T21.2, T21.5), 4 Minor; Robustness 4
Important (T21.3, T21.4, T21.6, T21.7), 2 Minor (R-7 lists what holds);
Refactor 4 Minor; Performance 2 Minor; plus the BinModem leads (two Minor
items, the class offset and the epsilon sign) and the test gaps. Found
later, in Task 21: T21.8 (fixed), T21.10 (deferred) and T21.11 (fixed:
the cleardown's signals, from the review of T21.1).

### Bugs / correctness

**T21.1 Cleardown that the caller does not answer turns into a retrain**
(Important, high).
- Where: `v92/v92_p34d.c:816-822` (`v92d_cleardown`: `clearing = 1`, then a
  rate renegotiation of ours), `:785-801` (`V92D_RENEG` waits for the
  caller's Ru-bar; `p->t > MS(T_RENEG_MS)` gives `retrain(p, 1)`),
  `:696-712` (in `V92D_PH4` the drn = 0 CPd waits for a fresh design on 600
  symbols of TRN2u, `:700` a refused design is `V92D_ERR_DESIGN`),
  `modem/answer.c:478-492` (any `V92D_RETRAIN`, and any `V92D_FAILED` other
  than CLEARDOWN / HANGUP / CARRIER, starts a retrain).
- What happens: the host asked to hang up; if the caller misses Rd, or the
  new design is refused, or the caller vanishes, the call retrains or ends
  as lost instead of `V90M_END_CLEARDOWN_OURS`. V.90 (`modem/p34d.c:536-540,
  606-617`) sends Rd, TRN2d and four MPs with rate 0 whatever the caller
  does, then ends with `P34_ERR_HANGUP`.
- Reproduced: `test_answer_v92`'s first scenario, 1 s into data mode the
  caller's input replaced by the idle octet, then `answer_cleardown`:
  phase `ANS_V92` -> `ANS_RETRAIN` at +8.0 s -> `ANS_P2` -> `ANS_FAILED` at
  +18.1 s, `cleared = 0`, `retrains = 1`. With the caller hearing us:
  `ANS_FAILED`, `cleared = 2`, after 2.33 s. In `v92d` alone (scratch
  `test_v92call`): `V92D_RENEG` -> `V92D_RETRAIN` 8 s after the request.
- Also: `answer.h:216-218` and `docs/api.md` promise `ANS_FAILED` "within a
  fraction of a second"; V.92 takes 2.3 s when all goes well (Rd, the
  caller's Ru, 600 TRN2u symbols, a paced design, four CPds).
- Fix shape: while `clearing`, never retrain and never wait for a design:
  CPd with drn = 0 built from the CPd in use (`p->cpd`), sent after Rd /
  Rd-bar / TRN2d whether or not Ru-bar came, then `V92D_ERR_HANGUP` after
  `CLEAR_CPDS`; a time limit inside `V92D_RENEG` / `V92D_PH4` that ends
  with HANGUP, not a retrain. Signal path untouched; the replay proof does
  not reach cleardown.
- Test that fails today: `test_answer_v92`: `answer_cleardown` in V.92
  data mode while the caller no longer hears us ends in `ANS_FAILED` with
  `cleared = 2` within 4 s and no retrain (today `ANS_RETRAIN` at +8 s,
  `cleared = 0`). (Review correction: the bound was 2 s, which the healthy
  cleardown's 2.33 s already passed; and the end code is defined either
  way: the harm is an 18 s hang-up and a spurious retrain.)
- Fixed in Task 21 (T21.1): while clearing, `V92D_RENEG` sends Rd, Rd-bar,
  48 data frames of TRN2d and four CPds with drn = 0 built from the CPd in
  use, then `V92D_ERR_HANGUP`, whether Ru-bar came or not (limit
  `T_CLEAR_MS`, also HANGUP); nothing in a cleardown retrains. Some 0.8 s
  with or without the caller. `test_answer_v92` 3b, `test_v92call` 18b.
- T21.11 (from the review of T21.1): those signals were not 9.11's. 9.11
  makes a cleardown a rate renegotiation; 9.8.1.1.2 then has TRN2d, SUVd,
  and on the caller's SUVu a single CPd followed by SUVd (9.6.1.1.2), not
  four CPds right after TRN2d. Now: Rd, Rd-bar, TRN2d, SUVd until SUVu, one
  CPd with drn = 0, SUVd (SUVd' once CPu is in); over at 9.6.1.1.4's point
  (our SUVd' out, the caller's acknowledge or E2u in), else at
  `T_CLEAR_MS` (3.5 s from the request); every end is `V92D_ERR_HANGUP`.
  `test_v92call` 18 (the signals in order), 18c (no SUVu: 3.5 s, no CPd).
- T21.12 (from the review of T21.11): our own V.92 caller hung up on the
  drn = 0 CPd without acknowledging it (9.6.2.1.2), so our answerer waited
  3.5 s against it. Now it sends SUVu' / CPu' until 9.6.2.1.4's point (no
  E2u), at most 100 ms + a round trip; our answerer ends some 0.4 s after
  the request (`test_v92a`). Also `test_v92call` 18d (no acknowledge: CPd
  repeated, SUVd between, 3.5 s), 18e (the caller's Tone A), 18f (both
  ends at once); the PH4 entry while clearing only on Rd-bar, TRN2d or
  SUVd.

**T21.2 Repeated CPd (9.6.1.1.3) is sent back to back** (Important,
medium-high).
- Where: `v92/v92_p34d.c:742-743` (the repeat starts 100 ms + rtd + 150 ms
  after the first CPd), `:745-748` (`is_cpd`: with `cpd_repeat` set and no
  acknowledge, `sel(p, cpd)` again; SUVd only once `got_ack`).
- Reproduced (scratch `test_v92call`, caller misses its first n CPds,
  transmit signal traced): n = 2: SUVd, CPd, SUVd' 256 ms, then 2 CPd' in
  a row; n = 4: 4 CPd' in a row, then Ed. The simulated caller takes them;
  BinModem's bench says an Agere engine left its SUVd flag at zero and
  cleared down on back-to-back CPds (134 to 200 sent), 9.6.1.1.2 has a
  single CPd followed by SUVd (Figures 12 to 14 draw no repeat), and the
  Agere needs 2 to 4 s of SUVd to acquire.
  The real modems we met acknowledged the first CPd (USR 5686 9 of 9), so
  the path has not run on a real line.
- Fix shape: after each repeated CPd, SUVd (with the acknowledge bit when
  CPu is in) for the repeat interval, then one CPd again. Phase 4 only; no
  replay recording reaches it.
- Test that fails today: `test_v92call`: a caller that misses its first two
  CPds still reaches data mode, and between any two CPd / CPd' the
  answerer sends at least one SUVd' (today 2 CPd' in a row).
- Fixed in Task 21 (T21.2): every CPd, a repeat included, is followed by
  SUVd (with the acknowledge bit once CPu is in); the next copy goes out
  when 100 ms + rtd + 150 ms have passed with no acknowledge. `sim/v92a`
  has the knob (`drop_cpd`); `test_v92call` 25e (2 and 4 CPds missed:
  data mode, never two CPds in a row; before the fix 3 and 5 in a row).
  The fix shape's "no replay recording reaches it" was wrong: the repeat
  runs in 13 replayed recordings (x the three V.92 sets), after our
  transmit has already left the recording; the manifest was re-recorded
  for it (5aca43d).

**T21.5 The rate drop of data mode double-counts earlier drops**
(Important, high by reading).
- Where: `v92/v92_p34d.c:773-781` (`rate_drops++`; retrain with `up_floor`
  when `up_rate - 4000 * rate_drops < 24000`), `:402-405` (design ceiling
  `up_rate - 4000 * rate_drops`). `up_rate` is the rate in use
  (`upstream_start`, `:123`), which already reflects the earlier drops.
- Arithmetic from 48000: ceilings 44000, then (44000 - 8000) 36000, then
  24000, then the fourth drop retrains with `up_floor = 1`, and
  `answer.c:491` takes PCM upstream away for the rest of the call. Meant
  (and what `p34d`'s `rate_down` does, `modem/p34d.c:265-276`): one step
  below the rate in use each time. `rate_drops` is also never cleared by a
  caller's renegotiation to a higher rate.
- Not covered by any test (`v92/README.md:545`: "The automatic path is not
  covered by a test").
- Test that fails today: `test_v92call`: upstream noise raised in data mode
  in two steps; each renegotiation of ours lowers the CPd ceiling 4000
  bit/s below the rate then in use (today the second asks for 8000 less).
- Fixed in Task 21 (T21.5): the drop renegotiation is marked
  (`reneg_ours` = 2) and only its design has the ceiling, `up_rate - 4000`;
  the retrain to V.90 upstream comes when that is below 24000.
  `rate_drops` is a count only. `test_v92call` 36: the noise rises twice
  (back once each drop has begun); drops at 40000 and 36000 ask for 36000
  and 32000 (before: 28000 for the second), and the caller's
  renegotiation after them has no ceiling (before: 20000, the design
  refused it and the call left PCM upstream). Found with the test: the
  modulus decoder flags few spoiled frames (11 to 33 in 10 s with a third
  of the bits wrong), so the first drop came 8 s after the noise rose.

**B-4. UINFO 112..127 ends the call instead of falling back** (Minor,
high). INFO1a's 7-bit Ucode (Table 18 bits 25:31) above 111 makes
`v90tx_init` refuse (`v90/v90_tx.c:440`), `v92d_init` fails
(`v92_p34d.c:83`), and `start_v92` does `fail(a)` (`answer.c:220`): the
call ends FAILED where a retrain with PCM upstream off (as for every other
V.92 start-up failure, `answer.c:491`) would keep it.

**B-5. No DIL, CPt before Ri** (Minor, low). `on_cpt` with no DIL selects
Ri-bar even when Ri was never sent (`v92_p34d.c:172-173`), so a caller that
sends CPt early (the Figure 11 reading) sees SCR then Ri-bar. And in the
few symbols before that Ri-bar starts, `V92D_TRAIN`'s "trained, send Ri"
(`:676-679`, `sig == V90TX_SCR`) can overwrite the pending Ri-bar
(`v90tx_select` replaces `pending`), leaving Ri on the line and the caller
waiting for Ri-bar until `T_TRAIN_MS`. Fix: `ri_sent`-guarded, Ri-bar only
after an Ri of at least a frame.

**B-6. E2u is looked for after any complete sequence** (Minor, high;
no effect seen). `seq_bit` arms the E1u / E2u counter after every parsed
sequence (`v92_p34d.c:246`), including an SUVu without the acknowledge;
`v92/README.md` reading 36 says only after the acknowledge. Harmless while
a sequence is always followed by the next frame sync's ones; make the code
and the reading agree.

**B-7. Cleardown refused outside plain data mode** (Minor, high).
`v92d_cleardown` returns -1 during a rate renegotiation and in Phases 3/4
(`:818`), so `v90m_cleardown` gives `V90M_ESTATE` and the host just stops:
the caller is not told. Same in `p34d`. With T21.1's fix a cleardown can
also start from `V92D_RENEG` / `V92D_PH4` after data mode was reached.

### Robustness

**T21.3 `v92_p6_tick` overflows int on loud samples** (Important, high,
reproduced).
- Where: `v92/v92_rx.c:17-27`: `X(k)` is `int32_t`, so
  `X(0) * X(6) - X(48) * X(54)` and its `2 *` are int arithmetic; with |x|
  above about 23170 the `2 *` products (in `c48`, `a48`, `c6`) exceed
  INT32_MAX before reaching the int64 accumulators. The sums of squares in
  `e48` / `e3` overflow only at -32768 (review correction).
- Reached by: Phase 3 (`V92D_WAIT_RU`, `JD`, `JP`), data mode and rate
  renegotiation in `v92d` (`v92_p34d.c:578, 642, 662, 770, 789`), on the
  echo-cancelled input; also the calling side's Sd detector
  (`modem/v90arx.c:763`), which the calling audit did not flag.
- Reproduced: scratch `test_v92call` with the simulated caller at level
  30000: UBSan `v92_rx.c:22:17: signed integer overflow: 2 * -1428553728`,
  in all eight variants tried (echo, gain, noise). With `X(k)` widened to
  int64 in the scratch copy the same runs end in an ordinary refusal
  (`V92D_ERR_DESIGN` / `V92D_ERR_CPT`) with no further report.
- Fix: `X(k)` as `int64_t`. Same values whenever nothing overflowed; the
  replay proof checks the recordings.
- Test that fails today (check-ubsan): `v92/test_rx`: a full-scale Ru
  the detector accepts (32767, and -32768 samples) through `v92_p6_tick`
  raises no UBSan report; detection is checked as well.
- Fixed in Task 21 (T21.3): `X(k)` as `int64_t`; `test_p6_full` in
  `v92/test/test_rx.c`.
- Found with the fix (T21.8): with exact sums, the canceller's output on a
  dead line (rx = idle code after a hang-up) is its own estimate of our
  echo, and when we send Rd it has Ru's shape: v92d took it for Ru (in
  the replay proof, 15 traces of the h1-h4 and cxe recordings went to
  `V92D_RENEG` 8-30 ms after the recordings' last sample). Nor did the
  carrier watch, which measured only after the canceller, see the line
  as quiet. Fixed in Task 21 (T21.8): no Ru while the line has been at
  the idle code (|rx| <= 8) for 24 samples; the carrier watch wants the
  caller's power in the line as well; `test_v92call` 24b-24d,
  `test_answer_v92` 10. The 15 traces are again those before T21.3.

**T21.4 The CPd design overflows on a long low-pass channel estimate**
(Important, high, reproduced).
- Where: `v92/v92_design.c:215-216` (`cw += (al * cp) >> 18`, `cr -= ...`
  in int32), `:221` (`cp = cr + (be * cp >> 18)`), `:190`
  (`(int32_t)(acc / 16384) + ...`), `:260` (`isi * 65536` in int64). The
  step sizes are clamped (`al` at 2^34, `be` at 2^22) but the vectors are
  not, and the iteration can diverge on an ill-conditioned channel.
- The channel is `v92_trn_h()` of the caller's own TRN1u / TRN2u: a broken
  or hostile caller chooses it.
- Reproduced (scratch driver of `v92_dsn_start` / `v92_dsn_step`, noise
  9, quantiser 3, echo 400 RMS, residual 600, Ltot 256, Lmax 192): h a
  ramp from 10^6 at tap 30 down to 0 over 40 taps: overflow at `:221`, end
  `V92_DSN_ERR_RATE` after 136 steps; ramp from tap 40 to the end: `:216`,
  `:221`, `:260`. A random fuzz of 400 configurations (six channel shapes)
  hit `:190`, `:215`, `:216`, `:221`, `:260`, every time on the ramp shape.
  No run was unbounded (the fuzz: at most 5360 steps; the ramps of R-6:
  up to 33865).
- Fix shape: saturate `cw` / `cr` / `cp` (or restart the iteration when a
  norm grows past a bound, and refuse the channel after a second time), and
  widen or pre-shift `isi` at `:260`. Outputs of designs that never came
  near the bounds stay the same; `test_design` and `test_v92call` print
  the designs and serve as the proof.
- Test that fails today (check-ubsan): `v92/test_design`: the 40-tap ramp
  channel above gives an end code (a design or a refusal) with no UBSan
  report.
- Fixed in Task 21 (T21.4): every value stored back in 32 bits (`g`,
  `cq`, `cw`, `cr`, `cp`, `w`) and the step products `al p`, `al q` are
  checked; out of range, the design ends `V92_DSN_ERR_CHANNEL`. `isi_q28`
  is formed without `isi * 65536` and saturates. Both ramps now stop in
  the first iteration (`cq`, A'A b already past 2^31).
- Found while fixing T21.4 (the audit's fuzz, run 2442: peak in the last
  tap, ISI as large as the peak): `isi_sample()` skipped the feedback taps
  with `j += lp1 + 1` and read `g[j]` past `ng` (index 224 of 224) when
  the cursor sits that late. Fixed in Task 21: the loop stops there.

**T21.6 int32 sample clocks overflow after 74.6 h** (Important, high,
reproduced). `v92d_t.now` and `.t` (`v92_p34d.c:112`, incremented every
tick at `:469`, also after `V92D_FAILED`), `trn.k` and `data_k` in data
mode (`v92_rx.h:60`, `v92_p34d.c:157`). The answering audit (its R-3)
listed `p34d`, `v34dx`, `x2d`, `k56d` but not `v92d`; this extends that
item. Reproduced: `now = t = INT32_MAX - 4000` set on reaching data mode,
UBSan `v92_p34d.c:469:11: signed integer overflow`. Test that fails today
(check-ubsan): `test_v92call`: with `now`, `t` and `trn.k` set 4000
samples short of INT32_MAX in data mode, one more second of data runs
without a report and with no error in the data.

**T21.7 No gain margin in the upstream decoder** (Important; high that it
is fragile, low how many callers it hits).
- Where: the decoder slices on the CPd points as designed
  (`v92_dec.c:65-87`); `v92d` follows the baseline (`base_step`) but not
  the gain; B1u fails on more than 8 wrong bits (`v92_p34d.c:97`).
- Reproduced (scratch `test_v92call`, the simulated caller's data mode
  amplitude times `gain_err`; 38666 bit/s up): 1.00 data mode, 0 bad
  frames; 0.85 and 1.15, and every factor tried from 0.5 to 2.8:
  `V92D_ERR_B1` 3.5 s into the call. `answer.c:491` then retrains with PCM
  upstream off. The `gain_err` knob of `sim/v92a` exists, but no scenario
  of `test_v92call` uses it.
- Real-world: the USR 5686 and the CX93001 matched our reading of 8.8.3
  (reading 35: G v = 1 at the power of TRN1u). BinModem found two V.92
  servers that built for an upstream about 9 dB louder than its own
  reading: implementations differ on this reference.
- Fix shape: estimate the caller's gain on B1u before judging it (its
  power is already measured, `sig_ms`, and the design knows the power it
  expects, `dsn.power_q14`; or a decision-directed gain over the first
  data frames, as `b1u_lead` looks at the first one) and scale the input
  of `v92_dec_put`. A data-path change: the replay proof shows whether any
  recorded call moves.
- Test that fails today: `test_v92call`: a caller whose data mode is 1.4
  times (3 dB) the designed level reaches data mode with B1u right (today
  `V92D_ERR_B1`).
- Fixed in Task 21 (T21.7): the gain is measured on the first 144
  symbols of B1u (`up_measure`: power against `dsn.usum`, the design's
  trial of u(n), for a start within some 15 %; then the scale at which most
  symbols lie within a quarter spacing of a point, 4 % then 1 % apart; a
  decision-directed estimate does not move, the G.711 points being nearly
  the same set at any nearby scale). Off the designed level, the decoder
  starts B1u again from the training receiver's ring, two samples a tick
  until it is back. `test_v92call` 32-35. Cost (ARMv5, counted): 14 k
  instructions once per B1u where 7/8 of the symbols sit on the points at
  scale 1 (simulated calls at the designed level: 131 to 144 of 144);
  otherwise some 175 k in the tick that measures, and that is most real
  calls: of the 26 recorded calls that reach B1u, 21 score under 126 at
  scale 1 (cxe 110 and h4 122, both working at 1) and take the search
  without taking a scale. A scale taken costs a multiply and a 32-bit
  division a sample from then on. For Phase 3: the 175 k tick (spread
  the search over ticks, or cut the coarse pass).
  The scale is only ever taken in the window 3/4 .. 4/3 of the power's
  start (G.711's octaves fit at twice and half the scale: cxe 128 of 144
  at 2 against 110 at 1); `test_v92call` 35 checks that with symbols that
  sit on the points at 2 only.
  A scale is taken only where at least 3/4 of the symbols sit on the
  points at it and 1/8 more than at scale 1: without the first rule the
  replay proof's usr3 (a call that fails its B1u at the designed level,
  67 of 144 on the points at 1) took 0.78 (86 of 144) and moved, still
  failing B1u; with it the 636 runs are identical.

**R-5. Ja's rate mask is checked only by the design** (Minor, high).
`c.rates = p->ja.rates` (`v92_p34d.c:400`); a Ja with no rate (or none
under `max_up_rate`) is found out by `set_rate` after Phase 3, the
2400-symbol TRN2u refinement and the design: some seconds of Phase 4,
then `V92D_ERR_DESIGN` and a retrain. Checking at Ja (`V92D_ERR_JA`) ends
the same way sooner.

**R-6. Design time on odd channels** (Minor, medium). The design took
33000 steps on some channels in the scratch runs (`dsn_tail`), 8 s at
`V92_DESIGN_PACE` 2, where the USR 5686 waits 3.5 s from its SUVu for CPd
(reading 61). SUVd goes out at 2.5 s regardless; the caller then retrains.
Not a defect by itself; worth a cap on steps (refuse and retrain early).

**R-7. Holds** (checked). Every state of `v92d` has a limit: `WAIT_RU`
6 s + MD, `MD`, `TRN1U` 6 s + rtd, `SD` (TRN1d runs out by itself),
`JD` / `JP` 5.1 s + rtd, `TRAIN` 8 s + rtd, `PH4` / `ED` 20 s + 6 rtd (8 s
plus the pacing in a renegotiation), `RENEG` 8 s, data mode the carrier
watch (0.5 s) and the quality check; rtd from `p2d` is at most 2 s. The
received sequences are bounded and checked before use: Ja fits
`V92D_FBUF` (2752 bits; the longest Ja, 255 segments, is about 2700),
its CRC is checked before the DIL is loaded, and `v90tx_load_dil` checks
every field; CPt / CPu go through `v90_map_from_cp` before use; CPu's
constellation count is bounded before its CRC; INFO1a's fields are at
most their code ranges (`ltot`, `lmax`, `md_len` 0..127 -> at most 35052
samples of MD). The Viterbi metrics are bounded as the header says
(input clamped to 2^20, distance to 8191, metrics renormalised). The
training receiver (`v92_trn_tick`) took a quiet TRN1u turning loud after
the cursor was chosen, and full-scale noise, without a UBSan report
(scratch `trn_jump`).

**T21.10 The quality-driven rate drop of data mode barely fires, and its
redesign does not see the new noise** (Important by effect, high;
deferred: recorded, no fix in 0.6). Found with T21.5's test
(`test_v92call` 36).
- Where: `v92/v92_p34d.c`, `V92D_DATA` (a drop when the modulus decoder
  flags 4 or more frames, `dec.bad_frames`, in each of two 8192-sample
  windows); `design_start` (`c.noise = p->noise`, `c.q_quiet`: TRN1d's
  figures from the start-up).
- What happens: the modulus decoder flags few of the frames that noise
  spoils: with the noise raised in data mode (3 -> 52..120 linear RMS on
  the short line at 40000 bit/s) and a third of the upstream bits wrong,
  11 to 33 frames were flagged in 10 s; most levels gave no drop at all,
  and the first drop came 8 s after the noise rose. When a drop does come,
  its design takes the start-up's noise floor (only the training residual
  of TRN2u is new): left at the raised noise, the one-step-lower design
  (36000) failed B1u (`V92D_ERR_B1`), and the call retrained without PCM
  upstream. The test lowers the noise once each drop has begun for that
  reason.
- Fix shape (later): a quality measure that sees wrong decisions (the
  decoder's own error estimate, `v92_dec_quality`, against the design's
  noise, as p34d's `rate_down` uses the receiver's MSE), and a noise floor
  for the redesign taken from data mode (the decoder's error) rather than
  TRN1d. No recorded call has reached the path (it would show in the
  replay proof).

### Refactor

- **F-1 (Minor).** `isqrt64` is copied in `v92_p34d.c:58`,
  `v92_design.c:7`, `modem/anstone.c`, `modem/v22rx.c`: one helper.
  Byte-identical by construction.
- **F-2 (Minor).** `V92D_PROBE` bench code (`v92_p34d.c:253-306`) sits in
  the product file; move it next to `live/replay.c`.
- **F-3 (Minor).** `V92D_PH4`'s sequencing of SUVd / CPd / acknowledge
  (`v92_p34d.c:690-755`) is the densest logic of the file and is where
  T21.1 and T21.2 land; a small function per concern (design, what to send
  next, time limit) before those fixes. Transmit side only; Phase 4 is not
  in the replay proof, the simulated calls are.
- **F-4 (Minor).** `v92d_renegotiate` has no product caller (`answer.c`
  and the API offer no renegotiation); tests use it. Keep it as test API or
  expose it through `v90m`.

### Performance

- **P-1 (Minor).** V.92 answering data mode has no ARM cost figure (the
  baseline above has V.92 calling only; `v92/README.md` gives an x86 ratio
  to `p34d`, 0.67). Per symbol `base_step` does up to two int64 divisions
  (`v92_p34d.c:375, 378`), library calls on the ARM926; TRN1d's tail
  update does two per sample (`:493-494`). A `cost-v92d` (rec + bench, as
  `cost-v92c`) belongs to Phase 3 before anything here is touched.
- **P-2 (Minor).** The design's cost is per step and paced; the slow
  channels of R-6 are the case to measure if the design is sped up (and
  then the TRN2d floor of the leads below matters).

### BinModem's V.92 findings against our digital side

From `~/modem-research/binmodem-findings.md` section 4 and 1.15 (facts of
another implementation's bench, checked against our code; not authority):

- **6.4.2 class index offset (Agere +2, D-Modem 0).** We decode the
  printed form only (`v92_map.c:31-38`, `v92_eta_to_k`); B1u of the USR
  5686 and the CX93001 decodes with it (`test_v92real`: 1512 bits, 0
  wrong). The Agere observation is of its digital side reading BinModem's
  upstream; whether Agere analogue modems encode with +2 is not known. If
  one does, B1u fails (`V92D_ERR_B1`) and the call goes on as V.90 after a
  retrain. Minor: a B1u-time check of both readings (as `b1u_lead` does for
  the lead frame) would cover it if a recording ever shows it.
- **Ri or CPt first (9.5.2.1.10 text vs Figure 11).** With no DIL we send
  Ri after 2400 equalised symbols of the second TRN1u
  (`v92_p34d.c:676-679`), so a caller of either reading moves on; with a
  DIL we send Ri on CPt (9.5.1.1.12), on which the readings agree. B-5 is
  the one rough edge.
- **TRN2d of at least 4800 symbols.** Our constants are 400 data frames
  (2400 T) at start-up and 48 (288 T, under the clause's 2040 T) in a
  renegotiation (`v92_p34d.c:15, 17`), but SUVd also waits for the design,
  which starts after 2400 (600 in a renegotiation) TRN2u symbols and
  takes thousands of steps: TRN2d is over 4800 T on every run seen (2.3 s
  and more at pace 2). A faster design (Phase 3) would expose the
  constants; set the floors to 800 frames and at least 340 (2040 T).
- **SUVd between repeated CPds.** Not done: T21.2.
- **S-bar-u epsilon sign (Table 22).** Our Jp asks for a fixed-sign
  fraction (`v92_p34d.c:649`, reading 25). We train the second TRN1u from
  scratch (`trn_at` re-arms `v92_trn_init`), so a caller that moves the
  other way leaves the channel peak off a sampling instant (more ISI, a
  lower design rate), not a failed CPt as on BinModem's receiver.
  `test_v92call` checks the peak lands within 56/256 T; logging
  `v92_trn_peak` after the second TRN1u on real calls would show a caller
  of the other sign. Minor, no switch needed yet.
- **A louder upstream wanted by real servers.** That is about what digital
  modems expect of analogue ones; for us it shows that the reference of G
  varies between implementations, and our decoder tolerates 15 % at most:
  T21.7.
- Also from 1.15: TRN1d longer than 2040 T (the Agere turned after 266
  ms): ours is 4800 T plus rtd and a guard (`TRN1D_SYMS`), holds. The
  first B1d / B1u data frame and the Su/Ru template: our B1d is
  `v90tx`'s (8.6.1 scrambled ones) and `v92_p6_tick` tells Ru from Su by
  timetable, not shape; hold.

### Test gaps that matter

- The quality-driven renegotiation and retrain of data mode
  (`v92_p34d.c:773-781`): no test (T21.5).
- The CPd repeat of 9.6.1.1.3: no scenario makes the caller miss a CPd
  (T21.2).
- Cleardown that the caller does not answer, and cleardown outside data
  mode (T21.1, B-7).
- A caller's data mode gain off the design (T21.7): the knob exists, no
  scenario.
- Full-scale input and hostile channel estimates under UBSan (T21.3,
  T21.4): `check-ubsan` runs only well-behaved simulated callers.
- Phase 3 failures other than "no Ru" (test 4 of `test_answer_v92`): no
  Su (9.5.1.2.2), no CPt (`T_TRAIN_MS`), refused design, refused UINFO
  (B-4); each ends in a retrain or a failure no test checks.
- No ARM cost run for V.92 answering (P-1).

## K56flex: a client that ignores our MP (Task 19)

`rec_k56 40 0 build/k56h.rec heavy` (the simulated client sends x2's
upstream: 64 states, non-linear, expanded shaping), then
`make cost-k56 K56_REC=build/k56h.rec K56_ARGS=heavy ARM_CC=$A5`:

| | ARM instructions per second of line |
|---|---|
| K56flex data mode, the heavy client (second 20 to 21) | 84.43 M (84.42 M at fix round 1) |
| V.90 data mode, the same run (second 15 to 16) | 39.35 M |
| K56flex data mode, a client that keeps to our MP (baseline above) | 39.27 M |
| x2 data mode (baseline above) | 82.57 M |

The transmit hash of the heavy call is the normal call's (52c83abc): our
downstream does not change; 742404 bits up, 0 bad.

The catch-up, from the same recording: `bench_v90.arm -k` (armv5) on
prefixes 10 ms apart, instructions per 10 ms block. B1 fails under our
MP's configuration at 16.202 s of the call (54 ms after its E: one data
frame and the Viterbi's delay, 173 symbols, under the ring's 192). Lateness
is when v90d's single pump thread would hand back each block's answer
after the block came in, at 122 M instructions a second with nothing else
running (each block starts when it arrives or when the one before is done;
v90d prefills 20 ms, `fw/drv/pump.c`).

First version (3 stored points every symbol period, catch-up about 27 ms):

| block (s of the call) | heavy client | lateness | a client that keeps to our MP |
|---|---|---|---|
| 16.19-16.20 | 0.34 M | 2.8 ms | 0.31 M |
| 16.20-16.21 | 1.73 M | 14.2 ms | 0.32 M |
| 16.21-16.22 | 1.94 M | 20.1 ms | 0.38 M |
| 16.22-16.23 | 2.02 M | **26.7 ms** | 0.41 M |
| 16.23-16.24 | 0.93 M | 24.3 ms | 0.40 M |
| 16.24-16.25 | 0.84 M | 21.2 ms | 0.40 M |

Over the 20 ms prefill: about 12 ms of idle codewords on the line at our
first data frames (Task 19 review). Ruling (amends D12): 3 stored points
every 2 symbol periods. As committed (B1 under x2's configuration decided
36 ms after the retry, caught up about 110 ms after it):

| block (s of the call) | heavy client | CPU at 122 M/s | lateness |
|---|---|---|---|
| 16.19-16.20 | 0.34 M | 2.8 ms | 2.8 ms |
| 16.20-16.21 | 1.24 M | 10.2 ms | 10.2 ms |
| 16.21-16.22 | 1.18 M | 9.6 ms | 9.8 ms |
| 16.22-16.23 | 1.18 M | 9.7 ms | 9.7 ms |
| 16.23-16.24 | 1.19 M | 9.7 ms | 9.7 ms |
| 16.24-16.25 | **1.28 M** (the heaviest) | 10.5 ms | 10.5 ms |
| 16.25-16.31 (6 blocks) | 1.24-1.25 M | 10.2 ms each | 10.7 to **11.7 ms** |
| 16.31-16.32 | 0.94 M | 7.7 ms | 9.4 ms |
| 16.32-16.33 | 0.84 M | 6.9 ms | 6.9 ms |

The blocks run at about 10.2 ms of CPU for 10 ms of line, not the review's
estimate of 9.3 ms (the live 64-state decoding, 0.84 M a block, comes on top
of the stored points' 0.4 M). The lateness grows by about 0.2 ms a block
and peaks at 11.7 ms, inside the 20 ms prefill with 8.3 ms to spare, with
the box doing nothing else. The equaliser is put back as it was at its E
before the stored points are decoded again (its taps, 576 bytes, kept in
k56d_t: 22272 -> 22848 bytes, still inside the union under v92d_t).

## T21.9: the calling receiver lost to one sample (found in Task 21b)

Found when Task 21b's start-up timing made test_v92a fail: after a rate
renegotiation begun by the digital modem, our V.92 calling side retrained
and the retrain failed. Present at e71e131 and at c282fb1 (before the
audit). test_v92a passed only because its windows (4 s of data, a
renegotiation, 2 s) ended before the event.

The line: test_v92a's (p2d and v92d without answer.c, 2 km, +30 ppm, echo
-10 dB). Data mode from 8.17 s; at 13.24 s the receiver's error goes from
4.7e4 to 2.4e7 in 2 ms and keeps growing. Up to then, sampled every
250 ms: A/D input about 6000 RMS, canceller output about 1650 (the echo
stays cancelled), the timing step at 1.0000300 (+30 ppm), its delta within
+-30, the error 13000 to 18000. Our clock slips one sample against the
network's at 6.2, 10.4 and 14.5 s, not at 13.24 s; the canceller runs on
our clock, so the drift does not move our echo. (The first guess, a
canceller not following the drift, is wrong.)

What happens at 13.231 s: the only full-scale A/D sample of the call's
data mode. Our PCM upstream peaks at 15783 (RMS about 2500); its echo is
-34392 there and the A/D sample would be -35579, clipped at -32767 (an
error of 2812). The error grows from 64 samples later, the delay from the
A/D to the decision at the equaliser's cursor.

The same without clipping (echo -16 dB, which never clips): 50 units added
to one A/D sample at 11.000 s lose the V.92 downstream for good (5000 and
more check failures, then our retrain); -50, 1000 and 20000 too; 20 and
less are survived. V.90 (V.34 upstream) the same.

Mechanism: the equaliser's feedback carries the line's low-frequency tail.
On this line b[0] = 0.73, b[1] = -0.55, b[3] = -0.30, ..., sum |b| = 3.9,
sum b^2 = 1.23. A wrong decision feeds back errors larger than half a level
step; they make more wrong decisions and the error never decays. Not the
adaptation: holding the taps (rx.hold) for 50 ms or 500 ms from the
impulse changes nothing. Feeding back the equaliser's soft output for
unreliable decisions does not stop it either (at e^2 above 64 gmse the
runaway goes on; at 16, 9 or 4 gmse the receiver is lost without any
impulse: the feedback is unstable run linearly).

Treating the clipped sample as an erasure (ruling (e)): tried and not
taken. The sample cannot be repaired: replaced by 0 it is off by the
downstream signal (1187 here), kept it is off by the clipping (2812), and
the decisions survive 20 to 50. Neither changes the outcome (the call ends
the same, at 21.56 s, the retrain failing in this harness). Nor does
feeding back soft values for 48, 96 or 160 symbols after it. Bounding our
own PCM upstream's peaks: not taken. They are the codewords the digital
modem decodes level by level (V.92 bounds the average power, not the
peaks), so cutting them would corrupt the upstream data; a conformant
reduction would go through the digital modem's precoder and filter
choices (CPd), which are not ours to make.

Taken (ruling (d)): recovery by retrain, as a real call recovers. The
receiver's watch retrains (12 dB over for 250 ms); in a whole call answer.c
restarts the digital modem. test/v90link.h gains `d_retrain` (a retrain of
either end restarts p34d / v92d the way answer.c does: 70 ms of silence,
Phase 2 of a retrain, PCM upstream offered again unless the digital modem
never reached data mode, failed, or its upstream did not hold the lowest
rate) and `imp_ms` / `imp_amp` (a click on one A/D sample). test_v92a runs
with `d_retrain`. Its new case, a 50-unit click in data mode, recovers in
10.06 s with the data correct after (10.97 s after Task 21b's start-up
changes; the check allows 15 s, Task 21b fix round). Without `d_retrain` it ends in
V90A_ERR_RETRAINS. test_v92a's other output is unchanged.

Deferred (a design item after the audit): recovery in v90arx without a
retrain, by
- re-seeding the feedback line and decisions after a runaway from a
  feedback-free estimate (whether a linear equaliser decides well enough
  at 48 kbit/s on this tail is open);
- a feedback design that cannot run away (part of the tail in the forward
  filter, or sum |b| bounded), at some cost in error and rate; or
- spectral shaping (V.90 Appendix I, CP's Sr), which removes the low
  frequencies the tail comes from (listed in docs/limits.md as a gap).

## Task 21b: calling start-up timing for live servers (golden changes explained)

Commits 2f735ad (the far L2 read for 280 ms), 22b0f72 (the DIL up to a
third of full scale), edadbde (Sd waited for 2000 ms), 2602a5d (guard
tests). Two golden changes their commit messages did not explain:

- test_orig_v90, "2 km, +30 ppm, 3 dB pad ... Table 1 levels" (2f735ad:
  connected at 28000, now never reaches data mode). The row is the test's
  deliberately wrong setting (Table 1's levels instead of what the DIL
  measured, on a line with a 3 dB pad), there to show what the DIL
  analysis buys; it checks only that it does worse than the DIL row. Our
  CP design, deciding TRN2d against levels 3 dB off, measures a noise
  that no rate fits (CP's K 0) and the calling modem clears down in Phase
  4 (the digital modem: P34_ERR_CLEARDOWN). Up to 2f735ad it found
  28000, the lowest rate, by a margin that the changed start-up (the
  probe analysis over 14 blocks, Phase 2 180 ms shorter) took away. It
  fails the same way with the old L2 read put back on top of the DIL
  change: a marginal row, not a regression. The DIL-measured row keeps
  48000.
- api/test_api, "tx_level 16384 both, V.90 back to back" (22b0f72: no
  data mode -> data mode). Not a change of headroom: the calling modem's
  V.34 upstream still goes over full scale near the top (43 samples in
  the mu-law call at 15360, 93 in the A-law call at 16384; peaks 41830 and
  44619 before the clip). What
  changed is the upstream rate the answerer picks from its measurement:
  26400 before, where our B1 did not get through (P34_ERR_B1 in Ed), and
  24000 now, where it does. Putting the DIL back to Ucodes 0..119 brings
  the failure back. docs/api.md, include/v90modem.h and test_api's
  comments say so; 15360 stays the level checked to connect.

## x2: one click must not ruin the upstream (Task 20; dsp 2.1, 2.2; D11)

The case: the x2 call of `tools/rec_x2 40 0`, with the six samples to the
answerer at 15.000 s replaced by full-scale mu-law (0x80 / 0x00
alternated, 0.75 ms). The upstream mse went from 16 to 257..363 (Q16) and
stayed there to the end of the call; x2d has no retrain, so the call stayed
up and decoded garbage. The same happened on the live x2 line
(x2-usr-ppp3-fixed.rec, replayed with the click added at 50 s): -39 dB to
-27 dB for the remaining 190 s.

**Root cause: equaliser-tap damage, not the loops.** Two answerers ran on
the same recording, one with the click. At 15.5 s one part of the clean
receiver's state was copied into the clicked one, a part at a time. Copying
`freq`, `theta`, `t_integ` or `tnext` / `m` / `nsym` left 300 000 of 713 400
bits wrong. Copying the taps `cr` / `ci` restored the call: the bits lined
up again, and the only errors were in the half second before the copy. The
click passes along the whole 72-tap line (24 symbols), and each LMS update
on the way moves every tap by the error times the click. The resulting tap
error sits at the band edges (about +-3200 to 3600 Hz, up to half the
passband gain). The signal has little energy there, so the tracking step
(2^-13) never takes it back: the error is the same 10 s later. The
wandering of `freq` and `t_integ` that dsp saw is a result of the bad
decisions, not their cause.

The brief's bounds, each alone, then combined (bits wrong of 713 400; the
clean run unchanged in every case):

| try | result |
|---|---|
| (a) er, ei clamped to +-32767 before the updates (the largest error in the click is 39101) | 299 483 |
| (b) freq's step per symbol bounded, phi to +-8192 = +-2^17 (clean max 5851) | 299 725 |
| (b) t_integ bounded to +-2048 (clean max 599) | 300 063 |
| (b) both | 299 796 |
| (a) + (b) | 299 325 |

**The fix (c): impulse blanking in data mode.** In data mode x2d, k56d,
p34d and v34dx pass the receiver's input through `qamrx_blank()`
(qamrx.h): a sample more than 18 dB above the input RMS measured at S (8 times
it; the AGC gain is set once there and frozen in data mode, so the
threshold does not follow later level changes; a full-scale click of 32124
is blanked only if that RMS was under 4015, about -12 dBm0) goes in as 0. No field was added (F27). With it
the click costs 74 bits in x2, and 0 bits are wrong from 2 s after it; the
same holds for K56flex, V.90 and V.34 (test_answer_x2, _k56, _v90, _v34).
On the live x2 line, 63 bits differ around the click and none after it.
Variants that were rejected:
- Blanking in `qamrx_tick` in every phase at 18 dB: some training sequences
  have peaks that loud (mid-call changes in com1, c3, pw_19_v90 and
  pw_29_v90).
- Blanking at the interpolator's clip (24 dB): it never fires on a line
  louder than the AGC target, and the V.90 click test fails.

**Replay proof: 42 runs re-recorded (the controller's ruling).** The
blanking fires in exactly 42 of the 636 runs, and those are exactly the
runs that differ. In each, it fires on a single burst of 67 to 84
full-scale samples that begins 54 to 72 ms before the recording ends: the
line's hang-up click. Every run is byte-identical up to that point, and
only the printed trace changes, not the bits. The recordings are
ab_cc_v34, ab_cl_v34, v34c, x2b, x2r1, x2r3 and x2r4, each in all six
flag sets. The goldens of the other programs and the bench hashes did not
change.

**D11's watchdog is not used.** It would end x2 / K56flex data when the mse
stayed above p34d's `mse_limit` for 2 s. For x2 at 24000 up and 3200 baud
that limit is 128 (-27.1 dB). No mse threshold separates a line deafened by
a click from a clean low-SNR line:
- The lowest SNR at which x2 connects in the test harness (noise rms 92,
  23.5 dB) runs at mse ~290, and the click left 257..363.
- The lowest SNR with a clean upstream (noise rms 68, 26.8 dB: 0 bad bits
  in 60 s) runs at ~137, above the limit.
The watchdog would end error-free calls near 27 dB. Both lines are in
test_answer_x2 (not ended; the second also clean).

dsp 2.2 (er / ei unbounded before 32-bit products): the clamp (a) did
nothing for the click. The largest error seen in it was 39101, which keeps
`pr * x` (at most about 1.8e9) inside 32 bits, and UBSan stays silent on
it. The clamp does not qualify as a fix (D11), so it was not added; a
larger disturbance than this click is still open there.

Cost (ARMv5, `make cost-x2 ARM_CC=$A5`, x2 data mode, ARM instructions per
second of line):

| | before (42e1c67) | after | change |
|---|---|---|---|
| x2 data mode | 82 757 983 | 82 845 486 | +0.09 M |
| V.90 data mode | 39 347 034 | 39 410 948 | +0.06 M |

That is one compare and one multiply per sample (the brief allows +0.3 M).

## Phase 1 gate

At 5c5e541 (Phase 1 complete, both tracks merged), 2026-11-23. `make
verify-phase` is deferred to the release proof (Task 39) by the owner's
ruling; `make verify` passed at this commit (merge B2).

### Replay proof against v0.5.0: 102 of 636 runs differ, all explained

`tools/replay-proof.sh diff v0.5.0` (recordings
`/home/georgica/sync-build/live-050`): `636 runs, 102 differ from v0.5.0`;
102 traces (.txt) and 3 bit files (.bits, pd_36000 in the V.92 sets), no
exit code. Every one of them was re-recorded, with its reason, by one of
the commits below (the union of their manifest changes, the merge left
out, against the 102: nothing differs that no commit explains).

| task | commit | runs | recordings (flag sets) | what changes |
|---|---|---|---|---|
| Task 7 | dbf91d4 | 21 | x2r2, v34a, v34b (all six); usr7 (v92, x2-v92, k56-v92) | end codes and the retraining / renegotiation figures only: x2r2 LOST (2), usr7 RETRAIN_FAILED (8), v34a/v34b stops counted as renegotiations |
| Task 10b | 2de5d4b | (x2r2, above) | x2r2 (all six) | +288 upstream bits at the caller's S (the mapping frames held at S delivered) |
| Task 10c | 3e4128e | (v34a, v34b, above) | v34a, v34b (all six) | +288 bits at each V.34 S; our tx S after the mapping frame (+1 ms) |
| Task 20 | 807d828 | 42 | ab_cc_v34, ab_cl_v34, v34c, x2b, x2r1, x2r3, x2r4 (all six) | the blanking fires on the hang-up click in the recordings' last 54-72 ms; identical up to it; no .bits change |
| T21.2 | 5aca43d | 39 | cxb, cxd, cxf, cxg, f4, pd_36000, pd_37333, usr1, usr2, usr4, usr5, usr_c33333, usr_c36000 (v92, x2-v92, k56-v92) | SUVd between repeated CPds, 22-26 s in, after our transmit already left the recording; pd_36000 now reaches data mode (its .bits) |
| T21.3, T21.8 | 596817b, 42e1c67 | 0 net | cxe, h1-h4 (the three V.92 sets) | T21.3 changed 15 traces after the recordings' end (our echo residual read as Ru); T21.8 put them back: they are identical to v0.5.0 |
| ISP-3 | 83eb991, 793f293 | 0 new | v34b, x2r2 (all six); usr7 (v92, x2-v92, k56-v92) | the end-of-call summary line only: renegotiations (a cleardown no longer counted as one), SNR and round trip kept after the call instead of -1; 15 traces, all already in Task 7's 21 |

21 + 42 + 39 = 102; the three groups do not overlap. (The ISP-3 row was
added after this gate, at the rc1 fix wave: it re-recorded traces that
already differed, so the count stays 102.)

### Costs after Phase 1

The verify-phase cost stage on its own (`make cost-x2 cost-k56 cost-v34
cost-v90a cost-v90c cost-v92c cost-bell ARM_CC=$A5 COST_TICKS=0`, armv5
uClibc, gcc -O2, ARM instructions counted under qemu), M instructions per
second of line in data mode, against the baseline (v0.5.0 code) above:

| | baseline | after Phase 1 | change |
|---|---|---|---|
| x2 answering (second 15 to 16) | 82.57 | 82.85 | +0.28 |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.20 | 39.40 | +0.20 |
| K56flex answering (second 20 to 21) | 39.27 | 39.64 | +0.37 |
| V.34 answering (second 10 to 11) | 40.53 | 40.71 | +0.18 |
| V.90 downstream receiver, 0 / 100 ppm | 44.62 / 48.97 | 44.57 / 48.92 | -0.05 / -0.05 |
| V.90 calling, 0 / 100 ppm | 55.11 / 59.31 | 55.06 / 58.89 | -0.05 / -0.42 |
| V.92 calling, 0 / 100 ppm | 69.95 / 74.32 | 70.16 / 74.39 | +0.21 / +0.07 |
| Bell 103 / Bell 212A calling (first second) | 9.86 / 4.43 | 9.87 / 4.69 | +0.01 / +0.26 |
| K56flex, the heavy client (Task 19, second 20 to 21; separate) | 84.43 | 84.54 | +0.11 |

Every row is within 0.5 M of its baseline. The largest relative change is
Bell 212A (+5.9 %): Task 13's v22rx products in 64 bits (49ff9a4), measured
on its own: 4.46 M before it, 4.69 M after (+0.23 M); the rest (+0.03 M)
came earlier. K56flex +0.37 M is the closest to the bound.

For Phase 3 to watch (each costs little or nothing in these steady-state
windows, so the table does not show it):
- T21.3: the Ru / Su / Sd detector's products in 64 bits (`v92_p6_tick`),
  every sample in V.92 data mode on ARMv5 (part of V.92 / K56 / x2 rows'
  rise; also Task 13's 64-bit products in v22rx, above).
- T21.7: the B1u gain search, some 175 k instructions in one tick on most
  real calls (not in any row: the simulated calls sit at scale 1, 14 k).
- K56flex catch-up after a heavy client's B1 retry: 10.2 ms of CPU per
  10 ms of line, lateness peaking at 11.7 ms inside the 20 ms prefill
  (Task 19, above); not re-measured here.
- Task 20's blanking: one compare and one multiply a sample in x2 /
  K56flex / V.34 / V.90 upstream data mode (+0.09 M x2, +0.06 M V.90 when
  measured).

### Memory

`v90m_mem_size()` as it stands: 118824 octets on x86-64, 118512 on ARMv5
(armv5 uClibc, under qemu) — unchanged from v0.5.0 (README). V.42:
`v90m_ec_mem_size()` 49792 (x86-64), 49568 (ARMv5); `v90m_async_mem_size()`
1064 on both.

## Task 24: one V.34 upstream receiver (v34up) for p34d and v34dx

`modem/v34up.{c,h}` (ans R3); p34d and v34dx run on it, byte-identical
(`make verify`: golden 128 identical, test_v34up's own new; replay 636
identical; bench hashes identical). Cost (`make cost-x2 cost-v34
ARM_CC=$A5 COST_TICKS=0`, armv5 uClibc), M ARM instructions per second
of line in data mode, at 49ef8e7 (before) and with v34up (after):

| | before | after | change |
|---|---|---|---|
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.33 | 39.52 | +0.18 |
| V.90 answering (`cost-v34`'s, second 10 to 11) | 39.33 | 39.52 | +0.18 |
| V.34 answering (second 10 to 11) | 40.58 | 40.59 | +0.01 |
| x2 answering (second 15 to 16; x2d not yet on v34up) | 82.70 | 82.69 | -0.00 |

Both within +0.3 M of before (and of the Phase 1 gate, 39.40 / 40.71),
so the per-symbol functions stay out of line in v34up.c. V.90's +0.18 M
is p34d's data path now going through v34_nl_decode / v34_nl_encode
(identities at cfg.nonlinear 0, test_v34up) and two more calls a symbol;
Phase 3 may inline v34up_data or skip the identities. Bench host hash =
qemu hash, unchanged from before.

Memory: p34d_t 8 octets smaller (padding; 25416 x86-64, 25344 ARMv5),
v34dx_t unchanged (23040 / 22976), `v90m_mem_size()` unchanged.

## Task 25: x2d and k56d on v34up; the K56 B1 retry is v34up_retry

x2d and k56d run on `v34up` (ans R3), byte-identical (`make verify`:
golden 128 identical, replay 636 identical, bench hashes 17 identical;
the K56 heavy and x2/K56 click cases unchanged). New in v34up:
`v34up_watch` (the dead / slow / loss watches, shared by v34up_data and
k56d's data path), `v34up_retry` (D12's retry under the other
configuration, the same putbit), `v34up_replay_sym` (one stored point
into the decoder), and `v34up_b1_in`. k56d's ring, catch-up and pacing
stay in k56d_t. v34up_data skips v34_nl_decode / v34_nl_encode when
cfg.nonlinear is 0 (they return their input then: Task 24's review,
Minor 1). `git diff --stat` of the code: 10 files, 200 insertions, 291
deletions (x2d.c and k56d.c: 248 lines out, 128 in; v34up: 48 in).

Cost (`make cost-x2 cost-k56 ARM_CC=$A5 COST_TICKS=0`, armv5 uClibc), M
ARM instructions per second of line in data mode, at 3f97acf (before)
and after:

| | before | after | change |
|---|---|---|---|
| x2 answering (second 15 to 16) | 82.69 | 82.83 | +0.13 |
| K56flex answering (second 20 to 21) | 39.58 | 39.58 | +0.01 |
| K56flex, a client that ignores our MP (`K56_ARGS=heavy`) | 84.40 | 84.40 | +0.00 |
| V.90 answering (second 15 to 16) | 39.52 | 39.44 | -0.08 |

All within +0.3 M of before. V.90 gets back 0.08 M of Task 24's +0.18 M
by skipping the identity calls; the rest is the extra calls a symbol.
x2's +0.13 M is the out-of-line v34up_data and v34up_watch calls. Bench
host hash = qemu hash, the same as before.

Memory: x2d_t 20296 -> 20312 octets on x86-64 (20264 -> 20280 on ARMv5),
k56d_t 22848 -> 22856 (22800 -> 22808): v34up_t brings gain, mse_limit and
q_bad. Both are in answer_t's union, which a larger member sets, so
answer_t (106176 / 105896) and `v90m_mem_size()` are unchanged; no test_api
pin changes.

## ISP-1: the echo search near the round trip (V.92, x2, K56flex); replay -r

PR #1's `ecdly_near` (p34d) extended to v92d, x2d and k56d (a93fa17);
v34dx unchanged (its far canceller is placed on the round trip). Below a
round trip of 1024 samples nothing changes: golden only gains the new
tests' lines, replay 636 identical. `v90m-replay -r RMS` (74a06aa). Reach
and trade-off: docs/line-interface.md "Echo delay", docs/limits.md.

**Follow-up (calling side, not investigated).** test_v34dx's far-echo case
shows our calling V.34 modem's downstream falling with the round trip:
A->C 31200 bit/s at 100 ms, 28800 at 300 ms, 12000 at 400 ms (the call
side has its own near echo, 0.3, and a far canceller on with nothing at
the round trip). The answer side is clean throughout. The 12000 is frozen
in test/golden/out/test/test_v34dx.txt; a fix that raises it changes that
golden line on purpose.

## Task 26: answer.c, one file per scheme behind a phase table

`modem/answer.c` (887 lines) keeps init, V.8, the dispatch, the end codes
and the backstop (289 lines); the schemes are `modem/ans_legacy.c`
(V.21, Bell 103, V.22bis / V.22 / Bell 212A with the 1270 Hz and 980 Hz
detectors, V.32bis), `ans_v90.c` (Phase 2 and the retrain silence, V.90,
V.92, the hand-off to V.34), `ans_v34.c`, `ans_x2.c`, `ans_k56.c`
(V.8bis, K56flex, its client's retrain), behind `ans_scheme_t`
(`modem/ans_scheme.h`). `answer_t.s` is the scheme of the phase, set by
`ans_enter` from a phase table. The signal to noise, round trip and the
echo delay between engines stay the modulation's, as before. Byte
identical (`make verify`: no golden, replay or bench re-record; the two
white-box tests that put a phase in place by hand now use `ans_enter`).

Retrain counting as it was, per scheme. V.90, V.92 and V.34 after a V.90
Phase 2 share `ans_rt_v90` (`rt_policy_t {3, 60000, 0}`): the retrain due
with 3 in a row ends the call before it is counted; a clock running
through every data mode sample forgets one per minute. K56flex keeps its
own code (counted, clock restarted, then the third ends the call as
K56flex's; all forgotten after 2 s of data mode on a clock that runs only
while there are any). V.34 from V.8 and V.32bis count in their engines;
x2 has none.

ARMv5 (uClibc), M instructions per second of line, before at 8d0c652:

| | before | after | |
|---|---|---|---|
| x2 answering (second 15 to 16) | 82.83 | 82.80 | -0.03 |
| K56flex answering (second 20 to 21) | 39.58 | 39.54 | -0.04 |
| V.90 answering (second 15 to 16) | 39.44 | 39.49 | +0.05 |

The per-sample tick now goes through `a->s->tick` (an indirect call in
place of the switch); V.90 pays +0.05 M, x2 and K56flex gain a little
from the smaller per-scheme functions. Bench hashes unchanged.

Memory: answer_t gains the scheme pointer, 106232 octets on x86-64
(105936 on ARMv5), still below orig_t (118712 / 118424): `v90m_mem_size()`
unchanged.

## Task 27: v90a.c, split into V.90 upstream, V.92 upstream, downstream sequences and the watch

First, from Task 26's review (no change in behaviour): `rt_policy_t`
loses `count_after` (no policy set it; `ans_rt_v90` is `{3, 60000}`),
`rt_begin` / `rt_data_tick` are `ans_rt_begin` / `ans_rt_data_tick` (the
latter returns nothing), and `modem/answer.c` checks at compile time that
its phase table has one row per phase.

`modem/v90a.c` (1441 lines) keeps init, the sample tick, the public calls
and the call's end (201 lines). `v90a_up90.c`: Phase 3's start and V.90's
V.34 upstream (`up_symbol`, `start_data`, the sequence being sent).
`v90a_up92.c`: V.92's Phase 3 start, Jd / Jp / Jp', TRN2d's SUVd / CPd /
Ed (`train92`) and the PCM upstream (`up92_symbol`, `seq_next`,
`start_b1u`). `v90a_down.c`: the downstream sequences (Jd, Jd', the DIL,
Ri, TRN2d, MP, Ed, B1d, data frames, Rd), the DIL's analysis and the CPt /
CP designs as jobs, the symbol renumbering. `v90a_watch.c`: the time
limits, the receiver's watch on itself, our renegotiation, the retrain.
The sub-states, constants and the shared calls (`v90a__*`) are in
`v90a_priv.h`. Byte identical (`make verify`: no golden, replay or bench
re-record); `v90a_t` unchanged, so `v90m_mem_size()` too.

ARMv5 (uClibc), M instructions per second of line (seconds 2 to 3 of data
mode), before at 15ee679:

| | before | after | |
|---|---|---|---|
| V.90 caller, 0 ppm | 55.21 | 55.19 | -0.02 |
| V.90 caller, 100 ppm | 59.04 | 59.02 | -0.02 |
| V.92 caller, 0 ppm | 70.35 | 70.33 | -0.02 |
| V.92 caller, 100 ppm | 74.57 | 74.56 | -0.02 |

The per-symbol calls that now cross files (the upstream symbol, each
decision, the watch) cost nothing measurable; bench hashes unchanged
(bbf20f60 V.90, 484a898d V.92, host and qemu).

## Task 29: the calling side's engines share memory; new pins (Phase 2 gate)

`orig_t`'s engines after V.8 are one union, `e`: `e.leg` (the V.22bis and
V.32bis callers together: Annex A runs both in ORIG_LEGACY, Bell 212A is
V.22bis's), `e.v34`, `e.v90`. Each `start_*` initialises its own and
`orig_init` clears all; every getter reads only the engine of the phase.
The one read after the end, `orig_retrains` in ORIG_FAILED, takes V.34 or
V.90 by the modulation, which only that engine sets, and no engine starts
after ORIG_FAILED: its output is as before. `tools/rec_orig` printed the
V.90 engine's state and error whatever the call became; it now prints
them only when JM chose V.90 (0, 0 otherwise, as an engine never started
gave). Also removed: `v22bis_t.total` (read nowhere) and, outside
`V34_TEST_API`, `v34_rx_t.hard` / `hard_y` and the `rx->hard` test in
`v34_rx_flush_before` (24 octets off answer_t and every V.34 receiver).

Byte identical (`make verify`: replay 636 identical, bench hashes
identical); one golden re-record, `test/golden/out/api/test_api.txt`: its
count line, 924 -> 925 checks (the new CHECK that the size is at least
12000 octets below 0.5.0's).

### Memory per target

`v90m_mem_size()` (octets) on every port target of `tools/check-ports.sh`,
read by a program that prints it, built with each target's compiler and
flags and run under its runner, with `api_test_api` (925 checks, 0
failures on every target; `v90m_mem_size() <= V90M_MEM_SIZE` among them).
The port suites themselves were not run (deferred to Task 39).

| target | before (orig_t sets it) | after (answer_t sets it) | orig_t after | answer_t after |
|---|---|---|---|---|
| host-gcc, host-clang, x86_64-musl | 118824 | 106320 | 53752 | 106208 |
| aarch64, aarch64be, ppc64, riscv64, s390x | 118824 | 106320 | 53752 | 106208 |
| armv5-uclibc, armv7hf-musl, mips, mipsel, ppc, win32-mingw | 118512 | 106016 | 53592 | 105928 |
| i686-musl | 118296 | 105832 | 53476 | 105748 |
| m68k-uclibc | 118188 | 105704 | 53414 | 105622 |

Before, `orig_t` (118712 x86-64, 118424 ARMv5) set the size; after, the
answerer's `answer_t` does: `orig_t` lost 64960 octets (x86-64), but the
modem takes only 12504 less (12496 on ARMv5), the gap to answer_t. The
brief's target, at most 118824 - 12000 = 106824 (x86-64) and 106512
(ARMv5), is met. From here on, shrinking the calling side further (D16's
deferred union inside `v90a_t`, some 6 KB) no longer lowers
`v90m_mem_size()`; only `answer_t` (its union, or the V.34 and Phase 2
state outside it) does. Pins: 106320 (x86-64), 106016 (32-bit ARM).
`V90M_MEM_SIZE` (D17): the largest, 106320, + 2048 = 108368, up to a
multiple of 4096: 110592 (108 KiB, was 120 KiB). `V90M_EC_MEM_SIZE` and
`V90M_ASYNC_MEM_SIZE` unchanged (49792 / 1064 x86-64).

### Costs after Phase 2

`make cost-x2 cost-k56 cost-v34 cost-v90c cost-v92c ARM_CC=$A5
COST_TICKS=0` (armv5 uClibc, each in its own BUILD), M ARM instructions
per second of line in data mode:

| | after Phase 1 | after Phase 2 | change |
|---|---|---|---|
| x2 answering (second 15 to 16) | 82.85 | 82.79 | -0.06 |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.40 | 39.46 | +0.06 |
| K56flex answering (second 20 to 21) | 39.64 | 39.54 | -0.10 |
| V.34 answering (second 10 to 11) | 40.71 | 40.51 | -0.20 |
| V.90 calling, 0 / 100 ppm | 55.06 / 58.89 | 55.19 / 59.01 | +0.13 / +0.12 |
| V.92 calling, 0 / 100 ppm | 70.16 / 74.39 | 70.32 / 74.55 | +0.16 / +0.16 |

Every row within 0.5 M of after Phase 1. The calling rows equal Task 27's
after (55.19 / 59.02 / 70.33 / 74.56) to 0.01 M: the union itself costs
nothing; their rise since Phase 1 was already there before this task.

## Task 30: the S detector's and qamrx's divisions that are shifts; tap update (dsp P1, P4, P7, P15)

- P1: `sdet_tick`'s threshold `pw * len^2 / div` was a 64-bit division
  every sample. `div` is 8 or 16 wherever the modem sets it (sdet_init;
  v34dx, x2d, k56d), so the threshold now divides by the constant 8 or 16
  when `div` is that value, and by `div` otherwise: the constant form is
  only taken where it is the same number.
- P15: the threshold and |z|^2 only when `pw > 400`; o2 and p only while
  the detector is armed (`holdoff < 0`), where they are read.
- P4: under `QRX_DENSE` qamrx_update divides `|e|^2 << 16` and
  `num * 65536` by the constant 2^25 (`DENSE_DN`, = 2 QRX_REF^2, checked
  at compile time); the timing gradient divides by it when `dnorm` is
  2^25, by `dnorm` otherwise.
- P7: the equaliser's tap update takes two taps a pass (QRX_NTAP even,
  checked at compile time).

Proof: `test/test_exact.c` (its own commit, before this one) runs both
against verbatim copies of the code before (sdet with div 8, 16, 5, 12
over 2*10^6 samples of noise, S and S-bar; qamrx_update on 10^6 random
states and on edge states; the timing third of qamrx_tick on 10^6 random
states with dnorm 2^25, |d|^2, 1, 2^31 - 1 and near 2^25): every field
and return value equal. `make verify`: golden 132 identical, replay 636
identical, bench 17 identical; no golden file re-recorded. The bench
transmit / received hashes are the same on the host and under qemu, and
the same as before.

### Costs

`make cost-x2 cost-k56 cost-v34 ARM_CC=$A5 COST_TICKS=0` (armv5 uClibc,
each in its own BUILD), M ARM instructions per second of line in data
mode:

| | before | after | change | dsp's estimate |
|---|---|---|---|---|
| x2 answering (second 15 to 16) | 82.79 | 79.19 | -3.60 | -4.4 (P1 -2.5, P4 -1.1, P7 -0.5, P15 -0.3) |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.46 | 35.58 | -3.88 | -3.85 and P7, P15 |
| K56flex answering (second 20 to 21) | 39.54 | 35.94 | -3.59 | |
| V.34 answering (second 10 to 11) | 40.51 | 36.92 | -3.59 | |
| V.90 answering (`cost-v34`'s, second 10 to 11) | 39.46 | 35.58 | -3.88 | |

The four items were measured together, not one by one.

## Task 31: the V.34 non-linear decoder without most 64-bit divisions (dsp P2, P9)

`v34_nl_decode` (x2's upstream: non-linear, every symbol) did nine 64-bit
divisions by `energy_q8`, eight of `ep 2^28` by `(Phi^2) >> 4` and two of
`x 2^17` by Phi, each some 250 instructions on ARMv5. Now:

- the nine divisions by `energy_q8` through its reciprocal, computed once
  a call (`modem/divrcp.h`; no new struct field);
- `(ep << 28) / ((Phi^2) >> 4)` by `divlu` (new `modem/fixdiv.h`,
  Hacker's Delight's 64 by 32 bit division with a 32-bit quotient, two
  32-bit divides): ep <= 2^31 and `(Phi^2) >> 4` >= 2^28, so the high
  word is below the divisor;
- the iteration stops when Phi repeats (the next e, and every round
  after, would be the same); a Phi alternating between two values still
  runs all eight rounds;
- the final `x 2^17 / Phi` as `2 (a / Phi)` plus one if twice the
  remainder reaches Phi, a = |x| 2^16 <= 2^31: 32-bit divides, then the
  sign;
- `v34_nl_phi_q16` (and so `v34_nl_encode`) divides by `div_s64_u32`
  (divlu when the quotient fits 32 bits, C's division otherwise): one
  division a call, where a reciprocal would itself cost a 64-bit one.

`V90M_CLZ32` (divlu's normalisation, one CLZ on ARMv5) and
`V90M_NOINLINE` are in the new `modem/compiler.h`, each builtin behind a
C99 fallback; `-DV90M_NO_BUILTINS` takes the fallbacks.

Proof: `v34/test/test_nl_exact.c` (its own commit, before this one) runs
the three functions against verbatim copies of their v0.5.0 code over all
130 parameter sets (symbol rate x rate x shaping): the int16 plane in
steps of 256, its edges, 20000 random points about the constellation each
and Phi of energies of any size; divlu, div_s64_u32 and V90M_CLZ32 against
C on 10^7 random cases each and their edges. It is built twice, the second
time (`test_nl_exact_nb`) with `-DV90M_NO_BUILTINS` for it and v34_nl.c.
0 differences. Run once by hand with `dsp` (the audit's scope: the plane
in steps of 97 by 89 and 200000 random points a set, 90 767 560 inputs,
plus the edges): 90 793 296 points decoded and encoded, 0 differences.
The whole plane at step 1 (`full`, 5.6 * 10^11 points) would take some 23
CPU hours; one of its 4096 column shards (`full 1 4096`: 136 314 880
points) gave 0 differences.
`make verify`: golden 134, replay 636, bench 17 identical; no golden file
re-recorded (the two new programs' own are new). The bench transmit and
received hashes are the same on the host and under qemu, and the same as
before.

### Costs

`make cost-x2 cost-k56 cost-v34 ARM_CC=$A5 COST_TICKS=0` (armv5 uClibc,
each in its own BUILD), M ARM instructions per second of line in data
mode:

| | before | after | change | dsp's estimate |
|---|---|---|---|---|
| x2 answering (second 15 to 16) | 79.19 | 69.94 | -9.25 | -9.7 (`v34_nl_decode` 16.0 -> 6.3) and -0.55 (`v34_nl_encode`) |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 35.58 | 35.58 | 0.00 | |
| K56flex answering (second 20 to 21) | 35.94 | 35.94 | 0.00 | |
| V.34 answering (second 10 to 11) | 36.92 | 36.92 | 0.00 | |
| V.90 answering (`cost-v34`'s, second 10 to 11) | 35.58 | 35.58 | 0.00 | |

Only x2's upstream is non-linear in these calls (v34up.c decodes each
symbol and re-encodes its decision); the decoder and the encoder were
measured together, -9.25 M against about -9.7 M expected. (Corrected
in Task 38 from "dsp's -10.25 M for both", which counted the encoder
twice: dsp's -9.7 M prototype already changed `v34_nl_phi_q16`, and P9's
-0.55 M was for a reciprocal kept in `v34_params_t`, a struct field this
task did not add.)

## Task 32: the Viterbi add-compare-select; the subset slicer in one pass (dsp P3, P3b, P6)

- P3: `acs()` (v34_rx.c) does the add-compare-select for every target
  state, out of line (`V90M_NOINLINE`) so that its few pointers stay in
  registers: four predecessors a pass (nkeys is 4, 8 or 16), only the
  winner's position carried through the loop, prev and the branch label
  stored once a state. The same strict `<` in the same (ascending)
  predecessor order, the same first predecessor for a state not yet
  reachable, the same first best state. The labels go through `bpair[]`
  afterwards, in the renormalisation's loop.
- P6: `v34_slice_subsets` slices the eight subsets of the 32- and
  64-state codes in one pass. In a = re + im, b = re - im their lattices
  have four residues of a and four of b modulo 8 between them: the four
  nearest a, the four nearest b and their squares are found once, a
  subset's point is the pair its residues pick, its squared distance
  (da^2 + db^2) / 2, exactly. Outside the constellation the same
  `nearest_inside` as before. `v34_slice_subset` stays as the tests'
  reference. `v34_slice_class`'s brute-force branch (constellations of 8
  points or fewer, through `v34_slice_classes`, 16 states) stays: the 8-way
  pass does not cover the 4-way partition.
- P3b skipped: it costs more than it saves. Kept exact (path metrics left
  unnormalised once every state is reachable, `pm[best]` taken off the
  branch metrics instead; the old renormalisation while a state is still
  unreachable), test_vit_hash and the suite unchanged, but the decoder
  counted 8375 instead of 8350 ARM instructions per 2D symbol at 64
  states, 4500 instead of 4463 at 32 and 1701 instead of 1688 at 16
  (`test_bench count`, armv5): the renormalisation already shares its loop
  with the label mapping, and taking `pm[best]` off up to 32 branch
  metrics plus the check for an unreachable state cost more than the
  compare and subtraction it removes.

Proof: `v34/test/test_vit_hash.c` (its own commit, before this one):
10^6 2D symbols from a fixed LCG (data and noise, integers only) through
`v34_rx_put` at 16, 32 (precoded) and 64 states, three noise levels each,
a hash of every trellis step's survivor history (nearest points,
predecessors, pairs), best state, smoothed metric and decoded bits. The
first noise level moves each coordinate by -1, 0 or +1 unit exactly, so
that path metrics tie: a throwaway probe counted 1.26 M ties in 3.56 M
compares at 16 states and 11.7 M in 56.9 M at 64 (about 35 000 and
80 000 in the other runs). Mutations of the new code each change the
printed hashes: `<=` for `<` in any of the four compares (compare 2's
checked by the task's reviewer) or in the best state's, a reordered pair
of compares, a wrong first predecessor; in the slicer, the residue table,
the rounding offset, the `V34_M_BIG` value. The
slicer against eight `v34_slice_subset` calls, metric and point: all 50
constellations without precoding (8 of 8 points or fewer) and three
precoded (ymax 9, 47, 227), the int16 grid in steps of 40 within the
clamping window, the plane's edges and 10000 random points a set; by hand
`full` (steps of 8: 95 725 421 points) and `plane` (the whole int16 plane
in steps of 8 for every set: 3 556 769 792 points, 8 shards), 0
differences. A rounding changed in the slicer shows in the suite (on the
grid of 8 alone it would not: every squared distance there is a multiple
of 128, hence the random points). The ARMv5 build's (short) output is the
same before and after.
`make verify`: golden 135, replay 636, bench 17 identical; ubsan, armv7hf,
bigend, fixed ok; no golden file re-recorded (test_vit_hash's own is
new). The bench transmit and received hashes are the same on the host and
under qemu, and the same as before.

### Costs

`make cost-x2 cost-k56 cost-v34 ARM_CC=$A5 COST_TICKS=0` (armv5 uClibc,
each in its own BUILD), M ARM instructions per second of line in data
mode:

| | before | after | change | dsp's estimate |
|---|---|---|---|---|
| x2 answering (second 15 to 16) | 69.94 | 60.95 | -8.99 | -8.2 (ACS) and -1.0 to -1.3 (slicer); P3b's -1.0 to -1.5 not taken |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 35.58 | 35.10 | -0.48 | -0.4 |
| K56flex answering (second 20 to 21) | 35.94 | 35.46 | -0.48 | |
| V.34 answering (second 10 to 11) | 36.92 | 36.43 | -0.48 | |
| V.90 answering (`cost-v34`'s, second 10 to 11) | 35.58 | 35.10 | -0.48 | |

x2's upstream decodes 64 states (8 subsets); the others 16 (4 classes,
whose slicer is unchanged), so they gain from the ACS alone. The v34
bench's decoder (`test_bench count`, armv5) per 2D symbol: 16 states
1837 -> 1688, 32 states 5295 -> 4463, 64 states 11166 -> 8350.

## Task 33: one interpolator in straight runs; the equaliser output unrolled (dsp P5, P10, P14, 3.6; call F6, P3)

- `modem/interp2.h`: `interp2_32` and `interp2_64`, the two neighbouring
  branches of a fractional-delay bank over a ring, newest sample first, in
  the two straight runs that do not wrap (no index masking), the same
  products into each accumulator in the same order as the plain loop. One
  body for both widths. In each run the sample pointer walks down from one
  past the run's newest sample, so that it never points before the ring
  (the old runs in pcmtx and v90arx ended one before it).
- qamrx `produce` (int32 accumulators, as before): `interp2_32`. Its sums
  cannot overflow: the largest row sum |h| of `v34_interp` is 46728 and
  46728 x 32768 = 1531183104 < 2^31 (`test/test_interp_bound.c`, its own
  commit before this one), so they are exact in any order; the order is
  kept anyway.
- pcmtx `pcmtx_out` and v90arx `produce` (int64 accumulators, as before):
  `interp2_64`, the same int products widened into the same int64 sums;
  nothing about them changed but the loop.
- qamrx `eq_out`: four taps a pass (QRX_NTAP = 72, a multiple of 4,
  checked at compile time), the same int64 sums in the same order.
- Four taps a pass, not two: GCC 12.3 for ARMv5 makes a two-tap pass 14
  instructions, the same 7 a tap as one tap a pass (it does not keep the
  post-indexed loads), so P10's and P14's two taps saved nothing. A first
  version with two taps a pass and array indexing measured worse than
  before in the calling modes (V.90 calling +0.47 M, V.92 calling
  +1.10 M at 0 ppm; 8 instructions a tap in its first run). A four-tap
  pass is 24 to 25 instructions (6 a tap), the 0 to 3 taps left of a run
  7 each: the calling audit's P3.
- `v34_rx`'s acs() is now handed `(const uint8_t *)rx->pred`, the whole
  table, rather than `rx->pred[0][0]` walked past its first row (its own
  commit; output identical).

Proof: the goldens cover all three paths. `make verify`: golden 136
identical (test_interp_bound's own is new), replay 636 identical, bench 17
identical; ubsan, armv7hf, bigend, fixed ok; no golden file re-recorded.
The x2 and V.90 bench transmit and received hashes (`cost-x2`) are the
same before and after, and the same on the host and under qemu.

### Costs

`make cost-x2 cost-v90c cost-v92c ARM_CC=$A5 COST_TICKS=0` (armv5 uClibc,
each in its own BUILD; build/cost-before.txt, build/cost-after.txt), M
ARM instructions per second of line in data mode:

| | before | after | change | dsp's / call's estimate |
|---|---|---|---|---|
| x2 answering (second 15 to 16) | 60.95 | 58.45 | -2.50 | -2.3 (P5) and -0.6 (P10) |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 35.10 | 32.60 | -2.50 | -2.3 and -0.6 |
| V.90 calling, 0 / 100 ppm | 55.19 / 59.01 | 54.99 / 58.81 | -0.20 / -0.20 | |
| V.92 calling, 0 / 100 ppm | 70.32 / 74.55 | 70.05 / 74.28 | -0.27 / -0.27 | -0.5 (P14); call P3 measured -0.2 |

The answering modes gain from qamrx (straight runs, 10 -> 6 instructions
a tap, and `eq_out`); the calling modes from v90arx's and pcmtx's runs
(7 -> 6 a tap), which were straight already. P10's -0.6 M assumed 7 ->
6 a tap from two taps a pass, which this compiler does not give; the
four-tap pass gets part of it.

## Task 34: each LMS step forms the next dot product; the estimate's divisions (call P1, P5, P6)

- `modem/v90arx.c`, the calling side's downstream receiver. The forward
  and feedback taps' LMS steps (`FUPD`, `BUPD`) and the channel
  estimate's (`upd_sb`) each also add the new tap times the sample of the
  tap before it (`b[j] += step; next += b[j] dl[j - 1]`, j >= 1; tap 0
  has none). That is the next symbol's dot product but its newest term,
  because the line moves on by one. The next symbol adds `f[0] x[0]` and
  `b[0] dl[0]` (`h[0] sb[m0]`) and skips its own loops. The terms are at
  most 2^31 x 2^15 = 2^46 (int32 taps of any value, int16 samples), at
  most 128 of them: the int64 sum is exact in any order, so it is the
  same as the loops'.
- A sum is used only by the symbol it was formed for: `eqnext_m` (the
  equaliser), `hnext_m0` (the estimate), -1 for none. Each use clears it.
  The paths other than the step itself that change the taps, the lines or
  `sb[]`, and how each one is handled:
  - a hold or a gated symbol: no step, so no sum, and the next symbol
    uses its own loops; for the estimate such a symbol also clears
    `hnext_m0` (fix round 1, below);
  - a design whose taps replace the equaliser's (`dsn_step`'s last step,
    after that symbol's LMS step): it clears `eqnext_m`;
  - `eq_start` (the lines refilled when TRAIN starts): it clears
    `eqnext_m` and `hnext_m0` (the estimate's m0 jumps back by DFF +
    cursor there and climbs to EST's marker again);
  - `ls_set` (TRN1d's interval levels rewrite `sb[]`): it clears
    `hnext_m0`;
  - `v90arx_rebase` (k and m drop by CLK_REBASE) clears both; a retrain
    or a restart (`v90arx_init`) sets both to -1;
  - `produce`, the band edge, the timing hold and the clock change no tap
    or line; the newest sample, decision or symbol (`sb[m0]`) is read
    when the sum is used, not when it is formed.
- `test/test_orig_retrain.c` wipes the receiver's h, f and b from outside
  (a test of the collapse). Its new `wipe()` also clears the two markers:
  a sum formed from the wiped taps belongs to them. Its output is
  unchanged. The rule is written next to the fields in v90arx.h: code that
  writes f, b, h, the lines or sb[] from outside sets the markers to -1.
- The estimate's step `e L 2^24 / (NCH pd)`. NCH = 128 divides e L 2^24,
  so the step is `(e L 2^17) / pd`, the same truncated quotient. Whenever pd
  is L^2 (always with the symbols known, EST and TRAIN; in DD by chance),
  the division is through L^2's reciprocal (`divrcp`, a new `ll_rcp`). Otherwise it is in 32 bits when
  `|e L 2^17| < 2^31` and `pd < 2^31`: C's int32 division truncates as
  int64's does, for negative values too. The rest stays a 64-bit
  division.
- `neq % rc` is `neq & (rc - 1)`: rc is 2048 >> 0..2 and neq is uint32.
- Proof. `test/test_v90arx_paths.c` (its own commit before this one, with
  its own golden file) drives v90arx through the paths above: holds of one
  symbol and of 200, the band edge on and off (a design at once), the
  timing hold, impulses (gate and outlier steps), 70 ppm (a design every
  512 symbols), ls_set. Its output is unchanged. In a throw-away build,
  each fused sum was checked against the plain loops (abort on a
  difference), over `make check` (14 programs use the sums) and V.90 and
  V.92 caller recordings at 0 and 100 ppm. There was no difference once
  test_orig_retrain's wipe cleared the markers: that wipe was the only
  stale sum it found.
- qamrx fusion skipped: it needs new fields in `qamrx_t` (the two partial
  sums and the symbol they are for). `qamrx_t` is inside `answer_t`,
  outside its union, and so in the memory pins. Between `qamrx_update` and
  the next on-time `eq_out`, the T/3 line also moves on by three samples,
  two of them for the early output. The timing nudge can change that
  count. The fusion is not one shift by one sample.

### Costs

`make cost-v90c cost-v92c ARM_CC=$A5 COST_TICKS=0` (armv5 uClibc, own
BUILD; build/cost-before.txt, build/cost-after.txt). The before figures
are Task 33's after (HEAD 347caac; the library has not changed since).
In M ARM instructions per second of line in data mode:

| | before | after | change | call's estimate |
|---|---|---|---|---|
| V.90 calling, 0 / 100 ppm | 54.99 / 58.81 | 47.39 / 51.34 | -7.60 / -7.47 | |
| V.92 calling, 0 / 100 ppm | 70.05 / 74.28 | 62.43 / 66.82 | -7.63 / -7.47 | -4 to -6 (P1), -1 (P5), -0.15 (P6) |

The gain is more than the estimates added up. The plain loops cost 6 or 7
instructions a tap over 304 taps (48 + 128 + 128). On the steps they
replace, each tap costs one multiply-accumulate more, on values already in
registers. Not measured apart: the three changes went in together.
Bench hashes (`bench-v90c`, `bench-v92c`, `bench-v90a`, ARM_CC=$A5) are
the same on the host and under qemu: v90c tx bbf20f60 / rx 64820c91,
v92c tx 484a898d / rx 0e9531e0, v90a cdf21a51. `make verify`: VERIFY
PASS. Golden 137 identical (test_v90arx_paths' own is new), replay 636,
bench 17 identical. ubsan, armv7hf, bigend and fixed ok. No golden file
re-recorded.

### Fix round 1: the estimate's sum held over a skipped step

- Review C1: at EST to TRAIN the estimate's m0 goes back DFF + cursor
  symbols and climbs again. Only an `estimate` call cleared `hnext_m0`, and
  a hold or a gate skips that call. With `ntrain` below EST's last symbol
  and a hold from TRAIN's start to that symbol, decisions overwrote `sb[]`
  in the span, and the stale sum was used. Nothing in v90a does this (it
  leaves ntrain at 2000 and does not hold there), so verify could not see
  it. `v90arx_rebase` had the same flaw in an absurd form.
- Fix: every symbol that makes no estimator step (hold, gate) clears
  `hnext_m0`, so the sum is good for the very next symbol only, as the
  equaliser's already was. `eq_start` (TRAIN's start) and `v90arx_rebase`
  clear both markers as well.
- test_v90arx_paths has a second receiver with ntrain 1800 in that state
  (the line inverted just before TRAIN, held to EST's last symbol). Its
  golden lines were recorded from 347caac's plain dot product. Before the
  fix they differed; after it they are identical.
- Throw-away fused-vs-plain checker: no difference over `make check`, the
  new stage, and a sweep of ntrain 40 .. 2400 (step 40), released at EST's
  last symbol -1, 0 and +1, with and without the inversion (360 runs). The
  same sweep on the pre-fix code failed 46 runs.
- `make verify`: VERIFY PASS (golden 137 identical, replay 636, bench 17).
  Costs after the fix (the same targets, BUILD build/ca34b): V.90 calling
  47.43 / 51.39, V.92 calling 62.48 / 66.86 (0 / 100 ppm). That is +0.05 M
  on the table above, so the changes against Task 33 are -7.56 / -7.42 and
  -7.58 / -7.42.

## Task 36: V.42's per-bit path inline; FCS by table (v42 perf 0, 1)

V.42 runs per line bit on the box's CPU, beside the modem, and is not in
the modem's cost figures. `make cost-v42` (new) counts it: `rig/bench_v42`
runs two endpoints back to back with both directions kept full of text
(30000 line bits to set up the link, then 56000 more each way); the count
is the difference of the two runs, halved: per endpoint, per second at 56
kbit/s both ways, the harness's loop (about 0.7 M) included. The host's
output and the target's (state, octets received, their hash) must agree.

What changed, output identical by construction:

- `v42hdlc_tx_need_frame`, `v42hdlc_tx_bit` and `v42hdlc_rx_bit` are
  `static inline` in `v42_hdlc.h`. Each inline function takes only the
  common case: the next bit of the octet being sent; a received bit with
  fewer than five ones before it. Every other case goes to
  `v42hdlc_tx_bit_full` / `v42hdlc_rx_bit_full`, which are the old
  functions whole, so they give the same answer for any state.
- `hdlc_bit` (v42.c) calls `hdlc_next` (the `orig_wait` countdown and the
  `stopped` / next-frame test, as before, in the same order) only when
  `orig_wait > 0` or the transmitter could take a frame. Otherwise both
  did nothing before.
- `v42ec_rx_bit` tests for the plain-HDLC states (establishing,
  connected, releasing) before the switch on the others.
- `v42_fcs16` and `v42_fcs32` work an octet at a time from two `const`
  tables (256 × uint16 and 256 × uint32, 1.5 KB). `rx_resync`'s bitwise
  steps stay: they work at bit offsets.
- No field was added to `v42ec_t` (`v90m_ec_mem_size()` unchanged).

The net: `v42/test/test_fcs` (its own commit and golden file, before the
change) compares both FCS functions with the bitwise definition, from
every octet and 10^5 random states. The V.42 goldens and replays are
unchanged. `bench_v42`'s output (states, octets received and their hash)
is the same before and after, and the same on the host and under qemu.

### Costs

`make cost-v42 ARM_CC=$A5` (armv5 uClibc, each run in its own BUILD), in M
ARM instructions per endpoint per second at 56 kbit/s both ways:

| | before | after | change |
|---|---|---|---|
| no compression | 7.96 | 5.21 | -2.75 (-35 %) |
| V.42 bis (2048/32) | 11.81 | 9.08 | -2.73 |
| V.44 (2048/255/4096) | 12.19 | 9.48 | -2.71 |

With no compression, the per-bit path alone gives 6.05 (-1.91 M). That
was measured with the old bitwise FCS put back into this code. The FCS
tables give the other -0.84 M. Leave out the harness's ~0.7 M and the
framing falls from ~7.3 M to ~4.5 M, about 38 %. The audit estimated
20-30 % for the per-bit path; it gives 26 %, and the tables add the rest.

The v42 audit's figures, from its scratch bench, were 7.9 / 11.6 / 12.2.

## Task 35: small exact forms (ans P1, P3; dsp P8, P11, P12, P13; call P4 trim, P8)

The proof came first, in its own commit: `test_small_exact` (test/, in
TESTS with its own golden file). It compares the library with reference
copies of 347caac, which live only in the test:
- the octet table, all 256 octets of both laws;
- the modulus encoder, for every Mi in 1..128. A step's dividend is
  checked at 0, 1, Mi 2^16 - 1 and 10^5 random values, and 10^6 random
  maps of mixed Mi. `test_small_exact full` checks every dividend below
  Mi 2^16: 541065216, 0 differ, 2 min 17 s on the host;
- the scramblers n bits at once against n one-bit calls: GPC and GPA,
  both directions, 10^6 random bytes from 100 random states;
- `x2map_frame`, 10^6 random frames in each of the 46 banks, and 10^6
  frames with the bank changed between them;
- `ec_tick`, its return value and whole state;
- the V.92 encoder on padded filters, and on filters changed under it
  without `v92_enc_init`.

35a, tables:
- `v90_octet_linear` reads a `const int16_t [2][256]` and is static
  inline. The table is `v90/v90_pcm_tab.c`, generated by
  `tools/gen_pcm_lin.py` (the build runs no Python). The file is in every
  build that has `v90_pcm.c`: the library, v90/ and v92/ Makefiles,
  `k56-crd-scan` and `k56enc-dump`.
- The one scrambler (`v34/v34.h`) takes up to 8 bits a call as shifts of
  one word. GPA's feedback inside the 8 bits is solved by `o ^= o >> 5`.
  No table is needed for these sparse polynomials.
- `v90_*_bits` run arrays through it. Five per-frame callers use them:
  x2tx, v90_tx, v90a_down (the caller's descrambler), v92_enc and k56tx.
- `x2map_t` keeps its bank's bits and lists. They are taken again when
  x2tx changes index, pad mode or levels A.

35b, arithmetic:
- `x2map_frame` divides in 32 bits once the number fits.
- `v90_modulus_encode` divides through `ceil(2^32 / Mi)` from a const
  table: `(cur rcp) >> 32`, exact for cur < Mi 2^16 and Mi < 256 (the
  proof is in the source).
- `ec_tick` sums eight taps a pass, not four (four already was the code).
  On ARMv5 the pass is 28 instructions for 8 taps, 3.5 a tap, against 16
  for 4. GCC makes no load-multiple of either form.
- The V.92 trim: v90a's start_b1u gives the encoder a private copy of the
  CPd filter, less the zeros at the ends of its sections
  (`v92_filt_trim`, `v90a_t.encf`). The first form cached zero counts in
  `v92_enc_t` and subtracted them from the live lengths. That could read
  out of bounds if the filter changed under a running encoder (a
  security review's finding; ASan showed it in `test_small_exact`), and
  it was replaced; see the commit `v92_enc: the zero-tap trim from a
  private copy`. Our own server's filters have no padding, so the bench
  figures do not show this trim.
- The review's second security finding is older than this task. It is in
  v0.3.0 to v0.5.0, from C6-2 (eff5db5) on. The V.92 caller's upstream
  starts once a CPd acknowledges ours, but its downstream decodes CPds
  until Ed. Each CPd with a good CRC replaced (0.5.0) or was merged into
  (since e71e131) the CPd the running encoder read at every symbol. A CPd
  with Mi = 0 divided by zero (SIGFPE), and one with a constellation index
  of 15 read past the map.
  Fixed in c31f744:
  - the encoder has its own map (`v90a_t.encm`), taken with `encf` in
    start_b1u after `v92_map_check` / `v92_filt_check`;
  - v92aseq refuses a CPd that names a rate and leaves a map or filter
    failing those checks (as a CPd whose CRC fails);
  - `test_v92_hostile` covers both.
  Only programs that enable V.92 when calling were exposed; it is not in
  the default modulations.
- P8 skipped: memory. A stored 64-bit reciprocal of the gain in `v34up_t`
  makes `answer_t` 16 bytes larger on x86-64 (106208 -> 106224) and on
  ARMv5 (105928 -> 105944), and so `v90m_mem_size()`; by D16 it is not
  made.

Memory: `answer_t` 106208 / 105928 and `v90m_mem_size()` 106320 / 106016
(x86-64 / ARMv5) are unchanged. `x2map_t`'s cache is in `x2d_t`, the
smaller member of the answering union, and `encf` is in `orig_t` (54528
/ 54376; with the encoder's map and v92aseq's merge copy of c31f744,
58440 / 58280).

Proof: `make verify` gave VERIFY PASS (790 s). Golden 137 identical,
replay 636 identical, bench 17 identical; ubsan, armv7hf, bigend and
fixed ok. No golden file was re-recorded but `test_small_exact`'s own (a
line added with the fix). The first verify run failed in the bigend
stage only: a link of `api_test_ec_fields` raced a rebuild of
`build/be/arm/libv90modem.a`. A second run passed.

### Costs

`make cost-x2 cost-v90c cost-v92c ARM_CC=$A5 COST_TICKS=0` (armv5 uClibc;
before: a `git archive` of 347caac, after: of 4013dab; build/cost-before.txt,
build/cost-after.txt), M ARM instructions per second of line in data mode:

| | before | after | change | audit's estimate |
|---|---|---|---|---|
| x2 answering (second 15 to 16) | 58.46 | 56.58 | -1.88 | -0.8 (x2map) -0.4 (octets) -0.4 (ec) |
| V.90 answering (`cost-x2`'s) | 32.60 | 31.19 | -1.41 | -1.0 (modulus) -0.3 (octets) -0.4 (ec) |
| V.90 calling, 0 / 100 ppm | 54.99 / 58.81 | 54.52 / 58.37 | -0.47 / -0.44 | |
| V.92 calling, 0 / 100 ppm | 70.05 / 74.28 | 69.69 / 73.94 | -0.36 / -0.34 | -0.5 (descrambler) |

The answering modes gain from the modulus encoder (V.90), x2map (x2), the
octet table and the canceller (both). The calling modes gain from the
byte-wise descrambler, the canceller and (V.92) the upstream scrambler.
Our own server's filters have no padding, so the V.92 trim does not show.
x2 saves 1.88 M against 1.6 M estimated; V.90 saves 1.41 M against 1.7 M,
and V.92 0.36 M against 0.5 M.

The x2 and V.90 bench hashes are the same before and after: tx 37c3e5f5
/ 8e3136e9, rx af8dfaea / 4bebb9be. Under qemu, x2's tx and rx hashes and
V.90's transmit hash and bits received match the host.

## Task 37: x2 with V.42 counted; the two-calls verdict (D18); Phase 3 figures

### x2 data mode with V.42

`make cost-x2-v42` (new) counts x2 data mode as `cost-x2` does (second 15
to 16 of the call, ARM instructions under qemu), with our V.42 endpoint on
the answerer's data bits. `tools/rec_x2` takes a fifth argument, `none`,
`v42bis` or `v44`: both ends then run a `v42ec_t` on their modem's getbit
and putbit (`rig/ec_side.h`), started when that modem reports data mode,
ticked every millisecond and kept full of an English sentence ("the quick
brown fox jumps over the lazy dog. "). `rig/bench_v90 -e none|v42bis|v44`
replays the recording with the same answerer-side code, so its V.42
endpoint is in the count; the client's is not (it is the far modem). The
compression parameters are `v42ec_cfg_default`'s: V.42 bis 2048 codewords,
strings of 32; V.44 2048 / 255 / 4096. `none` is V.42 with neither
offered. Each mode is recorded at 32000 (rate index 1, as `cost-x2`)
and at 56000 (index 15). `cost-x2-v42` checks that the host's and qemu's
tx / rx hashes and V.42 lines (state, octets received, their hash) agree,
and that the host's transmit hash is the recorder's. Every call is
in data mode from 10.73 s, x2 down and 24000 up, and V.42 is connected
(state 2) at both ends to the end of the 40 s. `bench-x2` passes
`X2_ARGS` (`"INDEX [none|v42bis|v44]"`) to `rec_x2`, and `-e` to the
bench when an EC is named. Without it, `bench-x2`, `cost-x2` and the bench
hashes are as before.

`make cost-x2-v42 cost-x2 ARM_CC=$A5` (armv5 uClibc), M ARM instructions
per second of line in data mode (second 15 to 16):

| | 32000 | 56000 |
|---|---|---|
| x2 alone (`cost-x2`, its LCG data; 32000) | 56.58 | |
| x2 with V.42, no compression | 58.89 | 61.50 |
| x2 with V.42 bis (2048/32) | 69.24 | 77.19 |
| x2 with V.44 (2048/255/4096) | 89.25 | 108.78 |

x2 alone is 56.58 M, inside the plan's target of 55-60 M (82.57 M at the
baseline).

What the V.42 rows cost over x2 alone depends on the data's compression.
The sentence repeats every 45 octets, so it compresses far beyond real
traffic. Over the 29.3 s of data mode, the answerer's encoder took in
about these octets per second:
- 32000: 85 k (V.42 bis), 185 k (V.44)
- 56000: 149 k (V.42 bis), 324 k (V.44)

Its decoder gave out 64 k (V.42 bis) and 139 k (V.44) octets per second
from the 24000 upstream. With no compression it is 3.8 k down at 32000,
6.6 k at 56000 and 2.8 k up. So the V.42 bis and V.44 rows are a
throughput ceiling for text, not what a typical PPP stream costs. The
no-compression row is the floor: +2.3 M at 32000, +4.9 M at 56000. That
matches `cost-v42`'s 5.21 M per endpoint for 56 kbit/s both ways.

### The verdict (D18)

two x2 calls with V.42bis per box: no (2 × 77.19 M = 154.37 M of about 122 M)

The worse V.42 bis figure is 56000's. Even with no compression, two x2
calls would need 2 × 61.50 = 123.0 M, beyond the 110 M bound. The docs
keep "one x2 call per box" and give the measured figures.

### Costs after Phase 3

`make cost-x2-v42 cost-x2 cost-k56 cost-v34 cost-v90a cost-v90c cost-v92c
cost-bell cost-v42 ARM_CC=$A5 COST_TICKS=0`, plus `cost-k56
K56_ARGS=heavy` (armv5 uClibc, gcc -O2, at 3ef1c00 with this task's bench
changes; the library is 3ef1c00's; each run in its own BUILD). M ARM
instructions per second of line in data mode:

| | baseline | after Phase 1 | after Phase 2 | after Phase 3 | change from baseline |
|---|---|---|---|---|---|
| x2 answering (second 15 to 16) | 82.57 | 82.85 | 82.79 | 56.58 | -25.99 (-31 %) |
| x2 + V.42, no compression, 32000 / 56000 | | | | 58.89 / 61.50 | new |
| x2 + V.42 bis (2048/32), 32000 / 56000 | | | | 69.24 / 77.19 | new |
| x2 + V.44 (2048/255/4096), 32000 / 56000 | | | | 89.25 / 108.78 | new |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.20 | 39.40 | 39.46 | 31.19 | -8.01 (-20 %) |
| K56flex answering (second 20 to 21) | 39.27 | 39.64 | 39.54 | 32.12 | -7.15 (-18 %) |
| K56flex, the heavy client (Task 19, second 20 to 21) | 84.43 | 84.54 | | 58.60 | -25.83 (-31 %) |
| V.34 answering (second 10 to 11) | 40.53 | 40.71 | 40.51 | 33.42 | -7.11 (-18 %) |
| V.90 downstream receiver, 0 / 100 ppm | 44.62 / 48.97 | 44.57 / 48.92 | | 36.46 / 40.92 | -8.16 / -8.05 |
| V.90 calling, 0 / 100 ppm | 55.11 / 59.31 | 55.06 / 58.89 | 55.19 / 59.01 | 46.97 / 50.95 | -8.14 / -8.36 |
| V.92 calling, 0 / 100 ppm | 69.95 / 74.32 | 70.16 / 74.39 | 70.32 / 74.55 | 62.12 / 66.52 | -7.83 / -7.80 |
| Bell 103 / Bell 212A calling (first second) | 9.86 / 4.43 | 9.87 / 4.69 | | 9.88 / 4.66 | **+0.02 / +0.23** |
| V.42 per endpoint, 56 kbit/s both ways (`cost-v42`): none / V.42 bis / V.44 | 7.96 / 11.81 / 12.19 (Task 36's before) | | | 5.21 / 9.08 / 9.48 | -2.75 / -2.73 / -2.71 |

An empty cell was not measured at that gate. Only the Bell rows went up
from the baseline. Both are Phase 1's: Task 13's v22rx products in 64
bits (+0.23 M for 212A, measured there; above). No Phase 3 task touched
the Bell paths, and 212A is 0.03 M under its Phase 1 figure. Every other
row fell by 7 to 26 M. x2 and the heavy K56flex client fell most: their
64-state non-linear upstream had the most of Tasks 31 and 32 in it. The
figures agree with the last task that touched each row:
- x2 56.58 and V.90 answering 31.19: Task 35.
- V.90 / V.92 calling: Task 35 after Task 34.
- V.42: Task 36, to the instruction.

`make verify` (TMPDIR=build/tmp) on this task's tree: **VERIFY PASS**
(442 s). Golden 140 identical, split ok; replay 636 identical; bench
hashes 17 identical; ubsan, armv7hf and bigend (short-cmp 129 identical)
ok; fixed ok. No golden, replay or bench file re-recorded. `make
verify-phase` (the ports and the costs again) is left to the release
proof (Task 39), as the owner decided.

## rc1 fix wave: hostile audio on the answering side (security pass I1, I2, M1, M3, M4)

From the final security pass (the answering side against a hostile
caller) and the final branch review, before 0.6.0-rc1:
- I1 (5a28091, bd3f8d2): the echo canceller's taps saturate at the int32
  range instead of overflowing, and the NLMS step holds an error past
  2^23 (`err * 2^40` left int64). Training only.
- I2 (bece0d4): the V.34 upstream receiver (`modem/qamrx.c`) has no
  signed overflow on loud or abrupt audio. A first form that clamped the
  error and the equaliser output changed 57 files of the replay proof:
  the recordings' hang-up clicks already overflowed the tap products
  (|pr| up to about 3 * 10^5 against x = 32767). Nothing was re-recorded;
  the committed form computes those products and sums modulo 2^N
  (unsigned, then the wrapping conversion DESIGN.md assumes), so every
  result is the one before, holds the taps to +-2^29 once in 256 to 4096
  symbols, and keeps the mse from going negative.
- M1 (d5fc77c): the V.92 design's noise term saturates.
- M3, M4 (33e4d1d): long and large XID values.

`make verify` at d8e42a3: VERIFY PASS. Golden identical (the new tests'
own check counts only), replay 636 identical, bench 17 identical.

### Costs

`make cost-x2 cost-v90a cost-v34 cost-v90c ARM_CC=$A5 COST_TICKS=0`
(armv5 uClibc; before: a `git archive` of 45ffb75, after: of bece0d4, each
target in its own BUILD; cost-v90a again on d8e42a3, which adds the
canceller's step hold), M ARM instructions per second of line in data
mode:

| | before | after | change |
|---|---|---|---|
| x2 answering (second 15 to 16) | 56.58 | 56.60 | +0.02 |
| V.90 answering (`cost-x2`'s) | 31.19 | 31.20 | +0.02 |
| V.34 answering (second 10 to 11) | 33.42 | 33.44 | +0.02 |
| V.90 downstream receiver, 0 / 100 ppm | 36.46 / 40.92 | 36.45 / 40.91 | -0.01 / -0.01 |
| V.90 downstream receiver, Sd and TRN1d (0.7 s), 0 / 100 ppm | 21.49 / 23.26 | 21.48 / 23.25 (d8e42a3) | -0.01 / -0.01 |
| V.90 calling, 0 / 100 ppm | 46.97 / 50.95 | 46.96 / 50.94 | -0.01 / -0.01 |

The answering modes pay the tap hold's count per update and the mse
guard: about 5 instructions a symbol, 0.02 M. The canceller's
saturation runs in training only, where the calling side's Sd and TRN1d
figure moved by less than 0.01 M. Far under the brief's 0.5 M. The x2 and
V.90 bench hashes are the same before and after: tx 37c3e5f5 / 8e3136e9,
rx af8dfaea / 4bebb9be.

## The release proof (0.6.0)

Run on the audit branch after 0.6.0-rc1 (1a11a08), with the two rc1 CI
fixes below on top: `make verify-phase` (armv5 uClibc and armv7hf musl
compilers, `RECS` the 106 private recordings), `tools/replay-proof.sh
diff v0.5.0`, and `make install` with the example built through
pkg-config.

The rc1 CI fixes (found by the release proof and the GitHub CI):
- a7c26ad `Makefile`: the cross-built libraries are built once before the
  api tests. Each `arm/api_test_*` rule ran a sub-make for the same
  `libv90modem.a`, and under `-j` several raced in a from-scratch armv5
  `make check-arm` (an empty `libv90modem.o`, a failed `check-symbols`).
- 2aed43f `tools/golden.sh`: the `-nt` probe works under dash (the CI's
  host jobs failed on it).
Neither touches library code or any output.

### Result

`VERIFY-PHASE PASS (15230 s)` (4 h 14 min), the stages:

| stage | time | result |
|---|---|---|
| armv5 | 2365 s | ALL ARM CHECKS PASS |
| ports | 12565 s | 15 of 15 targets identical to host gcc (below) |
| armv7hf | 269 s | short-cmp: 137 programs identical (build/a7 against build/short) |
| benches | 30 s | host = qemu hashes: 10 benches |
| crd | 1 s | k56-crd-scan: 106 recordings, 0 CRd detections |
| costs | 6356 s (beside the others) | 17 lines in build/costs.txt |

The stages ran one after another, the costs beside them; the matrix took
12565 s with 2 targets at once. The wall time is the ports' plus the other
stages' (the costs finish first).

`make verify` (TMPDIR=build/tmp, 16 threads) at a7c26ad: `VERIFY PASS
(460 s)`; golden 141 programs identical, split 4 programs, replay 636
identical, bench hashes 17 identical, ubsan, armv7hf and bigend (short-cmp
129 identical) ok, fixed ok (104 objects, 0 float helpers).

### Port matrix, per target

Every target gates; each ran the whole suite (229 programs) and was
compared with host gcc program by program. Whole suites on host gcc,
armv5-uclibc, s390x-glibc, riscv64-musl; QUICK (219 of 229, the 10 slow
programs of `tools/ports-slow.txt` skipped) on the rest.

| target | result |
|---|---|
| host-gcc (the reference), host-clang | host-clang: 219 of 229 identical |
| x86_64-musl, i686-musl | 219 of 229 identical, both |
| aarch64-musl, aarch64be-musl (big-endian) | 219 of 229 identical, both |
| armv7hf-musl | 219 of 229 identical |
| armv5-uclibc (the box's ABI) | 229 of 229 identical (7 lines only with uClibc printf's rounding up, "0.3 0.8 5.84 0.14") |
| s390x-glibc (big-endian) | 229 of 229 identical |
| riscv64-musl | 229 of 229 identical |
| ppc-musl, ppc64-musl (big-endian) | 219 of 229 identical, both |
| mips-musl (big-endian), mipsel-musl | 219 of 229 identical, both |
| m68k-uclibc (big-endian) | 219 of 229 identical (8 lines only with printf's rounding up) |
| win32-mingw (wine) | 219 of 229 identical |

### Memory per target

`v90m_mem_size()` is unchanged since Task 29's table (above; no later task
changes it), and `api_test_api` passed on every target above: it pins
106320 (x86-64) and 106016 (32-bit ARM) and checks `v90m_mem_size() <=
V90M_MEM_SIZE` (110592) on the others. Octets: 106320 on host-gcc,
host-clang, x86_64-musl, aarch64, aarch64be, ppc64, riscv64, s390x;
106016 on armv5, armv7hf, mips, mipsel, ppc, win32-mingw; 105832 i686;
105704 m68k. Before Task 29 it was 118824 / 118512 / 118296 / 118188.
`V90M_EC_MEM_SIZE` 51200 (`v90m_ec_mem_size()` 49792 on x86-64),
`V90M_ASYNC_MEM_SIZE` 2048 (1064).

### CPU per mode, before and after

M ARM instructions per second of line in data mode, armv5 uClibc, gcc -O2,
from `build/costs.txt` of this run (`COST_TICKS=0`), against the baseline
(v0.5.0 code, above):

| | baseline (v0.5.0) | 0.6.0 | change |
|---|---|---|---|
| x2 answering (`cost-x2`, second 15 to 16) | 82.57 | 56.60 | -25.97 (-31 %) |
| V.90 answering (`cost-x2`'s, second 15 to 16) | 39.20 | 31.20 | -8.00 (-20 %) |
| K56flex answering (second 20 to 21) | 39.27 | 32.14 | -7.13 (-18 %) |
| V.34 answering (second 10 to 11) | 40.53 | 33.44 | -7.09 (-17 %) |
| V.90 downstream receiver, 0 / 100 ppm | 44.62 / 48.97 | 36.45 / 40.91 | -8.17 / -8.06 |
| V.90 calling, 0 / 100 ppm | 55.11 / 59.31 | 46.96 / 50.94 | -8.15 / -8.37 |
| V.92 calling, 0 / 100 ppm | 69.95 / 74.32 | 62.11 / 66.52 | -7.84 / -7.80 |
| Bell 103 / Bell 212A calling (first second) | 9.86 / 4.43 | 9.88 / 4.66 | +0.02 / +0.23 |
| x2 + V.42, no compression, 32000 / 56000 | | 58.89 / 61.50 | new |
| x2 + V.42 bis (2048/32), 32000 / 56000 | | 69.24 / 77.19 | new |
| x2 + V.44 (2048/255/4096), 32000 / 56000 | | 89.25 / 108.78 | new |
| K56flex, the heavy client | 84.43 | 58.60 | -25.83 (-31 %) |
| V.42 per endpoint, 56 kbit/s both ways: none / V.42 bis / V.44 | 7.96 / 11.81 / 12.19 | 5.21 / 9.08 / 9.48 | -2.75 / -2.73 / -2.71 |

The x2 + V.42 rows and the heavy K56flex client are the figures measured
at b490ccd (Task 37); the only later code change, the rc1 fix wave, moved
x2 by +0.02 M, and since then the V.42 path has changed only in XID
parsing. `make cost-v42` was run again at the release proof and gave the
same 5.21 / 9.08 / 9.48 M (hashes f0475237, 490794a1, 7f358e0c). The other
rows are this run's `build/costs.txt`. Bell 212A is the one row above the
baseline, Phase 1's (Task 13, above). One x2 call per box (D18).

### Time

`make verify`: 460 s at a7c26ad (6.3 min was the plan's figure).
`make verify-phase`: 15230 s, the configuration of the release proof
(ports 12565 s, armv5 2365 s, armv7hf 269 s, benches 30 s, crd 1 s; the
costs, 6356 s, beside them). The first measurement, at Task 4, was 22468 s.

### Replay differences against v0.5.0

`tools/replay-proof.sh diff v0.5.0`: `636 runs, 102 differ from v0.5.0`;
102 traces (.txt) and 3 bit files (pd_36000 in the V.92 sets), no exit
code. The recordings and flag sets are the Phase 1 gate's, group for
group (table "Replay proof against v0.5.0", above): 21 (Tasks 7, 10b,
10c: end codes, retraining and renegotiation counts, +288 upstream bits
at the S), 42 (Task 20: the x2 click blanking), 39 (T21.2: SUVd between
repeated CPds). Each is a CHANGELOG entry of 0.6.0. Nothing else differs. The 636 replays of the current tree against the
manifest are identical (`make verify`).

### Install

`make install PREFIX=build/inst`, then `examples/call_loop.c` built with
`cc -std=c99 -Wall -Wextra -Werror $(pkg-config --cflags --libs --static
libv90modem)` and run: exit 0 (caller and answerer end on a clear-down,
line time 12.93 s).
