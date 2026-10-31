/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: VGM export (vgm.h). */
#include "vgm.h"
#include "player.h"

static struct {
    uint8_t *buf;
    uint32_t cap, len, last;
    uint32_t loop_off, loop_t;
    uint8_t loop_set, over;
    uint32_t dac_next, dac_cmd;        /* the data bank's next byte; where the last 0x80 is (+1) */
    uint32_t off[256];                 /* each bank sample's place in the data block, ~0: not in it */
} v;

static void put(uint32_t b)
{
    if (v.len < v.cap)
        v.buf[v.len++] = (uint8_t)b;
    else
        v.over = 1;
}
static void put32_at(uint32_t at, uint32_t x)
{
    if (at + 4u > v.cap)
        return;
    v.buf[at] = (uint8_t)x;
    v.buf[at + 1] = (uint8_t)(x >> 8);
    v.buf[at + 2] = (uint8_t)(x >> 16);
    v.buf[at + 3] = (uint8_t)(x >> 24);
}

static void wait_to(uint32_t now)
{
    uint32_t d = now - v.last;
    v.last = now;
    if (d && d <= 15u && v.dac_cmd && v.dac_cmd == v.len && v.len <= v.cap) {
        v.buf[v.len - 1u] = (uint8_t)(0x80u + d);   /* the DAC write waits too: 0x8n */
        v.dac_cmd = 0;
        return;
    }
    while (d) {
        if (d == 735u) {
            put(0x62);
            d = 0;
        } else if (d == 882u) {
            put(0x63);
            d = 0;
        } else if (d <= 16u) {
            put(0x70u + d - 1u);
            d = 0;
        } else {
            uint32_t k = d > 65535u ? 65535u : d;
            put(0x61);
            put(k);
            put(k >> 8);
            d -= k;
        }
    }
}

static void log_write(void *ctx, int kind, int port, uint8_t reg, uint8_t val)
{
    (void)ctx;
    wait_to(player.md.vgm_samples);
    if (kind == MD_LOG_DAC) {
        const uint8_t *at = player.md.dac_at;
        uint32_t i;
        for (i = 0; i < bank_n && i < 256u; i++)
            if (v.off[i] != 0xFFFFFFFFu && at >= bank[i].data && at < bank[i].data + bank[i].len) {
                uint32_t o = v.off[i] + (uint32_t)(at - bank[i].data);
                if (o != v.dac_next) {
                    put(0xE0);
                    put(o);
                    put(o >> 8);
                    put(o >> 16);
                    put(o >> 24);
                }
                put(0x80);
                v.dac_cmd = v.len;
                v.dac_next = o + 1u;
                return;
            }
        put(0x52);
        put(0x2A);
        put(val);
    } else if (kind == MD_LOG_PSG) {
        put(0x50);
        put(val);
    } else {
        put(port ? 0x53 : 0x52);
        put(reg);
        put(val);
    }
}

static void row_hook(void)
{
    if (!v.loop_set && player.ord == song.loop && player.row == 0) {
        wait_to(player.md.vgm_samples);
        v.loop_set = 1;
        v.loop_off = v.len;
        v.loop_t = player.md.vgm_samples;
        v.dac_next = 0xFFFFFFFFu;      /* after the loop the data bank's pointer is wherever the end left it */
        v.dac_cmd = 0;
    }
}

static void gd3_str(const char *s)
{
    while (s && *s) {
        put((uint8_t)*s++);
        put(0);
    }
    put(0);
    put(0);
}

uint32_t vgm_export(uint8_t *buf, uint32_t cap, uint32_t max_s)
{
    static int32_t scratch[2u * 256u];
    uint32_t i, total, gd3, gd3_len_at, end_samples;
    v.buf = buf;
    v.cap = cap;
    v.len = 0;
    v.last = 0;
    v.loop_set = v.over = 0;
    for (i = 0; i < 0x100u; i++)
        put(0);
    /* the data block: the samples the song's DAC instruments play */
    {
        uint32_t k, size = 0, at;
        for (k = 0; k < 256u; k++)
            v.off[k] = 0xFFFFFFFFu;
        for (k = 0; k < MAX_INS; k++)
            if (song.ins[k].type == INS_DAC && song.ins[k].u.dac.sample < bank_n && v.off[song.ins[k].u.dac.sample] == 0xFFFFFFFFu) {
                v.off[song.ins[k].u.dac.sample] = size;
                size += bank[song.ins[k].u.dac.sample].len;
            }
        if (size) {
            put(0x67);
            put(0x66);
            put(0x00);
            put(size);
            put(size >> 8);
            put(size >> 16);
            put(size >> 24);
            at = v.len;
            for (k = 0; k < bank_n && k < 256u; k++)
                if (v.off[k] != 0xFFFFFFFFu) {
                    uint32_t j;
                    for (j = 0; j < bank[k].len; j++)
                        if (at + v.off[k] + j < v.cap)
                            v.buf[at + v.off[k] + j] = bank[k].data[j];
                }
            if (at + size > v.cap)
                v.over = 1;
            else
                v.len = at + size;
        }
        v.dac_next = 0xFFFFFFFFu;
        v.dac_cmd = 0;
    }
    player_post(PC_STOP, 0, 0, 0);
    player_run_commands();
    /* the play command resets the chips: its writes are the file's first, at time 0 */
    player.md.log = log_write;
    player.md.vgm_samples = 0;
    player.stop_at_end = 1;
    player.row_hook = row_hook;
    player_post(PC_PLAY, 0, 0, 0);
    player_run_commands();
    while (player.playing && player.md.vgm_samples < max_s * 44100u && !v.over) {
        uint32_t i;
        for (i = 0; i < 2u * 256u; i++)
            scratch[i] = 0;
        player_render((int32_t *)scratch, 256);
    }
    end_samples = player.md.vgm_samples;
    wait_to(end_samples);
    put(0x66);
    player.md.log = 0;
    player.row_hook = 0;
    player.stop_at_end = 0;
    player_post(PC_STOP, 0, 0, 0);
    player_run_commands();
    if (v.over)
        return 0;
    gd3 = v.len;
    put('G');
    put('d');
    put('3');
    put(' ');
    put(0x00);
    put(0x01);
    put(0);
    put(0);
    gd3_len_at = v.len;
    put(0);
    put(0);
    put(0);
    put(0);
    gd3_str(song.title[0] ? song.title : "Untitled");
    gd3_str("");
    gd3_str("");
    gd3_str("");
    gd3_str(song.region ? "Sega Mega Drive" : "Sega Genesis / Mega Drive");
    gd3_str("");
    gd3_str(song.author);
    gd3_str("");
    gd3_str("");
    gd3_str("FM1-DRIVE");
    gd3_str("Made on an FM-1 with FM1-DRIVE");
    if (v.over)
        return 0;
    put32_at(gd3_len_at, v.len - gd3_len_at - 4u);
    total = v.len;
    v.buf[0] = 'V';
    v.buf[1] = 'g';
    v.buf[2] = 'm';
    v.buf[3] = ' ';
    put32_at(0x04, total - 4u);
    put32_at(0x08, 0x171);
    put32_at(0x0C, song.region ? 3546893u : 3579545u);
    put32_at(0x14, gd3 - 0x14u);
    put32_at(0x18, end_samples);
    if (v.loop_set && v.loop_t < end_samples) {
        put32_at(0x1C, v.loop_off - 0x1Cu);
        put32_at(0x20, end_samples - v.loop_t);
    }
    put32_at(0x24, song.region ? 50u : 60u);
    v.buf[0x28] = 0x09;                  /* SN76489: Sega's taps (bits 0 and 3), 16-bit register */
    v.buf[0x29] = 0x00;
    v.buf[0x2A] = 16;
    v.buf[0x2B] = 0;
    put32_at(0x2C, song.region ? 7600489u : 7670453u);
    put32_at(0x34, 0x100u - 0x34u);
    return total;
}
