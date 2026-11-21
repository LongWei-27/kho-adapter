// SPDX-License-Identifier: GPL-2.0
/*
 * kho-adapter: KHO adaptation layer for CRIU.
 *
 * Checkpoints the anonymous memory of a process into KHO and restores it in
 * the next kernel after a KHO enabled kexec.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pagewalk.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include "kho_adapter_abi.h"
#include "kho_adapter_uapi.h"

#if !IS_ENABLED(CONFIG_KEXEC_HANDOVER)
#error "kho-adapter requires CONFIG_KEXEC_HANDOVER"
#endif

/*
 * Upper bound of 4K pages that a single checkpoint may contain.  The default
 * is 4M pages (16GiB).  It is only a sanity limit for the kernel side
 * bookkeeping allocation, not a hard KHO limit.
 */
static unsigned long max_pages = 4UL * 1024 * 1024;
module_param(max_pages, ulong, 0644);
MODULE_PARM_DESC(max_pages, "Maximum number of 4K pages per checkpoint");

/*
 * Chained metadata blocks.  Each block is one kho_alloc_preserve() page.  The
 * layout intentionally mirrors what kho_block does, but is kept private to
 * this module because the generic helpers are not exported to modules.
 */
struct kho_adapter_block_hdr {
	__u64 next_pa;
	__u64 count;
} __packed;

#define KHO_ADAPTER_BLOCK_HDR_SIZE	sizeof(struct kho_adapter_block_hdr)
#define KHO_ADAPTER_ENTRIES_PER_BLOCK \
	((PAGE_SIZE - KHO_ADAPTER_BLOCK_HDR_SIZE) / \
	 sizeof(struct kho_adapter_ent))
#define KHO_ADAPTER_MAX_BLOCKS		65536

struct kho_adapter_ctx {
	struct list_head node;
	char key[KHO_ADAPTER_KEY_LEN];
	void *hdr;
	void **blocks;
	unsigned int nblocks;
	struct folio **folios;
	u64 *pfns;
	u64 nr_pages;
	bool incoming;
};

/* Checkpoints created in this kernel, waiting for kexec. */
static LIST_HEAD(outgoing_list);
/* Checkpoints restored in this kernel, kept alive until module unload. */
static LIST_HEAD(incoming_list);

/*
 * Protects the lists, the outgoing page refcount and the restored page cache.
 * All ioctls are serialized through this lock.
 */
static DEFINE_MUTEX(adapter_lock);

/*
 * Outgoing preserved pages: pfn -> refcount.  KHO's preserved memory map is a
 * set, not a refcounted map, so the adapter has to avoid unpreserving a page
 * that is still referenced by another checkpoint (shared/COW pages).
 */
static DEFINE_XARRAY(outgoing_pages);

/* Pages restored in this kernel: pfn -> struct page, holding one reference. */
static DEFINE_XARRAY(restored_pages);
static atomic64_t restored_page_count = ATOMIC64_INIT(0);

static int kho_adapter_validate_key(const char *key)
{
	size_t len = strnlen(key, KHO_ADAPTER_KEY_LEN);
	size_t i;

	if (!len || len >= KHO_ADAPTER_KEY_LEN)
		return -EINVAL;

	for (i = 0; i < len; i++) {
		unsigned char c = key[i];

		if (!isalnum(c) && c != '_' && c != '-' && c != '.')
			return -EINVAL;
	}

	return 0;
}

static int kho_adapter_get_mm(u32 pid, struct mm_struct **mmp)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct pid *pidp;

	pidp = find_get_pid(pid);
	if (!pidp)
		return -ESRCH;

	task = get_pid_task(pidp, PIDTYPE_PID);
	put_pid(pidp);
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm)
		return -EINVAL;

	*mmp = mm;
	return 0;
}

static bool kho_adapter_vma_supported(const struct vm_area_struct *vma)
{
	/* Only private anonymous mappings without special backing. */
	if (!vma_is_anonymous(vma))
		return false;
	if (vma->vm_flags & (VM_IO | VM_PFNMAP | VM_HUGETLB))
		return false;

	return true;
}

struct kho_adapter_collect {
	struct kho_adapter_ent *ents;
	struct folio **folios;
	unsigned long nr;
	unsigned long max;
	unsigned long skipped;
	bool overflow;
};

static int kho_adapter_pmd_entry(pmd_t *pmd, unsigned long addr,
				 unsigned long next, struct mm_walk *walk)
{
	struct kho_adapter_collect *c = walk->private;

	if (!pmd_present(*pmd))
		return 0;

	/*
	 * Transparent huge pages are not supported by the current ABI.  Do not
	 * let the generic page walk split them either: that would mutate the
	 * target process.  Skip the whole PMD.
	 */
	if (pmd_leaf(*pmd)) {
		c->skipped += (next - addr) >> PAGE_SHIFT;
		walk->action = ACTION_CONTINUE;
	}

	return 0;
}

static int kho_adapter_pte_entry(pte_t *pte, unsigned long addr,
				 unsigned long next, struct mm_walk *walk)
{
	struct kho_adapter_collect *c = walk->private;
	struct folio *folio;
	struct page *page;
	unsigned long pfn;

	if (pte_none(*pte) || !pte_present(*pte) || pte_special(*pte))
		return 0;

	pfn = pte_pfn(*pte);
	if (!pfn_valid(pfn) || is_zero_pfn(pfn))
		return 0;

	page = pfn_to_page(pfn);
	folio = page_folio(page);

	/* Large folios (including PTE-mapped mTHP) are not supported yet. */
	if (folio_order(folio) != 0) {
		c->skipped++;
		return 0;
	}

	if (c->nr >= c->max) {
		c->overflow = true;
		return -ENOSPC;
	}

	folio_get(folio);

	c->folios[c->nr] = folio;
	c->ents[c->nr].vaddr = addr;
	c->ents[c->nr].phys = PFN_PHYS(pfn);
	c->ents[c->nr].order = 0;
	c->ents[c->nr].pad = 0;
	c->nr++;

	return 0;
}

static const struct mm_walk_ops kho_adapter_walk_ops = {
	.pmd_entry = kho_adapter_pmd_entry,
	.pte_entry = kho_adapter_pte_entry,
	.walk_lock = PGWALK_RDLOCK,
};

/*
 * Increment the outgoing preservation refcount of a 0-order folio.  The KHO
 * radix tree is a set, so the actual kho_preserve_folio() is only called on
 * the 0 -> 1 transition.
 */
static int kho_adapter_preserve_folio(struct folio *folio)
{
	unsigned long pfn = folio_pfn(folio);
	void *entry, *new;
	unsigned long refs;
	int err;

	entry = xa_load(&outgoing_pages, pfn);
	refs = entry ? xa_to_value(entry) : 0;

	if (refs) {
		if (refs == ULONG_MAX)
			return -EOVERFLOW;

		new = xa_mk_value(refs + 1);
	} else {
		err = kho_preserve_folio(folio);
		if (err)
			return err;
		new = xa_mk_value(1);
	}

	entry = xa_store(&outgoing_pages, pfn, new, GFP_KERNEL);
	if (xa_is_err(entry)) {
		if (!refs)
			kho_unpreserve_pages(pfn_to_page(pfn), 1);
		return xa_err(entry);
	}

	return 0;
}

static void kho_adapter_unpreserve_pfn(unsigned long pfn)
{
	void *entry = xa_load(&outgoing_pages, pfn);
	unsigned long refs = entry ? xa_to_value(entry) : 0;

	if (!refs)
		return;

	if (refs > 1) {
		xa_store(&outgoing_pages, pfn, xa_mk_value(refs - 1),
			 GFP_KERNEL);
		return;
	}

	xa_erase(&outgoing_pages, pfn);
	kho_unpreserve_pages(pfn_to_page(pfn), 1);
}

static void kho_adapter_unpreserve_ctx(struct kho_adapter_ctx *ctx)
{
	u64 i;

	for (i = 0; i < ctx->nr_pages; i++)
		kho_adapter_unpreserve_pfn((unsigned long)ctx->pfns[i]);
}

static void kho_adapter_free_ctx(struct kho_adapter_ctx *ctx)
{
	unsigned int i;

	if (ctx->incoming) {
		for (i = 0; i < ctx->nblocks; i++)
			kho_restore_free(ctx->blocks[i]);
		kho_restore_free(ctx->hdr);
	} else {
		if (ctx->hdr) {
			kho_remove_subtree(ctx->hdr);
			kho_unpreserve_free(ctx->hdr);
		}
		for (i = 0; i < ctx->nblocks; i++)
			kho_unpreserve_free(ctx->blocks[i]);
		kho_adapter_unpreserve_ctx(ctx);

		for (i = 0; i < ctx->nr_pages; i++)
			folio_put(ctx->folios[i]);
	}

	kvfree(ctx->blocks);
	kvfree(ctx->folios);
	kvfree(ctx->pfns);
	kfree(ctx);
}

static long kho_adapter_checkpoint(struct kho_adapter_req *req)
{
	struct kho_adapter_collect c = { };
	struct kho_adapter_ctx *ctx = NULL;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long total = 0, i, preserved = 0;
	u64 nblocks;
	unsigned int b;
	int err;

	if (!kho_is_enabled())
		return -EOPNOTSUPP;

	err = kho_adapter_get_mm(req->pid, &mm);
	if (err)
		return err;

	mmap_read_lock(mm);
	{
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma) {
			unsigned long len;

			if (!kho_adapter_vma_supported(vma))
				continue;

			len = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
			if (check_add_overflow(total, len, &total)) {
				err = -EOVERFLOW;
				goto out_unlock;
			}
		}
	}

	if (total > max_pages) {
		err = -E2BIG;
		goto out_unlock;
	}

	if (total) {
		c.ents = kvmalloc_array(total, sizeof(*c.ents),
					GFP_KERNEL | __GFP_ZERO);
		c.folios = kvmalloc_array(total, sizeof(*c.folios), GFP_KERNEL);
		if (!c.ents || !c.folios) {
			err = -ENOMEM;
			goto out_unlock;
		}
	}
	c.max = total;

	{
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma) {
			if (!kho_adapter_vma_supported(vma))
				continue;

			err = walk_page_range(mm, vma->vm_start, vma->vm_end,
					      &kho_adapter_walk_ops, &c);
			if (err)
				break;
		}
	}

out_unlock:
	mmap_read_unlock(mm);

	if (err)
		goto out_put_folios;

	if (c.overflow) {
		err = -ENOSPC;
		goto out_put_folios;
	}

	if (c.skipped)
		pr_info("pid %u: skipped %lu huge/large pages\n",
			req->pid, c.skipped);

	/* Register the pages with KHO, keeping adapter level refcounts. */
	for (i = 0; i < c.nr; i++) {
		err = kho_adapter_preserve_folio(c.folios[i]);
		if (err)
			goto out_rollback;
		/* The folio may have been migrated since the page walk. */
		c.ents[i].phys = PFN_PHYS(folio_pfn(c.folios[i]));
		preserved++;
	}

	ctx = kzalloc_obj(*ctx);
	if (!ctx) {
		err = -ENOMEM;
		goto out_rollback;
	}

	memcpy(ctx->key, req->key, sizeof(ctx->key));

	nblocks = c.nr ? DIV_ROUND_UP(c.nr, KHO_ADAPTER_ENTRIES_PER_BLOCK) : 0;
	if (nblocks > KHO_ADAPTER_MAX_BLOCKS) {
		err = -E2BIG;
		goto out_free_ctx;
	}

	if (nblocks) {
		ctx->blocks = kvmalloc_array(nblocks, sizeof(*ctx->blocks),
					     GFP_KERNEL);
		if (!ctx->blocks) {
			err = -ENOMEM;
			goto out_free_ctx;
		}
	}

	for (b = 0; b < nblocks; b++) {
		struct kho_adapter_block_hdr *bh;
		unsigned long first = (unsigned long)b *
				      KHO_ADAPTER_ENTRIES_PER_BLOCK;
		unsigned long cnt = min_t(unsigned long,
					  KHO_ADAPTER_ENTRIES_PER_BLOCK,
					  c.nr - first);
		void *blk = kho_alloc_preserve(PAGE_SIZE);

		if (IS_ERR(blk)) {
			err = PTR_ERR(blk);
			goto out_free_ctx;
		}

		ctx->blocks[b] = blk;
		ctx->nblocks = b + 1;

		memset(blk, 0, PAGE_SIZE);
		bh = blk;
		bh->count = cnt;
		memcpy((char *)blk + KHO_ADAPTER_BLOCK_HDR_SIZE,
		       &c.ents[first], cnt * sizeof(struct kho_adapter_ent));

		if (b)
			((struct kho_adapter_block_hdr *)
			 ctx->blocks[b - 1])->next_pa = virt_to_phys(blk);
	}

	ctx->pfns = kvmalloc_array(c.nr, sizeof(*ctx->pfns), GFP_KERNEL);
	if (c.nr && !ctx->pfns) {
		err = -ENOMEM;
		goto out_free_ctx;
	}
	for (i = 0; i < c.nr; i++)
		ctx->pfns[i] = PHYS_PFN(c.ents[i].phys);
	ctx->nr_pages = c.nr;

	/* Keep the folio references until unpreserve/kexec. */
	ctx->folios = c.folios;
	c.folios = NULL;

	ctx->hdr = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(ctx->hdr)) {
		err = PTR_ERR(ctx->hdr);
		ctx->hdr = NULL;
		goto out_free_ctx;
	}

	{
		struct kho_adapter_hdr *hdr = ctx->hdr;

		memset(hdr, 0, PAGE_SIZE);
		hdr->magic = KHO_ADAPTER_MAGIC;
		hdr->version = KHO_ADAPTER_ABI_VERSION;
		hdr->entry_size = sizeof(struct kho_adapter_ent);
		hdr->hdr_size = sizeof(*hdr);
		hdr->nr_entries = c.nr;
		hdr->blocks_pa = ctx->nblocks ?
				 virt_to_phys(ctx->blocks[0]) : 0;
		hdr->count_per_block = KHO_ADAPTER_ENTRIES_PER_BLOCK;
		hdr->orig_pid = req->pid;
		memcpy(hdr->key, ctx->key, sizeof(hdr->key));
	}

	err = kho_add_subtree(ctx->key, ctx->hdr, PAGE_SIZE);
	if (err)
		goto out_free_ctx;

	ctx->incoming = false;
	list_add_tail(&ctx->node, &outgoing_list);

	pr_info("checkpointed pid %u key=%s pages=%lu\n",
		req->pid, ctx->key, c.nr);
	err = 0;
	goto out_put_folios;

out_free_ctx:
	/*
	 * If the context owns the page list it also owns the unpreserve
	 * rollback.  Otherwise the pages were preserved but the context could
	 * not be completed, so roll them back explicitly.
	 */
	if (ctx->nr_pages)
		preserved = 0;
	kho_adapter_free_ctx(ctx);
out_rollback:
	if (preserved) {
		for (i = 0; i < preserved; i++)
			kho_adapter_unpreserve_pfn(PHYS_PFN(c.ents[i].phys));
	}
out_put_folios:
	if (c.folios) {
		for (i = 0; i < c.nr; i++)
			folio_put(c.folios[i]);
	}
	kvfree(c.ents);
	kvfree(c.folios);
	mmput(mm);

	return err;
}

static struct page *kho_adapter_get_restored_page(u64 phys)
{
	unsigned long pfn = PHYS_PFN(phys);
	struct page *page;
	void *old;

	page = xa_load(&restored_pages, pfn);
	if (page)
		return page;

	if (!pfn_valid(pfn) || is_zero_pfn(pfn))
		return ERR_PTR(-EINVAL);

	page = kho_restore_pages(phys, 1);
	if (!page)
		return ERR_PTR(-EIO);

	old = xa_store(&restored_pages, pfn, page, GFP_KERNEL);
	if (xa_is_err(old)) {
		int err = xa_err(old);

		folio_put(page_folio(page));
		return ERR_PTR(err);
	}

	atomic64_inc(&restored_page_count);
	return page;
}

static long kho_adapter_restore(struct kho_adapter_req *req)
{
	struct kho_adapter_ctx *ctx, *iter;
	struct kho_adapter_hdr *hdr;
	struct mm_struct *mm;
	phys_addr_t phys;
	u64 max_blocks, pa, count = 0;
	size_t size;
	unsigned int b;
	int err;

	if (!is_kho_boot())
		return -ENODEV;

	list_for_each_entry(iter, &incoming_list, node) {
		if (!strcmp(iter->key, req->key))
			return -EEXIST;
	}

	err = kho_retrieve_subtree(req->key, &phys, &size);
	if (err)
		return err;

	if (size < sizeof(*hdr))
		return -EINVAL;

	hdr = phys_to_virt(phys);
	if (hdr->magic != KHO_ADAPTER_MAGIC ||
	    hdr->version != KHO_ADAPTER_ABI_VERSION ||
	    hdr->entry_size != sizeof(struct kho_adapter_ent) ||
	    hdr->hdr_size != sizeof(*hdr) ||
	    strcmp(hdr->key, req->key) ||
	    hdr->count_per_block != KHO_ADAPTER_ENTRIES_PER_BLOCK)
		return -EINVAL;

	max_blocks = hdr->nr_entries ?
		     DIV_ROUND_UP(hdr->nr_entries,
				  KHO_ADAPTER_ENTRIES_PER_BLOCK) : 0;
	if (max_blocks > KHO_ADAPTER_MAX_BLOCKS)
		return -E2BIG;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	memcpy(ctx->key, req->key, sizeof(ctx->key));
	ctx->incoming = true;
	ctx->hdr = hdr;
	ctx->nr_pages = hdr->nr_entries;

	if (max_blocks) {
		ctx->blocks = kvmalloc_array(max_blocks, sizeof(*ctx->blocks),
					     GFP_KERNEL);
		if (!ctx->blocks) {
			err = -ENOMEM;
			goto out_keep_ctx;
		}
	}

	/* Parse and remember the physical block chain first. */
	pa = hdr->blocks_pa;
	for (b = 0; pa; b++) {
		struct kho_adapter_block_hdr *bh;

		if (b >= max_blocks || !pfn_valid(PHYS_PFN(pa))) {
			err = -EINVAL;
			goto out_keep_ctx;
		}

		bh = phys_to_virt(pa);
		ctx->blocks[b] = bh;
		ctx->nblocks = b + 1;

		if (!bh->count || bh->count > KHO_ADAPTER_ENTRIES_PER_BLOCK) {
			err = -EINVAL;
			goto out_keep_ctx;
		}

		pa = bh->next_pa;
	}
	if (b != max_blocks) {
		err = -EINVAL;
		goto out_keep_ctx;
	}

	err = kho_adapter_get_mm(req->pid, &mm);
	if (err)
		goto out_keep_ctx;

	mmap_write_lock(mm);

	for (b = 0; b < ctx->nblocks; b++) {
		struct kho_adapter_block_hdr *bh = ctx->blocks[b];
		struct kho_adapter_ent *ents =
			(void *)((char *)bh + KHO_ADAPTER_BLOCK_HDR_SIZE);
		unsigned int j;

		for (j = 0; j < bh->count; j++) {
			struct kho_adapter_ent *ent = &ents[j];
			struct vm_area_struct *vma;
			struct page *page;

			if (count >= hdr->nr_entries || ent->order ||
			    ent->pad) {
				err = -EINVAL;
				goto out_unlock;
			}

			vma = find_vma(mm, ent->vaddr);
			if (!vma || ent->vaddr < vma->vm_start ||
			    ent->vaddr + PAGE_SIZE > vma->vm_end ||
			    vma->vm_ops ||
			    (vma->vm_flags &
			     (VM_IO | VM_PFNMAP | VM_HUGETLB))) {
				err = -EINVAL;
				goto out_unlock;
			}

			page = kho_adapter_get_restored_page(ent->phys);
			if (IS_ERR(page)) {
				err = PTR_ERR(page);
				goto out_unlock;
			}

			err = vm_insert_page(vma, ent->vaddr, page);
			if (err)
				goto out_unlock;

			count++;
		}
	}

	if (count != hdr->nr_entries) {
		err = -EINVAL;
		goto out_unlock;
	}

	err = 0;
out_unlock:
	mmap_write_unlock(mm);
	mmput(mm);
	if (err)
		goto out_keep_ctx;

	list_add_tail(&ctx->node, &incoming_list);
	pr_info("restored pid %u key=%s pages=%llu\n",
		req->pid, ctx->key, count);
	return 0;

out_keep_ctx:
	/*
	 * Keep the restored metadata alive even on failure.  The header and the
	 * block pages live in KHO preserved memory; releasing them while the
	 * key is still present in the incoming FDT would allow a retry to read
	 * freed memory.
	 */
	list_add_tail(&ctx->node, &incoming_list);
	return err;
}

static long kho_adapter_unpreserve(struct kho_adapter_req *req)
{
	struct kho_adapter_ctx *ctx;

	list_for_each_entry(ctx, &outgoing_list, node) {
		if (strcmp(ctx->key, req->key))
			continue;

		list_del(&ctx->node);
		kho_adapter_free_ctx(ctx);
		return 0;
	}

	return -ENOENT;
}

static void kho_adapter_status(struct kho_adapter_status *st)
{
	struct kho_adapter_ctx *ctx;

	memset(st, 0, sizeof(*st));
	st->abi_version = KHO_ADAPTER_ABI_VERSION;

	list_for_each_entry(ctx, &outgoing_list, node)
		st->outgoing_keys++;
	list_for_each_entry(ctx, &incoming_list, node)
		st->restored_keys++;
	st->restored_pages = atomic64_read(&restored_page_count);
}

static long kho_adapter_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct kho_adapter_req req;
	struct kho_adapter_status st;
	long err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (cmd) {
	case KHO_ADAPTER_IOC_CHECKPOINT:
	case KHO_ADAPTER_IOC_RESTORE:
	case KHO_ADAPTER_IOC_UNPRESERVE:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		err = kho_adapter_validate_key(req.key);
		if (err)
			return err;
		if (req.flags)
			return -EINVAL;

		mutex_lock(&adapter_lock);
		if (cmd == KHO_ADAPTER_IOC_CHECKPOINT) {
			struct kho_adapter_ctx *ctx;

			list_for_each_entry(ctx, &outgoing_list, node) {
				if (!strcmp(ctx->key, req.key)) {
					err = -EEXIST;
					goto out_unlock;
				}
			}
			err = kho_adapter_checkpoint(&req);
		} else if (cmd == KHO_ADAPTER_IOC_RESTORE) {
			err = kho_adapter_restore(&req);
		} else {
			err = kho_adapter_unpreserve(&req);
		}
out_unlock:
		mutex_unlock(&adapter_lock);
		return err;

	case KHO_ADAPTER_IOC_STATUS:
		mutex_lock(&adapter_lock);
		kho_adapter_status(&st);
		mutex_unlock(&adapter_lock);

		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations kho_adapter_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = kho_adapter_ioctl,
	.compat_ioctl = kho_adapter_ioctl,
};

static struct miscdevice kho_adapter_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "kho_adapter",
	.fops = &kho_adapter_fops,
	.mode = 0600,
};

static int __init kho_adapter_init(void)
{
	return misc_register(&kho_adapter_misc);
}

static void __exit kho_adapter_exit(void)
{
	struct kho_adapter_ctx *ctx, *tmp;
	unsigned long idx;
	void *entry;

	misc_deregister(&kho_adapter_misc);

	mutex_lock(&adapter_lock);

	list_for_each_entry_safe(ctx, tmp, &outgoing_list, node) {
		list_del(&ctx->node);
		kho_adapter_free_ctx(ctx);
	}

	list_for_each_entry_safe(ctx, tmp, &incoming_list, node) {
		list_del(&ctx->node);
		kho_adapter_free_ctx(ctx);
	}

	xa_for_each(&restored_pages, idx, entry)
		folio_put(page_folio((struct page *)entry));
	xa_destroy(&restored_pages);
	xa_destroy(&outgoing_pages);

	mutex_unlock(&adapter_lock);
}

module_init(kho_adapter_init);
module_exit(kho_adapter_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KHO adaptation layer for CRIU anonymous memory");

