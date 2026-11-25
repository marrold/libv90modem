# Error control

Once connected, the modem is a synchronous bit pump: it calls `getbit` for
every bit it transmits and `putbit` for every bit it receives, from inside
`v90m_process_*`. Two objects of the library sit on that pump, each with
its own memory and no allocation after create or init:

- `v90m_async_*`: start-stop characters (V.14 basic), no error control;
- `v90m_ec_*`: a V.42 endpoint (LAPM, detection phase, start-stop fallback,
  breaks) with V.42bis or V.44 compression.

Use one of them, or framing of your own. Neither makes callbacks: the
program moves bits between the modem's callbacks and the object, and
octets in and out with write / read.

## Start-stop characters (V.14)

Characters of 1 start bit, 8 data bits LSB first and 1 stop bit, mark when
idle. Pass `v90m_async_getbit` and `v90m_async_putbit` as the modem's
`getbit` / `putbit`, with the object as `user` (or call them from your own
callbacks, as `examples/call_loop.c` does). The queues are fixed at 512
octets each way, and all 512 hold an octet (up to 0.5.0 they held 511). A
character received while the receive queue is full is dropped and not
counted: read often enough, or use the V.42 endpoint, whose start-stop
mode counts its overruns (`async_overruns`).

| | |
|---|---|
| `v90m_async_create(&err)`, `v90m_async_init(mem, len, &err)` | NULL on failure, `*err` as for `v90m_create` / `v90m_init`; memory `V90M_ASYNC_MEM_SIZE` |
| `v90m_async_reset`, `v90m_async_destroy` | |
| `v90m_async_write(a, buf, n)` | octets accepted |
| `v90m_async_read(a, buf, max)` | octets delivered |
| `v90m_async_tx_pending(a)` | queued or being sent |
| `v90m_async_frame_errors(a)` | characters with a framing error |

This is what V.21, Bell 103 and any modem without error control speak. A
V.42 endpoint falls back to start-stop by itself when the far end has no
V.42 (below), so a program that always uses `v90m_ec_*` needs no framer of
its own.

## The V.42 endpoint

One endpoint on the bit pump of a connected modem:

```
getbit  -> v90m_ec_tx_bit(ec)          every bit the modem sends
putbit  -> v90m_ec_rx_bit(ec, bit)     every bit the modem receives
every ms   v90m_ec_tick(ec, ms)        timers (T400, T401, T403)
octets     v90m_ec_write / v90m_ec_read
```

`v90m_ec_cfg_default(&cfg, originator)` fills the configuration:
`originator` 1 for the calling end, 0 for the answering end. The defaults
(`v42/v42.c`): detection on, start-stop fallback on, `t400_ms` 750,
`establish_ms` 30000, `V90M_COMP_AUTO`, V.42bis and V.44 wanted both ways
(V.42bis 2048 codewords, strings of 32; V.44 2048 codewords, strings of
255, history 4096). `v90m_ec_create`, `v90m_ec_init` and `v90m_ec_reset`
check every value against its range ([api.md](api.md), `v90m_ec_cfg`) and
give `V90M_EINVAL` outside it; `v90m_ec_reset` restarts the endpoint with
a new configuration.

**When to start it.** At connection (`V90M_EV_CONNECTED`, or
`v90m_state.connected` turning 1), with `v90m_ec_reset`. Until then
`getbit` may return 1 (mark) and `putbit` may drop the bit, as the
examples do. When answering and `v90m_state.odp_bypass` is 1 (two V.92
modems that both named LAPM in V.8, V.92 9.3.1), set `detect = 0`: the
caller skips the detection phase.

**The detection phase** (V.42 7.2.1). The originator sends the ODP until
two ADPs are seen, or the answerer's flags (an answerer without the
detection phase), or T400 passes; the answerer sends mark until the ODP,
then the ADP. If T400 passes without an error-correcting peer, the
endpoint goes to `V90M_EC_ASYNC` (start-stop, no correction) with
`fallback` 1, or disconnects (`V90M_EC_R_NO_EC`) with `fallback` 0
(`v42/v42.h`).

**States** (`v90m_ec_state`): `V90M_EC_DETECT`, `V90M_EC_ESTABLISH`,
`V90M_EC_CONNECTED`, `V90M_EC_ASYNC`, `V90M_EC_RELEASING`,
`V90M_EC_DISCONNECTED`; the reason for the last is in
`v90m_ec_status.reason` (`V90M_EC_R_*`). Data moves in
`V90M_EC_CONNECTED` and `V90M_EC_ASYNC`.

**Compression.** V.42bis or V.44 when both ends agree in the XID exchange;
V.44 is preferred where both ends have both. `comp_policy`
(`V90M_COMP_AUTO`, `_ALWAYS`, `_NEVER`) is for our encoder. A decoder
error releases the connection (`V90M_EC_R_COMPRESSION`).
`v90m_ec_get_status` reports what was agreed (`comp_alg`, `comp_tx`,
`comp_rx`, the parameters per direction) and what is in use now (`enc_on`,
`dec_on`, `alg_on`). V.42bis's P1 (`v42bis_p1`) has a ceiling of
`V42COMP_MAX_CODEWORDS` (2048 in this build: the size of the dictionary
arrays); the endpoint never proposes more. Should a direction be agreed
that the encoder or decoder cannot run (only a bug could do it), the
connection is released with `V90M_EC_R_COMPRESSION` rather than run
uncompressed while the far end compresses.

**The XID exchange.** Our XID command gives the HDLC optional functions in
three octets (bit 16 set, bit 17 for the 32-bit FCS), the form real modems
send: some take nothing after a mask of another length, our compression
offer included. An answer has the form of the command it answers, three
octets or four. The V.44 parameters go last, in the user data subfield,
which has no group length (V.42 12.2.1.3). A responder that answers a
smaller window `k` or a shorter `N401` than we proposed for our sending
direction is taken at its word.

**Bounded waits** (a ruling that departs from V.42). V.42, like HDLC,
starts the retransmission count again at each timer recovery (8.4.8): a
peer that answers every poll (RR with F=1) but never acknowledges an I
frame keeps the link up for ever with nothing moving, and so does one that
answers every I frame with a REJ that acknowledges nothing. Here every
wait is bounded: after `n400` recoveries (timer recoveries, and go-backs
on a REJ that acknowledges nothing) with I frames unacknowledged and V(A)
not moving, the next one releases the link with `V90M_EC_R_N400` (about
(`n400` + 1) x `t401_ms`: 11 s with the defaults). With `n400` 1 one
recovery is still made. Acknowledged data starts the count again; a
destructive break, which resets the data path but moves no data, does
not. A peer that says it is busy (RNR) is not counted: V.42 8.4.7 lets
that last, and the program, which stops being able to write, decides.
T403 is off by default (`t403_ms` 0): with nothing unacknowledged a peer
that has gone silently is not noticed by V.42 itself; the modem's loss of
carrier (`V90M_EV_ENDED`) is what tells. Set `t403_ms` to have an idle
link polled.

**Flow control.** `v90m_ec_write` takes what fits (`v90m_ec_write_space`
says how much); `v90m_ec_read` gives what has arrived
(`v90m_ec_read_avail`). In error-corrected mode the far end is held off
when the program does not read, and nothing is dropped; in
`V90M_EC_ASYNC` there is no flow control and receive overruns are counted
(`async_overruns`). `v90m_ec_tx_idle` is 1 when everything written was
acknowledged; `v90m_ec_tx_busy` when a frame is still leaving the
transmitter.

**Retrains and renegotiations.** The modem stops the data
(`V90M_EV_DATA_STOPPED`, `v90m_state.moving` falling to 0) and reports when
it moves again both ways (`V90M_EV_DATA_MOVING`, `moving` back to 1). Pass
both on, within the sample they happen (from the event callback, or by
checking `v90m_get_state` after each sample): `v90m_ec_line_stopped` on the
falling edge, `v90m_ec_line_restored` on the rising one.

- `v90m_ec_line_stopped` notes the frame in flight and begins no new frame
  until the line is back: the modem may take our bits again a round trip
  before data moves both ways, and they are flags then.
- `v90m_ec_line_restored` aborts the frame the stop cut, if it is still
  going (its head went before the stop; an I frame comes again through
  LAPM's poll), opens the next frame with two whole flags (an x2 rate change
  resumes after fill ones, our last flag cut short), and has LAPM poll the
  far end when frames are still unacknowledged. Frames begun after the
  restore are never touched. Without a `v90m_ec_line_stopped` before it,
  only the poll happens.

The receiver for its part takes a frame whose opening flag was lost after a
run of ones when its FCS holds (`v42/v42_hdlc.h`), which is how a U.S.
Robotics x2 client resumes after its rate change. v90d also holds V.42's
timers while the data are stopped (`DESIGN.md`, 4.2).

**The end.** `v90m_ec_release` is orderly (DISC, then wait for UA);
`v90m_ec_abort` stops without a word when the line is gone, in every
state (detection and start-stop mode included), and the state is
`V90M_EC_DISCONNECTED` (`V90M_EC_R_LOCAL`) on return. After
`V90M_EC_DISCONNECTED`, if the line is still up, keep the bits moving for a
few dozen more so that a final UA or DM gets out; data already received
stays readable (`v42/v42.h`). Then the modem's cleardown, where there is
one.

**Breaks** (V.42 12.3.1). `v90m_ec_send_break(ec, opt, len10ms)`, `opt`
one of `V90M_BRK_NONDESTRUCTIVE_INSEQ`, `V90M_BRK_NONDESTRUCTIVE_EXPEDITED`,
`V90M_BRK_DESTRUCTIVE_EXPEDITED`, the length in 10 ms units: `V90M_OK` when
the break is on its way, `V90M_ESTATE` when error control is not up or a
break of ours is still pending. `v90m_ec_get_break` returns 1 with `*opt`
and `*len10ms` when a break came, in its place among the octets read, else
0.

**Memory.** At most `V90M_EC_MEM_SIZE` (50 KiB; `v90m_ec_mem_size()` gives
the exact size, 49792 octets on x86-64).

More on the V.42 code, the compression and the readings taken where the
Recommendations leave a choice: `v42/README.md`.

## A sketch

```c
static int  get_bit(void *u) { (void)u; return ec_on ? v90m_ec_tx_bit(ec) : 1; }
static void put_bit(void *u, int bit) { (void)u; if (ec_on) v90m_ec_rx_bit(ec, bit); }

/* after each block of n samples */
v90m_get_state(m, &st);
if (st.connected && !ec_on) {
    v90m_ec_cfg_default(&c, originator);
    c.detect = !st.odp_bypass;
    v90m_ec_reset(ec, &c);
    ec_on = 1;
}
if (ec_on) v90m_ec_tick(ec, n / 8);
```

The complete programs are in [getting-started.md](getting-started.md).
