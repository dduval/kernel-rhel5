/*
 *  include/asm-s390/pgtable.h
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Hartmut Penner (hp@de.ibm.com)
 *               Ulrich Weigand (weigand@de.ibm.com)
 *               Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 *  Derived from "include/asm-i386/pgtable.h"
 */

#ifndef _ASM_S390_PGTABLE_H
#define _ASM_S390_PGTABLE_H

#include <asm-generic/4level-fixup.h>

/*
 * The Linux memory management assumes a three-level page table setup. For
 * s390 31 bit we "fold" the mid level into the top-level page table, so
 * that we physically have the same two-level page table as the s390 mmu
 * expects in 31 bit mode. For s390 64 bit we use three of the five levels
 * the hardware provides (region first and region second tables are not
 * used).
 *
 * The "pgd_xxx()" functions are trivial for a folded two-level
 * setup: the pgd is never bad, and a pmd always exists (as it's folded
 * into the pgd entry)
 *
 * This file contains the functions and defines necessary to modify and use
 * the S390 page table tree.
 */
#ifndef __ASSEMBLY__
#include <asm/bug.h>
#include <asm/processor.h>
#include <linux/threads.h>

struct vm_area_struct; /* forward declaration (include/linux/mm.h) */
struct mm_struct;

extern pgd_t swapper_pg_dir[] __attribute__ ((aligned (4096)));
extern void paging_init(void);

/*
 * The S390 doesn't have any external MMU info: the kernel page
 * tables contain all the necessary information.
 */
#define update_mmu_cache(vma, address, pte)     do { } while (0)

/*
 * ZERO_PAGE is a global shared page that is always zero: used
 * for zero-mapped memory areas etc..
 */
extern char empty_zero_page[PAGE_SIZE];
#define ZERO_PAGE(vaddr) (virt_to_page(empty_zero_page))
#endif /* !__ASSEMBLY__ */

/*
 * PMD_SHIFT determines the size of the area a second-level page
 * table can map
 * PGDIR_SHIFT determines what a third-level page table entry can map
 */
#ifndef __s390x__
# define PMD_SHIFT	22
# define PGDIR_SHIFT	22
#else /* __s390x__ */
# define PMD_SHIFT	21
# define PGDIR_SHIFT	31
#endif /* __s390x__ */

#define PMD_SIZE        (1UL << PMD_SHIFT)
#define PMD_MASK        (~(PMD_SIZE-1))
#define PGDIR_SIZE      (1UL << PGDIR_SHIFT)
#define PGDIR_MASK      (~(PGDIR_SIZE-1))

/*
 * entries per page directory level: the S390 is two-level, so
 * we don't really have any PMD directory physically.
 * for S390 segment-table entries are combined to one PGD
 * that leads to 1024 pte per pgd
 */
#ifndef __s390x__
# define PTRS_PER_PTE    1024
# define PTRS_PER_PMD    1
# define PTRS_PER_PGD    512
#else /* __s390x__ */
# define PTRS_PER_PTE    512
# define PTRS_PER_PMD    1024
# define PTRS_PER_PGD    2048
#endif /* __s390x__ */

#define FIRST_USER_ADDRESS  0

#define pte_ERROR(e) \
	printk("%s:%d: bad pte %p.\n", __FILE__, __LINE__, (void *) pte_val(e))
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %p.\n", __FILE__, __LINE__, (void *) pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %p.\n", __FILE__, __LINE__, (void *) pgd_val(e))

#ifndef __ASSEMBLY__
/*
 * Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */
#define VMALLOC_OFFSET  (8*1024*1024)
#define VMALLOC_START   (((unsigned long) high_memory + VMALLOC_OFFSET) \
			 & ~(VMALLOC_OFFSET-1))

/*
 * We need some free virtual space to be able to do vmalloc.
 * VMALLOC_MIN_SIZE defines the minimum size of the vmalloc
 * area. On a machine with 2GB memory we make sure that we
 * have at least 128MB free space for vmalloc. On a machine
 * with 4TB we make sure we have at least 1GB.
 */
#ifndef __s390x__
#define VMALLOC_MIN_SIZE	0x8000000UL
#define VMALLOC_END		0x80000000UL
#else /* __s390x__ */
#define VMALLOC_MIN_SIZE	0x40000000UL
#define VMALLOC_END		0x40000000000UL
#endif /* __s390x__ */

/*
 * A 31 bit pagetable entry of S390 has following format:
 *  |   PFRA          |    |  OS  |
 * 0                   0IP0
 * 00000000001111111111222222222233
 * 01234567890123456789012345678901
 *
 * I Page-Invalid Bit:    Page is not available for address-translation
 * P Page-Protection Bit: Store access not possible for page
 *
 * A 31 bit segmenttable entry of S390 has following format:
 *  |   P-table origin      |  |PTL
 * 0                         IC
 * 00000000001111111111222222222233
 * 01234567890123456789012345678901
 *
 * I Segment-Invalid Bit:    Segment is not available for address-translation
 * C Common-Segment Bit:     Segment is not private (PoP 3-30)
 * PTL Page-Table-Length:    Page-table length (PTL+1*16 entries -> up to 256)
 *
 * The 31 bit segmenttable origin of S390 has following format:
 *
 *  |S-table origin   |     | STL |
 * X                   **GPS
 * 00000000001111111111222222222233
 * 01234567890123456789012345678901
 *
 * X Space-Switch event:
 * G Segment-Invalid Bit:     *
 * P Private-Space Bit:       Segment is not private (PoP 3-30)
 * S Storage-Alteration:
 * STL Segment-Table-Length:  Segment-table length (STL+1*16 entries -> up to 2048)
 *
 * A 64 bit pagetable entry of S390 has following format:
 * |                     PFRA                         |0IP0|  OS  |
 * 0000000000111111111122222222223333333333444444444455555555556666
 * 0123456789012345678901234567890123456789012345678901234567890123
 *
 * I Page-Invalid Bit:    Page is not available for address-translation
 * P Page-Protection Bit: Store access not possible for page
 *
 * A 64 bit segmenttable entry of S390 has following format:
 * |        P-table origin                              |      TT
 * 0000000000111111111122222222223333333333444444444455555555556666
 * 0123456789012345678901234567890123456789012345678901234567890123
 *
 * I Segment-Invalid Bit:    Segment is not available for address-translation
 * C Common-Segment Bit:     Segment is not private (PoP 3-30)
 * P Page-Protection Bit: Store access not possible for page
 * TT Type 00
 *
 * A 64 bit region table entry of S390 has following format:
 * |        S-table origin                             |   TF  TTTL
 * 0000000000111111111122222222223333333333444444444455555555556666
 * 0123456789012345678901234567890123456789012345678901234567890123
 *
 * I Segment-Invalid Bit:    Segment is not available for address-translation
 * TT Type 01
 * TF
 * TL Table lenght
 *
 * The 64 bit regiontable origin of S390 has following format:
 * |      region table origon                          |       DTTL
 * 0000000000111111111122222222223333333333444444444455555555556666
 * 0123456789012345678901234567890123456789012345678901234567890123
 *
 * X Space-Switch event:
 * G Segment-Invalid Bit:  
 * P Private-Space Bit:    
 * S Storage-Alteration:
 * R Real space
 * TL Table-Length:
 *
 * A storage key has the following format:
 * | ACC |F|R|C|0|
 *  0   3 4 5 6 7
 * ACC: access key
 * F  : fetch protection bit
 * R  : referenced bit
 * C  : changed bit
 */

/* Hardware bits in the page table entry */
#define _PAGE_RO	0x200		/* HW read-only bit  */
#define _PAGE_INVALID	0x400		/* HW invalid bit    */
#define _PAGE_SWT	0x001		/* SW pte type bit t */
#define _PAGE_SWX	0x002		/* SW pte type bit x */

/* Six different types of pages. */
#define _PAGE_TYPE_EMPTY	0x400
#define _PAGE_TYPE_NONE		0x401
#define _PAGE_TYPE_SWAP		0x403
#define _PAGE_TYPE_FILE		0x601	/* bit 0x002 is used for offset !! */
#define _PAGE_TYPE_RO		0x200
#define _PAGE_TYPE_RW		0x000

/*
 * Only four types for huge pages, using the invalid bit and protection bit
 * of a segment table entry.
 */
#define _HPAGE_TYPE_EMPTY	0x020	/* _SEGMENT_ENTRY_INV */
#define _HPAGE_TYPE_NONE	0x220
#define _HPAGE_TYPE_RO		0x200	/* _SEGMENT_ENTRY_RO  */
#define _HPAGE_TYPE_RW		0x000

/*
 * PTE type bits are rather complicated. handle_pte_fault uses pte_present,
 * pte_none and pte_file to find out the pte type WITHOUT holding the page
 * table lock. ptep_clear_flush on the other hand uses ptep_clear_flush to
 * invalidate a given pte. ipte sets the hw invalid bit and clears all tlbs
 * for the page. The page table entry is set to _PAGE_TYPE_EMPTY afterwards.
 * This change is done while holding the lock, but the intermediate step
 * of a previously valid pte with the hw invalid bit set can be observed by
 * handle_pte_fault. That makes it necessary that all valid pte types with
 * the hw invalid bit set must be distinguishable from the four pte types
 * empty, none, swap and file.
 *
 *			irxt  ipte  irxt
 * _PAGE_TYPE_EMPTY	1000   ->   1000
 * _PAGE_TYPE_NONE	1001   ->   1001
 * _PAGE_TYPE_SWAP	1011   ->   1011
 * _PAGE_TYPE_FILE	11?1   ->   11?1
 * _PAGE_TYPE_RO	0100   ->   1100
 * _PAGE_TYPE_RW	0000   ->   1000
 *
 * pte_none is true for bits combinations 1000, 1100
 * pte_present is true for bits combinations 0000, 0010, 0100, 0110, 1001
 * pte_file is true for bits combinations 1101, 1111
 * swap pte is 1011 and 0001, 0011, 0101, 0111, 1010 and 1110 are invalid.
 */

#ifndef __s390x__

/* Bits in the segment table entry */
#define _PAGE_TABLE_LEN 0xf            /* only full page-tables            */
#define _PAGE_TABLE_COM 0x10           /* common page-table                */
#define _PAGE_TABLE_INV 0x20           /* invalid page-table               */
#define _SEG_PRESENT    0x001          /* Software (overlap with PTL)      */

/* Bits int the storage key */
#define _PAGE_CHANGED    0x02          /* HW changed bit                   */
#define _PAGE_REFERENCED 0x04          /* HW referenced bit                */

#define _USER_SEG_TABLE_LEN    0x7f    /* user-segment-table up to 2 GB    */
#define _KERNEL_SEG_TABLE_LEN  0x7f    /* kernel-segment-table up to 2 GB  */

/*
 * User and Kernel pagetables are identical
 */
#define _PAGE_TABLE	_PAGE_TABLE_LEN
#define _KERNPG_TABLE	_PAGE_TABLE_LEN

/*
 * The Kernel segment-tables includes the User segment-table
 */

#define _SEGMENT_TABLE	(_USER_SEG_TABLE_LEN|0x80000000|0x100)
#define _KERNSEG_TABLE	_KERNEL_SEG_TABLE_LEN

#define USER_STD_MASK	0x00000080UL

#else /* __s390x__ */

/* Bits in the segment table entry */
#define _PMD_ENTRY_INV	0x20		/* invalid segment table entry      */
#define _PMD_ENTRY	0x00
#define _SEGMENT_ENTRY_ORIGIN	~0x7ffUL/* segment table origin		    */
#define _SEGMENT_ENTRY_LARGE	0x400
#define _SEGMENT_ENTRY_CO	0x100
#define _SEGMENT_ENTRY_RO	0x200	/* page protection bit		    */
#define _SEGMENT_ENTRY_INV	0x20	/* invalid segment table entry	    */

/* Bits in the region third table entry */
#define _PGD_ENTRY_INV   0x20          /* invalid region table entry       */
#define _PGD_ENTRY       0x07

/*
 * User and kernel page directory
 */
#define _REGION_THIRD       0x4
#define _REGION_THIRD_LEN   0x3 
#define _REGION_TABLE       (_REGION_THIRD|_REGION_THIRD_LEN|0x40|0x100)
#define _KERN_REGION_TABLE  (_REGION_THIRD|_REGION_THIRD_LEN)

#define USER_STD_MASK           0x0000000000000080UL

/* Bits in the storage key */
#define _PAGE_CHANGED    0x02          /* HW changed bit                   */
#define _PAGE_REFERENCED 0x04          /* HW referenced bit                */

#endif /* __s390x__ */

/*
 * Page protection definitions.
 */
#define PAGE_NONE	__pgprot(_PAGE_TYPE_NONE)
#define PAGE_RO		__pgprot(_PAGE_TYPE_RO)
#define PAGE_RW		__pgprot(_PAGE_TYPE_RW)

#define PAGE_KERNEL	PAGE_RW
#define PAGE_COPY	PAGE_RO

/*
 * The S390 can't do page protection for execute, and considers that the
 * same are read. Also, write permissions imply read permissions. This is
 * the closest we can get..
 */
         /*xwr*/
#define __P000	PAGE_NONE
#define __P001	PAGE_RO
#define __P010	PAGE_RO
#define __P011	PAGE_RO
#define __P100	PAGE_RO
#define __P101	PAGE_RO
#define __P110	PAGE_RO
#define __P111	PAGE_RO

#define __S000	PAGE_NONE
#define __S001	PAGE_RO
#define __S010	PAGE_RW
#define __S011	PAGE_RW
#define __S100	PAGE_RO
#define __S101	PAGE_RO
#define __S110	PAGE_RW
#define __S111	PAGE_RW

/*
 * Certain architectures need to do special things when PTEs
 * within a page table are directly modified.  Thus, the following
 * hook is made available.
 */
static inline void set_pte(pte_t *pteptr, pte_t pteval)
{
	*pteptr = pteval;
}
#define set_pte_at(mm,addr,ptep,pteval) set_pte(ptep,pteval)

/*
 * pgd/pmd/pte query functions
 */
#ifndef __s390x__

static inline int pgd_present(pgd_t pgd) { return 1; }
static inline int pgd_none(pgd_t pgd)    { return 0; }
static inline int pgd_bad(pgd_t pgd)     { return 0; }

static inline int pmd_present(pmd_t pmd) { return pmd_val(pmd) & _SEG_PRESENT; }
static inline int pmd_none(pmd_t pmd)    { return pmd_val(pmd) & _PAGE_TABLE_INV; }
static inline int pmd_bad(pmd_t pmd)
{
	return (pmd_val(pmd) & (~PAGE_MASK & ~_PAGE_TABLE_INV)) != _PAGE_TABLE;
}

#else /* __s390x__ */

static inline int pgd_present(pgd_t pgd)
{
	return (pgd_val(pgd) & ~PAGE_MASK) == _PGD_ENTRY;
}

static inline int pgd_none(pgd_t pgd)
{
	return pgd_val(pgd) & _PGD_ENTRY_INV;
}

static inline int pgd_bad(pgd_t pgd)
{
	return (pgd_val(pgd) & (~PAGE_MASK & ~_PGD_ENTRY_INV)) != _PGD_ENTRY;
}

static inline int pmd_present(pmd_t pmd)
{
	return (pmd_val(pmd) & ~PAGE_MASK) == _PMD_ENTRY;
}

static inline int pmd_none(pmd_t pmd)
{
	return pmd_val(pmd) & _PMD_ENTRY_INV;
}

static inline int pmd_bad(pmd_t pmd)
{
	return (pmd_val(pmd) & (~PAGE_MASK & ~_PMD_ENTRY_INV)) != _PMD_ENTRY;
}

#endif /* __s390x__ */

static inline int pte_none(pte_t pte)
{
	return (pte_val(pte) & _PAGE_INVALID) && !(pte_val(pte) & _PAGE_SWT);
}

static inline int pte_present(pte_t pte)
{
	unsigned long mask = _PAGE_RO | _PAGE_INVALID | _PAGE_SWT | _PAGE_SWX;
	return (pte_val(pte) & mask) == _PAGE_TYPE_NONE ||
		(!(pte_val(pte) & _PAGE_INVALID) &&
		 !(pte_val(pte) & _PAGE_SWT));
}

static inline int pte_file(pte_t pte)
{
	unsigned long mask = _PAGE_RO | _PAGE_INVALID | _PAGE_SWT;
	return (pte_val(pte) & mask) == _PAGE_TYPE_FILE;
}

static inline int pte_special(pte_t pte)
{
	return 0;
}

#define pte_same(a,b)	(pte_val(a) == pte_val(b))

/*
 * query functions pte_write/pte_dirty/pte_young only work if
 * pte_present() is true. Undefined behaviour if not..
 */
static inline int pte_write(pte_t pte)
{
	return (pte_val(pte) & _PAGE_RO) == 0;
}

static inline int pte_dirty(pte_t pte)
{
	/* A pte is neither clean nor dirty on s/390. The dirty bit
	 * is in the storage key. See page_test_and_clear_dirty for
	 * details.
	 */
	return 0;
}

static inline int pte_young(pte_t pte)
{
	/* A pte is neither young nor old on s/390. The young bit
	 * is in the storage key. See page_test_and_clear_young for
	 * details.
	 */
	return 0;
}

static inline int pte_read(pte_t pte)
{
	/* All pages are readable since we don't use the fetch
	 * protection bit in the storage key.
	 */
	return 1;
}

/*
 * pgd/pmd/pte modification functions
 */

#ifndef __s390x__

static inline void pgd_clear(pgd_t * pgdp)      { }

static inline void pmd_clear(pmd_t * pmdp)
{
	pmd_val(pmdp[0]) = _PAGE_TABLE_INV;
	pmd_val(pmdp[1]) = _PAGE_TABLE_INV;
	pmd_val(pmdp[2]) = _PAGE_TABLE_INV;
	pmd_val(pmdp[3]) = _PAGE_TABLE_INV;
}

#else /* __s390x__ */

static inline void pgd_clear(pgd_t * pgdp)
{
	pgd_val(*pgdp) = _PGD_ENTRY_INV | _PGD_ENTRY;
}

static inline void pmd_clear(pmd_t * pmdp)
{
	pmd_val(*pmdp) = _PMD_ENTRY_INV | _PMD_ENTRY;
	pmd_val1(*pmdp) = _PMD_ENTRY_INV | _PMD_ENTRY;
}

#endif /* __s390x__ */

static inline void pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	pte_val(*ptep) = _PAGE_TYPE_EMPTY;
}

/*
 * The following pte modification functions only work if
 * pte_present() is true. Undefined behaviour if not..
 */
static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	pte_val(pte) &= PAGE_MASK;
	pte_val(pte) |= pgprot_val(newprot);
	return pte;
}

static inline pte_t pte_wrprotect(pte_t pte)
{
	/* Do not clobber _PAGE_TYPE_NONE pages!  */
	if (!(pte_val(pte) & _PAGE_INVALID))
		pte_val(pte) |= _PAGE_RO;
	return pte;
}

static inline pte_t pte_mkwrite(pte_t pte)
{
	pte_val(pte) &= ~_PAGE_RO;
	return pte;
}

static inline pte_t pte_mkclean(pte_t pte)
{
	/* The only user of pte_mkclean is the fork() code.
	   We must *not* clear the *physical* page dirty bit
	   just because fork() wants to clear the dirty bit in
	   *one* of the page's mappings.  So we just do nothing. */
	return pte;
}

static inline pte_t pte_mkdirty(pte_t pte)
{
	/* We do not explicitly set the dirty bit because the
	 * sske instruction is slow. It is faster to let the
	 * next instruction set the dirty bit.
	 */
	return pte;
}

static inline pte_t pte_mkold(pte_t pte)
{
	/* S/390 doesn't keep its dirty/referenced bit in the pte.
	 * There is no point in clearing the real referenced bit.
	 */
	return pte;
}

static inline pte_t pte_mkyoung(pte_t pte)
{
	/* S/390 doesn't keep its dirty/referenced bit in the pte.
	 * There is no point in setting the real referenced bit.
	 */
	return pte;
}

static inline pte_t pte_mkspecial(pte_t pte)
{
	return pte;
}

static inline int ptep_test_and_clear_young(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep)
{
	return 0;
}

static inline int
ptep_clear_flush_young(struct vm_area_struct *vma,
			unsigned long address, pte_t *ptep)
{
	/* No need to flush TLB; bits are in storage key */
	return ptep_test_and_clear_young(vma, address, ptep);
}

static inline int ptep_test_and_clear_dirty(struct vm_area_struct *vma, unsigned long addr, pte_t *ptep)
{
	return 0;
}

static inline int
ptep_clear_flush_dirty(struct vm_area_struct *vma,
			unsigned long address, pte_t *ptep)
{
	/* No need to flush TLB; bits are in storage key */
	return ptep_test_and_clear_dirty(vma, address, ptep);
}

static inline void __ptep_ipte(unsigned long address, pte_t *ptep)
{
	if (!(pte_val(*ptep) & _PAGE_INVALID)) {
#ifndef __s390x__
		/* pto must point to the start of the segment table */
		pte_t *pto = (pte_t *) (((unsigned long) ptep) & 0x7ffffc00);
#else
		/* ipte in zarch mode can do the math */
		pte_t *pto = ptep;
#endif
		asm volatile(
			"       ipte    %2,%3"
			: "=m" (*ptep) : "m" (*ptep),
			"a" (pto), "a" (address));
	}
}

static inline void ptep_invalidate(unsigned long address, pte_t *ptep)
{
	__ptep_ipte(address, ptep);
	pte_val(*ptep) = _PAGE_TYPE_EMPTY;
}

/*
 * This is hard to understand. ptep_get_and_clear and ptep_clear_flush
 * both clear the TLB for the unmapped pte. The reason is that
 * ptep_get_and_clear is used in common code (e.g. change_pte_range)
 * to modify an active pte. The sequence is
 *   1) ptep_get_and_clear
 *   2) set_pte_at
 *   3) flush_tlb_range
 * On s390 the tlb needs to get flushed with the modification of the pte
 * if the pte is active. The only way how this can be implemented is to
 * have ptep_get_and_clear do the tlb flush. In exchange flush_tlb_range
 * is a nop.
 */
#define ptep_get_and_clear(__mm, __address, __ptep)			\
({									\
	pte_t __pte = *(__ptep);					\
	if (atomic_read(&(__mm)->mm_users) > 1 ||			\
	    (__mm) != current->active_mm)				\
		ptep_invalidate(__address, __ptep);			\
	else								\
		pte_clear((__mm), (__address), (__ptep));		\
	__pte;								\
})

static inline pte_t
ptep_clear_flush(struct vm_area_struct *vma,
		 unsigned long address, pte_t *ptep)
{
	pte_t pte = *ptep;
	ptep_invalidate(address, ptep);
	return pte;
}

/*
 * The batched pte unmap code uses ptep_get_and_clear_full to clear the
 * ptes. Here an optimization is possible. tlb_gather_mmu flushes all
 * tlbs of an mm if it can guarantee that the ptes of the mm_struct
 * cannot be accessed while the batched unmap is running. In this case
 * full==1 and a simple pte_clear is enough. See tlb.h.
 */
static inline pte_t ptep_get_and_clear_full(struct mm_struct *mm,
					    unsigned long addr,
					    pte_t *ptep, int full)
{
	pte_t pte = *ptep;

	if (full)
		pte_clear(mm, addr, ptep);
	else
		ptep_invalidate(addr, ptep);
	return pte;
}

static inline void
ptep_establish(struct vm_area_struct *vma, 
	       unsigned long address, pte_t *ptep,
	       pte_t entry)
{
	ptep_clear_flush(vma, address, ptep);
	set_pte(ptep, entry);
}

#define ptep_set_wrprotect(__mm, __addr, __ptep)			\
({									\
	pte_t __pte = *(__ptep);					\
	if (pte_write(__pte)) {						\
		if (atomic_read(&(__mm)->mm_users) > 1 ||		\
		    (__mm) != current->active_mm)			\
			ptep_invalidate(__addr, __ptep);		\
		set_pte_at(__mm, __addr, __ptep, pte_wrprotect(__pte));	\
	}								\
})

#define ptep_set_access_flags(__vma, __address, __ptep, __entry, __dirty) \
	ptep_establish(__vma, __address, __ptep, __entry)

/*
 * Test and clear dirty bit in storage key.
 * We can't clear the changed bit atomically. This is a potential
 * race against modification of the referenced bit. This function
 * should therefore only be called if it is not mapped in any
 * address space.
 */
#define page_test_and_clear_dirty(_page)				  \
({									  \
	struct page *__page = (_page);					  \
	unsigned long __physpage = __pa((__page-mem_map) << PAGE_SHIFT);  \
	int __skey = page_get_storage_key(__physpage);			  \
	if (__skey & _PAGE_CHANGED)					  \
		page_set_storage_key(__physpage, __skey & ~_PAGE_CHANGED);\
	(__skey & _PAGE_CHANGED);					  \
})

/*
 * Test and clear referenced bit in storage key.
 */
#define page_test_and_clear_young(page)					  \
({									  \
	struct page *__page = (page);					  \
	unsigned long __physpage = __pa((__page-mem_map) << PAGE_SHIFT);  \
	int __ccode;							  \
	asm volatile ("rrbe 0,%1\n\t"					  \
		      "ipm  %0\n\t"					  \
		      "srl  %0,28\n\t" 					  \
                      : "=d" (__ccode) : "a" (__physpage) : "cc" );	  \
	(__ccode & 2);							  \
})

/*
 * Conversion functions: convert a page and protection to a page entry,
 * and a page entry and page directory to the page they refer to.
 */
static inline pte_t mk_pte_phys(unsigned long physpage, pgprot_t pgprot)
{
	pte_t __pte;
	pte_val(__pte) = physpage + pgprot_val(pgprot);
	return __pte;
}

#define mk_pte(pg, pgprot)                                                \
({                                                                        \
	struct page *__page = (pg);                                       \
	pgprot_t __pgprot = (pgprot);					  \
	unsigned long __physpage = __pa((__page-mem_map) << PAGE_SHIFT);  \
	pte_t __pte = mk_pte_phys(__physpage, __pgprot);                  \
	__pte;                                                            \
})

#define pfn_pte(pfn, pgprot)                                              \
({                                                                        \
	pgprot_t __pgprot = (pgprot);					  \
	unsigned long __physpage = __pa((pfn) << PAGE_SHIFT);             \
	pte_t __pte = mk_pte_phys(__physpage, __pgprot);                  \
	__pte;                                                            \
})

#ifdef __s390x__

#define pfn_pmd(pfn, pgprot)                                              \
({                                                                        \
	pgprot_t __pgprot = (pgprot);                                     \
	unsigned long __physpage = __pa((pfn) << PAGE_SHIFT);             \
	pmd_t __pmd = __pmd(__physpage + pgprot_val(__pgprot));           \
	__pmd;                                                            \
})

#endif /* __s390x__ */

#define pte_pfn(x) (pte_val(x) >> PAGE_SHIFT)
#define pte_page(x) pfn_to_page(pte_pfn(x))

#define pmd_page_kernel(pmd) (pmd_val(pmd) & PAGE_MASK)

#define pmd_page(pmd) (mem_map+(pmd_val(pmd) >> PAGE_SHIFT))

#define pgd_page_kernel(pgd) (pgd_val(pgd) & PAGE_MASK)

/* to find an entry in a page-table-directory */
#define pgd_index(address) (((address) >> PGDIR_SHIFT) & (PTRS_PER_PGD-1))
#define pgd_offset(mm, address) ((mm)->pgd+pgd_index(address))

/* to find an entry in a kernel page-table-directory */
#define pgd_offset_k(address) pgd_offset(&init_mm, address)

#ifndef __s390x__

/* Find an entry in the second-level page table.. */
static inline pmd_t * pmd_offset(pgd_t * dir, unsigned long address)
{
        return (pmd_t *) dir;
}

#else /* __s390x__ */

/* Find an entry in the second-level page table.. */
#define pmd_index(address) (((address) >> PMD_SHIFT) & (PTRS_PER_PMD-1))
#define pmd_offset(dir,addr) \
	((pmd_t *) pgd_page_kernel(*(dir)) + pmd_index(addr))

#endif /* __s390x__ */

/* Find an entry in the third-level page table.. */
#define pte_index(address) (((address) >> PAGE_SHIFT) & (PTRS_PER_PTE-1))
#define pte_offset_kernel(pmd, address) \
	((pte_t *) pmd_page_kernel(*(pmd)) + pte_index(address))
#define pte_offset_map(pmd, address) pte_offset_kernel(pmd, address)
#define pte_offset_map_nested(pmd, address) pte_offset_kernel(pmd, address)
#define pte_unmap(pte) do { } while (0)
#define pte_unmap_nested(pte) do { } while (0)

#ifdef __s390x__
static inline pte_t pte_mkhuge(pte_t pte)
{
	/*
	 * PROT_NONE needs to be remapped from the pte type to the ste type.
	 * The HW invalid bit is also different for pte and ste. The pte
	 * invalid bit happens to be the same as the ste _SEGMENT_ENTRY_LARGE
	 * bit, so we don't have to clear it.
	 */
	if (pte_val(pte) & _PAGE_INVALID) {
		if (pte_val(pte) & _PAGE_SWT)
			pte_val(pte) |= _HPAGE_TYPE_NONE;
		pte_val(pte) |= _PMD_ENTRY_INV;
	}
	/*
	 * Clear SW pte bits SWT and SWX, there are no SW bits in a segment
	 * table entry.
	 */
	pte_val(pte) &= ~(_PAGE_SWT | _PAGE_SWX);
	pte_val(pte) |= (_SEGMENT_ENTRY_LARGE | _SEGMENT_ENTRY_CO);
	return pte;
}

static inline pte_t huge_pte_wrprotect(pte_t pte)
{
	pte_val(pte) |= _PAGE_RO;
	return pte;
}

static inline int huge_pte_none(pte_t pte)
{
	return (pte_val(pte) & _SEGMENT_ENTRY_INV) &&
		!(pte_val(pte) & _SEGMENT_ENTRY_RO);
}

static inline pte_t huge_ptep_get(pte_t *ptep)
{
	pte_t pte = *ptep;
	unsigned long mask;

	if (!MACHINE_HAS_HPAGE) {
		ptep = (pte_t *) (pte_val(pte) & _SEGMENT_ENTRY_ORIGIN);
		if (ptep) {
			mask = pte_val(pte) &
				(_SEGMENT_ENTRY_INV | _SEGMENT_ENTRY_RO);
			pte = pte_mkhuge(*ptep);
			pte_val(pte) |= mask;
		}
	}
	return pte;
}

static inline pte_t huge_ptep_get_and_clear(struct mm_struct *mm,
					    unsigned long addr, pte_t *ptep)
{
	pte_t pte = huge_ptep_get(ptep);

	pmd_clear((pmd_t *) ptep);
	return pte;
}

static inline void __pmd_csp(pmd_t *pmdp)
{
	register unsigned long reg2 asm("2") = pmd_val(*pmdp);
	register unsigned long reg3 asm("3") = pmd_val(*pmdp) |
					       _PMD_ENTRY_INV;
	register unsigned long reg4 asm("4") = ((unsigned long) pmdp) + 5;

	asm volatile(
		"	csp %1,%3"
		: "=m" (*pmdp)
		: "d" (reg2), "d" (reg3), "d" (reg4), "m" (*pmdp) : "cc");
	pmd_val(*pmdp) = _PMD_ENTRY_INV | _PMD_ENTRY;
}

static inline void __pmd_idte(unsigned long address, pmd_t *pmdp)
{
	unsigned long sto = (unsigned long) pmdp -
				pmd_index(address) * sizeof(pmd_t);

	if (!(pmd_val(*pmdp) & _PMD_ENTRY_INV)) {
		asm volatile(
			"	.insn	rrf,0xb98e0000,%2,%3,0,0"
			: "=m" (*pmdp)
			: "m" (*pmdp), "a" (sto),
			  "a" ((address & HPAGE_MASK) + 1)
		);
	}
	pmd_val(*pmdp) = _PMD_ENTRY_INV | _PMD_ENTRY;
	pmd_val1(*pmdp) = _PMD_ENTRY_INV | _PMD_ENTRY;
}

static inline void huge_ptep_invalidate(unsigned long address, pte_t *ptep)
{
	pmd_t *pmdp = (pmd_t *) ptep;

	if (!MACHINE_HAS_IDTE) {
		__pmd_csp(pmdp);
		__pmd_csp((void *) pmdp + 8);
		return;
	}

	__pmd_idte(address, pmdp);
	return;
}

#define huge_ptep_set_access_flags(__vma, __addr, __ptep, __entry, __dirty) \
({									    \
	huge_ptep_invalidate(__addr, __ptep);				    \
	set_huge_pte_at((__vma)->vm_mm, __addr, __ptep, __entry);	    \
})

#define huge_ptep_set_wrprotect(__mm, __addr, __ptep)			\
({									\
	pte_t __pte = huge_ptep_get(__ptep);				\
	if (pte_write(__pte)) {						\
		set_huge_pte_at(__mm, __addr, __ptep,			\
				huge_pte_wrprotect(__pte));		\
	}								\
})

static inline void huge_ptep_clear_flush(struct vm_area_struct *vma,
                                        unsigned long address, pte_t *ptep)
{
       huge_ptep_invalidate(address, ptep);
}
#endif /* __s390x__ */

/*
 * 31 bit swap entry format:
 * A page-table entry has some bits we have to treat in a special way.
 * Bits 0, 20 and bit 23 have to be zero, otherwise an specification
 * exception will occur instead of a page translation exception. The
 * specifiation exception has the bad habit not to store necessary
 * information in the lowcore.
 * Bit 21 and bit 22 are the page invalid bit and the page protection
 * bit. We set both to indicate a swapped page.
 * Bit 30 and 31 are used to distinguish the different page types. For
 * a swapped page these bits need to be zero.
 * This leaves the bits 1-19 and bits 24-29 to store type and offset.
 * We use the 5 bits from 25-29 for the type and the 20 bits from 1-19
 * plus 24 for the offset.
 * 0|     offset        |0110|o|type |00|
 * 0 0000000001111111111 2222 2 22222 33
 * 0 1234567890123456789 0123 4 56789 01
 *
 * 64 bit swap entry format:
 * A page-table entry has some bits we have to treat in a special way.
 * Bits 52 and bit 55 have to be zero, otherwise an specification
 * exception will occur instead of a page translation exception. The
 * specifiation exception has the bad habit not to store necessary
 * information in the lowcore.
 * Bit 53 and bit 54 are the page invalid bit and the page protection
 * bit. We set both to indicate a swapped page.
 * Bit 62 and 63 are used to distinguish the different page types. For
 * a swapped page these bits need to be zero.
 * This leaves the bits 0-51 and bits 56-61 to store type and offset.
 * We use the 5 bits from 57-61 for the type and the 53 bits from 0-51
 * plus 56 for the offset.
 * |                      offset                        |0110|o|type |00|
 *  0000000000111111111122222222223333333333444444444455 5555 5 55566 66
 *  0123456789012345678901234567890123456789012345678901 2345 6 78901 23
 */
#ifndef __s390x__
#define __SWP_OFFSET_MASK (~0UL >> 12)
#else
#define __SWP_OFFSET_MASK (~0UL >> 11)
#endif
static inline pte_t mk_swap_pte(unsigned long type, unsigned long offset)
{
	pte_t pte;
	offset &= __SWP_OFFSET_MASK;
	pte_val(pte) = _PAGE_TYPE_SWAP | ((type & 0x1f) << 2) |
		((offset & 1UL) << 7) | ((offset & ~1UL) << 11);
	return pte;
}

#define __swp_type(entry)	(((entry).val >> 2) & 0x1f)
#define __swp_offset(entry)	(((entry).val >> 11) | (((entry).val >> 7) & 1))
#define __swp_entry(type,offset) ((swp_entry_t) { pte_val(mk_swap_pte((type),(offset))) })

#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)	((pte_t) { (x).val })

#ifndef __s390x__
# define PTE_FILE_MAX_BITS	26
#else /* __s390x__ */
# define PTE_FILE_MAX_BITS	59
#endif /* __s390x__ */

#define pte_to_pgoff(__pte) \
	((((__pte).pte >> 12) << 7) + (((__pte).pte >> 1) & 0x7f))

#define pgoff_to_pte(__off) \
	((pte_t) { ((((__off) & 0x7f) << 1) + (((__off) >> 7) << 12)) \
		   | _PAGE_TYPE_FILE })

#endif /* !__ASSEMBLY__ */

#define kern_addr_valid(addr)   (1)

/*
 * No page table caches to initialise
 */
#define pgtable_cache_init()	do { } while (0)

#define __HAVE_ARCH_PTEP_ESTABLISH
#define __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
#define __HAVE_ARCH_PTEP_TEST_AND_CLEAR_YOUNG
#define __HAVE_ARCH_PTEP_CLEAR_YOUNG_FLUSH
#define __HAVE_ARCH_PTEP_TEST_AND_CLEAR_DIRTY
#define __HAVE_ARCH_PTEP_CLEAR_DIRTY_FLUSH
#define __HAVE_ARCH_PTEP_GET_AND_CLEAR
#define __HAVE_ARCH_PTEP_GET_AND_CLEAR_FULL
#define __HAVE_ARCH_PTEP_CLEAR_FLUSH
#define __HAVE_ARCH_PTEP_SET_WRPROTECT
#define __HAVE_ARCH_PTE_SAME
#define __HAVE_ARCH_PAGE_TEST_AND_CLEAR_DIRTY
#define __HAVE_ARCH_PAGE_TEST_AND_CLEAR_YOUNG
#include <asm-generic/pgtable.h>

#endif /* _S390_PAGE_H */

