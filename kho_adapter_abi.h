/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KHO_ADAPTER_ABI_H
#define _KHO_ADAPTER_ABI_H

#include "kho_adapter_uapi.h"

/*
 * On-disk (KHO preserved memory) ABI of the adaptation layer.
 *
 * This layout is handed over across kexec.  Every change to the structures,
 * the magic or the version is a breaking ABI change and requires bumping
 * KHO_ADAPTER_ABI_VERSION.
 */

#define KHO_ADAPTER_MAGIC	0x5041444bU	/* 'KADP' */
#define KHO_ADAPTER_ABI_VERSION	1

/*
 * struct kho_adapter_ent - one preserved page/tlb entry.
 * @vaddr: user virtual address the page was mapped at.
 * @phys:  physical address of the preserved page.
 * @order: page order of the preserved folio (only 0 is supported for now).
 * @pad:   reserved, must be zero.
 */
struct kho_adapter_ent {
	__u64 vaddr;
	__u64 phys;
	__u32 order;
	__u32 pad;
};

/*
 * struct kho_adapter_hdr - preserved metadata header.
 * @magic:              KHO_ADAPTER_MAGIC.
 * @version:            KHO_ADAPTER_ABI_VERSION.
 * @entry_size:         size of a single struct kho_adapter_ent.
 * @hdr_size:           size of this header.
 * @nr_entries:         total number of serialized entries.
 * @blocks_pa:          physical address of the first kho_block_set block.
 * @count_per_block:    entries per serialization block.
 * @orig_pid:           pid at checkpoint time (informational only).
 * @flags:              reserved, zero.
 * @key:                stable key used for the KHO sub-tree.
 * @reserved:           padding, zero.
 *
 * The header itself is allocated with kho_alloc_preserve() and registered as
 * a KHO sub-tree.  The entries live in a separate kho_block_set, also backed
 * by kho_alloc_preserve() blocks.
 */
struct kho_adapter_hdr {
	__u32 magic;
	__u32 version;
	__u32 entry_size;
	__u32 hdr_size;
	__u64 nr_entries;
	__u64 blocks_pa;
	__u64 count_per_block;
	__u32 orig_pid;
	__u32 flags;
	char key[KHO_ADAPTER_KEY_LEN];
	__u8 reserved[16];
};

static_assert(sizeof(struct kho_adapter_ent) == 24);
static_assert(sizeof(struct kho_adapter_hdr) <= 128);

#endif /* _KHO_ADAPTER_ABI_H */

