/*
 *  include/asm-s390/pgalloc.h
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Hartmut Penner (hp@de.ibm.com)
 *               Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 *  Derived from "include/asm-i386/pgalloc.h"
 *    Copyright (C) 1994  Linus Torvalds
 */

#ifndef _S390_PGALLOC_H
#define _S390_PGALLOC_H

#include <linux/threads.h>
#include <linux/gfp.h>
#include <linux/mm.h>

#define arch_add_exec_range(mm, limit) do { ; } while (0)
#define arch_flush_exec_range(mm)      do { ; } while (0)
#define arch_remove_exec_range(mm, limit) do { ; } while (0)

#define check_pgt_cache()	do {} while (0)

extern void diag10(unsigned long addr);

/*
 * Page allocation orders.
 */
#ifndef __s390x__
# define PGD_ALLOC_ORDER	1
#else /* __s390x__ */
# define PMD_ALLOC_ORDER	2
# define PGD_ALLOC_ORDER	2
#endif /* __s390x__ */

/*
 * Allocate and free page tables. The xxx_kernel() versions are
 * used to allocate a kernel page table - this turns on ASN bits
 * if any.
 */

static inline pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd = (pgd_t *) __get_free_pages(GFP_KERNEL, PGD_ALLOC_ORDER);
	int i;

	if (!pgd)
		return NULL;
	for (i = 0; i < PTRS_PER_PGD; i++)
#ifndef __s390x__
		pmd_clear(pmd_offset(pgd + i, i*PGDIR_SIZE));
#else
		pgd_clear(pgd + i);
#endif
	return pgd;
}

static inline void pgd_free(pgd_t *pgd)
{
	free_pages((unsigned long) pgd, PGD_ALLOC_ORDER);
}

#ifndef __s390x__
/*
 * page middle directory allocation/free routines.
 * We use pmd cache only on s390x, so these are dummy routines. This
 * code never triggers because the pgd will always be present.
 */
#define pmd_alloc_one(mm,address)       ({ BUG(); ((pmd_t *)2); })
#define pmd_free(x)                     do { } while (0)
#define __pmd_free_tlb(tlb,x)		do { } while (0)
#define pgd_populate(mm, pmd, pte)      BUG()
#else /* __s390x__ */
static inline pmd_t * pmd_alloc_one(struct mm_struct *mm, unsigned long vmaddr)
{
	pmd_t *pmd = (pmd_t *) __get_free_pages(GFP_KERNEL, PMD_ALLOC_ORDER);
	int i;

	if (!pmd)
		return NULL;
	for (i=0; i < PTRS_PER_PMD; i++)
		pmd_clear(pmd + i);
	return pmd;
}

static inline void pmd_free (pmd_t *pmd)
{
	free_pages((unsigned long) pmd, PMD_ALLOC_ORDER);
}

#define __pmd_free_tlb(tlb,pmd)			\
	do {					\
		tlb_flush_mmu(tlb, 0, 0);	\
		pmd_free(pmd);			\
	 } while (0)

static inline void pgd_populate(struct mm_struct *mm, pgd_t *pgd, pmd_t *pmd)
{
	pgd_val(*pgd) = _PGD_ENTRY | __pa(pmd);
}

#endif /* __s390x__ */

static inline void 
pmd_populate_kernel(struct mm_struct *mm, pmd_t *pmd, pte_t *pte)
{
#ifndef __s390x__
	pmd_val(pmd[0]) = _PAGE_TABLE + __pa(pte);
	pmd_val(pmd[1]) = _PAGE_TABLE + __pa(pte+256);
	pmd_val(pmd[2]) = _PAGE_TABLE + __pa(pte+512);
	pmd_val(pmd[3]) = _PAGE_TABLE + __pa(pte+768);
#else /* __s390x__ */
	pmd_val(*pmd) = _PMD_ENTRY + __pa(pte);
	pmd_val1(*pmd) = _PMD_ENTRY + __pa(pte+256);
#endif /* __s390x__ */
}

static inline void
pmd_populate(struct mm_struct *mm, pmd_t *pmd, struct page *page)
{
	pmd_populate_kernel(mm, pmd, (pte_t *)((page-mem_map) << PAGE_SHIFT));
}

/*
 * page table entry allocation/free routines.
 */
static inline pte_t *
pte_alloc_one_kernel(struct mm_struct *mm, unsigned long vmaddr)
{
	pte_t *pte = (pte_t *) __get_free_page(GFP_KERNEL|__GFP_REPEAT);
	int i;

	if (!pte)
		return NULL;
	for (i=0; i < PTRS_PER_PTE; i++) {
		pte_clear(mm, vmaddr, pte + i);
		vmaddr += PAGE_SIZE;
	}
	return pte;
}

static inline struct page *
pte_alloc_one(struct mm_struct *mm, unsigned long vmaddr)
{
	pte_t *pte = pte_alloc_one_kernel(mm, vmaddr);
	if (pte)
		return virt_to_page(pte);
	return NULL;
}

static inline void pte_free_kernel(pte_t *pte)
{
        free_page((unsigned long) pte);
}

static inline void pte_free(struct page *pte)
{
        __free_page(pte);
}

#define __pte_free_tlb(tlb,pte) tlb_remove_page(tlb,pte)

#endif /* _S390_PGALLOC_H */
