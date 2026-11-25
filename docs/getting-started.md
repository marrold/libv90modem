# Getting started

## Build

The library is portable C99 with a plain Makefile; there is nothing to
configure.

```
make                    # build/: libv90modem.a, libv90modem.so, libv90modem.pc, the examples
make check              # the test suite on the host (see testing.md)
```

`make` builds, in `build/`, the static library `libv90modem.a`, the shared
library `libv90modem.so.0.6.1` (with its `.so.0.6` and `.so` links; the
SONAME is `libv90modem.so.0.6`: before 1.0 each 0.x version has its own),
the pkg-config file `libv90modem.pc`, and the two examples
`build/answer_file` and `build/call_loop`. `CC`, `CFLAGS` and `BUILD`
can be set on the command line; the default flags are
`-std=c99 -O2 -Wall -Wextra -Werror`. For a cross build set `CC` and
`BUILD` (`ar`, `ld`, `objcopy`, `nm` and `readelf` are taken from the same prefix):

```
make CC=arm-linux-gcc BUILD=build/arm
```

## Install

```
make install PREFIX=/usr/local      # DESTDIR works too
make uninstall PREFIX=/usr/local
```

This installs `v90modem.h` (`INCDIR`), `libv90modem.a` and the shared
library (`LIBDIR`), `libv90modem.pc` (`PCDIR`, default `LIBDIR/pkgconfig`)
and the `v90m-replay` tool (`BINDIR`). Each of these variables overrides
its part of the layout.

## Link

```
cc -std=c99 prog.c $(pkg-config --cflags --libs libv90modem)              # shared
cc -std=c99 prog.c $(pkg-config --cflags --libs --static libv90modem)     # static (adds -lm)
```

From the source tree without installing:
`cc -std=c99 -Iinclude prog.c build/libv90modem.a -lm`.

The static library defines the `v90m_*` functions and nothing else a
program could clash with; its internal symbols are renamed `v90m__i_*`, so
a program must not define names that begin with `v90m__i_`. Only
`include/v90modem.h` is the API. It is not stable before version 1.0:
rebuild against the new header with every version (`CHANGELOG.md`).

## The shape of a program

1. Fill a `v90m_cfg` (`v90m_cfg_default`, then role, modulations, the bit
   callbacks).
2. `v90m_create` (the one allocation), or `v90m_init` on memory of your
   own (`V90M_MEM_SIZE` octets, aligned as `v90m_align`).
3. Every block of line samples: `v90m_process_g711`, `_lin_rx` or
   `_linear` with what was received, and send what it gives back. Any
   block length works.
4. Follow the call with `on_event` or by reading `v90m_get_state` after
   each block.
5. Once connected, data moves through `getbit` / `putbit`, called from
   inside the process call. Put a V.42 endpoint (`v90m_ec_*`) or a
   start-stop framer (`v90m_async_*`) there ([error-control.md](error-control.md)).
6. `v90m_reset` for the next call on the same memory, `v90m_destroy` at
   the end.

The examples below were compiled against the header and the static
library with `-std=c99 -Wall -Wextra -Werror` (gcc and clang) and run
against each other through a test relay. They read and write the line on
stdin and stdout, 80 samples (10 ms) per block; replace `codec_read` /
`codec_write` (or the `fread` / `fwrite`) with your codec.

## The smallest program

The one in the top-level README: an answering modem with V.92 on offer
that prints the connection, rate changes and the end, and moves no data
(`getbit` sends ones, `putbit` drops what arrives).

```c
#include <stdio.h>
#include <v90modem.h>

static int  get_bit(void *user) { (void)user; return 1; }      /* the next bit to send */
static void put_bit(void *user, int bit) { (void)user; (void)bit; }   /* a bit received */

int main(void)
{
    v90m_cfg cfg;
    v90m_state st, was = { 0 };
    uint8_t rx[80], tx[80];             /* 10 ms of G.711 codewords each way */
    int err;
    v90m_t *m;

    v90m_cfg_default(&cfg);             /* answering; V.90, V.34, V.32bis, V.22bis, V.22, V.21 */
    cfg.role = V90M_ROLE_ANSWER;        /* or V90M_ROLE_ORIGINATE, see below */
    cfg.modulations |= V90M_MOD_V92;
    cfg.getbit = get_bit;
    cfg.putbit = put_bit;
    if (!(m = v90m_create(&cfg, &err))) { fprintf(stderr, "error %d\n", err); return 1; }

    while (fread(rx, 1, sizeof rx, stdin) == sizeof rx) {       /* what the line brought */
        v90m_process_g711(m, rx, tx, sizeof rx);
        fwrite(tx, 1, sizeof tx, stdout);                       /* what to send */
        fflush(stdout);
        v90m_get_state(m, &st);
        if (st.connected && !was.connected)
            fprintf(stderr, "CONNECT %d/%d, modulation %d\n", st.tx_bps, st.rx_bps, (int)st.modulation);
        else if (st.connected && (st.tx_bps != was.tx_bps || st.rx_bps != was.rx_bps))
            fprintf(stderr, "rates now %d/%d\n", st.tx_bps, st.rx_bps);
        if (st.end != V90M_END_NONE) { fprintf(stderr, "ended: %d\n", (int)st.end); break; }
        was = st;
    }
    v90m_destroy(m);
    return 0;
}
```

## A minimal answering program

Answers one call with the default modulations, puts a V.42 endpoint on
the bit pump from the moment of connection, and echoes every octet back.
It uses the `on_event` callback; the events are reported from inside
`v90m_process_g711`, after the sample at which they happened.

- `odp_bypass` (V.92 9.3.1): two V.92 modems that both named LAPM in V.8
  skip V.42's detection phase, so the endpoint starts with `detect = 0`.
- `V90M_EV_DATA_STOPPED` and `V90M_EV_DATA_MOVING` around a retrain or
  rate renegotiation are passed to V.42 with `v90m_ec_line_stopped` and
  `v90m_ec_line_restored`, from the callback, so within the sample they
  happen ([error-control.md](error-control.md)).
- `v90m_ec_tick` gets the milliseconds that went by (10 per block here).

```c
/* answer_min.c: answer one call, V.42 on the bit pump, echo every octet back.
 * The line here is stdin / stdout: 80 G.711 codewords (mu-law) per 10 ms
 * block in, 80 out. Replace codec_read / codec_write with your codec. */
#include <stdio.h>
#include <v90modem.h>

#define BLOCK 80                        /* 10 ms at 8000 samples per second */

static int codec_read(uint8_t *rx, size_t n)  { return fread(rx, 1, n, stdin) == n; }
static void codec_write(const uint8_t *tx, size_t n) { fwrite(tx, 1, n, stdout); fflush(stdout); }

static v90m_t    *modem;
static v90m_ec_t *ec;
static int        ec_on;                /* V.42 runs from the connection on */

static int  get_bit(void *u) { (void)u; return ec_on ? v90m_ec_tx_bit(ec) : 1; }
static void put_bit(void *u, int bit) { (void)u; if (ec_on) v90m_ec_rx_bit(ec, bit); }

static void on_event(void *u, v90m_event ev)
{
    v90m_state st;
    (void)u;
    v90m_get_state(modem, &st);
    switch (ev) {
    case V90M_EV_CONNECTED: {
        v90m_ec_cfg c;
        v90m_ec_cfg_default(&c, 0);     /* 0: the answering end */
        c.detect = !st.odp_bypass;      /* V.92 9.3.1: no ODP / ADP */
        v90m_ec_reset(ec, &c);
        ec_on = 1;
        fprintf(stderr, "CONNECT %d/%d\n", st.tx_bps, st.rx_bps);
        break;
    }
    case V90M_EV_DATA_STOPPED:
        if (ec_on) v90m_ec_line_stopped(ec);
        break;
    case V90M_EV_DATA_MOVING:
        if (ec_on) v90m_ec_line_restored(ec);
        break;
    case V90M_EV_ENDED:
        fprintf(stderr, "ended: %d\n", (int)st.end);
        break;
    default:
        break;
    }
}

int main(void)
{
    v90m_cfg cfg;
    v90m_ec_cfg ecc;
    v90m_state st;
    uint8_t rx[BLOCK], tx[BLOCK], buf[256];
    int err;

    v90m_cfg_default(&cfg);             /* answering, default modulations */
    cfg.getbit = get_bit;
    cfg.putbit = put_bit;
    cfg.on_event = on_event;
    v90m_ec_cfg_default(&ecc, 0);
    if (!(ec = v90m_ec_create(&ecc, &err)) || !(modem = v90m_create(&cfg, &err))) {
        fprintf(stderr, "error %d\n", err);
        return 1;
    }
    while (codec_read(rx, BLOCK)) {
        v90m_process_g711(modem, rx, tx, BLOCK);
        codec_write(tx, BLOCK);
        if (ec_on) {
            size_t n;
            v90m_ec_tick(ec, 10);       /* 10 ms of line went by */
            n = v90m_ec_read(ec, buf, v90m_ec_write_space(ec) < sizeof buf ? v90m_ec_write_space(ec) : sizeof buf);
            v90m_ec_write(ec, buf, n);  /* echo it back */
        }
        v90m_get_state(modem, &st);
        if (st.end != V90M_END_NONE) break;
    }
    v90m_destroy(modem);
    v90m_ec_destroy(ec);
    return 0;
}
```

## A minimal calling program

Calls with V.92, V.90, V.34 and V.22 offered. It is started once the far
end has answered: dialling and call progress are the program's
([line-interface.md](line-interface.md)). It uses `v90m_process_linear`,
because the V.90 / V.92 calling side needs its received samples linear
(`v90m_process_lin_rx` would do as well), and polls the state instead of
using `on_event`. Once V.42 is up it sends a line, prints what comes back
and clears down; where the modulation has no cleardown signal
(`V90M_ESTATE`) it just stops.

The calling side has been tried in simulation and against this library's
own answerer only, never against a real modem or server
([calling.md](calling.md)).

```c
/* call_min.c: call once the far end has answered, V.92 / V.90 / V.34 / V.22
 * offered, V.42 on the bit pump; send a line, print what comes back, clear
 * down. The line here is stdin / stdout: 80 linear 16-bit samples per 10 ms
 * block each way (V.90 / V.92 calling need the received samples linear).
 * Dialling and call progress happen before this program starts. */
#include <stdio.h>
#include <string.h>
#include <v90modem.h>

#define BLOCK 80

static int codec_read(int16_t *rx, size_t n)  { return fread(rx, sizeof *rx, n, stdin) == n; }
static void codec_write(const int16_t *tx, size_t n) { fwrite(tx, sizeof *tx, n, stdout); fflush(stdout); }

static v90m_ec_t *ec;
static int        ec_on;

static int  get_bit(void *u) { (void)u; return ec_on ? v90m_ec_tx_bit(ec) : 1; }
static void put_bit(void *u, int bit) { (void)u; if (ec_on) v90m_ec_rx_bit(ec, bit); }

int main(void)
{
    static const char hello[] = "hello\r\n";
    v90m_cfg cfg;
    v90m_ec_cfg ecc;
    v90m_t *m;
    v90m_state st;
    int16_t rx[BLOCK], tx[BLOCK];
    uint8_t buf[256];
    int err, sent = 0, was_moving = 0;

    v90m_cfg_default(&cfg);
    cfg.role = V90M_ROLE_ORIGINATE;
    cfg.modulations = V90M_MOD_V92 | V90M_MOD_V90 | V90M_MOD_V34 | V90M_MOD_V22;
    cfg.getbit = get_bit;
    cfg.putbit = put_bit;               /* no on_event: the state is polled */
    v90m_ec_cfg_default(&ecc, 1);       /* 1: the calling end */
    if (!(ec = v90m_ec_create(&ecc, &err)) || !(m = v90m_create(&cfg, &err))) {
        fprintf(stderr, "error %d\n", err);
        return 1;
    }
    while (codec_read(rx, BLOCK)) {
        v90m_process_linear(m, rx, tx, BLOCK);
        codec_write(tx, BLOCK);
        v90m_get_state(m, &st);
        if (st.end != V90M_END_NONE) {
            fprintf(stderr, "ended: %d\n", (int)st.end);
            break;
        }
        if (st.connected && !ec_on) {
            v90m_ec_reset(ec, &ecc);
            ec_on = 1;
        }
        if (!ec_on) continue;
        if (st.moving && !was_moving) v90m_ec_line_restored(ec);
        was_moving = st.moving;
        v90m_ec_tick(ec, 10);
        if (!sent && v90m_ec_state(ec) == V90M_EC_CONNECTED)
            sent = v90m_ec_write(ec, (const uint8_t *)hello, strlen(hello)) == strlen(hello);
        if (v90m_ec_read_avail(ec)) {
            size_t n = v90m_ec_read(ec, buf, sizeof buf);
            fwrite(buf, 1, n, stderr);
            if (v90m_cleardown(m) != V90M_OK) break;    /* no cleardown signal: just stop */
        }
    }
    v90m_destroy(m);
    v90m_ec_destroy(ec);
    return 0;
}
```

## The examples in the tree

- `examples/call_loop.c`: a calling modem and an answering modem back to
  back through a 4-wire line in memory, linear both ways, a start-stop
  framer on each bit pump; each end sends a line of text, then the caller
  clears down. `-m v92|v90|v34|v32|v22|bell212|bell103` picks what the
  caller offers. `make check` runs it.
- `examples/answer_file.c`: answers a recorded call (the format of
  `tools/README.md`) and prints the connect line.

## Memory of your own

Nothing is allocated if each object is placed in memory the program
provides:

```c
static union { v90m_align a; uint8_t b[V90M_MEM_SIZE]; } mem;
v90m_t *m = v90m_init(&mem, sizeof mem, &cfg, &err);
```

`V90M_EC_MEM_SIZE` and `V90M_ASYNC_MEM_SIZE` with `v90m_ec_init` and
`v90m_async_init` do the same for the V.42 endpoint and the start-stop
framer. The sizes are upper bounds for this version on every platform;
the exact need is `v90m_mem_size()` and the like, and the init functions
check the length given (`V90M_ENOMEM` when it is too small).
