// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha/EcoNet vendor BMT+BBT translation, read side. See airoha_bmt.h. Every
 * rule here is the vendor's, as stock U-Boot and the stock kernel implement it.
 */
#include "airoha_bmt.h"

static const char BBT_MAGIC[4] = { 'R', 'A', 'W', 'B' };
static const char BMT_MAGIC[3] = { 'B', 'M', 'T' };

/* Little-endian reads by byte: the tables are LE on flash, and reading by byte
 * is correct regardless of the host's alignment rules. */
static airoha_u16 rd16(const airoha_u8 *p)
{
	return (airoha_u16)(p[0] | (p[1] << 8));
}

static airoha_u32 rd32(const airoha_u8 *p)
{
	return (airoha_u32)p[0] | ((airoha_u32)p[1] << 8) |
	       ((airoha_u32)p[2] << 16) | ((airoha_u32)p[3] << 24);
}

airoha_u32 airoha_bmt_pool_floor(airoha_u32 total_blocks)
{
	/* The least pool the vendor's downward good-block scan can return:
	 * total*8/100 good blocks, plus any bad block inside the scanned span. */
	return total_blocks * AIROHA_GOOD_NUMERATOR / AIROHA_GOOD_DENOMINATOR;
}

airoha_u32 airoha_bmt_pool_ceiling(airoha_u32 total_blocks)
{
	/* Above this, the vendor's bmt_init fails outright and no BMT exists. */
	return total_blocks / AIROHA_CEILING_DIVISOR;
}

airoha_u32 airoha_bmt_clamped_pool(airoha_u32 total_blocks)
{
	/* When the scan floor already exceeds the 250-entry capacity, every pool
	 * that survives the ceiling is clamped to the capacity, so the pool is
	 * fixed without reading the unit. On a smaller part (the W25N04LV's 2048
	 * blocks: floor 163) the clamp may not fire and the scan decides. */
	if (airoha_bmt_pool_floor(total_blocks) > AIROHA_BMT_MAX_ENTRIES)
		return AIROHA_BMT_MAX_ENTRIES;
	return 0;
}

int airoha_bmt_vendor_pool(airoha_u32 total_blocks, airoha_factory_bad_fn factory_bad,
			   void *ctx)
{
	airoha_u32 need = airoha_bmt_pool_floor(total_blocks), good = 0, block, pool;
	int bad;

	if (airoha_bmt_clamped_pool(total_blocks))
		return AIROHA_BMT_MAX_ENTRIES;
	if (!need || total_blocks > 0xffff || !factory_bad)
		return AIROHA_BMT_E_UNPINNABLE;
	/* Stock: for (b = total - 1; b != 0; b--) if (!is_bad(b, 0) && ++good ==
	 * need) break; return total - b. A scan that reaches block 0 returns
	 * total, which the ceiling then refuses. */
	for (block = total_blocks - 1; block; block--) {
		bad = factory_bad(ctx, block);
		/* The marker byte has no ECC. A good block's 0xff misread as set
		 * would move the pool by one; read a set marker again and refuse
		 * when the two reads disagree. A real factory mark reads 0x00. */
		if (bad > 0)
			bad = factory_bad(ctx, block) > 0 ? 1 : -1;
		if (bad < 0)
			return AIROHA_BMT_E_MARKER;
		if (!bad && ++good == need)
			break;
	}
	pool = total_blocks - block;
	if (pool > airoha_bmt_pool_ceiling(total_blocks))
		return AIROHA_BMT_E_POOL;
	return (int)(pool > AIROHA_BMT_MAX_ENTRIES ? AIROHA_BMT_MAX_ENTRIES : pool);
}

int airoha_bmt_set_geometry(struct airoha_bmt *t, airoha_u32 total_blocks,
			    airoha_u32 pool_blocks)
{
	airoha_u32 least = airoha_bmt_pool_floor(total_blocks);
	airoha_u32 most = airoha_bmt_pool_ceiling(total_blocks);

	memset(t, 0, sizeof(*t));
	if (total_blocks > 0xffff)
		return AIROHA_BMT_E_RANGE;
	/* Only a pool the vendor rule can produce: the scan returns floor..ceiling
	 * and the clamp caps it at 250. */
	if (least > AIROHA_BMT_MAX_ENTRIES)
		least = AIROHA_BMT_MAX_ENTRIES;
	if (most > AIROHA_BMT_MAX_ENTRIES)
		most = AIROHA_BMT_MAX_ENTRIES;
	if (!least || pool_blocks < least || pool_blocks > most)
		return AIROHA_BMT_E_UNPINNABLE;
	t->total_blocks = (airoha_u16)total_blocks;
	t->system_blocks = (airoha_u16)(total_blocks - pool_blocks);
	t->geometry_set = 1;
	return AIROHA_BMT_OK;
}

int airoha_bmt_set_pages(struct airoha_bmt *t, airoha_u32 page_bytes, airoha_u32 oob_bytes)
{
	if (!t->geometry_set || (page_bytes != 2048 && page_bytes != 4096) ||
	    oob_bytes < 4 || oob_bytes > AIROHA_BMT_OOB_MAX)
		return AIROHA_BMT_E_RANGE;
	t->page_bytes = (airoha_u16)page_bytes;
	t->oob_bytes = (airoha_u16)oob_bytes;
	return AIROHA_BMT_OK;
}

airoha_u32 airoha_bmt_pool_blocks(const struct airoha_bmt *t)
{
	return t->total_blocks - t->system_blocks;
}

int airoha_bmt_parse_bbt(struct airoha_bmt *t, const airoha_u8 *page, airoha_u32 len)
{
	airoha_u32 stored, computed, i;
	airoha_u8 version, size;

	t->bbt_loaded = 0;
	if (!t->geometry_set)
		return AIROHA_BMT_E_UNPINNABLE;
	if (len < AIROHA_BBT_BYTES)
		return AIROHA_BMT_E_SHORT;
	if (memcmp(page, BBT_MAGIC, sizeof(BBT_MAGIC)) != 0)
		return AIROHA_BMT_E_SIGNATURE;
	stored = rd32(page + 4);
	version = page[8];
	size = page[9];
	/* Both vendor writers write version 1 and pre-fill 0xff, so a zero version
	 * is not a vendor table; without this guard an all-zero block that happens
	 * to start with the signature validates trivially (size 0, checksum 0). */
	if (!version)
		return AIROHA_BMT_E_VERSION;
	if (size > AIROHA_BMT_MAX_ENTRIES)
		return AIROHA_BMT_E_SIZE;
	/* u16 accumulation over version, size and the whole fixed 500-byte entry
	 * area; compared against the stored u32 (the vendor writes a u16 into it). */
	computed = (airoha_u32)version + size;
	for (i = AIROHA_BBT_HEADER; i < AIROHA_BBT_BYTES; i++)
		computed += page[i];
	computed &= 0xffff;
	if (stored != computed)
		return AIROHA_BMT_E_CHECKSUM;
	/* Stock 0x81e16538: factory entries belong below system_blocks. */
	for (i = 0; i < size; i++)
		if (rd16(page + AIROHA_BBT_HEADER + 2 * i) >= t->system_blocks)
			return AIROHA_BMT_E_RANGE;
	t->bbt_size = size;
	t->bbt_ascending = 1;
	for (i = 0; i < size; i++) {
		t->bbt_entries[i] = rd16(page + AIROHA_BBT_HEADER + 2 * i);
		if (i && !(t->bbt_entries[i - 1] < t->bbt_entries[i]))
			t->bbt_ascending = 0;
	}
	t->bbt_loaded = 1;
	return AIROHA_BMT_OK;
}

int airoha_bmt_parse_bmt(struct airoha_bmt *t, const airoha_u8 *page, airoha_u32 len)
{
	airoha_u32 pool, end, computed, i;
	airoha_u8 version, size, stored;

	t->bmt_loaded = 0;
	if (!t->geometry_set)
		return AIROHA_BMT_E_UNPINNABLE;
	if (len < AIROHA_BMT_BYTES)
		return AIROHA_BMT_E_SHORT;
	if (memcmp(page, BMT_MAGIC, sizeof(BMT_MAGIC)) != 0)
		return AIROHA_BMT_E_SIGNATURE;
	version = page[3];
	size = page[5];
	stored = page[6];
	if (!version)
		return AIROHA_BMT_E_VERSION;
	if (size > AIROHA_BMT_MAX_ENTRIES)
		return AIROHA_BMT_E_SIZE;
	/* The extent is a runtime global the table does not carry: the pool,
	 * stored by its one writer after the scan and the clamp. On the KV it is
	 * always 250, the whole entry area; on the LV it is the scanned pool. A
	 * table matching another extent is refused: the vendor could not have
	 * written one, so it is a bad read. */
	pool = airoha_bmt_pool_blocks(t);
	end = AIROHA_BMT_HEADER + 4 * pool;		/* == AIROHA_BMT_BYTES at 250 */
	computed = (airoha_u32)version + size;
	for (i = AIROHA_BMT_HEADER; i < end; i++)
		computed += page[i];
	computed &= 0xff;
	if (stored != computed)
		return AIROHA_BMT_E_CHECKSUM;
	/* Stock 0x81e16ee4: source < total; destination in [system,total).
	 * Check all entries before making any of them available to translation. */
	for (i = 0; i < size; i++) {
		airoha_u32 from = rd16(page + AIROHA_BMT_HEADER + 4 * i);
		airoha_u32 to = rd16(page + AIROHA_BMT_HEADER + 4 * i + 2);

		if (from >= t->total_blocks || to < t->system_blocks ||
		    to >= t->total_blocks)
			return AIROHA_BMT_E_RANGE;
	}
	t->bmt_size = size;
	for (i = 0; i < size; i++) {
		t->bmt_from[i] = rd16(page + AIROHA_BMT_HEADER + 4 * i);
		t->bmt_to[i] = rd16(page + AIROHA_BMT_HEADER + 4 * i + 2);
	}
	t->bmt_loaded = 1;
	return AIROHA_BMT_OK;
}

int airoha_bmt_usable_blocks(const struct airoha_bmt *t)
{
	if (!t->geometry_set || !t->bbt_loaded)
		return AIROHA_BMT_E_NOTLOADED;
	return t->system_blocks - t->bbt_size;
}

/* The BBT shift: physical is the logical-th block not in the BBT. Computed the
 * vendor's increment way (en75_bmt get_mapping_block_bbt): walk the ascending
 * BBT, stepping the block past every entry at or below it. Valid only for an
 * ascending BBT, which the vendor requires and we refuse without. */
static int shift_block(const struct airoha_bmt *t, airoha_u32 logical, airoha_u32 *out)
{
	airoha_u32 physical = logical, i;

	if (!t->bbt_ascending)
		return AIROHA_BMT_E_ORDER;
	for (i = 0; i < t->bbt_size; i++) {
		if (t->bbt_entries[i] <= physical)
			physical++;
	}
	*out = physical;
	return AIROHA_BMT_OK;
}

/* The BMT substitution: below system_blocks, a matching `from` returns its `to`;
 * reserve blocks and unmapped blocks pass through. */
static airoha_u32 substitute(const struct airoha_bmt *t, airoha_u32 physical)
{
	airoha_u32 i;

	if (physical >= t->system_blocks)
		return physical;
	for (i = 0; i < t->bmt_size; i++) {
		if (t->bmt_from[i] == physical)
			return t->bmt_to[i];
	}
	return physical;
}

int airoha_bmt_translate(const struct airoha_bmt *t, airoha_u32 logical_block)
{
	airoha_u32 shifted, final;
	int limit, ret;

	if (!t->geometry_set || !t->bbt_loaded || !t->bmt_loaded)
		return AIROHA_BMT_E_NOTLOADED;
	limit = airoha_bmt_usable_blocks(t);
	if (limit < 0)
		return limit;
	/* Inside the usable area the block shifts; at or above it the vendor
	 * returns the block unchanged (the >= guard, matching get_mapping_block). */
	if (logical_block < (airoha_u32)limit) {
		ret = shift_block(t, logical_block, &shifted);
		if (ret)
			return ret;
	} else {
		shifted = logical_block;
	}
	final = substitute(t, shifted);
	if (final >= t->total_blocks)
		return AIROHA_BMT_E_RANGE;
	return (int)final;
}

int airoha_bmt_shift(const struct airoha_bmt *t, airoha_u32 logical_block)
{
	airoha_u32 shifted;
	int limit, ret;

	if (!t->geometry_set || !t->bbt_loaded)
		return AIROHA_BMT_E_NOTLOADED;
	limit = airoha_bmt_usable_blocks(t);
	if (limit < 0)
		return limit;
	if (logical_block < (airoha_u32)limit) {
		ret = shift_block(t, logical_block, &shifted);
		if (ret)
			return ret;
	} else {
		shifted = logical_block;
	}
	return (int)(shifted - logical_block);
}

int airoha_bmt_untranslate(const struct airoha_bmt *t, airoha_u32 final_block)
{
	int limit, mapped;
	airoha_u32 logical;

	if (!t->geometry_set || !t->bbt_loaded || !t->bmt_loaded)
		return AIROHA_BMT_E_NOTLOADED;
	limit = airoha_bmt_usable_blocks(t);
	if (limit < 0)
		return limit;
	for (logical = 0; logical < (airoha_u32)limit; logical++) {
		mapped = airoha_bmt_translate(t, logical);
		if (mapped < 0)
			return mapped;
		if ((airoha_u32)mapped == final_block)
			return (int)logical;
	}
	return AIROHA_BMT_E_RANGE;
}
