/* a500_phase0 — Fase 0: boot Kickstart 1.3 fino a "insert disk"
 * Validazione offline su PC. Ground truth: MAME (driver a500) + HRM.
 * CPU: Musashi (stesso core di MAME).
 */
#ifndef A500_H
#define A500_H

#include <stdint.h>
#include <stdio.h>
#ifdef ARDUINO
// su Arduino i log diagnostici file-based sono disattivati: rdlogf/blitlogf/tracef
// restano NULL, i blocchi if(...) sono sempre falsi. fprintf su di essi mai eseguito.
#endif

/* ---- memoria ---- */
#define CHIP_SIZE 0x80000
#define SLOW_BASE 0xC00000
#define SLOW_SIZE 0x80000
#define ROM_SIZE  0x40000   /* 256KB Kickstart 1.3 @ 0xFC0000 (mirror 0xF80000) */

#ifdef ARDUINO
extern uint8_t *chip_ram;   // PSRAM
extern uint8_t *kick_rom;   // PSRAM
#else
extern uint8_t chip_ram[CHIP_SIZE];
extern uint8_t kick_rom[ROM_SIZE];
#endif
extern int     ovl;         /* 1 = ROM overlay a 0x000000 (stato di reset) */

int  mem_load_rom(const char *path);
void mem_reset(void);

/* ---- CIA 8520 ---- */
typedef struct {
    uint8_t  pra, prb, ddra, ddrb;
    uint16_t ta, tb, ta_latch, tb_latch;
    uint8_t  cra, crb;
    uint8_t  sdr;              /* PS5 port: the serial data register (the keyboard) */
    uint8_t  icr_data, icr_mask;
    uint32_t tod, tod_latch, tod_alarm;
    int      tod_latched, tod_running;
    const char *name;
    int      irq_intreq_bit;   /* 3 = PORTS (CIA-A), 13 = EXTER (CIA-B) */
} cia_t;

extern cia_t cia_a, cia_b;

void    cia_reset(void);
uint8_t cia_read (cia_t *c, int reg);
void    cia_write(cia_t *c, int reg, uint8_t v);
void    cia_tick (int eclocks);      /* E-clock = CPU/10 */
/* PS5 port: the keyboard. kbd_key() from any thread (a raw key code 0x00-0x67,
 * up = 1 when let go); cia_kbd_vsync() once a frame on the emulator's thread
 * sends the next one to CIA-A, as the keyboard does, after the Amiga's handshake. */
void    kbd_key(uint8_t rawkey, int up);
void    cia_kbd_vsync(void);

/* ---- chipset custom (0xDFF000) ---- */
extern uint16_t intena, intreq, dmacon, adkcon;
extern int      vpos, hpos;          /* beam counter PAL: 312 linee */

uint16_t custom_read (uint32_t off);          /* off = addr & 0x1FE */
void     custom_write(uint32_t off, uint16_t v);
void     custom_reset(void);
uint16_t custom_get  (uint32_t off);          /* latch senza effetti collaterali */
void     custom_set_latch(uint32_t off, uint16_t v);  /* writeback puntatori blitter */
void     blitter_do(uint16_t bltsize);        /* esegue un blit (area o line) */
extern volatile int blit_irq_pending;         /* done-int differito di una linea */
/* Waveshare P4 (stage 11): area blits on another core. blitter_async_kick
   (if set) is called when a blit is waiting; that core then calls
   blitter_run_pending(). */
extern void (*blitter_async_kick)(void);
void blitter_run_pending(void);
int  blitter_busy(void);                      /* a blit not done yet (BBUSY) */
void blitter_wait(void);                      /* until it is done */
long long blitter_words(void);                /* words (area blits) and pixels (lines) done */
long long blitter_ns(void);                   /* tempo cumulato nel blitter (benchmark) */
void     irq_update(void);           /* ricalcola IPL verso Musashi */
void     intreq_set(int bit);        /* usato da CIA/vblank */

extern uint32_t bpl_pt[6];           /* puntatori bitplane live */
extern uint32_t blit_count;          /* blit totali richiesti (diagnostica) */
extern FILE *blitlogf;               /* log completo blit (--blitlog) */
extern FILE *rdlogf;                 /* log letture registri (--rdlog) */
extern int   cur_frame;              /* frame corrente (per il blitlog) */

/* ---- copper + denise (video.cpp) ---- */
void copper_vblank(void);
void copper_jmp(int which);
void copper_run_line(void);
void denise_render_line(void);
void paula_reset(void);
void paula_step(int colorclocks);
void paula_write(uint32_t off, uint16_t v);
void paula_set_dma(uint16_t dmacon);
int  paula_ring_pop(int16_t *l, int16_t *r);
void sprite_arm(int n);
void sprite_vblank(void);
const uint16_t *custom_regs_ptr(void); /* the custom registers as written, index = offset / 2 */
#ifdef ARDUINO
extern void (*denise_line_cb)(int vpos, const uint16_t *pixels, int w);

/* Waveshare ESP32-P4 port: a picture line split in two halves. The emulator
   task captures what the line needs (bitplane data, palette, scrolling,
   sprites) - cheap - and hands it to denise_job_cb; the other core then
   draws it with denise_draw_line(), which ends in denise_line_cb. */
typedef struct {
    int16_t  vpos;
    uint8_t  mode;        /* 0 = COLOR00 only, 1 = COLOR00 + sprites, 2 = bitplanes + sprites */
    uint8_t  hires, nplanes, scroll1, scroll2, nspr;
    uint16_t bg;          /* COLOR00 as read */
    uint16_t pal[32];     /* COLOR00-31 */
    struct { int16_t hstart; uint8_t num; uint16_t data, datb; } spr[8]; /* drawing order */
    uint8_t  nbytes;      /* bytes fetched per plane (DDFSTRT/DDFSTOP) */
    uint8_t  dpf, pf2pri; /* dual playfield (BPLCON0 bit 10), PF2 in front (BPLCON2 bit 6) */
    int16_t  x0;          /* where the first fetched pixel goes on the line (output pixels) */
    int16_t  clip0, clip1;/* display window, horizontally (output pixels) */
    alignas(4) uint8_t planes[6][104];
} denise_line_t;
/* denise_slot_cb (if set) gives the place to capture the next line into;
   denise_job_cb then hands the captured line over */
extern denise_line_t *(*denise_slot_cb)(void);
extern void (*denise_job_cb)(const denise_line_t *job);
/* the lines worth capturing: from denise_first_line up to (not including)
   denise_last_line */
extern int denise_first_line, denise_last_line;
void denise_draw_line(const denise_line_t *job);
#endif
int  video_write_ppm(const char *path);

/* ---- drive DF0 (fase 0: presente, senza disco) ---- */
void    drive_prb_write(uint8_t prb);   /* agganciato a CIA-B PRB */
int     drive_insert_adf(const char *path);
#ifdef ARDUINO
int     drive_set_adf(const uint8_t *data, uint32_t len);
uint8_t *drive_alloc_adf(void);
void    drive_mount_ready(void);
void    drive_eject(void);              /* Waveshare P4: disk out */
#endif
int     drive_track(void);
int     drive_side(void);
int     drive_motor_on(void);
int     drive_led(void);   /* PS5 port: the drive light */
const uint8_t *drive_adf(void);

/* ---- disco: MFM + DMA (disk.cpp) ---- */
void disk_dsklen_write(uint16_t v);
extern int disk_irq_pending;         /* DSKBLK differito di una linea */
int  disk_mfmdump(int track, const char *path);

/* ---- mouse porta 0 (input.cpp) ---- */
int      input_load_script(const char *path);
void     input_frame(int frame);
uint16_t input_joy0dat(void);
void     input_mouse_move(int dx, int dy);   /* Waveshare P4: touch screen mouse */
uint16_t input_joy1dat(void);
void     input_set_joy(int up, int down, int left, int right, int fire);
int      input_joy_fire(void);
void     input_set_lmb(int v);
void     input_set_rmb(int v);
int      input_lmb(void);
int      input_rmb(void);
uint8_t drive_pra_bits(void);           /* bit 2-5 di CIA-A PRA   */

/* ---- log & trace ---- */
extern FILE *tracef;
void logmsg(const char *fmt, ...);

#endif
