#include "hook.h"
#include "debug.h"
#include "linux/gfp.h"
#include "linux/list.h"
#include "linux/mm.h"
#include "linux/mmap_lock.h"
#include "linux/types.h"
#include "linux/pagewalk.h"
#include "task_data.h"
#include "snapshot.h"
#include "vdso/limits.h"

static DEFINE_PER_CPU(struct task_struct *, last_task);
static DEFINE_PER_CPU(struct task_data *, last_task_data);

static struct task_data *get_task_data_with_cache(struct task_struct *task)
{
	struct task_struct **cached_task = &get_cpu_var(last_task);
	struct task_data **cached_data = &get_cpu_var(last_task_data);

	struct task_data *data = NULL;

	if (*cached_task == task) {
		data = *cached_data;
	} else {
		data = get_task_data(task);

		*cached_task = task;
		*cached_data = data;
	}

	put_cpu_var(last_task);
	put_cpu_var(last_task_data);

	return data;
}

static void invalidate_task_data_cache(const struct task_struct *task)
{
	struct task_struct **cached_task;
	int i;

	for_each_possible_cpu (i) {
		cached_task = &per_cpu(last_task, i);
		if (*cached_task == task) {
			*cached_task = NULL;
			per_cpu(last_task_data, i) = NULL;
		}
	}
}

static pte_t *walk_page_table(unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep = NULL;

	struct mm_struct *mm = current->mm;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) {
		// DBG_PRINT("Invalid pgd.");
		goto out;
	}

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) {
		// DBG_PRINT("Invalid p4d.");
		goto out;
	}

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud)) {
		// DBG_PRINT("Invalid pud.");
		goto out;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd)) {
		// DBG_PRINT("Invalid pmd.");
		goto out;
	}

	ptep = pte_offset_map(pmd, addr);
	if (!ptep) {
		// DBG_PRINT("[NEW] Invalid pte.");
		goto out;
	}

out:
	return ptep;
}

// TODO lock thee lists

void exclude_vmrange(unsigned long start, unsigned long end)
{
	struct task_data *data = ensure_task_data(current);
	struct vmrange *n;

	n = kmalloc(sizeof(struct vmrange), GFP_KERNEL);
	if (!n) {
		FATAL("vmrange_node allocation failed");
		return;
	}

	n->start = start;
	n->end = end;
	INIT_LIST_HEAD(&n->node);

	list_add(&data->blocklist, &n->node);
}

void include_vmrange(unsigned long start, unsigned long end)
{
	struct task_data *data = ensure_task_data(current);
	struct vmrange *n;

	n = kmalloc(sizeof(struct vmrange), GFP_KERNEL);
	if (!n) {
		FATAL("vmrange_node allocation failed");
		return;
	}

	n->start = start;
	n->end = end;
	INIT_LIST_HEAD(&n->node);

	list_add(&data->allowlist, &n->node);
}

static struct vmrange *intersect_blocklist(struct task_data *data,
					   unsigned long start,
					   unsigned long end)
{
	struct vmrange *n = NULL;

	list_for_each_entry (n, &data->blocklist, node) {
		if (end > n->start && start < n->end)
			return n;
	}

	return NULL;
}

static struct vmrange *intersect_allowlist(struct task_data *data,
					   unsigned long start,
					   unsigned long end)
{
	struct vmrange *n = NULL;

	list_for_each_entry (n, &data->allowlist, node) {
		if (end > n->start && start < n->end)
			return n;
	}

	return NULL;
}

static struct snapshot_vma *add_snapshot_vma(struct task_data *data,
					     struct vm_area_struct *vma)
{
	struct snapshot_vma *ss_vma;

	DBG_PRINT("adding snapshot_vma, start: 0x%016lx end: 0x%016lx\n",
		  vma->vm_start, vma->vm_end);

	ss_vma = kmalloc(sizeof(struct snapshot_vma), GFP_KERNEL);
	if (!ss_vma) {
		FATAL("snapshot_vma allocation failed!");
		return NULL;
	}

	ss_vma->vm_start = vma->vm_start;
	ss_vma->vm_end = vma->vm_end;
	ss_vma->is_anonymous_private =
		vma_is_anonymous(vma) & !(vma->vm_flags & VM_SHARED);
	if (ss_vma->is_anonymous_private) {
		DBG_PRINT("anonymous private mapping: 0x%016lx", vma->vm_start);
	}

	ss_vma->prot |= vma->vm_flags | VM_READ ? PROT_READ : 0;
	ss_vma->prot |= vma->vm_flags | VM_WRITE ? PROT_WRITE : 0;
	ss_vma->prot |= vma->vm_flags | VM_EXEC ? PROT_EXEC : 0;

	INIT_LIST_HEAD(&ss_vma->all_vmas_node);
	INIT_LIST_HEAD(&ss_vma->snapshotted_vmas_node);

	list_add_tail(&ss_vma->all_vmas_node, &data->ss.all_vmas);

	return ss_vma;
}

#ifdef DEBUG
void dump_memory_snapshot(struct task_data *data)
{
	struct snapshot_page *sp;
	int i;

	if (!data)
		return;

	DBG_PRINT("dumping dirty pages from task_data %p:", data);
	hash_for_each (data->ss.ss_pages, i, sp, next) {
		if (sp->dirty)
			DBG_PRINT("  %d: 0x%016lx\n", i, sp->page_base);
	}

	DBG_PRINT("dumping pages in dirty list:\n");
	list_for_each_entry (sp, &data->ss.dirty_pages, dirty_list) {
		DBG_PRINT("  0x%016lx\n", sp->page_base);
	}
}
#endif

static bool is_snapshotted_address(struct task_data *data,
				   unsigned long page_base)
{
	struct snapshot_vma *ss_vma;

	list_for_each_entry (ss_vma, &data->ss.snapshotted_vmas,
			     snapshotted_vmas_node) {
		if (ss_vma->vm_start <= page_base &&
		    page_base < ss_vma->vm_end) {
			return true;
		}

		if (ss_vma->vm_start > page_base) {
			break;
		}
	}

	return false;
}

static struct snapshot_page *get_snapshot_page(struct task_data *data,
					       unsigned long page_base)
{
	struct snapshot_page *sp;

	hash_for_each_possible (data->ss.ss_pages, sp, next, page_base) {
		if (sp->page_base == page_base)
			return sp;
	}

	return NULL;
}

static struct snapshot_page *add_snapshot_page(struct task_data *data,
					       unsigned long page_base,
					       bool attempt_reuse)
{
	struct snapshot_page *sp = NULL;

	if (attempt_reuse)
		sp = get_snapshot_page(data, page_base);
	if (sp == NULL) {
		sp = kmalloc(sizeof(struct snapshot_page), GFP_ATOMIC);
		if (!sp) {
			FATAL("could not allocate snapshot_page");
			return NULL;
		}

		sp->page_base = page_base;
		sp->page_data = NULL;
		hash_add(data->ss.ss_pages, &sp->next, sp->page_base);
		INIT_LIST_HEAD(&sp->dirty_list);
	}

	sp->page_prot = 0;
	sp->has_been_copied = false;
	sp->dirty = false;
	sp->in_dirty_list = false;

	return sp;
}

static int make_snapshot_page(struct task_data *data, struct mm_struct *mm,
			      unsigned long addr, pte_t *pte)
{
	struct snapshot_page *sp;
	struct page *page;

	page = pte_page(*pte);
	DBG_PRINT(
		"making snapshot: 0x%08lx PTE: 0x%08lx Page: 0x%08lx PageAnon: %d\n",
		addr, pte->pte, (unsigned long)page, page ? PageAnon(page) : 0);

	sp = add_snapshot_page(data, addr, true);
	if (!sp)
		return -ENOMEM;

	if (pte_none(*pte)) {
		/* empty pte */
		sp->has_had_pte = false;
		set_snapshot_page_none_pte(sp);

	} else {
		sp->has_had_pte = true;
		if (pte_write(*pte)) {
			/* Private rw page */
			DBG_PRINT("private writable addr: 0x%08lx\n", addr);
			ptep_set_wrprotect(mm, addr, pte);
			set_snapshot_page_private(sp);

			/* flush tlb to make the pte change effective */
			k_flush_tlb_mm_range(mm, addr & PAGE_MASK,
					     (addr & PAGE_MASK) + PAGE_SIZE,
					     PAGE_SHIFT, false);
			DBG_PRINT("writable now: %d\n", pte_write(*pte));

		} else {
			/* COW ro page */
			DBG_PRINT("cow writable addr: 0x%08lx\n", addr);
			set_snapshot_page_cow(sp);
		}
	}

	return 0;
}

struct snapshot_walk_data {
	struct task_data *task_data;
	unsigned long next_allowed_address;
	unsigned long next_blocked_address;
};

static int snapshot_walk_check_range(unsigned long addr, unsigned long next,
				     struct mm_walk *walk)
{
	struct snapshot_walk_data *walk_data =
		(struct snapshot_walk_data *)walk->private;
	int config = walk_data->task_data->config;
	struct vmrange *blocked_overlapped_range = NULL;
	struct vmrange *allowed_overlapped_range = NULL;

	// Fast path for blocked addresses
	if (next < walk_data->next_blocked_address)
		return ACTION_CONTINUE;

	// Fast path for allowed addresses
	if (next < walk_data->next_allowed_address)
		return ACTION_SUBTREE;

	blocked_overlapped_range =
		intersect_blocklist(walk_data->task_data, addr, next);
	if (blocked_overlapped_range) {
		// Range is entirely blocked
		if (blocked_overlapped_range->start <= addr &&
		    blocked_overlapped_range->end >= next) {
			walk_data->next_blocked_address =
				blocked_overlapped_range->end;
			return ACTION_CONTINUE;
		}
	}

	allowed_overlapped_range =
		intersect_allowlist(walk_data->task_data, addr, next);
	if (allowed_overlapped_range) {
		// Range is entirely allowed
		if (allowed_overlapped_range->start <= addr &&
		    allowed_overlapped_range->end >= next) {
			walk_data->next_allowed_address =
				allowed_overlapped_range->end;
		}

		// If the allowlist is intersected, even partially, we need to
		// explore the subtree.
		return ACTION_SUBTREE;
	}

	// Skip all non whitelisted mappings if BLOCK is specified.
	if (config & AFL_SNAPSHOT_BLOCK) {
		// The whole interval does not intersect the allowlist.
		walk_data->next_blocked_address = next;
		return ACTION_CONTINUE;
	}

	// The whole interval does not intersect the blocklist.
	if (!blocked_overlapped_range)
		walk_data->next_allowed_address = next;

	return ACTION_SUBTREE;
}

static int snapshot_pgd_entry(pgd_t *pgd, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	// Still called because it updates `next_*_address`.
	snapshot_walk_check_range(addr, next, walk);
	return 0;
}

static int snapshot_p4d_entry(p4d_t *p4d, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	// Still called because it updates `next_*_address`.
	snapshot_walk_check_range(addr, next, walk);
	return 0;
}

static int snapshot_pud_entry(pud_t *pud, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	walk->action = snapshot_walk_check_range(addr, next, walk);
	return 0;
}

static int snapshot_pmd_entry(pmd_t *pmd, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	walk->action = snapshot_walk_check_range(addr, next, walk);
	return 0;
}

static int snapshot_pte_entry(pte_t *pte, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	struct snapshot_walk_data *walk_data =
		(struct snapshot_walk_data *)walk->private;

	if (snapshot_walk_check_range(addr, next, walk) == ACTION_CONTINUE)
		return 0;

	return make_snapshot_page(walk_data->task_data, walk->mm, addr, pte);
}

static const struct mm_walk_ops snapshot_walk_ops = {
	.pgd_entry = snapshot_pgd_entry,
	.p4d_entry = snapshot_p4d_entry,
	.pud_entry = snapshot_pud_entry,
	.pmd_entry = snapshot_pmd_entry,
	.pte_entry = snapshot_pte_entry,
};

// TODO: This seems broken?
// If I have a page that is right below the page of the stack, then it will count as a stack page.
inline bool is_stack(struct vm_area_struct *vma)
{
	return vma->vm_start <= vma->vm_mm->start_stack &&
	       vma->vm_end >= vma->vm_mm->start_stack;
}

int take_memory_snapshot(struct task_data *data)
{
	struct vm_area_struct *pvma = NULL;
	struct snapshot_vma *ss_vma = NULL;
	int res = 0;

	struct snapshot_walk_data walk_data = {
		.task_data = data,
	};

#ifdef DEBUG
	struct vmrange *n = NULL;

	list_for_each_entry (n, &data->allowlist, node)
		DBG_PRINT("Allowlist: 0x%08lx - 0x%08lx\n", n->start, n->end);

	list_for_each_entry (n, &data->blocklist, node)
		DBG_PRINT("Blocklist: 0x%08lx - 0x%08lx\n", n->start, n->end);
#endif

	invalidate_task_data_cache(data->tsk);

	mmap_read_lock(current->mm);
	for (pvma = current->mm->mmap; pvma; pvma = pvma->vm_next) {
		ss_vma = add_snapshot_vma(data, pvma);
		if (!ss_vma) {
			res = -ENOMEM;
			goto unlock;
		}

		if (!intersect_allowlist(data, pvma->vm_start, pvma->vm_end)) {
			// By default, only writable pages are snapshotted.
			if (!(pvma->vm_flags & VM_WRITE))
				continue;

			// By default, shared memory pages are skipped.
			if (pvma->vm_flags & VM_SHARED)
				continue;

			// Skip all non whitelisted mappings if BLOCK is specified.
			if (data->config & AFL_SNAPSHOT_BLOCK)
				continue;

			// Skip the stack if NOSTACK is specified.
			if ((data->config & AFL_SNAPSHOT_NOSTACK) &&
			    is_stack(pvma))
				continue;
		}

		DBG_PRINT("Make snapshot start: 0x%08lx end: 0x%08lx\n",
			  pvma->vm_start, pvma->vm_end);
		list_add_tail(&ss_vma->snapshotted_vmas_node,
			      &data->ss.snapshotted_vmas);
		res = walk_page_vma(pvma, &snapshot_walk_ops, &walk_data);
		if (res)
			goto unlock;
	}

unlock:
	mmap_read_unlock(current->mm);

	return res;
}

// Taken from mm/mmap.c
// XXX: Locking is broken because vm functions relock mm.
int restore_brk(unsigned long snapshotted_brk)
{
	unsigned long current_brk, aligned_current_brk, aligned_snapshotted_brk;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *next;

	current_brk = mm->brk;

	// The snapshotted break was a valid break, so no need to check it here.

	aligned_current_brk = PAGE_ALIGN(current_brk);
	aligned_snapshotted_brk = PAGE_ALIGN(snapshotted_brk);
	if (aligned_current_brk == aligned_snapshotted_brk) {
		goto success;
	}

	if (snapshotted_brk <= current_brk) {
		int ret;

		ret = vm_munmap(snapshotted_brk, current_brk - snapshotted_brk);
		if (ret < 0) {
			FATAL("Failed to unmap new program break");
			return -1;
		}

		goto success;
	}

	mmap_read_lock(mm);
	next = find_vma(mm, current_brk);
	if (next && snapshotted_brk + PAGE_SIZE > next->vm_start) {
		mmap_read_unlock(mm);
		FATAL("Snapshotted program break overlaps with new VMA");
		return -1;
	}
	mmap_read_unlock(mm);

	if (vm_brk(current_brk, snapshotted_brk - current_brk) < 0) {
		FATAL("Could not remap snapshotted program break");
		return -1;
	}

success:
	mm->brk = snapshotted_brk;
	return 0;
}

static int restore_vmas(struct task_data *data)
{
	struct vm_area_struct *vma_iter = data->tsk->mm->mmap;
	struct vm_area_struct *next_vma_iter = NULL;
	struct snapshot_vma *ss_vma_iter = list_first_entry(
		&data->ss.all_vmas, struct snapshot_vma, all_vmas_node);

	unsigned long cursor = 0;
	unsigned long next_cursor = 0;

	unsigned long next_vma_pos = 0;
	unsigned long next_ss_vma_pos = 0;

	bool in_ss_vmas = false;
	bool in_vmas = false;

	int res = 0;

	DBG_PRINT("unmapping new vmas:\n");

	while (vma_iter || !list_entry_is_head(ss_vma_iter, &data->ss.all_vmas,
					       all_vmas_node)) {
		// Calculate next valid positions for vma lists.
		if (vma_iter) {
			// `vm_munmap` may free the `vm_area_struct`, so save `vm_next` here.
			next_vma_iter = vma_iter->vm_next;
			next_vma_pos =
				in_vmas ? vma_iter->vm_end : vma_iter->vm_start;
		} else {
			next_vma_iter = NULL;
			next_vma_pos = ULONG_MAX;
		}

		if (!list_entry_is_head(ss_vma_iter, &data->ss.all_vmas,
					all_vmas_node)) {
			next_ss_vma_pos = in_ss_vmas ? ss_vma_iter->vm_end :
							     ss_vma_iter->vm_start;
		} else {
			next_ss_vma_pos = ULONG_MAX;
		}

		next_cursor = min(next_vma_pos, next_ss_vma_pos);

		// `in_vmas` and `in_ss_vmas` hold for the interval [cursor,
		// next_cursor).
		if (next_cursor != cursor && in_vmas && !in_ss_vmas) {
			DBG_PRINT("  unmapping (0x%016lx, 0x%016lx)\n", cursor,
				  next_cursor);
			res = vm_munmap(cursor, next_cursor - cursor);
			if (res) {
				FATAL("vm_munmap failed, start: 0x%016lx, end: 0x%016lx\n",
				      cursor, next_cursor);
				return res;
			}
		} else if (next_cursor != cursor && !in_vmas && in_ss_vmas) {
			if (ss_vma_iter->is_anonymous_private) {
				unsigned long addr;

				// An anonymous private mapping can be easily restored.
				addr = vm_mmap(
					NULL, cursor, next_cursor - cursor,
					ss_vma_iter->prot,
					MAP_PRIVATE | MAP_FIXED_NOREPLACE, 0);
				if (IS_ERR((void *)addr) || addr != cursor) {
					FATAL("vm_mmap failed, start: 0x%016lx, end: 0x%016lx, res: 0x%016lx\n",
					      cursor, next_cursor, addr);
					return res;
				}
			} else {
				FATAL("missing memory, start: 0x%016lx, end: 0x%016lx\n",
				      cursor, next_cursor);
			}
		}

		if (next_cursor == next_vma_pos) {
			in_vmas = !in_vmas;
			if (!in_vmas)
				vma_iter = next_vma_iter;
		}

		if (next_cursor == next_ss_vma_pos) {
			in_ss_vmas = !in_ss_vmas;
			if (!in_ss_vmas)
				ss_vma_iter = list_next_entry(ss_vma_iter,
							      all_vmas_node);
		}

		cursor = next_cursor;
	}

	return 0;
}

static void do_recover_page(struct snapshot_page *sp)
{
	DBG_PRINT(
		"found reserved page: 0x%08lx page_base: 0x%08lx page_prot: 0x%08lx\n",
		(unsigned long)sp->page_data, (unsigned long)sp->page_base,
		sp->page_prot);
	if (copy_to_user((void __user *)sp->page_base, sp->page_data,
			 PAGE_SIZE) != 0)
		DBG_PRINT("incomplete copy_to_user\n");
	sp->dirty = false;
}

static void do_recover_none_pte(struct snapshot_page *sp)
{
	struct mm_struct *mm = current->mm;

	DBG_PRINT(
		"found none_pte refreshed page_base: 0x%08lx page_prot: 0x%08lx\n",
		sp->page_base, sp->page_prot);

	k_zap_page_range(mm->mmap, sp->page_base, PAGE_SIZE);
}

int recover_memory_snapshot(struct task_data *data)
{
	struct snapshot_page *sp;
	struct snapshot_page *n;

	struct mm_struct *mm = data->tsk->mm;
	pte_t *pte;

	int res = 0;

	if (data->config & AFL_SNAPSHOT_MMAP) {
		res = restore_vmas(data);
		if (res)
			return res;
	}

	list_for_each_entry_safe (sp, n, &data->ss.dirty_pages, dirty_list) {
		DBG_PRINT("restoring page: 0x%016lx\n", sp->page_base);

		if (sp->dirty && sp->has_been_copied) {
			// it has been captured by page fault

			do_recover_page(sp); // copy old content
			sp->has_had_pte = true;

			pte = walk_page_table(sp->page_base);
			if (!pte)
				continue;

			/* Private rw page */
			DBG_PRINT("private writable addr: 0x%08lx\n",
				  sp->page_base);
			ptep_set_wrprotect(mm, sp->page_base, pte);
			set_snapshot_page_private(sp);

			/* flush tlb to make the pte change effective */
			k_flush_tlb_mm_range(mm, sp->page_base,
					     sp->page_base + PAGE_SIZE,
					     PAGE_SHIFT, false);
			DBG_PRINT("writable now: %d\n", pte_write(*pte));

			pte_unmap(pte);

		} else if (is_snapshot_page_private(sp)) {
			// private page that has not been captured
			// still write protected

		} else if (is_snapshot_page_none_pte(sp) && sp->has_had_pte) {
			do_recover_none_pte(sp);

			set_snapshot_page_none_pte(sp);
			sp->has_had_pte = false;
		}

		if (!sp->in_dirty_list) {
			WARNF("in_dirty_list not set: 0x%016lx\n",
			      sp->page_base);
		}
		sp->in_dirty_list = false;
		list_del(&sp->dirty_list);
	}

	return 0;
}

static void clean_snapshot_vmas(struct task_data *data)
{
	struct snapshot_vma *ss_vma, *next;

	DBG_PRINT("freeing snapshot vmas:\n");

	list_for_each_entry_safe (ss_vma, next, &data->ss.all_vmas,
				  all_vmas_node) {
		DBG_PRINT("  start: 0x%08lx end: 0x%08lx\n", ss_vma->vm_start,
			  ss_vma->vm_end);
		list_del(&ss_vma->all_vmas_node);
		list_del(&ss_vma->snapshotted_vmas_node);
		kfree(ss_vma);
	}
}

void clean_memory_snapshot(struct task_data *data)
{
	struct snapshot_page *sp;
	struct hlist_node *tmp;
	int i;

	invalidate_task_data_cache(data->tsk);

	clean_snapshot_vmas(data);

	hash_for_each_safe (data->ss.ss_pages, i, tmp, sp, next) {
		kfree(sp->page_data);
		hash_del(&sp->next);
		kfree(sp);
	}
}

struct snapshot_page *record_dirty_page(struct task_data *data,
					struct mm_struct *mm,
					unsigned long page_addr, pte_t pte)
{
	struct snapshot_page *ss_page = NULL;

	DBG_PRINT("%s: searching snapshot_page for 0x%016lx in task_data: %p\n",
		  __func__, page_addr, data);
	ss_page = get_snapshot_page(data, page_addr);
	if (!ss_page)
		return NULL;

	if (ss_page->dirty || is_snapshot_page_none_pte(ss_page))
		return NULL;
	ss_page->dirty = true;

	DBG_PRINT("adding page to dirty list: 0x%016lx\n", page_addr);
	if (ss_page->in_dirty_list) {
		WARNF("page (0x%016lx) already in dirty list (dirty: %d, copied: %d)\n",
		      ss_page->page_base, ss_page->dirty,
		      ss_page->has_been_copied);
	} else {
		ss_page->in_dirty_list = true;
		list_add_tail(&ss_page->dirty_list, &data->ss.dirty_pages);
	}

	/* copy the page if necessary.
	 * the page becomes COW page again. we do not need to take care of it.
	 */
	if (!ss_page->has_been_copied) {
		struct page *original_page = NULL;
		void *mapped_page_addr = NULL;

		DBG_PRINT("copying page 0x%016lx\n", page_addr);

		/* reserved old page data */
		if (!ss_page->page_data) {
			ss_page->page_data = kmalloc(PAGE_SIZE, GFP_ATOMIC);
			if (!ss_page->page_data) {
				FATAL("could not allocate memory for page_data");
				return NULL;
			}
		}

		original_page = pfn_to_page(pte_pfn(pte));
		mapped_page_addr = kmap_local_page(original_page);
		memcpy(ss_page->page_data, mapped_page_addr, PAGE_SIZE);
		kunmap_local(mapped_page_addr);

		ss_page->has_been_copied = true;
	}

	return ss_page;
}

static vm_fault_t do_wp_page_stub(struct vm_fault *vmf)
{
	return 0;
}

void do_wp_page_hook(unsigned long ip, unsigned long parent_ip,
		     struct ftrace_ops *op, ftrace_regs_ptr regs)
{
	struct pt_regs *pregs = ftrace_get_regs(regs);
	struct vm_fault *fault =
		(struct vm_fault *)regs_get_kernel_argument(pregs, 0);
	struct mm_struct *mm = fault->vma->vm_mm;
	unsigned long page_base_addr = fault->address & PAGE_MASK;

	struct task_data *data = NULL;
	struct snapshot_page *ss_page = NULL;

	pte_t entry;

	data = get_task_data_with_cache(rcu_access_pointer(mm->owner));
	if (!data || !have_snapshot(data))
		return;

	ss_page = record_dirty_page(data, mm, page_base_addr, fault->orig_pte);
	if (!ss_page)
		return;

	/* if this was originally a COW page, let the original page fault handler
	 * handle it.
	 */
	if (!is_snapshot_page_private(ss_page))
		return;

	DBG_PRINT(
		"handling page fault! process: %s addr: 0x%08lx ptep: 0x%08lx pte: 0x%08lx\n",
		current->comm, fault->address, (unsigned long)fault->pte,
		fault->orig_pte.pte);

	/* change the page prot back to ro from rw */
	entry = pte_mkwrite(fault->orig_pte);
	set_pte_at(mm, fault->address, fault->pte, entry);

	k_flush_tlb_mm_range(mm, page_base_addr, page_base_addr + PAGE_SIZE,
			     PAGE_SHIFT, false);

	pte_unmap_unlock(fault->pte, fault->ptl);

	// skip original function
	pregs->ip = (unsigned long)&do_wp_page_stub;
}

// actually hooking page_add_new_anon_rmap, but we really only care about calls
// from do_anonymous_page
void page_add_new_anon_rmap_hook(unsigned long ip, unsigned long parent_ip,
				 struct ftrace_ops *op, ftrace_regs_ptr regs)
{
	struct pt_regs *pregs = ftrace_get_regs(regs);
	struct vm_area_struct *vma;
	unsigned long address;

	struct mm_struct *mm;
	struct task_data *data = NULL;
	struct snapshot_page *ss_page = NULL;
	unsigned long page_base_addr;

	vma = (struct vm_area_struct *)regs_get_kernel_argument(pregs, 1);
	mm = vma->vm_mm;

	address = regs_get_kernel_argument(pregs, 2);
	page_base_addr = address & PAGE_MASK;

	// XXX: mm->owner is probably the group leader, not necessarily the
	// thread that triggered the page fault.
	data = get_task_data_with_cache(rcu_access_pointer(mm->owner));
	if (!data || !have_snapshot(data))
		return;

	DBG_PRINT("%s: searching snapshot_page for 0x%016lx in task_data: %p\n",
		  __func__, page_base_addr, data);
	ss_page = get_snapshot_page(data, page_base_addr);
	if (!ss_page) {
		if (!is_snapshotted_address(data, page_base_addr))
			return;

		// Allocate entries for pages that did not have a PTE on demand.
		DBG_PRINT("adding page without PTE to snapshot: 0x%08lx\n",
			  page_base_addr);
		ss_page = add_snapshot_page(data, page_base_addr, false);
		set_snapshot_page_none_pte(ss_page);
	}

	DBG_PRINT("do_anonymous_page 0x%08lx\n", address);
	// dump_stack();

	// HAVE PTE NOW
	ss_page->has_had_pte = true;
	if (is_snapshot_page_none_pte(ss_page)) {
		if (ss_page->in_dirty_list) {
			WARNF("0x%016lx: Adding page to dirty list, but it's already there??? (dirty: %d, copied: %d)\n",
			      ss_page->page_base, ss_page->dirty,
			      ss_page->has_been_copied);
		} else {
			ss_page->in_dirty_list = true;
			list_add_tail(&ss_page->dirty_list,
				      &data->ss.dirty_pages);
		}
	}
}

static int munmap_pte_entry(pte_t *pte, unsigned long addr, unsigned long next,
			    struct mm_walk *walk)
{
	struct task_data *data = (struct task_data *)walk->private;
	record_dirty_page(data, walk->mm, addr, *pte);
	return 0;
}

static const struct mm_walk_ops munmap_walk_ops = {
	.pte_entry = munmap_pte_entry,
};

void __do_munmap_hook(unsigned long ip, unsigned long parent_ip,
		      struct ftrace_ops *op, ftrace_regs_ptr regs)
{
	struct pt_regs *pregs = ftrace_get_regs(regs);
	struct mm_struct *mm =
		(struct mm_struct *)regs_get_kernel_argument(pregs, 0);
	unsigned long start = regs_get_kernel_argument(pregs, 1);
	size_t len = regs_get_kernel_argument(pregs, 2);
	unsigned long end = start + len;

	struct task_data *data = NULL;

	data = get_task_data_with_cache(rcu_access_pointer(mm->owner));
	if (!data || !have_snapshot(data))
		return;

	DBG_PRINT("%s: saving unmapped memory from 0x%08lx to 0x%08lx",
		  __func__, start, end);

	// __do_munmap is always called while holding a lock on mm, so no need to lock
	// to perform the page walk here.
	if (walk_page_range(mm, start, end, &munmap_walk_ops, data) < 0) {
		FATAL("could not walk page table for munmap");
	}
}

// void finish_fault_hook(unsigned long ip, unsigned long parent_ip,
//                    struct ftrace_ops *op, ftrace_regs_ptr regs)
// {
//   struct pt_regs* pregs = ftrace_get_regs(regs);
//   struct vm_fault *vmf = (struct vm_fault*)pregs->di;
//   struct vm_area_struct *vma;
//   struct mm_struct *     mm;
//   struct task_data *     data;
//   struct snapshot_page * ss_page;
//   unsigned long          address;

//   vma = vmf->vma;
//   address = vmf->address;

//   struct task_struct* ltask = get_cpu_var(last_task);
//   if (ltask == mm->owner) { // XXX: mm is not initialized!

//     // fast path
//     data = get_cpu_var(last_data);
//     put_cpu_var(last_task);
//     put_cpu_var(last_data);
//   } else {

//     // query the radix tree
//     data = get_task_data(mm->owner);
//     get_cpu_var(last_task) = mm->owner;
//     get_cpu_var(last_data) = data;
//     put_cpu_var(last_task);
//     put_cpu_var(last_task);
//     put_cpu_var(last_data);

//   }

//   if (data && have_snapshot(data)) {

//     ss_page = get_snapshot_page(data, address & PAGE_MASK);

//   } else {

//     return;

//   }

//   if (!ss_page) {

//     /* not a snapshot'ed page */
//     return;

//   }

//   DBG_PRINT("finish_fault 0x%08lx", address);
//   dump_stack();

//   // HAVE PTE NOW
//   ss_page->has_had_pte = true;
//   if (is_snapshot_page_none_pte(ss_page)) {
//     if (ss_page->in_dirty_list) {
//       WARNF("0x%016lx: Adding page to dirty list, but it's already there???", ss_page->page_base);
//     } else {
//       ss_page->in_dirty_list = true;
//       list_add_tail(&ss_page->dirty_list, &data->ss.dirty_pages);
//     }
//   }

//   return;
// }
