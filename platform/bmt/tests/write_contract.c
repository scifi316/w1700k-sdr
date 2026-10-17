/* SPDX-License-Identifier: GPL-2.0-only */
/* Independent byte expectations and recovery negatives, not a NAND simulator. */
#include <assert.h>
#include <errno.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include "airoha_bmt_write.h"
#include "geometry.h"

static void hash(const unsigned char *p, unsigned int len, unsigned char out[32])
{ assert(SHA256(p, len, out)); }
static void used(struct airoha_bmt_intent *r, unsigned block)
{ unsigned i = block - T_SYSTEM; r->free_pool[i / 8] &= ~(1u << (i % 8)); }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, unsigned v) { put16(p, v); put16(p + 2, v >> 16); }
static struct airoha_bmt table, scratch, old;
static unsigned char page[T_PS], saved[T_PS], check[T_PS];

static void geometry(struct airoha_bmt *t)
{
	assert(!airoha_bmt_set_geometry(t, T_TOTAL, T_POOL));
	assert(!airoha_bmt_set_pages(t, T_PS, T_FREE));
}

static void base(void)
{
	static const unsigned short bad[] = {5, 11, 20, 40, 100};
	geometry(&table);
	table.bbt_loaded = table.bmt_loaded = table.bbt_ascending = 1;
	table.bbt_size = 5;
	memcpy(table.bbt_entries, bad, sizeof bad);
	table.bmt_size = 1;
	table.bmt_from[0] = 52;
	table.bmt_to[0] = T_SYSTEM + 1;
}

static void bytes(void)
{
	unsigned char bmt_head[24] = {
		'B','M','T',1,255,1,0,255,255,255,255,255,255,255,255,255,
		255,255,255,255,52,0,0,0,
	};
	put16(bmt_head + 22, T_SYSTEM + 1);
	/* version + size + the one entry's bytes; the rest of the pool is zero */
	bmt_head[6] = (1 + 1 + 52 + bmt_head[22] + bmt_head[23]) & 255;
	static const unsigned char bbt_head[22] = {
		'R','A','W','B',182,0,0,0,1,5,255,255,5,0,11,0,20,0,40,0,100,0,
	};
	base();
	assert(!airoha_bmt_encode(&table, page, sizeof page));
	assert(!memcmp(page, bmt_head, sizeof bmt_head));
	for (unsigned i = sizeof bmt_head; i < 1020; i++) assert(!page[i]);
	for (unsigned i = 1020; i < sizeof page; i++) assert(page[i] == 255);
	assert(!airoha_bbt_encode(&table, page, sizeof page));
	assert(!memcmp(page, bbt_head, sizeof bbt_head));
	for (unsigned i = sizeof bbt_head; i < 512; i++) assert(!page[i]);
	for (unsigned i = 512; i < sizeof page; i++) assert(page[i] == 255);
	geometry(&scratch);
	assert(!airoha_bmt_parse_bbt(&scratch, page, sizeof page));
	assert(!airoha_bmt_encode(&table, page, sizeof page));
	assert(!airoha_bmt_parse_bmt(&scratch, page, sizeof page));
	assert(airoha_bmt_translate(&scratch, 48) == (int)T_SYSTEM + 1);
	memcpy(saved, page, sizeof page);
	assert(airoha_bmt_encode(&table, page, 1020) == -EINVAL);
	assert(!memcmp(saved, page, sizeof page));
	/* The checksum covers the pool's entries and no more: a byte past them
	 * changes nothing the vendor sums (LV), the last entry's byte does. */
	assert(!airoha_bmt_encode(&table, page, sizeof page));
	page[AIROHA_BMT_HEADER + 4 * T_POOL - 1] ^= 1;
	assert(airoha_bmt_parse_bmt(&scratch, page, sizeof page) == AIROHA_BMT_E_CHECKSUM);
	page[AIROHA_BMT_HEADER + 4 * T_POOL - 1] ^= 1;
	if (T_POOL < AIROHA_BMT_MAX_ENTRIES) {
		page[AIROHA_BMT_HEADER + 4 * T_POOL] ^= 1;
		assert(!airoha_bmt_parse_bmt(&scratch, page, sizeof page));
	}
	puts("canonical vendor bytes and independent checksum/layout expectations: passed");
}

static void replacement(void)
{
	base();
	assert(!airoha_bmt_write_validate(&table, 64, T_END, T_SYSTEM, T_LAST));
	assert(!airoha_bmt_replace(&table, 48, T_SYSTEM + 2));
	assert(table.bmt_size == 2 && table.bmt_from[0] == 52 && table.bmt_to[0] == T_SYSTEM + 2);
	assert(table.bmt_from[1] == T_SYSTEM + 1 && table.bmt_to[1] == T_SYSTEM + 1);
	assert(!airoha_bmt_replace(&table, 48, T_SYSTEM + 3));
	assert(table.bmt_size == 3 && table.bmt_from[0] == 52 && table.bmt_to[0] == T_SYSTEM + 3);
	assert(table.bmt_from[2] == T_SYSTEM + 2 && table.bmt_to[2] == T_SYSTEM + 2);
	assert(!airoha_bmt_replace(&table, 49, T_SYSTEM + 4));
	assert(table.bmt_size == 4 && table.bmt_from[3] == 53);
	old = table;
	assert(airoha_bmt_replace(&table, 49, T_SYSTEM + 3) == -EINVAL);
	assert(!memcmp(&old, &table, sizeof table));
	assert(airoha_bmt_write_validate(&table, 64, T_SYSTEM, T_SYSTEM, T_LAST) == -EINVAL);
	assert(airoha_bmt_write_validate(&table, 64, T_END, T_SYSTEM, T_SYSTEM + 3) == -EINVAL);
	table.bmt_from[1] = 52;
	assert(airoha_bmt_write_validate(&table, 64, T_END, T_SYSTEM, T_LAST) == -EINVAL);
	table = old; table.bmt_to[1] = T_SYSTEM + 3;
	assert(airoha_bmt_write_validate(&table, 64, T_END, T_SYSTEM, T_LAST) == -EINVAL);
	table = old; table.bmt_from[1] = 5;
	assert(airoha_bmt_write_validate(&table, 64, T_END, T_SYSTEM, T_LAST) == -EINVAL);
	table = old; table.bmt_from[1] = T_SYSTEM;
	assert(airoha_bmt_write_validate(&table, 64, T_END, T_SYSTEM, T_LAST) == -EINVAL);
	base(); table.bmt_size = T_POOL;
	for (unsigned i = 0; i < T_POOL; i++) {
		table.bmt_from[i] = 1000 + i;
		table.bmt_to[i] = T_SYSTEM + i;
	}
	/* Every pool destination is occupied; reusing one is not allocation, and
	 * a table holding more entries than the pool has blocks is not a map. */
	old = table;
	assert(airoha_bmt_replace(&table, 48, T_SYSTEM) == -EINVAL);
	assert(!memcmp(&old, &table, sizeof table));
	if (T_POOL < AIROHA_BMT_MAX_ENTRIES) {
		table.bmt_size = T_POOL + 1;
		table.bmt_from[T_POOL] = 999; table.bmt_to[T_POOL] = T_LAST;
		assert(airoha_bmt_replace(&table, 48, T_LAST) == -EINVAL);
		assert(airoha_bmt_encode(&table, page, sizeof page) == -EINVAL);
	}
	puts("original-key replacement, repeated wear, pool alias and ownership negatives: passed");
}

static void intent_base(struct airoha_bmt_intent *r, int mapped)
{
	base();
	if (!mapped) table.bmt_size = 0;
	memset(r, 0, sizeof *r);
	r->operation = AIROHA_INTENT_PROGRAM;
	r->journal_block = T_SYSTEM + 2;
	r->candidate_block = T_SYSTEM + 3;
	r->logical_block = 48;
	r->system_key = 52;
	r->old_physical = mapped ? T_SYSTEM + 1 : 52;
	r->old_table_block = T_LAST;
	r->bbt_block = T_SYSTEM;
	r->bbt_size = table.bbt_size;
	r->bmt_size = table.bmt_size;
	r->old_mapping_present = mapped;
	memset(r->free_pool, 0, sizeof r->free_pool);
	for (unsigned i = 0; i < T_POOL; i++) r->free_pool[i / 8] |= 1u << (i % 8);
	used(r, r->bbt_block); used(r, r->old_table_block);
	if (mapped) used(r, r->old_physical);
	assert(!airoha_bmt_encode(&table, page, sizeof page));
	hash(page, AIROHA_BMT_BYTES, r->bmt_digest);
	assert(!airoha_bbt_encode(&table, page, sizeof page));
	hash(page, AIROHA_BBT_BYTES, r->bbt_digest);
	hash((const unsigned char *)"intended block", 14, r->data_digest);
}

static void intents(void)
{
	unsigned corruptions = 0;
	for (int mapped = 0; mapped <= 1; mapped++) {
		struct airoha_bmt_intent r, decoded, sentinel;
		unsigned char head[20], geometry_bytes[16];
		intent_base(&r, mapped);
		assert(!airoha_bmt_intent_encode(&r, &table, page, sizeof page, hash));
		/* Independent fixed header byte positions, including little endian. */
		memcpy(head, "SDRWB003\3\0\1\0", 12);
		put16(head + 12, T_SYSTEM + 2); put16(head + 14, T_SYSTEM + 3);
		put16(head + 16, 48); put16(head + 18, 52);
		assert(!memcmp(page, head, sizeof head));
		assert(page[22] == (T_LAST & 255) && page[23] == T_LAST >> 8 &&
		       page[24] == (T_SYSTEM & 255) && page[25] == T_SYSTEM >> 8);
		put32(geometry_bytes, T_BS); put32(geometry_bytes + 4, T_PS);
		put32(geometry_bytes + 8, T_FREE); put32(geometry_bytes + 12, T_TOTAL);
		assert(!memcmp(page + 128, geometry_bytes, 16));
		assert(!airoha_bmt_intent_decode(&decoded, &table, page, sizeof page, hash));
		assert(!memcmp(&r, &decoded, sizeof r));
		assert(!airoha_bmt_intent_match(&r, &table, &scratch, check, hash));
		old = table;
		assert(!airoha_bmt_replace(&table, 48, r.candidate_block));
		assert(!airoha_bmt_intent_match(&r, &table, &scratch, check, hash));
		table.bbt_entries[0] = 6;
		assert(airoha_bmt_intent_match(&r, &table, &scratch, check, hash) == -EINVAL);
		table = old;
		assert(!airoha_bmt_replace(&table, 49, T_SYSTEM + 4));
		assert(airoha_bmt_intent_match(&r, &table, &scratch, check, hash) == -EINVAL);
		memcpy(saved, page, sizeof page);
		memset(&sentinel, 0xa5, sizeof sentinel);
		for (unsigned i = 0; i < sizeof page; i++) {
			decoded = sentinel;
			page[i] ^= 1;
			assert(airoha_bmt_intent_decode(&decoded, &table, page, sizeof page, hash) < 0);
			assert(!memcmp(&decoded, &sentinel, sizeof decoded));
			page[i] ^= 1;
			corruptions++;
		}
		assert(!memcmp(saved, page, sizeof page));
		/* Re-hashed malicious records still fail semantic validation. */
		page[14] = page[12]; page[15] = page[13]; hash(page, 208, page + 208);
		assert(airoha_bmt_intent_decode(&decoded, &table, page, sizeof page, hash) == -EINVAL);
		memcpy(page, saved, sizeof page);
		page[144] |= 1; /* A forged free bit for the factory BBT is never authority. */
		hash(page, 208, page + 208);
		assert(airoha_bmt_intent_decode(&decoded, &table, page, sizeof page, hash) == -EINVAL);
		for(unsigned version=1;version<=2;version++) {
			memcpy(page, saved, sizeof page);
			page[7] = '0'+version; page[8] = version;
			hash(page, 208, page + 208);
			assert(airoha_bmt_intent_decode(&decoded, &table, page, sizeof page, hash) == -EINVAL);
		}
		r.system_key++;
		assert(airoha_bmt_intent_match(&r, &old, &scratch, check, hash) == -EINVAL);
	}
	printf("journal header/hash/padding corruptions rejected without publishing output: %u\n", corruptions);
	puts("old/new mapping recovery with empty/nonempty tables and changed-factory negatives: passed");
}

int main(void)
{
	bytes(); replacement(); intents();
	return 0;
}
