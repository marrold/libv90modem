/* libv90modem: the V.42 endpoint's configuration and status, each field
 * listed once. The public structs (include/v90modem.h) are written out
 * there and keep their layout; these lists generate what is done field by
 * field: the range check, the copy into the code underneath (cfg_in), the
 * defaults back out (cfg_out), the status out (status_out). A field added
 * to a struct is added here once. Never installed; not part of the API. */
#ifndef V90MODEM_FIELDS_H
#define V90MODEM_FIELDS_H
#include <limits.h>
#include <string.h>
#include "../include/v90modem.h"
#include "../v42/v42.h"

/* X(member of v90m_ec_cfg, member of v42ec_cfg_t, lowest, highest): the
 * ranges of the header; a value the code underneath would clamp to another
 * is refused (0 only where it means the default or off) */
#define V90M_EC_CFG_FIELDS(X) \
    X(originator,       originator,       0, 1) \
    X(detect,           detect,           0, 1) \
    X(fallback,         fallback,         0, 1) \
    X(t400_ms,          t400_ms,          0, INT_MAX) \
    X(establish_ms,     establish_ms,     0, INT_MAX) \
    X(comp_policy,      comp_policy,      V90M_COMP_AUTO, V90M_COMP_NEVER) \
    X(lapm.use_xid,     lapm.use_xid,     0, 1) \
    X(lapm.k_tx,        lapm.k_tx,        1, V42_MAX_K) \
    X(lapm.k_rx,        lapm.k_rx,        1, 127) \
    X(lapm.n401_tx,     lapm.n401_tx,     1, V42_MAX_N401) \
    X(lapm.n401_rx,     lapm.n401_rx,     1, V42_MAX_N401) \
    X(lapm.fcs32,       lapm.fcs32,       0, 1) \
    X(lapm.v42bis_tx,   lapm.comp_tx,     0, 1) \
    X(lapm.v42bis_rx,   lapm.comp_rx,     0, 1) \
    X(lapm.v42bis_p1,   lapm.comp_p1,     512, V42COMP_MAX_CODEWORDS) \
    X(lapm.v42bis_p2,   lapm.comp_p2,     6, 250) \
    X(lapm.v44_tx,      lapm.v44_tx,      0, 1) \
    X(lapm.v44_rx,      lapm.v44_rx,      0, 1) \
    X(lapm.v44_p1_tx,   lapm.v44_p1_tx,   256, V44COMP_MAX_CODEWORDS) \
    X(lapm.v44_p1_rx,   lapm.v44_p1_rx,   256, V44COMP_MAX_CODEWORDS) \
    X(lapm.v44_p2_tx,   lapm.v44_p2_tx,   32, 255) \
    X(lapm.v44_p2_rx,   lapm.v44_p2_rx,   32, 255) \
    X(lapm.v44_p3_tx,   lapm.v44_p3_tx,   512, V44COMP_MAX_HISTORY) \
    X(lapm.v44_p3_rx,   lapm.v44_p3_rx,   512, V44COMP_MAX_HISTORY) \
    X(lapm.t401_ms,     lapm.t401_ms,     1, INT_MAX) \
    X(lapm.t403_ms,     lapm.t403_ms,     0, INT_MAX) \
    X(lapm.n400,        lapm.n400,        1, INT_MAX)

/* X(member of both v90m_ec_status and v42ec_status_t): all of the public
 * status but enc_on, dec_on and alg_on, which are the endpoint's own */
#define V90M_EC_STATUS_FIELDS(X) \
    X(state) X(reason) \
    X(k_tx) X(k_rx) X(n401_tx) X(n401_rx) \
    X(comp_alg) X(comp_tx) X(comp_rx) \
    X(comp_n2) X(comp_n7) \
    X(v44_n2_tx) X(v44_n7_tx) X(v44_n8_tx) X(v44_n2_rx) X(v44_n7_rx) X(v44_n8_rx) \
    X(fcs32) \
    X(tx_queued) X(rx_avail) X(unacked) \
    X(rx_frames) X(rx_frames_bad) \
    X(tx_i) X(tx_i_retx) X(rx_i) X(rx_i_dropped) \
    X(tx_rej) X(rx_rej) X(t401_expiries) X(violations) \
    X(comp_in) X(comp_out) X(decomp_in) X(decomp_out) \
    X(async_overruns) X(async_framing)

/* every member of v90m_ec_cfg is an int on the list */
#define V90M_EC_FIELD_ONE(pub, in, lo, hi) + 1
typedef char v90m_check_ec_cfg_fields[(0 V90M_EC_CFG_FIELDS(V90M_EC_FIELD_ONE)) * sizeof(int) == sizeof(v90m_ec_cfg) ? 1 : -1];
#undef V90M_EC_FIELD_ONE

static inline int ec_in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }

/* V90M_OK, or V90M_EINVAL for a value outside its range */
static inline int check_ec_cfg(const v90m_ec_cfg *c)
{
#define V90M_EC_FIELD_CHECK(pub, in, lo, hi) && ec_in_range(c->pub, lo, hi)
    return (1 V90M_EC_CFG_FIELDS(V90M_EC_FIELD_CHECK)) ? V90M_OK : V90M_EINVAL;
#undef V90M_EC_FIELD_CHECK
}

static inline void cfg_in(v42ec_cfg_t *o, const v90m_ec_cfg *c)
{
    memset(o, 0, sizeof *o);
#define V90M_EC_FIELD_IN(pub, in, lo, hi) o->in = c->pub;
    V90M_EC_CFG_FIELDS(V90M_EC_FIELD_IN)
#undef V90M_EC_FIELD_IN
}

static inline void cfg_out(v90m_ec_cfg *c, const v42ec_cfg_t *i)
{
    memset(c, 0, sizeof *c);
#define V90M_EC_FIELD_OUT(pub, in, lo, hi) c->pub = i->in;
    V90M_EC_CFG_FIELDS(V90M_EC_FIELD_OUT)
#undef V90M_EC_FIELD_OUT
}

/* s from v42ec_get_status(e) */
static inline void status_out(v90m_ec_status *st, const v42ec_status_t *s, const v42ec_t *e)
{
    memset(st, 0, sizeof *st);
#define V90M_EC_FIELD_STATUS(f) st->f = s->f;
    V90M_EC_STATUS_FIELDS(V90M_EC_FIELD_STATUS)
#undef V90M_EC_FIELD_STATUS
    st->enc_on = e->comp_tx; st->dec_on = e->comp_rx; st->alg_on = e->comp_alg;
}

#endif
