/* SPDX-License-Identifier: GPL-2.0-only */
/* Sparse synthetic NAND. Mutation cuts preserve bytes, program budgets and
 * page ordering. This tests the real C publisher, not a re-expression of it. */
#include <assert.h>
#include <errno.h>
#include <openssl/sha.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "airoha_bmt_write.h"
#include "geometry.h"

struct block {
	unsigned char main[T_PAGES][T_PS], spare[T_PAGES][T_OS], programs[T_PAGES];
	int last;
};
static struct block *nand[T_TOTAL];
static struct airoha_bmt t, scratch, original;
static struct airoha_bmt_intent intent;
static unsigned char expected[T_PS], actual[T_PS], image[T_PAGES*T_STRIDE];
static unsigned actions, cut_action, cut_phase, failures, old_mark_calls;
static int fail_program, fail_erase, fail_old_mark, fail_marker_read, fail_image;
static int fail_after_mark;
static jmp_buf reset_point;

static void hash(const unsigned char *p, unsigned len, unsigned char out[32])
{ assert(SHA256(p, len, out)); }

static struct block *storage(unsigned b)
{
#ifdef TRANSACTION_MODEL
	assert(b < T_TOTAL);
#else
	assert(b >= T_SYSTEM && b < T_TOTAL);
#endif
	if (!nand[b]) {
		nand[b] = malloc(sizeof *nand[b]); assert(nand[b]);
		memset(nand[b], 255, sizeof *nand[b]);
		memset(nand[b]->programs, 0, sizeof nand[b]->programs);
		nand[b]->last = -1;
	}
	return nand[b];
}

static void before(void)
{
	actions++;
	if (actions == cut_action && cut_phase == 0) longjmp(reset_point, 1);
}
static int torn(void) { return actions == cut_action && cut_phase == 2; }
static void after(void)
{
	if (actions == cut_action) longjmp(reset_point, 1);
}

static int read_page(void *ctx, unsigned b, unsigned p, unsigned char *out)
{
	(void)ctx; assert(p < T_PAGES);
	if(nand[b]) memcpy(out, nand[b]->main[p], T_PS); else memset(out,255,T_PS);
	return 0;
}
static int program_page(void *ctx, unsigned b, unsigned p, const unsigned char *data)
{
	struct block *s = storage(b);
	unsigned len = T_PS;
	int error;
	(void)ctx; assert(p < T_PAGES);
	before();
	assert((int)p >= s->last && s->programs[p] < 4);
	s->programs[p]++; s->last = p;
	error = fail_program && b == (T_LAST-1);
	if (error) { fail_program = 0; failures++; }
	if (torn() || error) len = (T_PS/2);
	for (unsigned i = 0; i < len; i++) {
		assert((s->main[p][i] & data[i]) == data[i]);
		s->main[p][i] &= data[i];
	}
	/* Non-FF parity models a programmed all-FF main page as used. */
	if (len == T_PS) s->spare[p][T_OS-1] = 0;
	after();
	return error ? -EIO : 0;
}
static int erase(void *ctx, unsigned b)
{
	struct block *s = storage(b);
	(void)ctx;
#ifndef TRANSACTION_MODEL
	assert(b != T_SYSTEM && b != T_LAST && b != intent.candidate_block &&
	       b != intent.journal_block); /* publisher's exact mutation boundary */
#else
	assert(b!=T_SYSTEM && b!=T_LAST && (b==65 || b>=T_SYSTEM));
#endif
	before();
	if (fail_erase && b == (T_LAST-1)) return -EIO;
	unsigned end = torn() ? T_PAGES/2 : T_PAGES;
	for (unsigned p = 0; p < end; p++) {
		memset(s->main[p], 255, T_PS); memset(s->spare[p], 255, T_OS);
		s->programs[p] = 0;
	}
	s->last = -1;
	for (unsigned p = 0; p < T_PAGES; p++) if (s->programs[p]) s->last = p;
	after();
	return 0;
}
static int erased(void *ctx, unsigned b, unsigned p)
{
	struct block *s = nand[b];
	(void)ctx;
	if(!s) return 1;
	for (unsigned i = 0; i < T_PS; i++) if (s->main[p][i] != 255) return 0;
	for (unsigned i = 0; i < T_OS; i++) if (s->spare[p][i] != 255) return 0;
	return 1;
}
static int markers(void *ctx, unsigned b, unsigned char out[2])
{
	(void)ctx;
	if (b == T_LAST && (fail_marker_read || (fail_after_mark && old_mark_calls))) return -EIO;
	if(nand[b]) memcpy(out, nand[b]->spare[0], 2); else memset(out,255,2);
	return 0;
}
static int retire(void *ctx, unsigned b)
{
	struct block *s = storage(b);
	(void)ctx;
#ifndef TRANSACTION_MODEL
	assert(b != T_SYSTEM && b != intent.journal_block && b != intent.candidate_block);
#else
	assert(b!=T_SYSTEM && b!=(T_SYSTEM+2) && (b==65 || b>=T_SYSTEM));
#endif
	before();
	assert(s->last <= 0 && s->programs[0] < 4);
	s->last = 0; s->programs[0]++;
	if (b == T_LAST) old_mark_calls++;
	if (b == T_LAST && fail_old_mark) return -EIO;
	s->spare[0][1] &= torn() ? 0xfe : 0;
	after(); return 0;
}
static void image_hash(unsigned b, unsigned char out[32])
{
	struct block *s = storage(b);
	for (unsigned p = 0; p < T_PAGES; p++) {
		unsigned char *buf = image + p*T_STRIDE;
		memcpy(buf, s->main[p], T_PS);
		for (unsigned sec = 0; sec < T_SECTIONS; sec++)
			memcpy(buf+T_PS+sec*14, s->spare[p]+sec*16+2, 14);
		buf[T_PS] = buf[T_PS+1] = 255; /* only the vendor-owned backreference */
	}
	hash(image, sizeof image, out);
}
static int verify_image(void *ctx, const struct airoha_bmt_intent *r)
{
	unsigned char digest[32]; (void)ctx;
	if (fail_image) return -EBADMSG;
	image_hash(r->candidate_block, digest);
	return memcmp(digest, r->data_digest, 32) ? -EBADMSG : 0;
}
static const struct airoha_bmt_flash io = {
	.read_page=read_page, .program_page=program_page, .erase=erase, .erased=erased,
	.markers=markers, .retire=retire, .verify_image=verify_image, .hash=hash,
};

/* Independent stock-order reader, with literal header fields and checksum. */
static unsigned stock_table(void)
{
	for (unsigned b = T_TOTAL; b-- > T_SYSTEM;) {
		struct block *s = nand[b]; unsigned sum, count;
		if(!s) continue;
		unsigned char *p = s->main[0];
		if (s->spare[0][0] != 255 || s->spare[0][1] != 255 || memcmp(p,"BMT",3)) continue;
		count = p[5]; if (count > 250) continue;
		sum = p[3]+count; for (unsigned j = 20; j < 20+4*T_POOL; j++) sum += p[j];
		if ((sum & 255) != p[6]) continue;
		int valid = 1;
		for (unsigned j = 0; j < count; j++) {
			unsigned from = p[20+4*j] | (p[21+4*j]<<8);
			unsigned to = p[22+4*j] | (p[23+4*j]<<8);
			if (from >= T_TOTAL || to < T_SYSTEM || to >= T_TOTAL) valid = 0;
		}
		if (valid) return b;
	}
	return 0;
}
static void unfree(unsigned b)
{ unsigned i=b-T_SYSTEM; intent.free_pool[i/8] &= ~(1u << (i%8)); }

static void setup(int mapped, int injected)
{
	for (unsigned b = T_SYSTEM; b < T_TOTAL; b++) { free(nand[b]); nand[b] = NULL; }
	actions=cut_action=cut_phase=failures=old_mark_calls=0;
	fail_program=fail_erase=fail_old_mark=fail_marker_read=fail_image=fail_after_mark=0;
	assert(!airoha_bmt_set_geometry(&t,T_TOTAL,T_POOL) && !airoha_bmt_set_pages(&t,T_PS,T_FREE));
	t.bbt_loaded=t.bmt_loaded=t.bbt_ascending=1; t.bbt_size=1; t.bbt_entries[0]=5;
	if (mapped) { t.bmt_size=1; t.bmt_from[0]=65; t.bmt_to[0]=(T_SYSTEM+1); }
	original=t;
	memset(&intent,0,sizeof intent);
	intent.operation=AIROHA_INTENT_PROGRAM; intent.logical_block=64; intent.system_key=65;
	intent.candidate_block=(T_SYSTEM+3); intent.journal_block=(T_SYSTEM+2); intent.old_table_block=T_LAST;
	intent.bbt_block=T_SYSTEM; intent.bbt_size=1; intent.bmt_size=mapped;
	intent.old_mapping_present=mapped; intent.old_physical=mapped ? (T_SYSTEM+1) : 65;
	memset(intent.free_pool,0,32);
	for (unsigned i=0;i<T_POOL;i++) intent.free_pool[i/8]|=1u<<(i%8);
	unfree(T_SYSTEM); unfree(T_LAST); if(mapped) unfree(T_SYSTEM+1);
	assert(!airoha_bbt_encode(&t,expected,T_PS));
	hash(expected,512,intent.bbt_digest); program_page(NULL,T_SYSTEM,0,expected);
	assert(!airoha_bmt_encode(&t,expected,T_PS));
	hash(expected,1020,intent.bmt_digest); program_page(NULL,T_LAST,0,expected);
	for (unsigned p=0;p<T_PAGES;p++) {
		for (unsigned i=0;i<T_PS;i++) expected[i]=(i*7+p*13)%251;
		program_page(NULL,(T_SYSTEM+3),p,expected);
		storage(T_SYSTEM+3)->spare[p][2]=65; storage(T_SYSTEM+3)->spare[p][3]=0;
	}
	image_hash((T_SYSTEM+3),intent.data_digest);
	assert(!airoha_bmt_intent_encode(&intent,&t,expected,T_PS,hash));
	program_page(NULL,(T_SYSTEM+2),0,expected);
	actions=0; fail_program=injected;
}
static int publish(unsigned *selected)
{
	return airoha_bmt_publish(&t,&scratch,&intent,&io,64,T_END,expected,actual,selected);
}

static void perform_cut(void)
{
	unsigned selected = 0;
	if (!setjmp(reset_point)) { publish(&selected); assert(!"cut not reached"); }
}

static void reset_matrix(void)
{
	unsigned cuts=0;
	for (int mapped=0;mapped<2;mapped++) for(int injected=0;injected<2;injected++) {
		unsigned selected=0, total;
		setup(mapped,injected); assert(!publish(&selected)); total=actions;
		assert(selected==(injected ? (T_LAST-2) : (T_LAST-1)) && stock_table()==selected);
		for (unsigned cut=1;cut<=total;cut++) for(unsigned phase=0;phase<3;phase++) {
			setup(mapped,injected); cut_action=cut; cut_phase=phase;
			perform_cut();
			unsigned before_recovery=stock_table();
			assert(before_recovery==T_LAST || before_recovery==(T_LAST-1) || before_recovery==(T_LAST-2));
			cut_action=0; t=original;
			/* Reload the durable record, not a surviving RAM-only intent. */
			assert(!airoha_bmt_intent_decode(&intent,&t,storage(T_SYSTEM+2)->main[0],T_PS,hash));
			assert(!publish(&selected)); assert(stock_table()==selected);
			assert(airoha_bmt_translate(&t,64)==(T_SYSTEM+3) && t.bmt_size==(mapped ? 2 : 1) && t.bmt_from[0]==65);
			if(mapped) assert(t.bmt_from[1]==(T_SYSTEM+1) && t.bmt_to[1]==(T_SYSTEM+1));
			assert(!verify_image(NULL,&intent));
			assert(storage(T_LAST)->programs[0] <= 4);
			cuts++;
		}
	}
	printf("publication before/after/torn NAND cuts, old/new stock selection and recovery: %u\n",cuts);
}

static void faults(void)
{
	unsigned selected=0;
	setup(0,0); fail_marker_read=1; assert(publish(&selected)==-EIO && !actions);
	setup(0,0); fail_image=1; assert(publish(&selected)==-EBADMSG && !actions);
	setup(0,0); storage(T_SYSTEM+2)->main[0][200]^=1;
	assert(publish(&selected)==-EBADMSG && !actions);
	setup(0,0); fail_erase=1;
	assert(publish(&selected)==-EIO && actions==2 && stock_table()==T_LAST);
	fail_erase=0; assert(!publish(&selected));
	setup(0,0); fail_after_mark=1; assert(publish(&selected)==-EIO);
	assert(stock_table()==(T_LAST-1)); fail_after_mark=0; t=original;
	assert(!publish(&selected) && selected==(T_LAST-1) && old_mark_calls==1);
	setup(0,0); fail_old_mark=1;
	for(unsigned i=0;i<3;i++) assert(publish(&selected)==-EIO);
	assert(old_mark_calls==3 && storage(T_LAST)->programs[0]==4 && stock_table()==T_LAST);
	unsigned prior=actions;
	assert(publish(&selected)==-EIO && actions==prior && old_mark_calls==3);
	setup(0,0);
	memset(intent.free_pool,0,32); intent.free_pool[0]=(1u<<2)|(1u<<3);
	assert(!airoha_bmt_intent_encode(&intent,&t,expected,T_PS,hash));
	memcpy(storage(T_SYSTEM+2)->main[0],expected,T_PS); /* alternate initial fixture */
	assert(publish(&selected)==-ENOSPC && !actions && stock_table()==T_LAST);
	setup(0,0); unfree(T_LAST-1);
	assert(!airoha_bmt_intent_encode(&intent,&t,expected,T_PS,hash));
	memcpy(storage(T_SYSTEM+2)->main[0],expected,T_PS);
	assert(!airoha_bmt_encode(&t,expected,T_PS));
	program_page(NULL,(T_LAST-1),0,expected); actions=0;
	assert(publish(&selected)==-EUCLEAN && !actions && stock_table()==T_LAST);
	setup(1,0);
	t.bmt_to[0]=(T_LAST-1); intent.old_physical=(T_LAST-1); unfree(T_LAST-1);
	assert(!airoha_bmt_encode(&t,expected,T_PS));
	hash(expected,1020,intent.bmt_digest);
	memcpy(storage(T_LAST)->main[0],expected,T_PS); /* alternate initial fixture */
	assert(!airoha_bmt_intent_encode(&intent,&t,expected,T_PS,hash));
	memcpy(storage(T_SYSTEM+2)->main[0],expected,T_PS);
	assert(publish(&selected)==-ENOSPC && !actions && stock_table()==T_LAST);
	setup(0,0); prior=actions;
	assert(airoha_bmt_publish(&t,&scratch,&intent,&io,65,T_END,expected,actual,&selected)==-EINVAL);
	assert(actions==prior);
	puts("publication faults, unknown selector, pool exhaustion and reset-persistent retry budget: passed");
}

#ifdef TRANSACTION_MODEL
int publication_baseline_main(void)
#else
int main(void)
#endif
{
	reset_matrix(); faults();
	for(unsigned b=T_SYSTEM;b<T_TOTAL;b++) free(nand[b]);
	return 0;
}
