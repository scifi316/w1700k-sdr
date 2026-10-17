/* SPDX-License-Identifier: MIT */
/*
 * AN7581 SPI-NAND controller ("SNFI": a SPI master front end plus an NFI DMA
 * engine), the SPI-NAND command set it issues, and the Airoha/EcoNet vendor
 * bad-block format (BBT + BMT) that the stock boot chain keeps on the NAND.
 *
 * ADDRESS SPACE:
 *   AN7581_SNFI_*   offsets relative to AN7581_SNFI_CTRL_BASE (0x1fa10000)
 *   AN7581_NFI_*    offsets relative to AN7581_SNFI_NFI_BASE  (0x1fa11000)
 *   AN7581_BBT_* / AN7581_BMT_*  byte offsets inside the on-flash tables;
 *                   block numbers are NAND eraseblock indices
 * The W1700K flash part, its geometry/OOB layout and the partition maps are
 * in w1700k_board.h. Register facts are [static] (spi-airoha-snfi) unless
 * tagged; the BBT/BMT format is [static] from the stock bootloader and the
 * stock kernel, read independently and required to agree.
 *
 * HAZARD: once STRADDR is programmed and a transfer started, an error or
 *   timeout leaves DMA completion unproven: the engine may still write the
 *   buffer. Keep the buffer and its mapping alive until a full reset; never
 *   unmap, retry into, or reuse it (the project kernel panics there).
 * HAZARD: a SPI-NAND status read that fails or times out does not prove the
 *   array is idle. Issue no cache/program/erase command until the part has
 *   been reset.
 */
#ifndef HW_AN7581_SNFI_H
#define HW_AN7581_SNFI_H

#include "../hw_bits.h"

/* ======================================================================== */
/* SPI control block (offsets from AN7581_SNFI_CTRL_BASE)                   */
/* ======================================================================== */

#define AN7581_SNFI_READ_MODE           0x000u
#define AN7581_SNFI_READ_IDLE_EN        0x004u
#define AN7581_SNFI_SIDLY               0x008u
#define AN7581_SNFI_CSHEXT              0x00cu
#define AN7581_SNFI_CSLEXT              0x010u
/* Matrix mode: 9 = manual (PIO), 0 = NFI DMA. */
#define AN7581_SNFI_MTX_MODE_TOG        0x014u
#define AN7581_SNFI_MTX_MODE_TOG_MODE   HW_GENMASK(3, 0)
#define AN7581_SNFI_MTX_MODE_MANUAL     9u
#define AN7581_SNFI_MTX_MODE_DMA        0u
/* Read-controller state; 0 = idle (wait for it before entering manual). */
#define AN7581_SNFI_RDCTL_FSM           0x018u
#define AN7581_SNFI_RDCTL_FSM_STATE     HW_GENMASK(3, 0)
#define AN7581_SNFI_MACMUX_SEL          0x01cu
#define AN7581_SNFI_MANUAL_EN           0x020u
#define AN7581_SNFI_MANUAL_EN_EN        HW_BIT(0)

/* Manual mode: an op FIFO of {op, length} entries and a byte data FIFO. */
#define AN7581_SNFI_OPFIFO_EMPTY        0x024u  /* bit 0 */
#define AN7581_SNFI_OPFIFO_WDATA        0x028u
#define AN7581_SNFI_OPFIFO_WDATA_OP     HW_GENMASK(13, 9)
#define AN7581_SNFI_OPFIFO_WDATA_LEN    HW_GENMASK(8, 0)
#define AN7581_SNFI_OPFIFO_FULL         0x02cu  /* bit 0 */
#define AN7581_SNFI_OPFIFO_WR           0x030u  /* write 1: push WDATA */
#define AN7581_SNFI_DFIFO_FULL          0x034u  /* bit 0 */
#define AN7581_SNFI_DFIFO_WDATA         0x038u  /* [7:0] */
#define AN7581_SNFI_DFIFO_EMPTY         0x03cu  /* bit 0 */
#define AN7581_SNFI_DFIFO_RD            0x040u  /* write 1: pop next byte */
#define AN7581_SNFI_DFIFO_RDATA         0x044u  /* [7:0] */
#define AN7581_SNFI_FIFO_FLAG           HW_BIT(0)
/* Op codes for OPFIFO_WDATA.OP; LEN is the byte count (<= 511). */
#define AN7581_SNFI_OP_CS_HIGH          0x00u   /* LEN 1 */
#define AN7581_SNFI_OP_CS_LOW           0x01u   /* LEN 1 */
#define AN7581_SNFI_OP_TX_SINGLE        0x08u
#define AN7581_SNFI_OP_TX_DUAL          0x09u
#define AN7581_SNFI_OP_TX_QUAD          0x0au
#define AN7581_SNFI_OP_RX_SINGLE        0x0cu
#define AN7581_SNFI_OP_RX_DUAL          0x0eu
#define AN7581_SNFI_OP_RX_QUAD          0x0fu
#define AN7581_SNFI_OP_MAX_LEN          511u

#define AN7581_SNFI_DUMMY               0x080u
#define AN7581_SNFI_DUMMY_CYCLES        HW_GENMASK(3, 0)
#define AN7581_SNFI_PROBE_SEL           0x088u
#define AN7581_SNFI_INTERRUPT           0x090u
#define AN7581_SNFI_INTERRUPT_EN        0x094u
#define AN7581_SNFI_SI_CK_SEL           0x09cu
#define AN7581_SNFI_SW_CFGNANDADDR_VAL  0x10cu
#define AN7581_SNFI_SW_CFGNANDADDR_EN   0x110u
/* Boot strap snapshot. On the EN7523 path bit 2 = 0 flags the "reserved"
 * boot mode (UART TXD held to ground at reset), in which DMA reads of some
 * flashes are unreliable; whether the AN7581 shares this is unverified. */
#define AN7581_SNFI_SFC_STRAP           0x114u
#define AN7581_SNFI_SFC_STRAP_NORMAL    HW_BIT(2)
/* Route the SPI master to the NFI DMA engine. */
#define AN7581_SNFI_NFI2SPI_EN          0x130u
#define AN7581_SNFI_NFI2SPI_EN_EN       HW_BIT(0)

/*
 * Mode switches [static]:
 *   manual: NFI2SPI_EN = 0; READ_IDLE_EN = 0; wait RDCTL_FSM == 0;
 *           MTX_MODE_TOG = 9; MANUAL_EN = 1; DUMMY = 0.
 *   DMA:    NFI2SPI_EN = 1; MTX_MODE_TOG = 0; MANUAL_EN = 0; DUMMY = 0.
 * Every FIFO poll in the driver times out after 250 ms.
 */

/* ======================================================================== */
/* NFI DMA engine (offsets from AN7581_SNFI_NFI_BASE)                       */
/* ======================================================================== */

#define AN7581_NFI_CNFG                 0x000u
#define AN7581_NFI_CNFG_OPMODE          HW_GENMASK(14, 12)
#define AN7581_NFI_CNFG_OPMODE_PROGRAM  3u
#define AN7581_NFI_CNFG_OPMODE_READ     6u
#define AN7581_NFI_CNFG_AUTO_FDM_EN     HW_BIT(9)   /* kept 0: SECCUS sizing */
#define AN7581_NFI_CNFG_HW_ECC_EN       HW_BIT(8)   /* kept 0: on-die ECC */
#define AN7581_NFI_CNFG_DMA_BURST_EN    HW_BIT(2)   /* needs 16-byte alignment */
#define AN7581_NFI_CNFG_READ_MODE       HW_BIT(1)
#define AN7581_NFI_CNFG_DMA_MODE        HW_BIT(0)

#define AN7581_NFI_PAGEFMT              0x004u
#define AN7581_NFI_PAGEFMT_SPARE_SIZE   HW_GENMASK(5, 4)
#define AN7581_NFI_PAGEFMT_PAGE_SIZE    HW_GENMASK(1, 0)

#define AN7581_NFI_CON                  0x008u
#define AN7581_NFI_CON_SEC_NUM          HW_GENMASK(15, 12)
#define AN7581_NFI_CON_WR_TRIG          HW_BIT(9)   /* 0 -> 1 starts */
#define AN7581_NFI_CON_RD_TRIG          HW_BIT(8)   /* 0 -> 1 starts */
#define AN7581_NFI_CON_RST              HW_BIT(1)
#define AN7581_NFI_CON_FIFO_FLUSH       HW_BIT(0)

#define AN7581_NFI_INTR_EN              0x010u
#define AN7581_NFI_INTR_EN_AHB_DONE     HW_BIT(6)   /* the only one enabled */
#define AN7581_NFI_INTR_EN_ACCESS_LOCK  HW_BIT(5)
#define AN7581_NFI_INTR_EN_BUSY_RETURN  HW_BIT(4)
#define AN7581_NFI_INTR_EN_ERASE_DONE   HW_BIT(3)
#define AN7581_NFI_INTR_EN_RST_DONE     HW_BIT(2)
#define AN7581_NFI_INTR_EN_WR_DONE      HW_BIT(1)
#define AN7581_NFI_INTR_EN_RD_DONE      HW_BIT(0)
#define AN7581_NFI_INTR                 0x014u
#define AN7581_NFI_INTR_AHB_DONE        HW_BIT(6)   /* DMA reached DRAM */

/* Command for the NFI sequencer: 0x00 for a cache read, 0x80 for a program. */
#define AN7581_NFI_CMD                  0x020u
#define AN7581_NFI_CMD_READ             0x00u
#define AN7581_NFI_CMD_PROGRAM          0x80u
#define AN7581_NFI_ADDR_NOB             0x030u
#define AN7581_NFI_ADDR_NOB_ROW         HW_GENMASK(6, 4)
#define AN7581_NFI_STA                  0x060u
#define AN7581_NFI_FIFOSTA              0x064u
/* DMA bus address (32-bit). */
#define AN7581_NFI_STRADDR              0x080u
/* Spare (FDM) bytes per sector n = 0..7: low and high words. */
#define AN7581_NFI_FDM_L(n)             (0x0a0u + 0x8u * (n))
#define AN7581_NFI_FDM_M(n)             (0x0a4u + 0x8u * (n))
#define AN7581_NFI_FIFODATA(n)          (0x190u + 0x4u * (n)) /* n = 0..3 */
#define AN7581_NFI_MASTERSTA            0x224u
/* Custom sector size: the whole transfer is one "sector" of this size. */
#define AN7581_NFI_SECCUS_SIZE          0x22cu
#define AN7581_NFI_SECCUS_SIZE_EN       HW_BIT(16)
#define AN7581_NFI_SECCUS_SIZE_SIZE     HW_GENMASK(12, 0)
#define AN7581_NFI_RD_CTL2              0x510u
#define AN7581_NFI_RD_CTL2_READ_CMD     HW_GENMASK(7, 0)  /* SPI-NAND cache-read opcode */
#define AN7581_NFI_RD_CTL3              0x514u              /* column address */
#define AN7581_NFI_PG_CTL1              0x524u
#define AN7581_NFI_PG_CTL1_LOAD_CMD     HW_GENMASK(15, 8) /* SPI-NAND program-load opcode */
#define AN7581_NFI_PG_CTL2              0x528u              /* column address */
#define AN7581_NFI_NOR_PROG_ADDR        0x52cu
#define AN7581_NFI_NOR_RD_ADDR          0x534u
#define AN7581_NFI_SNF_MISC_CTL         0x538u
#define AN7581_NFI_SNF_MISC_CTL_RW_MODE HW_GENMASK(18, 16) /* 0 x1, 1 x2, 2 x4 */
#define AN7581_NFI_SNF_MISC_CTL2        0x53cu
#define AN7581_NFI_SNF_MISC_CTL2_PROG_BYTES HW_GENMASK(28, 16)
#define AN7581_NFI_SNF_MISC_CTL2_READ_BYTES HW_GENMASK(12, 0)
/* Completion flags: write the flag back after use, even if already set. */
#define AN7581_NFI_SNF_STA_CTL1         0x550u
#define AN7581_NFI_SNF_STA_CTL1_LOAD_TO_CACHE_DONE   HW_BIT(26)
#define AN7581_NFI_SNF_STA_CTL1_READ_FROM_CACHE_DONE HW_BIT(25)
#define AN7581_NFI_SNF_STA_CTL2         0x554u
#define AN7581_NFI_SNF_NFI_CNFG         0x55cu
#define AN7581_NFI_SNF_NFI_CNFG_SPI_MODE HW_BIT(0)    /* set once at init */

/*
 * DMA cache read [static]:
 *   1. DMA mode (above); CON = RST | FIFO_FLUSH.
 *   2. CNFG: DMA_MODE | READ_MODE | DMA_BURST_EN | OPMODE 6, HW_ECC_EN and
 *      AUTO_FDM_EN clear.
 *   3. CON.SEC_NUM = 1; SECCUS_SIZE = EN | bytes; bytes = offset + length
 *      rounded up to 64. Rule: transfer length = SNF_MISC_CTL2.READ_BYTES =
 *      SEC_NUM x SECCUS size.
 *   4. STRADDR = buffer (a 4096 + 256-byte bounce buffer in mainline).
 *   5. RD_CTL2 = opcode (0x03/0x0b single, 0x3b dual, 0x6b quad; the dual-IO
 *      and quad-IO forms are not supported and are replaced by these);
 *      SNF_MISC_CTL.RW_MODE; RD_CTL3 = column; CMD = 0x00.
 *   6. RD_TRIG 0 -> 1; poll SNF_STA_CTL1.READ_FROM_CACHE_DONE (1 s), write it
 *      back; poll INTR.AHB_DONE (1 s); wait 1 us before reading DRAM.
 *   Programs mirror this with OPMODE 3, PG_CTL1/PG_CTL2, CMD = 0x80,
 *   WR_TRIG and LOAD_TO_CACHE_DONE.
 * The engine is a 32-bit DMA master and is not cache-coherent.
 */
#define AN7581_NFI_DMA_BUF_BYTES        (4096u + 256u)
#define AN7581_NFI_DMA_GRANULE          64u
#define AN7581_NFI_DMA_TIMEOUT_US       1000000u

/* ======================================================================== */
/* SPI-NAND command opcodes issued by the controller driver                 */
/* ======================================================================== */

#define AN7581_SNAND_OP_GET_FEATURE     0x0fu
#define AN7581_SNAND_OP_SET_FEATURE     0x1fu
#define AN7581_SNAND_OP_PAGE_READ       0x13u
#define AN7581_SNAND_OP_READ_CACHE      0x03u
#define AN7581_SNAND_OP_READ_CACHE_FAST 0x0bu
#define AN7581_SNAND_OP_READ_CACHE_X2   0x3bu
#define AN7581_SNAND_OP_READ_CACHE_DUALIO 0xbbu
#define AN7581_SNAND_OP_READ_CACHE_X4   0x6bu
#define AN7581_SNAND_OP_READ_CACHE_QUADIO 0xebu
#define AN7581_SNAND_OP_WRITE_ENABLE    0x06u
#define AN7581_SNAND_OP_WRITE_DISABLE   0x04u
#define AN7581_SNAND_OP_PROGRAM_LOAD    0x02u
#define AN7581_SNAND_OP_PROGRAM_LOAD_X4 0x32u
#define AN7581_SNAND_OP_PROGRAM_LOAD_RANDOM    0x84u
#define AN7581_SNAND_OP_PROGRAM_LOAD_RANDOM_X4 0x34u
#define AN7581_SNAND_OP_PROGRAM_EXECUTE 0x10u
#define AN7581_SNAND_OP_READ_ID         0x9fu
#define AN7581_SNAND_OP_BLOCK_ERASE     0xd8u
#define AN7581_SNAND_OP_RESET           0xffu
#define AN7581_SNAND_OP_DIE_SELECT      0xc2u

/* ======================================================================== */
/* Vendor bad-block format: BBT (factory bad list) + BMT (remap table)      */
/* ======================================================================== */

/*
 * Translation, in this order, for every vendor "flash read/write/erase"
 * (stock bootloader) and the stock kernel [static]:
 *   logical --[BBT shift]--> physical --[BMT substitution]--> final
 * BBT shift: each factory-bad block in the BBT shifts every logical block
 *   from its position upward by one (precomputed per logical block); blocks
 *   at or above (system_blocks - bbt_entries) are not shifted. The BBT
 *   entries also shrink the usable area.
 * BMT substitution: a block < system_blocks is looked up linearly among the
 *   BMT's `from` entries and replaced by `to`; blocks >= system_blocks are
 *   never translated. There is no block-0 special case.
 * The reserve pool is [system_blocks, total). The BBT is the first valid
 * table found scanning UPWARD from block system_blocks; the BMT is the first
 * valid table found scanning DOWNWARD from the last block. Neither carries a
 * generation counter. Both scans skip blocks whose OOB byte 0 or 1 is not
 * 0xff. A table occupies page 0 of its block.
 *
 * Pool sizing (total = blocks on the part) [static]:
 *   scan downward until total*8/100 good blocks are counted, a good block
 *   being one whose OOB byte 0 (the factory marker) reads 0xff; byte 1 (worn)
 *   is not looked at, and a failed marker read is not checked (U-Boot
 *   0x81e1b540, is_bad(block, 0) at 0x81e1b49c); pool = total - that cursor
 *   (>= total*8/100, growing with factory-bad blocks in the span);
 *   pool > total/8 fails BMT init ("bmt pool size > maximum");
 *   otherwise pool is clamped to 250, the table capacity, and
 *   system_blocks = total - pool.
 *   For any part of >= 3138 blocks the clamp always fires, so pool = 250.
 *   On a 2048-block part (W25N04LV) it does not: pool = 163 + the factory-bad
 *   blocks in the span, per unit, and the BMT checksum covers that many
 *   entries.
 *   The usable size published to Linux by the stock kernel is fixed at
 *   (total - total/8) blocks, independent of the measured pool.
 *
 * Stock behaviour [static]: allocation of data replacements scans upward and
 * of metadata downward, excluding current tables, marked blocks and live
 * destinations; the first allocation also erases other apparently free
 * reserve blocks. After accepting a nonempty BMT, the stock loader marks
 * unmarked original source blocks bad: a stock boot is not read-only. A
 * remapped replacement block carries its source block number as a
 * little-endian u16 at OOB bytes 2-3.
 *
 * HAZARD: OpenWrt's airoha BMT driver computes an unclamped pool (>= 327 on a
 *   4096-block part), checksums past the 1000-byte entry area, fails a
 *   vendor-written BMT, then erases and rewrites the table, destroying the
 *   factory remap state. It also translates block == system_blocks, which
 *   the vendor does not. Never run it against a stock-formatted part.
 */
#define AN7581_BMT_MAX_ENTRIES          250u
#define AN7581_BMT_POOL_GOOD(total)     ((total) * 8u / 100u)
#define AN7581_BMT_POOL_CEILING(total)  ((total) / 8u)
#define AN7581_BMT_USER_BLOCKS(total)   ((total) - (total) / 8u)

/* BBT on flash: 512 bytes. */
#define AN7581_BBT_BYTES                0x200u
#define AN7581_BBT_MAGIC                0x0u    /* "RAWB", 4 bytes */
#define AN7581_BBT_MAGIC_STR            "RAWB"
/* (version + size + sum of the 500 entry bytes) & 0xffff, stored as u32 LE. */
#define AN7581_BBT_CHECKSUM             0x4u
#define AN7581_BBT_VERSION              0x8u    /* u8 */
#define AN7581_BBT_SIZE                 0x9u    /* u8, entry count */
#define AN7581_BBT_ENTRIES              0xcu    /* u16 LE block[250] */
#define AN7581_BBT_ENTRY_BYTES          2u

/* BMT on flash: 1020 bytes. */
#define AN7581_BMT_BYTES                0x3fcu
#define AN7581_BMT_MAGIC                0x0u    /* "BMT", 3 bytes */
#define AN7581_BMT_MAGIC_STR            "BMT"
#define AN7581_BMT_VERSION              0x3u    /* u8, always 1 */
#define AN7581_BMT_VERSION_VAL          1u
#define AN7581_BMT_UNUSED               0x4u    /* u8, left 0xff */
#define AN7581_BMT_SIZE                 0x5u    /* u8, entry count */
/* (version + size + sum of the pool's entry bytes) & 0xff. The extent is the
 * runtime pool, not SIZE: 250 entries = 1000 bytes on the W25N04KV (clamped),
 * the scanned pool on the W25N04LV. Unused entries are zero. */
#define AN7581_BMT_CHECKSUM             0x6u
#define AN7581_BMT_ENTRIES              0x14u   /* {u16 from; u16 to}[250], LE */
#define AN7581_BMT_ENTRY_BYTES          4u
#define AN7581_BMT_ENTRY_FROM           0x0u
#define AN7581_BMT_ENTRY_TO             0x2u
/* Trap: the vendor's in-memory structs put the entry array first and the
 * version/size bytes after it (BMT +0x3e8/+0x3e9, BBT +0x1f4/+0x1f5); a
 * parser written against them misreads the flash layout above. */

/* OOB marker bytes (first page of a block). */
#define AN7581_BMT_OOB_FACTORY_BAD      0u      /* != 0xff: factory bad */
#define AN7581_BMT_OOB_WORN_BAD         1u      /* != 0xff: worn bad */
#define AN7581_BMT_OOB_BACKREF          2u      /* u16 LE source block */

#endif /* HW_AN7581_SNFI_H */
