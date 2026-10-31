/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: plat.h's storage over storage.c, while flash_ok (the flash answered at boot). Included by
 * plat_fm1.c and by the host simulator, after storage.c. */
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }
void plat_obj_scan(void)
{
    if (flash_ok)
        cs_scan();
}
uint32_t plat_obj_bytes(int obj) { return cs_bytes((uint32_t)obj); }
uint32_t plat_obj_room(void) { return flash_ok ? cs_room() : 0u; }
uint32_t plat_obj_capacity(void) { return cs_capacity(); }
int plat_obj_wopen(int obj) { return flash_ok ? cs_wopen((uint32_t)obj) : -9; }
int plat_obj_write(const void *src, uint32_t n) { return cs_write(src, n); }
int plat_obj_wclose(void) { return cs_wclose(); }
void plat_obj_wabort(void) { cs_wabort(); }
int plat_obj_open(int obj) { return flash_ok ? cs_open((uint32_t)obj) : -1; }
int plat_obj_read(void *dst, uint32_t n) { return cs_read(dst, n); }
int plat_obj_delete(int obj) { return flash_ok ? cs_delete((uint32_t)obj) : -9; }
uint32_t plat_obj_epoch(void) { return cs_r_epoch; }
