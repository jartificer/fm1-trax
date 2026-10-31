/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX platform interface (FM1-GM's, from FoMni's and X0X's): everything the app needs from the
 * hardware.
 *
 * The device implementation (plat_fm1.c) wraps Felucca's HAL and panel map; the
 * host implementation (host/drive_host.c) is a simulator fed by a script, which is
 * what lets the whole app — UI, sequencer, engine — run and be tested on a computer.
 * Drawing goes through Felucca's lcd_fill / lcd_blit (lcd.c on the device, a
 * framebuffer on the host) and gfx.c on top. */
#pragma once
#include <stdint.h>

/* big zero-initialised buffers go in the .pool RAM region on the device */
#ifdef OM_HOST
#define OM_POOL
#else
#define OM_POOL __attribute__((section(".pool")))
#endif

/* panel controls, by printed label (Felucca's panel.c map turns these into matrix ids; Felucca
 * calls the second button SCL, the panel prints SEL) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

#define NKEYS 27                       /* F3 .. G5 */

uint32_t plat_ms(void);
uint32_t plat_buttons(void);           /* held: bit B_* */
uint32_t plat_keys(void);              /* held: bit k = key k (0 = F3) */
int32_t plat_enc(int role);            /* detents since the last call, + = clockwise; EN_* */
uint32_t plat_master(void);            /* MASTER pot, 0..4096 gain Q12 */
int plat_battery(void);                /* 0..3 bars; 4: powered over USB; -1: not read yet */
int plat_battery_adc(void);            /* the battery divider's ADC reading, smoothed (0..1023); -1: none */
void plat_leds(uint32_t buttons, uint32_t keys);   /* lit: bit B_*, bit key */
void plat_glow(int on);                /* the unlit buttons glow dimly, readable in the dark (call before plat_leds) */
void plat_play_red(int red);           /* PLAY lights green; red (its other LED) while recording (before plat_leds) */

/* USB MIDI, 4-byte USB-MIDI event packets (cable 0) */
int plat_midi_in(uint32_t *pkt);       /* 1 = got one (called from the audio ISR) */
void plat_midi_out(uint32_t pkt);      /* queue one (audio ISR or main loop) */

/* persistent storage (Felucca storage.c objects: A/B copies, CRC, torn-write safe) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max);   /* bytes loaded, < 0 = none / bad */
int plat_store_save(uint32_t obj, const void *src, uint32_t len);   /* 0 = ok */
#define PLAT_STORE_MAX 3840u           /* bytes in an object (settings, panel) */
enum { OBJ_SET, OBJ_PANEL, OBJ_NOBJ };
/* flash: OBJ_SET 0xFC000, OBJ_PANEL 0xDC000, a sector a copy */

/* the chunk store (storage.c): numbered objects of any size, each saved as a new version that
 * replaces the one before only once it is complete. FM1-TRAX keeps the songs and the instrument
 * library there (app.h: OBJ_SONG, OBJ_LIB). The bytes are the app's. */
#define PLAT_NOBJ 512
void plat_obj_scan(void);              /* boot */
uint32_t plat_obj_bytes(int obj);      /* 0: no such object */
uint32_t plat_obj_room(void);          /* bytes a new object could take */
uint32_t plat_obj_capacity(void);      /* bytes in all */
int plat_obj_wopen(int obj);           /* 0; then plat_obj_write, plat_obj_wclose */
int plat_obj_write(const void *src, uint32_t n);
int plat_obj_wclose(void);             /* 0 = saved and verified, -10 = no room */
void plat_obj_wabort(void);
int plat_obj_open(int obj);            /* 0: it is there; read it with plat_obj_read */
int plat_obj_read(void *dst, uint32_t n);   /* bytes read (fewer at the end), -2 unreadable */
int plat_obj_delete(int obj);          /* 0 = gone */
uint32_t plat_obj_epoch(void);         /* changes with every plat_obj_open (the reader is shared) */

/* audio load in % of the render budget, and xruns since boot (device: from the ISR) */
uint32_t plat_cpu_pct(void);
uint32_t plat_cpu_peak_pct(void);   /* the slowest audio block since the last call, % of its time */
uint32_t plat_xruns(void);
