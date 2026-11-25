# libv90modem: design

How the modem is built and why. The API is in `include/v90modem.h`; what was
measured and when is in `docs/history/PROGRESS.md` (the calling side and
everything since the library got its own repository) and, for the
answering side before that, in v90d's `PROGRESS.md`, where this code grew.
User documentation is in `docs/`.

## 1. Rules

- Portable C99. Fixed-point only in the signal path; the simulator (`sim/`)
  may use double, it is a test peer and never installed.
- No allocation after `v90m_create()` (or none at all with `v90m_init()` on
  memory of the caller's), no global state: any number of modems in one
  process (two calls at once run on the HT802, one modem per port).
- Every block builds and passes its tests on x86 and on an ARM926 (cross-built,
  run under qemu-arm); the port matrix (`tools/check-ports.sh`) runs the
  whole suite on the other targets and wants the same output from each.
- Written from the ITU-T recommendations. Reference implementations (spandsp)
  are test peers only: linked into host-only interop tests, never shipped,
  never copied.
- One sample in, one sample out underneath; `v90m_process_*` runs blocks of
  any length through it, and the result does not depend on how the samples
  are cut into blocks (`test/api/test_api.c` checks it).

## 2. Layers

| | what |
|---|---|
| `api/` | the public API over everything below: configuration, the block calls, events found by reading the modem's state after each sample, stats, the start-up diagnostics (`v90modem_diag.c`); start-stop (V.14) and V.42 objects |
| `modem/` | answering modem: V.8, V.21 and Bell 103, V.22bis/V.22 and Bell 212A, V.14, V.90 Phase 2 (`p2d`), Phases 3/4 and data (`p34d`); V.34 answering (`p2ans`, `v34mp`, `v34mod`, `v34dx`, `v34ans`); x2 (`x2d`) and K56flex (`k56d`). One V.34 upstream receiver, `v34up` (on `qamrx`, `ec`, `v34train`), serves `p34d`, `v34dx`, `x2d` and `k56d`. The sequencer `answer` keeps V.8, the dispatch, the end codes and the backstop; each group of phases is a scheme behind `ans_scheme_t` (`ans_scheme.h`): `ans_legacy.c` (V.21, Bell, V.22bis / V.22 / Bell 212A, V.32bis), `ans_v90.c` (Phase 2, V.90, V.92), `ans_v34.c`, `ans_x2.c`, `ans_k56.c` (V.8bis, K56flex), entered by a phase table. Calling modem: V.8 (`v8orig`), V.34 (`p2call`, `v34orig`), the V.90 / V.92 analogue modem (`v90a` with `v90arx`, `v90dil`, `v90adec`; for V.92 `pcmtx`, `v92aseq` and `v92/v92_enc`); `v90a.c` keeps init, the tick, the public calls and the end, `v90a_up90.c` V.90's V.34 upstream, `v90a_up92.c` V.92's Phase 3 and PCM upstream, `v90a_down.c` the downstream sequences and the designs, `v90a_watch.c` the time limits, the receiver's watch, renegotiation and retrain (shared declarations in `v90a_priv.h`); Bell 212A and 103 (`belltone`, the 2225 Hz answer tone); the sequencer `orig`, whose engines after V.8 share one union. Shared helpers: `p2common` (what the three Phase 2 exchanges share), `seqhunt` (the frame-sync hunter), `clock.h` (clocks past 2^31 samples), `fixdiv.h`, `divrcp.h`, `fixmath.h`, `interp2.h` (exact cheaper arithmetic), `compiler.h` (the only compiler extensions, each with a C99 fallback) |
| `v32/` | V.32bis / V.32 (4800 to 14400 bit/s), answering (`v32.c`) and calling (`v32_call.c`) |
| `v34/` | V.34 coding: training point sequences, data encoder and decoder (trellis, shell mapping, non-linear encoder) |
| `v90/` | V.90 digital transmitter from Phase 3 on (PCM signals, Jd, DIL, MP, data encoder), CP codec |
| `v92/` | V.92 PCM upstream, digital side: start-up sequences, the receiver for the caller's training signals, the CPd design, the data decoder, Phases 3/4 and data (`v92_p34d`) |
| `v42/` | V.42 LAPM, V.42bis, V.44, detection phase, start-stop fallback |
| `sim/`, `test/` | channel simulator and calling peers; the test suite (never installed) |
| `tools/`, `rig/` | the replay tool and its trace printers, recorders and benches, table generators |

## 3. The API layer

`api/v90modem.c` adds no signal processing. It maps `v90m_cfg` onto the
answering modem's configuration (`answer`) or the calling modem's (`orig`,
by `v90m_cfg.role`), clears the modem's memory before every start (the code
underneath expects zeroed memory), runs `answer_tick_pcm`, `answer_tick_lin`
or `answer_tick` (calling: `orig_tick`, with the G.711 conversion around it)
once per sample, and after each sample
compares the modem's state with the one before to report `V90M_EV_*`
(connect, rates, data stopped / moving, end) and to count renegotiations.
The bit callbacks are called by the modem itself, inside that sample, at
the moment a bit is needed or decided: a V.42 endpoint (or a framer) on the
other side sees exactly the timing it would see sample by sample.

`api/v90modem_diag.c` (the start-up diagnostics, `v90m_get_diag`) reads the
modem too, never changes it: after each sample that did not end the call
it keeps the modem's phase, the furthest public phase reached (it never
goes back) and the modulation chosen, and whether data mode was reached
(`data_reached`: user data moved both ways, the same signal as
`v90m_state.moving`); while data moves, the phase reached is data and
the modem's own phase is the one last tracked, a sample skips the
tracking (most of data mode: a retrain stops the data first); the cause of the end is read when asked, from the
engine of that last phase, whose state nothing changes after the end. An
orderly cleardown after data mode reads error 0; the start-up failed if
the call ended and data mode was never reached. An error code is a
family (an engine's enum of errors, or one of the causes no engine
numbers) times 100 plus the engine's own number, named by the engine's own
name.

Everything but the `v90m_*` functions has hidden visibility: the shared
library exports nothing else, and the static library is linked into one
object (`ld -r`) whose internal symbols are renamed `v90m__i_*`
(`tools/symmap.sh`, `objcopy --redefine-syms`), so a program's own symbols
cannot clash with them. Renamed rather than made local: some targets'
relocations (i686's PC thunks, MIPS's GOT16) cannot reach a local symbol
the way they reach a global one. `make check-symbols` checks it.

## 4. The modem

Moved here from v90d's DESIGN.md (section 4 there) with the code.

### 4.1 Start-up
`modem/answer` runs the call: 200 ms silence, ANSam with phase reversals, V.8
(`v8rx`, `v8msg`, `v8ans`): receive CM, answer JM, wait CJ. The menu decides:

- V.90 offered by the caller (PCM analogue modem): Phase 2 (`p2d`), then
  Phases 3 and 4 and V.90 data.
- With V.92 allowed (`V90M_MOD_V92`): the same Phase 2 says so (INFO0d bit 27);
  a caller that is V.92 too and selects PCM upstream in its INFO1a gets
  V.92 Phases 3 and 4 and data (`v92/v92_p34d`); every other caller goes
  the V.90 or V.34 way as before. V.8 is the same either way.
- otherwise V.22bis / V.22 (`v22tx`, `v22rx`, `v22bis`) or V.21 (`v21`).
- no V.8 at all: the V.22bis answer sequence after ANSam, V.21 after 8 s.
- Bell 103 (300 bit/s, the V.21 modulator and demodulator on that system's
  tones: we send 2225 / 2025 Hz and receive 1270 / 1070 Hz) for a caller
  without V.8 that sends a steady 1270 Hz while we wait for it. Such a
  modem takes the answer tone or the unscrambled ones of the V.22bis answer
  sequence (2250 Hz is in them) for its carrier and replies with its mark.
  The tone watch (`tonedet`) sits behind a receive band-pass
  (`bell103_bpf.h`) that keeps our own signal out, and wants 200 ms of the
  tone in the V.22bis wait, 60 ms in the V.21 wait (before that receiver's
  100 ms would take the tone for a V.21 carrier). On with
  `V90M_MOD_BELL103` (`ANS_MODE_BELL103`). No error control, as in V.21.
- Bell 212A (1200 bit/s; modulation and scrambler are V.22's, the answer
  tone is not). A caller without V.8 that has not answered 4 s of the
  V.22bis answer sequence gets 2225 Hz, the Bell answer tone, in place of
  the unscrambled ones for the next 4 s, while the V.22bis modem goes on
  listening: single-chip modems of that kind detect 2225 Hz within 10 Hz
  and take neither 2100 Hz nor 2250 Hz for it. What the caller then sends
  is what a V.22 caller sends, and the start-up goes on as V.22's. A call
  that began during the tone is reported as Bell 212A. The same tone serves
  a Bell 103 caller that is as strict. On with `V90M_MOD_BELL212A`
  (`ANS_MODE_BELL212A`) and V.22. Simulated only: no modem that is known to
  call as 212A and nothing else has been on the line.

### 4.2 V.90, digital side
Downstream (us to caller) is PCM at 8000 symbols/s; upstream is V.34 at 3200 or
3000 symbols/s.

Phase 2 (`info`, `dpsk`, `probe`, `p2d`): INFO sequences as 600 bit/s DPSK and
tones A/B from one shaped transmitter; phase reversals timed to the sample for
the round-trip delay; L1/L2 analysed with a 160-sample block DFT (the probing
signal repeats every 53.33 samples, so three periods are exactly one block and
every probing tone is orthogonal to the tones A/B and to the four gaps).
INFO1d offers only the upstream symbol rates our receiver implements.

Phases 3 and 4 (`p34d`): the caller trains us first (S, S-bar, PP, TRN, then the DIL
descriptor in Ja) while we are silent, then we send Sd, TRN1d, Jd and the DIL
it asked for while it is silent, then both run (Ri, TRN2d, MP against CPt, CP,
E), then B1 / B1d and data.

Receive chain for the upstream signal:

```
 8 kHz samples -> echo canceller (ec) -> S detector (v34train) ......... timing reference
                        |
                        v
   qamrx: interpolate to 3 samples/symbol (Kaiser sinc bank, linear between branches)
          -> AGC -> T/3 equaliser, 72 complex taps on the real pass-band signal
          -> carrier derotation -> point per symbol
                        |
                        v
   v34 decoder: 4-point decisions for Ja / CP / E, then Viterbi + inverse
   shell mapping + descrambler for data
```

- No Hilbert filter: the equaliser's complex taps on real input do the image
  rejection, which a separate filter could only do with a very long response
  (the signal runs from about 300 Hz to 3700 Hz).
- Carrier loop: decision-directed, second order. Timing loop: decision-directed
  MMSE gradient (equaliser output a third of a symbol early and late, each
  derotated for its own instant), second order, plus a slow pull that keeps
  the equaliser taps centred, because the equaliser can absorb any timing
  phase and would otherwise wander.
- The echo canceller learns the hybrid during TRN1d, when the far end is
  silent, at full NLMS step; afterwards it is frozen. It spans 12 ms, placed
  where the echo is: before training, the distance to the echo is found by
  correlating a stretch of received TRN1d with what was sent (a block-based
  line driver puts tens of milliseconds between the two). The stretch is
  taken as the change from sample to sample: a modem's line transformer
  can return our low frequencies as a tail of tens of milliseconds, larger
  in sum than the start of the echo, and that tail moved the peak (a
  U.S. Robotics 5686; v90d's PROGRESS.md, 2026-11-16).
- Adaptive filters round their updates. Truncation is a steady pull on every
  tap that takes a minute or an hour to show.
- TRN2d is sent for 1200 data frames (0.9 s) before MP, and Tone A is
  listened for through Phase 4 as in data mode: a U.S. Robotics 56K of 1998
  gave the start-up up with 0.3 s of TRN2d and asked for a retrain that
  went unheard (v90d's PROGRESS.md, 2026-11-16).
- The caller falls silent twice (during TRN1d / Jd and, with a DIL, again
  until it has seen enough of it) and comes back with S. Its clock has
  drifted and its carrier phase is anybody's guess, but the line is the
  same: the equaliser is kept, the carrier phase comes from the mean of S
  (its carrier line), the timing phase from the known alternation with a
  fast loop, the symbol count from the S / S-bar boundary (`sres`).
- The transmit side is integer logic only (`v90/`): scrambler, modulus
  encoder, constellation mapping from the caller's CP, spectral shaping, one
  G.711 octet per sample.

We dictate the caller's upstream transmitter through INFO1d and MP: symbol
rate, carrier, 16-state trellis code, no non-linear encoding, no precoding
(zero coefficients), minimum shaping. That keeps the receiver small without
giving up anything the line would not also cost.

**In data mode** (`p34d`, V.90 9.5 to 9.7). Three watches run on the echo
cancelled signal: loss of the upstream signal, the caller's S (the Phase 3
tone-pair detector), and Tone A (the share of each 10 ms block's power at
2400 Hz; the data signal leaves a few per cent there, Tone A with its guard
tone 80 %).

Two pieces are shared by the three engines that have a line echo to deal
with (`p34d`, `v92/v92_p34d`, `v34dx`), each in one place:
`modem/ecdly` keeps what was sent for the canceller's delayed reference and
measures how late the echo comes back (on plain samples for V.34, on the
change from sample to sample for V.90 and V.92, see above on the 5686), and
`modem/tonedet` is the watch for the retrain tones (Tone A at 2400 Hz, Tone
B at 1200 Hz). Each engine once had its own copy, and a correction had to
be made three times. What the engines do with the result stays with
them: the length of TRN2d, for one, differs on purpose (1200 data frames in
V.90 for the 1998 U.S. Robotics, 400 in V.92, which that modem does not
speak).

- S starts a rate renegotiation: `sres` follows S to its S-bar with the
  carrier already locked (`sres_resume`), the 4-point sequence receiver takes
  the CP, the transmitter sends Rd (384 T from a data frame boundary), Rd-bar,
  TRN2d, MP, MP', Ed, B1d. The V.90 transmitter never loses data frame
  alignment, and queued user bits are sent before Rd begins. With the
  request for silence (CPs): Ed, silence, and Rt, Rt-bar, MP once a CP
  without the request arrives.
- drn = 0 in a CP or in our MP is the cleardown.
- Tone A, or a renegotiation stalled for 6 s, is a retrain: p34d stops in
  `P34_RETRAIN`, `answer` sends 70 ms of silence, runs Phase 2 again from
  Tone B (`p2d_init_retrain`) and then a fresh p34d. `answer.retraining`
  tells the layer above; `dce` (v90d's driver) holds V.42 and PPP, timers
  included, until data mode is back (about 4.3 s in simulation).
- The upstream rate is ours to set (MP). When the receiver's error stays
  above what the rate needs for a second, we renegotiate with a lower
  ceiling; when nothing lower is left, or nothing is received, we retrain.
  `answer` lets a call go at the fourth retrain in a row.
- `dce` ends every orderly call with a cleardown after the LAPM release, so
  the caller's modem reports the end at once instead of trying to retrain
  into silence.

### 4.2a V.92, digital side
Downstream is V.90's. Upstream the caller sends PCM-coded symbols at 8000/s
through its own precoder, at 24000 to 48000 bit/s; we measure the channel
from its training signals, design its constellations and filters (CPd) and
decode. `v92/README.md` has the detail and every reading of the text.
- `answer` holds `v92d_t` in a union with the V.32bis modem; a call is never
  both.
- A retrain runs the V.92 Phase 2 again and the caller selects again; PCM
  upstream stays on offer after any retrain of a call that reached V.92
  data mode, whoever began it. When a V.92 start-up never reached data
  mode, the V.92 engine failed, or it retrains because the upstream did not
  hold even the lowest PCM rate (`v92d_t.up_floor`), the retrain's INFO1d
  withdraws PCM upstream and the call goes on as V.90; `dce` follows the
  modulation and the rates.
- Two V.92 modems that both named LAPM in V.8 skip the ODP / ADP of V.42
  (V.92 9.3.1); `dce` then gives the detection phase 5 s for the caller's
  first frame. The server offers V.92 only with V.42 on.
- A carrier watch ends the call when the caller is gone (V.92 has no
  carrier detector of its own in data mode).
- What a real modem (CX93001) needed is in `v92/README.md`, stage 4: a model
  of the slow echo tail behind the canceller, the CPd design spread over
  time (one step every 4 samples on the unit then, 2 now), z2(64) kept at zero, the
  first data frame after E2u judged before B1u is counted, no assumed
  first state in the trellis decoder.
- What a second one (U.S. Robotics 5686) needed, v90d's PROGRESS.md of 2026-11-16:
  it waits 3.8 s for SUVd and 3.5 s more for CPd, so the design takes one
  step every 2 samples and SUVd goes out 2.5 s into Phase 4 at the latest;
  the first CPd never carries the acknowledge bit (one that did was
  answered with silence); in data mode the receiver follows the baseline
  under the caller's symbols with each decision's error, because what is
  left of our own low frequencies wanders there. With a G.711 codec on the
  receive path that step is scaled down where the codec's quantising noise
  exceeds the line's, and fades out for errors between a quarter and a half
  of the spacing (`base_step()` in `v92/v92_p34d.c`): the codec's large
  steps and near-boundary decisions had pushed it the wrong way (bursts of
  upstream errors at 34666 to 37333 bit/s, `docs/history/PROGRESS.md`
  answer-fix 1).

### 4.2b x2 and K56flex, digital (server) side
The two 56k schemes from before V.90 have no published specification.

**x2 (U.S. Robotics), done on the answering side (0.4.0).** It is agreed
inside V.8 and the V.34 Phase 2: when `V90M_MOD_X2` is on and the caller's
CM offers V.34 and no PCM on a mu-law line, our JM (its sign flipped after
each, the access octet returned digital) and INFO0a are an x2 server's,
and a U.S. Robotics client answers our L1 with its marker (`modem/v8ans`,
`modem/p2ans`). After the marker `modem/x2d` runs the call: the client's
training and MP on the V.34 upstream receiver, our sync frames, records
(`modem/x2rec`) and start segment, then data, PCM downstream at the rate
index the client asks for (`modem/x2tx`, the mapper `modem/x2map`: six
positions a frame, each with its own level list, a mixed-radix number of
B bits, signs differential, the sixth sign free), V.34 upstream as the
client dictates it (3200 baud, 64-state trellis, non-linear encoding,
shaping). The client's rate changes in data mode are answered with a
marker, records and the new levels for the rate and pad its MP names.
There is no way back to V.34 after the marker: any failure ends the call
(`V90M_END_X2_FAILED`). Neither A-law nor robbed bits are handled.

This is the stated exception to "written from the recommendations": the
facts come from third parties' reverse engineering (D-Modem, courier-emu)
and our own disassembly of the original firmware, each fact tagged with
its source in `docs/history/x2-facts.md`; no code of either project is
here. The level banks are included data (`modem/x2bank.c`, generated by
`tools/gen_x2_banks.py` from D-Modem's published values; no rule
reproduces them). The design and its rulings are in
`docs/superpowers/specs/2026-11-20-x2-answering-design.md` and
`docs/history/PROGRESS.md` ("x2 4" to "x2 9"; the facts and the
Courier harness of the first steps are in `docs/history/x2-facts.md`).

Tested against a simulated client (`sim/x2call`) and an emulated U.S.
Robotics Courier (courier-emu, outside the repository; `tools/x2-peer/`).
At 32000 the Courier decodes our data; above it our octets equal D-Modem's
mapper (as an oracle, `test/test_x2map.c`) and the I-modem's line octets,
but the published emulator decodes no server there. A real U.S. Robotics
56K FAX EXT then held x2 at 56000 / 28800 with PPP for 3 min 50 s through
two rate changes (0.5.0, `docs/history/PROGRESS.md` "k56 13, fix round 1");
a client's retrain is still not served (the call ends).

**K56flex (Rockwell / Lucent), done on the answering side (0.5.0,
experimental, off by default).** It is agreed in V.8bis, before V.8, and
again in INFO1. With `V90M_MOD_K56` on a mu-law line the answerer starts
with `modem/v8bis` (in `answer_t.u` while it runs; the messages in
`modem/v8bismsg`): at least 400 ms of silence, CRe, and to a caller that
answers with CRd our CL with the K56flex block in its NS field (the CL a
real server sent, decoded from a recording); an MS whose NS carries the
block is answered with ACK(1), and K56flex is agreed. V.8 then runs as
always and chooses the modulation: a caller offering PCM while V.90 is
offered gets V.90, and an agreement turns x2 off for the call. In the V.34
Phase 2 of an agreed call, an INFO1c whose frequency offset is `0x001`, with
3200 baud chosen for the upstream, gets INFO1a flagged `0x001` (mu-law)
and Phase 2 ends in `P2N_K56` (`modem/p2ans`); any other agreed call is
V.34 (a measured offset ending in binary 01 moved one step, which the
client would read as the flag). After our flagged INFO1a `modem/k56d`
runs the call: the training down (`modem/k56tx`: R, TRN1A, the client's
3-bit TRN1B, TRN1C, Js, then silence until the client's J, R, TRN2A, MARK,
TRN2B until its MP, our MP and MP', E; one scrambler across all of it),
the client's V.34 call-modem upstream (GPC) with its J carrying the pad
and the robbed symbols, its MP, then data: frames of 8 symbols, V.34's
shell mapper over K bits, a sign and q bits a symbol, rings and
magnitudes from the included tables of the rate and pad
(`modem/k56enc`, `modem/k56tab`), robbed symbols' last bit left 0. The
upstream is V.34 data at what our MP asked for (16 states, linear, minimum
shaping, as `p34d` in V.90). The client's renegotiation (its S in data
mode) and retrain (its tone B: V.34's retrain Phase 2, INFO1a flagged
again, then the whole training) are followed; we start neither
(`v90m_retrain` gives `V90M_ESTATE`), and the third retrain in a row ends
the call. There is no way back to V.34 after the flagged INFO1a: any
failure ends the call (`V90M_END_K56_FAILED`).

This is the stated exception to "written from the recommendations", as
x2's is: V.8bis and V.34 are ITU recommendations, but K56flex's own parts
come from third parties' reverse engineering (D-Modem's reading of
Motorola's SM56 driver), a published recording of a real call, and our
observations of SM56 under emulation, each fact tagged with its source in
`docs/history/k56-facts.md`; no code of D-Modem is here. The rings and
constellations are included data (`modem/k56tab.c`, generated by
`tools/gen_k56_tables.py` from D-Modem's published values; no rule
reproduces them). The design and its rulings are in
`docs/superpowers/specs/2026-11-21-k56flex-answering-design.md` and
`docs/history/PROGRESS.md` ("k56 1" to "k56 13"); what is left for the
owner's line tests is in `docs/history/k56-handover.md`.

Tested against a simulated client (`sim/k56call`, built from the facts),
D-Modem's encoder and training as oracles (every rate x pad x robbed-bit
mask, and the training segment by segment, equal), Motorola's SM56 decoder
on our octets (every combination decoded), and SM56 itself as the caller
under D-Modem's harness (outside the repository; `tools/k56-peer/`):
V.42 data both ways. No hardware Rockwell or Lucent client has called it.

### 4.2c V.90 and V.92, analogue (calling) side
`modem/orig` runs the call from the moment the far end has answered
(dialling and call progress are the owner's): V.8, then, when JM selects
PCM, `modem/v90a`: Phase 2 (`p2call`), Phases 3 and 4, data (the tick
and the calls in `v90a.c`; V.90's upstream in `v90a_up90.c`, V.92's in
`v90a_up92.c`, the downstream sequences in `v90a_down.c`, the time limits,
the watch and the retrain in `v90a_watch.c`). Downstream
is the PCM receiver (`v90arx`) with the DIL (`v90dil`) and the data
decoder (`v90adec`); upstream is V.34's transmitter (`v34mod`, the blocks
of the V.34 caller) in V.90, PCM in V.92 (below and 4.2d). One linear
sample in, one out, 8 kHz on our own clock, which the A/D and the D/A
share. Nothing in the receiver is specified by the Recommendation, only
the signals. Tried in simulation and against this library's own answerer
only; what it sends is checked field by field against real callers'
recordings (`test/test_realcheck.c`).

**V.8, originating** (`v8orig`, `anstone`):
- The answer tone detector brings 2100 Hz to baseband in blocks of 5 ms;
  a block is tone when its 2100 Hz component holds half of its power and is
  above -43 dBm0. The 15 Hz part of the envelope over windows of 200 ms
  (three periods) tells ANSam (above 0.12 of the mean) from ANS (below
  0.06); two windows that agree decide.
- CI, when asked for, is sent ON for 3 sequences and OFF for 1 s, and the
  tone is listened for only while CI is off, where our own echo is not in
  the way.
- ANS: no CM (V.8 7.2), the pre-V.8 ways of `modem/orig.h`. ANSam: Te (1 s)
  of silence, CM on V.21(L) until two identical JM on V.21(H), CJ at the
  next octet boundary, 75 ms. No tone in 10 s, or no JM in 5 s of CM, ends
  the call.
- The JM receiver reads idle mark while 2100 Hz is heard: it took part of
  the answer tone for a character and lost the first JM (V.8 8.1.2). CM
  carries the PSTN access category whenever it carries the PCM one, as
  every real V.90 / V.92 caller's CM does (V.8 6.3).

**Phase 2** (`p2call_init_v90`, V.90 9.2.2) is V.34's *answer* procedure
with V.90's INFO sequences, a second set of states in `p2call` sharing the
V.34 caller's helpers: INFO0a and Tone A; the digital modem's INFO0d and
Tone B; the reversals that measure the round trip (RTDEa); our L1 / L2
while the digital modem is silent; its L1 / L2, analysed as for V.34's
INFO1c; INFO1d; INFO1a (Table 10: UINFO 90, even so that a robbed bit
leaves its codeword alone; MD 22 x 35 ms; of 3000 and 3200 symbols a
second, the upstream rate INFO1d projects most for). V.92 (`cfg.v92`)
sets INFO0a bit 26 and, when INFO0d bit 27 and INFO1d bit 70 allow it,
sends INFO1a of Table 18 (PCM upstream: all four filter sections, the
longest filters, no MD; an MD there is counted in units of 276 symbols,
which the digital modem waited out). Phase 2 also measures our clock
against the network's from the digital modem's L2: each tone turns from
20 ms block to block by its frequency times the offset, and a weighted
least squares over the tones with 20 dB SNR gives the offset within 0.1
ppm on the test lines. Recovery 9.2.2.2.1 to .3; no INFO1d ends the call.

**The echo canceller** learns on MD (V.90): MD is manufacturer defined
and the digital modem is silent through it, so it is white noise at the
transmit level for 770 ms, the only wideband signal we send while the
downstream is silent (L1 / L2 is a comb of 25 lines and would leave the
canceller wrong between them); ERLE 62 to 73 dB at its end. V.92 learns on
TRN1u instead (4.2d). Its reference delay is `echo_delay`, fixed, not
measured (-1 is taken as 0).

**Phase 3** (9.3.2): S, S-bar, MD, S, S-bar, PP, TRN, then Ja with the DIL
descriptor until Sd-bar is heard; the receiver learns the clock and the
channel on Sd and TRN1d; Jd; S until Jd', S-bar; the DIL, measured while
we are silent; S, S-bar. Then Phase 4 (9.4.2): CPt until Ri-bar, SCR while
TRN2d shows the noise with CPt's constellations, CP, CP' once MP is in, E
once MP' or Ed is in; downstream Ri, Ri-bar, TRN2d, MP, MP', Ed, B1d.

**The PCM downstream receiver** (`v90arx`, integer only):

```
 A/D samples, our clock -> echo canceller (ec, our transmit as reference)
   -> fractional interpolator on to the network's 8 kHz clock (v34_interp)
   -> AGC, set once from Sd
   -> decision feedback equaliser: 48 forward taps (32 ahead of the cursor)
      on network-rate samples, 128 feedback taps on decided levels
   -> slicer: the nearest point of the constellation of the symbol's data
      frame interval -> Ucode, sign, error
```

- The clock is measured over Sd: Sd repeats every 6 symbols, so on our
  clock its 1333 Hz line turns by -2 pi times the offset from one period to
  the next (each period against the one 16 before); it is set before TRN1d
  begins. The far end is not trusted: the tangent is clamped to 1, the step
  to 1000 ppm, the slew to 500 ppm, and the interpolator makes at most 3
  outputs per input sample.
- TRN1d is a known binary signal. A 128-tap NLMS estimate of the channel
  runs on it for 1700 symbols, and the equaliser is computed from the
  estimate (the MMSE decision feedback equaliser, by a fixed-point Cholesky
  solution) rather than trained: LMS alone needed about 3 s to reach -45 dB
  on a line that passes little below 200 Hz and above 3.7 kHz.
- Then decision directed: the estimate goes on (its input is the symbol
  stream, white, so it follows every part of the band equally fast), the
  equaliser is computed from it again every 2048 symbols (1024 above 30
  ppm, 512 above 61 ppm) and LMS refines it in between. A second order loop
  steers the interpolator's step, with a slow phase slew, from how far the
  estimate has moved (the phase of its DFT at 1 and 2 kHz).
- Symbols whose error exceeds 4 sigma, and every symbol of an interval
  whose error stands 9 dB above the best interval's, are left out of the
  adaptation: a robbed bit or a pad moves points by a step, which no linear
  equaliser learns. Their levels come from the DIL.
- The band edge. Our A/D is not locked to the network, so what the line
  passes above about 3.8 kHz aliases with a phase that turns with the clock
  offset: a slowly changing channel at the band edge. An MMSE equaliser
  designed from an estimate 64 ms old amplified that band as if it stood
  still, and the receiver collapsed (0 km, 100 ppm). From Ri-bar on
  (`v90arx_edge()`) the design takes the image in as interference of
  unknown phase: a quarter of the power the estimate has at 3828, 3875,
  3922 and 3969 Hz. The DIL is measured with the plain equaliser.
- Behind a 6 dB pad UINFO 90 is a codeword a robbed bit changes, so in one
  interval in six TRN1d is not +-L; each interval's UINFO level is fitted
  from the estimate's residual partway through TRN1d (`ls_set`), and the
  estimate settles on that. The symbols the estimate is computed against
  are written also while the receiver is held, so it does not resume on
  stale ones.
- The equaliser's design is spread over ticks: a job of one step a symbol
  (start, fill of R, Cholesky by a budget of 9000 multiplies a step,
  solve, feedback taps), 8 symbols in all; the estimate goes on meanwhile.
  A tick in which the receiver took a design step takes no step of v90a's
  own jobs.

**The DIL** (`v90dil`, V.90 8.3.1, 8.4.1). We ask for every Ucode 0..119,
one 18-symbol segment each: UINFO three times positive, then 15 training
symbols, the first positive too. That opening is Ri's (UINFO + + + - -
-), so when the digital modem ends the DIL with Ri at a segment boundary
the fourth symbol tells the two apart while the decision feedback still
holds nothing wrong. The order is 0 2 4 ... 118 119 117 ... 3 1: what the
equaliser leaves of a segment at the largest level lies on the next
symbols, and with 119 before 0 it biased Ucodes 0 to 6 by up to 160 units,
enough for CP to take a false level for its smallest point. Ucodes 120..127
are left out: in runs of 15 they overload the receiver's 16-bit input, and
the constellations, which use only measured Ucodes, do not need them at
-12 dBm0. Four passes (at least 8 measurements per interval and Ucode,
about 1.1 s); the receiver is held through the first and learns on the
measured levels after it (held for the whole DIL, it lost 0 km, 100 ppm
lines).

**The DIL's analysis.** The equaliser output of every DIL symbol, sign
taken off, is summed per interval and Ucode. Each mean is the level that
Ucode arrives at, and a pad, a robbed bit and the codec's law all show in
it; but the DIL's repetitive patterns bias the means by up to 0.8 %, two to
six times the noise in data mode, so each is snapped to a Table 1 level
under one gain, where the codec's step there is 32 units or more. The gain
comes from the codeword UINFO arrives as (searched over 15 dB, refined by
least squares); the codeword of each mean is picked with its interval's
own gain (within 3 % of the common one: the intervals next to the REF
symbols are biased differently, and a common gain picked a neighbour now
and then under a pad), and the level written is the common gain's. Robbed
intervals (mu-law) are those departing from the median of the six at a
quarter or more of Ucodes 64..127. Without the measured levels a pad or a
robbed bit cost 9 to 16 kbit/s in simulation, and a 3 dB pad did not
connect.

**CPt and CP.** Sr = 0 (no spectral shaping asked for), ld = 0, one
constellation per interval. Points are picked from the bottom up among the
measured levels, each at least d = z sigma above the one before and the
first d / 2 from zero, sigma that interval's noise; for a given K the
moduli are the least whose product reaches 2^K, added where they cost least
power; z is then raised as far as the power limit allows (Table 15 less 0.1
dB, 8.5.2's weighted power rounded up). The highest K whose largest z
reaches 12 wins (2 Q(6) = 2e-9 for Gaussian noise); CPt wants 20, with K
at most 16. CP's sigma squared is twice what TRN2d shows, measured over
4000 symbols of SCR after Ri-bar: data mode shows 2 to 4 dB more error than
TRN2d with CPt's sparser points, and with TRN2d's figure alone an A-law
line ran with bit errors. The analysis (one gain candidate a step, one
interval a step) and the designs (one trial constellation set a step) are
jobs spread over some 60 ticks after the DIL; where a result is wanted
before its job is done (our S-bar before CPt, Ri-bar, our SCR's 2 s) the
rest runs then.

**Data mode.** `v90adec` undoes the digital modem's encoder (5.4
backwards: labels, R0, the bits; the signs for every Sr without the
shaper's filter, see its header). Rd is recognised as two frames of each
interval's largest point + + + - - - (or the inverse), and delivery runs
two frames late so that Rd is never handed on as data.

**V.92's PCM upstream.** The decision to use it is Phase 2's (above). In
Phases 3 and 4 the sequences are V.92's (the header of the V.92 block in
`v90a_up92.c` has the order), with SUVd, CPd and Ed taken out of the TRN2d
descrambler by `v92aseq` a data frame at a time: CPd may be some 20000
bits, so it is read word by word with the CRC running along and no bit
kept. CPd dictates the upstream: `v92/v92_enc`, the analogue transmitter as
a bit-exact model of what V.92 6.4 defines (scrambler, modulus encoder,
precoder filter and prefilter, inverse map, convolutional encoder), sends
of each equivalence class the member that minimises |x(n)| (what 8.8.3's
NOTE tells the digital modem to assume), keeps x(n) and v(n) with 8
fractional bits, and scales the prefilter output by CPd's gain to LU, the
configured level. The symbols are made on the network clock as the
receiver keeps it and carried on to our D/A clock by `pcmtx`; why, and the
echo canceller's training on TRN1u, are in 4.2d. Because the upstream runs
on the receiver's clock, V.92 holds the receiver's timing loop from the DIL
to Ri-bar (`v90arx_timing_hold()`; the clock came from L2): the DIL's
patterns, on levels still being measured, walked it by a hundredth of a
sample, and our symbols moved while the digital modem measured them for
CPd. V.90 leaves the loop free: its clock comes from Sd (up to 1.6 ppm off)
and nothing follows the loop.

**Retrains** (V.90 9.5, V.92 9.7), either end, from Phase 3 on:
- The digital modem's is 70 ms of silence and Tone B: more than 50 ms of
  Tone B, seen by `modem/tonedet` on what the echo canceller leaves. Ours
  (`v90m_retrain`) is 70 ms of silence and Tone A.
- Either way: 70 ms of silence from us, then Phase 2 again without INFO0
  (`p2call_init_v90_retrain`), the first INFO0d kept. 9.7/V.92 says the
  modems "proceed in accordance with the full Phase 2 start-up procedure":
  that is 9.3's Phase 2 with probing, not the short one, entered at the
  tones and their reversals; nothing in 9.7 sends INFO0. This library's
  answerer sends none, and in 35 recorded calls with a retrain no real
  caller sent INFO0a. A digital modem that sends INFO0d all the same is
  answered with INFO0a (tried against our own answerer made to do so).
- Then Phases 3 and 4 with everything fresh, `v90a` re-initialised in
  place (receiver, DIL, canceller, V.92's `pcmtx`) but the call's counts,
  the clock and the first INFO0d. A rate renegotiation without Ed (9.6.2)
  and a Phase 3 or 4 that fails once the call has carried data are retrains
  too; a retrain's Phase 2 that fails ends the call.
- The limit is the answering side's: 3 retrains without a minute of data
  mode between them, one forgotten per minute. Past it a retrain the far end
  asks for, or one that fails, ends the call, and one our receiver needs is
  a cleardown (CP / CPu with drn = 0): `V90M_END_RETRAIN_FAILED`.
- In simulation a retrain took 7.4 to 7.6 s from the event to data both
  ways in V.90 and 8.3 to 10.6 s in V.92.

**Rate renegotiation.** The digital modem's is followed (V.90: S, S-bar,
CP as Phase 4 from CP on; V.92: Ru on our data frame boundary after its
Rd-bar). Ours (V.90 9.6.2.1: S, S-bar, SCR until the digital modem's TRN2d
has shown 4000 symbols of noise, then a CP designed on it; V.92 9.8.2.1:
Ru, Ru-bar, TRN2u, SUVu, a CPu designed the same way) is begun by the
receiver's watch (there is no public call for it), never with the request
for silence (our canceller needs no new training). V.92's Phase 4 flags are cleared at its start (left set,
they sent E2u at once and the answerer failed B1u).

**The receiver's watch**, once a data frame: the sum over the six
intervals of the receiver's error against the noise CP was designed for.
On the test lines data mode shows 0.20 to 0.97 of it. 6 dB over is already
too late (the decision feedback is going: at noise +15 dB the ratio sat at
4 with no error for 2.5 s, then an avalanche), so:
- 3 dB over for 1 s: a renegotiation down when only the noise rose; a
  retrain when the equaliser's input power moved 3 dB (a level or pad
  change: the DIL no longer holds) or a renegotiation for the line ended
  within 5 s;
- 12 dB over for 250 ms (the receiver lost): a retrain; meanwhile V.92's
  timing loop is held (our PCM upstream runs on it, and a lost receiver
  dragged it) and Rd is not believed; after a collapse the loop stays held
  until the error is back within twice the design's;
- 6 dB under for 5 s after the watch took the rate down: a renegotiation
  back up, once.
On those test lines, 72 calls, it began neither a retrain nor a
renegotiation. Its thresholds come from the simulator's lines only.

**Cost** (qemu-counted ARMv5 instructions, README "Memory and CPU"): data
mode 54.6 / 58.8 M a second for V.90 at 0 / 100 ppm, 69.4 / 73.9 M for
V.92, the equaliser and its adaptation the largest part; the heaviest tick
98 k, once the equaliser design, the DIL's analysis and the CPt / CP
designs were spread over ticks (the DIL's end alone was 9.3 M in one
sample before).

### 4.2d V.92, analogue (calling) side: the PCM upstream
`modem/v90a` with `v92` on (`ORIG_MODE_V92`, `V90M_MOD_V92` calling): Phase
2 says V.92 in INFO0a and selects PCM upstream in INFO1a (Table 18: all four
filter sections, the longest filters, no MD) when INFO0d and INFO1d allow
it; otherwise the call is V.90. Downstream is V.90's receiver (`v90arx`,
DIL, CP) with V.92's Jd / Jp / Jp' and SUVd / CPd / Ed (`modem/v92aseq`,
word by word: CPd may be 20000 bits). Upstream: `v92/v92_enc` with what
CPd dictates, at LU = the configured level.
- The symbol clock is the network's (6.2/V.92). The transmitter
  (`modem/pcmtx`) runs the downstream receiver's interpolator accumulator
  with its increments, so it follows the receiver's timing loop sample for
  sample, and carries the symbols on to our D/A clock through a 64-tap
  bank that is also a low-pass at 3.7 kHz: the digital modem measures the
  channel once and does not follow it, so what reaches its codec must be
  one fixed response of our symbols however the two clocks stand, and near
  4 kHz no interpolator of that length is right.
- The clock is known before the first symbol: from the digital modem's L2
  in Phase 2 (each tone turns from block to block by its frequency times
  the offset; within 0.1 ppm on the test lines), and Sd's own estimate
  (off by up to 1.6 ppm) only overrules it beyond 5 ppm. A clock found at
  Sd and corrected afterwards moved our symbols against the network while
  the digital modem trained on them (failed starts at 100 ppm, a bit error
  every few seconds elsewhere).
- The echo canceller learns on the first TRN1u (the digital modem is
  silent), sent through the receiver's full band bank: the hybrid's echo is
  strongest at the band edge, where the low-pass has little left to learn
  on. Our own Ru has Sd's period, so Sd is not looked for until Ja.
- Rate renegotiation begun by the digital modem is followed (Ru on our data
  frame boundary after its Rd-bar); cleardown either way (CPu / CPd with
  drn = 0). The digital modem's cleardown is acknowledged before we go
  silent: SUVu' / CPu' (9.6.2.1.2) until its SUVd' or CPd' (9.6.2.1.4's
  point; no E2u, which leads to data), at most 100 ms + a round trip after
  its CPd. Retrains and rate renegotiations of our own are V.90's (the
  analogue side's, 4.2c; `docs/history/PROGRESS.md` robust 2c), with a
  fresh PCM upstream after a retrain. No quick connect, no short Phase 2, no modem-on-hold. Tried in
  simulation and against this library's own answerer only.

### 4.2e Bell 103 and Bell 212A, calling side
V.8 cannot offer them (`ORIG_MODE_BELL103` / `ORIG_MODE_BELL212A`,
`V90M_MOD_BELL103` / `V90M_MOD_BELL212A` calling; off by default), so they
are what a caller does when no V.8 comes about. The Bell answer tone is
2225 Hz within some 10 Hz (`modem/belltone`: 40 ms blocks, in which 2250 Hz,
a V.22 answerer's unscrambled ones, turns once and sums to nothing; 2100 Hz
is far off). The order, `modem/orig.h` in full:
1. ANSam and a V.8 mode on offer: V.8, as ever (the answerer chooses).
2. 2225 Hz before any 2100 Hz tone is told (a pre-V.8 Bell answerer), or
   after ANS while the legacy answerer is awaited: Bell 212A if offered,
   else Bell 103.
3. Only Bell on offer: no CM after ANSam (and no CI); silent, listening:
   2225 Hz -> 212A (else 103); with 103 alone, 150 ms of a carrier in the
   Bell 103 answer channel that is not 2100 Hz (a V.22 answerer's
   unscrambled ones are one, as for the plain modems of the time) -> 103;
   with 212A as well, 6 s of it without 2225 Hz -> 103 (this library's
   answerer plays 2225 Hz after 4 s of its unscrambled ones when 212A is
   on).
4. A 212A start answered by no 1200 bit/s within 3 s -> 103, if offered.

Bell 212A is the V.22 caller at 1200 bit/s, its scrambled ones from the
moment the tone is told (there are no unscrambled ones to time V.22's 456 ms
from), data 765 ms after the answerer's scrambled ones; no 2400, no
retrain. Bell 103 is the V.21 path on 1270 / 1070 Hz out and 2225 / 2025
Hz in, behind `bell103h_bpf.h` (`tools/gen_v21_bpf.py bell103h`: our own
tones 57 dB down). The Bell 212A handshake document was not to hand: the
timing follows V.22 and what this library's answerer (and the 5686 that
called it) accepts. Tried in simulation and against this library's own
answerer only; a V.21-only answerer with Bell 103 on is not reached (its
1650 Hz mark is 57 dB down in our Bell receiver).

### 4.3 Fixed-point conventions
- Samples are 16-bit linear; 0 dBm0 is an RMS of 15770.
- Phases are 32-bit accumulators; `sintab` has 1024 entries, Q15.
- Equaliser input is normalised to an RMS of 2048; taps are Q20 in 32 bits;
  a 4-point training symbol is (+-4096, +-4096) at the equaliser output.
- Filter tables are generated by `tools/gen_*.py` into headers; nothing is
  computed with floating point at run time.
- No signed overflow and no left shift of a negative value (undefined in
  C99; `make check-ubsan` traps them). Two implementation-defined
  behaviours are assumed: a right shift of a negative value is
  arithmetic (it rounds toward minus infinity), and a conversion to a
  signed type of a value out of its range wraps modulo 2^N (two's
  complement). Every compiler and target in the port matrix behaves so,
  and the matrix's identical output is the check; a target that did not
  would compute differently.
- Clocks and counters that can run past 2^31 samples or symbols in a long
  call wrap as unsigned values or stop at their limit (`modem/clock.h`).
- A compiler builtin or attribute only through `modem/compiler.h`
  (`V90M_NOINLINE`, `V90M_CLZ32`), each behind a C99 fallback with the
  same result; `-DV90M_NO_BUILTINS` takes the fallbacks everywhere (the
  tests build both). `V90M_CLZ32`'s builtin form assumes an `unsigned int`
  of at least 32 bits, true on every target named here.

## 5. Testing

- Every block has a host test (`make check`); the same tests run cross-built
  under qemu (`make check-arm`).
- Peers written here (`sim/`): channel with loss, delay, echo, noise, clock and
  carrier offset; calling modems for V.8 / V.22bis, V.32bis, V.34, V.90 Phase 2
  and Phases 3/4 (analogue side), V.92; QAM modulator; `dschan` (the PCM
  downstream as an analogue modem's A/D has it) and `test/v90link.h` (a whole
  line between the library's analogue and digital modems). They may use knowledge a
  real modem does not have; they are a test reference, not code to ship.
- Independent peers (`make test-interop`, host only): spandsp for V.8 and
  V.22bis, both roles; V.42 and V.42bis (`v42/`).
- Lessons that shaped the tests: a wrong-sign timing loop once passed every
  bit-error test because the equaliser hid it, and a delay line that moved
  under the equaliser update only showed for a third of the sub-sample
  alignments. So the tests check loop and equaliser state (tap centre, loop
  frequency, convergence across alignments), not just error counts.
- Real modems: recordings of real calls replayed with `tools/v90m_replay.c`;
  a fault found that way comes with its recording as a fixture. The
  calling side has no real modem to call yet; what it sends is checked
  field by field against what real callers sent in those recordings
  (`tools/v90m_fields.c`, `test/test_realcheck.c`).
- `make check-ubsan`: the host suite with undefined behaviour trapped.
- `make verify`, per commit: every test program's output against its
  golden file, the 106 recorded calls' replays against their manifest,
  the benches' hashes, UBSan, and the armv7hf and big-endian short suites
  against the host; a refactor or a faster form of the same arithmetic
  must change none of them (`docs/testing.md`).
