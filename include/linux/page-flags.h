/*
 * Macros for manipulating and testing page->flags
 */

#ifndef PAGE_FLAGS_H
#define PAGE_FLAGS_H

#include <linux/types.h>

/*
 * Various page->flags bits:
 *
 * PG_reserved is set for special pages, which can never be swapped out. Some
 * of them might not even exist (eg empty_bad_page)...
 *
 * The PG_private bitflag is set if page->private contains a valid value.
 *
 * During disk I/O, PG_locked is used. This bit is set before I/O and
 * reset when I/O completes. page_waitqueue(page) is a wait queue of all tasks
 * waiting for the I/O on this page to complete.
 *
 * PG_uptodate tells whether the page's contents is valid.  When a read
 * completes, the page becomes uptodate, unless a disk I/O error happened.
 *
 * For choosing which pages to swap out, inode pages carry a PG_referenced bit,
 * which is set any time the system accesses that page through the (mapping,
 * index) hash table.  This referenced bit, together with the referenced bit
 * in the page tables, is used to manipulate page->age and move the page across
 * the active, inactive_dirty and inactive_clean lists.
 *
 * Note that the referenced bit, the page->lru list_head and the active,
 * inactive_dirty and inactive_clean lists are protected by the
 * zone->lru_lock, and *NOT* by the usual PG_locked bit!
 *
 * PG_error is set to indicate that an I/O error occurred on this page.
 *
 * PG_arch_1 is an architecture specific page state bit.  The generic code
 * guarantees that this bit is cleared for a page when it first is entered into
 * the page cache.
 *
 * PG_highmem pages are not permanently mapped into the kernel virtual address
 * space, they need to be kmapped separately for doing IO on the pages.  The
 * struct page (these bits with information) are always mapped into kernel
 * address space...
 */

/*
 * Don't use the *_dontuse flags.  Use the macros.  Otherwise you'll break
 * locked- and dirty-page accounting.
 *
 * The page flags field is split into two parts, the main flags area
 * which extends from the low bits upwards, and the fields area which
 * extends from the high bits downwards.
 *
 *  | FIELD | ... | FLAGS |
 *  N-1     ^             0
 *          (N-FLAGS_RESERVED)
 *
 * The fields area is reserved for fields mapping zone, node and SPARSEMEM
 * section.  The boundry between these two areas is defined by
 * FLAGS_RESERVED which defines the width of the fields section
 * (see linux/mmzone.h).  New flags must _not_ overlap with this area.
 */
#define PG_locked	 	 0	/* Page is locked. Don't touch. */
#define PG_error		 1
#define PG_referenced		 2
#define PG_uptodate		 3

#define PG_dirty	 	 4
#define PG_lru			 5
#define PG_active		 6
#define PG_slab			 7	/* slab debug (Suparna wants this) */

#define PG_fs_misc		 8
#define PG_owner_priv_1          8      /* Owner use. If pagecache, fs may use*/
#define PG_arch_1		 9
#define PG_reserved		10
#define PG_private		11	/* Has something at ->private */

#define PG_writeback		12	/* Page is under writeback */
#define PG_nosave		13	/* Used for system suspend/resume */
#define PG_compound		14	/* Part of a compound page */
#define PG_swapcache		15	/* Swap page: swp_entry_t in private */

#define PG_mappedtodisk		16	/* Has blocks allocated on-disk */
#define PG_reclaim		17	/* To be reclaimed asap */
#define PG_nosave_free		18	/* Free, should not be written */
#define PG_buddy		19	/* Page is free, on buddy lists */
#define PG_xpmem		27	/* Testing for xpmem. */

/* PG_owner_priv_1 users should have descriptive aliases */
#define PG_checked              PG_owner_priv_1 /* Used by some filesystems */

#if (BITS_PER_LONG > 32)
/*
 * 64-bit-only flags build down from bit 31
 *
 * 32 bit  -------------------------------| FIELDS |       FLAGS         |
 * 64 bit  |           FIELDS             | ??????         FLAGS         |
 *         63                            32                              0
 */
#define PG_uncached		31	/* Page has been mapped as uncached */
#endif

/*
 * Manipulation of page state flags
 */
#define PageLocked(page)		\
		test_bit(PG_locked, &(page)->flags)
#define SetPageLocked(page)		\
		set_bit(PG_locked, &(page)->flags)
#define TestSetPageLocked(page)		\
		test_and_set_bit(PG_locked, &(page)->flags)
#define ClearPageLocked(page)		\
		clear_bit(PG_locked, &(page)->flags)
#define TestClearPageLocked(page)	\
		test_and_clear_bit(PG_locked, &(page)->flags)

#define PageError(page)		test_bit(PG_error, &(page)->flags)
#define SetPageError(page)	set_bit(PG_error, &(page)->flags)
#define ClearPageError(page)	clear_bit(PG_error, &(page)->flags)

#define PageReferenced(page)	test_bit(PG_referenced, &(page)->flags)
#define SetPageReferenced(page)	set_bit(PG_referenced, &(page)->flags)
#define ClearPageReferenced(page)	clear_bit(PG_referenced, &(page)->flags)
#define TestClearPageReferenced(page) test_and_clear_bit(PG_referenced, &(page)->flags)

#define PageUptodate(page)	test_bit(PG_uptodate, &(page)->flags)
#ifdef CONFIG_S390
#define SetPageUptodate(_page) \
	do {								      \
		struct page *__page = (_page);				      \
		if (!test_and_set_bit(PG_uptodate, &__page->flags))	      \
			page_test_and_clear_dirty(_page);		      \
	} while (0)
#else
#define SetPageUptodate(page)	set_bit(PG_uptodate, &(page)->flags)
#endif
#define ClearPageUptodate(page)	clear_bit(PG_uptodate, &(page)->flags)

#define PageDirty(page)		test_bit(PG_dirty, &(page)->flags)
#define SetPageDirty(page)	set_bit(PG_dirty, &(page)->flags)
#define TestSetPageDirty(page)	test_and_set_bit(PG_dirty, &(page)->flags)
#define ClearPageDirty(page)	clear_bit(PG_dirty, &(page)->flags)
#define __ClearPageDirty(page)	__clear_bit(PG_dirty, &(page)->flags)
#define TestClearPageDirty(page) test_and_clear_bit(PG_dirty, &(page)->flags)

#define PageLRU(page)		test_bit(PG_lru, &(page)->flags)
#define SetPageLRU(page)	set_bit(PG_lru, &(page)->flags)
#define ClearPageLRU(page)	clear_bit(PG_lru, &(page)->flags)
#define __ClearPageLRU(page)	__clear_bit(PG_lru, &(page)->flags)

#define PageActive(page)	test_bit(PG_active, &(page)->flags)
#define SetPageActive(page)	set_bit(PG_active, &(page)->flags)
#define ClearPageActive(page)	clear_bit(PG_active, &(page)->flags)
#define __ClearPageActive(page)	__clear_bit(PG_active, &(page)->flags)

#define PageSlab(page)		test_bit(PG_slab, &(page)->flags)
#define __SetPageSlab(page)	__set_bit(PG_slab, &(page)->flags)
#define __ClearPageSlab(page)	__clear_bit(PG_slab, &(page)->flags)

#ifdef CONFIG_HIGHMEM
#define PageHighMem(page)	is_highmem(page_zone(page))
#else
#define PageHighMem(page)	0 /* needed to optimize away at compile time */
#endif

#define PageChecked(page)       test_bit(PG_checked, &(page)->flags)
#define SetPageChecked(page)    set_bit(PG_checked, &(page)->flags)
#define ClearPageChecked(page)  clear_bit(PG_checked, &(page)->flags)

#define PageReserved(page)	test_bit(PG_reserved, &(page)->flags)
#define SetPageReserved(page)	set_bit(PG_reserved, &(page)->flags)
#define ClearPageReserved(page)	clear_bit(PG_reserved, &(page)->flags)
#define __ClearPageReserved(page)	__clear_bit(PG_reserved, &(page)->flags)

#define SetPagePrivate(page)	set_bit(PG_private, &(page)->flags)
#define ClearPagePrivate(page)	clear_bit(PG_private, &(page)->flags)
#define PagePrivate(page)	test_bit(PG_private, &(page)->flags)
#define __SetPagePrivate(page)  __set_bit(PG_private, &(page)->flags)
#define __ClearPagePrivate(page) __clear_bit(PG_private, &(page)->flags)

#define PageWriteback(page)	test_bit(PG_writeback, &(page)->flags)
#define SetPageWriteback(page)						\
	do {								\
		if (!test_and_set_bit(PG_writeback,			\
				&(page)->flags))			\
			inc_zone_page_state(page, NR_WRITEBACK);	\
	} while (0)
#define TestSetPageWriteback(page)					\
	({								\
		int ret;						\
		ret = test_and_set_bit(PG_writeback,			\
					&(page)->flags);		\
		if (!ret)						\
			inc_zone_page_state(page, NR_WRITEBACK);	\
		ret;							\
	})
#define ClearPageWriteback(page)					\
	do {								\
		if (test_and_clear_bit(PG_writeback,			\
				&(page)->flags))			\
			dec_zone_page_state(page, NR_WRITEBACK);	\
	} while (0)
#define TestClearPageWriteback(page)					\
	({								\
		int ret;						\
		ret = test_and_clear_bit(PG_writeback,			\
				&(page)->flags);			\
		if (ret)						\
			dec_zone_page_state(page, NR_WRITEBACK);	\
		ret;							\
	})

#define PageNosave(page)	test_bit(PG_nosave, &(page)->flags)
#define SetPageNosave(page)	set_bit(PG_nosave, &(page)->flags)
#define TestSetPageNosave(page)	test_and_set_bit(PG_nosave, &(page)->flags)
#define ClearPageNosave(page)		clear_bit(PG_nosave, &(page)->flags)
#define TestClearPageNosave(page)	test_and_clear_bit(PG_nosave, &(page)->flags)

#define PageNosaveFree(page)	test_bit(PG_nosave_free, &(page)->flags)
#define SetPageNosaveFree(page)	set_bit(PG_nosave_free, &(page)->flags)
#define ClearPageNosaveFree(page)		clear_bit(PG_nosave_free, &(page)->flags)

#define PageBuddy(page)		test_bit(PG_buddy, &(page)->flags)
#define __SetPageBuddy(page)	__set_bit(PG_buddy, &(page)->flags)
#define __ClearPageBuddy(page)	__clear_bit(PG_buddy, &(page)->flags)

#define PageMappedToDisk(page)	test_bit(PG_mappedtodisk, &(page)->flags)
#define SetPageMappedToDisk(page) set_bit(PG_mappedtodisk, &(page)->flags)
#define ClearPageMappedToDisk(page) clear_bit(PG_mappedtodisk, &(page)->flags)

#define PageReclaim(page)	test_bit(PG_reclaim, &(page)->flags)
#define SetPageReclaim(page)	set_bit(PG_reclaim, &(page)->flags)
#define ClearPageReclaim(page)	clear_bit(PG_reclaim, &(page)->flags)
#define TestClearPageReclaim(page) test_and_clear_bit(PG_reclaim, &(page)->flags)

#define PageCompound(page)	test_bit(PG_compound, &(page)->flags)
#define __SetPageCompound(page)	__set_bit(PG_compound, &(page)->flags)
#define __ClearPageCompound(page) __clear_bit(PG_compound, &(page)->flags)

/*
 * PG_reclaim is used in combination with PG_compound to mark the
 * head and tail of a compound page
 *
 * PG_compound & PG_reclaim	=> Tail page
 * PG_compound & ~PG_reclaim	=> Head page
 */

#define PG_head_tail_mask ((1L << PG_compound) | (1L << PG_reclaim))

#define PageTail(page)	((page->flags & PG_head_tail_mask) \
				== PG_head_tail_mask)

#define __SetPageTail(page)	((page)->flags |= PG_head_tail_mask)

#define __ClearPageTail(page)	((page)->flags &= ~PG_head_tail_mask)

#define PageHead(page)	((page->flags & PG_head_tail_mask) \
				== (1L << PG_compound))
#define __SetPageHead(page)	__SetPageCompound(page)
#define __ClearPageHead(page)	__ClearPageCompound(page)

#ifdef CONFIG_SWAP
#define PageSwapCache(page)	test_bit(PG_swapcache, &(page)->flags)
#define SetPageSwapCache(page)	set_bit(PG_swapcache, &(page)->flags)
#define ClearPageSwapCache(page) clear_bit(PG_swapcache, &(page)->flags)
#else
#define PageSwapCache(page)	0
#endif

#define PageUncached(page)	test_bit(PG_uncached, &(page)->flags)
#define SetPageUncached(page)	set_bit(PG_uncached, &(page)->flags)
#define ClearPageUncached(page)	clear_bit(PG_uncached, &(page)->flags)

#if defined(CONFIG_IA64) || defined(CONFIG_X86_64)
#define PageXpmem(page) test_bit(PG_xpmem, &(page)->flags)
#define SetPageXpmem(page) set_bit(PG_xpmem, &(page)->flags)
#define ClearPageXpmem(page) clear_bit(PG_xpmem, &(page)->flags)
#else
#define PageXpmem(page)	0
#endif

struct page;	/* forward declaration */

int test_clear_page_dirty(struct page *page);
int test_clear_page_writeback(struct page *page);
int test_set_page_writeback(struct page *page);

static inline void clear_page_dirty(struct page *page)
{
	test_clear_page_dirty(page);
}

static inline void set_page_writeback(struct page *page)
{
	test_set_page_writeback(page);
}

/*
 * Filesystem-specific page bit testing
 */
#define PageFsMisc(page)		test_bit(PG_fs_misc, &(page)->flags)
#define SetPageFsMisc(page)		set_bit(PG_fs_misc, &(page)->flags)
#define TestSetPageFsMisc(page)		test_and_set_bit(PG_fs_misc, &(page)->flags)
#define ClearPageFsMisc(page)		clear_bit(PG_fs_misc, &(page)->flags)
#define TestClearPageFsMisc(page)	test_and_clear_bit(PG_fs_misc, &(page)->flags)

#endif	/* PAGE_FLAGS_H */
