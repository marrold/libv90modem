# libv90modem: progress

What was done and measured, newest last, with the test output. Earlier
history (the answering modem) is in v90d's PROGRESS.md.

## Phase 2, the calling side: what is shared and what is new (2026-11-17)

Before any code. The library starts at "the far end has answered"
(dialling, DTMF and call progress stay outside).

### The test peers in sim/: what they skip

They test the answerer and are a reference for behaviour, not code to ship.

- `v8call`: 2100 Hz energy only (ANSam and ANS look the same), floating
  point, no time-outs, no CI, Te fixed at 1 s.
- `callpeer`: the library's own `v22m` in the caller role, so not
  independent of the answerer; V.21 without carrier detection.
- `p2c` (V.34 Phase 2, caller): integer, built from the library's blocks;
  but "the test decides what INFO1c says", the rate is a rough SNR formula,
  it never starts a retrain, INFO0a's capabilities are ignored.
- `v34peer`: the library's own `v34dx` in the call role; at most 3 blind
  retrains.
- `v32call`: independent of `v32/` and the most complete (echo canceller,
  timing and carrier recovery, equaliser, Viterbi), but double precision,
  no pre-start (sends AA at once), canceller frozen after TRN, AGC set once,
  no rate renegotiation, rough retrain trigger, one 12 s time-out.
- `p2a` (V.90 Phase 2, analogue): integer; INFO1a fixed (down SR 4, rate 12),
  the probe result unused, retrain assumes the first INFO0d.
- `p34a`, `v92a`, `v92call` (V.90 / V.92 analogue): downstream they read the
  digital modem's octets as sent (`p34a.h:6-9`, `v92a.h:11-12`), with 12 / 24
  octets of look-ahead; no receiver at all (no timing recovery, equaliser or
  canceller); DIL, CP and CPu fixed patterns; upstream in double.
- `v90_dec`: exact octets in, -1 on any Ucode outside the constellation.
- Channels: `channel` and `v92chan` have matched clocks and a known echo;
  `v92chan` does not model the downstream at all; `v32line` has real
  separate clocks but exports the exact delays to the tests.

### Shared with the answering side

| stage | reused as is | reused with a role parameter |
|---|---|---|
| C1 V.8 | `v8_encode/decode/frame/selected`, `v8rx`, `async` | `v21` (high-channel band-pass table needed, `v21_bpf.h` is low channel only) |
| C2 V.21, V.22, V.22bis | `v22m` with `V22_CALLER` (carrier swap, no guard tone, USB1 / 456 ms / S1, retrain), `v22tx/rx`, `scramble`, `v14` | V.21 data mode (channels swapped) |
| C3 V.32bis | `v32_code`, `v32_viterbi`, `v32_ec`, `v32_tabs`; `v32tx` already sends AA and CC | `v32_init` (passes `answer=1` today), `v32rx` (descrambler takes the role; AC/CA detection new) |
| C4 V.34 | `v34dx` with `answer=0` (Phases 3/4, MP, data, renegotiation, cleardown: already used by `v34ans_init_call`), `v34mp`, `v34mod`, `qamrx`, `ec`, `v34train`, `v34/`, `probe`, `dpsk`, `info0a_*` | `v34ans` (retrain loop for the caller), `info1d_build` used as INFO1c |
| C5 V.90 analogue | `info0a/1a_build`, `probe`, `dpsk` (Phase 2); `v34mod`, `v34/` (V.34 upstream); `v90_seq` (Jd, DIL, CP, MP formats), `v90_scr`, `v90_enc` (map from CP); `v90tx` as a reference generator for training | `v90_dec` promoted from the tests: fixed integer already, needs error tolerance |
| C6 V.92 analogue | `v92_enc` (the analogue transmitter, bit-exact, fixed point), `v92_mod`, `v92_map`, `v92_sig`, `v92_seq` (Ja, CPu, SUVu; CPd, SUVd, Jp parsing), `v92_info` | |

### New

- C1: a fixed-point answer-tone detector telling ANSam (15 Hz AM) from ANS;
  `v8orig` (optional CI, Te, CM until two JM, CJ, 75 ms, time-outs); the
  choice from JM; fallbacks without V.8 (V.22bis on USB1, V.32bis AA).
- C2: originate sequencing after V.8 and without it; V.21 carrier detection.
- C3: the V.32bis caller's start-up (AA, AC/CA reversal, CC, round trip,
  S / TRN / R1, R2, R3 / E), retrain and rate renegotiation from the caller's
  side, cleardown.
- C4: `p2call`, the caller's Phase 2 (from `sim/p2c`, with a real INFO1c
  decision from the probe, modelled on `p2ans`), the caller's retrain and
  recovery (V.34 11.5, 11.2.2.1.5-6), the `v34orig` sequencer.
- C5 (the bulk of Phase 2): the PCM downstream receiver (timing locked to the
  network's 8 kHz clock, AGC, long feedforward and feedback equaliser, an
  echo canceller for our own upstream, slicing to Ucodes); DIL design and
  DIL analysis (per-slot levels, robbed bits, digital pads, the law); the
  choice of constellations and rate for CP (and CPt, LD, a/b); training
  detection on noisy decisions; the V.34 upstream sequencer for V.90's
  Ja / CPt / CP (not `v34dx`'s MP); upstream timing from the receive clock.
- C6: the V.92 upstream sequencer (Ru, TRN1u, Ja, Su with the Jp slip,
  TRN1u, CPt, E1u, TRN2u, SUVu, CPu, E2u, B1u); a fixed-point fractional
  delay transmit filter for the Jp slip; CPu design (the analogue side's).
- Tests: a downstream channel model for C5 / C6 (codec reconstruction
  filter, loop, clock offset, robbed bits, pads), which no peer has today;
  calling-side versions of the spandsp interop tests (V.8, V.22bis).
- API: `V90M_ROLE_ORIGINATE` accepted stage by stage (the modulations not
  yet written refused with `V90M_ENOTSUP`); end reasons for the caller (no
  answer tone, no JM, no common mode); the V.42 endpoint's role from the
  modem's; rate ceilings defined as transmit / receive for both roles.

## C1: V.8 on the calling side (2026-11-17)

- `modem/anstone`: the calling modem's answer tone detector, fixed point.
  2100 Hz brought to baseband in blocks of 5 ms; a block is tone when its
  2100 Hz component holds half of its power and is above -43 dBm0 (three
  quarters of it, for ANSam's troughs); the 15 Hz part of the envelope over
  windows of 200 ms (three periods): above 0.12 of the mean ANSam (V.8 7.2:
  0.2), below 0.06 ANS (V.25); two windows that agree decide. A block where
  the phase reversed is bridged with the one before it.
- `modem/v8orig`: V.8 8.1 for the call DCE: optional CI (ON 3 sequences,
  OFF 1 s; the tone is listened for in the OFF periods), ANS -> no CM (7.2),
  ANSam -> Te (1 s by default, 0.5 s at least) -> CM on V.21(L) until two
  identical JM on V.21(H) -> CJ at the next octet boundary -> 75 ms.
  Results: OK, ANS, no tone (10 s), no JM (5 s of CM), no common mode.
- Each test was seen failing first against a stub (anstone: 35 of 45
  checks failed; v8orig: 40 of 53).

Test output (`test/test_anstone.c`, `test/test_v8orig.c`, `test/interop_v8orig_spandsp.c`):

```
ANSam, the library's own transmitter (modem/ansam)
  peak 5602, reversals 0: ANSam at 0.400 s
  peak 5602, reversals 1: ANSam at 0.400 s
  peak  223, reversals 0: ANSam at 0.400 s
  peak  223, reversals 1: ANSam at 0.400 s
ANSam and ANS, written here from V.8 7.2 and V.25
  ANSam -12 dBm0, reversals                                  ANSam at 0.400 s
  ANSam -12 dBm0, no reversals                               ANSam at 0.400 s
  ANSam -42 dBm0                                             ANSam at 0.400 s
  ANSam 0 dBm0                                               ANSam at 0.400 s
  ANSam 10 Hz high (a carrier shift on the line)             ANSam at 0.400 s
  ANSam 10 Hz low                                            ANSam at 0.400 s
  ANSam -30 dBm0 in noise 12 dB below                        ANSam at 0.400 s
  ANSam at the narrowest envelope V.8 allows (0.81..1.19)    ANSam at 0.400 s
  ANSam starting off the block grid                          ANSam at 0.398 s
  ANS -12 dBm0, reversals (V.25)                             ANS at 0.400 s
  ANS -12 dBm0, no reversals                                 ANS at 0.400 s
  ANS -40 dBm0                                               ANS at 0.400 s
  ANS -30 dBm0 in noise 12 dB below                          ANS at 0.400 s
  ANS with a level step and a phase jump (V.8 7.2 NOTE 1)    ANS at 0.800 s
  ANSam with the same transient                              ANSam at 0.400 s
ANS is never taken for ANSam, wherever its reversals fall
  24 starting offsets: 0 not ANS
not an answer tone
  ANSam at -50 dBm0, below the floor                         nothing at -1.000 s
  1800 Hz with the same modulation                           nothing at -1.000 s
  2250 Hz (a V.22bis answerer's unscrambled ones)            nothing at -1.000 s
  white noise at -20 dBm0                                    nothing at -1.000 s
  dial tone 350 + 440 Hz: nothing; 500 + 1300 + some 2100 Hz: nothing
anstone: 45 checks, 0 failures
V.8 calling side against the library's answering side
  clean line                                   result 1, tone ANSam at 0.910 s, CM from 1.910 s, CJ at 3.177 s, done 3.277 s; selected 3; answerer: result 1, selected 3
  10 dB loss, echoes, noise, 2 Hz, 100 ppm     result 1, tone ANSam at 0.930 s, CM from 1.930 s, CJ at 3.230 s, done 3.330 s; selected 3; answerer: result 1, selected 3
  the same with CI, the tone 2.5 s late        result 1, tone ANSam at 3.130 s, CM from 4.130 s, CJ at 5.430 s, done 5.530 s; selected 3; answerer: result 1, selected 3
PCM: a V.90 / V.92 analogue caller to a digital answerer
  result 1, selected 100 (V.90 / V.92 is 100), answerer selected 100
nothing in common: JM with no modulation mode (8.1.2)
  result 5, answerer result 1
a pre-V.8 answerer: ANS (V.25), so no CM (V.8 7.2)
  result 2, tone ANS at 0.730 s, our transmit before and at it 0
ANSam but never a JM
  result 4 after 6.730 s; CM from 1.730 s
no answer tone at all
  result 3 after 10.000 s
v8orig: 53 checks, 0 failures
  V.34 V.32 V.22 V.21 both ways: result 1, selected 1, JM modes 0x101a; spandsp: ok 1, our CM modes 0xa06, protocol 1
  V.34 V.32 V.22 V.21 both ways, with CI: result 1, selected 1, JM modes 0x101a; spandsp: ok 1, our CM modes 0xa06, protocol 1
  V.22bis answerer, noise and echo: result 1, selected 4, JM modes 0x0010; spandsp ok 1
  V.90 digital answerer: result 1, selected 100, JM pcm 1 0x02; spandsp ok 1, our pcm 1
  ANS answerer: result 2, tone 1, CM at 0
interop_v8orig_spandsp: 26 checks, 0 failures
```

The answering side of spandsp repeats in JM the modes of the CM it received;
its application is to keep only what it has itself when CM is reported
(V8_STATUS_V8_OFFERED). `test/peer_spandsp.c` does that now; before, a
V.22bis-only spandsp answered our V.34 offer with V.34.

Not tried yet: a real modem answering (that needs a call to go on after V.8:
C2).

## C2: V.21, V.22 and V.22bis on the calling side (2026-11-17)

- `modem/orig`: the calling modem's sequencer, the mirror of `modem/answer`:
  V.8 (`v8orig`) -> JM item 4: V.22bis / V.22 (`v22m` as caller, as it
  was), item 12: V.21 (channel 1 out, channel 2 in through a new band-pass,
  `modem/v21h_bpf.h`: 980 Hz 56 dB down). After ANS (no V.8): the V.22bis
  caller waits for the answerer's unscrambled ones; a steady channel 2 mark
  for 300 ms instead is a V.21 answerer. Ends: no answer tone, no JM, no
  common mode, start-up failed, carrier lost.
- The V.22bis caller's carrier flag rises on a V.21 channel 2 mark; what
  tells V.22bis is its caller leaving V22S_WAIT (155 ms of unscrambled
  ones). Found with the V.21 test.
- The public API takes `V90M_ROLE_ORIGINATE` with V.21 and V.22 / V.22bis;
  any other modulation gives V90M_ENOTSUP. New ends: no answer tone, no
  JM, no common mode; `cfg.send_ci`. The answering path is unchanged:
  `make bench-v90` gives 8e3136e9 six ways.
- Seen failing first: test_orig 39 of 40 checks against a stub.

```
V.8 then V.22bis, against the library's answerer
  clean line: data at 4.74 s, 2400 bit/s, modulation 3, V.8 1
    2400 bit/s: caller got 63 of 63 octets, answerer got 61 of 61
    after a retrain started by the answerer: ours 1, theirs 1; moving 1
    after the retrain: caller got 63 of 63 octets, answerer got 61 of 61
    the answerer silent: phase 4, end 5
  10 dB loss, echoes, noise, 1 Hz, 50 ppm: data at 4.64 s, 2400 bit/s
    2400 bit/s: caller got 63 of 63 octets, answerer got 61 of 61
V.8 then V.21
  data at 3.03 s, 300 bit/s, modulation 1
    300 bit/s: caller got 63 of 63 octets, answerer got 61 of 61
pre-V.8 answerers: ANS, then
  V.22bis: data at 5.11 s, 2400 bit/s, modulation 3, V.8 0
    2400 bit/s: caller got 63 of 63 octets, answerer got 61 of 61
  V.22 (1200 bit/s only): data at 5.56 s, 1200 bit/s, modulation 2
    1200 bit/s: caller got 63 of 63 octets, answerer got 61 of 61
  V.21: data at 3.74 s, modulation 1
    caller got 63 of 63 octets, answerer 61 of 61
no call
  no answer tone: phase 4, end 1
  nothing in common: phase 4, end 3
orig: 40 checks, 0 failures
  spandsp at 2400 bit/s, noise -50 dBFS, echo 0.20: up at 5.11 s, ours 2400 bit/s (modulation 3, V.8 0), spandsp 2400; errors spandsp->us 0 of 8000, us->spandsp 0 of 8000
  spandsp at 1200 bit/s, noise -50 dBFS, echo 0.20: up at 5.45 s, ours 1200 bit/s (modulation 2, V.8 0), spandsp 1200; errors spandsp->us 0 of 7780, us->spandsp 0 of 7784
  spandsp at 2400 bit/s, noise -44 dBFS, echo 0.35: up at 5.11 s, ours 2400 bit/s (modulation 3, V.8 0), spandsp 2400; errors spandsp->us 0 of 8000, us->spandsp 0 of 8000
  finding, not checked:
  spandsp at 2400 bit/s, noise -38 dBFS, echo 0.35: up at 5.53 s, ours 1200 bit/s (modulation 2, V.8 0), spandsp 1200; errors spandsp->us 0 of 7780, us->spandsp 3122 of 6303
interop_orig_v22bis_spandsp: 21 checks, 0 failures
```

Finding, not explained: with noise 20 dB below the signal (-38 dBFS),
spandsp's V.22bis answerer does not take our S1, falls back to 1200 bit/s and
then decodes about half of our bits wrong, while we decode it without error.
In the same channel the library's own V.22bis answerer trains on our caller at
2400 bit/s down to -32 dBFS (S1 SNR 41 dB at -38 dBFS), and spandsp's caller
trains with our answerer at -38 dBFS. Down to -44 dBFS spandsp's answerer and
our caller agree at 2400 bit/s without an error. Not settled whether it is
spandsp's answering receiver or something in our caller that only spandsp
minds; a second spandsp instance as the caller would tell.

Not tried yet: a real modem answering. v90d has a bench mode for it now
(`v90d -o`: a modem told ATA on the port is called at once).

## C3: V.32bis on the calling side (2026-11-17)

- `v32/v32_call.c`: the calling modem of V.32bis 6.1, chosen with
  `v32_init_call` (`v32_init` stays the answerer, its code moved, not
  changed): AA; the answerer's AC and its reversal, found on the part of the
  received signal that changes sign every symbol (our own AA / CC echo has
  none of it); CC 64 T after that reversal at the line (63.3 T measured);
  the second reversal stops the round-trip count (NT within 0.3 T of the
  line's); the answerer's S / S-bar / TRN and R1; S for NT, S 256, S-bar,
  TRN (the canceller's training, extended while it gains); R2 (our rates
  within R1, cut to what the receiver's error on R1 allows; cleardown when
  none); R3 -> E, B1, data. Retrain from either end (7.1); the answering
  side of rate renegotiation (8.2, tested against a test-only initiator:
  the library's answerer has no clause 8); carrier loss also when the
  signal falls 12 dB below its level at the start of data.
- `modem/orig.c`: JM item 3 -> V.32bis; after ANS, Annex A: AA once ANS has
  been heard about 1 s; AC -> V.32bis, the V.22bis answerer's tone -> V.22bis,
  a V.21 mark -> V.21; the 3.1 s wait of A.2.1.2.
- The API takes V90M_MOD_V32BIS for the calling role.
- Written by a subagent from the Recommendation, with `sim/v32call.c` as a
  behaviour reference; reviewed here; the counts below rerun here.

Seen failing first against stubs: test_acdet 44 of 75, test_caller 30 of 45
(and 24 renegotiation checks), test_orig_v32 53 of 90, test_api 1. Now (my
run, fresh build): orig_v32 95 checks, test_acdet 75, test_caller 247, api
124 (98 before: the V.32bis calling block added), 0 failures; every other
line of `make check` identical to C2; bench-v90 8e3136e9 six times;
test_orig_v32 on qemu-arm byte-identical to the host.

Results: clean line 14400, error-free, SNR 43.2 dB; retrains from either end
error-free after about 5.9 s; the impaired line of the C2 tests (about 20 dB
SNR) connects at 9600 (14400 needs about 25 dB; with -60 dBm0 noise and no
carrier shift it connects at 14400); rate ceilings 9600 and 4800 held;
Annex A cases (V.32bis, short ANS, V.22bis, V.21, the 3.1 s wait, an
automode answerer) pass; renegotiation about 213 ms then error-free at 9600.

No independent V.32bis answerer exists to test against (spandsp has no
V.32bis): the caller is checked against the library's answerer only, which
was itself checked against the independent `sim/v32call`.

Found in the answerer, not changed (each would change what it sends):
- its TRN1 can run 8230 symbols, over the 8192 of 5.2.3 (the extension is
  checked every 256 symbols from a minimum that is no multiple of 256);
- with no echo on the line its TRN1 always runs to the maximum (about 3.4 s):
  "less than 1 dB gained" never holds when there is nothing to cancel;
- with a carrier frequency shift neither end's frozen canceller removes the
  shifted far echo (about 17 dB of echo reduction on the impaired line).

Shortcuts: after the 3.1 s wait the V.22bis caller keeps its 456 ms of
silence before S1; AA starts 600 ms after ANS was recognised; R2 is judged
from the receiver's error on R1; the canceller freezes after TRN, as in the
answerer.

## C4: V.34 on the calling side (2026-11-17)

- `modem/p2call`: V.34 Phase 2 for the call modem (11.2.1.1) with the
  recovery rules of 11.2.2.1 (it begins a retrain itself when Tone A does
  not come under its L2; when INFO1a does not come it answers the
  answerer's Tone A as a retrain, or begins one 2 s later), and a real INFO1c
  decision from the probe and INFO0a: a symbol rate only where both ends
  allow it; the carrier by the band SNR per carrier (p2ans's 36 dB floor);
  the pre-emphasis filter that best evens the measured tone levels (if it
  beats none by 0.2 dB); the maximum rate by p2ans's formula, capped by
  Table 8, by 12 without 1664-point support and by our ceiling; power
  reduction from the empty probe bins' distortion. INFO1c (Table 15) has a
  builder and parser of its own in `modem/info.c`.
- `modem/v34orig`: the mirror of `v34ans` (left untouched): p2call, then
  `v34dx` in the call role, data; retrains from either side (70 ms, Phase 2
  without INFO0; at most 4 in a row without data), rate renegotiation,
  cleardown both ways, carrier loss.
- `modem/orig.c`: JM item 1 -> V.34; separate receive rate; rate ceilings,
  echo delay and law in its configuration. The API takes V90M_MOD_V34 for
  the calling role; v90m_cleardown works for a V.34 caller.
- Written by a subagent from the Recommendation with `sim/p2c` and
  `sim/v34peer` as behaviour references; reviewed here; the counts below
  rerun here.

Seen failing first against stubs: p2call 156 of 247, orig_v34 22 of 27,
api 3 of 128. Now (my run, fresh build): p2call 247 checks, orig_v34 366,
api 162 (124 before: V.34 calling cases added; two old checks that expected
V90M_ENOTSUP for V.34 now expect a call), 0 failures; every other line of
`make check` identical to C3; bench-v90 8e3136e9 six times; on qemu-arm the
output of test_p2call, test_orig_v34 and test_api is byte-identical to the
host's.

| line (caller against the library's answerer) | we send | we receive |
|---|---|---|
| good (no codec, 20 ms each way, echoes, noise 50 dB down, +30 ppm) | 31200 | 28800 |
| realistic (mu-law, -100 ppm, +-1 Hz, mild loop) | 24000 | 24000 |
| noise 30 dB down | 21600 | 19200 |
| noise 22 dB down | 12000 | 12000 |
| long loop (our pre-emphasis request helps what we receive) | 12000 | 24000 |
| ceilings 14400 out / 9600 in | 14400 | 9600 |

Retrain from either side: data again after 5.9 s, error-free. Rate
renegotiation from each side (16800, then 14400), error-free. Cleardown about
260 ms from either side. The answerer gone: "lost" after 550 ms in data mode,
"no carrier" after 16 s (one retrain) in training. The round trip measured
by the caller equals the answerer's on the good line.

No independent V.34 answerer exists to test against (spandsp has no V.34).

Findings, not explained or not changed:
- No 33600 anywhere: 31200 is the engine's best in every test; what the
  library's answerer sends tops out at 28800 because its V.34 always sends
  G.711 codec values, even on a linear line.
- With 3 % far echo plus noise and clock offset the answerer receives only
  16800 from our caller; `sim/v34peer` (the answerer's own test caller) gets
  the same, so it is the answerer's receiver.
- INFOMARKSa is not detected; a missing INFO1a falls back to a retrain.
- The INFO1c decision runs in one sample tick: about 1.5 M ARM instructions
  (counted under qemu), once per Phase 2. On the box that is one late block.
- The API counts the caller's V.34 renegotiations; it still does not count
  the answering side's own (as in Phase 1).

## C5-1: the PCM downstream receiver of the analogue modem (2026-11-17)

- `sim/dschan`: the downstream as the analogue modem's A/D sees it: octets
  pulled from the digital modem on the network clock -> optional pad
  (decode, scale, encode) -> optional robbed bit (LSB forced to 1 in one
  sample of 6, at a phase of the test's choosing) -> G.711 levels -> one
  pulse built in the frequency domain (two 8th order Chebyshev codec
  filters at 3550 Hz, 150 Hz transformer, 50 Hz DAA coupling, 0..6 km of
  0.4 mm cable, normalised at 1 kHz) -> noise in dBm0 -> sampled at
  8000 (1 + ppm) Hz from any phase -> plus the hybrid echo of our own
  transmit samples -> 16-bit A/D. Pulled per A/D sample, like the other
  channel models.
- `modem/v90arx`: the receiver, integer only. Echo canceller (`ec_t`),
  interpolator on to the network clock (`v34_interp`), AGC from Sd, DFE
  48 forward / 128 feedback taps, slicer per data frame interval with
  caller-settable levels. Clock offset measured over Sd (its 1333 Hz line
  turns by -2 pi offset per period); channel estimate (128 taps, NLMS) on
  TRN1d; the MMSE DFE computed from it by fixed-point Cholesky (LMS alone
  needed about 3 s to reach -45 dB); then decision directed: the estimate
  goes on, the DFE is computed again every 2048 / 1024 / 512 symbols
  (offset up to 30 / 61 / above ppm), timing loop on the estimate's phase,
  adaptation gated on 4 sigma and on slots 9 dB worse than the best.
- `test/test_v90arx.c` (488 checks), `tools/rec_v90a.c` + `rig/bench_v90a.c`
  (`make bench-v90a`, `make cost-v90a`).

Seen failing first against a stub: 287 checks, 80 failures (the channel's
own checks passed). Now 488 checks, 0 failures, host and qemu-arm output
byte-identical. Every other `make check` line identical; bench-v90
8e3136e9 six times.

Results (2 s of data per cell, 48000 bit/s = 64-point slots, Sr = 0,
points spaced as widely as -12 dBm0 allows):

```
data: Ucode decision error rate (against what the digital modem sent), 16000 symbols each
  channel \ bit/s                   28000    32000    36000    40000    44000    48000    52000    56000
    dmin, mu-law (units)             1280      896      544      384      256      176      128       64
  2 km, -75 dBm0,    0 ppm              0        0        0        0        0        0        0        0
  0 km, -75 dBm0,  +80 ppm              0        0        0        0        0        0  2.6e-01  8.7e-01
  6 km, -75 dBm0,  -80 ppm              0        0        0        0        0        0        0  7.8e-01
  2 km, -65 dBm0,  +30 ppm              0        0        0        0        0        0  9.1e-01  9.5e-01
  4 km, -60 dBm0,  -30 ppm              0        0        0        0  1.6e-03  7.9e-01  9.6e-01  9.6e-01
  2 km, -75 dBm0,  +30 ppm              0        0        0        0        0        0        0  9.4e-01
    as row 6, 3 dB pad                  0        0  2.5e-04  8.9e-01  9.3e-01  9.5e-01  9.5e-01  9.6e-01
    ... levels given (DIL)              0        0        0        0        0  3.2e-02  1.6e-01  9.4e-01
    as row 6, 6 dB pad                  0        0        0        0  8.8e-01  9.3e-01  9.5e-01  9.7e-01
    ... levels given (DIL)              0        0        0        0        0        0        0  9.3e-01
    as row 6, RBS                       0        0        0  7.2e-01  7.9e-01  9.2e-01  5.4e-02  2.9e-01
    ... levels given (DIL)              0        0        0        0  2.3e-02  3.1e-02  5.6e-02  9.5e-01
    as row 6, A-law                     0        0        0        0        0        0  2.6e-03  8.5e-01
  mse (dB re UINFO^2) and rms error (units) at 28000 / 40000 bit/s; rms of |y| - |sent| at 36000
  2 km, -75 dBm0,    0 ppm        -59.7 /  -60.5      6.88 /    6.29       5.88
  0 km, -75 dBm0,  +80 ppm        -52.3 /  -50.8     16.10 /   19.25      14.59
  6 km, -75 dBm0,  -80 ppm        -55.8 /  -55.2     10.73 /   11.52      10.40
  2 km, -65 dBm0,  +30 ppm        -51.1 /  -52.0     18.46 /   16.62      17.54
  4 km, -60 dBm0,  -30 ppm        -46.8 /  -47.1     30.27 /   29.44      29.82
  2 km, -75 dBm0,  +30 ppm        -55.7 /  -56.4     10.85 /   10.01      11.18
    as row 6, 3 dB pad            -40.7 /  -29.0     61.13 /  236.66      86.80
    ... levels given (DIL)        -54.6 /  -54.5     12.36 /   12.56     109.84
    as row 6, 6 dB pad            -44.6 /  -42.8     39.02 /   47.91      51.07
    ... levels given (DIL)        -53.4 /  -53.5     14.29 /   14.10      39.92
    as row 6, RBS                 -39.7 /  -25.9     68.92 /  336.12      60.06
    ... levels given (DIL)        -55.4 /  -55.1     11.25 /   11.73      46.67
    as row 6, A-law               -54.1 /  -56.6     13.32 /   10.02      10.45
    as row 6, RBS: robbed slot 0; per slot at 36000: errors, mean and rms of |y| - |sent| (units)
    [0] 0 -59.6 115.7 [1] 0 +1.6 50.0 [2] 0 +1.4 52.4 [3] 0 +1.1 24.9 [4] 0 +1.7 42.0 [5] 0 +1.0 25.2
    ... levels given (DIL): robbed slot 0; per slot at 36000: errors, mean and rms of |y| - |sent| (units)
    [0] 0 -60.7 111.5 [1] 0 -0.5 11.2 [2] 0 -0.3 11.2 [3] 0 -0.3 11.0 [4] 0 -0.4 11.3 [5] 0 -0.8 11.2
```

Clock offset found within 0.05 ppm from -100 to +100 ppm and any phase;
MSE after TRN1d (4800 T) -60 dB at 0 ppm, -47..-54 dB at 100 ppm; the
canceller (trained 0.5 s on wideband noise, then frozen) gives 69..80 dB
ERLE with our V.34 upstream 8 and 18 dB above the downstream.

Findings:
- The A/D at 8 kHz, not locked to the network: what the line passes above
  about 3.8 kHz aliases with a phase that turns with the offset, a slowly
  changing channel at the band edge. -61 dB at 0 ppm, about -48 dB at 10 to
  100 ppm with LMS tracking alone (an ideal 512-tap interpolator does no
  better); recomputing the DFE often brings 100 ppm back to -51..-58 dB.
  A codec clock locked to the recovered timing would remove it.
- NLMS on a V.34 upstream trains the canceller slowly (34 dB in 0.75 s,
  48 dB in 30 s): the band edges of the reference are weak. Wideband
  training (Phase 2 probing) is assumed.
- TRN1d's signs start far from white (lag-1 correlation 0.55 over the first
  300 symbols): the channel estimate needs about 1000 symbols.
- An odd UINFO is changed by a robbed bit in TRN1d and Sd; the tests use
  UINFO 90 (even codewords already have LSB 1).
- Without the levels, a robbed bit or pad propagates through the feedback
  into the other slots; with the levels given (as the DIL will find them)
  the other slots are clean and the limit is points moved to within dmin/2
  of a neighbour (a CP for the slot avoids those).

Cost (`make cost-v90a`, qemu instruction counts, 2 km, 48000 bit/s, our
V.34 echo 8 dB over the downstream): data mode 68.3 M ARM instructions per
second of line at 0 ppm, 78.5 M at 100 ppm (the difference, 11.7 more
computations of the DFE a second, puts one at about 0.87 M); Sd and TRN1d
(0.7 s) 38.6 M / 42.8 M. The digital modem's V.90 data mode is 39.5 M
(V34-REPORT.md), 32 % of the box's CPU, so this receiver alone would take
about 55 to 64 %, before the V.34 upstream transmitter and the rest of a
call. It fits one call; it does not fit beside other work. Not yet tried:
updating the taps every other symbol, a 16x16 multiply path, computing the
DFE less often with an A/D clock locked to the network.

C5-1 verified independently (fresh build, not the agent's):
- make check: 92 count lines, all 0 failures; against C4's run the only
  difference is the new line "v90arx: 488 checks, 0 failures".
- make bench-v90: transmit hash 8e3136e9, six of six.
- make bench-v90a: host and qemu-arm agree, hash a3bf6729, -56.4 dB, +100.00 ppm.
- modem/v90arx: no float, no allocation, the only statics are const tables.
- sizeof(v90arx_t) 12744 octets; orig_t is 68280 today, so it still fits
  V90M_MEM_SIZE (102400) with the receiver inside it.
- Open: cost (68-79 M ARM instructions per second of line, about twice the
  digital modem's data mode); the equaliser solve (0.87 M) runs within one
  tick; a band-edge loss at nonzero ppm.

## C5-2: the V.90 analogue modem, V.8 to data mode (2026-11-17)

- `modem/p2call`: a V.90 mode (`p2call_init_v90`, 9.2.2). The analogue
  modem's Phase 2 is V.34's *answer* procedure with V.90's INFO sequences
  (Tone A out, INFO0a, our L1 / L2 first, INFO1d in, INFO1a of Table 10
  out), so it is a second set of states inside p2call sharing its helpers
  (probe analysis, tone under probe, INFO0 builder); the V.34 call path is
  untouched (its only change: `default:` in the switch). INFO1a: UINFO 90
  (even: a robbed bit leaves its codeword alone), MD 22 x 35 ms, the
  upstream symbol rate INFO1d projects most for (3000 / 3200), its carrier,
  filter and power reduction for our transmitter. Recovery 9.2.2.2.1 to .3;
  no INFO1d (9.2.2.2.4) ends Phase 2 instead of retraining.
- `modem/v90dil`: the DIL we ask for (120 segments, Ucodes 0..119, 18
  symbols each: UINFO + + + then 15 training symbols, SP + + + + - - + - +
  + - - - + - + - +), its walk, the per-interval / per-Ucode accumulation,
  the analysis and the CPt / CP design (header comment has the details).
  The analysis snaps every mean to g x a Table 1 level (a pad or robbed bit
  still leaves the far codec on some codeword): g from the codeword UINFO
  arrives as (searched over a 15 dB range, least squares refined), only
  where the codec's step is >= 32 units. Robbed intervals: those departing
  from the median of the six at >= 16 of Ucodes 64..127. CP: Sr = 0, ld = 0,
  one constellation per interval, points from the bottom at d = z sigma,
  moduli where they cost least power, z raised to the power limit (Table 15
  less 0.1 dB, 8.5.2's weighted power rounded up), the highest K whose
  largest z reaches 12 (CPt: 20, K <= 16).
- `modem/v90adec`: the data mode decoder (5.4 backwards: labels, R0,
  signs for every Sr without the shaper's filter, see its header).
- `modem/v90a`: the sequencer (Phases 3, 4, data; renegotiation begun by
  the digital modem; cleardown both ways; time-outs of 9.3.2 / 9.4.2).
  Upstream: the V.34 blocks of C4 (`v34mod`, `v34_tmod`, `v34_tx`).
  Downstream: `v90arx` with the DIL as known symbols (their levels as
  measured so far), Ri recognised at the 4th symbol of a DIL segment.
- `modem/orig`: V.8 offers PCM (analogue) with `ORIG_MODE_V90`, JM's
  V.90 selection starts `v90a` (phase `ORIG_V90`, modulation
  `ORIG_MOD_V90` = `ANS_MOD_V90`), `bitrate` = our upstream, `rx_bitrate`
  = the downstream, `orig_snr_db10` (downstream), `orig_rtd` (RTDEa),
  `orig_cleardown`; `orig_retrain` gives -1. `v90a_t` is a member of its
  own: `orig_t` is 100744 octets (68280 before), within `V90M_MEM_SIZE`
  (102400; the API's compile-time check). A union with V.32bis / V.34 would
  have renamed `orig_t.v32` / `.v34`, which existing tests read.
- API: the calling role takes `V90M_MOD_V90` (V.92 still `V90M_ENOTSUP`).
- `v90arx` (C5-1): an optional known-level callback; `v90arx_hold` zeroes
  the timing loop's phase slew (it went on being applied each output while
  the loop was held: the DIL's output ran away). Neither changes C5-1's
  results (test_v90arx 488 checks as before, bench-v90a a3bf6729).
- Tests: test_v90arx_robust, test_v90adec, test_v90dil, test_p2v90,
  test_v90a (the sequencer against p2d + p34d, no V.8; time-outs),
  test_orig_v90 (end to end), test_api (V.90 calling case); the harness
  `test/v90link.h`. `make bench-v90c`, `make cost-v90c`
  (tools/rec_v90c.c, rig/bench_v90c.c).

v90arx against hostile line content (C5-1 code; the far end is not
trusted): (1) the estimator divided by the decided levels' power, which a
far end sending Ucode 0 alone decays to 0: floored at (L / 8)^2 (and L
itself at 1); (2) the clock measured on Sd: a tone 20 Hz off 1333 Hz makes
the tangent huge, the product overflowed int64 and the step came out wild
or wrapped: the tangent is clamped to 1 and the step to 1000 ppm; (3) the
timing loop integrated the step without bound: clamped to 1000 ppm, the
slew to 500 ppm (step + slew > 0 always), and the interpolator makes at
most 3 outputs per input sample. The other divisors (Cholesky pivots,
bias, the DFT phase, AGC) were already bounded. Valid signals: test_v90arx
488 checks as before, bench-v90a a3bf6729 unchanged. The changed lines no
longer left-shift signed values (multiplied instead).

Choices (where V.90 leaves them open): the echo canceller learns on MD,
white noise at the transmit level for 770 ms while the digital modem is
silent (L1 / L2 is a comb of 25 lines and would leave the canceller wrong
between them); its ERLE at the end of MD is 62..73 dB. DIL as above, four
passes (>= 8 measurements per interval and Ucode, about 1.1 s); the
receiver is held through the first pass and learns on the measured levels
after. CP's margin: d >= 12 sigma (2 Q(6) = 2e-9 for Gaussian noise), sigma^2
taken as twice what TRN2d shows (data mode shows 2 to 4 dB more error than
TRN2d with CPt's sparser points: with the TRN2d figure alone the A-law line
ran at 48000 with about 7 bit errors a second). TRN 1024 T; SCR after
Ri-bar for 4000 TRN2d symbols while the noise is measured; S, S-bar and CP
for our cleardown; Rd recognised as two frames of each interval's largest
point + + + - - - (or the inverse), delivery running two frames late so
that Rd is never handed on as data.

Seen failing first (against stubs, or the committed code):
- test_v90arx_robust: part 1 died (SIGFPE, exit 136), part 2 11 of 18
  (clock -30658 ppm; at 1351 Hz the overflow wrapped it to +21592), part 3
  1 of 1 (followed a 1500 ppm clock to -1536 ppm). Now 21 checks, 0.
- test_v90adec 204 of 398; test_v90dil 69 of 210 (391 checks now: a biased
  measurement case added after the snapping); test_p2v90 47 of 69;
  test_v90a 38 of 49 (50 now); test_orig_v90 34 of 104 with orig.c as
  committed (it fell back to V.34); test_api 8 of 167 with the committed
  api/v90modem.c (its V.90 block skipped), 187 checks now.

`make check` (fresh build): against the baseline taken from HEAD before any
change, the only differences are the six new lines and the API's:

```
> v90arx_robust: 21 checks, 0 failures
> v90adec: 398 checks, 0 failures
> v90dil: 391 checks, 0 failures
> p2v90: 69 checks, 0 failures
> v90a: 50 checks, 0 failures
> orig_v90: 104 checks, 0 failures
< api: 162 checks, 0 failures
> api: 187 checks, 0 failures
```

bench-v90: transmit hash 8e3136e9, six of six. bench-v90a: a3bf6729, host
and qemu-arm. bench-v90c: 5a5e5ca7, host and qemu-arm (data mode at the
recorded sample).

End to end (test_orig_v90; the library's caller with V.8 against the
library's answerer; downstream through sim/dschan, hybrid echo -16 dB,
upstream 6 dB loss and -70 dBm0 noise through the network codec; rows
"Table 1 levels" take Table 1's levels instead of the DIL's, a test
setting):

```
  line                                   data ms   down     up SNR dB   rtd  bits down  fail    bits up  fail  robbed
  2 km,    0 ppm, -75 dBm0                 10225  52000  28800  56.4    46     519987     0     288000     0  000000
  0 km, +100 ppm, -75 dBm0                 10527  33333  28800  38.5    47     333300     0     288000     0  000000
  6 km, -100 ppm, -75 dBm0                 10524  46666  28800  51.3    46     466690     0     288000     0  000000
  4 km,  +50 ppm, -65 dBm0                 10526  40000  28800  45.9    47     399960     0     288000     0  000000
  2 km,  +30 ppm, 3 dB pad                 10226  44000  28800  51.5    47     439989     0     288000     0  000000
    ... Table 1 levels                         0      0      0   0.0     0          0     0          0     0  000000
  2 km,  -30 ppm, 6 dB pad                 10525  40000  28800  49.0    46     400020     0     288000     0  000000
    ... Table 1 levels                     10225  30666  28800  37.2    46     306682     0     288000     0  000000
  2 km,  +30 ppm, robbed bit               10526  46666  28800  49.6    47     466655     0     288000     0  000010
    ... Table 1 levels                     10226  30666  28800  37.1    47     306659     0     288000     0  000001
  2 km,  +30 ppm, 6 dB pad, robbed bit     10226  42666  28800  48.8    46     426624     0     288000     0  010000
  2 km,  -50 ppm, A-law                    10224  45333  28800  50.6    46     453356     0     288000     0  000000
ending a call
  ours: caller ended 4 after 1223 ms (end 7), answerer phase 3, cleared 1
  the answerer's: caller ended after 99 ms, end 6
```

"fail" counts failures of the self-synchronising check over 10 s (each
wrong bit fails it three times): none anywhere. Without the DIL's levels
the pads and the robbed bit cost 9..16 kbit/s (the noise TRN2d shows grows
with the wrong levels and CP follows it), and the 3 dB pad does not
connect at all; the robbed interval is found in every robbed row (with
Table 1 levels the analysis still finds it, but the design ignores it).
"data ms" counts from the start of the call (V.8 about 2 s, Phase 2 about
2 s, Phase 3 3.8 s, Phase 4 1.4 s). The sequencer test (test_v90a, no V.8,
2 km, +30 ppm, echo -10 dB): 45333 / 28800, 4 s without an error, the
time-outs at 1500 ms from Ja (no Sd), 4500 ms from its end (no Jd), 15 s
from INFO1a (no MP), loss in data mode after 593 ms; renegotiation by the
digital modem followed (one break of the data stream: 27 check failures,
none in the 2 s after), cleardown either way.

ARM: every new test's output under qemu-arm is byte-identical to the
host's (test_v90arx_robust, test_v90adec, test_v90dil, test_p2v90,
test_v90a, test_orig_v90), and test_api passes under qemu.
Cost (qemu instruction counts, bench_v90c replaying a recorded call into
`orig_t` alone; 2 km, our V.34 upstream at 28800, echo -16 dB): data mode
77.4 M ARM instructions per second of line at 0 ppm (52000 down), 79.8 M at
100 ppm (46666 down): the C5-1 receiver's 68..79 M plus the V.34 upstream,
which is small beside it. That is about 65 % of the box's CPU (V34-REPORT:
39.5 M is 32 %); one call fits, nothing beside it. The longest ticks (the
four slowest on the host at 100 ppm, each counted alone as the recording
cut after it less the recording cut before it):

| tick | what | ARM instructions |
|---|---|---|
| 8.825 s | end of the DIL: analysis (gain search, snapping) and CPt design | 9.03 M (11.92 M before Table 1 was kept in a local array) |
| 9.552 s | CP design | 0.87 M (1.21 M before) |
| 14.151 s | the receiver's equaliser computed again, data mode | 0.87 M |
| 7.046 s | the receiver's first equaliser, end of TRN1d | 0.84 M |

The DIL tick is 9 M instructions in one sample (about 70 ms of the box's
CPU): mostly the gain search's 64-bit divisions (41 candidates x 576
measurements), which ARMv5 does in software. To be spread over ticks (or
the divisions turned into one reciprocal per candidate) before the box.

Findings:
- V.8 (C1, unchanged): with the hybrid echo at -10 dB, our own CM sits next
  to JM, and v8orig demodulates channel 2 without a band-pass (the V.21
  data path has one, `v21h_bpf.h`): on a 6 km loop and behind a 6 dB pad,
  whether two identical JMs are decoded depends on how the round trip
  aligns the echo with JM's bits (4 or more samples of extra upstream delay
  flipped the 6 km line from pass to "no JM"). At -14 dB and below both
  pass; the end-to-end test runs at -16 dB. A follow-up for v8orig.
- The receiver's 16-bit input (v90arx's AGC puts Sd at 4096 RMS) overloads
  on runs of Ucodes 122..127 (Ucode 127 arrived 9 % low): the DIL leaves
  out 120..127. A data constellation at -12 dBm0 never wanted them here.
- The DIL's repetitive patterns bias its means by up to 0.8 % (the
  equaliser's residual interference with a constant neighbour), which in
  data mode was 2 to 6 times the noise (rms error 26..69 units with the
  raw means, 10 with Table 1 levels on a clean line): hence the snapping.
- Holding the receiver through the whole DIL (1.1 s) lost 0 km / 100 ppm
  lines (the band edge's alias turns): it learns again after one pass.
- 0 km at 100 ppm stays C5-1's weak case: 33333, SNR 38 dB.
- At 50 ppm the receiver's error cycles by about 5 dB over 5 s (the band
  edge again); CP's 3 dB allowance covers it on the lines tried.
- Once, with CP designed on TRN2d's noise alone, an A-law call that already
  had a trickle of bit errors collapsed 14 s into data mode (the clock
  estimate ran from -50 to -24 ppm); with the allowance it did not happen
  again on that line, and it was not explained.
- The test harness's own upstream FIFO (our clock to the network clock)
  underflowed when it started empty: a dropped sample in our second S made
  the digital modem miss it. It fills 8 samples before it is read now.

Shortcuts and deviations:
- No retrains (9.5): `v90a_retrain` refuses; a digital modem's retrain
  (silence, Tone B) is taken as the downstream lost and ends the call.
  Phase 2 recovery 9.2.2.2.4 (no INFO1d) ends the call. No INFOMARKSa.
- No rate renegotiation of our own (9.6.2.1), nor its request for silence
  (CPs); the digital modem's is followed with the CP of the start-up.
- 4-point CP / E / SCR only (a Jd asking for 16 points is not honoured).
- No spectral shaping asked for (Sr = 0, ld = 0); the decoder does all Sr.
- The DIL is fixed (not adapted to what Phase 2 saw); the CP rms ratio
  field is sent as 1.0; INFO0a's capabilities are V.34's of C4.
- Bit 38 of INFO0d (power at the codec or the terminals) is not looked at:
  8.5.2's power is always computed on Table 1 levels.
- Echo delay: a fixed reference delay (`echo_delay`), not measured.
- Tested only in simulation against this library's own answerer.

Follow-ups: retrains (Phase 2 again from Tone A / Tone B); a band-pass in
v8orig's JM receiver; spreading the receiver's DFE solve (one tick) and the
DIL analysis / CP design (one tick each) over several ticks; renegotiation
from our side; 16-point CP; shaping; a real modem.

C5-2 verified independently (fresh build under /home, not the agent's):
- make check: all 0 failures; against C4 the only differences are the new
  lines (orig_v90 104, p2v90 69, v90a 50, v90adec 398, v90arx 488,
  v90arx_robust 21, v90dil 391) and api 162 -> 187.
- bench-v90: transmit hash 8e3136e9, six of six. bench-v90a (receiver,
  after the robustness fixes): a3bf6729 as at C5-1.
- New modem code: no float, no allocation, no mutable statics.
- Open: worst single tick 9.03 M ARM instructions (end of the DIL, ~70 ms
  of the box's CPU) must be spread before a real call; data mode ~65 % of
  the box; no retrains; V.8 JM lost at -10 dB hybrid echo (C1 v8orig has no
  channel-2 band-pass).

## Sync with v90d 0.4.1 (2026-11-17)

v90d main's modem changes since the split (819d38e..4799c4c: one copy of
the echo delay measurement, `modem/ecdly`, and of the retrain tone watch,
`modem/tonedet`, out of `p34d`, `v92/v92_p34d` and `v34dx`; `test_ecdly`;
`test_p34`'s mid-call events timed from data mode), applied as they are:
every file it touches is now octet for octet what main has. Here as well:
both modules in `MODEM_SRC`, `test_ecdly` in `TESTS`; the echo delay is
read from `ecd.delay` by the API's status, the replay tool's trace
(`tools/trace.c`) and `test_orig_v34` (the calling code itself used none of
what changed: `v34orig` and `orig` fill `v34dx_cfg.echo_delay`, which
stays). `V90M_MEM_SIZE` 100 KiB -> 112 KiB: the history in `p34d` and
`v92d` is 4096 samples now (2048), and `answer_t` is 106496 octets on the
host (98288), 106208 on ARM; `orig_t` 100752.

Checked against `calling` (0367bee), same PC:

| | calling | this |
|---|---|---|
| `make check` | 98 programs, ALL HOST CHECKS PASS | the same 98 with the same counts, plus `ecdly: 1019 checks, 0 failures`; ALL HOST CHECKS PASS |
| `make check-arm` | 98 programs, ALL ARM CHECKS PASS | the same, plus ecdly; ALL ARM CHECKS PASS |
| `make bench-v90` | | transmit hash 8e3136e9 (internals and API; host, shared, qemu-arm) |

The output of the calling side's tests (anstone, v8orig, orig, orig_v32,
orig_v34, orig_v90, p2call, p2v90, v90a, v90adec, v90arx, v90arx_robust,
v90dil, api) is identical line for line; the only lines of the whole run
that differ are `test_p34`'s (main's change to the test: its events now
start later in the call, so fewer bits, the same 372 checks) and timings.

In v90d (its branch `libv90modem-split` with main merged, built against
this): the 108 programs of main's `make check` with the same counts, the
bench hash 8e3136e9 as main gives it, 106 recordings x 8 configurations
replayed (1613 files) identical octet for octet; see v90d's PROGRESS.md.

Not done: `modem/v90a` (the calling side's V.90) keeps its own history of
what was sent for its fixed-delay canceller (`ecdl`, `V90A_ECDL`); it could
use `ecdly_put/ref/next`, a change of its own to be proved on its own.

## Sync with v90d 0.5.0: Bell 103 and Bell 212A answering (2026-11-17)

v90d main 4799c4c..9484fb7 (Bell 103 and Bell 212A callers answered),
modem side applied as it is: `modem/answer.c`, `answer.h`, `v21.h`, the new
`modem/bell103_bpf.h` and `test/test_answer.c` are octet for octet main's.
`tools/gen_v21_bpf.py` is merged (the library's `--high` and main's
`bell103`); it regenerates `v21_bpf.h`, `v21h_bpf.h` and `bell103_bpf.h`
octet for octet. No new source file, so no Makefile list changed.
`answer_t` 106496 -> 106560 octets on the host, 106208 -> 106264 on ARM
(`tonedet_t` and the tap pointer); `V90M_MEM_SIZE` stays 112 KiB.

API 0.2.0: `V90M_MOD_BELL103` (0x0040) and `V90M_MOD_BELL212A` (0x0080),
answering only and not in `v90m_cfg_default` (they change what a caller
without V.8 hears after 4 s); originating with either gives
`V90M_ENOTSUP`. `V90M_CONN_BELL103` = 8 and `V90M_CONN_BELL212A` = 9,
appended, checked equal to `ANS_MOD_*` at compile time. State, stats and
events need nothing new: the connection's modulation says it (SNR -1 in
Bell 103, as in V.21). `test_api` gains the cfg checks and five calls
(Bell 103 with and without V.22, Bell 212A, each not offered). Against the
unchanged API (the new names supplied by a scratch header): 203 checks, 10
failures; now 237 checks, 0 failures. Calling with Bell: planned after C6.

Checked against sync-0.4.1 (dc4e119), same PC, both run afresh:

| | sync-0.4.1 | this |
|---|---|---|
| `make check` | 99 programs, ALL HOST CHECKS PASS | the same 99, the same counts but `answer` 33 -> 54 and `api` 187 -> 237; ALL HOST CHECKS PASS |
| `make check-arm` | the same 99, ALL ARM CHECKS PASS | the same two changes; ALL ARM CHECKS PASS |
| `make bench-v90` | transmit hash 8e3136e9 | 8e3136e9 (internals and API; host, shared, qemu-arm) |

The output of both runs, host and ARM, is identical line for line but for
the new Bell lines of `answer` and `api`, their counts, the order `-j8`
gives and timings: the calling side's tests (anstone, v8orig, orig,
orig_v32, orig_v34, orig_v90, p2call, p2v90, v90a, v90adec, v90arx,
v90arx_robust, v90dil and the calling cases of api) print the same.

In v90d (`libv90modem-split` with main 9484fb7 merged, built against this):
main's 108 host and 107 ARM programs with the same counts (but `tel`, the
branch's own), bench hash 8e3136e9 as main, 106 recordings x 8
configurations replayed (1613 files) identical octet for octet, release
image built and `test-release` passes; see v90d's PROGRESS.md.

## C6-1: the PCM upstream transmitter's pieces (2026-11-17)

The analogue modem's V.92 upstream (6.2/V.92: 8000 symbol/s "derived from
the digital network") on our own D/A clock, and the receiver for what the
digital modem sends in Phase 4. Branch `calling-v92` from sync-0.5.0.

- `modem/pcmtx`: the transmit clock and interpolator. The symbols are made
  on the network clock as the downstream receiver (`v90arx`) knows it: the
  same accumulator as its interpolator, with the same increments (`step +
  slew`), so one symbol goes out for every network sample the receiver
  makes and every correction of its timing loop moves the transmitter too;
  the round trip between them is a constant. The symbols reach our clock
  through modem/v34_tabs.h's bank (the receiver's, used the other way
  round), 33 samples late. `pcmtx_slip()` delays the symbols that follow by
  a fraction of a symbol (the half symbol of the first Su-bar, Jp's
  fraction). Before the receiver has measured the clock (Ru, the first
  TRN1u, Ja) the step is our own sample.
- `modem/v92aseq`: SUVd, CPd and Ed out of the TRN2d descrambler, a data
  frame at a time. CPd may be about 20000 bits; every field after its frame
  sync is a start bit and 16 bits, so it is taken word by word into the
  `v92_cpd_t` with the CRC running along and no bit kept (v92_cpd_parse()
  would want a 20 kbyte array, more than `V90M_MEM_SIZE` has left). Ed is the
  first data frame of zeros straight after a sequence's fill (a fill that is
  a whole frame, when the CRC ends one, is passed over).

Seen failing first, against stubs: test_pcmtx 27 checks, 18 failures;
test_v92aseq 3020 checks, 1677 failures. Now:

```
same clock: 0 of 3967 samples differ from the symbols 33 samples earlier
    +0 ppm: 80000 symbols in 80000 samples (80000.0 expected), at most 1 in one sample
  +100 ppm: 79992 symbols in 80000 samples (79992.0 expected), at most 1 in one sample
  -100 ppm: 80008 symbols in 80000 samples (80008.0 expected), at most 2 in one sample
 +1000 ppm: 79920 symbols in 80000 samples (79920.1 expected), at most 1 in one sample
 -1000 ppm: 80080 symbols in 80000 samples (80080.1 expected), at most 2 in one sample
round trip through the test line's resampler, 3.4 kHz band limit:
     +0 ppm: fitted on 0.1..2.5 s, error over 2.5..7.5 s -283.3 dB, group delay -1.000
    +30 ppm: fitted on 0.1..2.5 s, error over 2.5..7.5 s -71.5 dB, group delay -1.000
   +100 ppm: fitted on 0.1..2.5 s, error over 2.5..7.5 s -71.7 dB, group delay -1.000
   -100 ppm: fitted on 0.1..2.5 s, error over 2.5..7.5 s -71.7 dB, group delay -1.000
   +300 ppm: fitted on 0.1..2.5 s, error over 2.5..7.5 s -70.8 dB, group delay -1.000
  slip 0.5000 at   +0 ppm: delay +0.5000 symbols, errors -298.4 / -70.3 dB
  slip 0.2500 at +100 ppm: delay +0.2500 symbols, errors -71.5 / -70.2 dB
  slip 0.7500 at -100 ppm: delay +0.7501 symbols, errors -71.6 / -70.4 dB
  slip 0.0044 at   +0 ppm: delay +0.0045 symbols, errors -298.4 / -80.2 dB
constant 32767: last output 32765, 0 outputs off
constant -32768: last output -32766, 0 outputs off
pcmtx: 27 checks, 0 failures
335 streams (D = 6..72, CPd of 2 to 1170 words), 15 with a whole frame of fill after the CRC
a bit wrong in the first of two CPd: 1 taken, 1 bad
ones, zeros, random: events 0x0
v92aseq: 3021 checks, 0 failures
```

"One fixed response": what the network codec gets is the symbols through a
response that does not change as the two clocks turn against each other
(fitted on 2.5 s, it predicts the next 5 s, two turns at 100 ppm, to -70 dB).
That needs the codec's band limit: without it, the band edge, where both
interpolators are poor, changes with the phase between the clocks. A real
line has that limit (G.712); the test line of C5 has none upstream, so C6-2
gives it one.

Both tests' output is byte-identical under qemu-arm. No answering-side
file is touched (two new modules, two new tests, the Makefile lists).

## C6-2: the V.92 analogue modem, V.8 to data mode with PCM upstream (2026-11-17)

- `modem/p2call` (V.90 mode): `cfg.v92` sets INFO0a bit 26; when INFO0d
  bit 27 and INFO1d bit 70 allow it, INFO1a is Table 18 (PCM upstream: all
  four filter sections, Ltot 384, Lmax 320, UINFO, no MD) and `pcm_up` is 1.
  `clk_ppm_q8`: our clock against the network's from the digital modem's
  L2 (each tone turns from 20 ms block to block by 2 pi 3 m e; weighted
  least squares over the tones with 20 dB SNR). Within 0.1 ppm on every
  test line; Sd's estimate, which V.90 uses, is off by up to 1.6 ppm.
- `modem/v90a` with `cfg.v92`: when INFO1a selected PCM upstream, V.92's
  Phases 3 and 4 and data (9.5.2, 9.6.2; the header of the V.92 block in
  v90a.c has the order): silence, Ru, Ru-bar, TRN1u 6000 T (the canceller
  learns on it), Ja with the DIL descriptor and our rates until Sd-bar (to
  a 12 bit boundary); Jd / Jp / Jp' parsed (Table 21, 22); Su 144 T, Su-bar
  24.5 T, Su until Jp, Su-bar (24 + Jp's fraction) T; TRN1u while the DIL is
  measured, CPt (Table 23 type 0, V.90's CPt design) once it is, until Ri;
  E1u; TRN2u (4 or 8 points as Jp says) until SUVd or 12000 T; SUVu, one CPu
  after SUVd (V.90's CP design), the acknowledge bit after CPd, E2u, B1u and
  data through `v92/v92_enc` with CPd's moduli, constellations, precoder,
  prefilter and gain at LU = the configured level. Downstream as C5, with
  SUVd / CPd / Ed from `v92aseq`. Rate renegotiation by the digital modem
  (Ru on our data frame boundary after its Rd-bar, TRN2u 2400 T, as Phase 4)
  and cleardown either way (CPu / CPd with drn = 0). Time limits: 9.5.2.2.1,
  9.5.2.2.2, 9.6.2.2.1 (20 s), Jp within 3 s of Su and Jp' of Su-bar (ours),
  12 s for a renegotiation (the digital modem designs CPd again).
- The transmitter (`pcmtx`, C6-1) runs on the receiver's clock from the
  first symbol: `v90arx_clock()` sets the receiver's step from L2 before
  Phase 3, and Sd overrules it only beyond 5 ppm. `v90arx_deaf()`: Sd is not
  looked for before Ja (our own Ru has its period: its echo was taken for
  Sd before the canceller had learnt). Neither changes V.90 (not called
  there; the Sd step computation is now relative to the step Sd came
  through, which is one sample in V.90, the same expression).
- `pcmtx` changed after C6-1: its bank is `modem/pcmtx_tab.h`
  (`tools/gen_pcmtx_tab.py`): 64 taps, a low-pass at 3.7 kHz (-1.7 dB at
  3.6 kHz, -15 dB at 3.8, -31 at 3.9), every branch within -78 dB of the
  others over the band; `pcmtx_wide()` uses the receiver's full band bank
  (v34_tabs.h), for the canceller's training on TRN1u. Why, below.
- `modem/orig`: `ORIG_MODE_V92` (= `ANS_MODE_V92`) passes `v92` to v90a;
  modulation `ORIG_MOD_V92` (= `ANS_MOD_V92`) when the upstream is PCM.
- API: the calling role takes `V90M_MOD_V92` with `V90M_MOD_V90`
  (`V90M_ENOTSUP` without it); header and README say "tried in simulation
  and against this library's own answerer only". `V90M_MEM_SIZE` stays
  112 KiB: `orig_t` 100752 -> 109504 octets on the host (109232 on ARM),
  so `struct v90m`, a union of the two roles, is now sized by the caller
  (about 109.6 KiB) where `answer_t` (106560) sized it before.
- Test line (`test/v90link.h`): the digital modem's V.92 parts (p2d with
  V.92 offered, v92d as answer.c starts it) or the whole answerer with
  `ANS_MODE_V92`; `up_line`: the upstream through a line and codec response
  on the network clock (1: sim/v92chan's pulse, the answerer's own V.92 test
  line, short to 2 km, long beyond; 2: dschan's downstream loop response),
  and our clock carried on to the network's by a 320-tap resampler
  (`sim/resamp`'s new `lresamp`). Off, everything is as in C5.
- Tests: test_p2v92a, test_v92a (the sequencer against p2d + v92d, no V.8),
  test_orig_v92 (end to end), test_api (V.92 calling case). Bench:
  `rec_v90c ... v92` / `bench_v90c` replay a V.92 recording (header
  "V92C0001"); `make bench-v92c`, `make cost-v92c`.

Seen failing first: test_p2v92a 46 checks, 9 failures (fields added, no
behaviour); test_v92a 65 checks, 18 failures (v90a not yet V.92: the call
went V.90), and its L2 clock check 66 / 1 before p2call measured it;
test_orig_v92 109 checks, 24 failures (orig not passing V.92 on: V.90
everywhere); test_api 242 checks, 6 failures (V.92 calling refused). Now
46 / 66 / 109 / 262 checks, 0 failures.

What it took (each found in simulation, in this order):
1. The answerer's MD wait: v90a defaults MD to 22 units, which Table 18
   reads as 22 x 276 symbols; the digital modem waited it out and missed
   our Ru. PCM INFO1a carries MD = 0.
2. Our Ru's echo taken for Sd (deafness until Ja, above).
3. C5's upstream (48-tap resampler, no band limit): PCM upstream needs
   the far codec's band limit and a resampler that is right to 3.9 kHz;
   without them B1u came with 21 units RMS of error at 30 ppm (9.6 at 0
   ppm) and decoded wrong (588 of 2688 bits).
4. The band edge. The digital modem's prefilter puts a third of our power
   above 3.5 kHz (it makes up for the codec filters); the receiver's 64-tap
   bank is -74 dB to 3.7 kHz but -9 dB at 3.9 kHz, and what it gets wrong
   there turns with the clocks: errors came and went with a 4 s period at
   30 ppm. A 128-tap bank cut at 3850 Hz was time-invariant but rang 64
   taps ahead of the cursor, outside the digital modem's 24-tap look-ahead
   (-39 dB of its energy): unbudgeted interference, errors at 0 ppm. The
   64-tap cut at 3700 Hz has -64 dB beyond 24 taps and is time-invariant to
   -78 dB.
5. The canceller trained on the low-passed TRN1u reached 33 dB (73 before):
   the hybrid's echo peaks at the band edge (+23 dB at 3.8 kHz against 1
   kHz in sim/dschan), where the low-pass leaves little to learn on. TRN1u
   (and Ru, Ja) go out through the full band bank (73 dB).
6. The clock. Found at Sd and corrected afterwards, it moved our symbols
   against the network by up to a hundredth of a sample during and after
   the digital modem's measurements (at 100 ppm its training receiver lost
   the first TRN1u altogether: no Ja); from L2 (0.1 ppm) the first TRN1u is
   already on the network clock and the receiver's loop has little to do.
7. Echo -10 dB: the PCM upstream's band edge echo clipped our 16-bit A/D
   (peaks of 30000) before the low-pass of 4; the end-to-end lines use
   -16 dB as C5's did; test_v92a's -10 dB call is clean now.

End to end (test_orig_v92, seeds as in the suite; 10 s of data per line;
"fail" counts failures of the self-synchronising check, three per wrong
bit; rtd includes the 320-tap resampler's 20 ms):

```
  line                                    mod data ms   down     up SNR dB   rtd  bits down  fail    bits up  fail
  2 km,    0 ppm, -75 dBm0               V.92   12251  52000  34666  56.7   184     520026     0     346632     0
  0 km, +100 ppm, -75 dBm0               V.92   12994  38666  34666  50.4   184     386628     0     346632     0
  6 km, -100 ppm, -75 dBm0               V.92   12707  44000  30666  51.6   183     440055     0     306682     0
  4 km,  +50 ppm, -65 dBm0               V.92   13822  40000  30666  45.8   185     399960     0     306636     0
  2 km,  +30 ppm, 3 dB pad               V.92   12246  48000  36000  45.3   184     479952     0     359964     0
  2 km,  -30 ppm, 6 dB pad               V.92   12702  42666  34666  50.1   184     426688     0     346632     0
  2 km,  +30 ppm, robbed bit             V.92   13155  44000  34666  50.3   184     439989     0     346684     0
  2 km,  +30 ppm, 6 dB pad, robbed bit   V.92   12018  40000  34666  49.5   184     399960     0     346632     0
  2 km,  -50 ppm, A-law                  V.92   11506  46666  37333  51.8   183     466690     0     373352     0
V.90 when V.92 is not on both sides
  the answerer without V.92              V.90   10697  52000  28800  56.5   184     520026     0     288000     0
  ours with V.92 off                     V.90   10394  52000  28800  56.9   184     520026     0     288000     0
reported only: the upstream through the downstream's loop and codec response
  2 km,    0 ppm, dschan's response up   V.92   13791  52000  32000  56.5   195     520026     0     319968     0
ending a call
```

Every line reaches V.92 with PCM upstream, 30666 to 37333 bit/s up and
38666 to 52000 down, with no check failure either way in 10 s; the V.90
fallback both ways (answerer without V.92, our V.92 off); cleardown from
either end. test_v92a (sequencer, 2 km, +30 ppm, echo -10 dB): 45333 /
32000, 4 s clean; the digital modem's renegotiation followed (one break of
the stream, 24 check failures, then 2 s clean); cleardown both ways; the
time-outs (no Sd, no Jd, no Jp, no DIL, no CPd by 20 s, carrier in data).

Not in the suite, the same table with six more seeds per line (54 calls),
the final code: 48 clean. The rest, honestly:
- 0 km, +100 ppm, seeds 100 and 104: the downstream receiver collapses in
  data mode and takes our transmitter's timing with it. Not V.92's: C5's
  V.90 caller on its own C5 line with the same seeds collapses on 3 of 6
  (and A-law -50 ppm had downstream errors on 2 of 6, which V.92's runs did
  not). C5's single-seed table hid it; it is C5-1's band edge at 100 ppm.
- 2 km, +30 ppm, robbed bit, seed 104: 1428 downstream check failures.
- 2 km, +30 ppm, 6 dB pad and robbed bit: seed 101 never reached data
  mode on both sides (the answerer gave B1u up, ERR_B1, and retrained; we
  do not retrain); seeds 103 and 104 have upstream errors (1836, 389). The
  cause found: our receiver's timing loop walks by about a hundredth of a
  sample between TRN1d and the end of the DIL (decisions on unmeasured
  pad / robbed bit levels), comes back during TRN2u, and our symbols move
  with it while the digital modem measures the channel for CPd, which it
  does not follow afterwards. Tried and not kept (no net gain over the 54
  calls): holding the loop from TRN1d to TRN2d, waiting 1 s after the DIL
  before CPt, the loop at 1/4 and 1/8 of its gains (48, 46, 44, 49 clean).

ARM: check-arm passes, the same programs and counts as the sync-0.5.0
baseline taken here first (99 programs) but for the new ones and api;
host and qemu-arm output of test_pcmtx, test_v92aseq, test_p2v92a,
test_v92a, test_orig_v92 identical. Benches: bench-v90 8e3136e9 (internals
and API, host, shared, qemu-arm); bench-v90c 5a5e5ca7 as at C5;
bench-v92c (2 km, +100 ppm) cd66ac82 on the host and under qemu-arm.

Cost (`make cost-v92c`, qemu instruction counts, the caller alone on its
recording): data mode 102.9 M ARM instructions per second of line at 0 ppm
(52000 / 34666 here), 112.5 M at 100 ppm (40000 / 34666), against C5's
V.90 77.4 / 79.8 M: the PCM upstream (encoder with LZ2 128, LP1 64; the 64
by 2 tap bank) costs some 25 to 33 M more than the V.34 upstream. That is
about 90 % of the box's CPU (V34-REPORT: 39.5 M is 32 %): one call, barely.
The six slowest ticks at 100 ppm:

| tick | what | ARM instructions |
|---|---|---|
| 8.286 s | end of the DIL: analysis and CPt design (C5's) | 9.03 M |
| 7.012, 9.125, 9.189, 13.861, 16.421 s | the receiver's equaliser computed again (C5's) | 0.84 to 0.86 M |

No V.92 step is among them: the worst tick is C5's DIL analysis, as
before, still to be spread over ticks.

Answering side, by diff against the sync-0.5.0 baseline (host and ARM):
the only differences are the new programs (pcmtx, v92aseq, p2v92a, v92a,
orig_v92) and api 237 -> 262 (its calling cases). No answering-side file
changed (modem/answer.c, p2d, p34d, v92/, v90/ untouched); `answer_t` is as it was.

Open:
- The multi-seed failures above; most of all our timing walking during
  Phase 3 on lines with pads and robbed bits (a transmitter clock that does
  not follow the receiver's loop excursions, but still the network, is the
  next thing to try).
- Cost: ~103 to 112 M instructions per second of data mode.
- No retrain (a digital modem's retrain or its B1u failure ends our call),
  no renegotiation of our own, no quick connect, no modem-on-hold, no
  fast parameter exchange. A CPd that leaves out a part (has_mod / has_filt
  / has_const = 0) is refused (V90A_ERR_CPD) rather than merged with the
  last one.
- SUVu says "no level measured"; 8-point TRN2u is coded but our answerer
  never asks for it, so it is untested; MD is never sent.
- The upstream lines are the answerer's own V.92 model (sim/v92chan);
  through dschan's full loop response (up_line 2) the 2 km row is clean on
  its seed, but the loop's low frequency tail outlasts the digital modem's
  12 ms channel estimate by -46 dB, which its design does not budget for.
- Tried in simulation and against this library's own answerer only: a
  reading of V.92 common to both sides passes every test.

## C7: Bell 103 and Bell 212A on the calling side (2026-11-18)

Branch `calling-bell` from `calling-v92` (c20c4ce). The calling modem
(`modem/orig`) calls in Bell 103 and Bell 212A when asked to
(`ORIG_MODE_BELL103` / `ORIG_MODE_BELL212A`, the numbers of the answering
side's `ANS_MODE_BELL*`; API: `V90M_MOD_BELL103` / `V90M_MOD_BELL212A` in
the calling role, still not in the defaults). Reported as
`ORIG_MOD_BELL103` / `ORIG_MOD_BELL212A` = `V90M_CONN_BELL103` /
`V90M_CONN_BELL212A`, as the answerer does.

- `modem/belltone`: the Bell answer tone, 2225 Hz within some 11 Hz, fixed
  point. 40 ms blocks brought down from 2225 Hz; a block is tone when that
  sum holds more than half its power and it is above -43 dBm0; 4 blocks in
  a row decide (160 ms). 2250 Hz (a V.22 answerer's unscrambled ones) turns
  once in a block and sums to nothing, 2100 Hz is far off.
- `modem/bell103h_bpf.h` (`tools/gen_v21_bpf.py bell103h`): the Bell 103
  answer channel for the caller, 2025 / 2225 / 2250 Hz flat, our 1070 / 1270
  Hz 57 dB down (and 1650 Hz too). The other three tables regenerate octet
  for octet.
- `modem/orig`: the order (orig.h has it in full, DESIGN.md 4.2d):
  1. ANSam and a V.8 mode on offer: V.8 as before (Bell is not in CM).
  2. 2225 Hz before any 2100 Hz tone is told (a pre-V.8 Bell answerer), or
     after ANS while a legacy answerer is awaited: 212A if offered, else 103.
  3. Only Bell on offer: no CI, no CM after ANSam / ANS; silent and
     listening (`ORIG_BELL`, new): 2225 Hz -> 212A (else 103); 103 alone:
     150 ms of a carrier in the Bell 103 answer channel that is not 2100 Hz
     -> 103 (a V.22 answerer's unscrambled ones are such a carrier, as for
     the plain modems the answering side was made for); 212A and 103: 6 s of
     that carrier without 2225 Hz -> 103 (our answerer plays 2225 Hz after 4
     s of unscrambled ones when 212A is on, so 212A wins where it is there).
  4. A 212A start without a 1200 bit/s answer in 3 s -> 103, if offered.
  Bell 212A: `v22m` as the caller at 1200 bit/s, scrambled ones from the
  moment the tone is told, data 765 ms after the answerer's (V.22's
  6.3.1.2.1 c, as the 212A caller in test_answer); no 2400, no retrain.
  Bell 103: the V.21 path on 1270 / 1070 Hz out, 2225 / 2025 Hz in. Off,
  nothing changes: the Bell code runs only with a Bell bit in the modes.
- Spec: the Bell 212A handshake document was not to hand. What was checked:
  2225 Hz answer tone for 212A and 103 and 1200 Hz originate carrier (TDK
  73K222BL and Conexant data sheets), V.22-compatible DPSK and scrambler,
  and the library's own answerer (V22_BELL_MS, its 1270 Hz watch) and its
  212A test caller. The 3 s and 6 s fallbacks are ours.
- `orig_t` 109504 -> 109616 octets on the host; `V90M_MEM_SIZE` stays
  112 KiB. `answer_t` and every answering-side file unchanged.

Seen failing first: test_belltone 27 checks, 8 failures (stub detector);
test_orig_bell 43 checks, 25 failures (the constants only: no Bell call,
only the V.8 fallback and the no-call cases passed); test_api 273 checks,
11 failures (Bell calling refused with V90M_ENOTSUP). Now 27 / 211 / 303
checks, 0 failures.

End to end (`test/test_orig_bell.c`: our caller, sim/v32line, pseudo-random
octets both ways through V.14 for 10.5 s, every data bit compared; "bits"
are data bits, 8 per octet; peers: the library's answerer with V.22, V.21,
Bell 103 and Bell 212A, or as said; two pre-V.8 answerers written in the
test that play nothing but 2225 Hz until answered, a 212A one that takes
103 callers too and a 103 one):

```
Bell calls, 10.5 s of data each way (bits: data bits compared)
  Bell 103, to our answerer                    clean                                          up  5.76 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  Bell 103, to our answerer                    10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  5.80 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2504 bits 0 errors
  Bell 103, to our answerer                    20 dB, -52 dBm0 noise, -3 Hz, -100 ppm         up  5.83 s, mod 8 / 8,  300 bit/s; up 2504 bits 0 errors, down 2504 bits 0 errors
  Bell 103, to our answerer                    6 dB, loud echoes (6 dB), 7 Hz, 100 ppm        up  5.78 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  Bell 212A (and 103), to our answerer         clean                                          up 10.82 s, mod 9 / 9, 1200 bit/s; up 10056 bits 0 errors, down 10056 bits 0 errors
  Bell 212A (and 103), to our answerer         10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up 10.91 s, mod 9 / 9, 1200 bit/s; up 10032 bits 0 errors, down 10040 bits 0 errors
  Bell 212A (and 103), to our answerer         20 dB, -52 dBm0 noise, -3 Hz, -100 ppm         up 10.93 s, mod 9 / 9, 1200 bit/s; up 10016 bits 0 errors, down 10016 bits 0 errors
  Bell 212A (and 103), to our answerer         6 dB, loud echoes (6 dB), 7 Hz, 100 ppm        up 11.07 s, mod 9 / 9, 1200 bit/s; up 10048 bits 0 errors, down 10048 bits 0 errors
  Bell 212A only, to our answerer              clean                                          up 10.82 s, mod 9 / 9, 1200 bit/s; up 10056 bits 0 errors, down 10056 bits 0 errors
  Bell 212A only, to our answerer              10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up 10.91 s, mod 9 / 9, 1200 bit/s; up 10032 bits 0 errors, down 10040 bits 0 errors
  Bell 212A only, to our answerer              20 dB, -52 dBm0 noise, -3 Hz, -100 ppm         up 10.93 s, mod 9 / 9, 1200 bit/s; up 10016 bits 0 errors, down 10016 bits 0 errors
  Bell 212A only, to our answerer              6 dB, loud echoes (6 dB), 7 Hz, 100 ppm        up 11.07 s, mod 9 / 9, 1200 bit/s; up 10048 bits 0 errors, down 10048 bits 0 errors
fallbacks
  212A and 103, answerer 103 only              clean                                          up 11.61 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  212A and 103, answerer 103 only              10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up 11.65 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  V.22, V.21 and Bell, answerer with V.8       clean                                          up  4.74 s, mod 3 / 3, 2400 bit/s; up 20088 bits 0 errors, down 20080 bits 0 errors
  V.22, V.21 and Bell, answerer with V.8       10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  4.64 s, mod 3 / 3, 2400 bit/s, V.8 1
  212A and 103, 2225 Hz-only 212A answerer     clean                                          up  1.54 s, mod 9 / 9, 1200 bit/s; up 10056 bits 0 errors, down 10056 bits 0 errors
  212A and 103, 2225 Hz-only 212A answerer     10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  1.63 s, mod 9 / 9, 1200 bit/s; up 10040 bits 0 errors, down 10040 bits 0 errors
  V.22 and Bell, 2225 Hz-only 212A answerer    clean                                          up  1.54 s, mod 9 / 9, 1200 bit/s; up 10056 bits 0 errors, down 10056 bits 0 errors
  V.22 and Bell, 2225 Hz-only 212A answerer    10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  1.63 s, mod 9 / 9, 1200 bit/s; up 10040 bits 0 errors, down 10040 bits 0 errors
  103 only, 2225 Hz-only 212A answerer         clean                                          up  0.47 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  103 only, 2225 Hz-only 212A answerer         10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  0.53 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2504 bits 0 errors
  212A and 103, 2225 Hz-only 103 answerer      clean                                          up  3.47 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  212A and 103, 2225 Hz-only 103 answerer      10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  3.53 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2504 bits 0 errors
  V.22 and Bell, 2225 Hz-only 103 answerer     clean                                          up  3.47 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2512 bits 0 errors
  V.22 and Bell, 2225 Hz-only 103 answerer     10 dB, echoes, -45 dBm0 noise, 1 Hz, 50 ppm    up  3.53 s, mod 8 / 8,  300 bit/s; up 2512 bits 0 errors, down 2504 bits 0 errors
no call
  212A only, answerer 103 only: phase 4, end 4; answerer phase 3, modulation 0
  no Bell offered, 2225 Hz-only answerer: phase 4, end 1
  Bell offered, nobody answers: phase 4, end 1
orig_bell: 211 checks, 0 failures
```

Every call above reached its modulation, with no bit error either way in
10.5 s of data: Bell 103 and Bell 212A on four lines each (clean; 10 dB,
echoes, -45 dBm0 noise, 1 Hz, 50 ppm; 20 dB, -52 dBm0, -3 Hz, -100 ppm; 6 dB
with echoes 6 to 8 dB down, 7 Hz, 100 ppm), the fallbacks on two. Bell 212A
against our answerer takes 10.8 to 11.1 s (its 5 s of ANSam, then 4 s of
unscrambled ones before the 2225 Hz); 212A and 103 offered to an answerer
with only 103 takes 11.6 s (the 6 s wait). test_api adds an API caller to
an API answerer (echo of a fifth each way): Bell 103, Bell 212A (both
`moving`, text both ways, retrain and cleardown refused), and V.22 + Bell
offered going V.8 / V.22bis.

Checked against calling-v92 (c20c4ce), same PC, both run afresh (the host
baseline equals ~/c52v/c6v/check.log count for count):

| | calling-v92 | this |
|---|---|---|
| `make check` | 104 programs, ALL HOST CHECKS PASS | the same 104 with the same counts but `api` 262 -> 303, plus `belltone` 27 and `orig_bell` 211; ALL HOST CHECKS PASS |
| `make check-arm` | 104 programs, ALL ARM CHECKS PASS | the same changes; ALL ARM CHECKS PASS |
| `make bench-v90` | 8e3136e9 | 8e3136e9 (internals and API; host, shared, qemu-arm) |
| `make bench-v92c` | cd66ac82 | cd66ac82 (host and qemu-arm) |

Sorted output of each pair, compile lines and timings left out, by diff: the
only baseline line that is not in the new run is `api: 262 checks`; every
other difference is a new line (belltone, orig_bell, the API's Bell calls).
test_orig_bell under qemu-arm prints what the host prints but for one
connect time (5.84 s against 5.83 s on the 20 dB line: the line simulator
is in double; the modem code is fixed point, bench-bell's hashes agree).

ARM cost (`make cost-bell`, one qemu run per call, the trace split where it
enters `bench_bell_mark`; the caller alone replaying `rec_bell`'s recording
of a call against our answerer on the 10 dB line; `make bench-bell`: hashes
48f22641 (103) and fc125ef2 (212A), host and qemu-arm the same):

| call | to data mode | first second of data mode | the second after |
|---|---|---|---|
| Bell 103 (data at 5.56 s) | 52.05 M | 9.85 M | 9.86 M |
| Bell 212A (data at 10.91 s) | 98.67 M | 4.43 M | 4.44 M |

So about 9 to 10 M ARM instructions per second of line while waiting and in
Bell 103 (the 81-tap band-pass runs every sample, as in V.21), 4.4 M in
Bell 212A: some 8 % of the box (V34-REPORT: 39.5 M is 32 %).

Open:
- Tried in simulation and against this library's own answerer (and two
  answerers written in the test) only; no real modem has been called.
- The Bell 212A handshake document was not to hand: no wait between the
  tone and our scrambled ones, and the 3 s / 6 s fallbacks, are our choices,
  checked against our own answerer only. A strict 212A answerer might want
  some silence first.
- An answerer with V.21 and Bell 103 but no V.22 plays 1650 Hz after ANSam;
  our Bell receiver has it 57 dB down, so a Bell-only caller waits there
  until it gives up (30 s). Not tested.
- JM with nothing in common does not fall back to Bell (it cannot: the
  answerer has gone V.8); ANSam with a V.8 mode on offer never tries Bell.
- A 212A start that fails without 103 on offer waits the V.22 30 s.

## ports-2: the port matrix on calling-bell (2026-11-18)

Branch `ports-2`, from calling-bell (632a820), with the parked `ports` work
(b26345f: the 16-target matrix, `tools/winerun`, the static library's
internal symbols renamed `v90m__i_*` by `tools/symmap.sh`, `check-symbols`,
CROSS / NM / SUB_BUILD in the Makefile, two clang warnings) cherry-picked
on top: no conflict.

### Why the earlier matrix runs failed on every target

They wrote their logs, and the compilers their temporary files, to /tmp,
which had run out of quota; nothing of them is left. The same script with
everything under /home builds and passes every target (below): it was the
environment. `tools/check-ports.sh` now refuses an OUT under /tmp and sets
TMPDIR to OUT/tmp.

### The matrix script

- Each test program's output is kept apart (`tools/portrun`, given to make
  as QEMU) and compared with host gcc's, program by program. Only lines
  that measure the host are left out: CPU and wall time, and the two lines
  that print the size of structures in memory (`sizeof(v90tx_t)` and
  `v34_rx_t`, which follow the pointer width and alignment).
- PAR targets at once (`xargs -P`), JOBS compilers each; QUICK="..." skips
  the slow programs of `tools/ports-slow.txt` on the targets named;
  COMPARE_ONLY=1 compares what a run left.
- `-DTEST_FULL` on every target: the ten tests that ran a shorter version
  under `__arm__` (v32 call, caller, startup, v8; v34 precoder, bench,
  resync, ber; v92 e2e, design) now say `TEST_SHORT`, which test/test.h
  sets on 32-bit ARM unless TEST_FULL is given. So `make check-arm` on the
  box's ARM is what it was, and the matrix runs the whole tests everywhere.
- 32-bit x86 (i686, win32) builds with `-msse2 -mfpmath=sse`; see below.
- `tools/printf-probe.c` is run on each target; see below.

### What was wrong

1. Unspecified order of evaluation (clang against gcc on the same host):
   test_viterbi drew its two noise samples as two arguments of one call,
   `v32_vit_step(&v, f(rng_gauss()), g(rng_gauss()), ...)`; gcc evaluates
   them right to left, clang left to right, so the two compilers fed the
   decoder different noise (clang's run: 8 bit errors at 12000 bit/s where
   gcc's has 0). test_constel did the same with `tu_rand()` twice in
   `v34_map_point`'s arguments. Both now draw in gcc's order in separate
   statements: gcc's output is unchanged, clang's is now the same.
2. Undefined behaviour in the signal path, found with `make check-ubsan`
   (new: the host suite built with clang `-fsanitize=undefined
   -fno-sanitize-recover`; it failed before, 36 reports, the tests aborted):
   - left shifts of negative numbers, on 23 lines: qamrx (2), dpsk, ec (2),
     v22rx, v90arx (10, and 3 more of the same kind beside them), v92_p34d
     (2), v32_rx (2), v32_ec, v34_nl (2). Each
     is now the multiplication it stood for (`x << 16` -> `x * 65536`,
     `x << -rs` -> `x * ((int64_t)1 << -rs)`): the same value wherever the
     shift was defined, and what every compiler made of it before.
   - signed overflow in 32-bit products: v22rx's carrier derotation
     (`zr * c + zi * s` while the gain has not settled: 168199 * -26905 in
     test_v22bis) and v90arx's forward-filter update (`es * x[i] + half`
     with a wild error, 286331153 * -8 in test_v90a, and in test_v92a). They
     wrapped on every target; they still wrap, now defined (unsigned
     arithmetic, converted back). Making them 64-bit would change what
     the modem does in those moments (and could move hashes): left as an
     open question, below.
   After: `make check-ubsan` passes, no report. The real-modem fixtures
   through v90m-replay, and the recordings of bench-v90, bench-v90c,
   bench-v92c and bench-bell replayed by sanitized builds: no report, and
   the same hashes (8e3136e9, 5a5e5ca7, cd66ac82, 48f22641, fc125ef2).
3. test.h's CHECK_EQ_I compared in `long` (32 bits on every 32-bit target
   and on Windows): now `long long`.

### Floating point: what differs and why

- uClibc-ng's printf (armv5 and m68k toolchains, uClibc-ng 1.0.43) rounds
  up where the correct rounding is down: 0.25 -> "0.3", 5.835 (the double
  5.83499999999999996...) -> "5.84", where glibc, musl and mingw print
  0.2 and 5.83. That is the 5.84 s against 5.83 s seen on ARM before
  (test_orig_bell): the same double, printed differently, not a different
  computation. `tools/printf-probe.c` finds such a printf on the target
  itself, and only there `tools/ports-cmp.awk` lets a number be one unit
  larger in its last printed digit (same sign and decimals); those lines
  are counted and reported; any other difference fails.
- The x87 (i686 and win32 by default) computes the simulator's doubles in
  80 bits: the simulated line differs, so the modem is fed other samples
  (test_v34dx on i686 without SSE: SNR at MP 40.3 / 43.3 dB against
  39.7 / 45.1, one renegotiation to 19200 against 16800 bit/s; the 32
  programs before it were identical). The library itself has no floating
  point: built for i686, its 78 objects contain no x87 instruction. So
  32-bit x86 builds the suite with SSE2 doubles, which round as binary64
  does everywhere else.

### Found after 4f4059d (the matrix and MemorySanitizer)

4. test_p2call compared INFO1d and INFO1c parsed from the same bits with
   `memcmp`, the padding octet before `freq_off` included, which neither
   parser writes: the check depended on stack garbage (clang
   `-fsanitize=memory` over the host suite: this was its only report).
   Both structs are cleared first now.
5. win32: test_v90arx_robust's watchdog used SIGALRM and alarm(), which
   Windows does not have, so the root tests did not even build there. The
   watchdog is there only `#ifdef SIGALRM` now.
6. v34 test_bench asserts at most 5000 ns of wall time per symbol; under
   qemu it failed on riscv64, mipsel and ppc (and was left out on ARM
   only). With `TEST_EMULATED=1`, which check-ports.sh gives its qemu
   targets, the check still counts but cannot fail.
7. test_v44 prints the encoder's and decoder's sizes: one more line that
   measures the host's ABI, left out of the comparison like `sizeof`.
8. Not a fix, a consequence of 1.: gcc for ARM evaluates arguments left to
   right, as clang does on x86, so calling-bell's `make check-arm` printed
   clang's numbers for test_viterbi (4 lines: "7200 bit/s ... plain
   decisions 613 symbol errors ..."). With the draws in a fixed order, ARM
   prints what the host prints. Same counts.

### m68k: qemu-m68k cannot be trusted with this code

(Corrected in the 0.6.0 audit, Task 5b: this was wrong. It was not qemu
but GCC 12.3's m68k back end; see "m68k: a GCC wrong-code bug, not qemu"
below. What follows is what was believed at ports-2.)

On m68k-uclibc (68040, qemu-m68k 10.1.5) twelve programs fail: every one
built on the V.34 receiver core, `modem/qamrx` (qamrx itself: training mse
+0.6 dB, 6607 symbol errors at 3000 symbols/s). The emulator, not the code:

- the same binary, run with `-one-insn-per-tb`, fails differently (3200
  symbols/s, which passes in a normal run, fails too) - a deterministic
  program whose result depends on how qemu cuts it into blocks;
- qamrx.c alone at -O1, the rest at -O2: passes; the whole suite at -O1:
  fails at 3200 as well; -mcpu=68060 run as m68060: passes;
- clang's UBSan and MSan over the host suite report nothing in qamrx, and
  the other 15 targets (five big-endian, seven 32-bit, the other uClibc)
  print what the host prints.
No m68k hardware here to settle it; m68k ran QUICK (11 slow programs left
out), 83 of the 95 that ran identical to the host (5 lines with uClibc's
rounding), the 12 qamrx users failing as above.

### The matrix (commit 4f4059d, the three programs changed since rerun on
### every target from this commit; win32 run whole from this commit)

`PAR=7 JOBS=1 tools/check-ports.sh` over ~/ports2-m2, then
`COMPARE_ONLY=1`. Every target built with -Werror. "Identical": the
program's output byte for byte, but for the timing / size lines.

| target | tests run | identical to host gcc | notes |
|---|---|---|---|
| host gcc | all 106 | (reference) | |
| host clang | all 106 | 106 | |
| x86_64 musl | all 106 | 106 | |
| i686 musl | all 106 | 106 | SSE2 doubles |
| armv5 uClibc | all 106 (TEST_FULL) | 106 | 6 lines with uClibc's rounding |
| armv7hf musl | all 106 (TEST_FULL) | 106 | |
| aarch64 musl | all 106 | 106 | |
| aarch64be musl | all 106 | 106 | big-endian |
| mips musl | all 106 | 106 | big-endian |
| mipsel musl | all 106 | 106 | v34 bench's wall-time check failed first (6.) |
| ppc musl | all 106 | 106 | big-endian; as mipsel |
| ppc64 musl | all 106 | 106 | big-endian |
| riscv64 musl | all 106 | 106 | as mipsel |
| s390x glibc | all 106 | 106 | big-endian |
| m68k uClibc | 95 (QUICK) | 83 | 12 fail: qemu-m68k, above |
| win32 mingw | all 106 (wine) | 106 | SSE2 doubles |

106 programs: the 60 tests of the top level, the API test against the
static library (check-symbols on every target: only `v90m_*` defined), and
the 45 of v34, v90, v92, v42, v32.

### Host and ARMv5 unchanged

| | calling-bell (632a820) | ports-2 |
|---|---|---|
| `make check` (host) | 106 count lines = ~/c52v/bellv/check.log | the same 106 lines, by diff; every other line the same (sorted, compile lines out) |
| `make check-arm` (armv5, qemu) | 106 count lines | the same 106 lines, by diff; output the same but test_viterbi's 4 lines (8.) |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 |
| `make bench-v92c` | cd66ac82 (host, qemu-arm) | cd66ac82 (host, qemu-arm) |
| `make bench-bell` | 48f22641 (103), fc125ef2 (212A), host and qemu-arm | the same |

### v90d re-proved on top of it

v90d-split d707463 built against this tree (`LIBV90M=~/ports2-wt`
at 4f4059d, the way libv90m.mk takes it; script
~/sync-build/proof/afterports.sh, after050.sh with that one
change), against the 0.5.0 sync's proof (libv90modem 629ae89):

| | 0.5.0 sync | ports-2 |
|---|---|---|
| `make check` | 126 programs, 2,839,306 checks | 133: the same 126 lines but `api` 237 -> 303, plus pcmtx, v92aseq, p2v92a, v92a, orig_v92, belltone, orig_bell (the calling side's C6, C7): 2,842,879 checks, 0 failures |
| `make check-arm` | 125 programs | 132, the same differences |
| `make bench-v90` | 8e3136e9 | 8e3136e9 x6 |
| replay, 106 recordings x 8 configs | 1613 files | manifest identical, all 1613 |
| `make -C fw release`, `test-release` | pass | pass, the same lines but timings |

So bringing in C6, C7 and the port changes changes nothing for v90d's
answering use; v90d-split needed no change.

### Open

- m68k: settled in the 0.6.0 audit (Task 5b): a gcc bug, "m68k" below.
- The two 32-bit products that wrap (v22rx derotation before the gain has
  settled, v90arx's forward update with a wild error) are now defined but
  still wrap: 64 bits would be the right arithmetic, but would change the
  modem's behaviour in those moments, maybe hashes. The owner's call.
- UBSan and MSan saw what the host suite, the fixtures and the benches
  run; code no test reaches is not covered.
- The benches' recordings (`rec_v90a`, `rec_v34`, v32's test_bench record
  mode) are written in host byte order; replaying one recorded on x86 on
  a big-endian target would read it wrongly. Not used across byte orders
  today.
- On 32-bit x86 the suite runs with SSE2 doubles; an x87-only build of the
  test harness prints other numbers (its line simulation), as described.

## realcheck: the calling side against recordings of real callers (2026-11-18)

Until now the caller (C1 to C7) had met only the library's own answerer, so
a misreading of a Recommendation shared by both sides would pass every
test. The only outside reference without a phone line: v90d's 106
recordings of real modems calling the box (sync-build/live-050, a copy of
v90d's live/*.raw; not in this repository). What real callers send was
read out of them with the answering side's own decoders, our caller's
calls were recorded the same way and read with the same tool, and the two
compared field by field.

### What the recordings hold

- Format (tools/README.md): per 10 ms, 80 samples the box's codec received
  and 80 codewords it sent. 75 recordings receive in 16-bit linear
  (`V90DLIN1`), 31 companded (mu-law). Both directions at the box's 2-wire
  port: what the caller sent (with the echo of the box's own signal under
  it) and what the box sent, from off-hook; 20 to 82 s, half under 36.
- No metadata in the files. The caller is told by what it sends (CM and
  INFO0a differ between the makers) and by v90d's PROGRESS.md (which modem
  was on the port that day): Conexant CX93001 (USB) 56 calls (16 V.90,
  33 with V.92 PCM upstream offered, 6 V.34 forced, 1 V.21 forced);
  U.S. Robotics 5686 (flash 5.4.62) 44, all V.92; U.S. Robotics 56K of
  1998 (flash 5.1.4, no V.92) 6, V.90. None of them sent CI. No V.32bis,
  V.22bis or Bell caller was recorded.
- The box's software changed between the calls (0.2.x to 0.4.x): today's
  answerer, set up as the recording box was (the options below), sends
  what was recorded until 15 to 25 s into most calls, data mode or late
  Phase 4; the 12 `pw_*` calls diverge after 8 to 10 s (Phase 3). Of every
  call the caller's side up to there is read with the answerer following.

### Tools

- `tools/v90m_fields.c` (`build/v90m-fields`, not installed): the answering
  modem run over a recording as `v90m-replay` runs it (same options), and
  after each sample what it decoded: CM (its octets), INFO0a / INFO0c,
  INFO1a / INFO1c, the probe's per-tone levels, Ja (V.90 and V.92), CPt,
  CP, CPu, SUVu, MP, the states of each engine with their times. Next to
  it, from the first sample of the recording, the library's V.21(L)
  start-stop receiver (CI, CM, CJ, V.21 data) and the Phase 2 DPSK
  receivers on both carriers (every CRC-clean INFO frame, every reversal
  of the caller's tone), and the caller's level per state of the answerer
  (L1 and L2 apart, the 1800 Hz guard tone against 2400 Hz). Every
  sequence is also rebuilt from what the parser read with the builder the
  caller sends it with (info0a_build, info1a_build, info1c_build,
  v90_dil_build, v90_cp_build, v92_ja_build, v92_cpu_build,
  v92_suvu_build, v34_mp_build, v8_encode) and compared bit by bit.
- `tools/rec_orig.c` (`build/rec_orig`, host): a call of our caller
  (V.92 + V.90 + V.34 + V.32bis + V.22bis + V.21; V.34 and below; V.21
  only) against the answerer through the public API set up exactly as
  `v90m-replay` sets it up, recorded at the answerer in the format above
  (test/v90link.h: 2 km, +100 ppm, the V.92 upstream line; test/line34.h's
  "real" line for V.34 and V.21). `v90m-replay` follows these recordings
  with 0 transmit differences up to data mode (15 to 59 in data mode,
  whose data are not the replay's), so `v90m-fields` reads them exactly as
  it reads the real ones (step 4: cross-decode).
- test/v90link.h: a hook for the digital end (`dig`), off in every test.

Options per recording: of `-9 -u 33600`, `-9`, `-9 -u 32000`,
`-9 -u 33333` and none, the one with which the replay's transmit stays
equal to the recording's longest (the seven extracts below name theirs).

### Real callers and ours, field by field

| field | Conexant CX93001 | USR 5686 | USR 1998 | ours | verdict |
|---|---|---|---|---|---|
| CI | none | none | none | none (send_ci off) | (a) V.8 7.1: optional |
| Te (ANSam start to first CM) | 1.24-1.26 s (1.56 in 10) | 1.50-1.52 s | 1.41-1.42 s | 1.49-1.52 s | (a) 8.1.1: >= 0.5 s, >= 1 s for V.25 echo control |
| CM octets | c1 65 13 94 2a 0d 27 (V.92/V.90), c1 45 13 94 2a (V.34) | c1 65 13 10 27 0d | c1 65 13 10 27 0d | c1 65 13 90 2a 27 (V.92/V.90), c1 45 13 90 2a | **(b)** 6.3 and 7.3: with the PCM category the PSTN access category "shall also be present"; every real PCM CM has 0d. The rest (V.23, LAPM, category order) (a): clause 6 fixes no order and the two makers differ |
| CM repeated | back to back, 10 ONEs between | same | same | same | = |
| JM to CJ (end) | 2 JM + the CM octet: 0.673 s (5-octet JM), 0.807 s (7) | 2 JM + the rest of the CM | same | 0.806 s on the V.90 line; one JM more (0.94 s) on line34's 20 ms delay | **(b)** 8.1.2 "after a minimum of 2": legal, but the first JM after ANSam is lost (below) |
| CJ | after callf (c1) | after the whole CM | after the whole CM | mid-CM, at the next octet | (a) 8.1.2: "complete the current octet" |
| INFO0a | all symbol rates, 3429 allowed, power reduction, 1664, bit 26 (V.92) 1, ack 0; sent once | the same | the same, bit 26 0 | 3000/3200 only, no 3429, power reduction, 1664, bit 26 1 with V.92; once | (a) capabilities; bits 26/27 as Table 16/V.92 |
| INFO0c (V.34) | all rates, bits 26:27 (clock) 00 | - | - | 3000/3200, 00 | (a) |
| Tone A / guard | carrier 1800/2400 Hz -6.1..-6.7 dB | -6.2 dB | -6.3 dB | -5.5 dB (the simulated loop's slope) | = 8.2.3.1/V.90: guard 6 dB under the carrier |
| reversals | 2nd = RTD + 80.0 ms; L2 521.6 ms (RTD 33 ms) | RTD + 80.0; L2 544 ms (RTD 34) | the same | RTD + 80.0; L2 528 ms (RTD 23) | = 9.2.2.1.5/.6: 40 + 40 ms; L2 <= 550 ms + RTD |
| L1 / L2 level | L1 = L2 + 6.0 dB | + 6.0 | + 6.0 | + 6.0 | = 10.1.2.4/V.34 |
| INFO1a, Table 18/V.92 | 12:17 0, MD 0, UINFO 79 (69..85 with the box's power), 32:33 0, 34:36 6, 37:39 6, **40:49 0x3fd** | 12:17 0x04 (Ltot 256), UINFO 78, 40:49 0x3ff | - | 12:17 0x3f (z1, p2; 384, 320), UINFO 90, 40:49 0x3ff | (a) ours as the table; the Conexant puts its frequency offset in 40:49, "not interpreted" by the digital modem, and ours does not |
| INFO1a, Table 10/V.90 | 12:17 0, MD 0, UINFO 79, up_sr 4, freq -3 | (in its retrain) MD 0, UINFO 78, 4, freq 0 | MD 0, UINFO 78, 4, freq 0 | MD 22 (770 ms), UINFO 90, 4, freq -5 (the line's 100 ppm) | (a) |
| INFO1c (V.34) | power red. 3-4, 3000/3200 pre-emph. 1, rates 11/13, freq -3 | - | - | power red. 0 + 3 more, pre-emph. 0, 11/12, freq -46 (line34's 1 Hz shift: sign and size right) | (a); freq. offset sign per Table 15 |
| Ja (V.90 DIL) | N 119, Lsp 124, Ltp 126, H 22..8, REF 79 | N 197, Lsp 66, H 10, REF 0 | the same as the 5686 | N 120, Lsp 18, H 2, REF 90 | (a) designs; layout the same (rebuilt bit for bit) |
| Ja (V.92) | as V.90 + rates 0x7ffff, filled to 12 | + 0x7ffff, **2092 bits: no fill to 12** | - | + 0x7ffff, filled | (a) ours per Table 20 / 8.5.4; the 5686 leaves the fill out, ours accepts it |
| CPt (V.90) | drn 9, Sr 1, ld 1, rates 0x1fff, a1 1.0, 1 constellation, **1 fill bit** | drn 16, Sr 1, ld 3, 0x07ff, a1 1.0 | drn 16, Sr 1, ld 3, 6 constellations, codec differs | drn 14, Sr 0, ld 0, 0x0fff, a1 0, 6 constellations | (a) Sr 0 (no shaping) and a1 0 are allowed (5.4.5); every real one asks for the DC null T(z) = 1 - z^-1. Conexant: Table 14 has 3 fill bits |
| CP (V.90) | (none recorded: V.92 taken) | drn 21, ack 1, 64-65 points | drn 21, ack 1, 65 points | drn 14, 25 points | (a) |
| CPt / CPu (V.92) | drn 9 / 18, Sr 1, ld 1, a1 1.0; CPt **289 bits** between repeats (no 12-fill) | drn 16 / 21, ld 3 | - | drn 14 / 10, Sr 0, ld 0, a1 0 | (a); Table 23 fills to 12 symbols, the Conexant does not between CPts, ours does |
| SUVu | no level, no wait, no silence | the same | - | the same | = |
| MP (V.34) | type 1 (precoder), max 12-13, nonlinear, rates 0x3fff, asym | - | type 1, trellis 64, shaping, 0x0ffe | type 0, 14/10, linear, 0x1ffe | (a) |
| Phase 3/4 durations (V.92) | TRN1u+Ja 2.4 s, Jd to Su 3.8 s, CPt after 2.0 s | 2.2, 2.1, 3.0 s | - | 0.92, 0.07, 1.25 s | (a) all inside 9.5.2: TRN1u >= 2040T, Su within 5 s of Jd, CPt within 5 s of Su |
| upstream level, Phase 3 (V.90, V.34 TRN) | 2.2 dB under Tone A | 6.0 under | 5.1 under | 0.2 under | (a) ours as INFO1d asks (no reduction); the real ones go lower |
| upstream level, V.92 TRN1u | 4.4 under | 2.0 under | - | 0.7 under | (a) LU is the modem's choice (3.8/V.92) |
| V.21 forced | no V.8 at all: silent through ANSam and V.22's unscrambled ones; V.21 after the answerer's mark | - | - | V.8 with CM "V.21 only", then V.21 | (a) |

Rebuilt bit for bit from the parsed fields with the caller's builders:
every CM of the 5686 and the 1998 modem and every V.34 CM; INFO0a/c 107
of 107; INFO1a 102 of 135 (the other 33: the Conexant's 40:49 of Table
18, nothing else); INFO1c 9/9; V.90 Ja 42/42, V.92 Ja 34 + 44 (the 5686's
fill); CPt, CP, CPt (V.92), CPu, SUVu, MP all equal but the fill lengths
named above. No field was read differently from what the tables say and
the values make sense for three modems of two makers (e.g. the training
Ucodes 1..118 or 116, 115, 0, 110 ...; the constellations' Ucodes
rising; the DIL's sign patterns), so the parsers' layout is confirmed from
outside, and the caller's builders give the real modems' bits.

Answering side (c): nothing the real modems would have had to tolerate.
Our answerer takes the shorter fills of the Conexant (V.90 CP: 1 bit; V.92
CPt: none) and of the 5686 (V.92 Ja) and the Conexant's 40:49; it
should.

### Found

1. **CM without the PSTN access category** (V.8 6.3, 7.3): the caller
   offers PCM and leaves access0 out. Fix (modem/orig.c, access 0: an
   analogue connection, as the real ones) and its test: branch
   `realcheck-v8`, see below.
2. **The first JM after ANSam is lost** on some line delays: the V.21(H)
   receiver reads ANSam's 2100 Hz as characters, and one in progress when
   JM begins swallows JM's ten ONEs, so its sync does not start a sequence
   and a third JM is needed (0.23 s more). test_v8orig over the clean
   line with delays 0 to 30 ms: 21 of 31 waited for a third JM. Fix
   (modem/v8orig.c: while 2100 Hz is heard the JM receiver reads idle
   mark; anstone's 5 ms block test, held two blocks): 0 of 31, CJ at most
   0.533 s after JM arrives. Branch `realcheck-v8`.
3. Both fixes move V.8's end by tens of milliseconds, and so every
   simulated call after it; with that, test_orig_v90 (0 km +100 ppm: 1626
   or 27362 downstream bit errors), test_orig_v92 (a row under its floor)
   and test_orig_v32 (the realistic row at 7200, floor 9600, at 21.1 dB)
   fail. The fault is not in the fixes: on ports-2 unchanged, moving only
   Te by 4 to 130 ms, the V.90 caller's data mode collapses (10^5 bit
   errors, the receiver's SNR from 45-50 dB to 25-30 dB within 10 s) in 10
   of 22 calls on the 0 km +100 ppm line, 1 of 13 on 2 km +30 ppm, 0 of 13
   on 6 km -100 ppm. In the bad calls there are errors from the start of
   data mode (about 1e-4), and after a few seconds the timing loop of
   modem/v90arx (fed by the decision-directed channel estimate) swings
   (delta -400 .. +2700 per block) and the errors run away. The tests
   passed because ports-2's V.8 timing happens to land in good calls. Not
   fixed here (C5's receiver; it needs its own work); the V.8 fixes stay
   on `realcheck-v8` until it is.

### New test

`test/test_realcheck.c` with `test/data/realcheck.txt` (20 kB of text: 32
sequences from 7 named recordings, as v90m-fields printed them, and where
each comes from). For each: the parser reads the fields named in the test
for that recording; the caller's builder given them gives the same bits,
CRC included (and the documented departures: the Conexant's 40:49 and
fill, the 5686's Ja fill, where ours follows the table); our INFO0a as
p2call sends it. CM, INFO0a, INFO0c, INFO1a (Tables 10 and 18), INFO1c,
Ja (V.90, V.92), CPt, CP, CPt and CPu of V.92, SUVu, MP. Portable: plain
text read with sscanf, no byte order. 652 checks, 0 failures.

### Unchanged

| | ports-2 (0f67c01) | realcheck |
|---|---|---|
| `make check` (host) | 106 count lines = ~/c52v/portsv/check.log | the same 106 lines, by diff, plus `realcheck: 652 checks, 0 failures`; every other line the same (sorted, compile lines out) but the ns timings of v34's bench |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 |
| `make bench-v92c` | cd66ac82 (host, qemu-arm) | cd66ac82 (host, qemu-arm) |
| `make bench-bell` | 48f22641, fc125ef2 | the same |

No file of the library changed (tools, test/, the Makefile).

### What the recordings cannot say

- The downstream as a client receives it: the recordings are taken at the
  box, so how the PCM downstream, JM, INFO0d / INFO1d, Jd, Jp, CPd, SUVd,
  MP and Ed of a real digital modem look, and how our caller's receivers
  take them, is not in them; the server side is always ours.
- A real answerer's ANSam, JM and Phase 2 timing (ours in all of them).
- Absolute levels: the box's port and the loop are not calibrated; only
  levels relative to the same call's Tone A compare.
- V.32bis, V.22 / V.22bis, Bell 103 / 212A and CI: no caller sent them.
  Short Phase 2, quick connect and modem-on-hold of V.92: none used them.
- Rate renegotiations and retrains begun by the caller are in a few calls
  only (v34a, x2r4, usr7's V.90 retrain); not compared here.

## realcheck-v8: the two V.8 fixes, held back (2026-11-18)

On branch `realcheck-v8`, over `realcheck`. Not to be merged as it is:
it turns three of the caller's simulation tests red, by the receiver fault
of "realcheck", Found 3 (below).

### The fixes, each test first

- CM: `modem/orig.c` adds the PSTN access category (access0 = 0, 0x0d)
  whenever the PCM category is sent (V.8 6.3, 7.3). Test: test_realcheck's
  "ours CM" against the 5686's CM: before, `m.has_access == 0, want 1`
  and no 0x0d (2 failures); after, `c1 65 13 90 2a 27 0d`, realcheck: 666
  checks, 0 failures.
- JM after ANSam: `modem/v8orig.c` holds the JM receiver in idle mark while
  2100 Hz is heard (5 ms blocks, the anstone test, two blocks of hold).
  Test: test_v8orig "CJ after the second JM, whatever the delay": the
  clean line at one-way delays 0 to 30 ms in 1 ms steps. Before: 21 of 31
  waited for a third JM, CJ up to 0.766 s after JM reached the caller;
  after: 0 of 31, at most 0.533 s. v8orig: 54 checks, 0 failures.
- Against spandsp's V.8 (an implementation not ours): `make
  test-interop` all pass with both fixes (interop_v8orig_spandsp 26, the
  V.90 digital answerer case with our CM's access octet included).
- Our caller's recorded calls read again with v90m-fields: CM `c1 65 13
  90 2a 27 0d`, every sequence rebuilt the same; the CJ now ends 0.707 s
  after the start of a 5-octet JM on line34 (two JMs; 20 ms each way
  included; was 0.94) and 0.807 s after a 7-octet one, the Conexant's
  0.673 and 0.807 s.

### What turns red, and why

Both fixes end V.8 at another moment (the CM is 33 ms longer, so the
answerer's JM starts 67 ms later; the JM fix saves a JM, 233 ms, on some
delays), and every simulated call after V.8 then meets other noise and
clock phases:

| program | ports-2 | realcheck-v8 |
|---|---|---|
| orig_v90 | 104, 0 failures | 2 failures: 0 km +100 ppm 41333 bit/s with 1626 bit errors in 10 s; "Table 1 levels" (2 km +30 ppm, robbed bit) at 33333, SNR 17.4 dB, 161670 errors |
| orig_v92 | 109, 0 | 1: "2 km +30 ppm, 3 dB pad" under its floor |
| orig_v32 | 95, 0 | 1: the realistic row (10 dB loss, 20 ms, 100 ppm) at 7200 bit/s, SNR 21.1 dB (ports-2: 9600 at 19.2 dB); floor 9600 |

The V.90 failures are the fault described in "realcheck", Found 3: on
ports-2 unchanged a shift of Te alone gives them (10 of 22 calls bad on 0 km
+100 ppm, 1 of 13 on 2 km +30 ppm). The V.32bis row sits at the edge of
9600 at that SNR; a Te shift alone on ports-2 did not move it (95, 0) at
the one shift tried. These want their own work (the V.90 receiver first);
then these fixes can go in with the tests green.

### Unchanged

| | ports-2 | realcheck-v8 |
|---|---|---|
| `make check` (host), answering side | 106 count lines | the same lines but v8orig 53 -> 54, orig_v32 / orig_v90 / orig_v92 as above, realcheck 666 added; the 45 subdirectory lines identical (run apart: `check` stops at `test`); fixtures all pass; api 303; symbols 551 internal |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 |
| `make bench-bell` | 48f22641, fc125ef2 | the same |
| `make bench-v92c` (the caller) | cd66ac82 | 66686c34, host and qemu-arm agree: the caller's own V.8 changed |
| `check-arm` (armv5, qemu; run once, on this tree) | the ARM count lines of sync-build/proof/afterports/check-arm.log | the same lines but v8orig 54, realcheck 666, and orig_v32 / orig_v90 / orig_v92 with the host's failures; v34, v90, v92, v42, v32 all pass, identical |
| v90d-split's live/replay, 106 recordings x 8 configs | 1613 files (sync-build/proof/afterports) | manifest identical, all 1613 (a copy of v90d-split d707463 built against this tree, uncommitted fixes in; rc-scripts/replayproof.sh) |

## robust 2a: the V.90 caller's data-mode collapse, and V.32bis's rate (2026-11-18)

Branch `robust`, from `realcheck-v8` (848f47a). Harnesses (not in the
repository): ~/rb-tmp/t (`ts.c` a Te sweep of one line, `tr.c`
the receiver's decisions against the octets the answerer sent, `tr4.c` /
`tr5.c` a least-squares channel fitted on the true symbols, `dil.c` the
DIL's levels, `sweep.sh` / `sweep92.sh` every line of test_orig_v90 /
test_orig_v92 with its seed, Te 1000 to 1130 ms).

### Test first

- `test/test_orig_v90_te.c` (new): test_orig_v90's 0 km +100 ppm line (seed
  21), 2 km +30 ppm (seed 24) and 2 km -50 ppm A-law (seed 31), each with
  Te = 1000, 1006 ... 1126 ms (22 calls); every call must reach data mode,
  meet the line's floor and carry 10 s both ways without a check failure.
  On realcheck-v8: 3 checks, 2 failures (15 of 22 calls bad on 0 km, 0 of 22
  on 2 km +30, 14 of 22 on A-law). ~16 s on the host; TEST_SHORT 4 per line.
- `test/test_orig_v32.c`: the realistic line again with Te = 1000 ... 1130
  ms (14 calls), each at 9600 or more and error free. On realcheck-v8: 96
  checks, 2 failures (the row at 7200; 5 of 14 calls at 7200 or 4800).

### What was wrong: three faults

1. **The DIL's small Ucodes** (modem/v90dil). The receiver's decisions in
   the bad calls against what the answerer sent: every early error (the
   1e-4) was one point, the smallest of one interval (e.g. slot 4, Ucode 1:
   the receiver's level 158 units, its true level 8; sign errors). The
   DIL's means of Ucodes 0 to 6 were off by up to 160 units on 0 km (Ucode
   0 at 105..131, Ucode 1 at 120..167, falling to normal by Ucode 7), not
   snapped (where the codec's step is under 32 units the mean stands), and
   CP took such a Ucode as its smallest point whenever its false level
   cleared d/2. The cause: the segment of Ucode 0 followed that of Ucode
   119; what the equaliser leaves of fifteen symbols at the largest level
   lies on the next 126 symbols (segments 0..6) and, the DIL repeating
   every pass, does not average out. The Te shift only decided whether a
   false level was picked. Fix: the DIL in the order 0 2 4 ... 118 119 117
   ... 3 1 (no segment after a much larger one). With it the levels of
   Ucodes 0..11 on 0 km are 1..98, rising, as Table 1 has them.
2. **The band edge** (modem/v90arx). With (1) alone 16 of 308 calls (0 km,
   +100 ppm, 7 seeds x 44 Te) still collapsed, four of five at the same
   moment of the clock drift (1.507 s into data mode). The equaliser's input
   fitted against the true symbols: its residual grows from 5 to 50 units
   at 4 kHz within 30 ms, and sliding fits show the channel at 4 kHz going
   from -20 to -70 dB and back every 1.25 s (3.9 kHz: -17 to -22 dB): the
   image of the band above 4 kHz folding on to the band below it with a
   phase that turns with the clocks (C5-1's finding). The MMSE equaliser,
   computed from an estimate 64 ms old, amplified that band as if it stood
   still (+12 dB over mid-band at Nyquist): errors on alternate symbols,
   then the decision directed estimate and the timing loop followed them
   (delta to -2500, total loss; the receiver never came back). Fix:
   `v90arx_edge()`: the image taken into the design as interference of
   unknown phase, a quarter of the power the estimate has at 3828, 3875,
   3922 and 3969 Hz, each over its 46.875 Hz (R += sum w P_k cos(w_k d)).
   v90a turns it on at Ri-bar (TRN2d, which CP is designed on, included);
   the DIL is measured with the plain equaliser (with it during the DIL the
   pad lines' levels came out worse: 6 dB pad's gain search picked the
   wrong codeword). Tried and dropped: a fixed triangle at Nyquist (costs
   rate on every line, breaks the pad lines).
3. **The estimate's regressor while held** (modem/v90arx): `sb[]`, the
   symbols the channel estimate is computed against, was written only when
   not held, so after every hold (Jd' to two DIL passes; Ri for 128
   symbols) the estimate resumed with up to 128 stale symbols in its span:
   its residual jumped (63 -> 1455) and the timing loop swung (delta -900
   during TRN2d) while TRN2d's noise, which CP is designed on, was being
   measured. Now written always.

V.32bis (orig_v32, realistic line, 20-22 dB): of 14 Te, 3 took 7200 and 2
took 4800; ports-2 the same line: 3 at 7200 and 2 that never reached data.
Not a row on its floor: a caller weakness, two of them.
4. Our receiver took the end of the answerer's AC for S (sim: in S mode for
   130 symbols, then the 16 T of silence before the real S, whose start
   passed for S-bar): it trained on S, read R1 with an error of 25000 and
   offered 4800 only in R2. Fix (v32_rx, calling role only: `!r->answer`):
   four symbols with next to nothing on time in S mode and it was not S;
   look for S again.
5. Our TRN ended at 1280 T + the round trip (the echo canceller had
   converged), but it also trains the answerer's receiver, which picks R3 by
   its error at the end of it: R3 at 7200 where its error in data mode
   allows 9600. Fix (v32_call): the caller's TRN at least 2560 T (6.1
   allows 8192); calls reach data 0.1 to 0.5 s later.

### After

`test_orig_v90_te`: 3 checks, 0 failures (0 of 22 on each line; 0 km at
45333 .. 46666, was 40000 .. 45333). `test_orig_v32`: 96, 0 (9600 on all 14).

Te sweeps, every line of the suites with its own seed, Te 1000 .. 1130 ms in
5 ms steps (27 calls a line; bad = any check failure or no data mode):

| line | V.90 before | V.90 after | V.92 before | V.92 after |
|---|---|---|---|---|
| 2 km, 0 ppm | 0, 52000 | 0, 50666..52000 | 0, 52000 | 0, 50666..52000 |
| 0 km, +100 ppm | **23**, 40000..45333 | 0, 45333..46666 | 2, 37333 | 0, 38666..48000 |
| 6 km, -100 ppm | 0, 46666..48000 | 0, 46666..48000 | 0, 42666..45333 | 0, 46666 |
| 4 km, +50 ppm, -65 dBm0 | 0, 40000..41333 | 0, 38666..40000 | 0, 38666..40000 | 0, 38666..40000 |
| 2 km, 3 dB pad | 0, 44000..48000 | 0, 44000..48000 | 0, 42666..48000 | 2 (up), 41333..48000 |
| 2 km, 6 dB pad | 0, 30666..40000 | 0, 40000..45333 | 0, 41333..45333 | 0, 44000..45333 |
| 2 km, robbed bit | 1, 45333..48000 | 0, 46666..49333 | 0, 44000..45333 | 0, 41333..46666 |
| 2 km, 6 dB pad, robbed bit | 1, 30666..42666 | 0, 29333..44000 | 10, 30666..44000 | 10 (up), 29333..45333 |
| 2 km, -50 ppm, A-law | **17**, 45333..46666 | 0, 46666..49333 | 0, 46666 | 0, 46666..48000 |

V.90: 42 of 243 calls bad, now 0. V.92: 12 of 243, still 12, all upstream
errors on the pad lines (2b). 0 km +100 ppm, 7 seeds x 44 Te: 0 of 308.

Floors moved, on that evidence (a row is one draw of its line's spread):
test_orig_v90's "6 dB pad, robbed bit" 40000 -> 28000 (29333 .. 44000
with Te alone, 30666 .. 42666 before these changes), test_orig_v92's "3 dB
pad" 45333 -> 40000 (41333 .. 48000; before 42666 .. 48000: five of 27
now at 41333, the DIL's levels under a pad, see 2b). The pad lines' spread
is the DIL's accuracy with pads and robbed bits (level biases of 45 units,
a codeword, on some points; the timing loop walking after the holds): 2b.

realcheck-v8's failures: orig_v90 104 checks 0 failures (0 km +100 ppm at
46666, SNR 52.8), orig_v92 109, 0, orig_v32 96, 0.

### Unchanged

| | realcheck (~/c52v/rcv/check.log) | robust 2a |
|---|---|---|
| `make check` (host), 107 count lines | | the same lines, by diff, but orig_v32 95 -> 96, v8orig 53 -> 54 and realcheck 652 -> 666 (realcheck-v8), orig_v90_te added; every answering-side line identical; the subdirectories (v34, v90, v92, v42, v32) identical |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 |
| `make bench-bell` | 48f22641, fc125ef2 | the same, host and qemu-arm |
| `make bench-v92c` (the caller) | 66686c34 (realcheck-v8) | ff7367a8, host and qemu-arm |
| `make bench-v90c` (the caller) | | 695b27e5, host and qemu-arm |
| `check-arm` (armv5, qemu; once, on this commit's tree) | sync-build/proof/afterports/check-arm.log's library lines | all pass; the same lines but orig_v32 96, v8orig 54, realcheck 666 (realcheck-v8), orig_v90_te 3 (TEST_SHORT: 4 Te per line), by diff |

Answering side: no file of it changed (v32_rx's change is under `!r->answer`;
v32_call, v90arx, v90dil, v90a are the calling side's). `v32rx_t` has one
int more (`s_lost`), which `answer_t` holds through its union.

## robust 2b: the rest of the simulation failures (2026-11-18)

### Test first

`test/test_orig_seeds.c` (new): every line of test_orig_v90 / test_orig_v92
with seeds 100 .. 105 (C6's extra seeds), V.90 and V.92: 108 calls, 10 s of
data each way (~50 s on the host; TEST_SHORT one seed). Every call: data
mode on both sides and kept, >= 28000 down (V.92: PCM upstream, >= 24000
up), the downstream without a check failure, the upstream likewise but for
one documented exception (below). On robust 2a: 3 checks, 2 failures:

- V.90 1 of 54: 3 dB pad, seed 103: the receiver lost (217000 failures);
- V.92 4 of 54, all on 6 dB pad + robbed bit: seeds 100, 103, 105 the
  answerer gave B1u up and retrained (we stayed in "data" until the carrier
  check ended the call 10 s later: 2c), seed 104 2389 upstream failures.

(On ports-2, C6 counted 6 of 54 for V.92; 2a fixed the two 0 km ones.)

### What was wrong

1. **The DIL's snapping under a pad** (modem/v90dil). The receiver's
   decisions against the octets sent: the 3 dB pad call lost after
   repeated errors on one point (slot 2, Ucode 69: level 2628, true 2743,
   one codeword off); one wrong decision there started an avalanche through
   the feedback (dense constellation, a feedback tail of norm 1.1) from
   which a decision directed receiver does not come back. The DIL's raw
   means against the true levels (data mode, 2 km, 3 dB pad): a gain of
   their own per interval, -0.4 .. -0.8 % in intervals 0..2, +0.3 .. +0.9 %
   in 3..5 (the segment positions next to the REF symbols or not), and one
   common gain was used to pick each mean's codeword: at a pad's spacing
   that picked a neighbour now and then. Fix: the codeword is picked with
   the interval's own gain (least squares over its Ucodes 64..127, twice,
   within 3 % of the common gain), the level written is the common gain's
   (data mode has no such bias). Snapped levels within 12 units of the true
   ones on that line (were off by a codeword on some points).
2. **TRN1d with UINFO robbed** (modem/v90arx). UINFO 90 is even so that a
   robbed bit leaves it alone, but behind a 6 dB pad it is codeword 75,
   which a robbed bit changes: in one interval in six TRN1d's symbols are
   not +-L. The channel estimate, fitted against +-L everywhere, carried
   that into every tap (TRN1d's error 42 dB where the other lines show
   21..24), the timing loop followed the estimate 0.03 sample away and back
   during TRN2d, and V.92's transmitter, on that clock, with it: the digital
   modem lost B1u (it retrained) or had upstream errors. Fix (`ls_set`):
   over TRN1d's estimator steps 700..1300 the residual is projected on each
   interval's part of the prediction, each interval's UINFO level set from
   it (within 25 % of L, left at L within 1 %), the reference and the
   symbols in the estimate's span take it; the estimate has 400 more steps
   to settle. TRN1d's error on that line 42 -> 33 dB, the timing walk
   during TRN1d gone.
3. **The timing loop during the DIL** (modem/v90a, V.92 only). Even so the
   loop walked 0.01 sample during the DIL on the pad / robbed bit lines
   (the DIL's repetitive patterns on levels still being measured pull the
   estimate) and came back during TRN2d, moving our PCM upstream while the
   digital modem measured it for CPd. V.92's clock comes from L2 (within
   0.1 ppm, C6-2): `v90arx_timing_hold()` holds the loop (step kept, no
   phase correction) from the DIL to Ri-bar. V.90 is left as it was: its
   clock comes from Sd (up to 1.6 ppm off) and nothing follows the loop
   (holding it there cost V.90 rates when tried).
4. **The v90arx input and Ucodes 122..127**: measured, not changed. Runs of
   Ucodes 116..127 in the DIL's sign pattern sent on 0 km, +100 ppm (test
   line, the A/D at the line's -6 dB): the A/D peaks 22716 (116) .. 32599
   (121) and clips from 122; the receiver's own 16-bit input (after the AGC,
   Sd at 4096 RMS, gain 1.28 there) clips from 125. Neither is reachable:
   the DIL stops at 119, CP uses measured Ucodes only, Rd is CP's largest
   point, every other downstream sequence is UINFO; data mode peaks at
   9200 .. 13500 (7.7 dB of headroom at the A/D). Taking the AGC below unit
   gain would move every fixed-point scale of the receiver for no reachable
   case; left, documented here.

### After

test_orig_seeds: 3 checks, 0 failures; V.90 0 of 54 bad, V.92 0 of 54 bad,
2 of 54 with an upstream burst (pad6rb seed 102 at 37333 up: 147 failures;
robbed bit seed 105 at 37333: 88). The C6 extra-seed table (54 calls, V.92):
6 bad on ports-2, 5 on 2a, 0 now (+ 2 bursts).

The Te sweeps again (27 calls a line, as in 2a):

| line | V.90 2a | V.90 2b | V.92 2a | V.92 2b |
|---|---|---|---|---|
| 2 km, 0 ppm | 0, 50666..52000 | 0, 50666..52000 | 0, 50666..52000 | 0, 49333..52000 |
| 0 km, +100 ppm | 0, 45333..46666 | 0, 45333..46666 | 0, 38666..48000 | 0, 45333..48000 |
| 6 km, -100 ppm | 0, 46666..48000 | 0, 46666..48000 | 0, 46666 | 0, 46666 |
| 4 km, +50 ppm, -65 dBm0 | 0, 38666..40000 | 0, 38666..40000 | 0, 38666..40000 | 0, 38666..40000 |
| 2 km, 3 dB pad | 0, 44000..48000 | 0, 44000..48000 | 2 (up), 41333..48000 | 0, 44000..48000 |
| 2 km, 6 dB pad | 0, 40000..45333 | 0, 40000..45333 | 0, 44000..45333 | 1 (up 81, at 37333), 44000..45333 |
| 2 km, robbed bit | 0, 46666..49333 | 0, 46666..49333 | 0, 41333..46666 | 0, 44000..46666 |
| 2 km, 6 dB pad, robbed bit | 0, 29333..44000 | 0, **38666..45333** | 10, 29333..45333 | 1 (up 32, at 34666), 40000..45333 |
| 2 km, -50 ppm, A-law | 0, 46666..49333 | 0, 46666..49333 | 0, 46666..48000 | 0, 46666..48000 |

V.92: 12 of 243 -> 2 of 243 (both one upstream burst). With the pad
lines' spread narrowed, 2a's floors go back up: test_orig_v90's "6 dB
pad, robbed bit" 28000 -> 37333 (38666 .. 45333), test_orig_v92's "3 dB
pad" 40000 -> 42666 (44000 .. 48000).

### Left open: V.92 upstream bursts

At the rates the digital modem's CPd picks at the top of a line (34666 to
37333 here) its PCM upstream receiver sometimes takes one symbol wrong: a
burst of 30 to 50 check failures (about 10 to 17 bits), recurring about
every 4.5 s on such a call (17.54 s and 22.01 s on robbed bit seed 105),
4 calls of 243 + 108. Traced: our transmit timing against the network is
steady to 0.0005 sample through the burst; the harness's upstream FIFO
neither under- nor overflows; freezing our timing in data mode makes it
far worse (drift). The answerer's receiver does not adapt in data mode
(v92_p34d: v92_trn_store) and picks the rate: answering-side code, not
touched here. test_orig_seeds counts such calls and lets them pass while
they stay bursts (under 200 failures in 10 s).

### Unchanged, and the end-of-step checks (on this commit's tree)

| | reference | robust 2b |
|---|---|---|
| `make check` (host) | ~/c52v/rcv/check.log | the same count lines, by diff, but orig_v32 95 -> 96, v8orig 53 -> 54, realcheck 652 -> 666 (2a, realcheck-v8), orig_v90_te 3 and orig_seeds 3 added; every answering-side line and the subdirectories identical |
| `check-arm` (armv5, qemu, once) | 2a's | all pass; the same lines plus orig_seeds 3 (TEST_SHORT: seed 100, 18 calls, 0 bad, 0 bursts), by diff |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 |
| `make bench-bell` | 48f22641, fc125ef2 | the same, host and qemu-arm |
| `make bench-v92c` (the caller) | ff7367a8 (2a) | 506f2bcc, host and qemu-arm agree |
| `make bench-v90c` (the caller) | 695b27e5 (2a) | 695b27e5, host and qemu-arm |
| v90d's replay, 106 recordings x 8 configs (rc-scripts/replayproof.sh, v90d-split d707463's copy built against this tree) | sync-build/proof/afterports/manifest.sha256 | MANIFEST IDENTICAL, 1613 of 1613 |
| `make check-ubsan` | | ALL UBSAN CHECKS PASS, no runtime error |
| `tools/check-ports.sh` i686-musl, aarch64be-musl, mips-musl, win32-mingw, host-clang (PAR=5, outputs under ~/rb-tmp/ports) | host gcc | ALL PORTS PASS: 109 of 109 programs identical on each |

No answering-side file changed in 2b (v90arx, v90dil, v90a are the
calling side's). The ubsan, port and replay runs were made on a copy of
this tree (~/rb-2b, `diff -r` identical but PROGRESS.md) while
check-arm ran in the worktree.

## robust 2c: retrains on the calling side (the design, as written before the step)

Left for its own step (2a and 2b took the branch's time). What is there:
V.32bis (v32_call: 7.1 / 7.2 both ways, test_orig_v32 checks both) and
V.34 (v34orig: 11.5.1.1 / 11.5.1.2 both ways, test_orig_v34) already
retrain from either end; V.90 and V.92 do not: `v90a_retrain()` and
`orig_retrain()` refuse in V.90 / V.92 (`v90m_retrain` gives V90M_ESTATE in
the calling role), an answerer's retrain (70 ms of silence, then Tone B,
answer.c ANS_RETRAIN) is taken for the downstream lost and ends the call
(V90A_ERR_CARRIER, ~0.5 s of input 12 dB under, or 10 s after a B1u the
answerer gave up), and the receiver does not ask for one when it degrades.
The digital modem's renegotiation is followed (9.6.2.2 / 9.8.2.2).

Design:
- Detecting the answerer's retrain in data mode or Phase 3/4: the PCM
  downstream silent (the 70 ms; v90arx's input power, as the loss check
  but over ~40 ms) followed by Tone B (2400 Hz, p2call's tone detector):
  go to Phase 2 of a retrain (V.90 9.5.1.2 / V.92 9.5: p2call's V.90 mode
  started at "after Tone B": Tone A from us, INFO0 not exchanged, INFO1d /
  INFO1a, V.92's Table 17/18 forms when both indicated V.92 at the start),
  then v90a's Phases 3 and 4 again from Ja with a fresh v90arx (the DIL
  measured again) and, for V.92, a fresh pcmtx / echo canceller training on
  TRN1u.
- Ours (9.5.1.1): 70 ms of silence, Tone A, the same Phase 2. Triggers:
  `orig_retrain()` (API), and the receiver: v90arx's error over 1 s more
  than 6 dB above what CP was designed for, or the self-check of the
  descrambled data (Rd never ambiguous), or decision errors in Rd / B1d.
  A limit (2 in a minute, as answer.c's rt_recent) before giving up.
- Our rate renegotiation (9.6.2.1 V.90 / 9.8.2.1 V.92): Rd-equivalent from
  the analogue side is CPs (CP with the request for silence) on V.90, Ru on
  V.92; then CP / CPu designed on the current noise. A first step that
  would also give the receiver a way down without a retrain.
- orig_t has room: the V.90 retrain reuses p2call and v90a in place.
- Tests, failing first: test_orig_v90 / v92: `answer_retrain()` mid-call
  (today: the call ends), a line change mid-call through test/v90link.h
  (level drop 10 dB, noise +20 dB: today the receiver collapses and the
  call ends 0.5 .. 10 s later) that must lead to our retrain and data again,
  renegotiation both ways; test_api: `v90m_retrain` in the calling role
  V90M_OK (today V90M_ESTATE, which test_api asserts); then README /
  include/v90modem.h on what the calling role does.

## robust 2c: retrains and rate renegotiation, V.90 / V.92 calling side (2026-11-18)

Branch `robust`, from robust 2b (8fb789c). Harnesses (not in the
repository): ~/rb-tmp/t2 (`dbg.c` one case with the caller's
and the answerer's states, the receiver's error against the design noise
every 250 ms; `ed.c` that ratio over 10 s of data on every test line).

### Test first

- `test/test_orig_retrain.c` (new): the library's caller against the
  library's answerer, 2 km, V.90 and V.92; each case to data mode, 2 s of
  data, the event, the time until data moves both ways again, then 5 s that
  must pass the check without a failure, V.92 staying V.92: the answerer
  retrains (answer_retrain), we retrain (orig_retrain), noise +15 dB (down,
  then back up when it goes), the level -10 dB, a 6 dB pad put in, our
  renegotiation with a ceiling of 40000 and back up without it, the
  answerer's renegotiation (p34d / v92d), the receiver wiped (channel
  estimate and equaliser zeroed: a total collapse), a 300 ms noise burst at
  -10 dBm0. Then the limits: three retrains in a minute, the fourth refused,
  the receiver lost after them a cleardown; a retrain never answered ends
  the call. On robust 2b: **194 checks, 91 failures** (every case but the
  answerer's renegotiation: the call ended on the answerer's retrain or a
  V.92 line change, the receiver stayed lost on the others, orig_retrain /
  orig_renegotiate gave -1; the later checks, V.92 staying V.92, who began
  the retrain, the cleardown past the limit, use fields 2b has not).
  ~13 s on the host; TEST_SHORT: V.90 and its limits.
- `test/api/test_api.c`: the V.90 and V.92 caller blocks ask for a retrain
  (V90M_OK, a second at once V90M_ESTATE), wait for data again (retrains 1,
  DATA_STOPPED, rates, no renegotiation counted), then clear down. On robust
  2b: **302 checks, 10 failures**. Then a block (it needs the new end code,
  so not run on 2b): a retrain the answerer never answers ends the call with
  V90M_END_RETRAIN_FAILED and V90M_EV_ENDED.
- test_orig_v90 / test_orig_v92 / test_v90a / test_v92a asserted the
  refusal (-1); those checks now assert no retrain on a clean start-up (the
  counts stay: 104, 109, ...).

### What was built (modem/v90a, modem/p2call, modem/orig, api)

1. **Phase 2 of a retrain** (`p2call_init_v90_retrain`): Tone A at once,
   no INFO0 (the answerer's p2d_init_retrain sends none, V.92 too, though
   9.7 says "the full Phase 2"; not ours to change), the first INFO0d kept;
   9.2.2.1.3 on as at the start, V.92's INFO1d / INFO1a forms as then.
2. **Retrains** (9.5.2 / 9.7.2): the answerer's Tone B (modem/tonedet on
   what our echo canceller leaves, from Phase 3 on: data mode, Phases 3 and
   4, renegotiations) or ours: 70 ms of silence, Phase 2 as above, then
   Phases 3 and 4 with everything fresh (v90a re-initialised in place:
   receiver, DIL, canceller, V.92's pcmtx) but the call's counts, clock and
   first INFO0d. `v90a_retrain()` / `orig_retrain()` from data mode or a
   renegotiation. A renegotiation without Ed (9.6.2), and a Phase 3 / 4 that
   fails once the call has carried data, are retrains too; a retrain's
   Phase 2 that fails ends the call.
3. **Our renegotiation** (9.6.2.1: S, S-bar, SCR until the digital modem's
   TRN2d has shown 4000 symbols of noise, then CP designed on it; 9.8.2.1:
   Ru, Ru-bar, TRN2u, SUVu, a CPu designed the same way), `v90a_renegotiate`
   / `orig_renegotiate(o, ceiling)`, the ceiling never above the
   configured one. Found on the way: V.92's SUVd / CPd / acknowledge / Ed
   flags of Phase 4 were still set when our Ru went out, so we sent E2u at
   once and the answerer failed B1u (v92d ERR_B1, a retrain, V.90 after it):
   cleared at the start of ours. Not CPs (the request for silence): our
   canceller needs no new training for a renegotiation.
4. **The receiver's watch** (once a data frame): the sum over the six
   intervals of v90arx's error against the noise CP was designed for (twice
   TRN2d's). Measured (`ed.c`, 72 calls on the test lines, 4 seeds): 0.20 to
   0.97 of it in data mode, max over 50 ms. The design said 6 dB over: by
   then the decision feedback is already going (noise +15 dB on 2 km: the
   ratio sits at 4, 0 errors for 2.5 s, then an avalanche, the error 10^5
   over). So: 3 dB over for 1 s -> a renegotiation down when only the noise
   rose, a retrain when the equaliser's input power moved 3 dB (level, pad:
   the DIL no longer holds) or a renegotiation for the line ended within 5 s;
   12 dB over for 250 ms (the receiver lost) -> a retrain, V.92's timing loop
   held meanwhile (our PCM upstream runs on its clock: unheld, the wiped
   receiver dragged it until the answerer's design failed and it retrained
   with PCM upstream off), and Rd not believed meanwhile (a lost receiver
   took garbage for Rd). 6 dB under for 5 s after the watch took the rate
   down -> a renegotiation back up (once). On those 72 calls: 0 retrains, 0
   renegotiations.
5. **Limits** (as answer.c's): 3 retrains without a minute of data mode
   between them (one forgotten per minute). Past it: orig_retrain gives -1;
   the answerer's Tone B, or a retrain that fails, ends the call; one our
   receiver needs is a cleardown (CP / CPu with drn = 0). Either way
   V90A_ERR_RETRAINS -> ORIG_END_RETRAIN (8) -> V90M_END_RETRAIN_FAILED
   (appended to v90m_end) with V90M_EV_ENDED.
6. orig: orig_retraining / orig_retrains for V.90 / V.92; include/v90modem.h
   and README say what the calling side does now.

### After

test_orig_retrain: 222 checks, 0 failures. Time from the event to data both
ways again, downstream / upstream before -> after:

| case | V.90 | V.92 |
|---|---|---|
| the answerer retrains | 7.4 s, 52000/28800 -> 48000/28800 | 10.6 s, 49333/34666 -> 52000/32000 |
| we retrain | 7.4 s, 50666 -> 50666 | 8.3 s, 50666/36000 -> 46666/36000 |
| noise +15 dB | 2.0 s, renegotiation, 46666 -> 36000; noise gone: 6.1 s, back up to 49333 | 10.2 s, renegotiation then retrain, 48000 -> 36000; back up to 45333 in 8.3 s |
| level -10 dB | 7.6 s, retrain, 48000 -> 36000 (later up to 41333) | 10.1 s, retrain, 46666 -> 40000 |
| 6 dB pad put in | 7.6 s, retrain, 52000 -> 45333 | 9.4 s, retrain, 46666 -> 45333 |
| our renegotiation, ceiling 40000 | 1.0 s, 52000 -> 40000; no ceiling: 1.0 s, 52000 | 4.0 s, 52000 -> 40000; 4.8 s, 52000 |
| the answerer renegotiates | 0.4 s (as before) | 4.8 s (as before) |
| receiver wiped | 7.6 s, retrain, 50666 -> 50666 | 10.6 s, 48000 -> 50666, PCM upstream kept |
| noise burst 300 ms | 7.6 s, retrain, 48000 -> 52000 | 9.3 s, 46666 -> 48000 |

Retrain limits: three in a row 7.4 s each (V.90), 9.8 to 10.7 s (V.92);
the fourth refused; lost after them: cleared down in 0.9 s, the answerer
told (cleared 1); never answered: ended 8.1 s after the request, silent.
test_api: 326 checks, 0 failures (V.90 retrain by API 7.3 s, V.92 8.9 s).

Per-tick work added: the Tone B watch, one tonedet_tick per sample from
Phase 3 on (two multiplies, three accumulates; every 80 samples a block
decision); the watch once a data frame (two six-term sums). A retrain
re-initialises v90a_t in place (one memset of it). Nothing heavy; costs are
step 3's.

### Unchanged (by diff)

| | reference | robust 2c |
|---|---|---|
| `make check` (host) | ~/c52v/robv/check.log | the same count lines, by diff, but orig_retrain 222 added and api 303 -> 326; every answering-side line and the subdirectories identical; orig_v90 104, orig_v92 109, orig_v90_te 3, orig_seeds 3 as they were |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 |
| `make bench-bell` | 48f22641, fc125ef2 | the same, host and qemu-arm |
| `make bench-v92c` (the caller) | 506f2bcc (2b) | 506f2bcc, host and qemu-arm (unchanged: nothing in these calls retrains) |
| `make bench-v90c` (the caller) | 695b27e5 | 695b27e5, host and qemu-arm |
| v90d's replay, 106 recordings x 8 configs (rc-scripts/replayproof.sh on a copy of this tree, ~/rb-2c) | sync-build/proof/afterports/manifest.sha256 | MANIFEST IDENTICAL, 1613 of 1613 |
| `make check-ubsan` (copy ~/rb-2c-u) | | ALL UBSAN CHECKS PASS, no runtime error; it found one on the way (below) |
| `tools/check-ports.sh` (copy ~/rb-2c-p, PAR=5, outputs ~/rb-tmp/ports2c) | host gcc, 110 programs | aarch64be-musl, mips-musl, win32-mingw, host-clang: 110 of 110 identical; i686-musl: the full-length run stopped by the coordinator an hour into test_orig_seeds, run instead with -DTEST_SHORT=1 against a host reference built the same way (~/rb-tmp/short), checked by the coordinator |
| `check-arm` (armv5, qemu, TEST_SHORT, once) | 2b's | all pass; the same lines plus orig_retrain 111 (TEST_SHORT: V.90 and its limits) and api 326, by diff |

ubsan's find: `v90arx`'s LMS step on the forward taps (`f[i] -= ...`) and
the feedback taps overflowed int32 once the receiver was wiped (the taps of
a lost receiver run to the end of the range). Both now wrap through
uint32 (defined; the same values whenever nothing overflows, so every
output above is unchanged); the retrain follows anyway.

No answering-side file changed: api/v90modem.c's change is the
originating end code, include/v90modem.h's an appended enum value and
comments; modem/p2call, orig, v90a, v90arx are the calling side's.

### Open

- The answerer drops PCM upstream for the rest of the call when it
  retrains on its own (answer.c: `retrain_ours && !rt_asked`); when its
  check of our upstream fires before our receiver's watch does, a V.92
  call goes on as V.90 after the retrain. The watch's 250 ms collapse rule
  and the held timing loop make that rare in the tests (the wiped and
  burst cases stay V.92); answering side, not touched.
- 9.7/V.92 asks for "the full Phase 2" in a retrain; the answerer sends no
  INFO0 (p2d_init_retrain), and so neither do we. Against a real V.92
  digital modem that sends INFO0d in a retrain, P2L_A_INFO0 would take it
  (got0d is set, a frame is parsed) but this is untried.
- No public `v90m_renegotiate`: our renegotiation is internal
  (orig_renegotiate) and automatic.
- The watch's thresholds (3 dB over for 1 s, 12 dB for 250 ms, 6 dB under
  for 5 s) come from the simulator's lines only.
- V.92 upstream bursts (2b) unchanged.

## answer-fix 0: check-arm runs its programs in parallel (2026-11-19)

Branch `answer-fix`, from `robust` (6d8e259). The user's standing rule:
verification is to be quick. `make check-arm` (and the subdirectories'
`test-arm`) ran their qemu programs one after the other; now
`tools/par-run.sh` runs them `PAR` at a time (default 8), each one's
output (stdout and stderr) to `$(BUILD)/arm_log/<name>.log`, and prints
the logs in the order of `TESTS`, each after its `[arm] name` line, as
before; the exit status says whether any failed. The builds were already
parallel under `make -j`. Host `make check` is unchanged (its programs
print straight to the terminal).

Run once on this tree (`make -j16 check-arm`, armv5, qemu-arm-static):
all pass, and the count lines are those of robust 2c's
(~/rb-tmp/check-arm-2c.log), by diff (test-api-arm's line
comes first now: it runs beside test-arm under -j). Wall time 47 min, on a
host busy with other runs: the last programs to finish were orig_seeds (33
min), orig_bell (36) and orig_v92 (41), then the subdirectories.

## answer-fix 1: the V.92 upstream bursts (answering side) (2026-11-19)

robust 2b left open: at the rates CPd picks at the top of a line (34666 to
37333) the digital modem's PCM upstream receiver had bursts of 30 to 150
check failures, 2 calls of test_orig_seeds' 54 V.92 calls and 2 of the Te
sweep's 243. The user allowed the answering side to change for it.
Harnesses (not in the repository): ~/afix-tmp/h (`er*.c` the
receiver's input against what our transmitter sent, symbol by symbol;
`lsq.c` a least-squares fit of the error on v(n); `ex.c` / `ups.c` one call,
the upstream failures after 11 s; `ts.c` / `sweep92.sh` the Te sweep), and
an instrumented copy of the tree (~/afix-tmp/inst: our
transmitter's symbols recorded, the baseline rule switchable).

### Test first

`test/test_orig_seeds.c`: the exception for upstream bursts (under 200
failures in 10 s passed as "open") is gone; every call must be clean both
ways. On robust 2c: 3 checks, 1 failure (V.92 2 of 54 bad: robbed bit seed
105, 45 upstream failures at 37333; 6 dB pad + robbed bit seed 102, 147 at
37333).

### What was wrong

- **Not the clock.** The ~4.5 s period of the robbed bit seed 105 call is
  near the 4.17 s in which a 30 ppm line slips one sample, and its three
  bursts (17.54, 22.01, 25.89 s) did fall within 0.05 of a sample of the
  slip. But on pad6rb seed 102 (also +30 ppm) the five bursts sit at 0.46,
  0.79, 0.76, 0.33, 0.28 of it, and over 14 s of data the receiver's
  error, its symbol error rate and its large errors are flat against the
  clocks' phase (20 bins). Holding our transmit step at the exact clock
  ratio changes nothing (SER 2.0e-3 -> 2.3e-3). A coincidence of three.
- **The raw error.** Our transmitter's symbols against the receiver's
  slicer: 2.0e-3 symbol errors before the trellis (one every 60 ms), all
  one point off; error RMS 9.7 at the small points (Gaussian, kurtosis
  2.9) where the design assumed 7.6 (kappa 7 at 53 units' spacing). Of the
  excess, a linear fit on v(n) over +-240 symbols takes 9.7 -> 6.6: the
  residual interference of a channel estimate that is not quite the
  channel, mostly at lags 90 .. 130 from the symbol. The trellis
  carries that: what turned errors into bursts was the baseline.
- **The baseline follower** (v92_p34d, reading 63: an eighth of each
  decision's error, for the wander a real U.S. Robotics 5686 showed). Each
  burst began where the baseline had moved: at 17.52 s an error of 127 at
  the point 4220 (+-128 units off there in about half the cases: the
  codec's step, nothing to do with the baseline) moved it by 16 units at
  once, a third of the smallest spacing (53); at 21.99 and 25.88 s errors
  near the decision boundary moved it by 30 to 40 units over a few ms;
  with the baseline that far off, decisions at the small points went wrong
  in runs (each wrong decision moves it further the wrong way), and the
  trellis gave up. With the
  follower off the same calls have 3.6 times fewer symbol errors (2.25e-3
  -> 6.2e-4) and no failures, but the follower is needed for the wander
  (test_v92call's 25d: 25 Hz, 100 units: 47981 bad bits without it).

Variants tried (two burst calls over 40 s, and test_v92call 25d): clipping
the error at 16 / 32 / 64 units (73 .. 197 failures left); ignoring points
beyond 1000 (0 failures on the burst calls, 38328 bad bits on the wander);
weighting by noise / (noise + q) (the wander fails: the large points no
longer follow it); scaling by 2 noise / q only where q > 2 noise (95
failures left on seed 102 at 40 s); that and the taper below (0, 0, and the
wander clean). Only the combination held all three.

### The fix (v92/v92_p34d.c, `base_step()`)

The baseline's step for a decision is still an eighth of its error, less
where the error says little about the baseline:
- where the codec's quantising variance q at the decided point is more than
  twice the line's noise (TRN1d's), scaled by 2 noise / q;
- where the error is beyond a quarter of the spacing on its side (the
  decision may be wrong), falling linearly to nothing at half the spacing.
With a linear codec (`rx_linear`) the step is the plain eighth, as before:
no quantiser there, and on v90d's 106 real recordings (all of them linear
where V.92 data mode is reached) the taper changed the modulus decoder's
flagged frames both ways (8 recording x configuration pairs one more, 15
one fewer; no decoded bit changed) and raised the receiver's error by 0.03
dB: not taken where it was not shown to help. Cost: two binary searches a
symbol (the point's index, the codec step), with a G.711 codec only.

The rate choice was left alone: the design's noise model is 2 dB short of
what the residual interference makes at the top rates, but with the
follower fixed no call of the suites has an upstream error, and a margin
would lower the rate of every V.92 call, real ones included.

### After

| | robust 2c | answer-fix 1 |
|---|---|---|
| test_orig_seeds, V.92 | 3 checks, 1 failure: 2 of 54 calls bad (45, 147 upstream failures) | 3 checks, 0 failures: 0 of 54 |
| V.92 Te sweep, 9 lines x 27 Te (1000 .. 1130 ms) | 2 of 243 bad (6 dB pad seed 25, pad6rb seed 27) | 0 of 243; every rate, down and up, the same |
| the two burst calls, 40 s (failures after 11 s) | 403, 334 | 0, 0 |
| test_v92call | 1054, 0 | 1054, 0; case 19 (the caller's Tone A: what the decoder made of it after the caller left data mode) 1039 -> 1007 "bad" bits; 25d still clean |
| test_answer_v92 | 231, 0 | 231, 0; the decoder's SNR 0.1 to 0.3 dB higher on each line |
| test_answer_lin, test_v92real, test_p2_v92 | | identical output |
| v90d's replay, 106 recordings x 8 configs | | MANIFEST IDENTICAL, 1613 of 1613 |
| v92 subdirectory (host and armv5) | | all pass, the same count lines |
| armv5 (qemu, TEST_SHORT), orig_seeds, v92call, answer_v92, v92real, p2_v92 | | all pass; output equal to the host's; v92call and answer_v92 changed as on the host, v92real and p2_v92 identical to before |

The replay and the Te sweep were run on a copy of this tree
(~/afix-tmp/dev, the same v92_p34d.c) built with the fixes of
answer-fix 2 and 3 as well; neither touches the data mode receiver.

## answer-fix 2: PCM upstream after the answerer's own retrain (2026-11-19)

robust 2c left open: after a retrain the digital modem began of its own
accord (v92d's checks: a rate renegotiation that stalls, the upstream not
holding even the lowest rate), answer.c offered no PCM upstream in the
retrain's Phase 2 (`retrain_ours && !rt_asked`): the call went on as V.90.
Only answer_retrain() (rt_asked) kept it. 9.3/V.92 has every retrain of a
call in which both indicated V.92 use V.92's Phase 2, and that Phase 2,
with Phases 3 and 4 after it, measures the line again.

### Test first

`test/test_orig_retrain.c`: a case "the answerer's own retrain": v92d_retrain()
(p34d_retrain() for V.90) called on the engine, not through answer_retrain(),
as v92d's own checks do. Checked as the other retrains are: data again
within 12 s, 5 s clean both ways, one retrain counted by each side, not
ours, and V.92 still V.92. On robust 2c: 248 checks, 1 failure (V.92: 46666
/ 34666 -> 50666 / 28800 V.90).

### The fix (modem/answer.c, v92/v92_p34d)

PCM upstream is offered again in the Phase 2 of every retrain of a call
that reached V.92 data mode, whoever began it, except when
- that start-up never reached data mode (as before),
- v92d failed (as before: the design found no rate, B1u wrong, ...), or
- v92d retrains because the upstream did not hold even the lowest PCM
  rate in data mode (its rate drops reached 24000): new, `v92d_t.up_floor`.
  There the line has shown it does not carry PCM upstream, whatever a new
  design says, and a V.90 call goes on where V.92 would retrain again.
`answer_t.rt_asked` is gone (nothing else read it).

### After

test_orig_retrain: 248 checks, 0 failures. V.92 "the answerer's own
retrain": 9.9 s, 46666 / 34666 -> 48000 / 34666, still V.92; V.90 7.4 s
(V.90 has no PCM upstream to lose). The other cases print as in robust 2c
(same seeds; the new case is appended). test_answer_v92 (whose retrains
are answer_retrain() and the caller's, and the start-up failures that end
in V.90), test_v92call, test_answer_lin, test_v92real: output identical to
answer-fix 1's; test_answer, test_answer_v90, test_answer_v34 identical to
before. armv5 (qemu, TEST_SHORT: V.90's cases): orig_retrain, answer_v92,
answer pass. v90d's replay: MANIFEST IDENTICAL, 1613 of 1613 (no
recorded call has a retrain of v92d's own from data mode; the retrains in
them are the callers' Tone A, or a start-up that failed).

## answer-fix 3: INFO0 in a retrain's Phase 2 (2026-11-19)

robust 2c's open point: 9.7/V.92 says the modems "proceed in accordance
with the full Phase 2 start-up procedure" in a retrain, and our answerer
(p2d_init_retrain) sends no INFO0d then, so our caller sends no INFO0a.

### The text

V.92 (11/2000), 9.7.1.1 (the digital modem begins): silence 70 ms, Tone B,
detect Tone A, then "condition its receiver to detect a Tone A phase
reversal and proceed in accordance with the full Phase 2 start-up
procedure"; 9.7.1.2, 9.7.2.1, 9.7.2.2 the same, from the tones. "Full Phase
2" is 9.3's title ("Full Phase 2 - Probing/ranging"), against 9.4's "Short
Phase 2 - Ranging": the retrain is a Phase 2 with probing, not the short
one, and it is entered at the tones and their reversals, as V.90's 9.5
enters it at 9.2.1.1.3 (after INFO0). Nothing in 9.7 sends INFO0.

### The recordings

`v90m-fields -9` over all 106 recordings of live-050 (every CRC-clean INFO
frame the caller sent, every reversal of its tone): 35 of them have a
second Phase 2 (two INFO1a: a retrain), from U.S. Robotics 5686s (usr*,
pd_*, pw_*, f*), the 1998 U.S. Robotics (x2b), Conexant (cxfail, exp_*),
and others (p*a, v92a..e, fin_48000). In none did the caller send INFO0a
in the retrain: each answered our INFO0-less Tone B with Tone A and its
reversals (e.g. usr7: retrain at 16.53 s, Tone A reversals at 16.68 and
17.44 s, INFO1a at 18.20 s; exp_1 waited 2.8 s, then the same). Real
callers do what our answerer expects. What real digital modems send cannot
be seen in these recordings (every one is of our answerer).

So the answerer is not wrong and is not changed. The caller must take
either form, for a server that sends INFO0d all the same.

### Test (a check, not a fix)

`test/test_orig_retrain.c`: a case "answerer retrains with INFO0d": after
answer_retrain() the answerer's Phase 2 is restarted as at the start of the
call (p2d_init with INFO0d, V.92 offered as before). Our caller's
P2L_A_INFO0 takes an INFO0d without the acknowledge bit for one that has not
heard ours and answers with INFO0a, and the new INFO0d holds. Checked: the
answerer got INFO0a, data again, 5 s clean both ways, V.92 still V.92. It
passes as the caller was (V.90 7.9 s, 52000 -> 46666; V.92 9.7 s, 48000 /
34666 -> 52000 / 34666; on robust 2c's caller the same): only the comments
of p2call say so now. test_orig_retrain: 276 checks, 0 failures; armv5
(qemu, TEST_SHORT, V.90's cases with this one): passes. test_p2call,
test_p2v90, test_p2v92a: pass, unchanged.

### Open (answer-fix)

- The V.92 design's noise model is about 2 dB short at the top upstream
  rates on the simulator's lines (error RMS 9.7 at the small points, 7.6
  assumed): residual interference of the channel estimate, at lags 90 ..
  130. The trellis carries it now that the baseline no longer turns single
  errors into runs; a margin would lower every V.92 call's rate. Measured
  on the simulator only.
- base_step()'s taper is not used with a linear codec: on v90d's real
  (linear) recordings it changed nothing that was decoded and moved flagged
  frames both ways. Bursts like the simulator's have not been seen there.
- What a real digital modem sends in a retrain's Phase 2 is unknown (every
  recording is of our answerer); our caller takes either form, tried only
  against our own answerer made to send INFO0d.
- v92d's own retrain for an upstream that does not hold 24000 still drops
  PCM upstream for the rest of the call (up_floor), by design.

## answer-fix: the end-of-branch checks (2026-11-19)

On the branch's last commit (3f271f4; ubsan and the port matrix on a copy
of its tree, ~/afix-tmp/final, identical but PROGRESS.md).

| | reference | answer-fix |
|---|---|---|
| `make check` (host, inside v90d-split's `make check`) | robust 2c's count lines (~/rb-tmp/check-2c.log) | the same lines, by diff, but orig_retrain 222 -> 276 (answer-fix 2, 3); every answering-side line identical (orig_seeds 3, its exception gone); subdirectories identical; fixtures all pass; realcheck 666; api 326; symbols 555 internal |
| `make bench-v90` | 8e3136e9 x6 | 8e3136e9 x6 (a V.90 call: nothing here acts on it) |
| v90d's replay, 106 recordings x 8 configs (v90d-split d707463 built against this tree; replay_all050.sh's files, run 12 at a time) | sync-build/proof/afterports/manifest.sha256 | MANIFEST IDENTICAL, 1613 of 1613 |
| `make check-ubsan` | | ALL UBSAN CHECKS PASS, no runtime error |
| `tools/check-ports.sh` (PAR=5, JOBS=2, outputs ~/afix-tmp/ports) | host gcc, 110 programs | aarch64be-musl, mips-musl, win32-mingw, host-clang: 110 of 110 identical; i686-musl with QUICK: 99 of 110 identical, the 11 of tools/ports-slow.txt skipped (orig_seeds, orig_retrain, orig_v90_te and realcheck whole: the full i686 run would have passed the hour); ALL PORTS PASS |
| v90d-split (d707463, unchanged, nothing committed) built against this tree (LIBV90M=the worktree, as sync-build/proof/afterports.sh; ~/afix-tmp/v90dproof.sh, outputs ~/afix-tmp/proof) | sync-build/proof/afterports | `make check`: ALL HOST CHECKS PASS, v90d's own count lines identical, the library's as above; `make check-arm` (the library's now in parallel): ALL ARM CHECKS PASS, v90d's own lines identical, the library's those of robust 2c's check-arm but orig_retrain 111 -> 138; bench-v90 8e3136e9 x6; replay MANIFEST IDENTICAL 1613; `fw release` and `fw test-release` exit 0, test-release's count lines identical |

No recording's output changed, so there is no recording / before / after
table to give: the answering-side changes act only where no recorded call
goes (a G.711 codec in V.92 data mode; v92d's own retrain from V.92 data
mode; an INFO0d in a retrain).

For v90d main (separate repository, its own copies of the answering
modem): answer-fix 1 and 2 are patches to its modem/answer.c, answer.h,
v92/v92_p34d.c and v92_p34d.h, which are the library's but for the
portability fix of ports-2 in v92_p34d.c (et_g's update); answer-fix 3
changes nothing there. Its HT802 records linear (`rx_linear`), so fix 1's
step is the old eighth on the box unless the codec runs G.711; fix 2 acts
on any V.92 call v92d retrains of its own accord.

## cpu 1: the calling side's ARM cost, bit-exact part (2026-11-19)

The box's budget (v90d-split HARDWARE.md, PROGRESS.md): DVF9918, ARM926EJ-S
at 400 MHz, no divide, no FPU. The answering modem's V.90 data mode is 39.5 M
ARM instructions per second of line under qemu and 32 % of the CPU on the
box, so the box is about 123 M qemu-counted instructions a second (1 % =
1.23 M): 15.4 k per sample, 1.23 M per 10 ms block. v90d serves the line in
blocks of 80 samples (10 ms), driver latency 240 samples (30 ms): what
matters for a long tick is the block it lands in. The answering modem's own
heaviest tick (`bench_v90`'s 40 s V.90 call, counted with tools/qprof): 173 k,
then 158 k, 144 k, 110 k; its V.34 receiver's periodic ticks 87 k.

How it is measured now: `make prof-v90c` / `prof-v92c` (PPM=0 or 100):
rec_v90c records a call with 4 s of data mode, bench_v90c `-t` replays it
under qemu (`-d in_asm,exec,nochain`, no single-instruction blocks) calling
a mark before every tick, tools/qprof turns the log into the instructions of
every tick and, over seconds 2 to 3 of data mode, per function. Checked
against `-one-insn-per-tb` on 2000 samples: 640601 against 640775.
One run of a whole call takes a minute or two (cost-v90c's per-tick runs
took longer each). The bench's own loop (fread, the hash) is in the counts
as it was in cost-v90c's, about 1 M a second.

Profile at the start (answer-fix, seconds 2 to 3 of data mode, M ARM
instructions a second):

| | V.90 0 ppm | V.90 100 ppm | V.92 0 ppm | V.92 100 ppm |
|---|---|---|---|---|
| total | 78.8 | 89.4 | 104.1 | 114.0 |
| v90arx_tick (equaliser 48 + 128 taps, its LMS, interpolator 2 x 64, Sd) | 28.6 | 28.6 | 28.6 | 28.6 |
| estimate (channel estimate, 128 taps, NLMS, two 64-bit divisions a symbol) | 27.5 | 27.5 | 27.5 | 27.5 |
| v92_enc_symbol (precoder and prefilter, LZ1 / LP1 / LZ2 / LP2) | | | 19.0 | 19.0 |
| dfe_design (the equaliser computed again, every 2048 / 512 symbols) and its 64-bit divisions (__udivmoddi4) | 4 | 17 | 7 | 16 |
| pcmtx_out (the upstream interpolator, 2 x 64 taps a sample) | | | 5.6 | 5.6 |
| ec_tick (96 taps) | 4.4 | 4.4 | 4.4 | 4.4 |

The heaviest ticks: 9.30 M at the end of the DIL (v90dil_analyse: 41 gain
candidates x 576 measurements, each a 64-bit division and a binary search);
0.90 M each time the equaliser is computed (0.55 M of it 64-bit division);
0.59 to 0.90 M the CP design.

What changed (none of it changes a result; every bench hash and the test
output of every caller program tried stays as it was):
- `estimate`: the two loops over the symbol ring in two runs that do not
  wrap, four taps a pass; the update without a 64-bit product per tap (es =
  256 q + rr: (es s) >> 8 = q s + ((rr s) >> 8), low 32 bits); the division
  by L by L's reciprocal (`l_rcp`, set once).
- `equalise`, `produce`, `ec_tick`, `pcmtx_out`, `v92_enc_symbol` (hdot16 /
  hdot32): four taps a pass, rings read in two runs instead of an index mask
  per tap, products accumulated one by one (gcc's SMLAL into one sum).
- The equaliser's design (dsn_start / dsn_step, run whole by dsn_now here):
  R built along its diagonals in 32 bits, the Cholesky factor, z and f
  multiplied 32 by 32 bits (bounded under 2^29 / 2^30; checked, and a value
  that does not fit is a failed design: none can), every division by
  G's diagonal and by the bias through a reciprocal (`sdiv_rcp`: the high
  word of |n| rcp is the quotient or one less), so 50 64-bit divisions
  instead of 1400. The band edge's image as one value per diagonal.
- The DIL analysis: `nearest_g` finds nearest(t1, (l << 16) / g) without
  the division (t1[i] 2^4 <= floor(L / g) is t1[i] 2^4 g <= L, and the same
  for the rounding), walking from the last answer; checked against nearest()
  on 4e7 random (l, g) pairs, both laws: no difference. A gain candidate's
  cost stops being summed once it can no longer win (its terms are not
  negative).
- `V90M_MEM_SIZE` 112 -> 120 KiB: `v90arx_t` holds the design's working
  state (`v90arx_dsn_t`, 6216 octets: R / G packed, the estimate taken,
  solutions) so that the next commit can spread it over symbols; `orig_t`
  109920 -> 116160 octets on x86-64 (115880 on ARM); `answer_t` unchanged.
- `rig/bench_v90c` -t / -n, tools/qprof.c, `make prof-v90c` / `prof-v92c`.

After (same recordings, M a second, % of the box at 123 M):

| | V.90 0 ppm | V.90 100 ppm | V.92 0 ppm | V.92 100 ppm |
|---|---|---|---|---|
| data mode, before | 78.8 (64 %) | 89.4 (72 %) | 104.1 (84 %) | 114.0 (92 %) |
| data mode, after | 54.5 (44 %) | 59.0 (48 %) | 69.5 (56 %) | 73.7 (60 %) |
| heaviest tick, before | 9.30 M (DIL) | 9.27 M | 9.30 M | 9.30 M |
| heaviest tick, after | 1.14 M (DIL) | 1.17 M | 1.14 M | 1.15 M |
| then | 0.62 M (CP design), 0.39 M (equaliser) | 0.90 M, 0.40 M | 0.72 M, 0.40 M | 0.59 M, 0.40 M |

Unchanged: bench-v90c 695b27e5 and bench-v92c 506f2bcc (host and qemu-arm);
the replays at 0 ppm f3111e17 / ca4ae751; test_v90arx, v90arx_robust,
v90dil, v90a, pcmtx, v92a, orig_v90, orig_v92 print byte for byte what
answer-fix's build prints; bench-v90 8e3136e9 x6 (ec.c is the answerer's
too: the same sums).

## cpu 2: Phase 2 and V.34 set-up ticks, shared code, bit-exact (2026-11-19)

With the DIL's tick spread (cpu 3), the heaviest ticks left were shared
code, the answering modem's as much as ours (its own heaviest two, 173 k
and 158 k, are these):

| tick | what | before | after |
|---|---|---|---|
| start of Phase 2 | probetx_init (the L1 / L2 table: a modulo per tone per sample, cos160's 64-bit divisions), dpsktx_init (240 64-bit divisions) | 189 k | 98 k |
| end of probing | the block DFT of the last 20 ms (25 bins x 160 samples at once), then the analysis (powers() four times a bin, log2 by a 64-step loop) | 223 k | 82 k |
| V.90 data mode begins | v34_params_init's shell tables (loops over every k with tests inside) | 167 k | 79 k |

What changed (the same results: bench-v90c 695b27e5, bench-v92c 506f2bcc
host and qemu-arm; bench-v90 8e3136e9 x6; test_p2call, p2v90, p2v92a,
orig, orig_v34, v8orig print what answer-fix's build prints):
- modem/probe.c: cos(2 pi k / 160) as a constant table (generated by the
  code it replaces); the generator by tone, the analyser's DFT summed
  sample by sample as the samples come (the same sums in the same order;
  proberx_t holds the 26 running bins instead of the block and the cosine
  table: 224 octets smaller, so `answer_t` 106552 -> 106104 on x86-64);
  proberx_bin() gives snr, level and power from one powers(); log2q8 by
  halving.
- modem/p2call.c: analyse() through proberx_bin().
- modem/dpsk.c: dpsktx_init divides in 32 bits where the product fits.
- v34/v34_params.c: the convolution loops over the k that pass the tests.

## cpu 3: the heavy computations spread over ticks (2026-11-19)

Target for the heaviest tick: under 100 k ARM instructions, i.e. under 8 %
of the 1.23 M a 10 ms block of the box has, below the answering modem's own
heaviest (173 k before cpu 2), and some 6 average samples of V.92 data mode
(a block of V.92 data mode, 0.6 M after cpu 1, has room for it twice over).

What is spread (these change when things happen, so they change numerics):
- The equaliser's design (`v90arx`): a job of steps, one a symbol, from
  the estimate taken when it starts: start (R's first column, diagonal,
  band edge, load, scale, the first 14 of R's 48 diagonals), fill (the
  other 34), Cholesky (columns by a budget of 9000 multiplies a step, 4
  steps), solve (z, f, the bias, the forward taps), feedback taps (then
  both sets go in): 8 symbols. The first equaliser, at the end of TRN1d's
  estimate, likewise: the estimate goes on and the equaliser starts on the
  symbol its design ends on. A tick in which the receiver took a step
  (`v90arx_t.dsn_ran`) takes no step of v90a's jobs.
- The DIL's analysis, CPt's design and CP's design (`v90dil_analyse_begin
  / _step`, `v90dil_design_begin / _step`; the whole-call functions run
  them to the end): at the end of the DIL, S goes out at once as before and
  the analysis (begin, one gain candidate a step, the common gain, one
  interval a step twice, the robbed bits) and CPt's design (one trial
  constellation set a step) follow, some 60 ticks (8 ms); CP's design
  from TRN2d's noise the same way. Where a result is wanted before its
  job is done (our S-bar before CPt, Ri-bar, our SCR's 2 s) the rest runs
  then (none of the tests needed it).
- `v90a_t` holds the job (a union of the analysis and the design: 2.8
  kbytes), `v90arx_dsn_t` grows by R's first column and diagonal values
  (768 octets): `orig_t` 116160 -> 118704 octets on x86-64 (118416 on
  ARM), within the 120 KiB of cpu 1.

Found on the way (test_orig_retrain's "lost past the limit", V.92, failed
with the new timing: the answerer never saw our cleardown): with the
receiver wiped, line_watch held the timing loop at the collapse, let it go
at the first data frame whose error fell under 16 times the design's,
and the half-recovered receiver (its estimate regrown from nothing) moved
the clock -25 ppm in 150 ms; the cleardown's CPus then went out where the
digital modem did not listen. answer-fix's run had moved it -2.4 ppm (on
8 seeds of that scenario: answer-fix 8 of 8, this commit without the fix 7
of 8, with it 8 of 8). Now the loop stays held after a collapse until the
error is back within twice the design's (`col_hold`).

Heaviest ticks (prof-v90c / prof-v92c, the whole call):

| | before (answer-fix) | after cpu 1 | after (this) |
|---|---|---|---|
| V.90 0 ppm | 9.30 M (DIL), 0.62 M (CP design), 0.39 M (equaliser) | 1.14 M, 0.62 M, 0.39 M | 98 k (Phase 2's start), 82 k (end of probing), 81 k (a step of the equaliser's design) |
| V.90 100 ppm | 9.27 M, 0.91 M, 0.91 M | 1.17 M, 0.90 M, 0.40 M | 98 k, 82 k, 80 k |
| V.92 0 ppm | 9.30 M, 0.72 M, 0.91 M | 1.14 M, 0.72 M, 0.40 M | 98 k, 82 k, 80 k |
| V.92 100 ppm | 9.30 M, 0.59 M, 0.91 M | 1.15 M, 0.59 M, 0.40 M | 98 k, 82 k, 81 k |

Data mode is as cpu 1 left it (the designs cost the same, spread):
54.6 / 58.8 M a second (V.90, 0 / 100 ppm), 69.4 / 73.9 M (V.92).

Rates (the same tests, answer-fix's build against this one; each call
is one seed, so single calls move by a step either way):
- test_orig_seeds (9 lines x 6 seeds each, with the mean added in a copy
  of the test): V.90 down 46073.8 -> 46123.1, up 28800.0 -> 28888.9; V.92
  down 45728.0 -> 45777.4, up 34765.2 -> 34765.1; 0 of 108 bad either way.
- test_orig_v92's table (11 calls): down 47111 -> 47444, up 33244 -> 33911.
- test_orig_retrain's tables (44 rate pairs): down 47666 -> 47818, up
  31642 -> 31703.
- test_orig_v90's table, its 9 lines: down 46370 -> 46222 (the 6 dB pad
  45333 -> 44000, robbed bit 49333 -> 48000, pad and robbed bit 40000 ->
  41333), the A-law line's upstream 31200 -> 28800.
- bench-v90c fa7486c7 (46666 / 28800 as before), bench-v92c 22bf87ff
  (46666 / 34666; it was 48000 / 36000): host and qemu-arm the same.

Tests (short set): v90arx, v90arx_robust, v90dil, v90a, orig_v90, v92a,
orig_v92, p2call, p2v90, p2v92a, orig_retrain, orig_v90_te, realcheck,
orig, orig_v34, api, orig_bell, orig_seeds: all 0 failures. bench-v90
8e3136e9 x6.

## cpu 4: retrain and renegotiation ticks measured (2026-11-19)

`tools/rec_v90c ... rt` / `rn`: 2 s into data mode the recorded caller
retrains (orig_retrain) or renegotiates (orig_renegotiate, no ceiling);
the magic ends in 2 and the header says which and before which tick;
`rig/bench_v90c` asks for the same at the same tick, so the replay stays
the call (V90C0001 recordings as before). Counted with prof (every tick of
the whole call, 2 km, +100 ppm, 12 s after the event):

| call | event | heaviest tick from the event on | data mode at the end |
|---|---|---|---|
| V.90 | our retrain | 97.7 k (the retrain itself: v90a_init's clearing of 50 kbytes), 95.2 k (+70 ms, Phase 2's start), then 82 k | 59.2 M a second |
| V.90 | our renegotiation | 81.8 k (steps of CP's design and of the equaliser's) | 59.3 M |
| V.92 | our retrain | 97.7 k, 95.2 k, then 82 k | 74.8 M |
| V.92 | our renegotiation | 77.7 k | 75.2 M |

Nothing new is heavy: the retrain's ticks are those of a call's start.
Tried and dropped (each measured on the tests): the channel estimate on
every other data symbol (-6.4 M a second; test_v90arx's MSE 1.5 to 2.5 dB
worse at 30 to 80 ppm, one check failed, test_orig_v92 one failure); the
equaliser's LMS on every other symbol (-4 M; orig_seeds' mean downstream
46123 -> 45185 and one bad call of 54); the interpolators (receiver and
PCM upstream) two taps a pass (gcc's code for two 64-bit sums per tap got
worse: +1.3 M).

## cpu: the end-of-branch checks (2026-11-19)

The calling side's cost on the box (qemu-counted ARM instructions; the box
is about 123 M a second, 1 % = 1.23 M; seconds 2 to 3 of data mode, 2 km):

| | answer-fix | cpu | % of the box, before -> after |
|---|---|---|---|
| V.90 data mode, 0 ppm | 78.8 M/s | 54.6 M/s | 64 % -> 44 % |
| V.90 data mode, 100 ppm | 89.4 M/s | 58.8 M/s | 72 % -> 48 % |
| V.92 data mode, 0 ppm | 104.1 M/s | 69.4 M/s | 84 % -> 56 % |
| V.92 data mode, 100 ppm | 114.0 M/s | 73.9 M/s | 92 % -> 60 % |
| Bell 103 / 212A data mode (cost-bell) | 9.85 / 4.43 M/s | 9.84 / 4.43 M/s | 8.0 / 3.6 % |
| heaviest tick, V.90 and V.92 calls | 9.30 M (DIL end) | 98 k (Phase 2's start) | 755 % -> 8 % of a 10 ms block |
| heaviest tick of our retrain / renegotiation (cpu 4) | (DIL 9.3 M again; CP design 0.6-0.9 M) | 98 k / 82 k | 8 % / 7 % of a block |
| Bell heaviest tick | | 2.3 k / 2.8 k | |
| the answering modem's heaviest tick (bench_v90, shared code of cpu 2) | 173 k | 87 k | its output unchanged |
| the answering modem, 40 s V.90 call | 1391 M | 1365 M | |

Where the 100 ppm data mode goes now (M/s, V.92): equaliser 48 + 128 taps
and its LMS, interpolator, Sd watch 24.1; channel estimate 13.1; V.92
precoder / prefilter 10.1; upstream interpolator 4.2; canceller 3.6; the
equaliser's designs (16 a second at 100 ppm, 4 at 0 ppm, some 350 k each)
5.5; the rest under 3 each. What is left is the per-tap work of the
algorithm (3.5 to 7 instructions a tap); halving the estimate's or the
equaliser's adaptation was tried and costs SNR or rate (cpu 4).

Checks on the branch's last commit:

| | result |
|---|---|
| `make check` (host) | ALL HOST CHECKS PASS; its 110 count lines are answer-fix's (~/c52v/afix/check.log) line for line, every answering-side line among them; fixtures pass |
| `make check-arm` (parallel, BUILD=build/armchk) | ALL ARM CHECKS PASS; the counts are the host's but for the programs that run shorter on ARM, as before (orig_retrain 138, v32's and v34's) |
| `make check-ubsan` | ALL UBSAN CHECKS PASS, no runtime error |
| `make bench-v90` | 8e3136e9 x6 |
| `make bench-bell` | 48f22641 / fc125ef2, host and qemu-arm |
| bench-v90c / bench-v92c | fa7486c7 / 22bf87ff (cpu 3 changed them; host and qemu-arm the same) |
| v90d's real-recording replay (afix-tmp/replaypar.sh against this tree, 106 recordings x 8 configs) | MANIFEST IDENTICAL 1613 (to sync-build/proof/afterports/manifest.sha256, sorted) |
| tools/check-ports.sh (PAR=3 JOBS=2, QUICK=i686-musl, SKIP=arm_test_orig_seeds) | host-clang, aarch64be-musl, mips-musl, win32-mingw: 109 of 110 identical, orig_seeds skipped; i686-musl: 98 of 110 identical, 12 skipped; ALL PORTS PASS |

`tools/check-ports.sh` takes SKIP="prog ..." now: programs every target
but the reference skips (orig_seeds here: covered on the host and on ARM).

Memory: `V90M_MEM_SIZE` 112 -> 120 KiB; `orig_t` 109920 -> 118704 octets
on x86-64 (109632 -> 118416 on ARM): the equaliser design's state (7 k),
the DIL / CP jobs' (2.8 k); `answer_t` 106552 -> 106104 (proberx_t smaller).

Open:
- V.92 at 100 ppm is at 60 % of the box (56 % at 0 ppm): one call fits
  with room, two V.92 calls at once do not. What is left is per-tap work;
  further cuts would be numeric (a shorter estimate or feedback filter, a
  slower design rate at high clock offsets), each to be weighed on rates.
- The estimate's per-symbol 64-bit division (some 1.4 M a second) and the
  timing loop's DFT (0.9 M) could still be made cheaper exactly.
- All counts are qemu's instruction counts; cycles on the ARM926 (cache,
  multiply latency) have not been measured on the box.

## release-0.3.0: version, changelog, documents and header reviewed (2026-11-19)

Branch `release-0.3.0` from cpu (e6e03c1). No signal-path code changed.

- Version 0.3.0: header, `v90m_version()`, Makefile (so the .pc file and
  the shared library's name). test_api checks the string, the header's
  numbers and that the two agree: 2 failures against 0.2.0, then 0.
- `CHANGELOG.md` (new): 0.1.0, 0.2.0, 0.3.0 for users of the library,
  from this file and the history.
- `README.md` rewritten as one document: the standards table with both
  roles; what was tried on real modems (answering) and what only in
  simulation (all of the calling side); building, cross builds, the port
  matrix; memory (120 KiB bound, 118816 octets on x86-64, 118504 on ARMv5)
  and the ARMv5 instruction counts of the cpu section; the limitations;
  examples. Gone: "everything inside [the static library] is local" (it
  is renamed `v90m__i_*` since ports-2) and the scattered stage notes.
- `include/v90modem.h`: per field and function, which role it applies to
  and every error return, checked against api/v90modem.c by a new
  test_errors in test_api (94 checks). Found:
  - `v90m_reset(m, NULL)` crashed (create, init and `v90m_ec_reset` give
    `V90M_EINVAL`): seen failing (SIGSEGV in v90m_reset), now
    `V90M_EINVAL`. The only code change outside the tests.
  - Comments that did not match the code, corrected: `v90m_stats.echo_delay`
    is answering V.90 / V.92 only (always -1 calling); `renegotiations`
    counts calling V.32bis too and not answering V.34; `odp_bypass` is
    answering only; `codec_linear_rx` and `max_tx_code` are answering
    only; the rate ceilings mean send / receive in V.34, V.90 and V.92 in
    either role (V.92's PCM upstream under max_down when calling); the
    header named a `v90m_process()` that does not exist and said
    `v90m_ec_*` make callbacks (they make none); the first line still said
    Bell was answering only. `v90m_ec_send_break` / `_get_break` return
    0 / -1 and 1 / 0, not V90M_* codes: documented as they are.
- `DESIGN.md`: 4.2b moved before 4.2c (the order was 4.2a, 4.2c, 4.2d,
  4.2b); stale: the symbol hiding (renamed, not localized), the API layer
  (also `orig`), `v32/` (both roles), 4.2c "no retrain, no renegotiation of
  our own" (robust 2c), 4.2a's PCM upstream after retrains (answer-fix 2)
  and the baseline follower (answer-fix 1), where measurements are kept,
  the `v92 = on` of v90d's config; testing: port matrix, ubsan,
  realcheck.
- `examples/call_loop.c` (new): a caller and an answerer back to back
  through the public API, `-m` for what the caller offers (v92 by
  default; v90, v34, v32, v22, bell212, bell103 all connect, text both
  ways, cleardown where there is one). `make` builds the examples now
  (`all: lib examples`); `make check` runs call_loop (test-examples).
- `libv90modem.pc.in`: the description names Bell and both roles.

Hygiene: no TODO / FIXME / XXX / `#if 0` in the library. The diagnostic
prints in modem/p34d.c, modem/v34dx.c and v42/v42_lapm.c are all behind
`P34D_TRACE` / `V42_XID_TRACE` (p34d's own under the V.42 macro: odd, left
as it is, signal-path file); the `#ifndef` knobs (`V22_BELL_MS`,
`V90_MAX_TX_CODE`, `V92_DESIGN_PACE`, `V92_DSN_ECHO_DIV_LIN`, `LOSS_MSE`,
`V92D_PROBE`) are the documented bench / EXTRA_CPPFLAGS ones. `getenv`
only in tools and tests. `make clean` leaves nothing (top level and the
five subdirectories). `make install DESTDIR=... PREFIX=/usr` installs the
header, `.a`, `.so.0.3.0` with its two links, the .pc file and
v90m-replay; call_loop built against it with pkg-config, shared and
`--static`, runs; `make uninstall` removes every file. gcc and clang
(-std=c99 -O2 -Wall -Wextra -Werror): library, examples, v90m-replay,
v90m-fields, every rec_* / bench_*, qprof, ber_v90, printf-probe: no
warning; the header alone also under -pedantic and as C++98.

Checks:

| | result |
|---|---|
| `make check` (host, from clean) | ALL HOST CHECKS PASS; 110 count lines, those of cpu (~/c52v/cpuv/check.log) but `api` 326 -> 421 (version +1, test_errors +94); fixtures pass; call_loop V.92 |
| `make check-arm` (BUILD=build/armchk) | ALL ARM CHECKS PASS; the host's counts but the programs shortened on ARM, as before (orig_retrain 138, test_bench, test_ber, test_call, test_caller, test_startup) |
| `make check-ubsan` | ALL UBSAN CHECKS PASS, no runtime error |
| bench-v90 | 8e3136e9 x6 |
| bench-bell | 48f22641 / fc125ef2, host and qemu-arm |
| bench-v90c / bench-v92c | fa7486c7 / 22bf87ff, host and qemu-arm |
| port matrix | not run to the end: stopped by the session for low system memory (swap full) while building; to be run again |

Open, for the owner before 0.3.0:
- `v90m_ec_send_break` returns -1, the value of `V90M_EINVAL`, where
  `V90M_ESTATE` is meant; changing it changes the API's values.
- The answering role takes `modulations` = 0, and `V90M_MOD_V92` without
  `V90M_MOD_V90`, without complaint; the calling role refuses both.
- In check_cfg, the calling role's `V90M_ENOTSUP` for "nothing it can do"
  and for unknown bits can no longer happen (every bit is callable);
  harmless dead code.
- DESIGN.md has no section on the V.90 analogue (calling) side, nor on
  V.21 / V.22bis / V.32bis / V.34 calling: 4.2c leans on it.

## x2 4: the API, and negotiation with V.34 fallback (2026-11-20)

x2 offered on the answering side, up to the client's marker; no PCM
training yet (ANS_X2 ends the call with V90M_END_X2_FAILED until x2 6).
The facts are docs/history/x2-facts.md "Phase 1" and "Phase 2 marker".

- API: `V90M_MOD_X2` (0x0100, not in the defaults; without V.34 or in the
  calling role `V90M_ENOTSUP`), `V90M_CONN_X2` and `V90M_END_X2_FAILED`
  appended. `ANS_MODE_X2`, `ANS_MOD_X2`, `ANS_X2`; `answer_t.x2_offered`
  (V.34 too, and a mu-law line: answer_v90_cfg recomputes it) and
  `x2_failed`.
- V.8 (v8ans): with x2 offered, a CM with V.34 and no PCM, and V.34 the JM's
  choice: the carrier is inverted after each complete JM; a CM with the
  PSTN access octet gets it back with the digital-network bit (0d -> 8d).
  `v8ans_offer_x2`, `v8ans_x2`.
- Phase 2 (p2ans, through v34ans_cfg.x2): INFO0a max_sr_diff = 4, cme = 1.
  From the start of our L1 the DPSK receiver hunts a 7-bit INFO frame
  (`p2ans_x2_info_frame` builds one: lead-in 11, fill, sync, body, CRC,
  fill; body 4d -> CRC 24dd as the facts say); one that checks out with 6
  in bits 1..3 by 960 ms after L1 began: P2N_X2, silent, `x2_marker`;
  v34ans V34A_X2 -> answer ANS_X2. L2 goes on meanwhile exactly as
  without x2; the window also closes when the tones restart (L2 unanswered:
  760 ms + round trip after L1 began) or the caller's L1 / L2 have been
  analysed.
- The JM flip broke our own V.8 receivers (sim/v8call, and so modem/v8orig
  would have): across the 180 degree step the V.21 correlators read space
  for more than half a bit, and the receiver took a character FF out of the
  ten ONEs, which left the next JM without its preamble (21 JMs sent, no
  CJ). `v8rx_t.jm_flip` (set by v8call and v8orig, not by v8ans) counts an
  FF character as the mark it is; no V.8 octet is FF. The answerer's V.8
  receiver is unchanged.
- tools: v90m-replay `-x`; trace / v90m-fields know the phase X2.

Tests (seen failing first, then passing): test_api (refusals, enum
values; the "unknown bit" check moved from 0x100 to 0x200), test_v8ans (JM
negated after each complete JM, octets unchanged; the access octet 8d; A-law
and a PCM CM sample for sample as without x2), test_p2ans (the frame's bits
and CRC; INFO0a bits 21..24 = 0 0 1 1 and the rest of Phase 2 the same
samples from L1 on; marker 4d at 170 and 260 ms and 0d accepted; a bad CRC,
a marker 900 ms after the end of L1, 49 and 41 refused, each with our
samples from L1 on identical to a run without x2), test_answer_x2 (new:
sim/v34peer with x2 offered connects V.34 28800 / 28800 at 3200 as without,
0 bad bits; with the access octet too; A-law: all 87752 samples as without
x2; a V.90 caller: all 64000 as without; a marker caller: ANS_X2 244 ms after
L1 began, the call ends, x2_failed). `make check`: ALL HOST CHECKS PASS.

Replay proof, every recording of ~/sync-build/live-050 (106), main
(19ba2c6, a worktree) against this branch, each tool in its own directory
(bits.txt kept), 8 at a time:

    ls live-050/*.raw | xargs -P 8 ... v90m-replay [FLAGS] REC > NAME.txt; bits.txt -> NAME.bits
    FLAGS: main "" and "-9"; branch "", "-9", "-x", "-x -9"

| comparison | outputs (106 .txt, 21 / 40 .bits) |
|---|---|
| main vs branch, no flags | identical, byte for byte |
| main -9 vs branch -9 | identical, byte for byte |
| main vs branch -x | 100 identical; 6 differ (ab_cc_v34, ab_cl_v34, v34a, v34b, v34c, x1: the V.34-only callers) |
| main -9 vs branch -x -9 | the same 6 |

In those 6 only the count of transmit samples that differ from the
recording changes, by 2094 each (x1: 110394 -> 112488): the second JM
negated (1867 samples; JM 233 ms, the first difference at its start, e.g.
x1 8.234 s, JM began 8.000 s) and INFO0a from bit 21 on (227). Every
decision, phase, rate and bit received is the same; .bits identical. None
of the recordings is an x2 caller, so no marker was taken.

`make check-arm` (armv5 uclibc, qemu): ALL ARM CHECKS PASS. test_answer_x2
was run in full (46 checks). It now has a TEST_SHORT version for ARM that
leaves out the access-octet call and the A-law pair: 30 checks, 3.4 s.

## x2 5: records with their CRC, and the downstream mapper at 32000 (2026-11-20)

Written up at release (x2 9); from the Task 5 report, commits b4b95cf and
109c2d0. No change to what the modem sends yet: the pieces Task 6 runs.

- `modem/x2rec`: `x2_crc16` (from FFFF, 0x8408 reflected, no final
  inversion), `x2rec_build` (17 ones, a zero, each word LSB first and a
  zero, the CRC of the word bits LSB first; `ack` sets word 0 bit 15) and
  `x2rec_parse` (from the sync; `X2REC_SHORT`, `X2REC_BAD`), with the MP
  field accessors.
- `modem/x2tab`: levels A (`a5 a7 ad af b7 bd c5 cf e5`) and levels B,
  and the levels received / octets sent behind each pad (the octets sent
  derived by `tools/x2_pad.c`'s rule, equal to the Quad's).
- `modem/x2map`: index 1 (24 bits a frame): six positions, signs
  differential, the free sign by the levels A rule (negative when the
  other five sum to more than 805 in 14-bit units) or, in the data levels,
  the running sum. `x2map_unframe` takes the octets as received.

Tests (each seen failing first: `test_x2rec: 29 checks, 20 failures`,
`test_x2map: 40 checks, 35 failures`):

    test_x2rec: 29 checks, 0 failures   (the facts' four CRCs 14AD 92B0 73F3 F9A6; 2000 random records round trip, every single flipped bit BAD)
    x2map: data levels, running sum within +-35840 from 500 frames after the switch
    test_x2map: 45 checks, 0 failures   (10000 random frames each in levels B, A, B behind 3 and 6 dB, A behind 6 dB, against a model of the facts)

Mutation checks: the free sign's tie rule `<=` to `<` gives 17 failures,
its threshold +2 or -5 six each. clang UBSan clean; ARM prints the host's
lines (`make check-arm`: ALL ARM CHECKS PASS).

## x2 6: training, records and data at 32000; a simulated client; the emulated Courier (2026-11-20)

The client's marker now leads to the x2 start-up and data mode at rate
index 1 (32000 down), V.34 up. The facts are docs/history/x2-facts.md
"Phase 3 script", "Records", "Mapping", "Free sign", "Pads", "Upstream".

- `modem/x2tx`: the server's script, octet by octet: 20 x 7f, the
  preamble (from its rule: Barker-11 signs, the 0712 magnitudes; checked
  against the facts' 22 octets), TRN (GPC from 0, b1 / 31), the J words
  (09B0 x 99, 099F), the ones (c1 / 41), the alignment, sync frames of
  ones in levels A (1667 at least, until the client's MP), our record
  F33C 7FFF 0000 padded to 4 frames (whole records, until its E), the
  start (1 + 680 frames of ones), data. GPC runs on through every mapped
  frame (not the alignment).
- `modem/x2d`, modelled on p34d, one sample in, one octet out: the
  client's V.34 Phase 3 (sdet, PP and 512 TRN symbols known, 4-point
  decisions, J of either kind twice) -> our script; the echo canceller
  learns on our TRN while the client is quiet (it stops its J 150 ms into
  our preamble: a fact added to "Phase 3 script"), the echo delay measured
  first when not given; sres on its Phase 4 S; 16-point decisions (the
  equaliser learns only from decisions near a point, so TRN's 4-point
  start does not pull it); its MP (an x2 record of 4 words) taken when two
  in a row agree: the index must be in X2D_RATE_MASK (0x0002), the pad 0..2,
  the upstream the highest of its word 1 at most 28800 that v34/ takes
  with GPA, expanded shaping, 64 states and non-linear encoding; E (20
  ones) -> B1 and data through qamrx + v34/. Each wait has a limit; after
  the marker every failure ends the call. The client's S in data mode is
  X2D_RENEG, not answered yet: the call ends 6 s later (x2 7).
- `modem/answer`: ANS_X2 runs x2d in `u` (with V.32bis and V.92: 20 KB of
  45; answer_t unchanged, V90M_MEM_SIZE unchanged); data mode is
  ANS_MOD_X2 with x2d's rates; an end before data mode is x2_failed
  (V90M_END_X2_FAILED); answer_cleardown in x2 data mode goes silent
  (cleared = 2, V90M_END_CLEARDOWN_OURS); SNR, rtd, echo delay, moving.
- `sim/x2call`: the client, from the facts: V.8 and Phase 2 (v8call with
  jm_flip, p2c) to our L1, the marker 4d 170 ms after it began, Phase 3
  (TRN 6400, J on 16 points until our preamble has run 150 ms), Phase 4 at
  our J words (TRN 512 on 4 points then 16, 5120 in all), its MP until our
  record decodes, one MP', E, B1, data. Downstream it passes our octets
  through the pad model and checks every segment.
- The Task 4 review's minors (commit c85b990): the marker in the first
  Phase 2 only; its 800 ms counted to the frame's start (and through a
  return to Tone A after an unanswered L2); V8_SEL_V34; self_retrain
  through x2_stop.
- tools: `rec_x2` (a recording of a sim call), `bench_v90 -x`, `make
  bench-x2`, `make cost-x2`; the x2-peer bridge and run.sh take MODS=x2.

Tests (each seen failing first: test_x2tx against a stub x2tx, 2027 of
2034 checks failed; test_answer_x2's and test_api's x2 calls against an
x2d that ends at once, as Task 4's ANS_X2 did: 56 and 5 failures):

    test_x2tx:     50 checks, 0 failures (every segment, records, start, 48000 data bits)
    test_answer_x2: 172 checks, 0 failures
      x2 no pad: data 32000 / 24000 bit/s; down 118512 bits 0 bad, up 100020 bits 0 bad
         pad mode 0 in the MP and the mapper; echo delay 295; upstream SNR 36.6 dB
         client saw: preamble 0 bad, TRN 0 bad, J words 99 + 1, ones 0 bad, alignment 0 bad,
         sync 1667 frames (0 bad), records 14 (0 bad), start 680 (0 bad), data frames 4938 (0 lost)
      x2 3 dB pad, 6 dB pad: the same (pad mode 1, 2; echo delay given 0; SNR 34.9, 33.8 dB)
      x2 MP asks for index 5: ended 6.47 s after the marker, error 5, x2_failed 1, 4 MPs sent
      x2 MP reports pad mode 3: ended 6.47 s after the marker, error 6, x2_failed 1, 4 MPs sent
      x2 client silent after its marker: the call ended 5.93 s later, error 2, x2_failed 1
      x2 client silent after its J: the call ended 3.02 s later, error 3, x2_failed 1
      x2 client silent after its MP: the call ended 6.61 s later, error 8, x2_failed 1
      x2 client silent in data mode: the call ended 0.53 s later, error 10, x2_failed 0
    test_api: the x2 call through the public API: V90M_CONN_X2, 32000 / 24000,
      SNR 38.1 dB, events CONNECTED, DATA_MOVING ... ENDED, v90m_cleardown -> CLEARDOWN_OURS
    test_p2ans: the marker refused in a retrain, taken 15 ms before the
      deadline (sent 945 ms after L1 began), refused 5 ms after it

The echo canceller carries the upstream: frozen (a diagnostic build, not
kept), the no-pad call never got the client's MP (SNR -0.1 dB).

The emulated Courier (`make test-x2-courier MODS=x2`, courier-emu
186d653f5624, its 4.03 board ROM) at each pad: PASS. Ours `CONNECT
32000/24000 x2` at 30.40 s of line, V.42 LAPM at 32.88 s; the Courier
`CONNECT 48000/ARQ/x2/LAPM/V42BIS` (PAD=3: 50666), ATI11 `Modulation
x2/V.34+`, `Trellis Code NONE/64S-4D`, `Nonlinear Encoding NONE/ON`,
`Shaping OFF/ON`, SNR 56 dB, x2 Status `0000` / `0001` / `0010` by pad.
12.1 s into data mode the Courier began its rate change, which this
version does not answer: our end came 6 s later. Details and what is not
settled: x2-facts.md "Harness". No difference from the simulated client
showed.

CPU (`make bench-x2`, `make cost-x2`; ARMv5 gcc -O2 under qemu): the
recording of a sim call (tools/rec_x2, 40 s, data mode from 10.73 s)
replays on the host and on ARM with the same transmit hash (37c3e5f5),
713400 bits up, 0 bad. Instructions per second of line in data mode
(seconds 15 to 16 of each call, counted as cost-v34 does):

| | M instructions / s | of the box (123 M = 100 %) |
|---|---|---|
| V.90 data mode (rec_v90: 50666 / 28800) | 38.9 | 32 % |
| x2 data mode (rec_x2: 32000 / 24000) | 80.4 | 65 % |

x2's data mode costs 206 % of V.90's, against the plan's "no more than
V.90's". The cost is the upstream the client dictates (x2-facts.md
"Upstream", as the Courier's ATI11 confirms): 64-state trellis, non-linear
encoding, expanded shaping; V.90's upstream is 16 states, linear, minimum
shaping, which p34d asks for in its MP and x2 has no way to ask. A scratch
build of client and server with 16 states, linear and minimum shaping
(not kept) costs the same as V.90 on the host bench (0.2 % of the host
against 0.4 % with the Courier's options, 3 runs each). One x2 call fits
the box; two at once would not.

`make check`: ALL HOST CHECKS PASS. `PAR=16 make check-arm` (armv5
uclibc, qemu): ALL ARM CHECKS PASS; arm_test_x2tx 50 checks,
arm_test_answer_x2 156 (its x2 cases all run on ARM, about 8 s, and print
the host's lines exactly; only the older TEST_SHORT block is left out),
arm_test_p2ans 255, api 458. clang -fsanitize=undefined builds of
test_x2tx and test_answer_x2 print what gcc's do.

Replay proof (every live-050 recording, 106; v90m-replay at 109c2d0
against this branch; flags "", "-9", "-x", "-x -9"): all four
comparisons identical, .bits included.

Review fix (x2 6): our record's word 1 named every rate index (the
I-modem's 7FFF) against the design's "our record's rate mask names only
the indices that have banks". Against the emulated Courier at PAD=0: 0001
behaves as 7FFF (CONNECT 48000, V.42 up, the rate change at 12.1 s; PAD=3
CONNECT 50666 too); 0002 never gets a CONNECT from it and it hangs up 7.1
s into our data mode. Word 1 is now 0001, bit n - 1 for index n (the MP's
word 1 convention), and X2D_RATE_MASK is that same word, so the record
and the check of the client's MP agree (the 0x0002 / bit n above is
superseded). x2-facts.md "Records" has the runs; 48000 and 50666 are the
rates of index 9 and 11. test_x2tx pins the word; test_answer_x2's record
check in sim/x2call follows.

## x2 7: the client's rate change (2026-11-20)

x2d now answers a rate change of the client's (docs/history/x2-facts.md
"Rate change"). It no longer ends the call 6 s after the client's S.

- `modem/x2d`: when the client's S comes in data mode (X2D_RENEG), the
  data stops going up and goes on down. sres_resume follows its S-bar. Then
  come 4-point decisions (RX_RN) on its TRN and its MP, which is checked as
  at the start-up. After its MP (never before), `x2tx_data` and `x2tx_go`
  start our answer. Its E leads to B1 and data at the new upstream rate,
  and then back to X2D_DATA with the rates taken again. The limit is 6 s
  from its S (X2D_ERR_RENEG).
- `modem/x2tx`: `x2tx_go` in data mode starts a rate change after the
  frame being sent. It sends 8 frames +++--- and one ---+++ at the largest
  level of the data levels in use, as sent (a5 / 25 as received). Then
  GPC restarts from zero with the sign positive, and our record goes out
  in levels B, 10 times as 733C and 33 times as F33C. Then one frame of
  ones in levels B, 681 at the new rate, and data from the owner's next
  bit.
- The Task 6 review's minors: `x2d_cleardown` now works in X2D_RENEG too.
  The MP's MD (word 3, high byte) must be 5, in the first MP and in a rate
  change's alike; any other value gives X2D_ERR_RATE.
- `modem/answer`: an end after data mode other than carrier loss or our
  cleardown counts as x2_failed (V90M_END_X2_FAILED). That covers a rate
  change we cannot serve or that does not complete.
- `sim/x2call`: `reneg_ms` gives the rate change: S, S-bar, TRN of 512
  symbols, its MP (reneg_index, reneg_mask, reneg_md) and MP' and E, all
  on 4 points, then B1 and data. Downstream, the sim checks our marker,
  records, ones and data frame by frame, and counts any marker frame
  before its MP. `md` sets the first MP's MD.
- The emulated Courier's rate-change MP is on 4 points, not 16. Found
  with an `X2D_TRACE` build (an fprintf trace, off by default, as p34d's
  V42_XID_TRACE). It asks for index 14 (12 behind 3 dB), above our
  mask, so we clear down, as the design says until there are more banks.
  Recorded in x2-facts.md "Rate change" and "Harness".

Tests (each seen failing first):

    test_x2tx: 66 checks, 0 failures
      x2tx rate change: marker 0 bad; records 10 733C then 33 F33C (0 bad); ones 1 + 681 frames; data 24000 bits on, 0 wrong; 32000 bit/s
    test_answer_x2: 303 checks, 0 failures
      x2 MP has MD 6: ended 6.47 s after the marker, error 5, x2_failed 1, 4 MPs sent
      x2 rate change, no pad (and 3 dB): data 32000 / 28800 bit/s; x2d saw the client's S 0.020 s after it began, data again 0.92 s after it
         client saw: marker 8 + 1 (0 bad, 0 before its MP), records 10 733C then 33 F33C (0 bad), ones 1 + 681 (0 bad), data frames 19017 (0 lost)
         down 456408 bits 0 bad (50016 after the change); up 369156 bits 0 bad (53256 after), the client's S: 300 bits lost, 0 extra
      x2 rate change asks for index 5 / has MD 6: ended 0.27 s after the client's S, error 5, x2_failed 1
      x2 rate change and our cleardown in it: ended 0.02 s after the client's S, error 12, cleared 2

The upstream loses the 300 bits that are still in our receiver's
decision delay when the client's S begins. The test resumes its check
from the client's next bit, and V.42 recovers such a loss. On TEST_SHORT
(ARM under qemu) the rate change comes 2 s into data and runs behind no
pad only.

The Courier (`make test-x2-courier MODS=x2`, PAD=0 / 3 / 6): each run
passes the start-up as before (CONNECT 32000/24000 x2, V.42 LAPM). Its
rate change asks for index 14 / 12 / 14 with mask 1FFE. We clear down
94 ms after its S ("ended: x2 failed"), and the Courier goes on hook 47 s
later. The call does not go on past the Courier's rate change until the
banks for its index exist (the bank task).

## x2 8: rates up to 56000 (2026-11-20)

x2's downstream now runs at every rate index the client can ask for, 1 to
15 (32000 to 56000 bit/s), behind no pad, 3 dB or 6 dB, at the start-up
and in each of its rate changes. The emulated Courier's rate change is
followed to the rate it asks for, and the call goes on.

- **The banks are included, not derived** (x2-facts.md "Banks"). No rule
  we tried reproduces the published mode 0 banks for 1, 8, 11, 12, 13 and
  14 (a greedy minimum-spacing ladder fits 17 of the 30 lists, but not the
  first position's of 12 or 14), and the Quad firmware builds its banks at
  run time instead of storing them. So `modem/x2bank.c` holds the 45 banks
  (3 pad modes x 15 indices) as data, generated by `tools/gen_x2_banks.py`
  from D-Modem's published values, which it read back from the Quad's
  constructor (QF060003). Origin in the file's head and in `docs/limits.md`.
  A pool of octets, 80 lists of (received, sent, n), and per bank B and
  the lists of position 0 and of positions 1..5.
- `modem/x2map`: each position has its own list (`x2map_list`), B up to 37
  bits (the number in 64 bits), and the free sign and the MD signs follow
  the levels received, not the digits, as the facts say (where the lists
  differ the two disagree: index 12, all digits 0, is `9e` at position 0
  and `9f` at the others, so position 0 has the largest level).
  `x2map_unframe` refuses digits past B bits.
- `modem/x2tx`: the rate change's marker has each position at its largest
  level in the rate in use (9e / 9f at 52000, as the I-modem's). From the
  client's MP on, everything goes out for the pad that MP reports: marker,
  records and new rate (one rule, stated in x2-facts.md "Rate change"). The
  two minors of the Task 7 review.
- Our record's word 1 and `X2D_RATE_MASK` are `7FFF` (every index has a
  bank in every pad mode), the I-modem's. Index 0, MD other than 5 and pad
  modes above 2 still clear down.
- `sim/x2call`: a second rate change (`reneg_index2`, `reneg_ms` after the
  first's data), per-rate-change counts, the marker per position, and
  `x2call_level`: the client takes each octet for the level whose octet
  sent arrives as it behind the pad. Two levels of the Quad's 3 dB banks
  (`e2`, `f1`, sent as `dc`, `ed`) arrive one code off under the pad model;
  the I-modem sends the same octets, so we keep them (x2-facts.md "Banks").
- `tools/rec_x2`: an optional fourth argument, the index the client asks
  for (for the cost at 56000).

Tests (each seen failing first):

    test_x2map: 46 checks, 0 failures
      x2map: 45 banks, 0 entries not as the facts, 49 mode 1 levels no octet arrives as; 90000 frames, 0 not as the model or not back
    test_x2tx: 109 checks, 0 failures
      x2tx rate change to index 12, pad mode 0: marker 0 bad; records 10 733C then 33 F33C (0 bad); ones 1 + 681 frames; data 39000 bits on, 0 wrong; 52000 bit/s
      x2tx rate change to index 14, pad mode 0: marker 0 bad (9e / 9f) ... 54666 bit/s
      x2tx rate change to index 8, pad mode 2: marker 0 bad (8d / 8e: the MP's pad) ... 46666 bit/s
    test_answer_x2: 359 checks, 0 failures
      x2 rate changes to 8 and 14, no pad (and 3 dB, 6 dB): data 46666 then 54666 / 28800 bit/s; data again 0.92 s after the client's last S
         client saw: markers 16 + 1 (0 bad, 0 before its MP), records 20 733C then 66 F33C (0 bad), ones 2 + 1362 (0 bad), data frames 36160 (0 lost)
         down 1084683 bits 0 bad (76852 after the last change); up 747876 bits 0 bad (50040 after), the client's S: 960 bits lost, 0 extra
      x2 rate change asks for index 0 / has MD 6: ended 0.27 s after the client's S, error 5, x2_failed 1

On TEST_SHORT (ARM under qemu) the two rate changes come 2 s into data and
2 s apart, behind no pad only.

The Courier (`make test-x2-courier MODS=x2`, PAD=0 / 3 / 6; x2-facts.md
"Harness"): CONNECT 32000/24000 x2 at 30.40 s each time. Its rate change
at 42.50 s asks for index 14 / 12 / 14, and we follow it: 54666/28800,
52000/28800, 54666/28800 at 43.24 s. At PAD=3 a second one at 111.56 s
asks for 13: 53333/28800 at 112.32 s. Every call stays up with V.42 to
the budget's end (116 s). Its MP' and E in a rate change are on 4 points,
and B1 follows E.

Size (ARM, -Os): the tables go from 72 bytes (`x2tab.o`) to 9 + 4577
(`x2tab.o`, `x2bank.o`), +4.5 KB; `x2map.o` 928 -> 1304, `x2tx.o` 1776 ->
1800. CPU (`bench_v90 -x` on a 40 s `rec_x2` call, second 15 to 16):
81.9 M ARM instructions per second at 32000 (80.4 M at x2 6: the general
mapper, +1.9 %), 83.7 M at 56000 (+2.1 % over 32000).

## x2 9: the Courier's data after its rate change (2026-11-20; release held)

The release was to rest on data the emulated Courier decodes after its rate
change; until now the evidence was indirect (V.42 stayed up). The harness
now pushes data and counts the acknowledgements, and the Courier does
**not** decode our downstream after the change. 0.4.0 is not released.

- `tools/x2-peer/bridge.c`, `-k KB`: once LAPM is up a control burst of KB
  KiB of incompressible octets, and another after each x2 rate change once
  data moves again. Each is logged when the Courier has acknowledged all of
  it, with the line frames in which its N(R) advanced and the errors seen
  from our end (I frames sent again, REJ, violations, T401). With
  `MODS=x2`, `run.sh` passes `-k 4` (`KB`), and the run passes only when
  there was a rate change and every burst was acknowledged cleanly. A
  bridge exit of 1 no longer kills the Courier, so its report is kept.

`make test-x2-courier MODS=x2 PAD=0 / 3 / 6` (x2-facts.md "Harness"), the
same figures in two runs at each pad:

| | PAD=0 | PAD=3 | PAD=6 |
| --- | --- | --- | --- |
| control at 32000 (32.88 s) | 33 of 33 I frames acknowledged in 1.22 s, nothing sent again | the same | the same |
| after the rate change (43.24 s) | 54666: **0 of 615** acknowledged, 68 T401 | 52000: **1 of 658**, 2 REJ, 68 T401 | 54666: **0 of 600**, 68 T401 |
| `x2-peer` | FAIL | FAIL | FAIL |

The Courier answers every poll after the change (55 to 59 good frames from
it), acknowledging nothing: short frames get through and I frames of some
128 octets almost never do. Without error control (a one-off scratch run,
PAD=0) its terminal shows our text clean before the change and mostly
corrupt after it: 20 % of what it printed in correct runs, median 12
octets. Our simulated client decodes with our own banks and mapper, so its
bit-exact runs could not show this. Index 1 is the only rate shown to carry
data to the Courier. The x2 8 reading ("the call stays up ... the evidence
that it decodes our new rate") was wrong: LAPM stays up on polls alone.

Also (the Task 8 review's minors): `test_x2map` pins the 49 mode 1 levels
no octet arrives as, and checks that mode 2's received lists equal mode
0's at all 90 positions (47 checks, 0 failures; the check fails at 90 of 90
when pointed at mode 1); `test_answer_x2`'s comments give the second rate
change as `reneg_ms` (12.7 s) into the first one's data, not 3 s; x2-facts
"Records" puts our `0001` record in the past.

## x2 8b: the Courier's data after its rate change, debugged (2026-11-20; BLOCKED, release held)

x2 9 found that the emulated Courier decodes our downstream at index 1
(32000) and not after its rate change (54666 at PAD=0 and 6, 52000 at
PAD=3). x2 8b looked for the cause in our octets and found none: the same
Courier fails alike on D-Modem's server and on the I-modem's own firmware,
whose octets are ours. The fault is the emulated Courier (or the harness's
line model, which every server shares), not our mapper, banks or rate
change. Per the brief, no library code was changed to suit it.

The evidence (x2-facts.md "Oracle: D-Modem's mapper" and "Harness", the
last part; the runs are scratch, outside the repository):

- **Mapper and banks.** D-Modem's `X2Map`, built and run as a black box,
  and our `x2map_frame` give identical octets at all 15 rate indices in
  pad modes 0, 1 and 2 (20000 frames from each of three seeds): no first
  differing frame. The I-modem's 106 570 data frames at index 14, as it
  sent them to the Courier, decode with our lists and give LAPM frames
  with good FCS.
- **Rate change and start-up.** On the line, our octets equal D-Modem's
  and the I-modem's from the preamble through the sync frames and records,
  and through the marker, the 43 records and the 682 frames of ones of the
  rate change; only the free sign of data-level frames differs (it differs
  between the I-modem and D-Modem too, and the client does not check it).
- **The Courier.** With the same settings and line, after the change:
  ours 0 of 615 I frames acknowledged, and without error control 0 of some
  73 text lines intact; D-Modem 0 of 72 lines (PAD=0), 10 of 71 (PAD=3,
  52000), 0 of 68 (PAD=6), and with V.42 only the lines of its first
  second; the I-modem's 34 short I frames (429 octets) needed 1 to 19
  transmissions each, one 10-octet frame 15.8 s. Before the change all
  three deliver everything.

Test (independent vectors, no fix behind it): `test_x2map` holds our
mapper to D-Modem's output at indices 1, 8, 12 and 14 in each pad mode
(frames 0, 1 and 3999 and an FNV-1a of 4000 frames, from the oracle run,
kept as data); `x2map: D-Modem's mapper's octets (oracle run) at 12 of 12
rate/pad pairs`. It passes on the current code, since the mapper is right.
Its teeth: with the free sign's tie rule turned (`<` for `<=`) 12 of 12
pairs fail; with positions 0 and 1 sharing a list, the 8 pairs whose
lists differ by position fail. Index 1's octets are unchanged.

`make test-x2-courier MODS=x2` still fails at every pad (x2 9's figures,
reproduced at PAD=0: 0 of 615); it cannot pass on this emulator. What
would: a check against a Courier that decodes the I-modem after its rate
change (another emulator setting, or a real modem), or an acceptance that
rests on the octets being the I-modem's, which this branch now shows.

Follow-up (a bounded search for a setup that passes; x2-facts.md
"Harness", the last part). D-Modem's documented courier-emu setup used a
locally modified courier-emu (`COURIER_AT`, `COURIER_NVRAM`,
`COURIER_INT0_TOUT` are not in the published one) and lines 6 to 14 dB
down; the rest is our harness. Our bridge's clock is not off: the line is
sample-locked (equal sample counts each frame; the stamp clocks nothing;
the Courier's codec at exactly 9600 Hz, 6 to 5), so the Courier's -112 /
+159 ppm are its own estimates, and courier-emu's one offset setting is
already zero (+-112 ppm breaks the start-up). Lines 3, 6, 8 and 10 dB down,
and the Courier's INT0 at 2700 to 4800 Hz, pass no data after the rate
change, for us (best: 15 of 265 I frames at 52000, 6 dB down) or for
D-Modem (best: 3 of some 72 lines). Nothing passed in the time box; the
bridge is unchanged and the acceptance check still fails at every pad.


### x2 9, continued: release 0.4.0 (2026-11-20/21)

After x2 8b (the emulator, not our octets, fails above index 1) the
ruling: labels as below, the post-change data check opt-in, the free sign
listed as a known difference, then the release.

- **Labels** (CHANGELOG, README, docs/limits.md, docs/answering.md): at
  32000, "decoded by the emulated USR Courier, V.42 data acknowledged";
  above 32000, "octet-identical to the USR I-modem and D-Modem's server;
  the published courier-emu decodes no server above index 1; not yet
  confirmed on a real modem".
- **Harness** (d09ce5b): `make test-x2-courier MODS=x2` passes on the
  control burst at 32000 and prints the post-change counts as
  information; `STRICT_RENEG=1` requires them. PAD=0, 3, 6: PASS; PAD=0
  strict: FAIL (0 of 615), as for every server on this emulator.
- **Version 0.4.0**: Makefile, header, `v90m_version()` (test_api first:
  `api: 458 checks, 1 failures` on the string, then on the numbers, then
  0). `v90m_mem_size()` 118824 on x86-64, 118512 on ARMv5 (8 octets more
  than 0.3.0); `V90M_MEM_SIZE` unchanged.
- **Docs**: CHANGELOG 0.4.0; README (x2 in the list, the standards table,
  what was tried, the CPU table: x2 81.9 / 83.7 M at 32000 / 56000);
  DESIGN 4.2b (x2 done, the stated exception to "written from the
  recommendations"; K56flex not); docs/answering.md (an x2 section),
  api.md (`V90M_MOD_X2`, `V90M_CONN_X2`, `V90M_END_X2_FAILED`, retrain and
  cleardown in x2), limits.md (an x2 section: offered only, A-law, robbed
  bits, no way back, the free sign, 3 dB octets; CPU twice V.90's, one x2
  call per box), testing.md; the header's comments on retrain and
  cleardown. The "x2 5" section above was missing and is added.

Checks on the release tree:

| check | result |
| --- | --- |
| `make check` | ALL HOST CHECKS PASS (api 458, answer_x2 359, test_x2map 59 checks, 0 failures) |
| `make check-ubsan` | ALL UBSAN CHECKS PASS |
| `PAR=16 make check-arm ARM_CC=...armv5...` | ALL ARM CHECKS PASS (answer_x2 247 TEST_SHORT; x2map's lines as the host's) |
| `PAR=1 tools/check-ports.sh build/ports-t9 host-gcc host-clang i686-musl aarch64be-musl mips-musl win32-mingw` | host-clang, i686-musl, aarch64be-musl, mips-musl, win32-mingw: 115 of 115 programs identical to host-gcc each; ALL PORTS PASS (17:11 to 00:06) |
| replay proof, 106 live-050 recordings, 19ba2c6 (0.3.0) against this tree | `""` and `-9`: identical (106 .txt; 21 / 40 .bits). `-x` and `-x -9`: the same 6 differ as at x2 4 (ab_cc_v34, ab_cl_v34, v34a, v34b, v34c, x1), each only in +2094 transmit samples (x1 110394 -> 112488), no other line, .bits identical |
| `make test-x2-courier MODS=x2` PAD=0 / 3 / 6 | PASS each; `STRICT_RENEG=1` PAD=0 FAIL (by design) |

(The tag `v0.3.0` in /home/georgica/libv90modem points at a v90d commit,
90687bd; the base was 19ba2c6, origin/main at 0.3.0, as at x2 4.)


## k56 1: the facts, written down (2026-11-21)

`docs/history/k56-facts.md`, the K56flex counterpart of x2-facts.md: every
value later tasks use, by section, each tagged documented / RE (who, from
what) / our observation / derived. No product code.

Sources fetched outside the repository:

- D-Modem cloned to `~/modem-research/D-Modem` at `c06a380d` (`git log -1`:
  `c06a380d86105b2309c8cb8958e41120856bb84b`); its `docs/dsp/k56flex-details.md`
  is identical to the copy in `~/modem-research/sources/`. Code read for
  values only. Not built.
- ITU-T V.8 bis (11/2000) to `~/modem-research/itu/` (SHA-256 `1eddb79f...`);
  the CL, SM56's MS and ACK(1) checked against Tables 3 to 6 and 8.2.3, 8.5,
  octet by octet.
- Gough Lui's recording `k56flex-48000.wav` to `~/modem-research/goughlui-k56/`
  (SHA-256 `e45f73cc...`), decoded with throwaway Python there (V.21 FSK and
  HDLC, V.34 INFO DPSK, tone and level steps) and two small C programs that
  link the repository's `modem/info.c` and `modem/v8msg.c` read-only.

What the recording settled (a real Rockwell RCV56DPF client and a real
server):

- The server sent **CRe** (segment 1 about 290 ms), not MRe; CL `12 c9 80
  80 81 c2 09 b5 02 00 94 81 83 42 46 e0` (FCS `23 50` good): the 9-octet
  block of decision 3 exactly, but with an S field offering data and V.42
  (`81 c2`). The client's MS `11 c9 80 80 81 c2 09 b5 02 00 94 81 81 02 47
  85` (FCS `71 d8`), ACK(1) `14` (FCS `dd a6`). CM and JM `e0 c1 45 13 94 2a
  0d` (V.34, no PCM; no digital-access bit in the JM).
- INFO1c offset `0x009`, INFO1a offset `0x005`: an **A-law** K56flex call,
  confirming the law bits D-Modem inferred.
- Training lengths: TRN1A about 16650, TRN1B about 8180, TRN1C and Js about
  18470, TRN2A and MARK about 8380 (SM56-accepted: 16368, 8448, 8448 + 128,
  16336 + 32); data at 48000 from about 2.2 s after TRN2A ends.

Found while writing it down (in "Unsettled", for the controller):

- D-Modem's MS check skips four level 1 blocks and would reject the real
  client's MS (its S field has a data Par(2) block); the "MS test" section
  parses the V.8bis trees instead.
- D-Modem restarts the data scrambler, running sum, shaping counter and
  3-frame cycle at every data start (each MP), and SM56 re-initialises its
  decoder on each MP: they do not carry across a renegotiation.
- V.8bis 10.2.2 asks for 400 ms of silence before MRe/CRe (spec: 200 ms);
  9.8 asks for NAK(1) on an invalid frame (decision 2: never NAK).

`glibc-devel.i686` is not installed (`rpm -q`): the SM56 harnesses cannot
run yet ("Harness"). No build or test change in this task; `make check` run
before the commit as the plan asks.

## k56 2: the oracles and the SM56 harnesses, outside the repository (2026-11-21)

`glibc-devel.i686` (and `libxcrypt-devel.i686`) installed by the owner during
the task: the SM56 harnesses ran; the release label is not held at L0 by the
toolchain.

D-Modem at `c06a380d` in `~/modem-research/D-Modem`, built unmodified:

    cmake -B build -DCMAKE_C_FLAGS=-mstackrealign -DCMAKE_CXX_FLAGS=-mstackrealign
    cmake --build build -j16
    ctest --test-dir build -R k56flex -L sm56     # 3 of 3 pass

(without `-mstackrealign` GCC 15's `half_sm56` faults at once: an aligned SSE
store on the module's 4-byte-aligned stack). D-Modem's server against SM56
(`sm56_k56flex`): CONNECT 44000/31200; its timeline is in `k56-facts.md`
"Harness".

Oracles (ours, `~/modem-research/k56-oracle/`, `make`):

- `./enc > test/data/k56enc_oracle.h`: `K56Enc` at 13 rates x 3 pads x 64
  masks, 600 frames each, 0.3 s. `K56Enc` scrambles internally; the pipeline
  for our encoder (xorshift32 from 1, GPA from 0, then the encoder) is in
  "Oracle" and the header's comment.
- `./train > test/data/k56tx_oracle.h`: `K56Train` driven alone (no
  `open_k56flex` run needed), 12 cases of J (`00 40 80 04 12 98` x J after
  12000 / 13000 of silence 2, MP after 3000 of TRN2B) and one renegotiation
  (MP after 1500); per segment length, FNV-1a, first 8 octets, to the first
  data frame; our MP's octets included.
- `sm56dec/` (32-bit, `sm56.ko`'s `pstn_init_rx` / `putBIOS`): on `enc -d`'s
  dumps, 3 of 3 (32000/0/00, 44000/1/12, 56000/2/3f) decode to the data
  sent; `check.sh DIR` runs a directory (Task 7's `k56-dump`).
- GPC upstream settled from D-Modem's `k56_start` / `V34Rx::init` (RE).

The peer (`tools/k56-peer/`, `make test-k56-sm56`, not in `check`):

    make test-k56-sm56                                        # DMODEM not set, exit 0
    make test-k56-sm56 DMODEM=/x CC=false                     # no 32-bit toolchain (glibc-devel.i686), exit 0
    make test-k56-sm56 DMODEM=~/modem-research/D-Modem MODS=none
      ours: CONNECT 28800/28800 V.34 at 13.0 s of line; sm56: CONNECT 28800/LAPM
      to SM56: 4096 octets, identical; to us: 4096 octets, identical; PASS
    ... RBS=0x08                                              # PASS, the same
    ... MODS=k56                                              # bridge: no V90M_MOD_K56 yet (Task 5), FAIL

SM56 dials `AT+MS=K56FLEX,0` (the brief's): with nothing answering its
V.8bis it falls back to V.34 by itself, so Ruling F2's automode is not
needed for a fall-back. With automode (`AT+MS=K56FLEX,1`, `SM56_MS=`) its CM
offers V.90 too and **our V.90 answerer fails against SM56 in Phase 2** (no
INFO1a after our INFO1d; SM56 NO CARRIER), as with `AT+MS=V90`; D-Modem's
server passes that call. Recorded in "Harness", not investigated here.

Corrections to `k56-facts.md` from Task 1's review: the server-started
renegotiation sends R (not R2); a retrain repeats Phase 2 (INFO1), not
"INFO1a flagged again".

`make check`: ALL HOST CHECKS PASS (nothing new runs in it).

## k56 3: the rate tables (2026-11-21)

`modem/k56tab.{h,c}`: K, u, r and M per pad of the 13 rates, the 39 ring
lists (468 octets) and the 39 constellations (1382 octets), `k56_ring(rate,
pad)` and `k56_bios(rate, pad)`. The rule was tried first, in a scratch
script outside the repository (`k56-facts.md` "Tables"): rings by group
energy, 0 of 39; magnitudes as a minimum-distance ladder, 25 of 39 behind
the pad and only with each table's first point and spacing given. So the
rings and magnitudes are included, generated from D-Modem's published
values (`docs/limits.md`, "Included tables (K56flex)"):

    python3 tools/gen_k56_tables.py ~/modem-research/D-Modem/dsp/k56flex_tables.cpp > modem/k56tab.c
      gen_k56_tables: 468 ring octets, 1382 constellation octets
    make build/test_k56tab && build/test_k56tab
      test_k56tab: 3838 checks, 0 failures

The generator checks K, u, r and M against the facts, the frame identity,
M^8 >= 2^K, each ring list a permutation and each constellation M << (u -
1) increasing magnitudes. Not in the signal path yet (Task 7's encoder
uses it); no check-arm.

## k56 4: the V.8bis messages (2026-11-21)

`modem/v8bismsg.{h,c}`: the FCS (CRC-16/X.25, checked against "123456789"
= `0x906e` and the recorded `23 50`, `71 d8`, `dd a6`), our CL (default the
real server's 16 octets, ruling C2; `V8B_NS7` the 13-octet form;
`V8B_NS_DEFAULT` overridable), ACK(1) `14`, the framing (32 ones, three
flags, octets and FCS LSB first with zero insertion, one closing flag; V.8bis's
own, not V.42's HDLC), the receiver (`v8bis_rx_bit`: octets, 0, or -1 for
fewer than three octets, not whole octets, bad FCS, too long, abort), SM56's
CL block test, and `v8bis_ms_is_k56`, which walks the I and S field trees of
8.2.3 (ruling C3), so the real client's MS (`81 c2` Par(2) block) passes where
D-Modem's fixed skip reads it as an NS block of length `0xc2`.

    make build/test_v8bismsg      (first: no rule for modem/v8bismsg.c; then the
                                   first run failed 10 checks, receiver and trees)
    build/test_v8bismsg
      test_v8bismsg: 297 checks, 0 failures

Not in the signal path (Task 5/8 use it); no check-arm.

## k56 5: the API, and Phase 1 with fall-through (2026-11-21)

`V90M_MOD_K56` (0x0200: answering only, with V.34, not in the defaults),
`V90M_CONN_K56FLEX`, `V90M_END_K56_FAILED` (both appended); `v90m_mem_size()`
pinned by test_api at 118824 (x86-64) and 118512 (32-bit ARM). The answerer
(`ANS_MODE_K56`, mu-law with V.34 only) begins with `ANS_V8BIS`
(`modem/v8bis`, in `answer_t.u` while it runs): 400 ms of silence (ruling C6),
CRe (ruling C1: 1375 + 2002 Hz 290 ms, 400 Hz 100 ms, 14.6 dB below our
messages, as the real server), CRd's pair begun within 1000 ms of CRe's end
and followed to its 1900 Hz end, our CL (the real server's, ruling C2) on V.21
channel 1 120 ms later, the MS on channel 2 until 1500 ms after the CL,
ACK(1) 180 ms after an MS with the block, NAK(1) after an invalid frame;
then V.8 as before. An agreement turns x2 off for the call and is handed to
V.34 (`v34ans_cfg.k56`, used from Task 6). A-law, or K56flex not offered:
no new state is entered. `sim/k56call` (a client from the facts, then
`sim/v34peer`), `tools/k56_crd_scan.c` (`make crd-scan RECS=<dir>`),
`v90m-replay -k`.

    make build/test_v8bis              (first: no rule for modem/v8bis.c)
    build/test_v8bis
      CRe: 1375.0 + 2002.0 Hz, 400.0 Hz; 14.6 / 14.6 dB below the CL
      CL begins 125.0 ms after CRd ends; ACK(1) 180.0 ms after the MS ends
      test_v8bis: 48 checks, 0 failures
    build/test_answer_k56              (first: ANS_V8BIS undeclared)
      K56flex client, K56flex and x2 offered: V.34 28800 / 28800, 0 bad; agreed 1, x2 0, ACK 1
      V.8bis caller without K56flex: our ANSam 1700.0 ms after our CL ended
      our calling modem in V.90 / V.92, K56flex offered: 52000 / 28800, 52000 / 34666, 0 fail,
        the same as the reference (which starts as late as V.8; V.92's rates move a step
        with the line's noise), the same octets for 12 s from V.8 on
      answer_k56: 115 checks, 0 failures
    make check                         ALL HOST CHECKS PASS

Delays before our ANSam, measured in test_answer_k56 (for `docs/limits.md`,
F10): a caller without V.8bis 1.790 s later than without K56flex; a V.8bis
caller without K56flex (CRd, no MS) 3.807 s later (ANSam 1700 ms after our
CL); a K56flex client 3.413 s later (ANSam at 3.613 s after answering).

The CRd scan over the 106 recordings of `~/sync-build/live-050`:

    make crd-scan RECS=~/sync-build/live-050
      k56-crd-scan: 106 recordings, 0 CRd detections (0 with the 1900 Hz end)

The same detector on the client channel of Gough Lui's recording (outside
the repository) finds its one CRd: the pair at 2.040 s (it began at 1.885),
the end at 2.400 s (1900 Hz ended at 2.390).

Replay proof: the 0.4.0 + x2-gate tree (704b370, `~/k56-base`) against this
one, the 106 recordings, flags `""`, `-9`, `-x`, `-x -9`, each run in its own
directory: every `.txt` (424) and `.bits` (122) byte-identical. With `-k`
(and `-k -9`) every recording completes; V.8bis hears silence and gives up
(result 2, no CRd) in all 106; the final modulation and rates line equals
the run without `-k` in 106 of 106 (the transmit-difference count grows by
CRe, which the recording does not have).

SM56 (`make test-k56-sm56 DMODEM=~/modem-research/D-Modem MODS=k56`, SM56
dialled `AT+MS=K56FLEX,0`): PASS, V.34 28800 / 28800, 4096 octets each way
identical. Decoded from its `call.rec`: SM56's CRd, our CL with the
9-octet block, SM56's MS `11 c9 80 80 80 09 b5 02 00 94 81 83 02 46 e0` (the
block, our 9-octet form echoed with version `02`), our ACK(1) `14`. So the
9-octet form stays the default (`V8B_NS_DEFAULT` = `V8B_NS9`). INFO1a is not
flagged yet (Task 6).

## k56 6: the INFO1a flag, the point of no return (2026-11-21)

`p2ans_cfg.k56` (from `v34ans_cfg.k56`, in the first Phase 2 and in a
retrain's): when INFO1c's frequency offset is `0x001` and the symbol rate
chosen is 3200 (spec decision 6, the builder-check answer), INFO1a's bits
40:49 are `0x001` (mu-law, x = 0), `p2ans_t.k56` is set, and Phase 2 ends in
`P2N_K56` (silent) where it would end in `P2N_DONE`; `v34ans` goes to
`V34A_K56`, the answerer to `ANS_K56`, which (until the engine of Task 9)
ends the call on the next sample with `k56_failed` (`V90M_END_K56_FAILED`).
Any other agreed call is plain V.34, with a measured offset ending in binary
01 moved one step toward zero (`p2ans_k56_nudge`). L1, L2 and everything
without `cfg.k56` unchanged. Test peers: `p2c_cfg.k56` (INFO1c `0x001` or
`0x200`), `v34peer_cfg.k56` (stops in `VP_K56` after a flagged INFO1a),
`k56call_default`, `k56call_cfg.info1c_k56` (default 1), `up_sr_3000`.
test_p2ans shifts the caller's frequency (a Hilbert transformer) to make
the measured offsets the nudge works on.

    build/test_p2ans                   (first: P2N_K56 undeclared; then, with the
                                        interface alone, 21 failures)
      K56flex flag: INFO1a offset 1, P2N_K56 at 2.0293 s (P2N_DONE without: 2.0293 s), silent
      INFO1c 0x200, line offset +0.10 / -0.06 / +0.02 / -0.14 / +0.18 Hz:
        measured 5 / -3 / 1 / -7 / 9, INFO1a 4 / -2 / 0 / -6 / 8; 0, 4, -2 kept (same samples)
      INFO1c 0x001 at 3000 baud (client without 3200, or 3200 not allowed): unflagged, 1 -> 0
      p2ans: 402 checks, 0 failures
    build/test_answer_k56              (first, without the answer.c change: 3 failures)
      agreed client, INFO1c 0x001: INFO1a offset 1 at 3200 baud, ANS_K56 at 7.884 s,
        the client took the flag at 7.873 s, the call ended 11.0 ms after that
      agreed, INFO1c 0x200: V.34 28800 / 28800, as without K56flex
      agreed, 3000 baud up at most: V.34 28800 / 26400 at 3000, as without K56flex
      answer_k56: 156 checks, 0 failures
    build/api_test_api                 (first, without the answer.c change: 4 failures)
      API K56flex call: the client took our flagged INFO1a at 7.836 s, end 10 (K56_FAILED) 11.9 ms after
      api: 490 checks, 0 failures
    make check                         ALL HOST CHECKS PASS
    make -j8 check-arm ARM_CC=...armv7-eabihf musl...
      p2ans: 402 checks; answer_k56 (TEST_SHORT): 108 checks; api: 490 checks (the
      K56flex call 11.9 ms, mem size as pinned)   ALL ARM CHECKS PASS

Replay proof: `~/k56-base` (704b370) against this tree, the 106 recordings
of `~/sync-build/live-050`, flags `""`, `-9`, `-x`, `-x -9`: every `.txt`
(424) and `.bits` (122) byte-identical. `-k` and `-k -9`: 106 of 106
complete, the final modulation line as without `-k` in 106 of 106.

## k56 7: the data encoder (2026-11-21)

`modem/k56enc` (frames of 8 symbols, "Data" and "Robbed bits" of
`k56-facts.md`): the K' shell bits (shifted left by L - n on a robbed-bit
line; K' = K, no shift, at 3 robbed in 6) through V.34's shell mapper, then
per symbol a sign bit (none on every r-th symbol over the call, whose sign
is set against the running sum: positive at 0 or below) and q bits (bit 0
not sent on a robbed symbol), ring to group by `k56_ring`, point to
magnitude by `k56_bios`, mu-law. 5 robbed count as 6; no robbing at 32000
and 34000. `k56enc_set_rate` is a new encoder (Ruling C4: nothing carries
across an MP). `k56enc_unframe` inverts it for the tests and the coming sim
client. V.34's shell-table construction moved out of `v34_params_init`
into `v34_shell_tables(p, M)` (same arithmetic; v34's own tests and every
V.34 test unchanged) so that K56flex sets the tables for its own M; the
encoder holds a `v34_params_t` (1044 bytes on x86-64) for them.
`tools/k56enc_dump.c`, `make k56-dump`: our octets and data bits for SM56's
decoder harness.

    make build/test_k56enc             (first: k56enc.h: No such file or directory)
      frame / unframe: 78 of 78 rate x pad x mask, 2000 frames each
      shaping: 2416 symbols, 0 with the wrong sign, 0 frames whose shaping signs carried data
      oracle: 2496 of 2496 rate x pad x mask equal to D-Modem's; frames 0, 1, 599 at mask 0: 0 of 39 differ
      k56enc: 98 checks, 0 failures   (0.28 s; also clean under clang -fsanitize=undefined)
    mutations: shaping sign `< 0` for `<= 0`: 57 of 2496 differ; 5 robbed not
      folded to 6: 198 differ (the n6 = 3 entry of the loss table is not used)
    make k56-dump; ~/modem-research/k56-oracle/sm56dec/check.sh build/k56dump
      sm56dec: 2496 of 2496 decode to the data sent
    make check                         ALL HOST CHECKS PASS
    make -j8 check-arm ARM_CC=...armv7-eabihf musl...
      k56enc (TEST_SHORT, masks 00 24 3f 15): oracle 156 of 156, 98 checks, 0 failures
      ALL ARM CHECKS PASS

## k56 8: the training transmitter (2026-11-21)

`modem/k56tx` ("Training down", "Phase 4", "Rate renegotiation" of
`k56-facts.md`): silence 1, R1, TRN1A, TRN1B, TRN1C, Js, silence 2 (until
the client's J: max(12288, J) rounded up to 8), R2, TRN2A, MARK, TRN2B
(until our MP, at least 2048, and the grid of 24), MP, MP', E, then the
data through `k56enc` (B1 first, a new encoder and data scrambler at each
data start, Ruling C4), one mu-law octet a tick. One GPA scrambler, its
history and the running sum from R1 through E, carried across a
renegotiation (`k56tx_reneg`: R2 now, or at the end of the frame being
sent; TRN2A 2048, MP / MP' 1 bit a symbol, E 24). `k56tx_got_j` refuses
pad code 3 (silence 2 goes on; Ruling F3, k56d checks the J first). Our MP
and MP' from `v34_mp_build`, J from `v34_j16`, the scrambler `v90_scr`.

Two facts corrected (`k56-facts.md` "Training down", "Corrections (k56
8)"), found against the oracle and confirmed in D-Modem's `K56Train`:
TRN2B scrambles a one and a zero a symbol (not two ones); the even
symbols of TRN1C, Js, TRN2A and MARK scramble as many bits as the last
TRN2B / MP / E symbol did, so the first renegotiation's TRN2A and MARK
take 2 bits an even symbol (a later one's 1). The client still
descrambles TRN2A to ones (GPA's sequence decimated by 2 obeys the same
recurrence), checked for the call and two renegotiations.

    make build/test_k56tx              (first: undefined references to k56tx_*)
      our MP and MP' equal the oracle's: yes
      oracle: 13 of 13 cases equal to D-Modem's, segment by segment
        (before the corrections: 0 of 13; TRN2B on from the first TRN2B,
        then the renegotiation's TRN2A on)
      TRN1B: 0 of 8448 magnitudes not 40 41 4b 4c 50 51 50 51 by 3 scrambled bits
      Js: 4 words of 0x89b0, 0 bits wrong
      pad 1: 0 of 4432 TRN2B / MP / MP' / E symbols off 35 4d 58 60
      pad 1, renegotiation: 0 of 1432 MP / MP' / E symbols off 44 5d; E 24
      silence 2: max(12288, J) rounded up to 8 at J 0 .. 13007
      J with pad 3 refused: silence 2 still after 32000; a good J then ends it
      grid: MP at 2048 .. 2071: 0 with the data off the grid of 24
      renegotiation in mid-frame: R2 at the frame's end
      TRN2A's even signs descrambled, not ones: call 0, first renegotiation 0, second 0
      k56tx: 84 checks, 0 failures   (0.04 s; also clean under clang -fsanitize=undefined,address)
    make check                         ALL HOST CHECKS PASS
    make -j8 check-arm ARM_CC=...armv7-eabihf musl...
      k56tx: 84 checks, 0 failures (oracle 13 of 13 under qemu-arm)
      ALL ARM CHECKS PASS   (6 min 49 s)

## k56 9: training, MP and data at 32000 against a simulated client (2026-11-21)

`modem/k56d` (the engine from our flagged INFO1a on, as `x2d` for x2; `x2d`
untouched): `k56tx` down; up, the client as V.34's call modem (GPC): our
echo canceller (and the echo delay, when not given) on our TRN1A to TRN1C
while the client is silent; from our Js its S, PP, TRN (4 points), its J24
(two J words 24 bits apart, the octet between: pad and robbed symbols; pad
code 3 ends the call, Ruling F3), J' 24 bits after the last J word, TRN, MP,
MP' and E on 16 points (MP: two in a row that agree), then V.34 data (16
states, linear, minimum shaping: what our MP asks) with p34d's watchdog. Our
MP: type 0, rate_ac r = min(its rate_ac, max_down's code, 13), rate_ca the
highest rate of its mask at 3200 baud (at most its rate_ca and max_up),
mask 3FFF, asymmetric. Time limits: S 3 s from our Js, J24 3.2 s from its S
(Ruling F4: 2.14 s + 1 s), MP 4 s from our TRN2B, E 2 s from our MP', B1 1 s.
`answer`: `u.k56` in the union, `ANS_K56` runs it, `ANS_MOD_K56`,
cleardown (silence 0xff at once), no retrain, SNR, rtd, `k56_lost` (the API's
`V90M_END_LOST` in data mode; `fail()` clears `connected`, so the general
rule could not say it). API: `echo_delay` from `u.k56.ecd`.

`sim/k56call` from the flagged INFO1a: the pad and robbed-bit line model,
every training segment checked against the facts (`seg_bad[]`), Js, MP and
MP' decoded and checked, E, B1 dropped, data through `k56enc_unframe` and
GPA; up, V.34's call modem with the J24 (the truth of the line or
`j_octet`), its MP (`rate`, `up_mask`, `mp_bad`), `silent` at five points.

    make build/test_answer_k56 (engine a stub)   272 checks, 62 failures
    build/test_answer_k56                          273 checks, 0 failures (2.7 s)
      r = 1, pad 0, no robbed bits: 32000 / 31200 (client 32000 / 31200),
        102057 bits down, 100065 up, 0 bad; 0 bad training octets, Js, 16 MP,
        16 MP', E, B1 good; data 8.33 s after ANS_K56; silence 2 12288,
        TRN2A 16336, TRN2B 2052
      mask 0FFF, echo delay measured (0): 32000 / 28800 = sim/v34peer's 28800 up (F9)
      max_down 32000, client asking 13: r = 1, 32000
      our cleardown: octet FF at once, cleared 2
      silent before S / J24 / MP / E / in data: ended 3.00 / 1.87 / 6.04 /
        1.99 / 0.53 s later, K56flex failed (data: lost)
      J24 octet C0: K56D_ERR_J; MP rate_ac 0, mask 0, mask 2000: K56D_ERR_RATE
    make test-api      502 checks, 0 failures: V90M_CONN_K56FLEX 32000 / 31200,
                       SNR 38.1 dB, retrain V90M_ESTATE, cleardown
                       V90M_END_CLEARDOWN_OURS, silent in data V90M_END_LOST; pin holds
    sizes (x86-64): k56d_t 21424 (u's largest is v92d_t 45096), answer_t 106184
    make check         ALL HOST CHECKS PASS
    replay proof (106 recordings, against 0.4.0's v90m-replay):
      flags '', -9, -x, '-x -9': identical (.txt and .bits); -k, -k -9: 106/106 exit 0
    make test-k56-sm56 MODS=k56 DOWN=32000   FAIL (k56-facts.md "Unsettled"):
      SM56 sends INFO1c 0x001 to us, takes our flagged INFO1a, the training to
      Js; we receive its S, J24 (octet 00) and J' with GPC (GPC confirmed);
      it then stays in its front end state 19 during our TRN2A, sends no MP,
      cause 0x21; we end K56D_ERR_NO_MP 4 s into TRN2B. Our octets from R1 to
      TRN2B equal D-Modem's oracle with the same J time, byte for byte.
    make -j16 check-arm ARM_CC=...armv7-eabihf musl...
      answer_k56: 225 checks, 0 failures (TEST_SHORT: 20000 bits each way)
      api: 502 checks, 0 failures (pin 118512 holds)
      ALL ARM CHECKS PASS

## k56 9b: SM56's TRN2A, the bridge's line model (2026-11-21)

k56 9's SM56 gate failure (SM56 stays in front end state 19 during our
TRN2A, no MP, cause `0x21`) was the bridge, not the modem. The
cross-experiment (D-Modem run as a black box, nothing of it in the tree):
our answerer, wrapped as a half program (`half_ours`, outside the
repository), called by SM56 through D-Modem's `interop_call` and netsim
(`NET_LOOP=0.45`, `AT+MS=K56FLEX,0`): K56flex connects, 32000 / 31200,
SM56 `CONNECT 32000`. Through the same netsim with the loop off or at 0,
D-Modem's own server fails exactly as ours did; with `AT+MS=K56FLEX,0` it
fails at 0.45, 0.6 and 0.75 as well. SM56's TRN2A acceptance moves with the
line model and the delay toward it, for either server; what inside SM56
decides it is not known (k56-facts "Harness", "Unsettled").

`tools/k56-peer/bridge.c`: the loop model is now a one-sample delay
(`LOOP=0`; a fraction still through the Lagrange interpolator) and a
first-order 300 Hz high pass (bilinear), and robbed bits go through it too.
The old second-order high pass also spoiled SM56's data above 32000 (error
80 to 140 against 5 to 7). No library change.

    make test-k56-sm56 ... MODS=k56 DOWN=32000      before: FAIL (state 19, cause 0x21)
                                                    after:  PASS, 32000 / 31200, 4 KiB each way
      MODS=k56 (no cap)                             PASS, 52000 / 31200
      MODS=k56 DOWN=32000 RBS=0x08                  PASS (J24 octet 08)
      MODS=k56 RBS=0x24                             PASS, 52000 / 31200
      MODS=none, and RBS=0x08                       PASS, V.34 28800 / 28800
      SM56_MS=AT+MS=V90                             FAIL (Phase 2), as before
      SM56_MS=AT+MS=K56FLEX,1                       PASS, but V.90 29333 (we pick V.90)
      STRICT_RENEG=1                                FAIL (no renegotiation yet: Task 11)
    the default model, 12 repeats (fractions 0 and 0.1)    24 / 24 PASS
    bridge sweep, k56 2 model: pass 0-0.35, 0.65-0.95; fail 0.40-0.60
    make check                                      ALL HOST CHECKS PASS

## k56 10: every rate, the pads and robbed bits; the CPU figure (2026-11-21)

`test/test_answer_k56.c` `k56_rate_tests`: the client (sim/k56call, data
check as k56 9) asking r = 1, 2, 7, 12 (54000, M = 16) and 13 at pad 0;
pads 3 and 6 dB at r = 13 and 7; robbed `0x24`, `0x12`, `0x3f`, `0x1f` (5
taken as 6) at r = 13 and 3, pad 0 and 6 dB. Each: `tx_bps` 30000 + 2000 r
both sides, the J octet's pad and robbed symbols in `k56d_pad`/`k56d_rbs`
and in k56tx (`pad`, `d_pad`, `d_rbs`), the training on that pad's levels
checked octet by octet (0 bad), 16 MP and MP', E, B1, the first data
symbol on the grid of 24 from TRN1B, 100000 bits each way with no error.
Both variants: r = 1 with robbed `0x24` at pad 0 and 6 dB (no bit to rob
at 32000: ignored; fix round 1). `TEST_SHORT` (ARM): r = 13 at pad 0 and 6 dB with robbed none and `0x24`,
r = 1 at 6 dB (r = 1, pad 0 is k56 9's).

The engine (`modem/k56d.c`, k56tx, k56enc) passed every one unchanged. The
first run failed only in the test's own models (43 failures), all three
fixed test-side:

- the sim's training checks: a robbed bit makes two training octets arrive
  alike (TRN1A's `0x4e`/`0x4f`, TRN1B's pairs); the sim took the first and
  its running sum drifted (thousands of "bad" octets, E missed). It now
  breaks such a tie with the octet as it left the server, and counts it bad
  if that is neither;
- the sim's data decoding looked for the received octet among all points;
  on a robbed symbol the server sends only points with bit 0 clear
  (k56-facts "Robbed bits"), and behind a 6 dB pad another point can arrive
  alike: 40000 bad bits at r = 13. It now looks among those points only;
- the test line's echo was of our octets before the network's pad: 6 dB
  too loud behind a 6 dB pad, and the upstream (31200, SNR about 35 dB)
  took 45 to 166 bad bits in some calls (pad 2, with or without robbed
  bits). The echo is now of what the client's line card plays, after the
  pad (pad 0: unchanged).

Mutations (throwaway, reverted): `k56tx_data` given robbed 0: 84 failures;
`k56tx_got_j` given pad 0: 204 failures.

    build/test_answer_k56 (first run)    1064 checks, 43 failures (the three above)
    build/test_answer_k56                1064 checks, 0 failures (3.7 s host)
      r = 1 / 2 / 7 / 12 / 13, pad 0: 32000 / 34000 / 44000 / 54000 / 56000 down, 31200 up
      pad 3 and 6 dB at r = 13, 7: J octet 40 / 80, 0 bad
      robbed 24 / 12 / 3F / 1F on the line: J octets 09 / 24 / 3F / 37 (pad 2: 89 / A4 / BF / B7)
    a sweep (scratch, not kept): all 13 rates x pads 0, 3, 6 dB x robbed
      none, 24, 12, 3F, 1F, 08, 01: 273 calls, 0 failures
    make check                           ALL HOST CHECKS PASS
    SM56 decoder harness: make k56-dump; ~/modem-research/k56-oracle/sm56dec/check.sh build/k56dump
                                         2496 of 2496 decode to the data sent
    make test-k56-sm56 DMODEM=... MODS=k56 (SM56's own rate; no pad run, SM56
      cannot train behind one):
      RBS none                           PASS, 52000 / 31200, SM56 CONNECT 52000, RBS 00
      RBS=0x24                           PASS, 52000 / 31200, SM56 RBS 09
      RBS=0x12, 0x3f, 0x1f, 0x01         PASS, 52000 / 31200, SM56 RBS 12, 3F, 3E, 20
      DOWN=34000 / 40000 / 48000         PASS at that rate
      DOWN=36000 / 44000 / 48000 RBS=0x24, DOWN=50000 RBS=0x12   PASS at that rate

CPU: `tools/rec_k56` (`rec_x2`'s link with K56flex offered, 56000 down,
optional pad and robbed bits), `bench_v90 -k`, `make bench-k56` and
`make cost-k56` (as `cost-x2`, but K56flex's data mode starts at 16.2 s,
after V.8bis, so its second is 20 to 21; V.90's stays 15 to 16). Host and
ARM give the same transmit hash (52c83abc), 742404 bits up, 0 bad.

    make cost-k56 ARM_CC=...armv7-eabihf musl...
      k56 data mode: 34749835 ARM instructions per second of line (second 20 to 21 of the call)
      V.90 data mode: 38722338 ARM instructions per second of line (second 15 to 16 of the call)
      ratio k56 / V.90: 89 %

Under the plan's 120 %, as the design expected: the upstream receiver is
set as the V.90 answerer's (16 states, linear, minimum shaping, asked for
in our MP). Not profiled further.

    replay proof (106 recordings, against 0.4.0's v90m-replay):
      flags '', -9, -x, '-x -9': identical (.txt and .bits); -k, -k -9: 106/106 exit 0
    make -j16 check-arm ARM_CC=...armv7-eabihf musl...
      answer_k56: 396 checks, 0 failures (TEST_SHORT; 9.3 s under qemu here)
      api: 502 checks, 0 failures (pin 118512 holds)
      ALL ARM CHECKS PASS

## k56 11: the client's rate renegotiation (2026-11-21)

`modem/k56d`: the client's S in data mode starts `K56D_RENEG` (design
decision 13, k56-facts.md "Rate renegotiation"). Our data stops at the end
of the frame being sent (`k56tx_reneg`), then R2, TRN2A 2048, MARK,
TRN2B until its MP and the grid of 24, MP and MP' at 1 bit a symbol, E 24
and data (B1 first, a new encoder: Ruling C4). Upstream its S-bar, TRN, MP,
MP' and E on 4 points (V.34 11.6, as `modem/v34dx` and x2d's rate change),
then V.34 data at the rate our new MP gives. An MP with rate 0 both ways
ends the call as the far end's cleardown (`K56D_ERR_CLEARDOWN`,
`V90M_END_CLEARDOWN_FAR`); not back in data mode within `K56D_T_RENEG`
(6 s from its S) is `K56D_ERR_RENEG` (`V90M_END_K56_FAILED`).
`answer_moving` is 0 in between, so the API gives DATA_STOPPED, RATES,
DATA_MOVING. `v90m_mem_size()` unchanged (118824 x86-64, 118512 ARM).

`sim/k56call` renegotiates (`reneg_ms`, `reneg_rate`, `reneg_ms2`,
`reneg_rate2`, `reneg_rate0`, `K56C_SIL_RENEG`) and checks our answer as
it checks the training: R as a whole frame, TRN2A's length, the hidden
second bit of TRN2A's and MARK's even symbols at 2 bits a symbol (so the
training's scrambler, history and running sum are checked going on from
the last E), MP / MP' / E on the 2 magnitudes, the grid; R in data it did
not ask for is counted.

    test_answer_k56 (host): 13 -> 10 -> 12, 5 s apart, pad 0, robbed none and 0x24:
      tx_bps 56000, 50000, 54000; every training octet as the facts; MP 32, MP' 32;
      3 data segments each way, 0 bad (e.g. none: down 274985 / 246186 / 173114,
      up 153362 / 153729 / 100065 bits); rate 0: cleardown 0.26 s after its S;
      silent after its S: ended 5.98 s later; 60 s of data, no R of ours
    test_api: events 1 4 3 2 4 3 2 4 3 5, RATES at 50000 then 54000;
      CLEARDOWN_FAR; K56_FAILED
    make check: ALL HOST CHECKS PASS
    PAR=$(nproc) make check-arm ARM_CC=...armv7-eabihf musl...: ALL ARM CHECKS PASS
      (answer_k56 586 checks, api 525, TEST_SHORT)

SM56 (information, `make test-k56-sm56 MODS=k56 STRICT_RENEG=1`, 120 s of
line): it did not renegotiate. Data at 52000 / 31200 from 22.1 s, the first
4 KiB each way byte for byte, then 98 s of data mode with no S from it and
no rate change; the same with `RBS=24`. So our renegotiation is checked by
the simulated client only; what SM56 does after one is still not seen
[our observation].

## k56 12: following the client's retrain (2026-11-21)

`modem/k56d`: the client's tone B (1200 Hz, `tonedet`, more than 50 ms) on
the upstream in data mode is its retrain (design decision 14, k56-facts.md
"Retrain") -> `K56D_RETRAIN`, silent. `modem/answer`: phase `ANS_K56RT`,
70 ms of silence, then `p2ans_init_retrain` on `v34.p2` with
`p2ans_cfg.k56 = 1` and the INFO0c of the first Phase 2; on `P2N_K56` (our
INFO1a flagged again) `start_k56`: the whole training from silence 1 and
data. Any other end of that Phase 2 (INFO1c 0x200: `P2N_DONE`; a failure;
not done 9 s after the retrain began) ends the call with `k56_failed`
(`V90M_END_K56_FAILED`): no way back to V.34. `retraining` is 1 from the
retrain to data again, so the API gives DATA_STOPPED, DATA_MOVING and no
renegotiation; `retrains` counts them; `v90m_retrain` stays `V90M_ESTATE`.

Retrains in a row: the third ends the call (`MAX_RETRAINS`, as K56flex's:
`rt_recent` reaching 3). K56flex forgets them all once data mode has held
2 s (`K56_RT_DATA_MS`), not one a minute as V.90, so three retrains 5 s of
data apart go on. V.90, V.92, V.34 and x2 retrain counting is not touched.
`v90m_mem_size()` unchanged (118824 x86-64, 118512 ARM).

`sim/k56call` retrains (`retrain_ms` into data mode and into the data
after each, `retrains` of them, `retrain_200`): 70 ms of silence, then
v34peer's Phase 2 of a retrain, and after our INFO1a flagged again its
whole training afresh (`k56_starts`), checked octet by octet as the first.

    test_answer_k56 (host): one retrain 5 s into data mode, r = 13, pad 0, robbed
      none and 0x24: ANS_K56RT 0.143 s after its retrain began, Phase 2 2.03 s, data
      again 10.50 s after it (the first training: 8.33 s); every training octet as
      the facts, MP 32, MP' 32; data before and after, each way, 0 bad. Three
      300 ms apart: the third ends the call 0.143 s after it began, K56_FAILED;
      three 5 s apart: 4 data segments, 0 bad, still in data; INFO1c 0x200 in the
      retrain: ended 2.17 s after it began, K56_FAILED
    test_api: events 1 4 3 4 3 5, retrains 1, renegotiations 0, v90m_retrain
      V90M_ESTATE in it; three in a row: retrains 3, K56_FAILED
    make check: ALL HOST CHECKS PASS (answer_k56 1407 checks, api 542)
    make check-arm (armv7 musl, TEST_SHORT: the single retrain only):
      ALL ARM CHECKS PASS (answer_k56 636 checks)

## k56 13: release 0.5.0, experimental, off by default (2026-11-21)

main (5db64c4: the CI workflow) merged into k56flex first, so that the
ARMv5 job's fix edits the real `ci.yml`.

- **Label L1** (spec "Release labels"): the SM56 decoder harness and the
  SM56 full call pass (below). In CHANGELOG, README, `docs/limits.md`,
  `docs/answering.md`: "connects to Motorola's SM56 soft modem run under
  emulation, V.42 data both ways at 32000 to 52000 bit/s down and 31200
  up, with no pad and with robbed bits; its decoder takes our data at
  every rate, pad and robbed-bit pattern; not yet tried on a Rockwell or
  Lucent modem." The rates: SM56 full calls at 32000 (k56 9b, `DOWN=32000`),
  34000 to 50000 capped (k56 10), 52000 uncapped (k56 9b, k56 10, and
  this step's runs); 31200 up in every one.
- **Version 0.5.0**: Makefile, header, `v90m_version()`, test_api's version
  checks. `v90m_mem_size()` unchanged (118824 x86-64, 118512 32-bit ARM).
- **Docs**: CHANGELOG 0.5.0; README (K56flex in the list and the standards
  table, how to switch x2 and K56flex on, what was tried, the CPU table);
  DESIGN 4.2b (K56flex done, the stated exception as x2's); answering.md (a
  K56flex section), api.md (`V90M_MOD_K56`, `V90M_CONN_K56FLEX`,
  `V90M_END_K56_FAILED`, retrain and cleardown in K56flex), limits.md (a
  K56flex section: CRe, not MRe, and the delays measured in k56 5; the
  31200 upstream's margin behind a strong echo; the client's
  renegotiation on 4 points, unconfirmed; retrains forgotten after 2 s of
  data, our value; the retrain's Phase 2 limited to 9 s; SM56 in
  automode gets V.90), testing.md (K56flex's checks, `ARM_CC` for
  check-arm), the header's comments on `V90M_MOD_K56`, retrain, cleardown
  and `V90M_END_K56_FAILED`; `docs/history/k56-handover.md` (the owner's
  line tests, the v90d change, what is not settled).
- **CI, ARMv5**: on main the job took 2 h 57 min of its 180. Its limit is
  now 300 minutes (GitHub's per job: 360), and the API test's long
  K56flex calls have `TEST_SHORT` versions (one renegotiation, not the
  client silent after its S; the single retrain, not three in a row).
- **Found by the checks**: `make check-ubsan` did not build. clang 21's
  `-Wunused-but-set-variable` flagged `ones` in `sim/k56call.c` (k56 9), and
  it is a `-Werror` build. The dead variable is removed (test code only).

Checks on the release tree:

| check | result |
| --- | --- |
| `make check` | ALL HOST CHECKS PASS (api 542, p2ans 402, answer_k56 1418) |
| 0.4.0's counts (`make check` on 704b370 against this tree) | every suite of 0.4.0 the same, but api 458 -> 542 and p2ans 259 -> 402 (the K56flex tests of k56 5, 6, 9, 11, 12); new: answer_k56, k56enc, k56tx, test_k56tab, test_v8bis, test_v8bismsg |
| `make check-ubsan` | ALL UBSAN CHECKS PASS (after the fix above) |
| `PAR=4 make check-arm ARM_CC=...armv5-eabi uClibc...` | ALL ARM CHECKS PASS; api 532 (TEST_SHORT), answer_k56 636 |
| replay proof, 106 live-050 recordings, 704b370 (0.4.0 + the x2 gate) against this tree | `""`, `-9`, `-x`, `-x -9`: identical (106 .txt; 21 / 40 / 21 / 40 .bits); `-k`, `-k -9`: 106 of 106 complete |
| `make crd-scan RECS=~/sync-build/live-050` | 106 recordings, 0 CRd detections |
| `make cost-k56` (ARMv5 uClibc) | K56flex 38985309, V.90 38935592 instructions a second of line: 100 % (armv7 musl in k56 10: 89 %) |
| `make test-k56-sm56 MODS=k56`, RBS none / `24` | PASS, 52000 / 31200, 4 KiB each way identical (SM56: RBS 00 / 06) |
| `make test-k56-sm56 MODS=k56 DOWN=32000` | PASS, 32000 / 31200 |
| SM56's decoder (`make k56-dump`, `sm56dec/check.sh`) | 2496 of 2496 decode to the data sent |
| `PAR=1 tools/check-ports.sh build/ports-k56 host-gcc host-clang i686-musl aarch64be-musl mips-musl win32-mingw` | host-clang, i686-musl, aarch64be-musl (PAR=1, in `build/ports-k56`) and mips-musl, win32-mingw (PAR=3, in `build/ports-k56b`, run beside it to save hours, each against its own host-gcc run): 121 of 121 programs identical to host-gcc each; ALL PORTS PASS |

check-arm time, ARMv5 under qemu, PAR=4 as on CI's four cores, the
programs already built: 4180 s before the API test's shortening, 3218 s
after. Most of that difference is the host's other load during the first
run, not the change. The API test itself went from 8.5 s to 7.2 s. K56flex's
programs are about 40 s of some 10800 to 13500 program-seconds (answer_k56
29 to 32 s). The long ones are older: test_p34 alone takes 1250 to 1575 s,
then orig_v34, orig_retrain, orig_v32, v92a and orig_v90 take 500 to 630 s
each. So 0.4.0's 2 h 57 min on CI was not K56flex's doing. The raised limit
is what gives the headroom; shortening p34 and the orig_* tests on ARM
would be the next step.

### k56 13, fix round 1: x2-upstream merged; the live x2 call (2026-11-21)

**The owner's first live x2 call:** a U.S. Robotics 56K FAX EXT (flash
5.1.4) called the box. It ran x2 at 56000 / 28800, with PPP, for 2 min 50 s,
before the fixes below. The modem's own counters: 24 of 255 blocks resent.
Its Tone A retrain, some 153 s into data mode, went unanswered, and the call
ended ("Unable to Retrain"). The investigation is on branch `x2-upstream`;
the facts are in `docs/history/x2-facts.md`:

- 10 of the resends came from two frames lost at the client's two rate
  changes. Each was sent after fill ones with its opening flag missing.
- The other 14 were checkpoint resends after its idle polls.
- The upstream had no FCS errors.

**The confirming call** (2026-11-21): the same USR, flash 5.1.4, called
the box running a v90d 0.7.0 bench build with libv90modem af86c3f and the
`v90m_ec_line_stopped` glue. It connected x2 at 56000 / 28800 with PPP and
held 3 min 50 s, until we hung up. It made 2 rate changes up and no
retrain; the USR gave SNR 59.7 dB. Its counters, against the call before
the fixes:

| | before | after |
| --- | --- | --- |
| blocks sent | 255 | 293 |
| resent | 24 | 4 |
| link NAKs | 12 | 2 |
| BLER | 1 | 0 |
| pings across PPP answered | 76 of 90 | 90 of 90 |
| worst ping | 2.9 s | 590 ms (average 148 ms) |

The USR asked for no retrain on this call, so the cleardown on its Tone A
is tested only by the replay of the earlier recording.

Merged here (x2-upstream, af86c3f):

- the V.42 receiver takes a frame whose opening flag was lost after a run
  of ones when its FCS holds;
- `v90m_ec_line_stopped` (new, public), for the falling edge of `moving`,
  with `v90m_ec_line_restored` aborting only the frame the stop cut and
  opening with two whole flags;
- x2d ends the call on the client's Tone A in data mode
  (`V90M_END_RETRAIN_FAILED`);
- `v90m-replay` decodes x2 data from the recorded transmit and prints each
  state change.

CHANGELOG 0.5.0, README (`v90m_ec_mem_size()` 49792 on x86-64, 32 more),
error-control.md, getting-started.md, answering.md and limits.md (an x2
retrain now ends as `V90M_END_RETRAIN_FAILED`, not by the watchdog) follow
the merge. `tools/x2-peer/bridge.c` and `tools/k56-peer/bridge.c` now call
`v90m_ec_line_stopped` on `V90M_EV_DATA_STOPPED`. Review fixes: the version in
api.md and getting-started.md; the CHANGELOG preamble and wrap; the CI
comment; DESIGN's PROGRESS range; testing.md's TEST_SHORT list; README's
CPU row.

Replay proof after the merge: 704b370 rebuilt in a temporary worktree
(removed again), the 106 recordings, flags `""`, `-9`, `-x`, `-x -9`.
Every `.bits` is identical. Every `.txt` differs only by the replay tool's
new lines `modem connected C moving M retraining R`; with those removed,
424 of 424 are identical. `-k`, `-k -9`: 106 of 106 complete.

Checks after the merge: `make check` ALL HOST CHECKS PASS (api 542,
answer_x2 386, answer_k56 1418, test_hdlc 4312, test_v42 5634); `make
check-ubsan` ALL UBSAN CHECKS PASS; `PAR=8 make check-arm` (armv5 uClibc)
ALL ARM CHECKS PASS; `make test-k56-sm56 MODS=k56` PASS, 52000 / 31200.

## m68k: a GCC wrong-code bug, not qemu (0.6.0 audit, Task 5b)

The twelve m68k failures of ports-2 were the compiler's. GCC 12.3.0 for
m68k (Bootlin `m68k-68xxx--uclibc--stable-2023.08-1`, `--with-cpu=68040`),
with `-fomit-frame-pointer` (the default at -O1 and up), compiles
`(uint64_t)(int)x << 32` divided by a 64-bit libcall (`__udivdi3`,
`__divdi3`) with x an incoming stack argument as: push the divisor, `subq.l
#8,%sp` for the dividend, then load x from an sp-relative offset that
counts the pushes but not the `subq`, 8 bytes short. So it reads another
argument or the return address. In the tree:

- `modem/qamrx.c`, `qamrx_init`: `dcar` came from the address of the
  `qamrx_t` (`r`) instead of `car_num`, so the V.34 receiver's carrier ran
  at the wrong frequency and never trained: the twelve failures. The
  wrong value depends on where the linker puts the struct, which is why
  some rebuilds passed by chance (and almost certainly why "fails
  differently with `-one-insn-per-tb`" was two differently linked binaries,
  not the emulator).
- `modem/v90arx.c`, `v90arx_clock`: `ppm_q8` was the return address, so
  the answering V.90 receiver's clock read a value that depends on the
  binary's link layout (no test looked at it; a check program printed 4290672329 for
  every ppm against the host's 4294967296 + ppm * 2^32 / 256e6).

Fix: `qamrx_init` takes `(uint64_t)(uint32_t)car_num << 32` (only the low
32 bits survive the shift, so the same value; zero-extended, gcc uses a
different insn), and `v90arx_clock` divides `ppm_q8 * 2^21` by 125000
instead of `ppm_q8 * 2^32` by 256000000 (the same fraction, so the same
truncated quotient; no shift by 32). Both checked equal to the old
expressions for all 2^32 int32 inputs on the host. An m68k assembly scan
of every tracked C file (all but the two spandsp peers, which need
spandsp) at -O1, -O2, -Os and -O3, for `subq.l #N,%sp; move.l %sp,%aK`
followed by an sp-relative source, found these two sites and no others
before the change, and none after. The port matrix also builds m68k with
`-fno-omit-frame-pointer`, with which the suite passed under GCC 12.3,
against new sites; m68k gates in `make verify-phase` like every other target.

qemu-m68k was cleared: 300,000 random operand sets through `mulu.l`,
`muls.l`, `divu.l`, `divs.l` (64-bit forms, results and flags) matched a
reference written from the 68040 manual, in normal mode and with
`-one-insn-per-tb`.

A newer gcc (m68k moved to LRA in 14) was not tried; the bug was not
reported upstream (the owner's call; a draft is kept outside the
repository).
