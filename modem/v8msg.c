#include "v8msg.h"

#define TAG_CALLF  0x01   /* b0..b3 = 1000 */
#define TAG_MODN   0x05   /* 1010 */
#define TAG_PROT   0x0A   /* 0101 */
#define TAG_ACCESS 0x0D   /* 1011 */
#define TAG_PCM    0x07   /* 1110 */

#define IS_EXT(o) (((o) & 0x38) == 0x10)

static uint8_t cat(uint8_t tag, uint8_t opt) { return (uint8_t)(tag | ((opt & 7) << 5)); }

int v8_encode(const v8_menu_t *m, uint8_t *oct, int max)
{
    int n = 0;
    unsigned md = m->modes;
    if (max < 7) return 0;
    oct[n++] = cat(TAG_CALLF, m->call_function);
    if (m->n_mod_octets >= 1)
        oct[n++] = cat(TAG_MODN, (uint8_t)((m->has_pcm ? 1 : 0) | ((md >> 1) & 1) << 1 | ((md >> 2) & 1) << 2));
    if (m->n_mod_octets >= 2)
        oct[n++] = (uint8_t)(0x10 | ((md >> 3) & 7) | ((md >> 6) & 3) << 6);
    if (m->n_mod_octets >= 3)
        oct[n++] = (uint8_t)(0x10 | ((md >> 8) & 7) | ((md >> 11) & 3) << 6);
    if (m->has_prot)   oct[n++] = cat(TAG_PROT, m->prot);
    if (m->has_pcm)    oct[n++] = cat(TAG_PCM, m->pcm);
    if (m->has_access) oct[n++] = cat(TAG_ACCESS, m->access);
    return n;
}

int v8_decode(const uint8_t *oct, int n, v8_menu_t *m)
{
    int i, cur = -1, ext = 0;
    m->call_function = 0; m->modes = 0; m->n_mod_octets = 0;
    m->has_pcm = m->pcm = 0; m->has_prot = m->prot = 0;
    m->has_access = m->access = 0;
    if (n < 1 || (oct[0] & 0x1F) != TAG_CALLF) return -1;
    for (i = 0; i < n; i++) {
        uint8_t o = oct[i];
        if (IS_EXT(o)) {
            ext++;
            if (cur == TAG_MODN && ext == 1) {
                m->modes |= (uint16_t)((o & 7) << 3 | ((o >> 6) & 3) << 6);
                m->n_mod_octets = 2;
            } else if (cur == TAG_MODN && ext == 2) {
                m->modes |= (uint16_t)((o & 7) << 8 | ((o >> 6) & 3) << 11);
                m->n_mod_octets = 3;
            }
            continue;
        }
        if (o & 0x10) { cur = -1; continue; }   /* neither category nor extension: ignore */
        cur = o & 0x0F; ext = 0;
        switch (cur) {
        case TAG_CALLF:  m->call_function = (uint8_t)(o >> 5); break;
        case TAG_MODN:   m->modes |= (uint16_t)(((o >> 6) & 3) << 1); m->n_mod_octets = 1; break;
        case TAG_PROT:   m->has_prot = 1; m->prot = (uint8_t)(o >> 5); break;
        case TAG_PCM:    m->has_pcm = 1; m->pcm = (uint8_t)(o >> 5); break;
        case TAG_ACCESS: m->has_access = 1; m->access = (uint8_t)(o >> 5); break;
        default: break;                          /* reserved category: ignore */
        }
    }
    return 0;
}

static int put_char(uint8_t c, uint8_t *bits, int k)
{
    int i;
    bits[k++] = 0;
    for (i = 0; i < 8; i++) bits[k++] = (uint8_t)((c >> i) & 1);
    bits[k++] = 1;
    return k;
}

int v8_frame(uint8_t sync, const uint8_t *oct, int n, uint8_t *bits, int max)
{
    int k = 0, i;
    if (max < 20 + 10 * n) return 0;
    for (i = 0; i < 10; i++) bits[k++] = 1;
    k = put_char(sync, bits, k);
    for (i = 0; i < n; i++) k = put_char(oct[i], bits, k);
    return k;
}

void v8_build_jm(const v8_menu_t *cm, const v8_menu_t *local, v8_menu_t *jm)
{
    jm->call_function = local->call_function;
    jm->n_mod_octets = cm->n_mod_octets;
    jm->modes = 0;
    jm->has_pcm = jm->pcm = 0;
    jm->has_prot = jm->prot = 0;
    jm->has_access = jm->access = 0;
    if (cm->call_function != local->call_function) return;   /* 8.2.3: zeros */

    jm->modes = (uint16_t)(cm->modes & local->modes);
    /* A digital answerer pairs with an analogue caller (V.90 9.1.1); PCM also
     * needs V.34 common (V.8 6.3) since V.90 start-up/upstream is V.34. */
    if (cm->has_pcm && (cm->pcm & V8_PCM_ANALOG) && (local->pcm & V8_PCM_DIGITAL)
        && (jm->modes & V8_MOD_V34) && cm->n_mod_octets >= 1) {
        jm->has_pcm = 1; jm->pcm = V8_PCM_DIGITAL;
    } else if (local->pcm & V8_PCM_NO_V34_DATA) {
        jm->modes &= (uint16_t)~V8_MOD_V34;
    }
    if (cm->has_prot && cm->prot == V8_PROT_LAPM && local->prot == V8_PROT_LAPM) {
        jm->has_prot = 1; jm->prot = V8_PROT_LAPM;
    }
    if (jm->has_pcm || (cm->has_access && (cm->access & V8_ACC_CALL_CELL))) {
        jm->has_access = 1;
        jm->access = (uint8_t)((local->access & (V8_ACC_DIGITAL | V8_ACC_ANS_CELL))
                               | (cm->has_access ? (cm->access & V8_ACC_CALL_CELL) : 0));
    }
}

static int lowest_mode(const v8_menu_t *jm)
{
    int i;
    for (i = 1; i <= 12; i++) if (jm->modes & (1u << i)) return i;
    return 0;
}

int v8_selected(const v8_menu_t *jm)
{
    if (jm->has_pcm && (jm->pcm & (V8_PCM_DIGITAL | V8_PCM_ANALOG))) return V8_SEL_V90;
    return lowest_mode(jm);
}

int v8_selected_caller(const v8_menu_t *jm)
{
    if (jm->has_pcm && (jm->pcm & V8_PCM_DIGITAL)) return V8_SEL_V90;
    return lowest_mode(jm);
}
