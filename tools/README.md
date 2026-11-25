# tools

| | |
|---|---|
| `v90m_replay.c` (`make replay`, installed as `v90m-replay`) | plays a recorded call into the answering modem and reports what it made of it |
| `replay-proof.sh` (`make replay-proof`) | the private recordings (`RECS`, default `/home/georgica/sync-build/live-050`, 106 of them, never in the repository) through `v90m-replay` in six flag sets (`""`, `-9`, `-x`, `-x -9`, `-k`, `-k -9`): trace, decided bits (`-b FILE`) and exit code of each run hashed against `test/golden/replay.sha256`; `record` writes it, `diff REF` compares this tree's outputs with the tool built from git REF's; `V90M_NO_RECS=1` skips |
| `verify.sh` (`make verify`) | per commit, seven stages at once, one line each: host `check`, `golden.sh check` and `split-check.sh`, `replay-proof.sh check`, `bench-hashes.sh check`, `check-ubsan`, the armv7hf short suite (less `ports-slow.txt`) and the same for aarch64be (big-endian) against the host's `-DTEST_FORCE_SHORT` set (`short-cmp.sh`), `check-fixed` and `check-symbols`; then `VERIFY PASS (S s)`; logs in `build/verify/log` |
| `verify-phase.sh` (`make verify-phase RECS=...`) | the release proof (hours; `docs/history/audit-0.6.md`): armv5 `check-arm`, `check-ports.sh` over every target (whole suites on host gcc, armv5, s390x, riscv64, QUICK on the rest; every target gates, m68k included), the armv7hf short suite in full, the benches' hashes host against qemu, the `cost-*` counts into `build/costs.txt`, `crd-scan`; then `VERIFY-PHASE PASS` |
| `golden.sh check\|record BUILD` (`make golden`, `make golden-record`) | every test program's output (`make check PAR=N`'s logs, filtered by `out-filter.sh`) against `test/golden/out/<dir>/<program>.txt`; a golden file is re-recorded only when a change explains it, and the commit says which and why (`docs/testing.md`) |
| `bench-hashes.sh check\|record` (`make bench-hashes`) | the benches' transmit and received-bit hashes on the host against `test/golden/bench.txt` |
| `out-filter.sh` | a test program's output without the lines that measure the host (CPU and wall time, structure sizes): what `golden.sh` and `check-ports.sh` compare |
| `short-cmp.sh HOSTBUILD TARGETBUILD [SKIPFILE]` | a target's `check-arm` outputs against the host short set's, program by program (a split program's cases joined); runs the host short set itself when HOSTBUILD has none (CI); a program the target did not run fails unless SKIPFILE lists it |
| `check-fixed.sh` (`make check-fixed`) | each library object's undefined symbols against an allowlist (integer helpers, mem*): no soft-float helper, no libm, malloc/free only in `api/v90modem.o` |
| `split-check.sh [BUILD]` (in `make verify`) | each program of `SPLIT_TESTS` (run case by case on ARM: `prog --cases`, `prog N`): its cases one after another print what the whole program prints |
| `v90m_fields.c` (`make build/v90m-fields`, not installed) | what the caller of a recorded call sent, field by field: the answering modem run as `v90m-replay` runs it, the library's V.21 and Phase 2 receivers on the caller's signal, the caller's level per state; each sequence's bits rebuilt with the caller's builders and compared (`docs/history/PROGRESS.md`, "realcheck") |
| `rec_orig.c` (`make build/rec_orig`, host only) | a simulated call of the library's caller recorded at the answerer in the format above, the answerer set up as `v90m-replay` sets it up: `v90m-fields` reads it as it reads a real one; `rec_orig v34 OUT ec` with V.42 on both sides, for `v90m-replay -F` (`test/fixtures/replay-opts.sh`) |
| `trace.c`, `trace.h` | the replay's report: the trace hooks of the modem (built with `-DP34D_TRACE`) and the lines printed per phase; v90d's server replay prints the same |
| `rec_v90.c`, `rec_v34.c` | record a simulated call (`make bench-v90`, `make bench-v34`) |
| `ber_v90.c` | bit error rate of V.90 over the simulated line |
| `qprof.c` (`make prof-v90c`, `make prof-v92c`, host only) | ARM instructions per tick and per function, from qemu's log of a bench run with a mark before every tick (`rig/bench_v90c -t`) |
| `capinfo.py`, `spec.py`, `hdlc.py` | a recording's level per 200 ms; its strongest spectral lines; the HDLC frames in a bit dump |
| `gen_*.py` | generators of the constant tables in `modem/` |
| `gen_x2_banks.py` | `modem/x2bank.c`, x2's level banks, from D-Modem's published values (its `dsp/x2_tables.cpp`, kept outside the repository; the numbers only, `docs/limits.md` "Included tables (x2)") |
| `x2_pad.c`, `x2_pad.h` (host only, tested by `build/test_x2_pad`) | x2's pad octets: the octet the server sends so that the client receives a given level behind a 3 or 6 dB digital pad (`docs/history/x2-facts.md`, "Pads") |
| `k56enc_dump.c` (`make k56-dump`, host only) | K56flex's data encoder on the oracle's pipeline at every rate, pad and robbed mask: `build/k56dump/r<R>p<PAD>m<MASK>.{oct,bits}` for SM56's decoder harness outside the repository (`docs/history/k56-facts.md`, "Oracle") |

## The call recording format

A recording holds, for every 10 ms (80 samples) of a call, what the codec
received from the line and what it sent, in that order:

- **companded**: 80 octets heard, then 80 octets sent: G.711 codewords
  (mu-law or A-law, as the line was). 160 octets per block.
- **linear receive**: the file starts with the 8 octets `V90DLIN1`; then per
  block 80 heard samples as little-endian signed 16-bit values on the scale
  of V.90 Table 1 (what `v90m_g711_decode` gives for a codeword), then 80
  codewords sent. 240 octets per block.

The recording starts before the call is answered (v90d records from
off-hook); the replay finds the answer tone in what was sent (the first
sound after 200 ms of silence) and starts the modem so that its own answer
tone begins at that moment.

A recording that starts at the answer tone, with no silence before it (the
replay's search needs 200 ms of it), is played with `-s SAMPLE`: the modem
starts at that sample (0-based, in samples of the heard stream) and the
times in the report stay the recording's (with `-k`, the alignment after
V.8bis is skipped). `-l` takes decimals (`-14.4`), to 0.01 dB; a bad value
for either exits 2 with the usage message.

For the replay to follow the call, the modem must be set up as the recording
modem was (level, modulations offered, rate ceilings, V.92): what it hears
contains the echo of what that modem sent. The replay's last line counts the
samples where its transmit differs from the recording's; 0 until data mode
(where the data were the recording modem's own) means it was set up right.
