/* HDLC bit-level framing for V.42 LAPM (ITU-T V.42 clause 8.1). */
#include <string.h>
#include "v42_hdlc.h"

#define FLAG 0x7E

/* ------------------------------------------------------------------- FCS */
/* Both generators are run least significant bit first, which is the order
 * the bits go on the line (8.1.2.2), so the register is the bit reversal of
 * the polynomial arithmetic written out in 8.1.1.6. One octet at a time:
 * entry i of a table is register i after eight steps of the generator with
 * nothing more shifted in (polynomial 0x8408, reversed 0x1021; 0xEDB88320,
 * reversed 0x04C11DB7), which is what the register's low octet, the
 * octet's bits added in, contributes; the high bits move down by eight.
 * v42/test/test_fcs checks them against the eight steps. */
static const uint16_t fcs16_tab[256] = {
    0x0000u, 0x1189u, 0x2312u, 0x329Bu, 0x4624u, 0x57ADu, 0x6536u, 0x74BFu,
    0x8C48u, 0x9DC1u, 0xAF5Au, 0xBED3u, 0xCA6Cu, 0xDBE5u, 0xE97Eu, 0xF8F7u,
    0x1081u, 0x0108u, 0x3393u, 0x221Au, 0x56A5u, 0x472Cu, 0x75B7u, 0x643Eu,
    0x9CC9u, 0x8D40u, 0xBFDBu, 0xAE52u, 0xDAEDu, 0xCB64u, 0xF9FFu, 0xE876u,
    0x2102u, 0x308Bu, 0x0210u, 0x1399u, 0x6726u, 0x76AFu, 0x4434u, 0x55BDu,
    0xAD4Au, 0xBCC3u, 0x8E58u, 0x9FD1u, 0xEB6Eu, 0xFAE7u, 0xC87Cu, 0xD9F5u,
    0x3183u, 0x200Au, 0x1291u, 0x0318u, 0x77A7u, 0x662Eu, 0x54B5u, 0x453Cu,
    0xBDCBu, 0xAC42u, 0x9ED9u, 0x8F50u, 0xFBEFu, 0xEA66u, 0xD8FDu, 0xC974u,
    0x4204u, 0x538Du, 0x6116u, 0x709Fu, 0x0420u, 0x15A9u, 0x2732u, 0x36BBu,
    0xCE4Cu, 0xDFC5u, 0xED5Eu, 0xFCD7u, 0x8868u, 0x99E1u, 0xAB7Au, 0xBAF3u,
    0x5285u, 0x430Cu, 0x7197u, 0x601Eu, 0x14A1u, 0x0528u, 0x37B3u, 0x263Au,
    0xDECDu, 0xCF44u, 0xFDDFu, 0xEC56u, 0x98E9u, 0x8960u, 0xBBFBu, 0xAA72u,
    0x6306u, 0x728Fu, 0x4014u, 0x519Du, 0x2522u, 0x34ABu, 0x0630u, 0x17B9u,
    0xEF4Eu, 0xFEC7u, 0xCC5Cu, 0xDDD5u, 0xA96Au, 0xB8E3u, 0x8A78u, 0x9BF1u,
    0x7387u, 0x620Eu, 0x5095u, 0x411Cu, 0x35A3u, 0x242Au, 0x16B1u, 0x0738u,
    0xFFCFu, 0xEE46u, 0xDCDDu, 0xCD54u, 0xB9EBu, 0xA862u, 0x9AF9u, 0x8B70u,
    0x8408u, 0x9581u, 0xA71Au, 0xB693u, 0xC22Cu, 0xD3A5u, 0xE13Eu, 0xF0B7u,
    0x0840u, 0x19C9u, 0x2B52u, 0x3ADBu, 0x4E64u, 0x5FEDu, 0x6D76u, 0x7CFFu,
    0x9489u, 0x8500u, 0xB79Bu, 0xA612u, 0xD2ADu, 0xC324u, 0xF1BFu, 0xE036u,
    0x18C1u, 0x0948u, 0x3BD3u, 0x2A5Au, 0x5EE5u, 0x4F6Cu, 0x7DF7u, 0x6C7Eu,
    0xA50Au, 0xB483u, 0x8618u, 0x9791u, 0xE32Eu, 0xF2A7u, 0xC03Cu, 0xD1B5u,
    0x2942u, 0x38CBu, 0x0A50u, 0x1BD9u, 0x6F66u, 0x7EEFu, 0x4C74u, 0x5DFDu,
    0xB58Bu, 0xA402u, 0x9699u, 0x8710u, 0xF3AFu, 0xE226u, 0xD0BDu, 0xC134u,
    0x39C3u, 0x284Au, 0x1AD1u, 0x0B58u, 0x7FE7u, 0x6E6Eu, 0x5CF5u, 0x4D7Cu,
    0xC60Cu, 0xD785u, 0xE51Eu, 0xF497u, 0x8028u, 0x91A1u, 0xA33Au, 0xB2B3u,
    0x4A44u, 0x5BCDu, 0x6956u, 0x78DFu, 0x0C60u, 0x1DE9u, 0x2F72u, 0x3EFBu,
    0xD68Du, 0xC704u, 0xF59Fu, 0xE416u, 0x90A9u, 0x8120u, 0xB3BBu, 0xA232u,
    0x5AC5u, 0x4B4Cu, 0x79D7u, 0x685Eu, 0x1CE1u, 0x0D68u, 0x3FF3u, 0x2E7Au,
    0xE70Eu, 0xF687u, 0xC41Cu, 0xD595u, 0xA12Au, 0xB0A3u, 0x8238u, 0x93B1u,
    0x6B46u, 0x7ACFu, 0x4854u, 0x59DDu, 0x2D62u, 0x3CEBu, 0x0E70u, 0x1FF9u,
    0xF78Fu, 0xE606u, 0xD49Du, 0xC514u, 0xB1ABu, 0xA022u, 0x92B9u, 0x8330u,
    0x7BC7u, 0x6A4Eu, 0x58D5u, 0x495Cu, 0x3DE3u, 0x2C6Au, 0x1EF1u, 0x0F78u
};
static const uint32_t fcs32_tab[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du
};

uint16_t v42_fcs16(uint16_t fcs, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        fcs = (uint16_t)((fcs >> 8) ^ fcs16_tab[(fcs ^ data[i]) & 0xFFu]);
    return fcs;
}

uint32_t v42_fcs32(uint32_t fcs, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        fcs = (fcs >> 8) ^ fcs32_tab[(fcs ^ data[i]) & 0xFFu];
    return fcs;
}

/* -------------------------------------------------------------- transmit */
void v42hdlc_tx_init(v42hdlc_tx_t *t)
{
    memset(t, 0, sizeof *t);
}

int v42hdlc_tx_busy(const v42hdlc_tx_t *t)
{
    return t->len != 0 || t->in_frame;
}

int v42hdlc_tx_frame(v42hdlc_tx_t *t, const uint8_t *data, int len, int fcs32)
{
    if (t->len != 0 || len < 1 || len > V42HDLC_MAX_PAYLOAD) return -1;
    memcpy(t->buf, data, (size_t)len);
    if (fcs32) {
        uint32_t f = ~v42_fcs32(V42_FCS32_INIT, data, (size_t)len);
        t->buf[len++] = (uint8_t)f;
        t->buf[len++] = (uint8_t)(f >> 8);
        t->buf[len++] = (uint8_t)(f >> 16);
        t->buf[len++] = (uint8_t)(f >> 24);
    } else {
        uint16_t f = (uint16_t)~v42_fcs16(V42_FCS16_INIT, data, (size_t)len);
        t->buf[len++] = (uint8_t)f;
        t->buf[len++] = (uint8_t)(f >> 8);
    }
    t->len = len;
    t->pos = 0;
    return 0;
}

void v42hdlc_tx_flags(v42hdlc_tx_t *t, int n)
{
    if (n > t->min_flags) t->min_flags = n;
}

void v42hdlc_tx_abort(v42hdlc_tx_t *t)
{
    if (t->in_frame) t->abort_req = 1;
}

static void tx_raw(v42hdlc_tx_t *t, uint8_t octet)
{
    t->cur = octet; t->nbits = 8; t->raw = 1; t->ones = 0;
}

int v42hdlc_tx_bit_full(v42hdlc_tx_t *t)
{
    int bit;

    if (t->abort_req && t->in_frame) {
        /* eight ones abort the frame; flags follow */
        t->abort_req = 0; t->in_frame = 0; t->len = 0; t->pos = 0;
        t->flag_done = 0; t->aborts++;
        tx_raw(t, 0xFF);
    }
    if (t->in_frame && !t->raw && t->ones == 5) {
        t->ones = 0;                      /* zero insertion (8.1.1.2) */
        return 0;
    }
    if (t->nbits == 0) {
        if (t->in_frame && t->pos < t->len) {
            t->cur = t->buf[t->pos++]; t->nbits = 8; t->raw = 0;
        } else if (t->in_frame) {
            /* frame data exhausted: closing flag */
            t->in_frame = 0; t->len = 0; t->pos = 0; t->frames++;
            tx_raw(t, FLAG);
            t->flag_done = 1;
            if (t->min_flags > 0) t->min_flags--;
        } else if (t->len != 0 && t->flag_done && t->min_flags == 0) {
            /* the flag just sent serves as the opening flag */
            t->in_frame = 1; t->flag_done = 0; t->ones = 0;
            t->cur = t->buf[t->pos++]; t->nbits = 8; t->raw = 0;
        } else {
            tx_raw(t, FLAG);              /* interframe time fill (8.1.5) */
            t->flag_done = 1;
            if (t->min_flags > 0) t->min_flags--;
        }
    }
    bit = t->cur & 1;
    t->cur >>= 1;
    t->nbits--;
    if (!t->raw) {
        if (bit) t->ones++; else t->ones = 0;
    }
    return bit;
}

/* --------------------------------------------------------------- receive */
void v42hdlc_rx_init(v42hdlc_rx_t *r)
{
    memset(r, 0, sizeof *r);
    r->max_len = V42HDLC_MAX_FRAME;
    r->fcs_mode = V42HDLC_FCS16;
}

void v42hdlc_rx_set_fcs(v42hdlc_rx_t *r, int fcs_mode)
{
    r->fcs_mode = fcs_mode;
}

void v42hdlc_rx_set_max(v42hdlc_rx_t *r, int max_len)
{
    if (max_len < 4) max_len = 4;
    if (max_len > V42HDLC_MAX_FRAME) max_len = V42HDLC_MAX_FRAME;
    r->max_len = max_len;
}

static void rx_restart(v42hdlc_rx_t *r)
{
    r->len = 0; r->nbits = 0; r->cur = 0; r->overflow = 0;
}

/* What followed a run of ones, collected from the zero that ended it, up to
 * the flag that has just completed: len octets and nbits more in cur, the
 * last six bits the flag's. A frame if, with its first k bits ones lost in
 * the run (k = 0..5; at k = 5 the zero that ended the run was an inserted
 * one), it is octets and its FCS holds. Rebuilt in buf, its FCS stripped;
 * 0 when it is not a frame. */
static int bit_at(const v42hdlc_rx_t *r, int len, uint8_t cur, int i)
{
    return i < 8 * len ? (r->buf[i >> 3] >> (i & 7)) & 1 : (cur >> (i - 8 * len)) & 1;
}

static int rx_resync(v42hdlc_rx_t *r, int len, int nbits, uint8_t cur)
{
    int have = 8 * len + nbits - 6, k;
    for (k = 0; k <= 5; k++) {
        int skip = k == 5, n = k + have - skip, i, j, d = k - skip;
        uint16_t c16 = V42_FCS16_INIT; uint32_t c32 = V42_FCS32_INIT;
        int ok16, ok32;
        if (have < 1 || n % 8 || n / 8 > r->max_len || n / 8 < 4) continue;
        for (i = 0; i < n; i++) {
            unsigned b = i < k ? 1u : (unsigned)bit_at(r, len, cur, i - d);
            c16 = (uint16_t)(((c16 ^ b) & 1) ? (c16 >> 1) ^ 0x8408u : c16 >> 1);
            c32 = ((c32 ^ b) & 1) ? (c32 >> 1) ^ 0xEDB88320u : c32 >> 1;
        }
        ok32 = r->fcs_mode != V42HDLC_FCS16 && n / 8 >= 6 && c32 == V42_FCS32_GOOD;
        ok16 = !ok32 && r->fcs_mode != V42HDLC_FCS32 && c16 == V42_FCS16_GOOD;
        if (!ok16 && !ok32) continue;
        /* move the bits up by d and put the k ones in front, from the top
         * down so that nothing is overwritten before it is read */
        for (j = n - 1; j >= 0; j--) {
            int b = j < k ? 1 : bit_at(r, len, cur, j - d);
            if (b) r->buf[j >> 3] = (uint8_t)(r->buf[j >> 3] | (1u << (j & 7)));
            else r->buf[j >> 3] = (uint8_t)(r->buf[j >> 3] & ~(1u << (j & 7)));
        }
        r->last_fcs32 = ok32;
        r->good++; r->resynced++;
        return n / 8 - (ok32 ? 4 : 2);
    }
    return 0;
}

/* A flag has just completed. The accumulator holds the frame octets plus
 * the flag's leading zero and five of its ones (six stray bits). */
static int rx_flag(v42hdlc_rx_t *r)
{
    int len = r->len, nbits = r->nbits, overflow = r->overflow;
    int was_synced = r->synced, tent = r->tent;
    uint8_t cur = r->cur;

    rx_restart(r);
    r->synced = 1; r->tent = 0;
    if (!was_synced) { r->flag_run = 1; return 0; }
    if (len == 0 && nbits == 6) { if (r->flag_run < V42HDLC_FLAG_RUN_MAX) r->flag_run++; return 0; }
    r->flag_run = 1;
    if (tent) return overflow ? 0 : rx_resync(r, len, nbits, cur);
    if (overflow) { r->too_long++; return V42HDLC_RX_LONG; }
    if (nbits != 6) {
        if (len == 0) return 0;           /* debris between flags */
        r->misaligned++; return V42HDLC_RX_ALIGN;
    }
    /* 8.1.3 b): at least address, control and FCS */
    if (len < 4) { r->runts++; return V42HDLC_RX_SHORT; }
    if (r->fcs_mode != V42HDLC_FCS16 && len >= 6 &&
        v42_fcs32(V42_FCS32_INIT, r->buf, (size_t)len) == V42_FCS32_GOOD) {
        r->last_fcs32 = 1; r->good++;
        return len - 4;
    }
    if (r->fcs_mode != V42HDLC_FCS32 &&
        v42_fcs16(V42_FCS16_INIT, r->buf, (size_t)len) == V42_FCS16_GOOD) {
        r->last_fcs32 = 0; r->good++;
        return len - 2;
    }
    if (r->fcs_mode == V42HDLC_FCS32 && len < 6) { r->runts++; return V42HDLC_RX_SHORT; }
    r->bad_fcs++;
    return V42HDLC_RX_BAD_FCS;
}

int v42hdlc_rx_bit_full(v42hdlc_rx_t *r, int bit)
{
    if (bit & 1) {
        if (r->ones < 7) r->ones++;
        if (r->ones == 7) {
            /* seven contiguous ones: abort (8.1.4); wait for a flag */
            int had = r->synced && r->len > 0 && !r->tent;
            r->ones = 8;                  /* saturate; report once */
            r->synced = 0; r->flag_run = 0; r->tent = 0;
            rx_restart(r);
            if (had) { r->aborts++; return V42HDLC_RX_ABORT; }
            return 0;
        }
        if (r->ones >= 6) return 0;       /* flag or abort decided by next bit */
    } else {
        int ones = r->ones;
        r->ones = 0;
        if (ones == 5) return 0;          /* inserted zero: delete (8.1.1.2) */
        if (ones == 6) return rx_flag(r);
        if (ones >= 7) {
            /* the end of a run of ones: a frame may follow with no flag
             * (v42_hdlc.h); this zero is its first bit, or one inserted */
            rx_restart(r);
            r->synced = 1; r->tent = 1;
        }
    }
    if (r->synced) v42hdlc_rx_collect(r, bit);
    return 0;
}
