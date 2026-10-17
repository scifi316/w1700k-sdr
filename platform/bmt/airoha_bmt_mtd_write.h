/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared MTD adapter, included by both persistent bindings. It drives the
 * W25N04KV (2048 + 128 byte pages, 128 KiB blocks) and the W25N04LV (4096 +
 * 256, 256 KiB): the page geometry comes from the chip, the writable range from
 * the byte policy, the pool from the vendor scan. layer holds saved physical
 * callbacks. All entrypoints hold BMT_ENTER/LEAVE; internal callbacks never
 * recurse through public MTD operations. */
#include "airoha_bmt_transaction.h"
#include "airoha_bmt_policy.h"

static int bmt_transport_failed;
#define BMT_FAST_FALLBACK 1
/* Sized for the smallest block the policy allows; a larger block uses less. */
#define BMT_FAST_BLOCKS ((u32)((BMT_WRITE_END_BYTES-BMT_WRITE_FIRST_BYTES)/BMT_MIN_BLOCK_BYTES))
static u64 fast_known[(BMT_FAST_BLOCKS+63)/64];
static u64 fast_pre_erased[(BMT_FAST_BLOCKS+63)/64];
static u64 fast_programmed_pages[BMT_FAST_BLOCKS];
static signed char fast_highest_page[BMT_FAST_BLOCKS];
/* The writable logical blocks, [bmt_first, bmt_end), and the page geometry. */
static u32 bmt_first, bmt_end, bmt_page, bmt_oob, bmt_spare, bmt_block;

static int fast_index(u32 logical)
{
	return logical<bmt_first || logical>=bmt_end ? -1 : (int)(logical-bmt_first);
}
static int fast_bit(const u64 *bits,u32 logical)
{
	int bit=fast_index(logical);
	return bit<0 ? 0 : (bits[bit/64]>>(bit%64))&1;
}
static void fast_set_bit(u64 *bits,u32 logical,int value)
{
	int bit=fast_index(logical);
	if(bit<0) return;
	if(value) bits[bit/64]|=1ULL<<(bit%64);
	else bits[bit/64]&=~(1ULL<<(bit%64));
}
static void fast_prepare(u32 logical)
{
	int index=fast_index(logical); if(index<0) return;
	fast_set_bit(fast_known,logical,1);fast_set_bit(fast_pre_erased,logical,1);
	fast_programmed_pages[index]=0;fast_highest_page[index]=-1;
}
static void fast_drop(u32 logical)
{ fast_set_bit(fast_known,logical,0);fast_set_bit(fast_pre_erased,logical,0); }
static void fast_reset(void)
{
	memset(fast_known,0,sizeof fast_known);memset(fast_pre_erased,0,sizeof fast_pre_erased);
	memset(fast_programmed_pages,0,sizeof fast_programmed_pages);
	memset(fast_highest_page,0xff,sizeof fast_highest_page);
}

static int managed_transport(int rc)
{
	/* A manual SPI timeout can leave CS/command completion unknown. No next
	 * command is allowed until reset. DMA/array-ready failures panic earlier. */
	if(rc == -ETIMEDOUT) bmt_transport_failed=1;
	return rc;
}
static int physical_read(struct mtd_info *mtd, loff_t from, struct mtd_oob_ops *ops)
{
	int rc;
	if(bmt_transport_failed) return -EIO;
	rc=managed_transport(layer._read_oob(mtd,from,ops));
	if(ops->retlen!=ops->len || ops->oobretlen!=ops->ooblen)
		return rc<0 ? rc : -EIO;
	return rc;
}
static int physical_write(struct mtd_info *mtd, loff_t to, struct mtd_oob_ops *ops)
{
	if(bmt_transport_failed) return -EIO;
	return managed_transport(layer._write_oob(mtd,to,ops));
}
static loff_t physical_address(u32 block, u32 page)
{ return ((loff_t)block << layer.blk_shift) + (loff_t)page * bmt_page; }

static int flash_read(void *ctx,u32 b,u32 p,u8 *data)
{
	struct mtd_oob_ops op={.mode=MTD_OPS_AUTO_OOB,.len=bmt_page,.datbuf=data};
	int rc;
	layer.counters.readback_attempts++;
	rc=physical_read(ctx,physical_address(b,p),&op);
	if(!rc || rc==-EUCLEAN) layer.counters.readback_completed++;
	return rc;
}
static int flash_program(void *ctx,u32 b,u32 p,const u8 *data)
{
	struct mtd_oob_ops op={.mode=MTD_OPS_AUTO_OOB,.len=bmt_page,.datbuf=(u8 *)data};
	int rc;
	layer.counters.program_attempts++;
	rc=physical_write(ctx,physical_address(b,p),&op);
	if(!rc && op.retlen==bmt_page) layer.counters.program_completed++;
	return rc ? rc : (op.retlen==bmt_page ? 0 : -EIO);
}
static int flash_erase(void *ctx,u32 b)
{
	struct erase_info erase={.addr=physical_address(b,0),.len=bmt_block};
	int rc;
	if(bmt_transport_failed) return -EIO;
	layer.counters.erase_attempts++;
	rc=managed_transport(layer._erase(ctx,&erase));
	if(!rc) layer.counters.erase_completed++;
	return rc;
}
static int flash_markers(void *ctx,u32 b,u8 out[2])
{
	struct mtd_oob_ops op={.mode=MTD_OPS_RAW,.ooblen=2,.oobbuf=out};
	int rc;
	layer.counters.readback_attempts++;
	rc=physical_read(ctx,physical_address(b,0),&op);
	if(!rc) layer.counters.readback_completed++;
	return rc;
}
static int flash_erased(void *ctx,u32 b,u32 p)
{
	/* RAW main data on the SNFI path shows controller bytes, not 0xff, for an
	 * erased page (0x55 has been seen).  Programming uses the
	 * ECC/AUTO view, so its erased admission must use that same view plus every
	 * client-writable OOB byte; factory/worn markers are checked separately. */
	struct mtd_oob_ops op={.mode=MTD_OPS_AUTO_OOB,.len=bmt_page,.ooblen=bmt_oob,
		.datbuf=layer.io_page,.oobbuf=layer.io_page+bmt_page};
	unsigned i;
	int rc;
	layer.counters.readback_attempts++;
	rc=physical_read(ctx,physical_address(b,p),&op);
	if(!rc || rc==-EUCLEAN) layer.counters.readback_completed++;
	if(rc<0) return rc;
	for(i=0;i<bmt_page+bmt_oob;i++) if(layer.io_page[i]!=255) return 0;
	return 1;
}
static int flash_retire(void *ctx,u32 b)
{
	u8 marker=0;
	struct mtd_oob_ops op={.mode=MTD_OPS_RAW,.ooboffs=1,.ooblen=1,.oobbuf=&marker};
	int rc;
	layer.counters.markbad_attempts++;
	layer.counters.program_attempts++;
	rc=physical_write(ctx,physical_address(b,0),&op);
	if(!rc && op.oobretlen==1) {
		layer.counters.markbad_completed++;
		layer.counters.program_completed++;
	}
	return rc ? rc : (op.oobretlen==1 ? 0 : -EIO);
}
static int flash_read_data(void *ctx,u32 b,u32 p,u8 *data,u8 *oob)
{
	struct mtd_oob_ops op={.mode=MTD_OPS_AUTO_OOB,.len=bmt_page,.ooblen=bmt_oob,
		.datbuf=data,.oobbuf=oob};
	int rc;
	layer.counters.readback_attempts++;
	rc=physical_read(ctx,physical_address(b,p),&op);
	if(!rc || rc==-EUCLEAN) layer.counters.readback_completed++;
	return rc;
}
static int flash_program_data(void *ctx,u32 b,u32 p,const u8 *data,const u8 *oob,
		u32 *written,u32 *oobwritten)
{
	struct mtd_oob_ops op={.mode=MTD_OPS_AUTO_OOB,.len=bmt_page,.ooblen=bmt_oob,
		.datbuf=(u8 *)data,.oobbuf=(u8 *)oob};
	int rc;
	*written=*oobwritten=0;
	layer.counters.program_attempts++;
	rc=physical_write(ctx,physical_address(b,p),&op);
	if(op.retlen>bmt_page || op.oobretlen>bmt_oob) return -EIO;
	*written=op.retlen; *oobwritten=op.oobretlen;
	if(!rc && op.retlen==bmt_page && op.oobretlen==bmt_oob)
		layer.counters.program_completed++;
	return rc;
}

static void managed_free(void)
{
	BMT_FREE(layer.io_page); BMT_FREE(layer.work); BMT_FREE(layer.writer);
	layer.io_page=NULL; layer.work=NULL; layer.writer=NULL;
	fast_reset();
}
static int managed_prepare(struct mtd_info *mtd)
{
	struct airoha_bmt_data_io io={.metadata={.ctx=mtd,.read_page=flash_read,
		.program_page=flash_program,.erase=flash_erase,.erased=flash_erased,
		.markers=flash_markers,.retire=flash_retire,.hash=BMT_SHA256},
		.read_data=flash_read_data,.program_data=flash_program_data};
	int rc;
	if(bmt_transport_failed) return -EIO;
	/* 512 MiB of 64-page blocks with 2048- or 4096-byte pages, OOB a 16th of
	 * the page, free OOB 14 of every 16 bytes of its first half from byte 2
	 * (Winbond's layout: W25N04KV 128/56, W25N04LV 256/112), and a block that
	 * divides the policy's bounds. */
	if(mtd->size!=0x20000000ULL ||
	   (mtd->writesize!=2048 && mtd->writesize!=4096) ||
	   mtd->erasesize!=AIROHA_BMT_BLOCK_PAGES*mtd->writesize ||
	   mtd->erasesize<BMT_MIN_BLOCK_BYTES || mtd->oobsize!=mtd->writesize/16 ||
	   mtd->oobavail!=mtd->oobsize/32*14 || !layer._write_oob || !layer._erase ||
	   BMT_WRITE_FIRST_BYTES%mtd->erasesize || BMT_WRITE_END_BYTES%mtd->erasesize ||
	   !(mtd->flags & MTD_WRITEABLE)) return -EINVAL;
	if(airoha_bmt_set_pages(&layer.t,mtd->writesize,mtd->oobavail)) return -EINVAL;
	bmt_page=mtd->writesize; bmt_oob=mtd->oobavail; bmt_spare=mtd->oobsize;
	bmt_block=mtd->erasesize;
	bmt_first=(u32)(BMT_WRITE_FIRST_BYTES>>layer.blk_shift);
	bmt_end=(u32)(BMT_WRITE_END_BYTES>>layer.blk_shift);
	layer.writer=BMT_ALLOC(sizeof(*layer.writer));
	layer.work=BMT_ALLOC(AIROHA_TRANSACTION_WORK_BYTES(&layer.t));
	/* Two decoded main+free-OOB pages for direct write/readback. */
	layer.io_page=BMT_ALLOC(2*(bmt_page+bmt_oob));
	if(!layer.writer || !layer.work || !layer.io_page) { managed_free(); return -ENOMEM; }
	/* The journal is the block after the BBT, which never moves (3847 on the
	 * W25N04KV), not an offset from a scanned pool. */
	rc=airoha_bmt_transaction_init(layer.writer,&layer.t,&io,bmt_first,bmt_end,
		layer.bbt_block,layer.bmt_block,layer.bbt_block+BMT_JOURNAL_AFTER_BBT,
		layer.work);
	if(rc) managed_free(); else fast_reset();
	return rc;
}

static int managed_ready(struct mtd_info *mtd)
{
	return layer.mtd==mtd && layer.writer && !layer.writer->failed &&
	       !bmt_transport_failed ? 0 : -EIO;
}
static int managed_read(struct mtd_info *mtd,loff_t from,struct mtd_oob_ops *ops)
{
	int rc=BMT_ENTER();
	ops->retlen=ops->oobretlen=0;
	if(rc) return rc;
	rc=managed_ready(mtd);
	if(!rc) rc=managed_transport(BMT_READ(mtd,from,ops));
	if(bmt_transport_failed && layer.writer) layer.writer->failed=1;
	BMT_LEAVE(); return rc;
}

/* Validate the ENTIRE request before any destructive step. Public master and
 * partition callers have identical vendor-logical protection. */
static int managed_bounds(loff_t to,size_t len,size_t olen,u32 offs,u32 mode)
{
	u64 first=(u64)bmt_first*bmt_block, end=(u64)bmt_end*bmt_block;
	u64 pages;
	u32 per=mode==MTD_OPS_AUTO_OOB ? bmt_oob : bmt_spare;
	if(to<0) return -EINVAL;
	if((u64)to<first || (u64)to>=end || len>end-to) return -EROFS;
	pages=(end-((u64)to & ~(u64)(bmt_page-1)))/bmt_page;
	if(olen && (offs>=per || olen>pages*per-offs)) return -EROFS;
	return 0;
}
static int managed_oob(struct mtd_oob_ops *ops,u32 *auto_off)
{
	u32 col=ops->ooboffs;
	if(ops->mode==MTD_OPS_RAW) return -EOPNOTSUPP;
	if(ops->mode!=MTD_OPS_AUTO_OOB && ops->mode!=MTD_OPS_PLACE_OOB) return -EINVAL;
	if(!ops->ooblen) { *auto_off=0; return 0; }
	if(ops->mode==MTD_OPS_AUTO_OOB) {
		if(col<2 || col>=bmt_oob || ops->ooblen>bmt_oob-col) return -EOPNOTSUPP;
		*auto_off=col; return 0;
	}
	/* PLACE may name one contiguous free region, excluding the backreference.
	 * Marker/parity/gap writes are refused, never converted into corrected data. */
	if(col>=bmt_spare/2 || col%16<2 || (col<16 && col<4) || ops->ooblen>16-col%16)
		return -EOPNOTSUPP;
	*auto_off=(col/16)*14+col%16-2;
	return 0;
}

static u32 fast_overlap(u32 a,u32 n,u32 b,u32 m)
{
	u32 first=a>b ? a : b, end=a+n<b+m ? a+n : b+m;
	return end>first ? end-first : 0;
}

/* A block first written in this boot without an erase in it (UBI keeps its
 * free PEBs erased across boots; UBIFS appends to them page by page): read
 * its pages from the last down to the highest one in use, once per boot, and
 * treat everything up to that page as programmed. Appends above it are then
 * direct, not a journalled whole-block replacement per page. */
static int fast_learn(struct mtd_info *mtd,u32 logical,int index,int mapped)
{
	int rc;
	u32 p;
	fast_programmed_pages[index]=0; fast_highest_page[index]=-1;
	for(p=AIROHA_BMT_BLOCK_PAGES;p-- > 0;) {
		rc=flash_erased(mtd,mapped,p);
		if(rc<0) return BMT_FAST_FALLBACK;	/* the engine reports it */
		if(rc) continue;
		fast_highest_page[index]=p;
		fast_programmed_pages[index]=p==AIROHA_BMT_BLOCK_PAGES-1 ? ~0ULL : (1ULL<<(p+1))-1;
		break;
	}
	fast_set_bit(fast_known,logical,1);
	return 0;
}

/* Direct, ascending programs into erased pages above every page in use: the
 * block's state is known from an erase in this boot or from fast_learn.
 * Verify each written page exactly once. A failed program, and any other
 * uncertainty, falls back to the durable transaction/remap engine. */
static int fast_program(struct mtd_info *mtd,const struct airoha_bmt_change *c,
		struct airoha_bmt_progress *done)
{
	u32 stride=bmt_page+bmt_oob;
	u8 *expected=layer.io_page,*actual=layer.io_page+stride;
	u32 p,written,oobwritten;
	int index,mapped,shift,rc;
	index=fast_index(c->logical);
	if(index<0) return BMT_FAST_FALLBACK;
	mapped=map_block(c->logical); shift=airoha_bmt_shift(&layer.t,c->logical);
	if(mapped<0 || shift<0) return -EIO;
	if(!fast_bit(fast_known,c->logical)) {
		rc=fast_learn(mtd,c->logical,index,mapped);
		if(rc) return rc;
	}
	for(p=0;p<AIROHA_BMT_BLOCK_PAGES;p++) {
		u32 data=fast_overlap(c->offset,c->length,p*bmt_page,bmt_page);
		u32 oob=fast_overlap(c->oob_offset,c->oob_length,p*bmt_oob,bmt_oob);
		u32 start;
		if(!data && !oob) continue;
		if((fast_programmed_pages[index]&(1ULL<<p)) ||
		   (int)p<fast_highest_page[index])
			return BMT_FAST_FALLBACK;
		memset(expected,255,stride);
		if(data) {
			start=c->offset>p*bmt_page ? c->offset : p*bmt_page;
			memcpy(expected+start-p*bmt_page,c->data+start-c->offset,data);
		}
		if(oob) {
			start=c->oob_offset>p*bmt_oob ? c->oob_offset : p*bmt_oob;
			memcpy(expected+bmt_page+start-p*bmt_oob,c->oob+start-c->oob_offset,oob);
		}
		if(mapped>=(int)layer.t.system_blocks) {
			expected[bmt_page]=c->logical+shift;
			expected[bmt_page+1]=(c->logical+shift)>>8;
		}
		rc=flash_program_data(mtd,mapped,p,expected,expected+bmt_page,
			&written,&oobwritten);
		if(rc || written!=bmt_page || oobwritten!=bmt_oob) return rc<0 ? rc : -EIO;
		rc=flash_read_data(mtd,mapped,p,actual,actual+bmt_page);
		if((rc<0 && rc!=-EUCLEAN) || memcmp(expected,actual,stride)) return -EIO;
		fast_programmed_pages[index]|=1ULL<<p; fast_highest_page[index]=p;
	}
	fast_set_bit(fast_pre_erased,c->logical,0);
	done->data=c->length; done->oob=c->oob_length;
	return 0;
}

static int managed_write(struct mtd_info *mtd,loff_t to,struct mtd_oob_ops *ops)
{
	u32 auto_off;
	int rc=BMT_ENTER();
	ops->retlen=ops->oobretlen=0;
	if(rc) return rc;
	rc=managed_ready(mtd); if(rc) goto out;
	rc=managed_bounds(to,ops->len,ops->ooblen,ops->ooboffs,ops->mode); if(rc) goto out;
	if((ops->len && !ops->datbuf) || (ops->ooblen && !ops->oobbuf)) { rc=-EINVAL; goto out; }
	rc=managed_oob(ops,&auto_off); if(rc) goto out;
	while(ops->retlen<ops->len || ops->oobretlen<ops->ooblen) {
		u32 offset=(u64)to%bmt_block, span=bmt_block-offset;
		u32 oob_offset=(offset/bmt_page)*bmt_oob+auto_off;
		struct airoha_bmt_progress done;
		struct airoha_bmt_change change={.logical=(u64)to/bmt_block,.offset=offset,
			.length=min_t(size_t,span,ops->len-ops->retlen),
			.data=ops->datbuf ? ops->datbuf+ops->retlen : NULL,
			.oob_offset=oob_offset,
			.oob_length=min_t(size_t,ops->ooblen-ops->oobretlen,
				AIROHA_BMT_BLOCK_PAGES*bmt_oob-oob_offset),
			.oob=ops->oobbuf ? ops->oobbuf+ops->oobretlen : NULL};
		rc=fast_program(mtd,&change,&done);
		if(rc) {
			fast_drop(change.logical);
			rc=airoha_bmt_transaction_apply(layer.writer,&change,&done);
		}
		ops->retlen+=done.data; ops->oobretlen+=done.oob;
		if(rc) break;
		to+=span; auto_off=0;
	}
out:
	BMT_LEAVE(); return rc;
}
static int managed_erase(struct mtd_info *mtd,struct erase_info *erase)
{
	u64 cursor=erase->addr,end;
	int rc=BMT_ENTER();
	if(rc) return rc;
	rc=managed_ready(mtd); if(rc) goto out;
	if(cursor%bmt_block || erase->len%bmt_block) { rc=-EINVAL; goto out; }
	if(cursor<(u64)bmt_first*bmt_block || cursor>=(u64)bmt_end*bmt_block ||
	   erase->len>(u64)bmt_end*bmt_block-cursor) { rc=-EROFS; goto out; }
	end=cursor+erase->len;
	while(cursor<end) {
		struct airoha_bmt_change change={.logical=cursor/bmt_block,.erase=1};
		struct airoha_bmt_progress done;
		if(fast_bit(fast_pre_erased,change.logical)) rc=0;
		else rc=airoha_bmt_transaction_apply(layer.writer,&change,&done);
		if(rc) break;
		fast_prepare(change.logical);
		cursor+=bmt_block;
	}
out:
	if(rc) erase->fail_addr=cursor;
	BMT_LEAVE(); return rc;
}
static int managed_isbad(struct mtd_info *mtd,loff_t from)
{
	u8 marker[2];
	int mapped,rc=BMT_ENTER();
	if(rc) return rc;
	rc=managed_ready(mtd); if(rc) goto out;
	if(from<0 || (u64)from>=mtd->size) { rc=-EINVAL; goto out; }
	mapped=map_block((u64)from/bmt_block);
	if(mapped<0) { rc=mapped; goto out; }
	rc=flash_markers(mtd,mapped,marker);
	if(!rc) rc=marker[0]!=255 || marker[1]!=255;
out:
	BMT_LEAVE(); return rc;
}
static int managed_markbad(struct mtd_info *mtd,loff_t from)
{
	int rc=BMT_ENTER();
	if(rc) return rc;
	rc=managed_ready(mtd);
	if(!rc) rc=managed_bounds(from,1,0,0,MTD_OPS_AUTO_OOB);
	/* A successful transparent remap is NOT logical retirement. UBI must
	 * enter read-only on this explicit refusal rather than corrupt accounting. */
	if(!rc) rc=-EOPNOTSUPP;
	BMT_LEAVE(); return rc;
}
