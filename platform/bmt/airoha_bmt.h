/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Airoha/EcoNet vendor BMT+BBT translation, the read side.
 *
 * The vendor's logical->physical NAND mapping, as stock U-Boot and the stock
 * kernel apply it. The same file is compiled into our U-Boot, our Linux and a
 * host test (tests/test_platform_bmt.py), so the loader and the kernel agree
 * block for block with each other and with stock.
 *
 * Translation is two stages, in the vendor's order:
 *     logical --[BBT shift]--> physical --[BMT substitution]--> final
 *
 * The reserve pool is the vendor's: a downward scan of the factory markers,
 * a ceiling and a clamp to the 250-entry tables (airoha_bmt_vendor_pool). On
 * the W25N04KV (4096 blocks) the clamp always fires and the pool is 250; on
 * the W25N04LV (2048 blocks) it does not, and the pool is 163 plus the factory
 * bad blocks the scan steps over. The BMT checksum covers the pool's entries.
 *
 * This is not OpenWrt's mtk_bmt: that computes an unclamped reserve pool, fails
 * the checksum of a vendor-written BMT, and then erases and rewrites the table.
 * This core clamps the pool as the vendor does, checks the checksum over the
 * pool's extent, and refuses a table that does not validate. It never writes;
 * replacement lives in airoha_bmt_write.c and airoha_bmt_transaction.c.
 */
#ifndef AIROHA_BMT_H
#define AIROHA_BMT_H

#if defined(AIROHA_BMT_HOST)
#include <stdint.h>
#include <string.h>
typedef uint8_t airoha_u8;
typedef uint16_t airoha_u16;
typedef uint32_t airoha_u32;
#else
#include <linux/types.h>
#include <linux/string.h>
typedef u8 airoha_u8;
typedef u16 airoha_u16;
typedef u32 airoha_u32;
#endif

/* Both tables hold at most 250 entries; the clamp and each table's capacity. */
#define AIROHA_BMT_MAX_ENTRIES 250
/* On-flash table sizes and layouts. */
#define AIROHA_BBT_BYTES 0x200
#define AIROHA_BMT_BYTES 0x3fc
#define AIROHA_BBT_HEADER 0xc
#define AIROHA_BMT_HEADER 0x14
/* The vendor's reserve pool: scan target total*8/100, ceiling total/8. */
#define AIROHA_GOOD_NUMERATOR 8
#define AIROHA_GOOD_DENOMINATOR 100
#define AIROHA_CEILING_DIVISOR 8

/* Negative return codes. Bindings map these onto -EIO/-EINVAL/-EROFS. */
enum {
	AIROHA_BMT_OK = 0,
	AIROHA_BMT_E_SHORT = -1,	/* buffer smaller than the table */
	AIROHA_BMT_E_SIGNATURE = -2,	/* wrong magic */
	AIROHA_BMT_E_VERSION = -3,	/* zero version; no vendor writer makes one */
	AIROHA_BMT_E_SIZE = -4,		/* entry count exceeds the 250 capacity */
	AIROHA_BMT_E_CHECKSUM = -5,	/* checksum mismatch at the pinned extent */
	AIROHA_BMT_E_UNPINNABLE = -6,	/* no geometry, or a pool the vendor rule cannot produce */
	AIROHA_BMT_E_RANGE = -7,	/* block outside the device after mapping */
	AIROHA_BMT_E_ORDER = -8,	/* BBT not strictly ascending */
	AIROHA_BMT_E_NOTLOADED = -9,	/* translate before both tables parsed */
	AIROHA_BMT_E_POOL = -10,	/* the vendor scan leaves no BMT (pool > total/8) */
	AIROHA_BMT_E_MARKER = -11,	/* a factory marker the pool scan needs is unreadable */
};

struct airoha_bmt {
	airoha_u16 total_blocks;	/* whole chip: 4096 (KV), 2048 (LV) */
	airoha_u16 system_blocks;	/* total minus the pool: 3846 (KV) */
	/* Page geometry, needed only by the write side (airoha_bmt_set_pages):
	 * main bytes per page, and free ECC-protected OOB bytes per page. */
	airoha_u16 page_bytes;
	airoha_u16 oob_bytes;
	airoha_u8 geometry_set;
	airoha_u8 bbt_loaded;
	airoha_u8 bmt_loaded;
	airoha_u8 bbt_ascending;
	airoha_u16 bbt_size;
	airoha_u16 bmt_size;
	airoha_u16 bbt_entries[AIROHA_BMT_MAX_ENTRIES];
	airoha_u16 bmt_from[AIROHA_BMT_MAX_ENTRIES];
	airoha_u16 bmt_to[AIROHA_BMT_MAX_ENTRIES];
};

/* The vendor's pool bounds for a part with `total_blocks` blocks: the scan's
 * target (total*8/100 good blocks, the least pool it returns) and the ceiling
 * (total/8) above which no BMT is initialised. */
airoha_u32 airoha_bmt_pool_floor(airoha_u32 total_blocks);
airoha_u32 airoha_bmt_pool_ceiling(airoha_u32 total_blocks);
/* The pool when the clamp fixes it without a scan (250 when the floor already
 * exceeds the 250-entry capacity, i.e. parts of ~3138 blocks or more), else 0. */
airoha_u32 airoha_bmt_clamped_pool(airoha_u32 total_blocks);

/* Reads one block's factory marker (OOB byte 0 of its first page): 1 when it
 * is not 0xff, 0 when it is, or a negative code when it cannot be read. */
typedef int (*airoha_factory_bad_fn)(void *ctx, airoha_u32 block);

/* The pool exactly as stock computes it at every boot (U-Boot 0x81e1b540 and
 * its kernel twin): counting down from the last block, the blocks whose
 * factory marker reads 0xff, until total*8/100 are found; the pool is total
 * minus the block where the count completes; above total/8 there is no BMT;
 * above 250 it is clamped to 250. Only the factory byte counts, so retiring a
 * block (OOB byte 1) never moves it. Where the clamp fixes the pool, nothing is
 * read. Returns the pool, or AIROHA_BMT_E_POOL / AIROHA_BMT_E_MARKER /
 * AIROHA_BMT_E_UNPINNABLE. Stock ignores a failed marker read and tests stale
 * bytes; this refuses instead, and reads a set marker twice (the byte has no
 * ECC), refusing when the reads disagree. */
int airoha_bmt_vendor_pool(airoha_u32 total_blocks, airoha_factory_bad_fn factory_bad,
			   void *ctx);

/* Set total_blocks and the pool, which must be one the vendor rule can return
 * for this part (from airoha_bmt_vendor_pool). Must precede parse_bmt (the
 * checksum extent is the pool's) and translate. */
int airoha_bmt_set_geometry(struct airoha_bmt *t, airoha_u32 total_blocks,
			    airoha_u32 pool_blocks);

/* The write side's page geometry: 2048- or 4096-byte pages, 64 to a block,
 * and the free OOB bytes per page (56 or 112 here). Not needed to translate. */
#define AIROHA_BMT_BLOCK_PAGES 64u
#define AIROHA_BMT_PAGE_MAX 4096u
#define AIROHA_BMT_OOB_MAX 112u
int airoha_bmt_set_pages(struct airoha_bmt *t, airoha_u32 page_bytes, airoha_u32 oob_bytes);
/* Reserve blocks: total_blocks - system_blocks. */
airoha_u32 airoha_bmt_pool_blocks(const struct airoha_bmt *t);

/* Parse one table from the first page of its reserve block. Idempotent; the
 * parse functions only read the buffer. */
int airoha_bmt_parse_bbt(struct airoha_bmt *t, const airoha_u8 *page, airoha_u32 len);
int airoha_bmt_parse_bmt(struct airoha_bmt *t, const airoha_u8 *page, airoha_u32 len);

/* logical block -> final physical block, or a negative code. Requires geometry
 * and both tables loaded, and a strictly ascending BBT. */
int airoha_bmt_translate(const struct airoha_bmt *t, airoha_u32 logical_block);

/* The shift amount at a logical block: physical-after-shift minus logical. This
 * is `f`. Negative code on error. */
int airoha_bmt_shift(const struct airoha_bmt *t, airoha_u32 logical_block);

/* The lowest logical block that maps to `final`, or AIROHA_BMT_E_RANGE if none.
 * Used by the round-trip check; linear over the usable area. */
int airoha_bmt_untranslate(const struct airoha_bmt *t, airoha_u32 final_block);

/* Usable blocks: system_blocks minus the BBT entry count (what stock publishes). */
int airoha_bmt_usable_blocks(const struct airoha_bmt *t);

#endif /* AIROHA_BMT_H */
