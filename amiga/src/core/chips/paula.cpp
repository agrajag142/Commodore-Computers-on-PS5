/* ============ PAULA — 4 canali audio DMA ============
 * Ogni canale legge un campione (8-bit signed) dalla Chip RAM via DMA,
 * lo riproduce alla frequenza data da AUDxPER, in loop. I 4 canali si
 * mixano su 2 uscite stereo (0,3 = sinistra; 1,2 = destra).
 *
 * Timing: il clock Paula avanza col colorclock (~3.546895 MHz PAL).
 * Il periodo AUDxPER e' in colorclock: un nuovo campione ogni PER cicli.
 * Qui avanziamo il tempo per-riga (CYC_LINE colorclock a riga) e
 * produciamo campioni a una frequenza di uscita fissa (44100 Hz) per il WAV.
 */
#include "a500.h"
#include <string.h>
#ifdef ARDUINO
#include "esp_heap_caps.h"
#endif

#define PAL_COLORCLOCK 3546895.0
#define OUT_RATE       48000   /* PS5 port: AudioOut takes 48 kHz only (44100 before) */

typedef struct {
    uint32_t lc;        /* AUDxLC: puntatore campione (Chip RAM) */
    uint16_t len;       /* AUDxLEN: lunghezza in word */
    uint16_t per;       /* AUDxPER: periodo (colorclock per campione) */
    uint16_t vol;       /* AUDxVOL: volume 0-64 */
    uint16_t dat;       /* AUDxDAT: data register, utile per step successivi/software audio */
    /* stato DMA */
    uint32_t cur;       /* puntatore corrente */
    uint16_t wordcnt;   /* word rimanenti nel loop */
    int      percnt;    /* contatore periodo */
    int8_t   sample;    /* campione corrente in uscita */
    int      hi;        /* quale byte della word (0=alto,1=basso) */
} audchan_t;

static audchan_t ch[4];
static int audio_on = 0;   /* maschera DMA canali (da DMACON bit 0-3) */

/* accumulatore per il resampling a OUT_RATE */
/* accumulatore resampling a punto fisso 16.16 (no float nel loop) */
static uint32_t out_acc_fix = 0;

/* buffer WAV solo nella build PC. Su ESP32 andra' sostituito con ring buffer/I2S. */
#ifndef ARDUINO
#define AUD_BUFMAX (OUT_RATE * 60)   /* max 60 secondi */
static int16_t out_l[AUD_BUFMAX], out_r[AUD_BUFMAX];
static int out_n = 0;
#else
/* ESP32: ring buffer stereo interleaved (L,R,L,R...). Paula scrive in coda,
   il task I2S legge dalla testa. Potenza di 2 per il wrap con AND. */
#define RING_SAMPLES 8192              /* 8192 frame stereo (~186ms a 44.1kHz) */
#define RING_MASK (RING_SAMPLES - 1)
/* Non tenere 32KB di ring audio in .bss interna: su ESP32-S3 la DRAM e' gia'
   stretta. Allochiamo a runtime, preferendo PSRAM. */
static volatile int16_t *ring_l = 0;
static volatile int16_t *ring_r = 0;
static volatile uint32_t ring_wr = 0;   /* indice scrittura (Paula) */
static volatile uint32_t ring_rd = 0;   /* indice lettura (I2S) */

static void ring_alloc_if_needed(void)
{
    if (!ring_l)
        ring_l = (volatile int16_t*)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring_r)
        ring_r = (volatile int16_t*)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    /* Fallback interno solo se serve: meglio partire che crashare, ma puo'
       ridurre il margine DRAM. */
    if (!ring_l)
        ring_l = (volatile int16_t*)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                                                     MALLOC_CAP_8BIT);
    if (!ring_r)
        ring_r = (volatile int16_t*)heap_caps_malloc(RING_SAMPLES * sizeof(int16_t),
                                                     MALLOC_CAP_8BIT);
}

/* quanti frame disponibili da leggere */
static inline uint32_t ring_avail(void) { return (ring_wr - ring_rd) & 0xFFFFFFFF; }
/* spazio libero */
static inline uint32_t ring_free(void) { return RING_SAMPLES - ring_avail(); }

/* PS5 port: frames waiting in the ring (the sound thread keeps the delay short) */
int paula_ring_avail(void) { return (int)ring_avail(); }

/* letto dal task I2S: estrae un frame; ritorna 0 se vuoto (silenzio) */
int paula_ring_pop(int16_t *l, int16_t *r)
{
    if (!ring_l || !ring_r) { *l = 0; *r = 0; return 0; }
    if (ring_rd == ring_wr) { *l = 0; *r = 0; return 0; }   /* underflow: silenzio */
    uint32_t i = ring_rd & RING_MASK;
    *l = ring_l[i]; *r = ring_r[i];
    ring_rd++;
    return 1;
}
#endif

void paula_reset(void)
{
    memset(ch, 0, sizeof ch);
    audio_on = 0; out_acc_fix = 0;
#ifndef ARDUINO
    out_n = 0;
#else
    ring_alloc_if_needed();
    ring_wr = ring_rd = 0;
#endif
}

/* scrittura registri audio (offset custom 0xA0-0xDF) */
void paula_write(uint32_t off, uint16_t v)
{
    int c = (off - 0xA0) / 16;      /* canale 0-3 */
    if (c < 0 || c > 3) return;
    switch ((off - 0xA0) % 16) {
    case 0x0: ch[c].lc = (ch[c].lc & 0x0000FFFF) | ((uint32_t)v << 16); break; /* LCH */
    case 0x2: ch[c].lc = (ch[c].lc & 0xFFFF0000) | v; break;                   /* LCL */
    case 0x4: ch[c].len = v ? v : 1; break;                                     /* LEN */
    case 0x6: ch[c].per = v ? v : 1; break;                                     /* PER */
    case 0x8: ch[c].vol = (v & 0x7F) > 64 ? 64 : (v & 0x7F); break;             /* VOL */
    case 0xA: ch[c].dat = v; ch[c].sample = (int8_t)(v >> 8); ch[c].hi = 1; break; /* DAT, base software audio */
    }
}

/* attiva/disattiva canali dal DMACON (bit 0-3 = AUD0-3, bit 9 = master) */
void paula_set_dma(uint16_t dmacon)
{
    int master = (dmacon & 0x0200) != 0;
    int newmask = master ? (dmacon & 0x000F) : 0;
    for (int c = 0; c < 4; c++) {
        int on = (newmask >> c) & 1;
        if (on && !((audio_on >> c) & 1)) {   /* canale appena attivato: ricarica */
            ch[c].cur = ch[c].lc & 0x7FFFF;
            ch[c].wordcnt = ch[c].len;
            ch[c].percnt = ch[c].per;
            ch[c].hi = 0;
        }
    }
    audio_on = newmask;
}

static int8_t fetch_sample(int c)
{
    uint32_t a = ch[c].cur & 0x7FFFE;
    uint16_t w = ((uint16_t)chip_ram[a] << 8) | chip_ram[a + 1];
    int8_t s = ch[c].hi ? (int8_t)(w & 0xFF) : (int8_t)(w >> 8);
    if (ch[c].hi) {                    /* consumata la word: avanza */
        ch[c].cur += 2;
        if (--ch[c].wordcnt == 0) {     /* fine campione: loop */
            ch[c].cur = ch[c].lc & 0x7FFFF;
            ch[c].wordcnt = ch[c].len;
        }
    }
    ch[c].hi ^= 1;
    return s;
}

/* avanza Paula di 'colorclocks' cicli, produce campioni a OUT_RATE nel buffer */
/* accumulatore resampling a PUNTO FISSO: evito il float nel loop piu' stretto.
   incremento per colorclock = OUT_RATE/PAL_COLORCLOCK in fixed-point 16.16 */
#define RESAMP_FIX ((uint32_t)(((double)OUT_RATE / 3546895.0) * 65536.0 + 0.5))

/* one output sample from the current channel samples */
static inline void paula_emit(void)
{
    int l = (ch[0].sample * ch[0].vol + ch[3].sample * ch[3].vol) * 2;
    int r = (ch[1].sample * ch[1].vol + ch[2].sample * ch[2].vol) * 2;
    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
#ifndef ARDUINO
    if (out_n < AUD_BUFMAX) {
        out_l[out_n] = (int16_t)l; out_r[out_n] = (int16_t)r;
        out_n++;
    }
#else
    if (ring_l && ring_r && ring_free() > 1) {
        uint32_t i = ring_wr & RING_MASK;
        ring_l[i] = (int16_t)l; ring_r[i] = (int16_t)r;
        ring_wr++;
    }
#endif
}

void paula_step(int colorclocks)
{
#ifdef ARDUINO
    /* Ottimizzazione critica ESP32: se nessun canale audio DMA e' attivo,
       non generiamo campioni di silenzio nel ring. Il task I2S emette gia'
       silenzio quando il ring e' vuoto. */
    if (audio_on == 0) return;
#endif
    /* Waveshare P4: the same result as stepping one colour clock at a time,
       but in jumps from one output sample to the next (about 80 colour
       clocks): each channel counts its period down by the whole jump and
       fetches the samples it passed. Some 900 steps a frame instead of
       71000 x 4. */
    while (colorclocks > 0) {
        /* colour clocks until the resampling accumulator passes 65536 */
        uint32_t need = 65536 - out_acc_fix;
        int seg = (int)((need + RESAMP_FIX - 1) / RESAMP_FIX);
        if (seg < 1) seg = 1;
        if (seg > colorclocks) seg = colorclocks;
        for (int c = 0; c < 4; c++) {
            if (!((audio_on >> c) & 1)) continue;
            ch[c].percnt -= seg;
            while (ch[c].percnt <= 0) {
                ch[c].percnt += ch[c].per;
                ch[c].sample = fetch_sample(c);
            }
        }
        out_acc_fix += RESAMP_FIX * (uint32_t)seg;
        if (out_acc_fix >= 65536) {
            out_acc_fix -= 65536;
            paula_emit();
        }
        colorclocks -= seg;
    }
}

int paula_samples(void) {
#ifndef ARDUINO
    return out_n;
#else
    return 0;
#endif
}

/* scrive il buffer accumulato in un WAV stereo 16-bit */
#ifndef ARDUINO
#include <stdio.h>
int paula_write_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int nch = 2, bits = 16, rate = OUT_RATE;
    int datasz = out_n * nch * (bits / 8);
    int byterate = rate * nch * (bits / 8);
    fwrite("RIFF", 1, 4, f);
    uint32_t riff = 36 + datasz; fwrite(&riff, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtsz = 16; fwrite(&fmtsz, 4, 1, f);
    uint16_t fmt = 1; fwrite(&fmt, 2, 1, f);
    uint16_t chn = nch; fwrite(&chn, 2, 1, f);
    uint32_t rt = rate; fwrite(&rt, 4, 1, f);
    uint32_t br = byterate; fwrite(&br, 4, 1, f);
    uint16_t align = nch * (bits / 8); fwrite(&align, 2, 1, f);
    uint16_t bps = bits; fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = datasz; fwrite(&ds, 4, 1, f);
    for (int i = 0; i < out_n; i++) {
        fwrite(&out_l[i], 2, 1, f);
        fwrite(&out_r[i], 2, 1, f);
    }
    fclose(f);
    logmsg("[AUD] WAV scritto: %s (%d campioni, %.2f s)\n", path, out_n, (double)out_n / OUT_RATE);
    return 0;
}
#endif
