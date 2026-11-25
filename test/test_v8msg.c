/* Tests for V.8 CM/JM coding and framing. */
#include <string.h>
#include "test.h"
#include "../modem/v8msg.h"

static int has_flag(const uint8_t *b, int n)
{
    static const uint8_t f[8] = {0,1,1,1,1,1,1,0};
    int i;
    for (i = 0; i + 8 <= n; i++) if (!memcmp(b + i, f, 8)) return 1;
    return 0;
}

int main(void)
{
    uint8_t oct[V8_MAX_OCTETS], bits[400];
    v8_menu_t m, d, local, jm;
    int n, nb, i;

    /* a V.34 data caller with LAPM: the well-known menu C1 45 13 90 2A */
    memset(&m, 0, sizeof m);
    m.call_function = V8_CALLF_DATA;
    m.modes = V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21;
    m.n_mod_octets = 3;
    m.has_prot = 1; m.prot = V8_PROT_LAPM;
    n = v8_encode(&m, oct, V8_MAX_OCTETS);
    CHECK_EQ_I(n, 5);
    CHECK_EQ_I(oct[0], 0xC1);
    CHECK_EQ_I(oct[1], 0x45);
    CHECK_EQ_I(oct[2], 0x13);
    CHECK_EQ_I(oct[3], 0x90);
    CHECK_EQ_I(oct[4], 0x2A);

    /* a V.90 analogue caller: modn0 b5 set, pcm0 + access0 present */
    m.has_pcm = 1; m.pcm = V8_PCM_ANALOG;
    m.has_access = 1; m.access = 0;
    n = v8_encode(&m, oct, V8_MAX_OCTETS);
    CHECK_EQ_I(n, 7);
    CHECK_EQ_I(oct[1], 0x65);            /* PCM-present flag in modn0 */
    CHECK_EQ_I(oct[5], 0x27);            /* pcm0: tag 0111, b5 */
    CHECK_EQ_I(oct[6], 0x0D);            /* access0: tag 1011, analogue */

    /* decode round-trip */
    CHECK_EQ_I(v8_decode(oct, n, &d), 0);
    CHECK_EQ_I(d.call_function, V8_CALLF_DATA);
    CHECK_EQ_I(d.modes, m.modes);
    CHECK_EQ_I(d.n_mod_octets, 3);
    CHECK(d.has_pcm && d.pcm == V8_PCM_ANALOG);
    CHECK(d.has_prot && d.prot == V8_PROT_LAPM);
    CHECK(d.has_access && d.access == 0);

    /* unknown category (T.66 tag 0011... = 0x0C) and its extension are ignored */
    {
        uint8_t o2[6] = {0xC1, 0x0C, 0x13, 0x05 | 0x40, 0x13, 0x90};
        CHECK_EQ_I(v8_decode(o2, 6, &d), 0);
        CHECK_EQ_I(d.modes, V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21);
    }
    /* first category must be the call function */
    CHECK_EQ_I(v8_decode(oct + 1, n - 1, &d), -1);

    /* framing: 10 ones, sync 0000001111, then start + LSB-first octet + stop */
    nb = v8_frame(V8_SYNC_CM_JM, oct, 1, bits, 400);
    CHECK_EQ_I(nb, 30);
    {
        static const uint8_t want[30] = {1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,1,1,1,1,
                                         0, 1,0,0,0,0,0,1,1, 1};
        CHECK(!memcmp(bits, want, 30));
    }

    /* no HDLC flag in any repeated sequence, across all mode/option menus */
    {
        int bad = 0; unsigned mm;
        for (mm = 0; mm < 8192; mm++) {
            int rep;
            memset(&m, 0, sizeof m);
            m.call_function = (uint8_t)(mm & 7);
            m.modes = (uint16_t)(mm & ~1u);
            m.n_mod_octets = 3;
            m.has_pcm = 1; m.pcm = (uint8_t)((mm >> 3) & 7);
            m.has_prot = 1; m.prot = (uint8_t)((mm >> 6) & 7);
            m.has_access = 1; m.access = (uint8_t)((mm >> 9) & 7);
            n = v8_encode(&m, oct, V8_MAX_OCTETS);
            nb = 0;
            for (rep = 0; rep < 2; rep++)
                nb += v8_frame(V8_SYNC_CM_JM, oct, n, bits + nb, 400 - nb);
            if (has_flag(bits, nb)) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }

    /* ---- JM derivation ---- */
    memset(&local, 0, sizeof local);
    local.call_function = V8_CALLF_DATA;
    local.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21;
    local.pcm = V8_PCM_DIGITAL;
    local.prot = V8_PROT_LAPM;
    local.access = V8_ACC_DIGITAL;

    /* V.90 analogue caller meets our digital modem */
    memset(&m, 0, sizeof m);
    m.call_function = V8_CALLF_DATA;
    m.modes = V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22;
    m.n_mod_octets = 3;
    m.has_pcm = 1; m.pcm = V8_PCM_ANALOG;
    m.has_access = 1;
    m.has_prot = 1; m.prot = V8_PROT_LAPM;
    v8_build_jm(&m, &local, &jm);
    CHECK_EQ_I(jm.call_function, V8_CALLF_DATA);
    CHECK_EQ_I(jm.modes, V8_MOD_V34 | V8_MOD_V22);
    CHECK_EQ_I(jm.n_mod_octets, 3);
    CHECK(jm.has_pcm && jm.pcm == V8_PCM_DIGITAL);
    CHECK(jm.has_access && (jm.access & V8_ACC_DIGITAL));
    CHECK(jm.has_prot && jm.prot == V8_PROT_LAPM);
    CHECK_EQ_I(v8_selected(&jm), V8_SEL_V90);

    /* plain V.34 caller (no PCM category): no PCM in JM, lowest item wins */
    m.has_pcm = 0; m.has_access = 0; m.has_prot = 0;
    v8_build_jm(&m, &local, &jm);
    CHECK(!jm.has_pcm && !jm.has_prot && !jm.has_access);
    CHECK_EQ_I(v8_selected(&jm), 1);     /* V.34 */

    /* V.22bis-only caller */
    m.modes = V8_MOD_V22; 
    v8_build_jm(&m, &local, &jm);
    CHECK_EQ_I(v8_selected(&jm), 4);

    /* PCM needs V.34 on both sides (V.8 6.3): without it, no PCM in JM */
    m.modes = V8_MOD_V22; m.has_pcm = 1; m.pcm = V8_PCM_ANALOG; m.has_access = 1;
    v8_build_jm(&m, &local, &jm);
    CHECK(!jm.has_pcm);
    CHECK_EQ_I(v8_selected(&jm), 4);

    /* nothing in common: same number of mod octets, all zero */
    m.modes = V8_MOD_V32; m.has_pcm = 0; m.n_mod_octets = 2;
    v8_build_jm(&m, &local, &jm);
    CHECK_EQ_I(jm.modes, 0);
    CHECK_EQ_I(jm.n_mod_octets, 2);
    CHECK_EQ_I(v8_selected(&jm), 0);

    /* unavailable call function: zero modes, no PCM (V.8 8.2.3) */
    m.call_function = 4; m.modes = V8_MOD_V34; m.n_mod_octets = 3;
    m.has_pcm = 1; m.pcm = V8_PCM_ANALOG;
    v8_build_jm(&m, &local, &jm);
    CHECK_EQ_I(jm.call_function, V8_CALLF_DATA);
    CHECK_EQ_I(jm.modes, 0);
    CHECK(!jm.has_pcm);

    /* the calling (analogue) side: V.90 / V.92 needs a digital modem at the
     * far end, so a JM naming "PCM analogue" only means V.34 */
    {
        v8_menu_t jm_v34_only;
        memset(&jm, 0, sizeof jm);
        jm.call_function = V8_CALLF_DATA; jm.n_mod_octets = 3;
        jm.modes = V8_MOD_V34 | V8_MOD_V22;
        jm_v34_only = jm;
        jm.has_pcm = 1; jm.pcm = V8_PCM_ANALOG;
        CHECK_EQ_I(v8_selected_caller(&jm), v8_selected(&jm_v34_only));
        CHECK_EQ_I(v8_selected_caller(&jm), V8_SEL_V34);
        jm.pcm = V8_PCM_DIGITAL;
        CHECK_EQ_I(v8_selected_caller(&jm), v8_selected(&jm));
        CHECK_EQ_I(v8_selected_caller(&jm), V8_SEL_V90);
        jm.pcm = V8_PCM_DIGITAL | V8_PCM_ANALOG;
        CHECK_EQ_I(v8_selected_caller(&jm), V8_SEL_V90);
        jm.has_pcm = 0;
        CHECK_EQ_I(v8_selected_caller(&jm), V8_SEL_V34);
        jm.modes = 0; jm.has_pcm = 1; jm.pcm = V8_PCM_ANALOG;
        CHECK_EQ_I(v8_selected_caller(&jm), 0);
    }
    (void)i;

    T_REPORT("v8msg");
}
