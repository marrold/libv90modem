# v90d/v92: V.92 PCM upstream (stages 1 to 3)

The digital modem (server) side of ITU-T V.92 with PCM upstream.

Stage 1: a bit-exact model of the analogue modem's data mode transmitter
(6.4), the matching decoder for the digital modem, and builders and parsers
for the V.92 sequences of a full start-up.

Stage 2 (see "Stage 2" at the end): the receiver for the analogue modem's
training signals, the design of the CPd parameters from the measured
channel, and `v92_p34d`, the Phase 3 / Phase 4 / data mode state machine. A
complete V.92 call with PCM upstream runs in simulation against a simulated
analogue modem (`../sim/v92a`, `../sim/v92chan`).

Stage 3 (see "Stage 3" at the end): `../modem/answer.c` offers V.92 in
Phase 2 and runs `v92_p34d` for a caller that selects PCM upstream; dce, srv
and cfg know the modulation, and there is a setting (`v92`, default off).

Written from ITU-T V.92 (11/2000), V.90 (09/98) and V.34 (02/98) only
(`../refs/itu/`, text files, and the PDFs where the text extraction loses
overbars, tables or figures). No other modem implementation was opened or
consulted. Stage 4 (at the end) is what one real V.92 modem did on the
line, and what was changed for it; for everything else see "Not verified"
below.

Portable C99, integer only, no allocation, no globals (every object is a
caller-owned struct). Floating point appears in the tests only.

## Build and test

    make test        host tests
    make test-arm    the same tests cross-built for the ARM926, under qemu-arm-static
    make cost-arm    ARM instructions per upstream symbol (slow, about a minute)
    make clean

Last run (x86-64 and ARM, identical check counts):

    test_mod: 3299 checks, 0 failures
    test_map: 1448 checks, 0 failures
    test_enc: 514 checks, 0 failures
    test_dec: 368 checks, 0 failures
    test_seq: 528 checks, 0 failures
    test_info: 201 checks, 0 failures
    test_sig: 258 checks, 0 failures
    test_e2e: 258 checks, 0 failures
    rx: 25461 checks, 0 failures
    design: 160 checks, 0 failures

and in the top directory (`make test`, `make test-arm` there):

    p2_v92: 103 checks, 0 failures
    v92call: 653 checks, 0 failures

and in `../v90`: `tx92: 688 checks, 0 failures`.

The tests compare against values worked out by hand from the Recommendation
(the derivations are in the comments next to the expected values), and only
after that against round trips. The CRC of every sequence is checked with a
cell-by-cell model of Figure 14/V.34 kept in the test files.

## Files

| file | contents | source |
|---|---|---|
| `v92.h` | framing constants, error codes, `v92_map_t` (K, trellis, M0..M11, constellation sets), `v92_filt_t` (precoder and prefilter), linear scale convention | Figure 1, 6.4.2, Table 30 |
| `v92_map.c` | parameter checks, index to Ki | 6.4.1, 6.4.2, 8.8.3 |
| `v92_mod.h/.c` | modulus encoder with differential sign, and its inverse | 6.4.1 |
| `v92_enc.h/.c` | analogue modem data mode transmitter model: scrambler, modulus encoder, equivalence classes, precoder, prefilter, gain, inverse map, convolutional encoder; RM / RM' patterns | 6.3, 6.4, 8.7.1, 8.7.4 |
| `v92_dec.h/.c` | digital modem decoder: per-class slicer, Viterbi over the V.34 codes, Ki recovery, modulus decoder, descrambler | (not specified by V.92; inverse of 6.4) |
| `v92_seq.h/.c` | Ja, Jd, Jp, Jp', CPt, CPu, CPus, SUVu, SUVd, CPd, E helpers, frame sync search | 8.5 to 8.8, Tables 20 to 24, 27, 30, 31 |
| `v92_info.h/.c` | INFO0d, INFO0a, INFO1d, INFO1a (three forms), MH sequences, QC1a / QCA1a / QC1d / QCA1d | 8.2, 8.3, 8.4.1, 8.9.2, Tables 2, 4, 11, 13, 15 to 19, 32, 33 |
| `v92_sig.h/.c` | symbol mappings of the training signals: TRN1u, Ja / CPt / E1u, SCR, Jd / Jp / Jp', Ru, Su, TRN2u and the sequences sent in its modulation | 8.5.1 to 8.5.7, 8.6.2 to 8.6.6, 8.7.2, 8.7.5, 8.7.6, Tables 28, 29 |
| `v92_rx.h/.c` | Ru / Su detector with reversals; training receiver: channel estimator, decision feedback equaliser for the 2 and 4 level training signals | (not specified) |
| `v92_design.h/.c` | design of CPd: prefilter, precoder, gain, constellation, moduli, rate | 6.4, 8.8.3 (method not specified) |
| `v92_p34d.h/.c` | Phases 3 and 4, data mode, rate renegotiation, cleardown, retrain entry | 9.5.1, 9.6.1, 9.7.1, 9.8.1, 9.11 |
| `test/` | one test per module, `test_e2e.c` (CPd, encoder with filters, channel, mu-law quantiser, decoder, noise), `testutil.h` | |

Code reused from the tree, linked but not modified: `../v90/v90_scr.c`
(clause 7/V.34 scrambler), `../v90/v90_seq.c` (CRC register, `v90_dil_t`,
`v90_cp_t`), `../v34/v34_core.c` with `../v34/v34_constel.c` (Figure 9,
Table 13 and the convolutional encoders of V.34 through `v34_priv.h`). Tests
also use `../v90/v90_pcm.c` with its table `../v90/v90_pcm_tab.c` (generated
by `../tools/gen_pcm_lin.py`) and `../test/test.h`.

## Conventions

* **Framing** (Figure 1): 8000 symbols/s; data frame 12 symbols (i), 
  constellation frame 6 (j = i mod 6), trellis frame 4 (k = i mod 4).
* **Rates**: CPd drn = 1..19 means (drn + 17) * 8000 / 6 bit/s (24000 to
  48000) and K = 2 * (drn + 17) bits per data frame. `V92_UP_K`, `V92_UP_BPS`.
* **Linear scale**: constellation points, u(n) and the decoder input are
  plain integers on the 16-bit linear scale of Table 1/V.90
  (`v90_ucode_linear`: mu-law up to 32124, A-law up to 32256).
* **Constellation set**: LC positive points, smallest first; the set has
  N = 2 * LC points a(eta), -LC <= eta < LC, a(eta) = pt[eta] for eta >= 0,
  a(eta) = -pt[-eta - 1] for eta < 0.
* **Coefficients** (`v92_filt_t.coef`, the order CPd carries them):
  z1(1..LZ1) signed Q0.15, p1(1..LP1) signed Q1.14, z2(0..LZ2-1) signed
  Q0.15, p2(1..LP2) signed Q1.14. `gain4` is 4G in unsigned Q0.16, so
  G = gain4 / 2^18.
* **Encoder internals**: x(n) and v(n) carry 8 fractional bits (`V92_XQ`);
  the encoder returns G * v(n) in Q14 where 16384 is the level at which the
  mean square is the "1" of 8.8.3.
* **Bit arrays**: one bit per `uint8_t`, index 0 first in time and equal to
  "bit 0" of the tables, integer fields LSB first, builders return the
  length or a negative `V92_ERR_` code, parsers return the bits consumed or
  a negative code (the conventions of `../v90/v90_seq.h`).
* **Fill**: sequences that end with "fill to the next multiple of 6 (12)
  symbols" take a `unit` argument, the number of bits in that many symbols
  of the modulation in use; `unit` = 1 stops after the one mandatory fill
  bit. Parsers take the fill that is there and tolerate a short one.
* **Line bits of two-point signals**: upstream 0 = +LU, 1 = -LU (8.5.7);
  downstream 1 = positive, 0 = negative (8.6.2). `V92_UP_SIGN`, `V92_DN_SIGN`.

## Decoder

Not specified by the Recommendation. Input: one value per symbol on the
linear scale, expected to be u(n) plus noise (the precoder and prefilter the
digital modem asked for in CPd make the channel deliver u(n) at the network
sampling instants). Per symbol it finds the nearest point in each of the
four residue classes of the index modulo 4 (the 8-way partition of
Figure 9/V.34 of the point (2 y(0) + 1, 2 y(1) + 1) depends on the indices
modulo 4 only). Per trellis frame it runs one Viterbi step over the V.34
code named in CPd (16, 32 or 64 states), with the parity of the index sum
as the coded bit Y0 (the k = 3 rule of 6.4.2 makes that parity equal Y0).
`V92_VIT_DEPTH` (24) trellis frames later the decided indices go through
`v92_eta_to_k()`, the modulus decoder and the GPA descrambler. Delay:
`V92_DEC_DELAY` = 104 symbols. An inverted line is absorbed by the
differential sign of 6.4.1 (tested for all three codes).

Cost, ARM instruction counts from qemu (`make cost-arm`), 64- and 48-point
constellations, 36000 bit/s, all four filter sections in the encoder:

    trellis 0 (16 states): encoder model 820, decoder 1649 instructions per symbol
    trellis 1 (32 states): encoder model 819, decoder 2494 instructions per symbol
    trellis 2 (64 states): encoder model 823, decoder 5528 instructions per symbol

At 8000 symbols/s that is 13, 20 and 44 million instructions per second.
These are instruction counts, not cycle counts on the ARM926.

Sizes on ARM: `v92_dec_t` 6420 bytes, `v92_enc_t` 3308, `v92_map_t` 1562,
`v92_filt_t` 778, `v92_cpd_t` 2346. Code: about 25 KB for all seven objects.

`test_e2e` with AWGN of standard deviation sigma in front of a mu-law
quantiser, points at least 200 (260) linear units apart, bit errors after
the descrambler out of 216000 bits:

    sigma 40: 16 states 5386 hard / 54 trellis; 32 states 5330 / 0; 64 states 5620 / 0
    sigma 55: 16 states 16843 hard / 854 trellis; 32 states 17585 / 305; 64 states 17561 / 202

## Sequence and signal inventory

Everything clause 8 of V.92 defines, who sends it, and what exists here.
"done" means build and parse with tests against the table; "mapping" means
the symbol level rule is in `v92_sig`, without any signal processing.

| signal | clause | sent by | status |
|---|---|---|---|
| QC1a, QCA1a | 8.2.1, 8.2.3 | analogue | done (`v92_qc_*`) |
| QC2a, QCA2a | 8.2.2, 8.2.4 | analogue | not done (V.8 bis identification fields, Tables 3 and 5) |
| TONEq | 8.2.5 | analogue | not done (980 Hz tone) |
| ANSpcm | 8.3.1 | digital | not done (Tables 6 to 10 are 301 PCM codewords per level) |
| QC1d, QCA1d | 8.3.2, 8.3.4 | digital | done (`v92_qc_*`) |
| QC2d, QCA2d | 8.3.3, 8.3.5 | digital | not done (V.8 bis, Tables 12 and 14) |
| QTS, QTS-bar | 8.3.6 | digital | not done |
| INFO0d | 8.4.1, Table 15 | digital | done |
| INFO0a | 8.4.1, Table 16 | analogue | done |
| INFO1d | 8.4.1, Table 17 | digital | done |
| INFO1a, PCM upstream | 8.4.1, Table 18 | analogue | done |
| INFO1a, V.34 upstream | Table 19, Table 10/V.90 | analogue | done (one form, they differ in bit 33) |
| INFO1a, V.34 | Table 11/V.90 | analogue | done |
| Tones A and B, L1, L2, INFOMARKS | 8.4, V.90 | both | not here (V.90 Phase 2, `../modem`) |
| CPt | 8.5.1, Table 23 | analogue | done (`v92_cpu_*`, type 0) |
| E1u | 8.5.2 | analogue | mapping, `v92_e_match` |
| MD | 8.5.3 | analogue | not done (manufacturer defined, only its length matters) |
| Ja (DIL descriptor) | 8.5.4, Table 20 | analogue | done |
| Ru, Ru-bar | 8.5.5 | analogue | mapping (`v92_ru_sbit`) |
| Su, Su-bar | 8.5.6 | analogue | mapping (`v92_su_level`) |
| TRN1u | 8.5.7 | analogue | mapping (`v92_sgn_*`, GPA) |
| DIL, Sd, Sd-bar, TRN1d, Ri, Ri-bar | 8.6.1, 8.6.5, 8.6.7, 8.6.8 | digital | not here (as V.90, `../v90/v90_tx`) |
| Jd | 8.6.2, Table 21 | digital | done |
| Jp | 8.6.3, Table 22 | digital | done |
| Jp' | 8.6.4 | digital | 12 zeros (`V92_JPP_BITS`), mapping |
| SCR | 8.6.6 | digital | mapping (`v92_sgn_tx_trn`, GPC) |
| B1u | 8.7.1 | analogue | `v92_enc` fed with ones (`V92_B1U_FRAMES`) |
| E2u | 8.7.2 | analogue | mapping (`v92_ml_*`), `V92_E2U_BITS`, `v92_e_match` |
| CPu | 8.7.3, Table 23 | analogue | done (`v92_cpu_*`, type 1; `v92_cpu_to_v90`) |
| CPus | 8.7.3, Table 24 | analogue | done |
| RM, RM' | 8.7.4, Tables 25, 26 | analogue | `v92_rm_k` + `v92_enc_force_k` |
| SUVu | 8.7.5, Table 27 | analogue | done |
| TRN2u | 8.7.6, Tables 28, 29 | analogue | mapping (`v92_ml_*`, `v92_trn2u_level`, `v92_trn2u_slice`) |
| FB1u | 8.7.7 | analogue | not done (fast parameter exchange only) |
| B1d, TRN2d, Rd, Rt | 8.8.1, 8.8.4, 8.8.6 | digital | not here (as V.90) |
| Ed | 8.8.2 | digital | not done as a signal: 2 data frames of zeros through the TRN2d mapping of `../v90` |
| CPd | 8.8.3, Table 30 | digital | done as bits; sending it needs the TRN2d mapping of `../v90` |
| Rf, Rf-bar | 8.8.4 | digital | not done (rate renegotiation, fast parameter exchange) |
| SUVd | 8.8.5, Table 31 | digital | done as bits |
| RT | 8.9.1 | both | not done (Tone A or B) |
| MH sequences | 8.9.2, Tables 32, 33 | both | done |

Procedures (clause 9): none. Short Phase 1 and 2, rate renegotiation, fast
parameter exchange, modem-on-hold and cleardown have their sequences above
where listed as done, nothing else.

## Readings and choices

Where the text leaves something open, is unclear, or had to be completed.

**Data mode (clause 6)**

1. 6.4.2, which u(n). The Recommendation only says the precoder "selects a
   point u(n) from the equivalence class". The NOTE of 8.8.3 tells the
   digital modem to assume minimum precoder output power symbol by symbol.
   `v92_enc` picks the member that minimises |x(n)|, a tie going to the
   higher index; `v92_enc_set_pick()` replaces the rule. The decoder accepts
   any member. (Changed in stage 4. It was the lower index; the CX93001,
   given p1 = 0 and M = 9, sent a(4) where a(-5) is as small, every time.)
2. 6.4.2, arithmetic. Word lengths, rounding and saturation of x(n) and
   v(n) are not specified. Here: 8 fractional bits, each sum rounded to
   nearest (half away from zero) once, saturation at 32 bits.
3. 6.4.2, shape of a set. Table 30 lists "positive points" only; the set is
   taken as symmetric about zero, which is what "negative points have
   negative indices, and positive points have non-negative indices" and
   N = 2 * LC imply.
4. 6.4.2, empty classes. Nothing forbids Mi > N (or 2 Mi > N where k = 3),
   which would leave a class without a point. `v92_map_check()` rejects it.
5. 6.4.1. Mi = 0 is rejected, Mi = 1 is accepted (the interval carries no
   data). The decoder recovers d(f) from R0 alone: d(f) = [R0 > (M-1)/2],
   except that for odd M and R0 = (M-1)/2 it keeps the previous value. This
   follows from steps 2 to 4 and is what makes the decoded bits independent
   of the line polarity; it is derived, not quoted.
6. 6.4.3, coordinate order. "The odd-integer coordinates used in
   9.6.3.1/V.34 are calculated as 2 y(k) + 1": the 2D points are taken as
   (2 y(0) + 1, 2 y(1) + 1) and (2 y(2) + 1, 2 y(3) + 1), first value = the
   real (horizontal) coordinate of Figure 9/V.34, first pair = the first 2D
   symbol of the 4D symbol. With that reading the k = 3 parity rule of 6.4.2
   is exactly the constraint of the V.34 4D code (LSB of the Figure 9 label
   = parity of the two indices), which the tests check; the order of the two
   coordinates inside a pair is not confirmed by that.
7. 6.4.4, "the 2T delays are replaced by 4T delays": the V.34 encoder is
   clocked once per trellis frame; Y0 of a trellis frame depends on the
   frames before it only.
8. The 64-state encoder (Figure 12/V.34) is used as `../v34` implements it.
   Figures 10 and 11 were compared with the PDF by hand for this work,
   Figure 12 was not.

**CPd (8.8.3, Table 30)**

9. Unit of the "linear value" of a constellation point: not stated. Taken
   as the 16-bit linear scale of Table 1/V.90. `v92_map_check()` wants
   values of 1..32767; the builder and parser pass any non-zero 16-bit value.
10. "The number of points in a constellation set shall not exceed 128":
    read as LC <= 128 positive points (the LC fields are 8 bits wide and a
    mu-law codec has 128 positive levels). If N <= 128 was meant, this
    code accepts more than it should; a cautious sender keeps LC <= 64.
11. "All of the constellation sets with non-zero size shall be listed
    first" and "Constellations shall not contain the zero point" are
    enforced by the builder; the first set must not be empty (LC1 is the
    only one without "possibly zero").
12. Lengths. Each of LZ1, LP1, LZ2, LP2 is limited to 320 and their sum to
    384 (the largest Lmax and Ltot of INFO1a) by builder and parser;
    `v92_filt_allowed()` checks a filter against what a given INFO1a
    announced, including which sections the analogue modem supports.
    LZ2 = 0 (no prefilter output) is refused by `v92_filt_check()`; LP1 = 0
    is accepted although the table lacks an "(if LP1 > 0)".
13. Absent parts are removed and the later parts move up (stated in 8.8.3);
    the gain word 35:50 is always present.
14. Output level. 8.8.3: when G * v(n) "has a mean-square value of 1, the
    analogue modem will transmit at the desired power". How 1.0 relates to
    the linear scale is the analogue modem's business; the encoder model
    returns Q14.

**Other sequences**

15. Table 20 (Ja): "N/2" is the ceil(N/2) of Table 12/V.90, and "0 : 187 +
    beta + ..." includes the start bit at 187 + beta + ceil(N/2) * 17. With
    N = 0 and one word each for SP and TP this gives the 276 bits 8.5.4
    states.
16. Table 23: bit "289 + delta" is printed both as the fill bit and as the
    start of the further fill; taken as one mandatory fill bit at
    289 + delta, more from 290 + delta.
17. Fill "to the next multiple of 12 (6) symbols" is counted from bit 0 of
    the sequence, which assumes the sequence starts on a symbol boundary and
    the stream of sequences on a frame boundary.
18. Kind of a sequence: bit 18 separates CP from SUV, bits 19:20 CPt, CPu
    and CPus, bit 47 Jd from Jp. Parsers judge the kind before the CRC and
    return `V92_ERR_KIND`.
19. 8.5.1: "24 differentially encoded binary ones shall be transmitted prior
    to transmitting the first CPt". Whether they pass the scrambler is not
    said (for Ja the 24 ones are part of the scrambled sequence).
    `v92_sgn_tx_bit()` scrambles everything; a receiver loses at most the
    first CPt if the peer does not. (Stage 4: the CX93001 sends no 24 ones
    in front of Ja at all, see there.)
20. 8.5.1, 8.5.4: the scrambler is not re-initialised between TRN1u and Ja
    or CPt (only "prior to the transmission of TRN1u"); assumed to run on.
    The receiver is self-synchronising either way.
21. 8.7.6, bit order in a TRN2u symbol. Tables 28 and 29 give "MSB:LSB"
    patterns; clause 8 states the bit order for other tables only. Default:
    first bit in time = LSB (so the last bit of the group is the sign);
    `v92_ml_set_msb_first()` selects the other reading. Only the sign bit
    is differentially encoded.
22. 8.7.4: Table 25 prints "u11 = 0" for interval 11 of RM and Table 26
    "k11 = M11 - 1"; read as K11 = 0 and K11 = M11 - 1.
23. 8.7.5, SUVu bits 27:31: signed Q2.2, "the value 16 (-4.00)" = no
    measurement, so levels run from -3.75 to +3.75 dB. `level` is in quarter
    dB, `V92_SUVU_NO_LEVEL` = -16.
24. 8.5.6 says Su and Su-bar "shall be an integer multiple of 12 symbols",
    9.5.2.1.7 and 9.5.2.1.8 send Su-bar for 24.5T and (24 + fraction)T.
    The procedure text is taken as right.
25. Table 22 (Jp): the 16-bit fraction is taken as frac / 65536 of a symbol
    by which the analogue modem lengthens Su-bar, that is, delays everything
    that follows.
26. 9.11 says cleardown is signalled by "setting drn to 0 in either SUVu
    ... or SUVd", but neither has a drn field (Tables 27 and 31); CPu, CPus
    and CPd have. Only those carry drn here.

**INFO, MH, QC**

27. Tables 15 and 16 put the V.92 capability bit and the short Phase 2
    request in swapped positions (INFO0d: 26 = short Phase 2, 27 = V.92;
    INFO0a: 26 = V.92, 27 = short Phase 2). 9.3 confirms "bit 27 of INFO0d
    and bit 26 of INFO0a". Implemented as printed.
28. INFO1a forms are told apart by bits 34:36 and 37:39: 6 and 6 = PCM
    upstream (Table 18), 0..5 and 6 = V.34 upstream (Table 10/V.90, and
    Table 19/V.92 which only adds the carrier bit 33 for short Phase 2),
    37:39 = 0..5 = V.34. A code of 7, or 34:36 = 6 without 37:39 = 6, is
    `V92_ERR_RANGE`.
29. The CRC of INFO and MH frames covers the bits after the frame sync, the
    reading `../modem/info.c` already uses for V.90.
30. MH signal and information patterns, the T1 codes and the WXYZ / LM
    codes of the QC signals are kept as printed (left-most bit = most
    significant = first in time, clause 8).
31. QC parser policy: both copies of the 10-bit frame must be well formed
    and equal.

## Not verified

* Until stage 4 there was no interoperability test of any kind; everything
  was checked against the text and against itself. One real modem has now
  been through start-up, data mode and a rate renegotiation (stage 4).
* Of the readings that only a real V.92 analogue modem can confirm, items
  6, 8, 9, 21 and 35 held with that modem (B1u and data decode without
  error, at the level designed for). Item 25 (Jp) and item 10 remain
  unconfirmed.
* The decoder has seen simulated channels only. Stage 1: white noise, a
  mu-law nearest-level quantiser, an inverted line. Stage 2 adds A-law,
  line filtering with residual intersymbol interference, our own echo, a
  digital pad. Still none: robbed-bit signalling, timing error.
* The encoder model's choice of u(n) is one of many a real modem may make;
  the decoder does not depend on it, but the transmit power and the
  precoder stability a digital modem designs for do.
* Instruction counts come from qemu, not from the target.
* Fast parameter exchange, modem-on-hold and the short phases are not
  exercised beyond their sequences (rate renegotiation: see Stage 2).

## Start-up with PCM upstream, as the digital modem sees it

The digest the next stage works from. Clause numbers are V.92 unless noted.

**Phase 1** (9.1): as V.90. V.8 cannot signal V.92 on its own.

**Phase 2** (9.3): the V.90 procedure with other INFO bits.

1. Send INFO0d with bit 27 = 1 (Table 15). If INFO0a has bit 26 = 1
   (Table 16), both sides are V.92 and INFO1d takes the Table 17 form.
2. In INFO1d set bit 70 = 1 if the channel supports PCM upstream, 0 to
   force V.34 upstream. The 3429 probing result shrinks to 8 bits (71:78).
3. Receive INFO1a and look at bits 34:36 and 37:39 (`v92_info1a_parse`):
   * 6 / 6: PCM upstream. Keep `sections` (which of z1, p2 the analogue
     modem has), Ltot = 192 + 64 * code, Lmax = 128 + 64 * code, the MD
     length (in units of 276 symbols) and UINFO. Continue below.
   * 3..5 / 6: V.34 upstream, the ordinary V.90 Phase 3. This is the
     fallback, chosen by the analogue modem, or forced by INFO1d bit 70 = 0
     (Table 18 "shall not" be used then).
   * otherwise V.34.
4. Later retrains use the same Phase 2 (9.3, 9.7.1: silence 70 ms, Tone B,
   wait for Tone A).

**Phase 3** (9.5.1, Figures 10 and 11). T = 1/8000 s.

1. Stay silent. Detect Ru (384T of + + + - - - at level LU) and the change
   to Ru-bar (24T). If INFO1a announced an MD, wait its length and detect
   Ru and Ru-bar again (9.5.1.1.1).
2. TRN1u follows: +-LU, signs = GPA scrambler output for ones, scrambler
   zeroed at its start, 1 = negative (8.5.7). Train on it; it lasts at
   least 2040T and a multiple of 12 symbols (9.5.1.1.2, 9.5.2.1.2).
3. Receive Ja (8.5.4): same two-point modulation, one bit per symbol,
   scrambled, differentially encoded from the last TRN1u symbol; 24 ones,
   then repeated DIL descriptors (Table 20) with the upstream rate mask
   (24000 .. 48000) appended. Timeout: 4500 ms + round trip delay from the
   end of INFO1a, then retrain (9.5.1.2.1).
4. After a complete descriptor, at most 500 ms later, send Sd for 384T,
   Sd-bar for 48T (9.5.1.1.3), then TRN1d for at least 2040T, and within
   4000 ms of its start Jd (Table 21), repeated (9.5.1.1.4, 9.5.1.1.5). The
   analogue modem stops Ja at the next 12-bit boundary after the Sd to
   Sd-bar change and goes silent (9.5.2.1.3).
5. Detect Su (144T of +a 0 +a -a 0 -a, a = sqrt(3/2) LU: a 1333 Hz
   pattern), then the change to Su-bar (24.5T), then Su again; measure the
   sampling phase of the upstream symbols on it (9.5.1.1.6, 9.5.1.1.7).
   Timeout: 5100 ms + round trip delay from the start of TRN1d
   (9.5.1.2.2).
6. Finish the current Jd, send Jp repeatedly (Table 22): the fraction of a
   symbol, 0 <= frac / 65536 < 1, by which the analogue modem shall lengthen
   the next Su-bar, and the choice of 4- or 8-point TRN2u (bits 48, 49).
   This is the only timing adjustment there is: the digital modem cannot
   move the sampling instant of the central office codec (8.6.3), so the
   analogue modem, whose 8000 Hz symbol clock is "derived from the digital
   network" (6.2), shifts its transmit phase by the requested fraction.
7. The analogue modem answers with Su-bar for (24 + fraction)T
   (9.5.2.1.8). On that Su to Su-bar change finish the current Jp, assert
   circuit 107, send Jp' (12 zeros) (9.5.1.1.9).
8. Then send the DIL the descriptor asked for (8.4.1/V.90), or SCR if
   N = 0 (9.5.1.1.11). Meanwhile the analogue modem sends a second TRN1u
   (scrambler zeroed again; at least 2040T if a DIL was requested; a
   multiple of 12 symbols). **Frame alignment**: count data frame intervals
   modulo 12 from the first symbol of this TRN1u (8.5.7, 9.5.1.1.10); that
   count runs unbroken into B1u. Train the upstream receiver on it.
9. End of Phase 3, two variants:
   * DIL requested (9.5.1.1.12, 9.5.2.1.11): the analogue modem sends CPt
     (24 ones, then Table 23 with type 0, two-point modulation) when it has
     seen enough DIL, within 5000 ms of its Su-bar. On receiving a CPt send
     Ri; the analogue modem then completes its CPt and sends E1u (12
     zeros); on E1u send Ri-bar and enter Phase 4.
   * no DIL (9.5.1.1.13, 9.5.2.1.10): when trained, send Ri; the analogue
     modem then sends CPt; on receiving it send Ri-bar and enter Phase 4;
     the analogue modem ends with E1u on Ri-bar.
   CPt gives the constellations and drn for TRN2d and the Phase 4 sequences
   (`v92_cpu_to_v90` turns it into the `v90_cp_t` that `v90tx_load_cp`
   takes).

**Phase 4** (9.6.1, Figures 12 to 14).

1. Send TRN2d for at least 2040T (8.6.5/V.90, mapping from CPt). The
   analogue modem sends TRN2u: 4 or 8 levels (Tables 28, 29), scrambled
   ones, scrambler reset at its start, sign differentially encoded from the
   last E1u symbol, for at least 12000T or until it has an SUVd. Estimate
   the upstream channel on it (8.7.6: "may be used").
2. When ready to receive CPu, send SUVd repeatedly (Table 31) in the TRN2d
   modulation and look for SUVu (Table 27, TRN2u modulation, 2 or 3 bits
   per symbol). SUVu carries the measured level of G * v in quarter dB and
   may ask (bit 26) to hold CPd until CPu was seen; complying is optional.
3. After an SUVu send one CPd (Table 30), then SUVd again (9.6.1.1.2). CPd
   carries what the upstream transmitter shall use:
   * drn (rate) and with it K = 2 * (drn + 17), which must be a rate the
     analogue modem enabled in Ja; trellis code (16, 32, 64 states);
   * M0..M11 with 2^K <= product (6.4.1);
   * constellation set index for intervals j and j + 6, LC1..LC6 and the
     positive points of each set, smallest first, no zero point, non-empty
     sets first;
   * LZ1, LP1, LZ2, LP2 and the coefficients z1 (Q0.15), p1 (Q1.14), z2
     (Q0.15), p2 (Q1.14), within Ltot, Lmax and the sections of INFO1a;
   * 4G > 0 (Q0.16), designed so that G * v(n) has mean square 1 at the
     wanted transmit power, assuming the analogue modem minimises the
     precoder output power symbol by symbol (8.8.3 and its NOTE);
   * bit 29: lengthen E2u by one symbol.
   How these are derived from TRN1u and TRN2u is not specified anywhere:
   the Recommendation fixes formats and constraints only.
4. Receive CPu (Table 23, type 1): the downstream data mode parameters, as
   CP in V.90. From then on set the acknowledge bit in every CPd and SUVd.
5. If no CPu or SUVu with its acknowledge bit set has arrived by the end of
   the sequence received 100 ms + round trip delay after the end of the
   CPd, send CPd again, repeatedly (9.6.1.1.3).
6. Once an acknowledging CPd' or SUVd' has gone out and a CPu' or SUVu'
   with the acknowledge bit, or E2u, has come in: finish the current
   sequence, send Ed (2 data frames of scrambled zeros), then B1d with the
   CPu parameters, then data (9.6.1.1.4, 9.6.1.1.5).
7. Upstream: after E2u (one data frame of zeros in the TRN2u modulation,
   plus a symbol if asked) comes B1u: 48 data frames of scrambled ones with
   the CPd parameters, every transmitter memory zero at its first symbol,
   which is data frame interval 0 and n = 0 of the filter equations
   (8.7.1). Start `v92_dec` on that symbol. After B1u unclamp circuit 104,
   turn on 109, data follows without a break (9.6.1.1.6).
8. Timeout: B1u within 20 s + 6 round trip delays from the end of INFO1a,
   else retrain (9.6.1.2.1).

## Stage 2

### What is there

* `../modem/info.[ch]`, `../modem/p2d.[ch]`: the V.92 bits of INFO0d and
  INFO1d, the three-way INFO1a decision. `p2d_offer_v92(p, 1)` before Phase
  2 offers PCM upstream; after P2D_DONE `p->mode` is P2D_MODE_V34,
  P2D_MODE_V90 or P2D_MODE_V92. `p2d_retrain_v92()` for the Phase 2 of a
  retrain. Test: `../test/test_p2_v92.c`.
* `../v90/v90_tx`: signals Jp, Jp', SCR, SUVd, SUVd', CPd, CPd', Rf, Rf-bar
  (`v90tx_set_xseq`, `v90_pack_bits`). Test: `../v90/test/test_tx92.c`.
* `v92_rx`: `v92_p6_t` finds Ru and Su (period 6 symbols) and their
  reversals. `v92_trn_t` runs from the first symbol of TRN1u: a 96 tap
  NLMS channel estimate at the network's sampling instants (the input of
  the design) and a 24 + 32 tap decision feedback equaliser that decides
  TRN1u, Ja, CPt, E1u (2 levels) and TRN2u, SUVu, CPu, E2u (4 levels).
* `v92_design`: see the header for the method. In short: the prefilter is
  the least squares solution (conjugate gradients, integer) that leaves the
  64 samples after the cursor to the precoder; the constellation is one set
  with every point at 7 noise deviations from its neighbour, the noise
  model being line noise + G.711 step at the operating point (our echo
  included) + measured residual interference; the moduli are the largest
  that keep the analogue modem's transmitter, simulated here, inside the
  power limit; a last long trial checks for precoder excursions. Cut into
  steps of bounded cost (about 1500 multiply-accumulates), one per sample:
  6600 to 17400 steps in the tests, 0.8 to 2.2 s, while TRN2u / TRN2d run.
* `v92_p34d`: the state machine. Start-up, data, rate renegotiation from
  either side (9.8, without the silence option), cleardown from either side
  (9.11), retrain on the caller's Tone A or on request (ends in
  V92D_RETRAIN).
* `../sim/v92a.[ch]`: a simulated V.92 analogue modem for tests (Phase 3,
  Phase 4, data mode with `v92_enc`, renegotiation, cleardown; reads our
  downstream octets with 24 octets of lookahead and checks every signal we
  send). `../sim/v92chan.[ch]`: the line (three pulse shapes: codec filters
  and loop), noise, hybrid echo of our downstream, G.711 mu-law or A-law, a
  digital pad upstream, delays; the analogue symbol clock is slaved to the
  network clock, the upstream is seen through G.711 at 8000 samples/s.
* `../test/test_v92call.c`: 21 calls from Ru to data mode and beyond, and
  Phase 2 in front for a V.92 caller and a V.90 caller.

### API for stage 3

Mirrors `../modem/p34d` as `../modem/answer.c` drives it today:

    p2d_init(&p2, &p2cfg);
    p2d_offer_v92(&p2, 1);                  /* before the first p2d_tick() */
    ... p2d_tick() until P2D_DONE ...
    switch (p2.mode) {
    case P2D_MODE_V92:                      /* INFO1a asked for PCM upstream */
        v92d_cfg c = { .alaw, .uinfo = p2.info1a.uinfo, .md_len, .sections,
                       .ltot, .lmax,        /* from INFO1a (Table 18) */
                       .rtd, .echo_delay, .down_rates, .max_up_rate,
                       .getbit, .putbit, .user };
        v92d_init(&m, &c);
        each sample:  octet = v92d_tick(&m, linear_rx);
        break;
    case P2D_MODE_V90:  p34d as today;  break;
    case P2D_MODE_V34:  V.34 as today;  break;
    }

* `m.state`: V92D_DATA when both directions carry data (`m.tx_ready`,
  `m.rx_ready`, `m.up_rate`, `m.down_rate` in bit/s). V92D_RETRAIN: silent,
  the owner waits 70 ms and runs Phase 2 again (`p2d_retrain_v92`), then a
  fresh `v92d_init` (or p34d if that Phase 2 decides otherwise).
  V92D_FAILED with `m.error` (V92D_ERR_x; V92D_ERR_CLEARDOWN and
  V92D_ERR_HANGUP are the orderly ends).
* `v92d_renegotiate()`, `v92d_cleardown()`, `v92d_retrain()` from data mode.
* The module itself starts a rate renegotiation when the decoder reports 4
  or more bad frames in each of two consecutive seconds; its design goes no
  higher than one step (4000 bit/s) below the rate in use (a retrain once
  that would fall below 24000 bit/s), and later renegotiations have no such
  ceiling. It retrains on Tone A. `../test/test_v92call.c` 36 runs the
  automatic path. (The modulus decoder flags few of the frames that noise
  spoils: 11 to 33 in ten seconds with a third of the bits wrong, so the
  drop can come late.)
* `echo_delay`: samples from handing an octet to the line until its echo
  comes back (the bulk delay of the codec path). Negative: measured during
  TRN1d (stage 3, reading 46).
* All state is in the caller's `v92d_t` (about 40 kB); no allocation.

### Measured in simulation

`../test/test_v92call.c` (x86-64 and ARM under qemu, same figures), all with
several seconds of data both ways and no bit error unless noted:

| call | law | line | noise (rms) | upstream bit/s | downstream |
|---|---|---|---|---|---|
| 1 | mu | short | 3 | 40000 | 50666 |
| 2 | A, DIL | short | 3 | 40000 | 50666 |
| 3 | mu | short | 40 | 37333 | 50666 |
| 4 | A | short | 40 | 36000 | 50666 |
| 5 | mu | long | 3 | 38666 | 50666 |
| 6 | A | long | 40 | 33333 | 50666 |
| 7 | A | long, bump at 2900 Hz | 3 | 37333 | 50666 |
| 8 | mu, weak signal | bump | 20 | 28000 | 50666 |
| 9 | mu, 3 dB pad upstream | short | 3 | 37333 | 50666 |
| 10 | A, 6 dB pad upstream | long | 3 | 40000 | 50666 |
| 11 | mu, strong late echo | short | 3 | 32000 | 50666 |
| 12 | A, no echo, loud | short | 3 | 40000 | 50666 |
| 13 | our ceiling 33333 | short | 3 | 33333 | 50666 |
| 14 | caller allows 24000..32000 | short | 3 | 32000 | 50666 |
| 15, 16 | renegotiation by the caller, by us | | 3 | 40000, 38666 | 50666 |
| 17, 18 | cleardown by the caller, by us | | 3 | ends in V92D_ERR_CLEARDOWN / _HANGUP | |
| 19, 20 | the caller's Tone A, our retrain | | 3 | ends in V92D_RETRAIN | |
| 21 | Phase 2 in front, V.92 caller | long | 3 | 38666 | 50666 |
| - | Phase 2 in front, V.90 caller, then `p34d` | | | 31200 (V.34) | 50666 |

Noise is in linear units RMS at the codec (full scale 32124); the signal
arrives at about 4000 per unit symbol, the echo at about 440 RMS.
Data mode is reached 3.0 to 3.9 s after the caller's Ru. In call 19 the
upstream bits received while Tone A overwrites the signal are wrong, as
they must be.

The test runs with one noise and data seed. With the same test built with
40 other seeds (800 calls, a one-off experiment, not part of the suite) no
call had a bit error. Before the last two margins went in (readings 44 and
45) the clean A-law calls at 41333 bit/s and above had some tens of bit
errors with many of those seeds, so the margin is not large.

CPU: `v92d_tick` in data mode costs 0.67 of `p34d_tick` in data mode (x86-64,
-O2, 40000 recorded samples replayed, best of 15: 197 ns against 295 ns per
sample; both include the transmitter and the echo canceller).

### Readings and choices, continued

32. Figures 10 and 11 against 9.5.2.1: after the second Su-bar, of
    (24 + fraction)T, TRN1u follows at once; no further Su is expected.
33. Jp (Table 22, 9.5.1.1.8). How the digital modem finds the timing phase
    to ask for is not specified. The fraction is taken from the peak of the
    channel estimate of the first TRN1u (parabola through three taps), not
    from Su, and asks for the peak to fall on a sampling instant; the half
    symbol slip of 9.5.2.1.7 is allowed for.
34. 8.6 (Rf): "repeating the 12-symbol sequence" is read as the sign
    pattern + + - - three times, each symbol being the largest point of the
    data constellation of its data frame interval. Rf-bar is two
    repetitions of the inverse. (Fast parameter exchange itself is not
    implemented.)
35. 8.8.3, "mean-square value of 1": the reference is the power of TRN1u,
    that is, G v(n) = 1 means the level LU of the training symbols.
36. E1u and E2u are looked for only after a complete CPt, or CPu / SUVu
    with the acknowledge, has been received.
37. 9.6.1.1.3 gives no time for repeating CPd, and says only "repeated CPd
    sequences". It is sent again when no CPu has come 100 ms + a round trip
    + 150 ms after the last one ended, one copy at a time with SUVd between:
    9.6.1.1.2's single CPd followed by SUVd, once per copy (Figures 12 to 14
    draw no repeat; an Agere engine cleared down on CPds sent back to back).
38. 9.11: we clear down with a rate renegotiation of ours that sends a CPd
    with drn = 0 (9.11 names SUVd, which has no rate field; the rate
    sequence the digital modem sends is CPd). As 9.8.1.1.2 gives it: Rd,
    Rd-bar, 48 data frames of TRN2d, then SUVd; on the caller's SUVu a
    single CPd (the CPd in use with drn = 0, no new design) followed by
    more SUVd, with the acknowledge bit once the caller's CPu is in
    (9.6.1.1.2). It is over where 9.6.1.1.4 would send Ed: our SUVd' out and
    the caller's acknowledge (CPu' / SUVu') or E2u in; silence then, not
    Ed, since no data mode follows drn = 0. With the simulated caller some
    0.3 s, with this library's own caller some 0.4 s. A caller that sends
    no SUVu (or no longer hears us, or hangs up on our CPd without
    acknowledging it) gets SUVd, and our CPd (repeated as in reading 37) if
    its SUVu came, until 3.5 s from the request; nothing in the cleardown
    retrains. A CPu or SUVu with drn = 0 is the
    caller's cleardown.
39. SUVu's "wait for CPu" hint is ignored: CPd is sent once after the first
    SUVu in any case (9.6.1.1.2).
40. 9.8: Ru in data mode is recognised about 13 ms after it began; the
    upstream data of that time is delivered and is garbage. Rate
    renegotiation with the silence option is not implemented.
41. The quantiser's operating point in data mode depends on the echo of
    our downstream signal, which is not there yet while the design runs
    (we send TRN2d). The echo is predicted from the canceller's taps for a
    downstream power of -9 dBm0. (Stage 4, reading 54: for the power INFO0d
    announced.)
42. 8.8.3 NOTE: the design assumes the analogue modem picks, symbol by
    symbol, the member of the equivalence class that minimises |x(n)|. A
    modem that picks differently sends another power and may leave the
    range in which the precoder stays bounded (see "Not done").
43. The constellation is one set (all six intervals share it); LZ1 = LP2 =
    0, LZ2 <= 128, LP1 <= 64, both within the caller's INFO1a limits.
44. Noise allowance (our choice, `v92_design.c` ST_POINTS). Added to the
    measured noise floor: the error of the channel estimate (training
    residual / 64, the misadjustment of the NLMS estimate) and residual
    echo in data mode, set to 34 dB below the echo. The second figure is a
    calibration against the simulated calls, where the decoder's error at
    the smallest points was larger than floor, quantiser and estimate
    explain; its cause was not found. The quantiser's operating point is
    taken at 1.5 times the echo RMS.
45. Precoder excursions (our choice). The final trial rejects a design in
    which |x(n)| exceeds 3.5 times its RMS within 12000 symbols and lowers
    the modulus (about 1333 bit/s per step). On lines with a strong
    feedback filter (the "long line" shapes) this costs one to three steps.

### Not done, not verified

* No interoperability test. The analogue side is our own reading of the
  same text; an error of reading common to both sides passes every test.
* (Integration: see "Stage 3".)
* A V.34 caller against a Phase 2 that offers V.92 is tested through Phase
  2 only (`test_p2_v92`: mode P2D_MODE_V34); the full V.34 call tests run
  the unchanged path without the V.92 offer.
* Quick connect, modem-on-hold, fast parameter exchange (9.9), the silence
  variants of rate renegotiation: not implemented.
* No adaptation in data mode: the echo canceller is frozen after TRN1d,
  there is no equaliser or gain tracking on the upstream data, and the
  sampling phase is whatever Jp achieved. The simulated analogue clock is
  slaved to the network clock exactly; a real modem's loop jitter is not
  modelled. Slow drifts end in a rate renegotiation (bad frame watch).
* (The bulk delay search came with stage 3, reading 46.)
* The MD path (md_len > 0) is coded and not tested. The 8-point TRN2u is
  not used (4 points are asked for).
* Precoder stability is checked by simulating 12000 symbols during the
  design, not proved. With a channel that has zeros at 0 Hz (every real
  one) and unevenly spaced equivalence classes, x(n) can leave its normal
  range if the feedback asks for a point beyond the last member of a
  class. In an earlier version of the design this happened in simulation
  (transmit power running away); the present one passes all calls and
  channel shapes of the tests, but a different line or a different point
  choice in the analogue modem may still trigger it. The remedy in place is
  the bad frame watch (rate renegotiation).
* Robbed-bit signalling in the upstream direction is not modelled or
  handled. Digital pads are handled only through the measured noise.
* The design time (up to 2.2 s at one step per sample) and the per-step
  cost have been measured on x86 only.
* The cause of the excess decoder noise of reading 44 is open.

## Stage 3: in the answering modem

### What is there

* `../modem/answer.c`: with `ANS_MODE_V92` among the modes (and
  `ANS_MODE_V90`, which it needs) Phase 2 offers V.92 (`p2d_offer_v92`).
  After Phase 2 the call goes three ways by INFO1a: PCM upstream
  (`v92_p34d`, phases `ANS_P3X` and `ANS_V92`, modulation `ANS_MOD_V92`),
  V.34 upstream (`p34d`, as before), V.34 both ways (as before). Retrains
  from either side, rate renegotiation, cleardown, `answer_snr_db10`,
  `answer_rtd`, `bitrate` / `rx_bitrate` work for the V.92 phases.
  `answer_moving()` says whether user data flows both ways (dce's hint for
  LAPM after a retrain or a rate renegotiation); `answer_odp_bypass()`
  whether 9.3.1 applies.
* `v92_p34d`: the echo delay is measured when it is not given (reading 46);
  a carrier watch ends a call whose caller is gone (reading 47);
  `v92d_trace` under `P34D_TRACE` for `../live/replay.c`.
* `../dce`: the modulation is followed across retrains (V.92 to V.90 when
  the fallback of reading 48 happens); an upstream rate that changes alone
  is followed; T400 is 5 s when the caller bypasses the ODP (reading 49).
* `../srv`, `../cfg`: `v92 = on|off` in `[modem]` and `[modem2]` (default
  off), on the settings pages, `"v92"` in the status JSON, modulation
  `V.92` / `"v92"` in the pages, the JSON, the history and the call log.
  V.92 is offered only when `v90` is among the modulations and `v42 = on`
  (reading 49). `max_upstream` is the ceiling of the PCM upstream too: its
  default 33600 gives at most 33333 bit/s; 48000 lifts it.
* `../sim/v92call`: a whole simulated V.92 (or V.90) caller with its line:
  V.8, Phase 2, then PCM upstream or V.90 as its INFO1a says, retrains,
  renegotiation, cleardown, hanging up.
* Memory: `v92d_t` shares a union with `v32_t` in `answer_t` (a call is
  never both). `sizeof(answer_t)` 93560 -> 98192 on x86-64, 93328 -> 97888
  on ARM; `sizeof(dce_t)` 154184 -> 158816 and 153616 -> 158176.
* CPU in data mode, the answerer alone on recorded input, x86-64 at -O2: V.90
  301 ns per sample, V.92 182 ns (0.60 of V.90). The same code as ARM
  under qemu: 2521 and 1471 ns (0.58). The start-up (the CPd design
  above all) is not in these figures and has not been timed on the unit.

### Tests

* `../test/test_answer_v92.c`: the answer layer against `sim/v92call`,
  mu-law and A-law: V.92 to data both ways; retrains and rate
  renegotiations from either side; both cleardowns; rate ceilings; a failed
  V.92 start-up that ends as V.90; a V.92 caller with PCM upstream off; a
  V.90 caller and a V.34 caller against the V.92-enabled answerer; V.92
  not enabled; the caller gone.
* `../test/test_v92call.c` scenarios 22 to 24: echo delay measured at 60
  and 400 samples; the carrier watch.
* `../dce/test/test_call.c` (o) and (p): V.42 with V.44 and PPP over V.92
  with traffic both ways, renegotiations, retrains, endings, A-law, the
  fallback to V.90, the ODP bypass, callers without V.92.
* `../srv/test/test_srv.c` (`test_v92_call`) and `../srv/test/ns_call.c`
  (4b): a V.92 call through the server and through the real daemon, with the
  status page, the JSON and the call log line.

### What callers without V.92 see on the line

* V.92 off (the default): nothing. The octets we transmit are identical to
  those of the tree before stage 3, compared over whole calls with retrains,
  renegotiation and cleardown (V.90 mu-law and A-law, measured and given
  echo delay, a V.92-capable caller, a V.34 caller).
* V.92 on, V.34 / V.32bis / V.22bis / V.21 callers: nothing. V.8 cannot say
  V.92 (reading 50), so ANSam and JM are the same, and such callers never
  reach the V.90 Phase 2.
* V.92 on, V.90 caller: our INFO0d has bit 27 set and with it another CRC
  (bits 42:57). Nothing else: in the test 392 of 240000 octets differ, all
  within the INFO0d sequences of the first 3 s.
* V.92 on, V.92-capable caller with PCM upstream switched off: as above,
  and INFO1d is the V.92 form (Table 17), the caller's INFO1a is Table 18,
  and the caller skips ODP / ADP (reading 49). The modulation is V.90.

### Readings and choices, continued

46. Echo delay (our choice). With `echo_delay < 0` TRN1d is lengthened and
    the first received samples under it are correlated against the
    transmitted history over lags 0 to 1280 samples, as `../modem/p34d`
    does in its TRN1d; the canceller's bulk delay is the best lag less 8.
47. Carrier (our choice; the Recommendation has no carrier detector in
    data mode). The mean square of the received samples during B1u is the
    reference; 50 consecutive blocks of 10 ms more than 18 dB below it end
    the call (`V92D_ERR_CARRIER`). The watch also runs while a rate
    renegotiation waits for the caller, because the onset of silence can
    look like the caller's Ru to the detector of 9.8.
48. Recovery (9.5.1.2, 9.6.1.2, 9.7). Whatever goes wrong in Phases 3 and 4
    is answered with a retrain, and Phase 2 of a retrain between two V.92
    modems is V.92's (9.3). We offer PCM upstream in it again (INFO1d bit
    70) unless this start-up never reached data mode, or `v92_p34d` itself
    gave up in data mode; then bit 70 is 0 for the rest of the call and the
    caller is left with V.34 upstream: the call goes on as V.90. A retrain
    the owner asked for (`answer_retrain`) or the caller began from data
    mode keeps the offer. `answer` ends the call at the fourth retrain in a
    row, as for V.90.
49. ODP / ADP bypass (9.3.1): when both modems indicated V.92 (INFO0d bit
    27, INFO0a bit 26) and both V.8 menus carried LAPM in the protocol
    category, the detection phase of V.42 is skipped; this follows from
    INFO0, so it holds for a V.92 caller that then selects V.34 upstream
    too. `../v42` was not changed: its detection phase already takes flags
    or a frame without an ODP as the start of LAPM and sends an ADP only
    after an ODP. dce lengthens T400 to 5 s in this case so that the timer
    cannot send a caller that is committed to LAPM into start-stop mode.
    Because of this the server offers V.92 only with `v42 = on`.
50. V.8 (9.1; Table 5/V.8): Phase 1 of V.92 is V.90's, and the PCM
    category has one bit for "V.90 or V.92 digital modem". JM is not
    changed by the V.92 setting. Quick connect is not implemented; the
    short Phase 2 (INFO0d bit 26) is not asked for.
51. Upstream ceiling. `max_up` of `answer_v90_line` (the `max_upstream`
    setting) limits the PCM upstream rate as it limits V.34's.
52. A rate renegotiation can change the upstream rate alone (CPd carries
    both, 9.8); dce follows `rx_bitrate` for V.92. (For V.90 it follows the
    downstream rate only, as before.)

### Not done, not verified

* (Calls with a real V.92 modem: stage 4.)
* Quick connect, modem on hold, the short Phase 2: not implemented.
* The simulated caller does not read the downstream rates of Jd; the
  `max_downstream` ceiling is checked in Jd only.
* A V.92 call without error control (start-stop over PCM upstream) is not
  offered by the server and not tested.
* (The CPd design on the unit's ARM926: reading 56.)
* Whether real modems accept bit 70 = 0 in a retrain's INFO1d after bit 70
  = 1 in the first (reading 48) is not known.

## Stage 4: a real V.92 modem

A Conexant CX93001 (USB, `AT+PIG=0`: PCM upstream allowed) calling the unit
over its FXS port, recorded at the codec (`../live/rec.sh`) and replayed
through the same code on the host (`../live/replay.c`, a debug tool built
with `P34D_TRACE`; in V.92 data mode it runs the modem alone, with the
recorded octets as the echo reference, because the layers above cannot
repeat a recorded call's data). The first calls failed in Phase 3 and fell
back to V.90. What the recordings showed, in the order met, and what was
done:

### What the modem sends

* Ru, then a first TRN1u of 16368 T (the text asks for 2040 T at least).
  Its clock moves by 38 ppm against ours during that time and is locked
  afterwards.
* Ja with the DIL descriptor for N = 119 and all upstream rates, without
  the 24 ones in front (8.5.4): the first frame sync follows the last
  TRN1u symbol. Our descrambler, which starts with that symbol, garbles the
  first 22 bits, so the second copy is the one received, 192 ms later. Not
  changed.
* Silence after Sd-bar as the text says; Su 3.8 s after Jd began.
* Level at our codec about 1380 units RMS (-21 dBm0), as loud as the echo
  of our own transmitter on this port.
* INFO1a: p1 and z2 only, Ltot 192, Lmax 128, UINFO 79.
* After E2u one data frame in which every Ki is 0, then the 48 data frames
  of B1u (reading 57).
* A rate renegotiation of its own (Ru) some 20 s into data mode.

### Readings and choices, continued

53. Echo on a two-wire port (our choice). The echo of the FXS port has a
    slow tail (a single time constant of about 25 ms) that the 12 ms
    canceller of `../modem/ec` cannot hold: 150 to 250 units of low
    frequency wander were left while the caller was silent, and the
    detector of Su took them for Su and its reversals (first recording:
    Jd to Jp at 17.542 s and on to the DIL at 17.566 s while 1396 of the
    1401 units RMS on the line were our own echo; the engine then failed
    and the call went on as V.90). Two changes. The period-6 detector
    (`v92_p6_t`, Ru and Su) also asks that x(n) = -x(n - 3) over its
    window, which both signals satisfy and a wander does not. And behind
    the canceller sits a model of the tail: the transmitted signal, 12 ms
    on, through two first order low-passes (16 and 32 ms), one adaptive
    coefficient each, trained with the canceller during TRN1d. On the
    recordings the residual fell to 20 to 45 units (33 dB under the echo).
54. Echo in data mode (replaces the -9 dBm0 of reading 41). The echo the
    design allows for is predicted for the largest downstream power we may
    use, which is what INFO0d bits 33:37 told the caller (-12 dBm0 on the
    unit), `v92d_cfg.down_rms`. With -9 dBm0 the design found no
    constellation for 24000 bit/s on this line (V92D_ERR_DESIGN, second
    recording); with the real figure it does.
55. (Not used.)
56. Design time (our choice). A step of the CPd design costs two to four
    times what a sample of V.90 data mode costs on the unit's ARM926 (199
    BogoMIPS; data mode takes 29 % of it). At one step per sample the
    daemon fell behind in the third recording: 24.2 s into the call the
    echo on the line no longer matched what we had sent (correlation at
    the echo's lag from 0.27 to 0.05), the receiver lost the caller's
    symbols and no SUVu was decoded. `v92d_cfg.dsn_pace` spreads the steps
    (one every 4 samples on the unit, `V92_DESIGN_PACE` in
    `../modem/answer.c`); the channel estimate is held meanwhile. A design
    takes about 4 s that way, in Phase 4 and again in every rate
    renegotiation, and the time limits of the renegotiation allow for it.
57. 8.7.1, the first symbol of B1u. The CX93001 sends one data frame with
    every Ki = 0 between E2u and the scrambled ones (its encoder is a data
    frame late), and the 48 data frames that follow are those of 8.7.1 as
    if that frame had not been sent: scrambler and modulus encoder start
    with them. Seen in the decoder's input: twelve samples at the smallest
    positive point (once the second smallest, in a k = 3 interval), then
    B1u. With p1 = 0 that frame is a constant through a channel without
    DC and looks like 12 T of silence. `v92_p34d` judges the first data
    frame: if at least ten of its twelve hard decisions give Ki = 0 it is
    passed over and the decoder starts again (at most two such frames).
    Of scrambled ones no data frame looks like that.
58. Prefilter coefficient 64. The CX93001 takes z2(64) as zero whatever
    CPd carries. Found with CPd sequences made for the purpose (a compile
    time switch, `V92D_PROBE` in `v92_p34d.c`, bench only): with the
    designed z2, whose largest tap was z2(64), B1u came at 0.4 of the
    expected amplitude and did not decode; with z2 a single tap at 64 the
    modem sent E2u and then nothing; with p1 = 0 the response from u(n) to
    our samples, fitted over B1u, was that of h * z2 with z2(64) removed
    (to three decimals with LP1 = 48, and the same response with LP1 = 64);
    with LZ2 = 96 (peak at z2(48)) it was h * z2 itself. The design keeps z2(64) at zero (the
    conjugate gradient iteration runs without that coordinate) and puts
    the cursor 16 taps before the end of z2 instead of in its middle, so
    that the coefficient lost is one of the small ones in front. With 64
    taps or fewer nothing changes. (A z2 of one tap, LZ2 = LP1 = 1, the
    modem did not accept at all: no E2u, silence.)
59. 8.7.1, "convolutional encoder ... initialized to zero". After the
    rate renegotiation it began, the CX93001's B1u had every Ki as
    expected (576 of 576 hard decisions) but our decoder, which took the
    encoder to start from state zero, delivered 25 wrong bits of 1728 and
    the renegotiation ended in a retrain and V.90. At start-up the same
    decoder made no error. The decoder in `v92_p34d` no longer assumes a
    first state (`v92_dec_any_start`).

The other conventions were confirmed by the same probes: with p1 = 0 and
the tie rule of reading 1, `v92_enc` run on the CPd we sent predicts the
received B1u to -33 dB (bits, scrambler, modulus encoder, classes, parity
and trellis, coordinate order, coefficient formats and order, G and the
power reference of reading 35: fitted gain 1.002). With the designed p1
all 576 hard decisions of B1u give the Ki of the model; the points chosen
differ in places where two members are almost equally good.

### Tests

* `../test/test_v92call.c`, scenarios 25 to 31: the FXS echo with its slow
  tail at the caller's level, the data frame of Ki = 0, z2(64) ignored, a
  trellis encoder that does not start from zero (with a renegotiation),
  and all of it together with the CX93001's INFO1a limits.
  `../sim/v92a` and `../sim/v92chan` have the switches (`ja_no_ones`, `b1u_lead`,
  `z2_dead`, `conv_start`, `echo_tail`); their defaults are the text's.
* `../test/test_v92real.c` on `../test/data/cx93001_b1u.txt`: the
  decoder's input of one recorded call after E2u, at start-up and after
  the renegotiation (skipped when the file is absent).
* `test/test_rx.c`: the period-6 detector on a low frequency wander.

### Measured on the line (unit, port 2, CX93001)

* V.92 49333 to 52000 bit/s down, 24000 bit/s up, LAPM and V.44, PPP up 32
  to 34 s after dialling (V.90 after a failed V.92 attempt: 47 s). 300000
  octets of HTML in 41 s (7297 octets/s) on a call that stayed V.92 through
  the caller's renegotiation.
* The upstream rate is the lowest there is: at the codec the caller is no
  louder than our own echo, and the design allows for the quantiser steps
  at the echo's peaks (reading 54). V.34 upstream gave 26400 bit/s on the
  same line.

### Not done, not verified

* One modem model, one line. Whether other modems share readings 57 to 59
  is not known; the changes do not disturb a modem that follows the text.
* The first Ja is lost (no 24 ones; 192 ms). Jp's phase comes from the
  first TRN1u, before the caller's clock has settled (reading 33); the
  second TRN1u is estimated from scratch, so nothing depends on it.
* A rate renegotiation costs 4.5 s without data (reading 56); the design
  is run again although the channel is the same.
* Renegotiation and retrain begun by us, cleardown by either side, and a
  call longer than a few minutes have not been tried on the line.
* The replay cannot reproduce data mode beyond the modem itself (see top).


## Stage 5: a second V.92 modem (U.S. Robotics 5686)

Measurements and test output: v90d's PROGRESS.md, 2026-11-16. The modem asks for
PCM upstream without being told to. What it needed, as readings:

60. The echo delay is searched on the change from sample to sample (as in
    `modem/p34d.c`): this modem returns our low frequencies as a tail of
    more than 12 ms, and on plain samples the tail put the peak 4 ms late.
61. 9.6.1.1.1 says TRN2d for at least 2040 T and then SUVd; it does not
    say that SUVd waits for the design. The modem waits 3.8 s into TRN2u
    for SUVd and 3.5 s from its SUVu for CPd, then falls silent and
    retrains. SUVd is sent when the design is done or 2.5 s into Phase 4,
    whichever is first, and the design takes a step every 2 samples
    (9000 to 15000 steps: 2.2 to 3.7 s).
62. The first CPd is sent without the acknowledge bit even when CPu has
    been received already (which reading 61 makes possible). A first CPd
    with the bit set was acknowledged and then answered with silence, 7
    calls of 7; without it the modem went on to B1u, 9 of 9. The bit
    follows in SUVd' as before.
63. In data mode the receiver follows the baseline under the symbols: each
    decision's error moves it by an eighth. Without that, what the echo
    canceller and its tail estimate leave of our own low frequencies moved
    whole runs of symbols a point off (100 units, points 245 apart).
    (Later, with a G.711 codec only: less where the codec's step at the
    point is more than the noise, or the error may be a wrong decision;
    the simulator's bursts at the top rates, ../docs/history/PROGRESS.md answer-fix 1.)

Not a reading: for a while the refusals of reading 62 were taken for a
limit on the transmit power asked for. Designs at 0.84 to 1.02 of the
limit were accepted once 62 and 63 were in.

### Measured on the line (unit, port 2)

V.92 54666 / 40000 with the upstream limit at 48000, 54666 / 33333 at the
default 33600; CONNECT after 22 to 25 s; five calls of five each.

### Not done, not verified

* The CX93001 on the line against readings 60 to 63: V.92 in about 6
  calls of 7. In the one failure that was recorded its transmit clock was
  58 ppm off ours during the second TRN1u (v90d's PROGRESS.md); the call retrains
  to V.90. A receiver that follows such a caller is not there.
