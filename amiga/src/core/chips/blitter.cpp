/* Blitter OCS — Fase 0. Ground truth: HRM cap. 6 + spec empirica dal log [BLT]
 * (i blit reali del KS 1.3: coppie area-mode EA/2A con FWM/LWM, e line mode
 *  CON1 bit0 per il disegno della grafica insert-disk).
 * Modello funzionale: il blit viene eseguito interamente alla scrittura di
 * BLTSIZE (BBUSY mai visibile: DMACONR risponde gia' 0). A fine blit viene
 * alzato INTREQ bit 6 (BLIT).
 * Area mode: canali A/B/C/D, minterm LF completo, shift ASH/BSH con carry
 * tra word, FWM/LWM, ascending/descending, fill inclusivo/esclusivo (DESC).
 * Line mode: accumulatore BLTAPTL, ottanti SUD/SUL/AUL, SING, texture BLTBDAT.
 */
#include "a500.h"
#include <time.h>
#include <string.h>
#include <stdint.h>
extern "C" {
#include "m68k.h"
}

static long long blit_total_ns = 0;
long long blitter_ns(void) { return blit_total_ns; }
static long long blit_total_words = 0;
long long blitter_words(void) { return blit_total_words; }  /* words (area) or pixels (line) done */

/* accesso word alla chip RAM (indirizzi word-aligned, mask 512KB) */
static inline uint16_t rd16(uint32_t a)
{
    a &= 0x7FFFE;
    return (uint16_t)(chip_ram[a] << 8 | chip_ram[a + 1]);
}
static inline void wr16(uint32_t a, uint16_t v)
{
    a &= 0x7FFFE;
    chip_ram[a] = (uint8_t)(v >> 8);
    chip_ram[a + 1] = (uint8_t)v;
}

static inline uint16_t minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t d = 0;
    if (lf & 0x80) d |= (uint16_t)( a &  b &  c);
    if (lf & 0x40) d |= (uint16_t)( a &  b & ~c);
    if (lf & 0x20) d |= (uint16_t)( a & ~b &  c);
    if (lf & 0x10) d |= (uint16_t)( a & ~b & ~c);
    if (lf & 0x08) d |= (uint16_t)(~a &  b &  c);
    if (lf & 0x04) d |= (uint16_t)(~a &  b & ~c);
    if (lf & 0x02) d |= (uint16_t)(~a & ~b &  c);
    if (lf & 0x01) d |= (uint16_t)(~a & ~b & ~c);
    return d;
}

static uint32_t pt(uint32_t off_h)   /* puntatore 20 bit da coppia H/L */
{
    return (((uint32_t)custom_get(off_h) << 16) | custom_get(off_h + 2)) & 0x7FFFF;
}

/* ---------------- area mode ----------------
 * Waveshare P4 (stages 10, 11): the same blit as before, done faster - the
 * minterm with masks instead of eight tests per word, area fill a byte at a
 * time from a table instead of a bit at a time, and chip RAM read and
 * written a word at a time. Gives exactly the old results. */
static inline uint16_t crd16(uint32_t a)
{
    uint16_t v;
    memcpy(&v, chip_ram + (a & 0x7FFFE), 2);
    return __builtin_bswap16(v);
}
static inline void cwr16(uint32_t a, uint16_t v)
{
    v = __builtin_bswap16(v);
    memcpy(chip_ram + (a & 0x7FFFE), &v, 2);
}

/* fill: [exclusive][carry in][byte] -> filled byte | carry out << 8 */
static uint16_t fill_tab[2][2][256];
static bool fill_ready = false;
static void fill_init(void)
{
    for (int ex = 0; ex < 2; ex++)
        for (int c = 0; c < 2; c++)
            for (int v = 0; v < 256; v++) {
                int fc = c, out = 0;
                for (int i = 0; i < 8; i++) {
                    int bit = (v >> i) & 1;
                    int ob = ex ? (fc & ~bit) : (bit | fc);
                    out |= (ob & 1) << i;
                    fc ^= bit;
                }
                fill_tab[ex][c][v] = (uint16_t)(out | fc << 8);
            }
    fill_ready = true;
}

/* Stage 11: a separate loop for each combination of channels, direction,
   fill and the commonest minterms (D = A, and the "cookie cut"
   D = AB + /A C), so the inner loop has no tests of what is switched on. The
   results are the same as before. */
struct BlitP {
    int w, h, ash, bsh, fci, fex;
    uint16_t fwm, lwm, adat, bdat, cdat;
    uint16_t m0, m1, m2, m3, m4, m5, m6, m7;
    uint32_t apt, bpt, cpt, dpt;
    uint32_t amd, bmd, cmd, dmd, step;
};

enum { MT_ANY = 0, MT_A = 1, MT_COOKIE = 2 };

template <bool UA, bool UB, bool UC, bool UD, bool DESC, bool FILL, int MT>
static void area_loop(BlitP &p)
{
    const int w = p.w, h = p.h, ash = p.ash, bsh = p.bsh;
    const uint32_t step = p.step;
    uint32_t apt = p.apt, bpt = p.bpt, cpt = p.cpt, dpt = p.dpt;
    const uint16_t fwm = p.fwm, lwm = p.lwm, adat = p.adat, bdat = p.bdat, cdat = p.cdat;
    const uint16_t m0 = p.m0, m1 = p.m1, m2 = p.m2, m3 = p.m3, m4 = p.m4, m5 = p.m5, m6 = p.m6, m7 = p.m7;
    const bool needB = MT != MT_A, needC = MT != MT_A;
    uint16_t aprev = 0, bprev = 0;
    for (int y = 0; y < h; y++) {
        int fc = p.fci;
        for (int x = 0; x < w; x++) {
            uint16_t araw = UA ? crd16(apt) : adat;
            if (x == 0)     araw &= fwm;
            if (x == w - 1) araw &= lwm;
            uint16_t ahold;
            if (!DESC) ahold = (uint16_t)((((uint32_t)aprev << 16) | araw) >> ash);
            else       ahold = (uint16_t)((((uint32_t)araw << 16) | aprev) >> (16 - ash));
            aprev = araw;
            uint16_t dhold;
            if (MT == MT_A) {
                dhold = ahold;
            } else {
                uint16_t braw = UB ? crd16(bpt) : bdat;
                uint16_t bhold;
                if (!DESC) bhold = (uint16_t)((((uint32_t)bprev << 16) | braw) >> bsh);
                else       bhold = (uint16_t)((((uint32_t)braw << 16) | bprev) >> (16 - bsh));
                if (UB) bprev = braw;
                uint16_t chold = UC ? crd16(cpt) : cdat;
                if (MT == MT_COOKIE) {
                    dhold = (uint16_t)((ahold & bhold) | ((uint16_t)~ahold & chold));
                } else {
                    uint16_t ab = ahold & bhold, anb = ahold & (uint16_t)~bhold;
                    uint16_t nab = (uint16_t)~ahold & bhold, nanb = (uint16_t)~(ahold | bhold);
                    uint16_t with_c = (ab & m7) | (anb & m5) | (nab & m3) | (nanb & m1);
                    uint16_t without_c = (ab & m6) | (anb & m4) | (nab & m2) | (nanb & m0);
                    dhold = (uint16_t)((chold & with_c) | ((uint16_t)~chold & without_c));
                }
            }
            (void)needB; (void)needC;
            if (FILL) {         /* fill (meaningful in DESC): bits from LSB to MSB */
                uint16_t lo = fill_tab[p.fex][fc][dhold & 0xFF];
                uint16_t hi = fill_tab[p.fex][lo >> 8][dhold >> 8];
                fc = hi >> 8;
                dhold = (uint16_t)((lo & 0xFF) | (hi & 0xFF) << 8);
            }
            if (UD) { cwr16(dpt, dhold); dpt += step; }
            if (UA) apt += step;
            if (UB) bpt += step;
            if (UC) cpt += step;
        }
        if (UA) apt += p.amd;
        if (UB) bpt += p.bmd;
        if (UC) cpt += p.cmd;
        if (UD) dpt += p.dmd;
    }
    p.apt = apt; p.bpt = bpt; p.cpt = cpt; p.dpt = dpt;
}

typedef void (*area_fn)(BlitP &);
template <int I> static void area_call(BlitP &p)
{
    area_loop<(I & 1) != 0, (I & 2) != 0, (I & 4) != 0, (I & 8) != 0, (I & 16) != 0, (I & 32) != 0, (I >> 6)>(p);
}
template <int... I> struct AreaTable { static constexpr area_fn fn[sizeof...(I)] = {area_call<I>...}; };
template <int... I> constexpr area_fn AreaTable<I...>::fn[sizeof...(I)];
template <int N, int... I> struct MakeAreaTable : MakeAreaTable<N - 1, N - 1, I...> {};
template <int... I> struct MakeAreaTable<0, I...> { typedef AreaTable<I...> type; };
typedef MakeAreaTable<192>::type area_table;

/* everything an area blit needs, from the registers as they are now */
static int blit_area_setup(uint16_t bltsize, BlitP &p)
{
    uint16_t con0 = custom_get(0x040), con1 = custom_get(0x042);
    p.w = bltsize & 0x3F;         if (!p.w) p.w = 64;
    p.h = (bltsize >> 6) & 0x3FF; if (!p.h) p.h = 1024;
    p.ash = con0 >> 12; p.bsh = con1 >> 12;
    int usea = (con0 & 0x800) != 0, useb = (con0 & 0x400) != 0, usec = (con0 & 0x200) != 0, used = (con0 & 0x100) != 0;
    uint8_t lf = (uint8_t)con0;
    int desc = (con1 & 0x002) != 0;
    int ife = con1 & 0x008, efe = con1 & 0x010;
    int fill = (ife || efe) ? 1 : 0;
    p.fci = (con1 & 0x004) ? 1 : 0;
    p.fex = ife ? 0 : 1;   /* inclusive fill wins, as before */
    if (fill && !fill_ready) fill_init();
    p.fwm = custom_get(0x044); p.lwm = custom_get(0x046);
    p.apt = pt(0x050); p.bpt = pt(0x04C); p.cpt = pt(0x048); p.dpt = pt(0x054);
    int dir = desc ? -1 : 1;
    p.step = (uint32_t)(dir * 2);
    p.amd = (uint32_t)(dir * (int16_t)custom_get(0x064));
    p.bmd = (uint32_t)(dir * (int16_t)custom_get(0x062));
    p.cmd = (uint32_t)(dir * (int16_t)custom_get(0x060));
    p.dmd = (uint32_t)(dir * (int16_t)custom_get(0x066));
    p.adat = custom_get(0x074); p.bdat = custom_get(0x072); p.cdat = custom_get(0x070);
    /* minterm masks: term n of LF on = all ones */
    p.m0 = (lf & 0x01) ? 0xFFFF : 0; p.m1 = (lf & 0x02) ? 0xFFFF : 0;
    p.m2 = (lf & 0x04) ? 0xFFFF : 0; p.m3 = (lf & 0x08) ? 0xFFFF : 0;
    p.m4 = (lf & 0x10) ? 0xFFFF : 0; p.m5 = (lf & 0x20) ? 0xFFFF : 0;
    p.m6 = (lf & 0x40) ? 0xFFFF : 0; p.m7 = (lf & 0x80) ? 0xFFFF : 0;
    int mt = lf == 0xF0 ? MT_A : (lf == 0xCA ? MT_COOKIE : MT_ANY);

    /* the pointers when the blit is done (each used channel moves on by a
       row and its modulo, h times): written back at once, so the next blit
       can carry on from them even while this one is still running */
    uint32_t rows = (uint32_t)p.h, row = (uint32_t)p.w * p.step;
    uint32_t fa = usea ? p.apt + rows * (row + p.amd) : p.apt;
    uint32_t fb = useb ? p.bpt + rows * (row + p.bmd) : p.bpt;
    uint32_t fcp = usec ? p.cpt + rows * (row + p.cmd) : p.cpt;
    uint32_t fd = used ? p.dpt + rows * (row + p.dmd) : p.dpt;
    custom_set_latch(0x050, (uint16_t)(fa >> 16)); custom_set_latch(0x052, (uint16_t)fa);
    custom_set_latch(0x04C, (uint16_t)(fb >> 16)); custom_set_latch(0x04E, (uint16_t)fb);
    custom_set_latch(0x048, (uint16_t)(fcp >> 16)); custom_set_latch(0x04A, (uint16_t)fcp);
    custom_set_latch(0x054, (uint16_t)(fd >> 16)); custom_set_latch(0x056, (uint16_t)fd);
    return usea | useb << 1 | usec << 2 | used << 3 | desc << 4 | fill << 5 | mt << 6;
}

/* ---------------- line mode ----------------
 * HRM: BLTAPTL = 4*dy - 2*dx (accumulatore, signed), BLTAMOD = 4*(dy-dx),
 * BLTBMOD = 4*dy, BLTCMOD = larghezza raster in byte, lunghezza = h di
 * BLTSIZE (w deve essere 2). Ottanti: SUD(0x10) SUL(0x08) AUL(0x04),
 * SIGN iniziale in CON1 bit6, SING(0x02) = un pixel per riga raster.
 * Regola: se SIGN=0 (acc >= 0) avanza anche l'asse minore; l'asse maggiore
 * avanza sempre; poi acc += (SIGN=0 ? AMOD : BMOD).
 */
static void blit_line(uint16_t bltsize)
{
    uint16_t con0 = custom_get(0x040), con1 = custom_get(0x042);
    int len = (bltsize >> 6) & 0x3FF; if (!len) len = 1024;
    int x   = con0 >> 12;                 /* pixel di partenza nella word */
    int bsh = con1 >> 12;                 /* rotazione texture */
    uint8_t lf = (uint8_t)con0;
    int sign = (con1 & 0x040) ? 1 : 0;
    int sing = con1 & 0x002, sud = con1 & 0x010, sul = con1 & 0x008, aul = con1 & 0x004;
    uint32_t cpt = pt(0x048);             /* dest = C = D */
    int16_t amod = (int16_t)custom_get(0x064), bmod = (int16_t)custom_get(0x062);
    int16_t cmod = (int16_t)custom_get(0x060);
    uint16_t adat = custom_get(0x074) & custom_get(0x044);   /* ADAT & FWM */
    uint16_t bdat = custom_get(0x072);
    int16_t acc = (int16_t)custom_get(0x052);                /* BLTAPTL */
    int dot_row = 0;

    for (int i = 0; i < len; i++) {
        if (!sing || !dot_row) {
            uint16_t am = adat;
            if (i == len - 1) am &= custom_get(0x046);   /* LWM sull'ultimo pixel */
            uint16_t ahold = (uint16_t)(am >> x);
            uint16_t bhold = (uint16_t)((bdat >> bsh) | (bdat << (16 - bsh)));
            if (bsh == 0) bhold = bdat;
            uint16_t chold = rd16(cpt);
            wr16(cpt, minterm(lf, ahold, bhold, chold));
            dot_row = 1;
        }
        bdat = (uint16_t)((bdat << 1) | (bdat >> 15));       /* texture avanza per pixel */

        /* ottanti (ground truth WinUAE blitter.cpp):
           SUD=1: maggiore=X, minore=Y | SUD=0: maggiore=Y, minore=X
           direzione minore = SUL, direzione maggiore = AUL */
        if (!sign) {                                          /* passo asse minore */
            if (sud) { if (sul) { cpt -= (uint32_t)cmod; dot_row = 0; }
                       else     { cpt += (uint32_t)cmod; dot_row = 0; } }
            else     { if (sul) { if (x-- == 0) { x = 15; cpt -= 2; } }
                       else     { if (++x == 16) { x = 0; cpt += 2; } } }
        }
        if (sud) {                                            /* passo asse maggiore */
            if (aul) { if (x-- == 0) { x = 15; cpt -= 2; } }
            else     { if (++x == 16) { x = 0; cpt += 2; } }
        } else {
            if (aul) { cpt -= (uint32_t)cmod; } else { cpt += (uint32_t)cmod; }
            dot_row = 0;
        }
        acc = (int16_t)(acc + (sign ? bmod : amod));
        sign = acc < 0;
    }
    custom_set_latch(0x048, (uint16_t)(cpt >> 16));
    custom_set_latch(0x04A, (uint16_t)cpt);
    custom_set_latch(0x052, (uint16_t)acc);
}

volatile int blit_irq_pending = 0;   /* completamento differito alla prossima scanline:
                               l'int sincrono verrebbe cancellato dall'ack del
                               server QBlit che arriva DOPO l'avvio del blit */

/* Stage 11: area blits can run on the other core (blitter_async_kick set),
   at the same time as the 68000, as on the real machine: DMACONR shows
   BBUSY until the blit is done, then the blitter interrupt comes. A new
   blit, a line blit or a copper wait for the blitter first waits for the
   one running. */
void (*blitter_async_kick)(void) = nullptr;
static volatile int blit_busy = 0;
static BlitP job_p;
static int job_idx;

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

int blitter_busy(void) { return __atomic_load_n(&blit_busy, __ATOMIC_ACQUIRE); }

void blitter_wait(void)
{
    while (__atomic_load_n(&blit_busy, __ATOMIC_ACQUIRE)) { }
}

/* on the blitter core: run the blit handed over, if there is one */
void blitter_run_pending(void)
{
    if (!__atomic_load_n(&blit_busy, __ATOMIC_ACQUIRE)) return;
    long long t0 = now_ns();
    area_table::fn[job_idx](job_p);
    blit_total_ns += now_ns() - t0;
    __atomic_store_n(&blit_irq_pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&blit_busy, 0, __ATOMIC_RELEASE);
}

void blitter_do(uint16_t bltsize)
{
    blitter_wait();
    if (custom_get(0x042) & 0x0001) {
        long long t0 = now_ns();
        blit_line(bltsize);
        int len = (bltsize >> 6) & 0x3FF;
        blit_total_words += len ? len : 1024;
        blit_total_ns += now_ns() - t0;
        __atomic_store_n(&blit_irq_pending, 1, __ATOMIC_RELEASE);
        return;
    }
    int w = bltsize & 0x3F, h = (bltsize >> 6) & 0x3FF;
    blit_total_words += (long long)(w ? w : 64) * (h ? h : 1024);
    job_idx = blit_area_setup(bltsize, job_p);
    __atomic_store_n(&blit_busy, 1, __ATOMIC_RELEASE);
    if (blitter_async_kick) blitter_async_kick();
    else blitter_run_pending();
}
