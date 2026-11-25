# Calling

`V90M_ROLE_ORIGINATE`: the calling modem, and for V.90 and V.92 the
analogue (client) side.

**Simulation and own answerer only.** Every modulation of the calling role
has been tried against simulated lines and this library's own answering
modem, never against a real modem or a real server. What it sends has been
compared, field by field, with what real callers sent in 106 recorded calls
(`test/test_realcheck.c`); that checks the formats of its sequences, not
how a real answerer responds to them.

How it is built is in `DESIGN.md`, sections 4.2c (V.90 / V.92), 4.2d
(V.92's PCM upstream) and 4.2e (Bell).

## Where it starts

The calling role starts when the far end has answered: dialling and call
progress are the program's ([line-interface.md](line-interface.md)). It
listens for the answer tone and tells ANSam (V.8) from ANS (V.25):

- **ANSam** with a V.8 modulation on offer: silent for Te (1 s), then CM
  until two identical JM, then CJ and 75 ms of silence; the JM selects the
  modulation. With `send_ci` set, CI is sent before the answer tone (V.8
  7.1).
- **ANS** (a pre-V.8 answerer): no CM. With V.32bis on offer, V.32bis
  Annex A (the calling automode modem); otherwise the V.22bis answerer's
  unscrambled ones lead to V.22bis and a steady channel 2 mark to V.21, or,
  with V.21 not offered, a steady mark on V.23's forward channel to V.23.
- **Bell**: see below.

It ends the call with `V90M_END_NO_ANSWER_TONE` when no answer tone comes
(10 s), `V90M_END_NO_JM` when ANSam came but no JM answered CM (5 s of CM),
and `V90M_END_NO_COMMON` when the answerer has no modulation in common.

**Run V.42 on a calling modem.** Our CM always names LAPM (V.8's protocol
category), whether or not the program runs a `v90m_ec` endpoint. An
answerer that takes that at its word expects V.42 frames: a V.92 one that
named LAPM too skips V.42's detection phase (V.92 9.3.1) and waits for our
first LAPM frame (v90d, for one, waits 5 s). A calling program should
therefore run the V.42 endpoint ([error-control.md](error-control.md)),
with the detection phase unless `v90m_state.odp_bypass` says otherwise;
its start-stop fallback covers an answerer without V.42.

## The modulations

`v90m_cfg.modulations`; the default is everything but V.92 and Bell, as
when answering. At least one is required (0 gives `V90M_EINVAL`).

| flag | the calling side | retrain | rate renegotiation | cleardown |
|---|---|---|---|---|
| `V90M_MOD_V21` | 300 bit/s, after V.8 or a pre-V.8 answer tone | | | |
| `V90M_MOD_V23` | 75 bit/s out / 1200 bit/s in, after V.8 or a pre-V.8 answer tone | | | |
| `V90M_MOD_V22` | V.22 1200, V.22bis 2400 bit/s | at 2400 bit/s | | |
| `V90M_MOD_V32BIS` | V.32bis / V.32, 4800 to 14400 bit/s; also Annex A after ANS | either end | the one the answerer asks for | |
| `V90M_MOD_V34` | Phase 2 with its own INFO1c decision, Phases 3 and 4, data | either end | yes | either way |
| `V90M_MOD_V90` | V.90 analogue modem: PCM downstream, V.34 upstream | either end | either end | either way |
| `V90M_MOD_V92` | with `V90M_MOD_V90` only (else `V90M_ENOTSUP`): PCM upstream when the answerer offers it in Phase 2, V.90 otherwise | either end | either end | either way |
| `V90M_MOD_BELL103` | 300 bit/s FSK | | | |
| `V90M_MOD_BELL212A` | V.22 at 1200 bit/s on the Bell answer tone | | | |

(From the header and the top-level README's standards table.)

## V.90 and V.92

- **Receive side.** It needs the received samples linear as the line
  delivers them: `v90m_process_linear` or `v90m_process_lin_rx`.
- **Start-up.** V.8 offers PCM; Phase 2 (probing and ranging, the V.34
  answer procedure with V.90's INFO sequences); Phase 3, in which the
  downstream receiver learns the clock and the channel on Sd and TRN1d and
  the digital impairment learning sequence (DIL) is measured: pads, robbed
  bits, the law; Phase 4, in which the constellations it asks for (CP) are
  designed from the DIL and the noise. Data mode in the simulator was
  reached some 10 to 14 s from the start of the call
  (`docs/history/PROGRESS.md`, C5-2 and C6-2).
- **Who answers.** V.90 / V.92 needs a digital modem at the far end: PCM
  is chosen only when the answerer's JM names one. A JM naming only "PCM
  analogue" (another analogue V.90/V.92 modem) gives V.34, or the lowest
  other mode in common.
- **V.92.** With `V90M_MOD_V92` it says V.92 in Phase 2 and selects PCM
  upstream when the digital modem allows it; the upstream is then PCM
  through the precoder and prefilter the digital modem dictates, on the
  network's clock. Otherwise the call is V.90. `v90m_state.modulation` is
  `V90M_CONN_V92` only when the upstream is PCM. The digital modem's CPd
  (the moduli, the precoder and prefilter, the constellations) must be
  whole in Phase 4; in a rate renegotiation a CPd may carry only the parts
  that changed, the others being kept from the CPd before.
- **Retrains**, either end: `v90m_retrain`, or the answerer's Tone B. Ours
  is 70 ms of silence and Tone A, then Phase 2 to data mode again, some 7
  to 11 s, with the receiver and (V.92) the PCM upstream trained afresh.
  `v90m_retrain` also works during a rate renegotiation.
- **Rate renegotiation**: the answerer's is followed; the modem begins one
  of its own when its receiver's error moves far from what the rate was
  chosen for, and a retrain when the line's level moved or the receiver is
  lost. There is no public call to renegotiate.
- **The limit.** `v90m_retrain` gives `V90M_ESTATE` when a retrain is under
  way or three have run without a minute of data mode between them. A
  retrain that fails, or one more due past that limit (the far end's, or
  one the receiver needs), ends the call with `V90M_END_RETRAIN_FAILED`;
  the receiver's is sent as a cleardown first (V.90 9.7 / V.92 9.11).
- **Cleardown** either way (`v90m_cleardown`; `V90M_END_CLEARDOWN_FAR`
  when the answerer clears down).
- **Not done:** V.92 quick connect, short Phase 2, modem-on-hold.

## Bell 103 and Bell 212A

Not in the defaults. V.8 cannot offer Bell, so Bell is what the caller does
when no V.8 comes about; a V.8 exchange that finds nothing in common does
not fall back to Bell. In this order:

1. ANSam with a V.8 modulation also on offer: V.8, as ever.
2. 2225 Hz (the Bell answer tone, within some 10 Hz) before any 2100 Hz
   tone, or after ANS: Bell 212A if offered, else Bell 103.
3. Only Bell on offer: after ANSam or ANS the modem stays silent and
   listens. 2225 Hz gives Bell 212A (else 103). With 103 alone, 150 ms of a
   carrier in the Bell answer channel (2025..2250 Hz, a V.22 answerer's
   unscrambled ones included, 2100 Hz not) gives Bell 103; with 212A as
   well, 6 s of that carrier without 2225 Hz gives Bell 103.
4. A Bell 212A start that gets no 1200 bit/s answer in 3 s falls back to
   Bell 103, if offered.

Bell 212A is V.22 at 1200 bit/s, with no retrain; Bell 103 is 300 bit/s FSK
(mark 1270 / space 1070 Hz out, 2225 / 2025 Hz in), start-stop and no error
control, as V.21. Neither has a cleardown signal. The 212A handshake
follows V.22 and what this library's answerer accepts; it was not checked
against a Bell 212A specification, and the 3 s and 6 s fallbacks are this
library's choices.

## What the program sees

The same events as answering: `V90M_EV_CONNECTED`, `V90M_EV_RATES`,
`V90M_EV_DATA_STOPPED`, `V90M_EV_DATA_MOVING`, `V90M_EV_ENDED`. End codes:
`V90M_END_FAILED`, `V90M_END_LOST`, `V90M_END_CLEARDOWN_FAR`,
`V90M_END_CLEARDOWN_OURS`, and the calling role's own
`V90M_END_NO_ANSWER_TONE`, `V90M_END_NO_JM`, `V90M_END_NO_COMMON`,
`V90M_END_RETRAIN_FAILED`.

**When the far end hangs up** in data mode, the end code depends on what
the line then brings:

- the answerer clears down first (V.90 9.7, V.92 9.11, V.34 11.7, V.32bis's
  rate signal with no rate): `V90M_END_CLEARDOWN_FAR`;
- silence (V.90 and V.92: the downstream 12 dB under its level for 0.5 s;
  V.21, V.22, V.22bis, Bell: no carrier for 0.4 s; V.32bis and V.34: their
  receivers' carrier loss): `V90M_END_LOST`;
- a signal as loud as the modem's that is not one (dial tone, reorder, a
  recorded message, noise), in V.90, V.92 and V.34: the receiver loses
  lock and the modem retrains; nobody answers the retrain, and its Phase 2
  fails within its own limits (30 s at most): `V90M_END_RETRAIN_FAILED`;
  the same when the retrain limit has been used up. V.21, V.22, V.22bis
  and Bell watch only their carrier: what their receive filter does not
  pass is silence to them.

A program that sees `V90M_END_LOST` or `V90M_END_RETRAIN_FAILED` after its
data finished should treat it as the far end gone; only
`V90M_END_CLEARDOWN_FAR` says the far modem meant it.

For V.90 and V.92, `tx_bps` is the upstream and `rx_bps` the downstream.
`v90m_get_stats` gives `snr_db10` (the downstream receiver's), `rtd` (V.32bis,
V.34, V.90, V.92), and the counts of retrains and of rate renegotiations
that stopped the data (V.32bis, V.34, V.90, V.92). `echo_delay` in the stats
and `odp_bypass` in the state are always -1 and 0 when calling.
`v90m_get_diag` (0.6.1) gives, as answering, the cause of the end in the
engines' words (`V8_ERR_NO_TONE`, `P2L_ERR_REV`, `V90A_ERR_SD`, ...), the
furthest phase the call reached (it never goes back) and the modulation
chosen, and `data_reached`, whether data mode was reached. An orderly
cleardown, ours or the far end's, after data mode reads error 0. The
start-up failed if, and only if, the call ended and `data_reached` is 0
(docs/api.md).

## Configuration that matters here

- `max_down_bps` caps what we send (the V.34 upstream, or V.92's PCM
  upstream), `max_up_bps` what we receive (the PCM downstream). V.32bis
  takes the lower of the two both ways.
- `echo_delay`: see [line-interface.md](line-interface.md); V.90 / V.92
  use it as a fixed delay.
- `send_ci`: CI before the answer tone.
- `codec_linear_rx` and `max_tx_code` are unused.

## Limits

Everything on this page was tried in simulation and against this
library's own answerer only. What a real digital modem sends in a retrain's
Phase 2 is not known: this library's answerer sends no INFO0 there, V.92
9.7 asks for "the full Phase 2", and the caller takes either form, tried
only against its own answerer made to send INFO0d. One V.92 call takes
some 51 to 55 % of an ARM926 at 400 MHz (qemu instruction counts); two
do not fit. More in [limits.md](limits.md).
