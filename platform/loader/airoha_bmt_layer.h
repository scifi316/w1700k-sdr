/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Attach/detach for the vendor BMT translation layer, called from spinand_probe
 * (which declares them itself). platform/loader/build.py copies it to
 * drivers/mtd/nand/.
 */
#ifndef AIROHA_BMT_LAYER_H
#define AIROHA_BMT_LAYER_H

struct mtd_info;

/* Attach vendor-logical translation, and recover persistent transactions when
 * configured. Failure refuses probe/registration; never expose raw fallback. */
int airoha_bmt_attach(struct mtd_info *mtd);

/* Whether `mtd` is attached, i.e. addressed through the vendor tables. */
int airoha_bmt_attached(const struct mtd_info *mtd);

/* Restore the original ops. Safe to call when not attached. */
void airoha_bmt_detach(struct mtd_info *mtd);

#endif /* AIROHA_BMT_LAYER_H */
