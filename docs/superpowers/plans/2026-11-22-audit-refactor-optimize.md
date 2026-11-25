# libv90modem 0.6.0 audit, refactor and CPU work: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the five audits of 0.5.0 into 0.6.0: a safety net first, then
the bugs fixed test-first, the duplicated code merged with byte-identical
output, the hot paths made cheaper with bit-identical output, and the
release prepared (not tagged).

**Architecture:** Phase 0 builds a per-commit proof (`make verify`: golden
outputs of every test program, the 106-call replay manifest, bench
transmit/received-bit hashes, UBSan, a short armv7hf suite compared with
the host, a no-float/no-alloc scan) and a per-phase proof (`make
verify-phase`: ARMv5, port matrix, cost counts). Phase 1 changes behaviour
one stated fix at a time. Phases 2 and 3 must pass `make verify` with no
golden file re-recorded. Phase 4 updates the docs and figures.

**Tech Stack:** C99 fixed point, GNU make, POSIX sh, qemu-user, Bootlin
cross toolchains in `/home/georgica/toolchains`, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-22-audit-refactor-optimize-design.md`
(decisions C1-C8, D1-D20; findings ranked in its §5). Evidence:
`build/audit/{answering,calling,dsp,v42-api,legacy-tests}.md` (cited
ans / call / dsp / v42 / leg with their finding ids). Read the spec and
the report sections a task cites before starting it.

## Global Constraints

- Worktree `/home/georgica/audit-wt`, branch `audit`. Never touch `/home/georgica/v90d` or any other worktree. No push, tag or merge.
- Commit with `git -c user.name='George Partene' -c user.email='partene.george@gmail.com' commit -F build/msg.txt`; the message's last line is exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- C99; fixed point in the signal path; no allocation after create; no global mutable state; fixed-width types; no signed overflow; no left shift of a negative value; no byte-order assumption; identical output on every target (x86, ARM, big-endian, riscv64, Windows; gcc and clang).
- A compiler builtin only behind a C99 fallback with identical results, an attribute only through `modem/compiler.h` (`V90M_NOINLINE`, `V90M_CLZ32`); `-DV90M_NO_BUILTINS` forces every fallback and is tested.
- GPL-2.0-only. Nothing copied from D-Modem or courier-emu as code or prose.
- Every wait bounded; every end of a call has a defined end code.
- `v90m_mem_size()` stays 118824 (x86-64) and 118512 (ARMv5), `V90M_MEM_SIZE` 120 KiB, `V90M_EC_MEM_SIZE` 50 KiB (`v90m_ec_mem_size()` 49792 on x86-64), `V90M_ASYNC_MEM_SIZE` 2 KiB (1064), in every task except Task 29. `test/api/test_api.c:186-192` enforces the first. (Superseded at Task 29: since then `v90m_mem_size()` is 106320 (x86-64) and 106016 (ARMv5), `V90M_MEM_SIZE` 108 KiB (110592); `test/api/test_api.c` pins the new values, and the sdd global-constraints.md carries them. Noted at the rc1 fix wave, final review M7.)
- New `.c` files under `modem/`, `v90/`, `v34/`, `v92/` go into `MODEM_SRC` in the Makefile (an explicit list); `CORE_SRC` is a wildcard.
- Phase 1: each task's test is seen failing, with the stated failure, before the fix. Each behaviour change gets a line in `CHANGELOG.md` under `## 0.6.0 (unreleased)`. A golden file may be re-recorded only when the task's change explains it, and the commit body lists each re-recorded file and why (D20).
- Phases 2 and 3: `make verify` passes with **no** golden file re-recorded, except a new program's own golden file (a new test is not a re-record) and Task 29's memory pins.
- Paths: `RECS=/home/georgica/sync-build/live-050` (106 private recordings; never copied into the repository). `A5=/home/georgica/toolchains/armv5-eabi--uclibc--stable-2023.08-1/bin/arm-linux-gcc` (the box's ABI). `A7=/home/georgica/toolchains/armv7-eabihf--musl--stable-2023.08-1/bin/arm-linux-gcc`. 16 threads.
- CPU references (ARMv5 counts, dsp "How it was measured"): box ≈ 122 M instructions/s; x2 data 82.46 M; V.90 answering 39.04 M; V.92 calling 74.00 M (100 ppm). Phase 3 tasks record before/after `make cost-*` figures (with `ARM_CC=$A5`) in `docs/history/audit-0.6.md`.
- The docs keep "one x2 call per box" unless Task 37 measures D18's condition.

## Review Focus

1. **Block size.** A host may call `v90m_process_*` with any n. The same call run with n = 1, 160 and 4000 must give identical transmit samples and the same event sequence, before and after the callback guard and the answer.c split. Tested in Task 14, re-run as proof in Task 26.
2. **A slow but legitimate Phase 2.** A peer that repeats INFO0 three times and then proceeds must still connect under the new 20 s exchange bound. Tested in Task 8.
3. **A K56flex client whose B1 is really bad.** The B1 retry under hypothesis B must not turn a bad B1 into a pass: `b1_bad` still ends `K56D_ERR_B1`, with and without `heavy_up`. Tested in Task 19.
4. **A weak but working x2 line.** Clamps or the watchdog of Task 20 must not end a call that works today: the lowest-SNR x2 case of `test_answer_x2` that connects runs 60 s of data with no end. Tested in Task 20.
5. **A-law.** Most tests run mu-law. Every table or shift that replaces a law-dependent computation is checked exhaustively for both laws (Task 35), and Task 15's level bound is checked on an A-law call.

---

## Phase 0: safety net

### Task 1: Test archive, one parallel runner, test data paths

**Files:**
- Modify: `Makefile` (test rules at `:153-158`, `arm_` rule, `test`, `check`, `check-arm`, `check-ubsan`), `v34/Makefile`, `v90/Makefile`, `v92/Makefile`, `v42/Makefile`, `v32/Makefile`
- Modify: `tools/par-run.sh` (entries `path[:arg]`; longest first)
- Create: `tools/test-times.txt`
- Modify: `test/test.h` (`TEST_FORCE_SHORT`), `test/test_v92real.c:27-30`, `test/test_realcheck.c:38`, `v34/test/test_bench.c:95`

**Interfaces:**
- Produces: `$(BUILD)/libtest.a`, `$(BUILD)/arm/libtest.a` (T_SRC objects, same `T_CFLAGS`); `make check PAR=N` and `make check-arm PAR=N` run every program of every directory in one pool; `make -s -C <sub> print-tests BUILD=...` prints that directory's program paths one per line; `-DTEST_FORCE_SHORT` gives the ARM short set on any target; `-DTEST_DATA_DIR="\"…/test/data\""` (default `$(CURDIR)/test/data`).

- [ ] **Step 1: Capture the baseline outputs.** Before editing: `make -s -j16 BUILD=build/base all build/base/v90m-replay && make -s BUILD=build/base check > build/base-check.txt 2>&1; tail -1 build/base-check.txt` → `ALL HOST CHECKS PASS`. Then run each built program of `build/base` and every sub-dir `build/` alone into `build/base/out/<dir>/<prog>.txt`.
- [ ] **Step 2: Failing test (leg F7).** `make BUILD=build/nodata EXTRA_CPPFLAGS='-DTEST_DATA_DIR="\"/nonexistent\""' build/nodata/test_v92real; build/nodata/test_v92real; echo rc=$?`. Today the define is ignored and the cwd decides; expected after the change: `rc=1` with a `FAIL` line naming the missing file. Run it now: expected `skipped` and `rc=0`.
- [ ] **Step 3: Implement.** Archive per directory; tests link `test/test_X.c $(BUILD)/libtest.a $(LDLIBS)` (keep `test_x2_pad`'s and `test_x2map`'s extra objects); `print-tests` in each sub-Makefile; `check` runs all host programs through `tools/par-run.sh` (runner empty) when `PAR` is set, serially otherwise; `check-arm` puts the top level and all sub-dirs into one `par-run.sh` call; `par-run.sh` orders by `tools/test-times.txt` (seconds per program, longest first; unknown programs first) and rewrites it after a run; output still printed in the given order. `test_bench.c:95`: print `t_rx` instead of `CHECK`. `TEST_DATA_DIR` used by both data tests; a missing file is a `CHECK` failure.
- [ ] **Step 4: Identity.** `make -j16 check PAR=16` → `ALL HOST CHECKS PASS`; each program's log equals `build/base/out/…` after removing lines matching `tools/check-ports.sh`'s `TIMING` regex (`:44`). Step 2's command → `rc=1`. Record `time make -j16 check PAR=16` (leg expects about 1 min) in the commit body.
- [ ] **Step 5: Commit** `test: build the sources once into an archive; one parallel runner for all suites`.

### Task 2: Golden outputs, bench hashes, V.42 golden, fixture traces

**Files:**
- Create: `tools/out-filter.sh` (the `TIMING` regex of `tools/check-ports.sh:44`, which now sources it), `tools/golden.sh`, `tools/bench-hashes.sh`, `test/golden/out/**`, `test/golden/bench.txt`, `test/fixtures/golden/*.txt`, `v42/test/test_v42_golden.c`
- Modify: `tools/check-ports.sh`, `test/fixtures/check.sh`, `rig/bench_v90.c`, `rig/bench_v34.c`, `rig/bench_v90c.c`, `rig/bench_bell.c`, `v42/Makefile` (TESTS), `Makefile` (`golden`, `golden-record`, `bench-hashes`)

**Interfaces:**
- Produces: `tools/golden.sh record|check BUILD` (filtered output of every program vs `test/golden/out/<dir>/<prog>.txt`; prints `golden: N programs identical`, exit 1 with the first 10 differing lines per program otherwise); `tools/bench-hashes.sh record|check` (prints `bench hashes: N identical`); every bench prints `tx hash XXXXXXXX` and `rx hash XXXXXXXX` (rx: the same hash function over the received data bits); `test/fixtures/check.sh` compares the full trace with `test/fixtures/golden/<name>.txt` and still checks the `modulation` line.

- [ ] **Step 1: Failing check.** `tools/golden.sh check build` → `golden: no test/golden/out` and exit 1 (file does not exist yet: the script is written first with `check` only).
- [ ] **Step 2: V.42 golden program.** `test_v42_golden` prints one FNV-1a-32 hash line per case: V.42bis encoder output for inputs {8192 octets of an LCG (seed 1), 8192 octets of the sentence `"the quick brown fox jumps over the lazy dog. "` repeated, 8192 zero octets} × P1 {512, 2048} × P2 {6, 32, 250}; V.44 encoder output for the same inputs with the default config and with P1/P2/P3 2048/255/4096; and a back-to-back `v42ec_t` pair (default cfg, V.42bis), 56 bits per ms for 2000 ms, both writing the sentence text, hashing each direction's line bits. It also `CHECK`s that each decoder returns the input.
- [ ] **Step 3: Implement** `record`, the rx hashes in the four benches, the bench script (rec_v90 `-55 0.2 40 0`, `rec_x2 40 0`, `rec_k56 40 0`, `rec_v34 16`, `rec_v90a 100 48000`, `rec_v90c 100 5` and `… v92`, `rec_bell 103 3` and `212 3`, all on the host as the Makefile's bench targets do), fixture traces, fixtures replayed in parallel.
- [ ] **Step 4: Record and prove.** `tools/golden.sh record build && tools/bench-hashes.sh record && test/fixtures/check.sh --record build/v90m-replay`. Then `tools/golden.sh check build` → `golden: N programs identical`; append a line to one golden file → check fails naming that program; restore. `du -sh test/golden test/fixtures/golden` → under 1 MB together.
- [ ] **Step 5: Commit** `test: golden outputs of every program, bench tx/rx hashes, V.42 golden, full fixture traces`.

### Task 3: Replay proof over the 106 recordings

**Files:**
- Create: `tools/replay-proof.sh`, `test/golden/replay.sha256`, `test/golden/recordings.sha256`
- Modify: `tools/v90m_replay.c:167` (`-b FILE`), `tools/README.md`, `Makefile` (`replay-proof`)

**Interfaces:**
- Produces: `tools/replay-proof.sh check|record|diff REF` (D1, spec §7.3). Flag-set directories: `base` `""`, `v92` `-9`, `x2` `-x`, `x2-v92` `-x -9`, `k56` `-k`, `k56-v92` `-k -9`; outputs `build/replay/<set>/<rec>.{txt,bits,rc}`; manifest lines `<sha256>  <set>/<rec>.<ext>`, sorted. `V90M_NO_RECS=1` → `replay proof: skipped (V90M_NO_RECS)`, exit 0. `v90m-replay -b FILE` writes the bits there (default `bits.txt`, unchanged).

- [ ] **Step 1: Failing check.** `tools/replay-proof.sh check` → `replay proof: no test/golden/replay.sha256`, exit 1.
- [ ] **Step 2: Implement.** Recordings checked first against `test/golden/recordings.sha256` (the `.raw` lines of `/home/georgica/sync-build/live-050.sha256`); runs `xargs -P$(nproc)`; `diff REF` builds `v90m-replay` from `git archive REF | tar -x -C build/replay-ref/REF` and prints, for each file whose hash differs, `diff` of the two full traces (first 20 lines).
- [ ] **Step 3: Record and prove against 0.5.0.** `tools/replay-proof.sh record`; `tools/replay-proof.sh diff v0.5.0` → `replay proof: 636 runs, 0 differ from v0.5.0`; `time tools/replay-proof.sh check` → `replay proof: 636 runs identical` in under 15 s (leg: 5 s for 424). Alter one manifest line → check fails naming it; restore.
- [ ] **Step 4: Commit** `tools: scripted replay proof of the 106 recordings, six flag sets, sha256 manifest`.

### Task 4: Split the slow programs; check-fixed; verify and verify-phase; baseline figures

**Files:**
- Modify: `test/test_p34.c:202`, `test/test_orig_v92.c`, `test/test_orig_seeds.c`, `test/test_orig_bell.c` (case argument), `Makefile` (`SPLIT_TESTS`, `check-fixed`, `verify`, `verify-phase`, `check-ubsan` adds `test-fixtures` and `-j`), `tools/ports-slow.txt`
- Create: `tools/verify.sh`, `tools/verify-phase.sh`, `tools/short-cmp.sh`, `docs/history/audit-0.6.md`

**Interfaces:**
- Produces: `prog --cases` prints the case count; `prog N` runs case N alone; no argument runs all with unchanged output (pattern: `test/test_v90arx_robust.c:189`). `SPLIT_TESTS := test_p34 test_orig_v92 test_orig_seeds test_orig_bell`: the ARM runner queues `arm_prog:N` for each case. `make check-fixed` (`FIXED_CC`, default the armv5 Bootlin compiler `$(CURDIR)/../toolchains/armv5-eabi--uclibc--*/bin/arm-linux-gcc`): `$(FIXED_CC) -c` every `CORE_SRC` file (`-j16`), `$(NM)` of each object: no undefined soft-float helper (`__aeabi_f*`, `__aeabi_d*`, `__aeabi_[fd]*`, `__*sf*`, `__*df*`), no `malloc|calloc|realloc|free` outside `api/v90modem.o`; prints `fixed: N objects, 0 float helpers, allocation only in api/v90modem.o`. `tools/short-cmp.sh HOSTBUILD TARGETBUILD` compares filtered outputs program by program. `make verify` and `make verify-phase` as spec §7.4, last lines `VERIFY PASS (S s)` / `VERIFY-PHASE PASS`.

- [ ] **Step 1: Failing test.** `build/test_p34 --cases` → today runs the whole program (no count). Add to `test/test_p34.c` etc. and to a new `tools/split-check.sh`: for each split program, concatenating `prog 1..N` equals `prog` (filtered). Run before the change: fails.
- [ ] **Step 2: Implement** the case arguments, the queueing, `check-fixed`, `short-cmp.sh` (host build `BUILD=build/short EXTRA_CPPFLAGS=-DTEST_FORCE_SHORT`), `verify.sh` (stages of spec §7.4 in parallel, `RECS` default above, one status line each), `verify-phase.sh` (spec §7.4; m68k report-only; costs to `build/costs.txt`).
- [ ] **Step 3: Run.** `tools/split-check.sh` → `split: 4 programs, cases concatenate to the whole`. `make verify` → `VERIFY PASS`. If the measured time exceeds 300 s, move the slowest armv7hf programs (from `build/a7` logs) into `tools/ports-slow.txt`, per phase only, until under (D13); record the final time.
- [ ] **Step 4: Baseline.** `make verify-phase RECS=$RECS` → `VERIFY-PHASE PASS`. Create `docs/history/audit-0.6.md` with: verify and verify-phase wall times; a "Baseline (v0.5.0 code)" cost table from `build/costs.txt` (x2, V.90, K56, V.34, V.90a, V.90c 0/100 ppm, V.92c 0/100 ppm, Bell 103/212A); compare with dsp (x2 82.46, V.90 39.04, V.92c 74.00) and README (81.9/38.9/69.4/73.9).
- [ ] **Step 5: Commit** `test: split the four slowest programs by case; check-fixed; make verify and verify-phase; baseline costs`.

### Task 5: CI

**Files:**
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `make check PAR=`, `tools/golden.sh`, `tools/bench-hashes.sh`, `tools/short-cmp.sh`, `V90M_NO_RECS=1`.

- [ ] **Step 1: Write the jobs** (spec §7.5): `host` (gcc, clang): `make -j"$(nproc)" CC=… check PAR="$(nproc)"`, `tools/golden.sh check build`, `tools/bench-hashes.sh check`, the install/pkg-config step kept; `ubsan`: `make -j"$(nproc)" check-ubsan PAR="$(nproc)"`; `armv7hf`: Debian `gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-user-static`, `make -j check-arm ARM_CC=arm-linux-gnueabihf-gcc PAR="$(nproc)" BUILD=build/a7` then `tools/short-cmp.sh build/short build/a7`; `s390x`: Debian `gcc-s390x-linux-gnu libc6-dev-s390x-cross`, same with `qemu-s390x-static` and `EXTRA_CPPFLAGS=-DTEST_FORCE_SHORT`; `win32` unchanged; `armv5` (today's job) only on `schedule: cron '0 2 * * *'`, `workflow_dispatch` and `push: tags: ['v*']`. All `V90M_NO_RECS=1`.
- [ ] **Step 2: Check locally.** `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"` → no output; `actionlint` if installed → no findings; run the s390x job's commands with the Bootlin `s390x-linux-gcc` → `short: N programs identical`.
- [ ] **Step 3: Phase 0 gate.** `make verify` → `VERIFY PASS`.
- [ ] **Step 4: Commit** `ci: parallel host jobs, armv7hf and s390x output comparison per push, ARMv5 nightly`.

---

## Phase 1: fixes (behaviour changes, test-first)

### Task 6: V.92 digital side, read-only audit

**Files:**
- Modify: `docs/history/audit-0.6.md` (section "V.92 digital side")

- [ ] **Step 1: Read in full** `v92/v92_p34d.{c,h}`, `v92/v92_dec.{c,h}`, `v92/v92_design.{c,h}`, `v92/v92_info.{c,h}`, their tests, and their callers in `modem/answer.c` (V.92 phases). Judge against `build/audit/BRIEF.md`'s rules: bounded waits, defined end codes, signed overflow, shifts of negatives, divisions and indexes from received values (INFO1d, CPu/CP fields, DIL/Ri), hostile far end, test gaps.
- [ ] **Step 2: Check claims** in a scratch copy (`git archive HEAD | tar -x -C build/scratch-v92d`): UBSan runs of `test_answer_v92`, `test_v92real`, `test_p2_v92` and any reproducer written for a finding.
- [ ] **Step 3: Write** the section in the reports' format (summary, findings by kind with file:line, severity, confidence, reproduction). Each Important finding gets an id `T21.k` and a one-line test that would fail today; Task 21 fixes them.
- [ ] **Step 4: Commit** `docs: V.92 digital-side audit`.

### Task 7: Answering end reasons and state flags (ans B1, B2, B3, B4; D3, D4)

**Files:**
- Modify: `modem/answer.h:161-186` (remove `cleared`, `x2_failed`, `k56_failed`, `k56_lost`; add `int end`), `modem/answer.c` (`fail()` at `:94`; every failure site `:228,346,388,446-448,473-477,507,512-515,543,558-561,568,579,598-601,607,639`), `api/v90modem.c:214-237` (`end_of`), `:244-265` (`after`: `retraining`, renegotiation count `:260`), `:354-358` (echo_delay)
- Modify: `include/v90modem.h:278-296`, `docs/api.md:160-174`, `docs/answering.md`, `CHANGELOG.md`
- Test: `test/api/test_api.c` (`test_end_codes`)

**Interfaces:**
- Produces: `enum { ANS_END_NONE, ANS_END_FAILED, ANS_END_LOST, ANS_END_CLEARDOWN_FAR, ANS_END_CLEARDOWN_OURS, ANS_END_NO_COMMON, ANS_END_RETRAIN_FAILED, ANS_END_X2_FAILED, ANS_END_K56_FAILED };` in `answer.h`; `static void fail(answer_t *a, int why)` (sets `end`, `phase = ANS_FAILED`, `connected = 0`, `t = 0`); `static int lost_or(const answer_t *a, int why)` (`connected ? ANS_END_LOST : why`); `int answer_end(const answer_t *a)`. `end_of()` maps `ANS_END_*` to `V90M_END_*` by table. Later tasks call `fail(a, ANS_END_…)` for every new bound.

- [ ] **Step 1: Write the failing test** (from `build/scratch-answering/lost.c`): back-to-back library caller and answerer through the API; 16000 samples after the answerer's `moving`, its input is zeros.

```c
/* for mods in {V22, V34, V90|V34, V21, V32BIS, V92|V90|V34} and x2 (test_x2's sim client) */
CHECK_EQ_I(st.end, V90M_END_LOST);
/* answerer fed zeros from sample 0 */          CHECK_EQ_I(st.end, V90M_END_FAILED);
/* V.90: v90m_retrain(a) each time moving returns, until the end */
CHECK_EQ_I(st.end, V90M_END_RETRAIN_FAILED);
/* both run V.8, JM has nothing in common: the modulation pair of the calling side's own
   V90M_END_NO_COMMON test */                  CHECK_EQ_I(st_a.end, V90M_END_NO_COMMON);
/* x2 client reneg_ms 3000: while data is stopped */ CHECK_EQ_I(st.retraining, 0);
/* ... after it */                              CHECK_EQ_I(stats.renegotiations, 1);
/* V.92 call in data; poke a->phase = ANS_P3, a->p34.ecd.delay = 123, a->u.v92.ecd.delay = 77 */
CHECK_EQ_I(stats.echo_delay, 123);
```

- [ ] **Step 2: Run to see it fail.** `make -j16 build/api_test_api && build/api_test_api` → `FAIL … st.end == 1, want 2` for each modulation (lost.c measured `end=1` for v22, v34, v90, v21, v32).
- [ ] **Step 3: Implement** D3 and D4: losses after connect `lost_or(a, …)`; retrain exhaustion (`:228`, `:507`, `:543`) `ANS_END_RETRAIN_FAILED`; V.8 nothing in common `ANS_END_NO_COMMON`; `retraining` 0 in `V34A_RENEG`, `X2D_RENEG`, `K56D_RENEG`, 1 in retrains and `ANS_K56RT`; renegotiations counted for V.34, x2, K56; echo_delay chosen by phase first, modulation only in `ANS_FAILED`/`ANS_RETRAIN`. Header and docs: `RETRAIN_FAILED` and `NO_COMMON` no longer "calling only". `V8A_RES_NO_CJ` behaviour unchanged.
- [ ] **Step 4: Verify.** `build/api_test_api` → `0 failures`. `make verify`: re-record only `test/golden/out/api_test_api.txt` (and any program printing an end code, listed); `tools/replay-proof.sh check` must pass unchanged, or every differing run is shown by `diff v0.5.0` to differ only in end-code lines (list them).
- [ ] **Step 5: Commit** `answer: one end reason set where the call fails; LOST after a connection, RETRAIN_FAILED, NO_COMMON; retraining flag in renegotiations`.

### Task 8: Answering Phase 2 bounds (ans R-1; D5)

**Files:**
- Modify: `modem/p2d.{c,h}` (`:172-183`, `:212-219`), `modem/p2ans.{c,h}` (`:360-368`), `modem/answer.{c,h}` (phase backstop), `sim/p2a.{c,h}`, `sim/p2c.{c,h}`, `CHANGELOG.md`
- Test: `test/test_p2.c`, `test/test_p2ans.c`, `test/test_answer.c` (backstop)

**Interfaces:**
- Produces: `#define P2D_EXCHANGE_MS 20000`, `#define P2D_MAX_RECOVER 4`, `P2D_ERR_TIME`; `#define P2N_EXCHANGE_MS 20000`, `P2N_ERR_TIME`; `p2d_t.t_x`, `p2d_t.n_recover`, `p2ans_t.t_x` (exchange clocks: reset at init and on a counted restart only); `answer_t.t_phase` (saturating, reset on every `phase` change, not by sub-states) with budgets `ANS_P2`, `ANS_RETRAIN`, V.34 Phase 2: 30000 ms; `ANS_P3`, `ANS_P3X`: 60000 ms. Failure → `fail(a, ANS_END_RETRAIN_FAILED)` when the phase belongs to a retrain of a connected call (`ANS_RETRAIN`, or a V.34/x2/K56 retrain's Phase 2), else `fail(a, ANS_END_FAILED)`. Sim: `p2a_cfg.info0_repeat`, `p2c_cfg.info0_repeat` (0 none, N: repeat INFO0 N times before proceeding, -1 forever); `p2a_cfg.no_rev2` (never sends the second reversal; restarts from INFO0 2500 ms later).

- [ ] **Step 1: Write the failing tests.**

```c
/* p2d vs p2a info0_repeat = -1 */  CHECK_EQ_I(p.state, P2D_FAILED); CHECK_EQ_I(p.error, P2D_ERR_TIME);
                                     CHECK(t_fail <= MS(20000) + MS(100));
/* p2d vs p2a no_rev2 = 1 */         CHECK_EQ_I(p.state, P2D_FAILED); CHECK(p.n_recover <= P2D_MAX_RECOVER);
                                     CHECK(t_fail <= MS(20000) + MS(100));
/* p2ans vs p2c info0_repeat = -1 */ CHECK_EQ_I(p.state, P2N_FAILED); CHECK_EQ_I(p.error, P2N_ERR_TIME);
/* Review Focus 2: info0_repeat = 3, both pairs */ CHECK_EQ_I(p.state, P2D_DONE); /* P2N_DONE */
/* answer_t in ANS_P3, t_phase = MS(60000) - 1, two ticks */ CHECK_EQ_I(a.phase, ANS_FAILED); CHECK_EQ_I(answer_end(&a), ANS_END_FAILED);
```

- [ ] **Step 2: Run to see it fail.** Each forever case runs 60 s of line without reaching FAILED (`FAIL … p.state == …, want …`).
- [ ] **Step 3: Implement** per Interfaces; the limits are checked in the existing per-tick code; nothing else moves.
- [ ] **Step 4: Verify.** Tests pass; `make verify` with only the new tests' golden lines added; replay manifest unchanged (no recording's Phase 2 lasts 20 s).
- [ ] **Step 5: Commit** `p2d, p2ans, answer: bound Phase 2 (20 s exchange, 4 recoveries) and every start-up phase`.

### Task 9: Calling: a retrain's Phase 2 bounded; tone B during our cleardown (call B2, R3; D5)

**Files:**
- Modify: `modem/p2call.c:389-394`, `:689-706` (INFO0 repeats counted into `restarts`), `modem/v90a.{c,h}:1226-1231`, `:1282`, `:1334-1336`, `modem/v34orig.{c,h}`, `CHANGELOG.md`
- Test: `test/test_p2v90.c` (or the file pairing `p2call` with `p2d`), `test/test_p2call.c`, `test/test_v90a.c`

**Interfaces:**
- Produces: `#define V90A_RT_P2_MS 30000` (a retrain's Phase 2 while `connected`; expiry → `V90A_ERR_RETRAINS`, API `V90M_END_RETRAIN_FAILED`); the same 30 s limit in `v34orig` with its retrain-failed error; `rt_due` ignored while `clearing`, the cleardown then ends `V90A_ERR_HANGUP`.

- [ ] **Step 1: Write the failing tests.**

```c
/* p2call (V.90, retrain) vs p2d that the test re-inits each time it leaves P2D_INFO0 */
CHECK_EQ_I(c.state, P2L_FAILED);           /* within MAX_RESTARTS * 8 s + 1 s */
/* p2call (V.34) vs p2ans re-initialised each time it leaves P2N_INFO0 */
CHECK_EQ_I(c.state, P2L_FAILED);
/* v90a in V90A_P2 after a retrain, connected, its retrain clock at MS(30000) - 1, two ticks */
CHECK_EQ_I(v.err, V90A_ERR_RETRAINS);
/* test_v90a's link in data: v90a_cleardown(); the answerer begins a retrain on the next sample */
CHECK_EQ_I(v.err, V90A_ERR_HANGUP); CHECK(!entered_p2); CHECK(t_end <= MS(2000));
```

- [ ] **Step 2: Run to see it fail** (the repeat cases still in Phase 2 after 120 s; the cleardown case enters Phase 2).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** `make verify`; re-record only the new tests' lines; bench-v90c/v92c hashes unchanged.
- [ ] **Step 5: Commit** `p2call, v90a, v34orig: bound a retrain's Phase 2; our cleardown is not turned into a retrain`.

### Task 10: Calling: no data frame lost at Rd; `clock_q8` overflow (call B4, B6)

**Files:**
- Modify: `modem/v90a.c:672`, `:686-704`; `modem/p2call.{c,h}:643-658`; `CHANGELOG.md`
- Test: `test/test_orig_retrain.c` (the far end's rate renegotiation case), `test/test_p2call.c`

**Interfaces:**
- Produces: `int32_t p2call_clock_q8(const proberx_t *r)` (today's `static clock_q8`, exported for the test; saturates when `|num| > den * 20` before the multiply by 13581342).

- [ ] **Step 1: Write the failing tests.** In the far-end-renegotiation case, no V.42: the answerer's source sends 16-bit counter words MSB first; the caller's sink counts gaps after the renegotiation.

```c
CHECK_EQ_I(lost_words, 0);                       /* today one frame (21..42 bits) is lost */
/* proberx_t with cr, ci giving |num| about 2^41 (tones turning 1 rad per 20 ms block) */
CHECK_EQ_I(p2call_clock_q8(&r), clamp_q8);       /* the +-1000 ppm clamp; check-ubsan clean */
```

- [ ] **Step 2: Run to see it fail.** `lost_words` ≥ 1; the UBSan build reports `signed integer overflow` in `p2call.c`.
- [ ] **Step 3: Implement**: at Rd detection deliver the older slot when it is not Rd; saturate before the multiply.
- [ ] **Step 4: Verify.** `make verify` (only the new lines added); bench-v90c, bench-v92c hashes unchanged.
- [ ] **Step 5: Commit** `v90a: deliver the frame before Rd; p2call: clock_q8 saturates before it can overflow`.

### Task 11: Calling: partial CPd; PCM only on the JM's digital bit (call B1, B3)

**Files:**
- Modify: `modem/v92aseq.{c,h}:87`, `:114`; `modem/v90a.c:1040-1042` and Phase 4 entry; `modem/v8msg.{c,h}:113-116`; `modem/v8orig.c:156`; `CHANGELOG.md`, `docs/calling.md`
- Test: `test/test_v92aseq.c`, `test/test_v8msg.c`, `test/test_v8orig.c`

**Interfaces:**
- Produces: `void v92aseq_phase4(v92aseq_t *s)` (forget the stored CPd at the start of each Phase 4); a parsed CPd merges into the stored one: always-carried fields (drn, trellis, e2u_extend, ack, gain4) replaced, each present part replaced, absent parts kept, `has_*` or-ed; `int v92aseq_cpd_complete(const v92aseq_t *s)` (1 when all three parts are present), used by `start_b1u`. No new fields in `v90a_t` (D16). `int v8_selected_caller(const v8_menu_t *jm)`: as `v8_selected` but PCM only with `V8_PCM_DIGITAL`.

- [ ] **Step 1: Write the failing tests.**

```c
/* full CPd A, then CPd B with has_mod = 1, has_filt = 0, has_const = 0 */
CHECK_EQ_I(cpd->has_filt, 1); CHECK_EQ_I(cpd->has_const, 1);
CHECK(memcmp(&cpd->filt, &A.filt, sizeof A.filt) == 0); CHECK_EQ_I(cpd->map.M[0], B.map.M[0]);
CHECK_EQ_I(v92aseq_cpd_complete(&s), 1);
/* v92aseq_phase4(), then B alone */ CHECK_EQ_I(v92aseq_cpd_complete(&s), 0);
/* JM: V.34 + pcm = V8_PCM_ANALOG */ CHECK_EQ_I(v8_selected_caller(&jm), v8_selected(&jm_v34_only));
/* JM: V.34 + pcm = V8_PCM_DIGITAL */ CHECK_EQ_I(v8_selected_caller(&jm), v8_selected(&jm));
/* test_v8orig: a scripted JM naming PCM-analogue only */ CHECK_EQ_I(v.selected, v8_selected(&jm_v34_only));
```

- [ ] **Step 2: Run to see it fail** (parts zeroed by `v92aseq.c:87`; V.90 selected).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** `make verify`; replay unchanged (our answerer sends all three parts and sets DIGITAL); `test_realcheck` unchanged.
- [ ] **Step 5: Commit** `v92aseq: a CPd carries only what changed; v8orig: V.90 only when the JM names a digital modem`.

### Task 12: 32-bit sample clocks (ans R-2, call B5, leg B2; D6)

**Files:**
- Create: `modem/clock.h`, `test/test_clock.c` (in `TESTS`)
- Modify: `modem/answer.c:367`, `modem/x2d.{c,h}:386`, `modem/k56d.{c,h}:404`, `modem/p34d.{c,h}:392`, `modem/v34dx.{c,h}:576`, `modem/orig.c:307`, `modem/v90a.{c,h}:1224`, `modem/v90arx.{c,h}:586,671,697,758`, `modem/v22bis.c:135-136`, `CHANGELOG.md`

**Interfaces:**
- Produces: `static inline int32_t clk_sat_inc(int32_t t)` (+1 up to `INT32_MAX`); `static inline int32_t clk_diff(uint32_t a, uint32_t b)` (`a - b` modulo 2^32 as int32, written without implementation-defined conversion: `d <= INT32_MAX ? (int32_t)d : -(int32_t)(UINT32_MAX - d) - 1`). Monotone counters (`answer_t.t`, `orig_t.t`, `v22bis_t.t`) use `clk_sat_inc`; `now`/`t` of x2d, k56d, p34d, v34dx, v90a and `v90arx.n`, `.neq` become `uint32_t`, every difference through `clk_diff`, `neq % rc` on uint32. Sizes unchanged.

- [ ] **Step 1: Write the failing test.**

```c
CHECK_EQ_I(clk_diff(5u, 0xFFFFFFFBu), 10); CHECK_EQ_I(clk_diff(0u, 1u), -1);
CHECK_EQ_I(clk_diff(0x80000000u, 0u), INT32_MIN); CHECK_EQ_I(clk_sat_inc(INT32_MAX), INT32_MAX);
/* V.90 answering, x2, K56, V.34 answering, V.90 calling, V.22bis: in data mode set every clock to
   2^31 - 8000 (int32: INT32_MAX - 8000), run 3 s */
CHECK(moving); CHECK_EQ_I(bit_errors, 0);
/* after V90M_EV_ENDED: answer t = INT32_MAX - 10, 100 ticks */ CHECK_EQ_I(st.end, end_before);
```

- [ ] **Step 2: Run to see it fail** under UBSan: `make -s BUILD=build/ubsan CC=clang CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=undefined" build/ubsan/test_clock && build/ubsan/test_clock` → `runtime error: signed integer overflow` (and, for v90arx, `moving` with no decisions).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** UBSan build of `test_clock` clean; `make verify` with no re-record but the new program's; pins unchanged.
- [ ] **Step 5: Commit** `modem: sample clocks saturate or wrap defined after 2^31 samples`.

### Task 13: V.22bis: S1 bounded; v22rx products in 64 bits (leg B1, B3)

**Files:**
- Modify: `modem/v22bis.{c,h}:175-176`, `:244-245`; `modem/v22rx.c:237`, `:251-252`; `CHANGELOG.md`
- Test: `test/test_v22bis.c`, `test/api/test_api.c`

**Interfaces:**
- Produces: `#define V22_S1_MAX_MS 1000`: an S1 longer than that → `retrain_send()` when retraining (its `RT_MAX_TRIES` then ends with `V22S_FAILED`), back to `WAIT`/`TRAIN` in start-up. `(int64_t)perr * 20861`, `(int64_t)fr * r->xr[k]`.

- [ ] **Step 1: Write the failing test** (from `build/scratch-legacy-tests/fuzz/s1hang.c`): both roles at 2400 bit/s in data mode, then the answerer's input becomes a 1200 Hz sine of amplitude 3000 (second run: 1205 Hz).

```c
CHECK_EQ_I(st_a.end, V90M_END_LOST); CHECK(t_end - t_tone <= MS(30000));
/* the caller fed the same tone */ CHECK_EQ_I(st_o.end, V90M_END_LOST);
/* v22rx: gain high, full-scale click at 48 < ndata <= 600: check-ubsan clean */
```

- [ ] **Step 2: Run to see it fail** (state 3 `S1_RX`, `retraining=1` after 60 s).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** `make verify` (no S1 in any test or recording lasts 1 s: no re-record but the new lines).
- [ ] **Step 5: Commit** `v22bis: an S1 longer than 1 s is not waited for; v22rx: products in 64 bits`.

### Task 14: API: callbacks and the NULL policy (v42 B1, B8; D2, D8)

**Files:**
- Modify: `api/v90modem.c:40-60` (flags), `:270-309` (process loops), `:444` and every public function; `include/v90modem.h`; `docs/api.md:10-14`; `CHANGELOG.md`
- Test: `test/api/test_api.c` (`test_callbacks`, `test_null`, `test_block_sizes`)

**Interfaces:**
- Produces: `uint8_t busy, dead` in `struct v90m` next to the existing flags (if `test_api`'s pins move, the `was_*` flags they sit with become `uint8_t`); `v90m_reset` on a busy modem → `V90M_ESTATE`; `v90m_destroy` on a busy modem sets `dead`; the loop ends after the current sample, fills the rest of `tx` with silence (linear 0, mu-law 0xFF, A-law 0xD5), frees when owned, returns `V90M_OK`. NULL results as D8.

- [ ] **Step 1: Write the failing tests** (from `build/scratch-v42-api/reent.c`).

```c
/* answering, silence until V90M_EV_ENDED; on_event calls v90m_reset(m, &cfg_originate) */
CHECK_EQ_I(rc_reset, V90M_ESTATE); CHECK_EQ_I(st.end, V90M_END_FAILED);   /* check-ubsan clean */
/* on_event calls v90m_destroy(m) at ENDED (created modem) */
CHECK_EQ_I(rc_process, V90M_OK); CHECK(all_zero(tx + k + 1, n - k - 1));
/* on_event calls v90m_cleardown(m) at DATA_MOVING */ CHECK_EQ_I(rc_cleardown, V90M_OK);
/* NULL */ CHECK_EQ_I(v90m_retrain(NULL), V90M_EINVAL); CHECK_EQ_I(v90m_ec_reset(NULL, &ec), V90M_EINVAL);
          CHECK_EQ_I(v90m_ec_state(NULL), V90M_EINVAL); CHECK_EQ_I(v90m_ec_read(NULL, b, 4), 0);
          v90m_get_state(NULL, &st); v90m_get_stats(NULL, &sx); v90m_ec_tick(NULL, 1); /* no crash */
/* Review Focus 1: one V.90 call with n = 1, 160, 4000 */
CHECK_EQ_I(hash_tx[1], hash_tx[0]); CHECK_EQ_I(hash_tx[2], hash_tx[0]); CHECK(same_events);
```

- [ ] **Step 2: Run to see it fail** (UBSan `division by zero` at `modem/async.c:46`; `v90m_ec_reset(NULL, …)` segfaults: run that line last).
- [ ] **Step 3: Implement.** Document in the header and docs/api.md which calls a callback may make (D2) and the NULL rule (D8), naming the four per-bit functions that must not get NULL.
- [ ] **Step 4: Verify.** `make BUILD=build/asan CC=clang CFLAGS="-std=c99 -O1 -g -fsanitize=address,undefined" test-api` → `0 failures`; `make verify`.
- [ ] **Step 5: Commit** `api: reset refused and destroy deferred inside a callback; one NULL rule`.

### Task 15: API: configuration validation (v42 B4, B7, B10, B11, R4; ans R-6, R7; D7)

**Files:**
- Modify: `api/v90modem.c:83` (`check_cfg`), the ec create/init/reset paths, `v90m_ec_send_break`; `v42/v42.c:510`, `:520`; `modem/answer.c` (`answer_v90_cfg`, `k56_offer`, `x2_offer`); `include/v90modem.h`; `docs/api.md`; `CHANGELOG.md`
- Test: `test/api/test_api.c` (`test_cfg_ranges`), `v42/test/test_v42.c`

**Interfaces:**
- Produces: `#define V90M_TX_LEVEL_MAX 16384`; `static int check_ec_cfg(const v90m_ec_cfg *c)` → `V90M_EINVAL`. Ranges: a value the current code clamps to a different value is invalid, except 0 where the code treats 0 as default or off; `comp_policy` 0..2; `v42bis_p1` 512..`V42COMP_MAX_CODEWORDS`; `v42bis_p2` 6..250; `establish_ms`, `t403_ms` ≥ 0; flags 0..1. Every range written in the header next to its field. `max_up_bps` in 1..2399 with `V90M_MOD_X2` or `V90M_MOD_K56` → `V90M_EINVAL`; `V90M_MOD_BELL212A` without `V90M_MOD_V22` → `V90M_ENOTSUP`; `v42ec_tick` clamps `ms` to 1 000 000; `k56_offer`/`x2_offer` change nothing after the first tick.

- [ ] **Step 1: Write the failing tests.**

```c
c.tx_level = INT_MAX;  CHECK(v90m_create(&c, &err) == NULL); CHECK_EQ_I(err, V90M_EINVAL);
c.tx_level = 16385;    /* same */                c.tx_level = 16384; CHECK(m != NULL);
/* tx_level 16384: back-to-back V.21, V.22, V.22bis, V.32bis, V.34, V.90 and one A-law V.90: connect, check-ubsan clean */
c.modulations = V90M_MOD_V34 | V90M_MOD_X2; c.max_up_bps = 2399;  /* EINVAL; same with K56 */
c.modulations = V90M_MOD_BELL212A;                               /* V90M_ENOTSUP */
ec.comp_policy = 3; ec.establish_ms = -1; ec.lapm.t403_ms = -1; ec.lapm.v42bis_p1 = 4096;  /* each EINVAL */
CHECK_EQ_I(v90m_ec_send_break(e, 0x80, 10), V90M_EINVAL);
/* v42ec in DETECT, v42ec_tick(&e, UINT_MAX) */ CHECK(t400_expired);   /* check-ubsan clean */
/* answer_v90_cfg after the first tick */ CHECK_EQ_I(a->k56_offered, before);
```

- [ ] **Step 2: Run to see it fail** (UBSan `signed integer overflow: 2147483647 * 181` at `modem/answer.c:143`; the others accepted).
- [ ] **Step 3: Implement.** Before fixing the bound, grep every use of the level (`cfg.level`, `tx_level`) in `modem/`, `v32/`, `api/`; if any scaling wraps int16 below 16384, set `V90M_TX_LEVEL_MAX` to the largest multiple of 1024 that does not (D7) and say so in the commit.
- [ ] **Step 4: Verify.** `make verify`.
- [ ] **Step 5: Commit** `api: tx_level, upstream ceiling, Bell 212A and V.42 configuration validated; break option checked`.

### Task 16: V.42 endpoint: abort in every state; the originator sees HDLC flags in detection (v42 B3, B6)

**Files:**
- Modify: `v42/v42.{c,h}` (`:243`, `:426-463`), `api/v90modem.c:486`, `CHANGELOG.md`
- Test: `v42/test/test_v42.c` (`test_abort_states`, `test_detection_mirror`), `test/api/test_api.c`

**Interfaces:**
- Produces: `void v42ec_abort(v42ec_t *e)`: in every state, LAPM stopped and `finish(e, V42LAPM_R_LOCAL)` at once; already-owed UA/DM may still go. The originator in DETECT also runs `v42hdlc_rx_bit`; `hrx.flag_run >= FLAGS_SEEN` (`v42.c:8`, 3) → `want_hdlc`.

- [ ] **Step 1: Write the failing tests** (from `build/scratch-v42-api/abort.c`, `det.c`).

```c
/* DETECT, abort */            CHECK_EQ_I(v90m_ec_state(e), V90M_EC_DISCONNECTED);
/* ... after T400 */           CHECK_EQ_I(v90m_ec_state(e), V90M_EC_DISCONNECTED);
/* ASYNC, abort */             CHECK_EQ_I(v90m_ec_state(e), V90M_EC_DISCONNECTED); CHECK_EQ_I(v90m_ec_write(e, b, 1), 0);
/* CONNECTED, abort, no call */ CHECK_EQ_I(v90m_ec_state(a), V90M_EC_DISCONNECTED);
CHECK_EQ_I(st.reason, V90M_EC_R_LOCAL);
/* originator detect on, answerer detect off */ CHECK_EQ_I(v42ec_state(&o), V42EC_CONNECTED);
CHECK_EQ_I(octets_read_by_originator, 0);
```

- [ ] **Step 2: Run to see it fail** (`abort.c` printed states 0, 3, 3 and 2; `det.c` printed `originator(detect on) state 3 … read 1 octets`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** `make verify`; the V.42 golden of Task 2 unchanged (no abort, both ends detect).
- [ ] **Step 5: Commit** `v42: abort stops the endpoint in every state; an originator in detection takes the answerer's flags`.

### Task 17: V.42: P1 ceiling, LAPM progress watchdog, XID response (v42 B2, R1, R3, R6, R7; D9)

**Files:**
- Modify: `v42/v42.c:83-107`, `:172-176`; `v42/v42_lapm.{c,h}:4-7`, `:477-501`, `:1049`; `docs/error-control.md`; `CHANGELOG.md`
- Test: `v42/test/test_v42.c`, `v42/test/test_lapm.c`

**Interfaces:**
- Produces: `cfg.lapm.comp_p1` clamped to `V42COMP_MAX_CODEWORDS` in `v42ec_init`; a C-INIT that fails releases with `V42EC_R_COMPRESSION`; `lapm.no_progress` (recoveries since V(A) last advanced while I frames are outstanding) ≥ `n400` → release with `V42LAPM_R_N400`; `rx_xid_resp` takes the responder's k and N401 when smaller than ours; a compile-time check that the largest XID response (58 octets, v42 R6) fits `xid_resp`; `V42_XID_TRACE` code removed.

- [ ] **Step 1: Write the failing tests.**

```c
/* both ends v42ec cfg v42bis_p1 = 4096 (internal cfg; the API refuses it since Task 15) */
CHECK_EQ_I(st.comp_n2, V42COMP_MAX_CODEWORDS); CHECK_EQ_I(st.enc_on, 1); CHECK(round_trip_ok);
/* after negotiation, comp_n2 forced to 4096 before comp_init */
CHECK_EQ_I(e.state, V42EC_DISCONNECTED); CHECK_EQ_I(e.reason, V42EC_R_COMPRESSION);
/* peer answers every poll with RR F=1, N(R) = V(A); t401 100 ms, n400 3 */
CHECK_EQ_I(v42ec_state(&e), V42EC_DISCONNECTED); CHECK_EQ_I(st.reason, V42LAPM_R_N400); CHECK(t <= 5000);
/* responder answers k = 4, N401 = 64 to our 15 / 128 */ CHECK_EQ_I(st.k_tx, 4); CHECK_EQ_I(st.n401_tx, 64);
```

- [ ] **Step 2: Run to see it fail** (`p1test.c` showed `comp_tx 1, comp_n2 4096`, `encoder on 0`; the watchdog case still CONNECTED after 60 s; k_tx 15).
- [ ] **Step 3: Implement.** `docs/error-control.md`: the ruling (D9: deviation from V.42/HDLC, which restart the count at each recovery, because every wait must be bounded), the T403-off sentence (v42 R2), the P1 ceiling.
- [ ] **Step 4: Verify.** `make verify`; V.42 golden unchanged.
- [ ] **Step 5: Commit** `v42: V.42bis P1 within the dictionary; LAPM released after N400 recoveries without progress; XID response honoured`.

### Task 18: SONAME per minor version; static library internals hidden (v42 B5, refactor 6; D10)

**Files:**
- Modify: `Makefile:37-38`, `:170-195` (shared library, install, `check-symbols`), `tools/symmap.sh`, `CHANGELOG.md`, `docs/getting-started.md` (if it names `.so.0`)

**Interfaces:**
- Produces: `SOVER` = `MAJOR.MINOR` from `VERSION` while MAJOR is 0, `MAJOR` from 1; `make check-soname` (`readelf -d` shows `libv90modem.so.$(SOVER)`), run by `check`. `symmap.sh` renames `v90m__*` not already `v90m__i_*`; `check-symbols` rejects them.

- [ ] **Step 1: Failing checks.** `make check-soname` → missing target; after writing it first: `SONAME [libv90modem.so.0]`, want `libv90modem.so.0.5`. `nm -g build/libv90modem.a | grep -cE ' v90m__(answer|orig|ec)$'` → 3, want 0 (write the check into `check-symbols` first: it fails).
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify.** `make check-soname` → `soname: libv90modem.so.0.5`; `make check-symbols` passes; `make install PREFIX=build/inst` creates `libv90modem.so`, `.so.0.5`, `.so.0.5.0`; the CI pkg-config example builds and runs; `make verify`.
- [ ] **Step 4: Commit** `build: SONAME per minor version before 1.0; the static library hides v90m__ internals`.

### Task 19: K56flex: a client that ignores our MP; tone B during a renegotiation (ans §5, R-3; D12)

**Files:**
- Modify: `sim/k56call.{c,h}` (`heavy_up`, `retrain_in_reneg`), `modem/k56d.{c,h}:84-91`, `:146`, `:266-367`, `:427-439`, `tools/rec_k56.c` (argument `heavy`), `CHANGELOG.md`, `docs/answering.md`
- Test: `test/test_answer_k56.c`

**Interfaces:**
- Produces: `k56call_cfg.heavy_up` (1: its upstream uses 64 states, non-linear, expanded shaping, GPC, whatever our MP asked); `k56call_cfg.retrain_in_reneg` (gives up its renegotiation and retrains: tone B during `K56D_RENEG`). In `k56d_t`: `#define K56D_RING 192`, `int16_t ring[K56D_RING][2]` (clipped points before `v34_nl_decode`, from E on), `uint8_t up_hyp` (0 = ours, 1 = x2's configuration), catch-up state; when B1 fails under hypothesis A: re-init `vrx`/`vpar` with 64 states, non-linear, expanded, GPC, same rate; decode stored points, 3 per symbol period, with `QRX_CARRIER | QRX_TIMING` only; B passes → live under B; else `K56D_ERR_B1`. A trace line `k56 upstream: hypothesis B` when B is taken. Tone B in `K56D_RENEG` → `K56D_RETRAIN`.

- [ ] **Step 1: Write the failing tests.**

```c
/* heavy_up = 1, 40 s */ CHECK_EQ_I(d.state, K56D_DATA); CHECK_EQ_I(d.up_hyp, 1); CHECK_EQ_I(up_errors_10s, 0);
/* every existing case */ CHECK_EQ_I(d.up_hyp, 0);
/* Review Focus 3: b1_bad = 1, heavy_up 0 and 1 */ CHECK_EQ_I(d.err, K56D_ERR_B1);
/* retrain_in_reneg = 1 */ CHECK_EQ_I(d.state, K56D_DATA); CHECK_EQ_I(retrains_seen, 1);
```

- [ ] **Step 2: Run to see it fail** (heavy_up ends `K56D_ERR_B1`; retrain_in_reneg ends `K56_FAILED` at `K56D_T_RENEG`, 6 s).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** `make verify` (existing K56 outputs and `bench_v90 -k` hashes unchanged; replay `k56`/`k56-v92` unchanged); pins unchanged (k56d_t stays under v92d_t, 45.1 KB). `rec_k56 40 0 build/k56h.rec heavy`, then `make cost-k56 K56_REC=build/k56h.rec ARM_CC=$A5` → record data-mode cost (expected about x2's, 82 M) and the heaviest 10 ms block during the catch-up in `docs/history/audit-0.6.md`.
- [ ] **Step 5: Commit** `k56d: decode a client that ignores our MP (B1 retried under x2's configuration); follow tone B in a renegotiation`.

### Task 20: x2: one click must not ruin the upstream (dsp 2.1, 2.2; D11)

**Files:**
- Modify (by outcome): `modem/qamrx.c:189`, `:194`, `:215`, `:222` and its carrier/timing loop updates; or `modem/x2d.{c,h}`, `modem/k56d.{c,h}` (watchdog); `CHANGELOG.md`, `docs/limits.md`, `docs/history/audit-0.6.md`
- Test: `test/test_answer_x2.c`, `test/test_answer_k56.c`, `test/test_answer_v90.c`, `test/test_answer_v34.c` (click cases)

**Interfaces:**
- Produces (fallback only): `X2D_ERR_DEAF`, `K56D_ERR_DEAF`: upstream MSE above p34d's `mse_limit` for the rate for 2 s in data mode → `fail(a, ANS_END_LOST)`.

- [ ] **Step 1: Write the failing test.** x2 call as `rec_x2 40 0` makes it; at 15.000 s of line, 6 samples to the answerer replaced by mu-law 0x80/0x00 alternated (full scale, 0.75 ms; dsp's `x2_click.rec`).

```c
CHECK(errs_from(click + MS(2000)) == 0 || (ended && end == ANS_END_LOST && t_end <= click + MS(3000)));
/* the same click in K56 data */ /* same assertion */
/* V.90 and V.34: */ CHECK(errs_from(click + MS(10000)) == 0 || end != ANS_END_NONE);
/* Review Focus 4: the lowest-SNR x2 case that connects today, 60 s of data */ CHECK_EQ_I(end, ANS_END_NONE);
```

- [ ] **Step 2: Run to see it fail** (dsp: mse 16 → 257..363 to the end, no 128-bit stretch matches).
- [ ] **Step 3: Investigate, in order, each tried alone then combined:** (a) clamp `er`, `ei` to ±2^15 before the tap, carrier and timing updates (`qamrx.c:189`); (b) bound the per-symbol change of `freq` (one click symbol can add ±2^19 today) and the excursion of `t_integ` (±200 clean, -3500..+16000 after the click); (c) print mse, `freq`, `t_integ`, centroid per 100 ms after the click to find what does not return. A candidate counts only if `make verify` passes with no re-record (golden, replay, bench hashes) and the test passes. Record what each try did in `docs/history/audit-0.6.md`.
- [ ] **Step 4: Fallback.** If no candidate qualifies, implement the watchdog (Interfaces) and keep the clamp of (a) only if it qualifies alone.
- [ ] **Step 5: Verify.** Tests pass; `make verify`; cost-x2 within +0.3 M of before.
- [ ] **Step 6: Commit** `qamrx: <root cause> after an impulse` or `x2d, k56d: end a call whose upstream stays deaf`.

### Task 21: V.92 digital side: Important findings; Phase 1 gate

**Files:**
- As each `T21.k` of `docs/history/audit-0.6.md` names; `CHANGELOG.md`

- [ ] **Step 1: For each `T21.k`:** write its test, run it and see it fail as the audit says, fix, `make verify` (re-records listed), commit `v92: <finding>`. If the audit found no Important finding, write "Phase 1: no Important V.92 digital-side finding" in the audit doc.
- [ ] **Step 2: Phase 1 gate.** `make verify-phase` → `VERIFY-PHASE PASS`; `tools/replay-proof.sh diff v0.5.0`: every differing run listed in `docs/history/audit-0.6.md` with the task that explains it; costs "after Phase 1" recorded (each within 0.5 M of baseline, the K56 heavy figure separate).
- [ ] **Step 3: Commit** `docs: Phase 1 gate figures`.

---

## Phase 2: refactor (byte-identical)

Every task here ends with `make verify` → `VERIFY PASS` and no golden file
re-recorded. Tasks 22-28 may change `v90m_mem_size()`; when they do, they
update the pins in `test/api/test_api.c:186-188` and say so in the commit
body (Task 29 fixes the final values and the docs).

### Task 22: One copy of each table and primitive (dsp 3.1-3.3; ans R5)

**Files:**
- Create: `modem/sintab.c`, `modem/v34_tabs.c`
- Modify: `modem/sintab.h:8`, `modem/v34_tabs.h:10`, `modem/v34train.c:37` and its PP/S/S-bar, `v34/v34_core.c:16`, `v34/v34_train.c`, `v90/v90_scr.c:21`, `modem/x2rec.c:17`, `modem/info.{c,h}:253-282`, `Makefile`

**Interfaces:**
- Produces: `extern const int16_t sintab[1024];`, `extern const int16_t v34_interp[…]` (same dimensions); `v34scr_*` and `v90_scramble_*` thin wrappers over `v34_scramble`/`v34_descramble`; PP, S, S-bar: one sequence generator in `v34/v34_train.c`; each user keeps its own value scaling only if a shared scale is not exact for every point; `x2_crc16` → `info_crc`; `info1c_build`/`info1c_parse` `static inline` aliases of the `info1d_*` functions.

- [ ] **Step 1: Write the equivalence tests first** (new program `test_shared`): old and new scrambler outputs over 10^6 random bits from 100 random states each direction; every PP/S/S-bar point of both scalings equal to the old tables.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify.** `make verify`; `nm build/bench_v90.arm | grep -c ' sintab$'` → 1 (was 16).
- [ ] **Step 4: Commit** `modem, v34, v90: sintab and v34_interp once; one V.34 scrambler, one PP/S/S-bar, one INFO CRC`.

### Task 23: Shared helpers, dead code, include paths (ans R4, R6, R7; call F3, F4, F5, F7; dsp 3.5, 3.7; leg F5)

**Files:**
- Create: `modem/p2common.{c,h}`, `modem/fixmath.h`, `modem/seqhunt.h`, `test/test_fixmath.c`
- Modify: `modem/p2d.c:93-95,143`, `modem/p2ans.c:15-20,188,310`, `modem/p2call.c`; the isqrt copies (`anstone.c:24`, `dpsk.c:157`, `v22rx.c:31`, `probe.c:110`, `qamrx.c:25`, `v90arx.c:31`, `v92_p34d.c:58`, `v92_design.c:7`, `v32_code.c:105`, `v90dil.c:446`); the sync hunters (`v90a.c:397,621,942`, `v92aseq.c:125`, `v92_p34d.c:239`, `p34d.c:193`, `x2d.c:219`, `k56d.c:234`, `v34dx.c:347`); `k56enc.c:47`, `v34ans.c:169-174`, `v90dil.c:405`, `v34/v34_rx.c:551-575,672`, `v34/v34_params.c:1-4`; includes `k56enc.h:32`, `k56tx.h:27`, `k56enc.c:10`, `k56tx.c:12`, `x2map.c:9`; `Makefile` (`check-headers`)

**Interfaces:**
- Produces: `p2common.h`: `tone_under_probe` (today's signature, `p2d.c:143`), the `MS`/`TONE_SEEN`/`TURN`/`TAIL`/`PROBE_BLOCKS` constants, V.34 Tables 1/2 (both forms if one cannot be derived exactly from the other), `db10_lin`, `lin_q16`, `template_db10`, `band`; comment that `P2D_L1` ends at `MS(160) - 1` and `P2N_L1` at `MS(160)` deliberately. `fixmath.h`: `static inline uint32_t isqrt_u64(uint64_t)`, `static inline int32_t isqrt_s64(int64_t)` (and a named variant for any copy whose `v <= 0` result differs). `seqhunt.h`: `typedef struct { int ones; uint32_t buf; int n; } seqhunt_t; static inline int seqhunt_bit(seqhunt_t *h, int bit);`. `v34_rx_set_hard`, `v34_rx_flush` and the hard path under `#ifdef V34_TEST_API` (defined by `v34/Makefile` only). `make check-headers`: every header under `modem/ v34/ v90/ v92/ v32/ v42/ include/` compiles alone with `-I. -Iinclude`.

- [ ] **Step 1: Differential tests first.** `test_fixmath`: each old isqrt copy (kept in the test) vs the shared one over every input < 2^20, 10^6 random 64-bit inputs, and 0, negative, `INT64_MAX`, `UINT64_MAX` where the copy accepts them; each sync-hunter copy vs `seqhunt_bit` over every bit sequence of length ≤ 24 and 10^6 random sequences. A copy that differs keeps its own code with a comment.
- [ ] **Step 2: Implement.** `make check-headers` fails before the include fix (`modem/answer.h` with only `-I.`), passes after.
- [ ] **Step 3: Verify.** `make verify`.
- [ ] **Step 4: Commit** `modem: Phase 2 helpers shared, one isqrt, one sync hunter, dead code out, includes relative`.

### Task 24: `v34up`, part 1: the shared V.34 upstream receiver; p34d and v34dx on it (ans R3; dsp 3.4)

**Files:**
- Create: `modem/v34up.{c,h}`
- Modify: `modem/p34d.{c,h}:294-386,414`, `modem/v34dx.{c,h}:436,476,485-565`, `Makefile`

**Interfaces:**
- Produces (spec §8, ans R3):

```c
typedef struct { qamrx_t rx; v34_tdem_t tdem; v34trn_t ref; int64_t zpw;
                 int32_t quiet, q_dead, q_bad, mse_limit; v34_params_t par; v34_rx_t vrx;
                 int32_t gain; int rx_ready; } v34up_t;
enum { V34UP_NONE, V34UP_B1_BAD, V34UP_DEAD, V34UP_SLOW, V34UP_LOST };
void     v34up_sync_pp(v34up_t *, const sdet_t *, int sym_rate);   /* 132000*256/sym_rate - (SDET_DELAY+1)*256 */
int      v34up_known(v34up_t *, int n);
unsigned v34up_dec4(v34up_t *, unsigned upd_flags);
unsigned v34up_dec16(v34up_t *, int gate_eq);                       /* x2d/k56d 1 (|e| < lev/2), v34dx 0 */
int      v34up_data_begin(v34up_t *, const v34_cfg_t *, v34_putbit_fn, void *);
int      v34up_data(v34up_t *, int watch_dead, int watch_slow);      /* V34UP_*; includes the (z*512)/g scaling */
void     v34up_ec_schedule(/* x2d.c:361's parameters */);
/* clip16, word16: static inline in v34up.h */
```

- [ ] **Step 1: Identity check first.** Add to `test_shared`: `v34_nl_decode`/`v34_nl_encode` with `cfg.nonlinear == 0` return their input for every point of the int16 grid at step 16 (`v34/v34_nl.c:28,38`), so p34d can call the shared data path.
- [ ] **Step 2: Move the code verbatim** (arithmetic and update flags unchanged); p34d and v34dx keep their policies (retrain on `V34UP_DEAD`, rate down on `V34UP_SLOW`).
- [ ] **Step 3: Verify.** `make verify`; `make bench-v90 bench-v34 ARM_CC=$A5` (host hash = qemu hash); `make cost-x2 cost-v34 ARM_CC=$A5`: V.90 and V.34 within +0.3 M of before (else make the per-symbol functions `static inline` in `v34up.h`); record in the audit doc.
- [ ] **Step 4: Commit** `modem: one V.34 upstream receiver (v34up) for p34d and v34dx`.

### Task 25: `v34up`, part 2: x2d and k56d on it; the K56 retry in `v34up`

**Files:**
- Modify: `modem/x2d.c:219-357,361,404`, `modem/k56d.c:234-367,371,390` (and Task 19's retry), `modem/v34up.{c,h}`

**Interfaces:**
- Consumes: Task 24's `v34up_*`.
- Produces: `int v34up_retry(v34up_t *u, const v34_cfg_t *alt)` (re-inits `vrx`/`vpar` under `alt`); `int v34up_replay_sym(v34up_t *u, int16_t re, int16_t im)` (one stored point, updates frozen). The ring and pacing stay in `k56d_t` (so p34d's `v34up_t`, outside the union, does not grow). `put_data` and the common part of `rx_begin_data` move into `v34up`.

- [ ] **Step 1: Move verbatim.**
- [ ] **Step 2: Verify.** `make verify` (incl. Task 19's heavy case and the x2/K56 click cases); `make bench-x2 bench-k56 ARM_CC=$A5`; `make cost-x2 cost-k56 ARM_CC=$A5` within +0.3 M; about 300 duplicated lines gone (`git diff --stat`).
- [ ] **Step 3: Commit** `modem: x2d and k56d on v34up; the K56 B1 retry is v34up_retry`.

### Task 26: `answer.c`, one file per scheme behind a phase table (ans R2, R8)

**Files:**
- Create: `modem/ans_scheme.h`, `modem/ans_legacy.c`, `modem/ans_v90.c`, `modem/ans_v34.c`, `modem/ans_x2.c`, `modem/ans_k56.c`
- Modify: `modem/answer.{c,h}` (`:229,498,510,521,544,548,630,687,697,733,745,755,765`), `api/v90modem.c:354-358`, `Makefile`

**Interfaces:**
- Produces (ans R2):

```c
typedef struct {
    int16_t (*tick)(answer_t *, int16_t rx);
    int     (*cleardown)(answer_t *);        /* NULL: -1 */
    int     (*retrain)(answer_t *);          /* NULL: -1 */
    int     (*moving)(const answer_t *);     /* NULL: connected && !retraining */
    int     (*snr_den)(const answer_t *, int64_t *num, int64_t *den);
    int     (*rtd)(const answer_t *);
    int     (*echo_delay)(const answer_t *);
    uint8_t pcm_out;
} ans_scheme_t;
extern const ans_scheme_t ans_legacy, ans_v90, ans_v92, ans_v34, ans_x2, ans_k56;
typedef struct { int limit; int32_t forget_ms; uint8_t count_after; } rt_policy_t;  /* exact today's >= / ++ >= semantics */
void rt_begin(answer_t *, const rt_policy_t *);
int  rt_data_tick(answer_t *, const rt_policy_t *);
```

`answer_t` gets `const ans_scheme_t *s`, set on each phase entry; `answer.c` keeps init, dispatch, end reasons, the phase backstop. Files: `ans_legacy.c` (V.21, Bell 103, V.22/212A, V.32 AA + V.32), `ans_v90.c` (P2, RETRAIN, P3, V90, P3X, V92, V.34 hand-off), `ans_v34.c`, `ans_x2.c`, `ans_k56.c` (V8BIS, K56, K56RT). Tick order inside a phase unchanged; the union unchanged; `V34A_MAX_RETRAINS` stays in `v34ans.c`.

- [ ] **Step 1: Move the code.**
- [ ] **Step 2: Verify.** `make verify` (incl. Task 14's block-size test, Task 7's end codes, Task 8's backstop); `make bench-v90 bench-x2 bench-k56 ARM_CC=$A5`; `make cost-x2 cost-k56 ARM_CC=$A5` within +0.3 M.
- [ ] **Step 3: Commit** `answer: one file per scheme behind a phase table; one retrain policy per scheme`.

### Task 27: `v90a.c` split (call F2)

**Files:**
- Create: `modem/v90a_up90.c`, `modem/v90a_up92.c`, `modem/v90a_down.c`, `modem/v90a_watch.c`, `modem/v90a_priv.h`
- Modify: `modem/v90a.c`, `Makefile`

**Interfaces:**
- Produces: `v90a_up90.c` (`up_symbol`: S, S-bar, MD, PP, TRN, Ja, CP, E, data), `v90a_up92.c` (`up92_symbol`, `seq_next`, `start_b1u`, `train92`, `jd92_bit`), `v90a_down.c` (`jd_bit`, `dil_*`, `ri_dec`, `frame`, `rd_dec`, `mp_bit`), `v90a_watch.c` (`line_watch`, `begin_retrain`, `watch`), `v90a.c` (init, tick, API). Former statics become `v90a__<name>` declared in `v90a_priv.h`.

- [ ] **Step 1: Move the code.**
- [ ] **Step 2: Verify.** `make verify`; `make bench-v90c bench-v92c ARM_CC=$A5`; `make cost-v90c cost-v92c ARM_CC=$A5` within +0.3 M (else move the per-symbol callees back into their caller's file).
- [ ] **Step 3: Commit** `v90a: split into V.90 upstream, V.92 upstream, downstream sequences and the watch`.

### Task 28: V.42 and API refactors (v42 refactors 1-5, 7; R5)

**Files:**
- Create: `v42/comp_out.h`
- Modify: `api/v90modem.c:34,38,244-265,403-439,491-510`, `v42/v42_comp.c:265-272`, `v42/v44_comp.c:54-61`, `v42/v42.c:110-134`, `Makefile` (`check-version` in `check`)

**Interfaces:**
- Produces: `#define V90M_EC_CFG_FIELDS(X)` and `V90M_EC_STATUS_FIELDS(X)` generating `cfg_in`, `v90m_ec_cfg_default`, `v90m_ec_get_status`; `comp_out.h`: the bounded output writer with a compile-time check that one step's output fits `ovf` (64 / 40 octets); a two-entry compression ops table in `v42.c`; `after_answer()` / `after_orig()` from the split loops; `v90m_version()` from the header's version macros; `make check-version` (Makefile `VERSION` = header macros); `typedef struct { char c; v90m_align u; } v90m_align_probe;` for `V90M_ALIGN`.

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Verify.** `make verify` (V.42 golden, `test_api`, every V.42 test unchanged); `make check-version` → `version: 0.5.0 everywhere`.
- [ ] **Step 3: Commit** `api, v42: field lists generated once, one bounded compression writer, ops table, after() per role, one version source`.

### Task 29: `orig_t` engines in a union; new memory pins; Phase 2 gate (call F1; D17)

**Files:**
- Modify: `modem/orig.{c,h}`, `modem/v22bis.{c,h}` (remove `total`), `include/v90modem.h:217-220`, `test/api/test_api.c:181-192`, `README.md:234-238`, `docs/api.md:51`, `docs/limits.md`, `CHANGELOG.md`

**Interfaces:**
- Produces: `union { struct { v22bis_t v22; v32_t v32; } leg; v34orig_t v34; v90a_t v90; } e;` in `orig_t` (Annex A uses `e.leg.v22` and `e.leg.v32` together); `orig_retrains`/`orig_rtd` read only the active engine; `V90M_MEM_SIZE` per D17.

- [ ] **Step 1: Failing test.** In `test_api`: `CHECK(v90m_mem_size() <= 118824 - 12000)` on x86-64 (`<= 118512 - 12000` on 32-bit ARM) → fails today.
- [ ] **Step 2: Implement** the union (every `start_*` already calls its init; `orig_init` memsets all).
- [ ] **Step 3: New pins.** Read `v90m_mem_size()` on every port target (`tools/check-ports.sh` with a printing line filtered by TIMING, or `test_api` built per target); set the exact x86-64 and ARMv5 pins; `V90M_MEM_SIZE` = smallest multiple of 4096 ≥ the largest + 2048; update README, docs/api.md, docs/limits.md, CHANGELOG (sizes changed, rebuild).
- [ ] **Step 4: Verify and Phase 2 gate.** `make verify` (golden unchanged); `make verify-phase` → `VERIFY-PHASE PASS` (every target's `CHECK(v90m_mem_size() <= V90M_MEM_SIZE)` passes); costs "after Phase 2" recorded (each within 0.5 M of after Phase 1).
- [ ] **Step 5: Commit** `orig: the engines after V.8 share memory; v90m_mem_size about 12.5 KB smaller`.

---

## Phase 3: CPU (bit-identical)

Each task: before editing, `make cost-x2 cost-k56 cost-v34 cost-v90c cost-v92c ARM_CC=$A5` (the ones the task names) into `build/cost-before.txt`; after, the same into `build/cost-after.txt`; both rows into `docs/history/audit-0.6.md`. Exactness tests that need the old expression keep it inside the test. A new test program's golden file is recorded in a commit **before** the optimisation commit, so the optimisation commit changes no golden file.

### Task 30: S detector and qamrx divisions that are shifts; tap update (dsp P1, P4, P7, P15)

**Files:**
- Modify: `modem/v34train.{c,h}:67,89-115`, `modem/qamrx.c:147,194,215,222`
- Test: `test/test_exact.c` (new)

**Interfaces:**
- Produces: `sdet_t.div` stays (8 or 16, `v34train.c:67`; `v34dx.c:97,469,632`; `x2d.c:76`; `k56d.c:59`); `thr = d->div == 16 ? x / 16 : x / 8` (constant divisors: the compiler emits shifts, fully defined C); `thr` computed only when `pw > 400`; in `QRX_DENSE` the divisions by `dn`/`dnorm` (= 2^25) written as `/ ((int64_t)1 << 25)`; tap update two taps a pass.

- [ ] **Step 1: Test first** (commit it alone): `test_exact` prints and `CHECK`s `x / v == x / (int64_t)C` paths over 10^6 random values in each expression's range plus edges (0, ±1, ±(2^25 - 1), ±2^25, the largest `num * 65536` and `g * 4096` the code can form); commit `test: exactness of the sdet and qamrx divisions`.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify.** `make verify`; `make bench-x2 bench-v90 bench-k56 bench-v34 ARM_CC=$A5`; costs: expected x2 -2.5 M (sdet) -1.1 M (qamrx) -0.5 M (P7) -0.3 M (P15); V.90 -2.75 M -1.1 M.
- [ ] **Step 4: Commit** `v34train, qamrx: power-of-two divisions as constants; sdet threshold only when needed`.

### Task 31: The V.34 non-linear decoder (dsp P2, P9)

**Files:**
- Create: `modem/compiler.h`, `modem/fixdiv.h`, `v34/test/test_nl_exact.c`
- Modify: `v34/v34_nl.c:15,28-50`, `v34/Makefile`

**Interfaces:**
- Produces: `compiler.h`: `V90M_NOINLINE` (`__attribute__((noinline))` for GCC/clang, `__declspec(noinline)` for MSVC, empty otherwise); `static inline int V90M_CLZ32(uint32_t x)` (x ≠ 0; `__builtin_clz` under `__GNUC__` unless `V90M_NO_BUILTINS`, else a loop). `fixdiv.h`: `static inline uint32_t divlu(uint32_t u1, uint32_t u0, uint32_t v)` (u1 < v, Hacker's Delight, as in `build/scratch-dsp/prof/proto.diff` but through `V90M_CLZ32`), `static inline int64_t div_s64_u32(int64_t n, uint32_t d)`. `v34_nl_decode`: reciprocal of `energy_q8` per call (`divrcp_rcp`/`divrcp_sdiv`, `modem/divrcp.h`); stop when Phi repeats (2-cycles still run 8 rounds); `(ep<<28)/((phi*phi)>>4)` via `divlu` (ep < 2^32, (phi²)>>4 ≥ 2^28); the final `x*131072/phi` as `2(a/phi) + carry`, a = |x|·2^16. `v34_nl_phi_q16` and the encode side use the per-call reciprocal. No new struct field.

- [ ] **Step 1: Test first** (commit alone): `test_nl_exact` keeps the 0.5.0 `v34_nl_decode`/`phi_q16` as `ref_*` and compares with the library over every symbol rate × rate × expanded `v34_params_t`, a grid over the int16 plane (step 256 in the suite, step 1 with `full`) and 20000 random points near the constellation each (200000 with `full`); `divlu` and `div_s64_u32` vs `/` over 10^7 random cases plus edges, built twice (with and without `-DV90M_NO_BUILTINS`). 0 differences. dsp's full run: 90 767 560 inputs.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify.** `make verify`; `test_nl_exact full` once → `0 differences`; costs: expected x2 about -9.7 M (`v34_nl_decode` 16.0 → 6.3 M) and -0.55 M (encode).
- [ ] **Step 4: Commit** `v34_nl: the non-linear decoder without most 64-bit divisions`.

### Task 32: Viterbi add-compare-select; slicer in one pass (dsp P3, P3b, P6)

**Files:**
- Modify: `v34/v34_rx.c:505-533,578`
- Test: `v34/test/test_vit_hash.c` (new)

**Interfaces:**
- Consumes: `V90M_NOINLINE` (Task 31).
- Produces: `static V90M_NOINLINE uint32_t acs(const uint32_t *pm, const uint32_t *bm, const uint8_t *pp, unsigned ns, unsigned nk, uint32_t *npm, uint8_t *prev, uint8_t *lab, unsigned *bsp)` (proto.diff shape: winner position only, the same strict `<` and order, 4 predecessors a pass; labels mapped through `bpair[]` after); the 8 `v34_slice_subset` calls per 2D symbol as one pass (a = re+im, b = re-im once; 2 offsets × 4 classes of the 4Z² grid).

- [ ] **Step 1: Test first** (commit alone): `test_vit_hash` feeds 10^6 noisy symbols (fixed LCG, 3 noise levels) at 16, 32 and 64 states, prints a hash of the decisions; and compares the one-pass slicer with 8 `v34_slice_subset` calls over the int16 grid at step 8.
- [ ] **Step 2: Implement ACS and slicer.** Verify, commit.
- [ ] **Step 3: P3b** (fold the renormalisation into the next ACS, `bm` with Y0 folded): implement; keep only if `make verify` and `test_vit_hash` are unchanged; else revert and record "P3b skipped: <reason>" in the audit doc.
- [ ] **Step 4: Verify.** `make verify`; costs: expected x2 -8.2 M (ACS, 8.75 instructions per branch), -1.0..1.3 M (slicer), -1.0..1.5 M (P3b); V.90 -0.4 M.
- [ ] **Step 5: Commit** `v34_rx: add-compare-select in registers; the subset slicer in one pass`.

### Task 33: Interpolators and equaliser loops (dsp P5, P10, P14, 3.6; call F6, P3)

**Files:**
- Create: `modem/interp2.h`
- Modify: `modem/qamrx.c:81,102`, `modem/pcmtx.c:61-62`, `modem/v90arx.c` (`produce`)

**Interfaces:**
- Produces: `static inline void interp2_32(const int16_t *ring, int idx, int mask, const int16_t *h0, const int16_t *h1, int n, int32_t *acc0, int32_t *acc1)` and `interp2_64` (int64 accumulators, for the user that has them today): newest first, two straight runs, no masking, same products in the same order, two taps a pass. `eq_out` unrolled by 4 (`QRX_NTAP` = 72). A test asserts the int32 bound: the largest row sum |h| of `v34_interp` (46728) × 32768 < 2^31.

- [ ] **Step 1: Implement** (the existing goldens cover these paths: no new test needed beyond the bound check).
- [ ] **Step 2: Verify.** `make verify`; costs: expected x2 and V.90 -2.3 M (produce + eq_out 12.13 → 9.82 M), -0.6 M (P10); V.92c -0.5 M (pcmtx).
- [ ] **Step 3: Commit** `qamrx, pcmtx, v90arx: one interpolator in straight runs; equaliser output unrolled`.

### Task 34: Fused LMS update and next dot product; the caller's divisions (call P1, P5, P6)

**Files:**
- Modify: `modem/v90arx.c:182,697` (`BUPD`, `FUPD`, `estimate`'s `upd_sb` + `dot_sb`), `modem/qamrx.c` (only as Step 3 says)

**Interfaces:**
- Produces: in `v90arx`, each tap update also accumulates the next symbol's dot product (`b[j] += upd(dl_k[j]); acc_next += b[j] * dl_k[j-1]`, j ≥ 1; the j = 0 term added once decision k is pushed); a "precomputed for m" marker, else the plain dot product (hold, gate, a design that replaces the taps). `estimate`: TRAIN `pd = L·L` through `divrcp`, DD nested division `div(div(e·L·2^24, 128), pd)` with a 32-bit path when `|e·L·2^17| < 2^31` and `pd < 2^31`. `neq % rc` as `neq & (rc - 1)` (rc = 2048 >> 0..2; `neq` is uint32 since Task 12).

- [ ] **Step 1: Implement in v90arx.** Accumulators are int64 sums of products bounded by 2^46 (b, f) or 2^47 (h), at most 128 terms: reordering is exact.
- [ ] **Step 2: Verify.** `make verify`; `make bench-v90c bench-v92c bench-v90a ARM_CC=$A5`; costs: expected V.92c -4..-6 M (P1), about -1 M (P5), -0.15 M (P6).
- [ ] **Step 3: qamrx.** Apply the same fusion to `qamrx_update` + the next `eq_out` only if it needs no new field in `qamrx_t` (it is in `answer_t` outside the union: pins) and saves ≥ 0.3 M on cost-x2; else record "qamrx fusion skipped: <reason>".
- [ ] **Step 4: Commit** `v90arx: each LMS update computes the next dot product; the estimate's divisions cheaper`.

### Task 35: Small exact items (ans P1, P3; dsp P8, P11, P12, P13; call P4 trim, P8)

**Files:**
- Create: `v90/v90_pcm_tab.c` (generated by `tools/gen_pcm_lin.py`), `test/test_small_exact.c`
- Modify: `modem/x2map.{c,h}:86-87`, `modem/x2tx.c:201,213,232,270,282`, `v90/v90_pcm.c:49`, `v90/v90_enc.c:61-68`, `modem/ec.c:28-32`, `v90/v90_scr.c` (GPC/GPA byte-wise), `v92/v92_enc.c` (`v92_enc_init` trims trailing zero coefficients), `modem/v34up.c` (P8, conditional)

**Interfaces:**
- Produces: `x2map_frame` switches to 32-bit `/` and `%` once `x >> 32 == 0`; `x2map_t` caches `x2map_bits`/`x2map_list` per bank and level (x2map_t is in the union: pins unchanged, checked); `v90_octet_linear` from a `const int16_t [2][256]` table; `v90_modulus_encode` by a 32-bit reciprocal per Mi taken at map time (`m = ceil(2^32/Mi)`, `(cur·m) >> 32`, cur < Mi·2^16 ≤ 2^23); `ec.c` four taps a pass; scramblers byte-wise by table; P8 reciprocal of the gain in `v34up` only if `v90m_mem_size()` is unchanged on x86-64 and ARMv5, else "P8 skipped: memory" recorded.

- [ ] **Step 1: Test first** (commit alone): `test_small_exact` checks the octet table against the formula for all 256 octets × both laws (Review Focus 5); the reciprocal for every Mi in 1..128 over cur edges (0, 1, Mi·2^16 - 1) and 10^5 random cur each (`full`: every cur, once per phase); the byte-wise scramblers vs bit-serial over 10^6 random bytes from 100 random states; the x2map 32-bit path vs 64-bit over 10^6 random frames per bank.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify.** `make verify` (A-law cases included); `test_small_exact full` once → 0 differences; costs: expected x2 -0.8 M (x2map) -0.4 M (octets), V.90 -1.0 M (modulus) -0.3 M, all -0.4 M (ec), V.92c -0.5 M (descrambler).
- [ ] **Step 4: Commit** `x2map, v90_pcm, v90_enc, ec, scramblers: exact cheaper forms`.

### Task 36: V.42: per-bit path and table-driven FCS (v42 perf 0, 1)

**Files:**
- Create: `rig/bench_v42.c` (from `build/scratch-v42-api/bench42.c`), `v42/test/test_fcs.c`
- Modify: `v42/v42_hdlc.{c,h}:124-144,326-334`, `v42/v42.c:337` and `v42ec_rx_bit`, `Makefile` (`cost-v42`), `v42/Makefile`

**Interfaces:**
- Produces: `v42hdlc_tx_need_frame`, `v42hdlc_tx_bit`, `v42hdlc_rx_bit` as `static inline` in `v42_hdlc.h`; `orig_wait`/`stopped` tests off the common path of `hdlc_bit`; FCS16 and FCS32 byte-wise (`const uint16_t [256]`, `const uint32_t [256]`); no new field in `v42ec_t` (`V90M_EC_MEM_SIZE`, 49792). `make cost-v42 ARM_CC=$A5`: per endpoint per second at 56 kbit/s both ways for none / V.42bis / V.44.

- [ ] **Step 1: Test first** (commit alone): `test_fcs` compares table and bitwise FCS16/FCS32 over every octet from 10^5 random states, and the existing FCS residue tests; `cost-v42` baseline: v42 measured about 7.9 M / 11.6 M / 12.2 M.
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Verify.** `make verify` (V.42 golden unchanged); `make cost-v42 ARM_CC=$A5` (expected 20-30 % less framing cost).
- [ ] **Step 4: Commit** `v42: the per-bit HDLC path inline; FCS by table`.

### Task 37: x2 with V.42 measured; the two-calls verdict; Phase 3 gate (D18)

**Files:**
- Modify: `tools/rec_x2.c` (argument `v42bis` | `v44`: the client runs a `v42ec_t` writing the sentence text both ways), `rig/bench_v90.c` (`-e none|v42bis|v44`: our `v42ec_t` on getbit/putbit), `Makefile` (`cost-x2-v42`), `docs/history/audit-0.6.md`

- [ ] **Step 1: Implement** `cost-x2-v42`: seconds 15 to 16 as `cost-x2` counts, for none, V.42bis (2048/32) and V.44 (2048/255/4096), at the rate `cost-x2` uses and at 56000.
- [ ] **Step 2: Measure** `make cost-x2-v42 cost-x2 ARM_CC=$A5`. Write in the audit doc: the six figures, the x2 figure alone (target ≤ 55-60 M), and the verdict line `two x2 calls with V.42bis per box: yes|no (2 × N M = M of about 122 M)`, yes only if 2 × the worse V.42bis figure ≤ 110 M.
- [ ] **Step 3: Phase 3 gate.** `make verify-phase` → `VERIFY-PHASE PASS`; the "after Phase 3" cost table (all modes) against the baseline.
- [ ] **Step 4: Commit** `bench: x2 data mode with V.42 counted; Phase 3 figures`.

---

## Phase 4: 0.6.0 prepared

### Task 38: Docs, CHANGELOG, version 0.6.0

**Files:**
- Modify: `Makefile:37` (`VERSION := 0.6.0`), `include/v90modem.h` (version macros 0.6.0), `CHANGELOG.md`, `README.md` ("Memory and CPU"), `docs/limits.md` ("CPU", bounds, clocks), `docs/api.md`, `docs/answering.md`, `docs/calling.md`, `docs/error-control.md`, `docs/testing.md`, `DESIGN.md` (§2 layers, §4.3), `tools/README.md`

- [ ] **Step 1: Write.** CHANGELOG `## 0.6.0 (<date of this commit>)`: what changed for a program (end codes, callbacks, NULL rule, ranges, SONAME `.so.0.6`, memory sizes, LAPM watchdog, K56 heavy upstream, bounds), what has been shown (replay proof, port matrix), rebuild note. README/limits.md: the Phase 3 figures of `docs/history/audit-0.6.md` (data-mode M/s per mode, heaviest ticks where measured), "per box" per Task 37's verdict, memory per Task 29. docs/api.md: v42 B11 mismatches (`echo_delay` covers K56 and x2, `rtd`, calling `echo_delay` "-1 taken as 0, more as 511", `V90M_ENOTSUP` cases). docs/calling.md: calling hosts run V.42 (CM offers LAPM, call B7); which end codes a far-end hang-up gives (call F8). docs/answering.md: `V8A_RES_NO_CJ` proceeds (ans B4). docs/error-control.md: V.14 queues hold 511 octets (v42 B9). docs/testing.md: `make verify`, `make verify-phase`, golden files and the D20 rule, `tools/replay-proof.sh`, split programs, CI. DESIGN.md: v34up, ans_scheme, v90a files; the two's-complement / arithmetic-right-shift assumption (v42 §6).
- [ ] **Step 2: Verify.** `make check-version` → `version: 0.6.0 everywhere`; `make check-soname` → `soname: libv90modem.so.0.6`; `grep -rn "0\.5\.0" include/v90modem.h Makefile` → nothing; `make verify` (re-record only goldens that print the version, listed).
- [ ] **Step 3: Commit** `0.6.0: docs, figures and changelog`.

### Task 39: Release proof

**Files:**
- Modify: `docs/history/audit-0.6.md`

- [ ] **Step 1: From a clean tree.** `rm -rf build/release && mkdir -p build/release && git archive HEAD | tar -x -C build/release && make -C build/release -j16 all check PAR=16` → `ALL HOST CHECKS PASS`.
- [ ] **Step 2: Full proof.** `make verify` → `VERIFY PASS`; `make verify-phase` → `VERIFY-PHASE PASS`; `tools/replay-proof.sh diff v0.5.0`: every differing run explained by a CHANGELOG entry (list); `make install PREFIX=build/inst` and the pkg-config example builds and runs.
- [ ] **Step 3: Record** the final table (memory, CPU per mode before/after, verify times, replay differences) in the audit doc.
- [ ] **Step 4: Commit** `docs: 0.6.0 release proof`. Do not tag or push.
