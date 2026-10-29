/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * X0X / OMNI: object map changed. FM1-GM: the songs in a store of their own. FM1-DRIVE: that store
 * is a chunk store, for the songs and the instrument library. */
/* Persistent storage on the SPI NOR.
 *
 * Every object has an A/B sector pair. A save goes to the copy that is not
 * the current one: erase the sector, program the payload (from offset 256),
 * then the 32-byte header at offset 0 last. The header is the commit record;
 * on load the valid copy with the highest seq wins, so a torn write leaves
 * the previous copy in charge.
 *
 * Flash access goes through three hooks (also used by the host test):
 *   st_read(off, dst, n)   st_erase(off)   st_prog(off, src, n)
 */
#define ST_MAGIC 0x30583058u                   /* "X0X0": never mistaken for a Felucca object */
#define ST_SECTOR 4096u
#define ST_PAYLOAD_OFF 256u
#define ST_PAYLOAD_MAX (ST_SECTOR - ST_PAYLOAD_OFF)

/* flash map, FM1-GM's and FM1-DRIVE's (plat.h OBJ_*): the settings at 0xFC000 (Felucca's FL_GLOB), the panel calibration
 * at 0xDC000 (as OMNI), each one sector a copy. Every other sector of Felucca's data area
 * (0x97000..0xDFFFF) belongs to the chunk store, below. Nothing above 0xDFFFF but the
 * settings: 0xE0000.. is the update loader's staging area, 0xEA000.. the stock firmware's user
 * patches (kept). The magic is X0X's, so an object is told apart by its own magic. */
typedef struct {
    uint32_t magic;
    uint16_t type, slot;
    uint32_t seq, len, crc, rsv[2];
    uint32_t hcrc;
} st_hdr_t;

static int st_read(uint32_t off, void *dst, uint32_t n);
static int st_erase(uint32_t off);
static int st_prog(uint32_t off, const void *src, uint32_t n);

static uint32_t st_crc_add(uint32_t c, const void *p, uint32_t n)   /* zlib CRC-32, 4 bits per step */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    c = ~c;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}
static uint32_t st_crc32(const void *p, uint32_t n) { return st_crc_add(0, p, n); }

static uint32_t st_max(uint32_t obj) { (void)obj; return ST_PAYLOAD_MAX; }

static uint32_t st_sector(uint32_t obj, uint32_t copy)  /* flash offset of copy A (0) / B (1) */
{
    return (obj == OBJ_PANEL ? 0xDC000u : 0xFC000u) + copy * ST_SECTOR;
}

static uint8_t st_buf[ST_SECTOR] __attribute__((aligned(4)));   /* an object's payload, or a song sector's */

static int st_head(uint32_t obj, uint32_t copy, st_hdr_t *h)   /* commit record valid: 0 */
{
    if (st_read(st_sector(obj, copy), h, sizeof *h))
        return -1;
    if (h->magic != ST_MAGIC || h->type != obj || h->len > st_max(obj) ||
        h->hcrc != st_crc32(h, sizeof *h - 4u))
        return -1;
    return 0;
}

static int st_body(uint32_t obj, uint32_t copy, const st_hdr_t *h)   /* payload -> st_buf, CRC ok: 0 */
{
    if (st_read(st_sector(obj, copy) + ST_PAYLOAD_OFF, st_buf, h->len) || st_crc32(st_buf, h->len) != h->crc)
        return -1;
    return 0;
}

/* the current copy: the valid one with the highest seq (A on a tie), -1 when
 * neither is valid. Headers first, so only the winner's payload is read (it
 * is left in st_buf); *h gets its header. */
static int st_current(uint32_t obj, st_hdr_t *h)
{
    st_hdr_t a, b;
    int va = st_head(obj, 0, &a) == 0, vb = st_head(obj, 1, &b) == 0;
    if (vb && (!va || b.seq > a.seq)) {
        if (st_body(obj, 1, &b) == 0) {
            *h = b;
            return 1;
        }
        vb = 0;
    }
    if (va && st_body(obj, 0, &a) == 0) {
        *h = a;
        return 0;
    }
    if (vb && st_body(obj, 1, &b) == 0) {
        *h = b;
        return 1;
    }
    return -1;
}

/* load object into dst (up to max bytes); returns the length, or -1 */
static int st_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t i;
    st_hdr_t h;
    if (st_current(obj, &h) < 0)
        return -1;
    if (h.len > max)
        h.len = max;
    for (i = 0; i < h.len; i++)
        ((uint8_t *)dst)[i] = st_buf[i];
    return (int)h.len;
}

static int st_save(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t seq, base, off;
    int cur, rc;
    st_hdr_t h;
    if (obj >= OBJ_NOBJ || len > st_max(obj))
        return -1;
    cur = st_current(obj, &h);
    seq = cur < 0 ? 0u : h.seq;
    base = st_sector(obj, cur == 0 ? 1u : 0u);       /* write the other copy */
    for (off = 0; off < len; off++)
        st_buf[off] = ((const uint8_t *)src)[off];    /* the driver wants RAM sources */
    if ((rc = st_erase(base)) != 0)
        return rc;
    for (off = 0; off < len; off += 256u) {
        uint32_t n = len - off > 256u ? 256u : len - off;
        if ((rc = st_prog(base + ST_PAYLOAD_OFF + off, st_buf + off, n)) != 0)
            return rc;
    }
    h.magic = ST_MAGIC;
    h.type = (uint16_t)obj;
    h.slot = (uint16_t)(cur == 0 ? 1 : 0);
    h.seq = seq + 1u;
    h.len = len;
    h.crc = st_crc32(st_buf, len);
    h.rsv[0] = h.rsv[1] = 0xFFFFFFFFu;
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if ((rc = st_prog(base, &h, sizeof h)) != 0)       /* the commit record, last */
        return rc;
    {   /* read back: a write-protected or failing part must not report SAVED */
        st_hdr_t chk;
        uint32_t c = cur == 0 ? 1u : 0u;
        if (st_head(obj, c, &chk) || chk.seq != h.seq || st_body(obj, c, &chk))
            return -7;
    }
    return 0;
}

/* ------------------------------------------------------------- the chunk store --- *
 * FM1-DRIVE. Every sector of the data area that is not the panel's (0x97000..0xDBFFF and
 * 0xDE000..0xDFFFF, CS_NSEC sectors) is cut into 256-byte chunks, one flash page each. The store
 * keeps numbered objects (plat.h: the songs, the library's instruments), each saved as a new
 * version: its bytes in as many chunks as they take, each chunk programmed whole (header, payload
 * and CRC in one page program) and found by its header: object, version (a global number, each
 * save the next), part, and in the last part the number of parts. An object is its newest version
 * with every part present and right, so a save never touches the version it replaces and a torn
 * one leaves it in charge. Chunks of older versions, of torn saves, of another firmware and of
 * nothing are dead; a sector of dead and free chunks is erased when room is needed, and a sector
 * that also holds live chunks is compacted first (its live chunks copied elsewhere: a copy is the
 * same chunk, so a torn compaction leaves two of it, which is fine). One sector's worth of chunks
 * is always kept free for that. Deleting programs the object's chunks' magic to zero, its older
 * versions first. Nothing here knows the objects' formats: the app streams bytes in and out. */
#define CS_MAGIC 0xD71Cu
#define CS_HDR 20u
#define CS_PAY (256u - CS_HDR)               /* 236 bytes a chunk */
#define CS_LO_N 69u                          /* 0x97000..0xDBFFF; then two at 0xDE000 */
#define CS_NSEC (CS_LO_N + 2u)
#define CS_PER 16u                           /* chunks a sector */
#define CS_N (CS_NSEC * CS_PER)
#define CS_FREE 0xFFFFu                      /* cs_obj[]: erased */
#define CS_DEAD 0xFFFEu                      /*           neither erased nor a valid chunk */
#define CS_RESERVE CS_PER                    /* free chunks kept for compaction */
#ifndef CS_NOBJ
#define CS_NOBJ PLAT_NOBJ
#endif

typedef struct {
    uint16_t magic, obj;
    uint32_t gen;
    uint16_t part, nparts;                     /* nparts: in the last part; 0 in the others */
    uint16_t len, rsv;                         /* this part's bytes */
    uint32_t crc;                              /* of the header before it and the payload */
} cs_hdr_t;

static uint16_t cs_obj[CS_N] __attribute__((section(".pool")));   /* each chunk: its object, or CS_FREE / CS_DEAD */
static uint16_t cs_part[CS_N] __attribute__((section(".pool")));
static uint32_t cs_gen[CS_N] __attribute__((section(".pool")));
static uint32_t cs_cur[CS_NOBJ] __attribute__((section(".pool")));      /* each object's version, 0: none */
static uint32_t cs_size[CS_NOBJ] __attribute__((section(".pool")));     /* its bytes */
static uint32_t cs_top, cs_next;             /* the highest version seen; the allocation cursor */
static uint8_t cs_pg[256] __attribute__((aligned(4)));

static uint32_t cs_addr(uint32_t c)
{
    uint32_t s = c / CS_PER;
    return (s < CS_LO_N ? 0x97000u + s * ST_SECTOR : 0xDE000u + (s - CS_LO_N) * ST_SECTOR) + (c % CS_PER) * 256u;
}

/* writing: the version being written counts as live (compaction keeps it) */
static struct {
    uint32_t obj, gen, part, fill;
    int open, err;
    uint8_t buf[CS_PAY];
} cs_w;

static int cs_live(uint32_t c)
{
    uint32_t o = cs_obj[c];
    if (o >= CS_NOBJ)
        return 0;
    return cs_gen[c] == cs_cur[o] || (cs_w.open && o == cs_w.obj && cs_gen[c] == cs_w.gen);
}

static uint32_t cs_hcrc(const cs_hdr_t *h, const uint8_t *pay)
{
    return st_crc_add(st_crc32(h, CS_HDR - 4u), pay, h->len);
}

/* chunk c -> cs_pg; its header if valid (0), else -1 (and *erased: all 0xFF) */
static int cs_load(uint32_t c, cs_hdr_t *h, int *erased)
{
    uint32_t i;
    *erased = 0;
    if (st_read(cs_addr(c), cs_pg, 256u))
        return -1;
    memcpy(h, cs_pg, sizeof *h);
    if (h->magic == CS_MAGIC && h->obj < CS_NOBJ && h->len <= CS_PAY && h->gen &&
        h->crc == cs_hcrc(h, cs_pg + CS_HDR))
        return 0;
    for (i = 0; i < 256u && cs_pg[i] == 0xFFu; i++)
        ;
    *erased = i == 256u;
    return -1;
}

static int cs_complete(uint32_t o, uint32_t g, uint32_t *bytes)   /* version g of o has all its parts: 1 */
{
    uint32_t c, np = 0, last = 0, p, k;
    for (c = 0; c < CS_N; c++)
        if (cs_obj[c] == o && cs_gen[c] == g && cs_part[c] & 0x8000u) {
            np = (cs_part[c] & 0x7FFFu) + 1u;  /* the last part (flagged in the index) */
            last = c;
        }
    if (!np)
        return 0;
    for (p = 0; p < np; p++) {
        for (c = 0; c < CS_N; c++)
            if (cs_obj[c] == o && cs_gen[c] == g && (cs_part[c] & 0x7FFFu) == p)
                break;
        if (c == CS_N)
            return 0;
    }
    {
        cs_hdr_t h;
        int e;
        if (cs_load(last, &h, &e))
            return 0;
        k = (np - 1u) * CS_PAY + h.len;
    }
    *bytes = k;
    return 1;
}

static uint32_t cs_free_n(void)
{
    uint32_t c, n = 0;
    for (c = 0; c < CS_N; c++)
        n += cs_obj[c] == CS_FREE;
    return n;
}

static int cs_erase_sector(uint32_t s)
{
    uint32_t c;
    int rc = st_erase(cs_addr(s * CS_PER));
    for (c = s * CS_PER; c < (s + 1u) * CS_PER; c++)
        cs_obj[c] = rc ? CS_DEAD : CS_FREE;
    return rc;
}

static int cs_take(void);

/* make room: the sector with the most dead chunks (then the fewest live ones; then the one the
 * cursor wrote longest ago) is erased, its live chunks copied out first. 0, or -10: nothing to win */
static int cs_reclaim(void)
{
    uint32_t k, s, c, best = CS_NSEC, best_dead = 0, best_live = CS_PER;
    for (k = 1; k <= CS_NSEC; k++) {
        uint32_t live = 0, dead = 0;
        s = (cs_next / CS_PER + k) % CS_NSEC;
        for (c = s * CS_PER; c < (s + 1u) * CS_PER; c++) {
            live += (uint32_t)cs_live(c);
            dead += cs_obj[c] != CS_FREE && !cs_live(c);
        }
        if (dead > best_dead || (dead && dead == best_dead && live < best_live)) {
            best = s;
            best_dead = dead;
            best_live = live;
        }
    }
    if (best == CS_NSEC)
        return -10;
    s = best;
    for (c = s * CS_PER; c < (s + 1u) * CS_PER; c++)    /* its free chunks go with it: not targets */
        if (cs_obj[c] == CS_FREE)
            cs_obj[c] = CS_DEAD;
    for (c = s * CS_PER; c < (s + 1u) * CS_PER; c++) {   /* move the live chunks out */
        cs_hdr_t h;
        int e, d, rc;
        if (!cs_live(c))
            continue;
        if (cs_load(c, &h, &e))
            return -7;
        cs_obj[c] = CS_DEAD;                   /* not a target, and moved */
        d = cs_take();
        if (d < 0)
            return -10;
        if ((rc = st_prog(cs_addr((uint32_t)d), cs_pg, 256u)) != 0)
            return rc;
        cs_obj[d] = h.obj;
        cs_part[d] = (uint16_t)(h.part | (h.nparts ? 0x8000u : 0u));
        cs_gen[d] = h.gen;
    }
    return cs_erase_sector(s);
}

/* a free chunk from the cursor (outside the sector being compacted: it has none free), or -1 */
static int cs_take(void)
{
    uint32_t k;
    for (k = 0; k < CS_N; k++) {
        uint32_t c = (cs_next + k) % CS_N;
        if (cs_obj[c] == CS_FREE) {
            cs_next = (c + 1u) % CS_N;
            return (int)c;
        }
    }
    return -1;
}

void cs_scan(void)
{
    uint32_t c, o;
    cs_top = 0;
    memset(&cs_w, 0, sizeof cs_w);
    for (o = 0; o < CS_NOBJ; o++)
        cs_cur[o] = cs_size[o] = 0;
    for (c = 0; c < CS_N; c++) {
        cs_hdr_t h;
        int e;
        if (cs_load(c, &h, &e)) {
            cs_obj[c] = e ? CS_FREE : CS_DEAD;
            continue;
        }
        cs_obj[c] = h.obj;
        cs_part[c] = (uint16_t)(h.part | (h.nparts ? 0x8000u : 0u));
        cs_gen[c] = h.gen;
        if (h.gen > cs_top)
            cs_top = h.gen;
    }
    for (c = 0; c < CS_N; c++) {               /* each object: its newest complete version */
        uint32_t b;
        o = cs_obj[c];
        if (o >= CS_NOBJ || !(cs_part[c] & 0x8000u) || cs_gen[c] <= cs_cur[o])
            continue;
        if (cs_complete(o, cs_gen[c], &b)) {
            cs_cur[o] = cs_gen[c];
            cs_size[o] = b;
        }
    }
    while (cs_free_n() < CS_RESERVE && cs_reclaim() == 0)   /* a fresh store, or another firmware's */
        ;
    cs_next = 0;
}

uint32_t cs_bytes(uint32_t o) { return o < CS_NOBJ && cs_cur[o] ? cs_size[o] : 0u; }

/* the room a new object could take, in bytes (dead chunks count: they are reclaimed) */
uint32_t cs_room(void)
{
    uint32_t c, n = 0;
    for (c = 0; c < CS_N; c++)
        n += !cs_live(c);
    return n > CS_RESERVE ? (n - CS_RESERVE) * CS_PAY : 0u;
}
uint32_t cs_capacity(void) { return (CS_N - CS_RESERVE) * CS_PAY; }

/* writing: cs_wopen, cs_write as often as needed, cs_wclose (0: saved and verified, -10: no room,
 * else a flash error). The version before stays the object until cs_wclose commits. */
static int cs_flush(int last)
{
    cs_hdr_t h;
    int c, rc;
    while (cs_free_n() <= CS_RESERVE)
        if ((rc = cs_reclaim()) != 0)
            return rc;
    c = cs_take();
    if (c < 0)
        return -10;
    memset(cs_pg, 0xFF, sizeof cs_pg);
    h.magic = CS_MAGIC;
    h.obj = (uint16_t)cs_w.obj;
    h.gen = cs_w.gen;
    h.part = (uint16_t)cs_w.part;
    h.nparts = (uint16_t)(last ? cs_w.part + 1u : 0u);
    h.len = (uint16_t)cs_w.fill;
    h.rsv = 0xFFFFu;
    h.crc = cs_hcrc(&h, cs_w.buf);
    memcpy(cs_pg, &h, sizeof h);
    memcpy(cs_pg + CS_HDR, cs_w.buf, cs_w.fill);
    cs_obj[c] = CS_DEAD;                       /* until it reads back right */
    if ((rc = st_prog(cs_addr((uint32_t)c), cs_pg, 256u)) != 0)
        return rc;
    {
        cs_hdr_t chk;
        int e;
        if (cs_load((uint32_t)c, &chk, &e) || chk.gen != h.gen || chk.part != h.part)
            return -7;
    }
    cs_obj[c] = h.obj;
    cs_part[c] = (uint16_t)(h.part | (last ? 0x8000u : 0u));
    cs_gen[c] = h.gen;
    cs_w.part++;
    cs_w.fill = 0;
    return 0;
}

int cs_wopen(uint32_t o)
{
    if (o >= CS_NOBJ)
        return -1;
    if (cs_w.open)
        return -5;                             /* one write at a time */
    cs_w.obj = o;
    cs_w.gen = ++cs_top;
    cs_w.part = cs_w.fill = 0;
    cs_w.open = 1;
    cs_w.err = 0;
    return 0;
}

int cs_write(const void *src, uint32_t n)
{
    const uint8_t *p = src;
    if (!cs_w.open)
        return -1;
    while (n && !cs_w.err) {
        uint32_t k = CS_PAY - cs_w.fill;
        if (k > n)
            k = n;
        memcpy(cs_w.buf + cs_w.fill, p, k);
        cs_w.fill += k;
        p += k;
        n -= k;
        if (cs_w.fill == CS_PAY && n)          /* the last part is written by cs_wclose */
            cs_w.err = cs_flush(0);
    }
    return cs_w.err;
}

int cs_wclose(void)
{
    uint32_t size = cs_w.part * CS_PAY + cs_w.fill;
    int rc;
    if (!cs_w.open)
        return -1;
    rc = cs_w.err ? cs_w.err : cs_flush(1);
    cs_w.open = 0;
    if (rc == 0) {
        cs_cur[cs_w.obj] = cs_w.gen;           /* every part is in: the new version is the object */
        cs_size[cs_w.obj] = size;
    }
    return rc;
}

void cs_wabort(void) { cs_w.open = 0; }

int cs_delete(uint32_t o)                      /* 0, or a flash error */
{
    static const uint8_t zero[2] = {0, 0};
    uint32_t c, pass;
    int rc;
    if (o >= CS_NOBJ)
        return -1;
    for (pass = 0; pass < 2u; pass++)           /* the older versions, then the object */
        for (c = 0; c < CS_N; c++)
            if (cs_obj[c] == o && (pass == 1u) == (cs_gen[c] == cs_cur[o])) {
                if ((rc = st_prog(cs_addr(c), zero, 2u)) != 0)
                    return rc;
                cs_obj[c] = CS_DEAD;
            }
    cs_cur[o] = cs_size[o] = 0;
    return 0;
}

/* reading: cs_open, then cs_read until it returns less than asked */
static struct {
    uint32_t obj, part, pos, len, np;
    int bad;
    uint8_t buf[CS_PAY];
} cs_r;
static uint32_t cs_r_epoch;                    /* each cs_open: a reader that was interrupted knows */

static int cs_fetch(void)                      /* the next part -> cs_r.buf: 0 */
{
    uint32_t c;
    for (c = 0; c < CS_N; c++)
        if (cs_obj[c] == cs_r.obj && cs_gen[c] == cs_cur[cs_r.obj] && (cs_part[c] & 0x7FFFu) == cs_r.part) {
            cs_hdr_t h;
            int e;
            if (cs_load(c, &h, &e) || h.gen != cs_cur[cs_r.obj])
                continue;                      /* a copy may be bad: try another */
            memcpy(cs_r.buf, cs_pg + CS_HDR, h.len);
            cs_r.len = h.len;
            cs_r.pos = 0;
            if (h.nparts)
                cs_r.np = h.nparts;
            return 0;
        }
    return -1;
}

int cs_open(uint32_t o)                        /* 0: the object is there */
{
    if (o >= CS_NOBJ || !cs_cur[o])
        return -1;
    cs_r_epoch++;
    cs_r.obj = o;
    cs_r.part = 0;
    cs_r.np = 0xFFFFu;
    cs_r.bad = cs_fetch();
    return cs_r.bad;
}

/* up to n bytes; returns how many (fewer at the end), or -2 unreadable */
int cs_read(void *dst, uint32_t n)
{
    uint8_t *d = dst;
    uint32_t got = 0;
    while (got < n) {
        if (cs_r.bad)
            return -2;
        if (cs_r.pos >= cs_r.len) {
            if (cs_r.part + 1u >= cs_r.np)
                break;
            cs_r.part++;
            if ((cs_r.bad = cs_fetch()) != 0)
                return -2;
            continue;
        }
        {
            uint32_t k = cs_r.len - cs_r.pos;
            if (k > n - got)
                k = n - got;
            memcpy(d + got, cs_r.buf + cs_r.pos, k);
            cs_r.pos += k;
            got += k;
        }
    }
    return (int)got;
}
