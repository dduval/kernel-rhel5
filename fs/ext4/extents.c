/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 *
 * Architecture independence:
 *   Copyright (c) 2005, Bull S.A.
 *   Written by Pierre Peiffer <pierre.peiffer@bull.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */

/*
 * Extents support for EXT4
 *
 * TODO:
 *   - ext4*_error() should be used in some situations
 *   - analyze all BUG()/BUG_ON(), use -EIO where appropriate
 *   - smart tree reduction
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/jbd2.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/falloc.h>
#include <asm/uaccess.h>
#include <linux/fiemap.h>
#include "ext4_jbd2.h"
#include "ext4_extents.h"


/*
 * ext_pblock:
 * combine low and high parts of physical block number into ext4_fsblk_t
 */
ext4_fsblk_t ext_pblock(struct ext4_extent *ex)
{
	ext4_fsblk_t block;

	block = le32_to_cpu(ex->ee_start_lo);
	block |= ((ext4_fsblk_t) le16_to_cpu(ex->ee_start_hi) << 31) << 1;
	return block;
}

/*
 * idx_pblock:
 * combine low and high parts of a leaf physical block number into ext4_fsblk_t
 */
ext4_fsblk_t idx_pblock(struct ext4_extent_idx *ix)
{
	ext4_fsblk_t block;

	block = le32_to_cpu(ix->ei_leaf_lo);
	block |= ((ext4_fsblk_t) le16_to_cpu(ix->ei_leaf_hi) << 31) << 1;
	return block;
}

/*
 * ext4_ext_store_pblock:
 * stores a large physical block number into an extent struct,
 * breaking it into parts
 */
void ext4_ext_store_pblock(struct ext4_extent *ex, ext4_fsblk_t pb)
{
	ex->ee_start_lo = cpu_to_le32((unsigned long) (pb & 0xffffffff));
	ex->ee_start_hi = cpu_to_le16((unsigned long) ((pb >> 31) >> 1) & 0xffff);
}

/*
 * ext4_idx_store_pblock:
 * stores a large physical block number into an index struct,
 * breaking it into parts
 */
static void ext4_idx_store_pblock(struct ext4_extent_idx *ix, ext4_fsblk_t pb)
{
	ix->ei_leaf_lo = cpu_to_le32((unsigned long) (pb & 0xffffffff));
	ix->ei_leaf_hi = cpu_to_le16((unsigned long) ((pb >> 31) >> 1) & 0xffff);
}

static int ext4_ext_truncate_extend_restart(handle_t *handle,
					    struct inode *inode,
					    int needed)
{
	int err;

	if (!ext4_handle_valid(handle))
		return 0;
	if (handle->h_buffer_credits > needed)
		return 0;
	err = ext4_journal_extend(handle, needed);
	if (err <= 0)
		return err;
	err = ext4_truncate_restart_trans(handle, inode, needed);
	/*
	 * We have dropped i_data_sem so someone might have cached again
	 * an extent we are going to truncate.
	 */
	ext4_ext_invalidate_cache(inode);

	return err;
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 */
static int ext4_ext_get_access(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	if (path->p_bh) {
		/* path points to block */
		return ext4_journal_get_write_access(handle, path->p_bh);
	}
	/* path points to leaf/index in inode body */
	/* we use in-core data, no need to protect them */
	return 0;
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 *  - EIO
 */
static int ext4_ext_dirty(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	int err;
	if (path->p_bh) {
		/* path points to block */
		err = ext4_handle_dirty_metadata(handle, inode, path->p_bh);
	} else {
		/* path points to leaf/index in inode body */
		err = ext4_mark_inode_dirty(handle, inode);
	}
	return err;
}

static ext4_fsblk_t ext4_ext_find_goal(struct inode *inode,
			      struct ext4_ext_path *path,
			      ext4_lblk_t block)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	ext4_fsblk_t bg_start;
	ext4_fsblk_t last_block;
	ext4_grpblk_t colour;
	ext4_group_t block_group;
	int flex_size = ext4_flex_bg_size(EXT4_SB(inode->i_sb));
	int depth;

	if (path) {
		struct ext4_extent *ex;
		depth = path->p_depth;

		/* try to predict block placement */
		ex = path[depth].p_ext;
		if (ex)
			return ext_pblock(ex)+(block-le32_to_cpu(ex->ee_block));

		/* it looks like index is empty;
		 * try to find starting block from index itself */
		if (path[depth].p_bh)
			return path[depth].p_bh->b_blocknr;
	}

	/* OK. use inode's group */
	block_group = ei->i_block_group;
	if (flex_size >= EXT4_FLEX_SIZE_DIR_ALLOC_SCHEME) {
		/*
		 * If there are at least EXT4_FLEX_SIZE_DIR_ALLOC_SCHEME
		 * block groups per flexgroup, reserve the first block 
		 * group for directories and special files.  Regular 
		 * files will start at the second block group.  This
		 * tends to speed up directory access and improves 
		 * fsck times.
		 */
		block_group &= ~(flex_size-1);
		if (S_ISREG(inode->i_mode))
			block_group++;
	}
	bg_start = (block_group * EXT4_BLOCKS_PER_GROUP(inode->i_sb)) +
		le32_to_cpu(EXT4_SB(inode->i_sb)->s_es->s_first_data_block);
	last_block = ext4_blocks_count(EXT4_SB(inode->i_sb)->s_es) - 1;

	/*
	 * If we are doing delayed allocation, we don't need take
	 * colour into account.
	 */
	if (test_opt(inode->i_sb, DELALLOC))
		return bg_start;

	if (bg_start + EXT4_BLOCKS_PER_GROUP(inode->i_sb) <= last_block)
		colour = (current->pid % 16) *
			(EXT4_BLOCKS_PER_GROUP(inode->i_sb) / 16);
	else
		colour = (current->pid % 16) * ((last_block - bg_start) / 16);
	return bg_start + colour + block;
}

/*
 * Allocation for a meta data block
 */
static ext4_fsblk_t
ext4_ext_new_meta_block(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path,
			struct ext4_extent *ex, int *err)
{
	ext4_fsblk_t goal, newblock;

	goal = ext4_ext_find_goal(inode, path, le32_to_cpu(ex->ee_block));
	newblock = ext4_new_meta_blocks(handle, inode, goal, NULL, err);
	return newblock;
}

static inline int ext4_ext_space_block(struct inode *inode, int check)
{
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent);
	if (!check) {
#ifdef AGGRESSIVE_TEST
		if (size > 6)
			size = 6;
#endif
	}
	return size;
}

static inline int ext4_ext_space_block_idx(struct inode *inode, int check)
{
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent_idx);
	if (!check) {
#ifdef AGGRESSIVE_TEST
		if (size > 5)
			size = 5;
#endif
	}
	return size;
}

static inline int ext4_ext_space_root(struct inode *inode, int check)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent);
	if (!check) {
#ifdef AGGRESSIVE_TEST
		if (size > 3)
			size = 3;
#endif
	}
	return size;
}

static inline int ext4_ext_space_root_idx(struct inode *inode, int check)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent_idx);
	if (!check) {
#ifdef AGGRESSIVE_TEST
		if (size > 4)
			size = 4;
#endif
	}
	return size;
}

/*
 * Calculate the number of metadata blocks needed
 * to allocate @blocks
 * Worse case is one block per extent
 */
int ext4_ext_calc_metadata_amount(struct inode *inode, int blocks)
{
	int lcap, icap, rcap, leafs, idxs, num;
	int newextents = blocks;

	rcap = ext4_ext_space_root_idx(inode, 0);
	lcap = ext4_ext_space_block(inode, 0);
	icap = ext4_ext_space_block_idx(inode, 0);

	/* number of new leaf blocks needed */
	num = leafs = (newextents + lcap - 1) / lcap;

	/*
	 * Worse case, we need separate index block(s)
	 * to link all new leaf blocks
	 */
	idxs = (leafs + icap - 1) / icap;
	do {
		num += idxs;
		idxs = (idxs + icap - 1) / icap;
	} while (idxs > rcap);

	return num;
}

static int
ext4_ext_max_entries(struct inode *inode, int depth)
{
	int max;

	if (depth == ext_depth(inode)) {
		if (depth == 0)
			max = ext4_ext_space_root(inode, 1);
		else
			max = ext4_ext_space_root_idx(inode, 1);
	} else {
		if (depth == 0)
			max = ext4_ext_space_block(inode, 1);
		else
			max = ext4_ext_space_block_idx(inode, 1);
	}

	return max;
}

static int ext4_valid_extent(struct inode *inode, struct ext4_extent *ext)
{
	ext4_fsblk_t block = ext_pblock(ext);
	int len = ext4_ext_get_actual_len(ext);

	return ext4_data_block_valid(EXT4_SB(inode->i_sb), block, len);
}

static int ext4_valid_extent_idx(struct inode *inode,
				struct ext4_extent_idx *ext_idx)
{
	ext4_fsblk_t block = idx_pblock(ext_idx);

	return ext4_data_block_valid(EXT4_SB(inode->i_sb), block, 1);
}

static int ext4_valid_extent_entries(struct inode *inode,
				struct ext4_extent_header *eh,
				int depth)
{
	struct ext4_extent *ext;
	struct ext4_extent_idx *ext_idx;
	unsigned short entries;
	if (eh->eh_entries == 0)
		return 1;

	entries = le16_to_cpu(eh->eh_entries);

	if (depth == 0) {
		/* leaf entries */
		ext = EXT_FIRST_EXTENT(eh);
		while (entries) {
			if (!ext4_valid_extent(inode, ext))
				return 0;
			ext++;
			entries--;
		}
	} else {
		ext_idx = EXT_FIRST_INDEX(eh);
		while (entries) {
			if (!ext4_valid_extent_idx(inode, ext_idx))
				return 0;
			ext_idx++;
			entries--;
		}
	}
	return 1;
}

static int __ext4_ext_check(const char *function, struct inode *inode,
					struct ext4_extent_header *eh,
					int depth)
{
	const char *error_msg;
	int max = 0;

	if (unlikely(eh->eh_magic != EXT4_EXT_MAGIC)) {
		error_msg = "invalid magic";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_depth) != depth)) {
		error_msg = "unexpected eh_depth";
		goto corrupted;
	}
	if (unlikely(eh->eh_max == 0)) {
		error_msg = "invalid eh_max";
		goto corrupted;
	}
	max = ext4_ext_max_entries(inode, depth);
	if (unlikely(le16_to_cpu(eh->eh_max) > max)) {
		error_msg = "too large eh_max";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_entries) > le16_to_cpu(eh->eh_max))) {
		error_msg = "invalid eh_entries";
		goto corrupted;
	}
	if (!ext4_valid_extent_entries(inode, eh, depth)) {
		error_msg = "invalid extent entries";
		goto corrupted;
	}
	return 0;

corrupted:
	ext4_error(inode->i_sb, function,
			"bad header/extent in inode #%lu: %s - magic %x, "
			"entries %u, max %u(%u), depth %u(%u)",
			inode->i_ino, error_msg, le16_to_cpu(eh->eh_magic),
			le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max),
			max, le16_to_cpu(eh->eh_depth), depth);

	return -EIO;
}

#define ext4_ext_check(inode, eh, depth)	\
	__ext4_ext_check(__func__, inode, eh, depth)

int ext4_ext_check_inode(struct inode *inode)
{
	return ext4_ext_check(inode, ext_inode_hdr(inode), ext_depth(inode));
}

#ifdef EXT_DEBUG
static void ext4_ext_show_path(struct inode *inode, struct ext4_ext_path *path)
{
	int k, l = path->p_depth;

	ext_debug("path:");
	for (k = 0; k <= l; k++, path++) {
		if (path->p_idx) {
		  ext_debug("  %d->%llu", le32_to_cpu(path->p_idx->ei_block),
			    idx_pblock(path->p_idx));
		} else if (path->p_ext) {
			ext_debug("  %d:[%d]%d:%llu ",
				  le32_to_cpu(path->p_ext->ee_block),
				  ext4_ext_is_uninitialized(path->p_ext),
				  ext4_ext_get_actual_len(path->p_ext),
				  ext_pblock(path->p_ext));
		} else
			ext_debug("  []");
	}
	ext_debug("\n");
}

static void ext4_ext_show_leaf(struct inode *inode, struct ext4_ext_path *path)
{
	int depth = ext_depth(inode);
	struct ext4_extent_header *eh;
	struct ext4_extent *ex;
	int i;

	if (!path)
		return;

	eh = path[depth].p_hdr;
	ex = EXT_FIRST_EXTENT(eh);

	ext_debug("Displaying leaf extents for inode %lu\n", inode->i_ino);

	for (i = 0; i < le16_to_cpu(eh->eh_entries); i++, ex++) {
		ext_debug("%d:[%d]%d:%llu ", le32_to_cpu(ex->ee_block),
			  ext4_ext_is_uninitialized(ex),
			  ext4_ext_get_actual_len(ex), ext_pblock(ex));
	}
	ext_debug("\n");
}
#else
#define ext4_ext_show_path(inode, path)
#define ext4_ext_show_leaf(inode, path)
#endif

void ext4_ext_drop_refs(struct ext4_ext_path *path)
{
	int depth = path->p_depth;
	int i;

	for (i = 0; i <= depth; i++, path++)
		if (path->p_bh) {
			brelse(path->p_bh);
			path->p_bh = NULL;
		}
}

/*
 * ext4_ext_binsearch_idx:
 * binary search for the closest index of the given block
 * the header must be checked before calling this
 */
static void
ext4_ext_binsearch_idx(struct inode *inode,
			struct ext4_ext_path *path, ext4_lblk_t block)
{
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent_idx *r, *l, *m;


	ext_debug("binsearch for %u(idx):  ", block);

	l = EXT_FIRST_INDEX(eh) + 1;
	r = EXT_LAST_INDEX(eh);
	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < le32_to_cpu(m->ei_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, le32_to_cpu(l->ei_block),
				m, le32_to_cpu(m->ei_block),
				r, le32_to_cpu(r->ei_block));
	}

	path->p_idx = l - 1;
	ext_debug("  -> %d->%lld ", le32_to_cpu(path->p_idx->ei_block),
		  idx_pblock(path->p_idx));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent_idx *chix, *ix;
		int k;

		chix = ix = EXT_FIRST_INDEX(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ix++) {
		  if (k != 0 &&
		      le32_to_cpu(ix->ei_block) <= le32_to_cpu(ix[-1].ei_block)) {
				printk(KERN_DEBUG "k=%d, ix=0x%p, "
				       "first=0x%p\n", k,
				       ix, EXT_FIRST_INDEX(eh));
				printk(KERN_DEBUG "%u <= %u\n",
				       le32_to_cpu(ix->ei_block),
				       le32_to_cpu(ix[-1].ei_block));
			}
			BUG_ON(k && le32_to_cpu(ix->ei_block)
					   <= le32_to_cpu(ix[-1].ei_block));
			if (block < le32_to_cpu(ix->ei_block))
				break;
			chix = ix;
		}
		BUG_ON(chix != path->p_idx);
	}
#endif

}

/*
 * ext4_ext_binsearch:
 * binary search for closest extent of the given block
 * the header must be checked before calling this
 */
static void
ext4_ext_binsearch(struct inode *inode,
		struct ext4_ext_path *path, ext4_lblk_t block)
{
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent *r, *l, *m;

	if (eh->eh_entries == 0) {
		/*
		 * this leaf is empty:
		 * we get such a leaf in split/add case
		 */
		return;
	}

	ext_debug("binsearch for %u:  ", block);

	l = EXT_FIRST_EXTENT(eh) + 1;
	r = EXT_LAST_EXTENT(eh);

	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < le32_to_cpu(m->ee_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, le32_to_cpu(l->ee_block),
				m, le32_to_cpu(m->ee_block),
				r, le32_to_cpu(r->ee_block));
	}

	path->p_ext = l - 1;
	ext_debug("  -> %d:%llu:[%d]%d ",
			le32_to_cpu(path->p_ext->ee_block),
			ext_pblock(path->p_ext),
			ext4_ext_is_uninitialized(path->p_ext),
			ext4_ext_get_actual_len(path->p_ext));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent *chex, *ex;
		int k;

		chex = ex = EXT_FIRST_EXTENT(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ex++) {
			BUG_ON(k && le32_to_cpu(ex->ee_block)
					  <= le32_to_cpu(ex[-1].ee_block));
			if (block < le32_to_cpu(ex->ee_block))
				break;
			chex = ex;
		}
		BUG_ON(chex != path->p_ext);
	}
#endif

}

int ext4_ext_tree_init(handle_t *handle, struct inode *inode)
{
	struct ext4_extent_header *eh;

	eh = ext_inode_hdr(inode);
	eh->eh_depth = 0;
	eh->eh_entries = 0;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = cpu_to_le16(ext4_ext_space_root(inode, 0));
	ext4_mark_inode_dirty(handle, inode);
	ext4_ext_invalidate_cache(inode);
	return 0;
}

struct ext4_ext_path *
ext4_ext_find_extent(struct inode *inode, ext4_lblk_t block,
					struct ext4_ext_path *path)
{
	struct ext4_extent_header *eh;
	struct buffer_head *bh;
	short int depth, i, ppos = 0, alloc = 0;

	eh = ext_inode_hdr(inode);
	depth = ext_depth(inode);

	/* account possible depth increase */
	if (!path) {
		path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 2),
				GFP_NOFS);
		if (!path)
			return ERR_PTR(-ENOMEM);
		alloc = 1;
	}
	path[0].p_hdr = eh;
	path[0].p_bh = NULL;

	i = depth;
	/* walk through the tree */
	while (i) {
		int need_to_validate = 0;

		ext_debug("depth %d: num %d, max %d\n",
			  ppos, le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));

		ext4_ext_binsearch_idx(inode, path + ppos, block);
		path[ppos].p_block = idx_pblock(path[ppos].p_idx);
		path[ppos].p_depth = i;
		path[ppos].p_ext = NULL;

		bh = sb_getblk(inode->i_sb, path[ppos].p_block);
		if (unlikely(!bh))
			goto err;
		if (!bh_uptodate_or_lock(bh)) {
			if (bh_submit_read(bh) < 0) {
				put_bh(bh);
				goto err;
			}
			/* validate the extent entries */
			need_to_validate = 1;
		}
		eh = ext_block_hdr(bh);
		ppos++;
		BUG_ON(ppos > depth);
		path[ppos].p_bh = bh;
		path[ppos].p_hdr = eh;
		i--;

		if (need_to_validate && ext4_ext_check(inode, eh, i))
			goto err;
	}

	path[ppos].p_depth = i;
	path[ppos].p_ext = NULL;
	path[ppos].p_idx = NULL;

	/* find extent */
	ext4_ext_binsearch(inode, path + ppos, block);
	/* if not an empty leaf */
	if (path[ppos].p_ext)
		path[ppos].p_block = ext_pblock(path[ppos].p_ext);

	ext4_ext_show_path(inode, path);

	return path;

err:
	ext4_ext_drop_refs(path);
	if (alloc)
		kfree(path);
	return ERR_PTR(-EIO);
}

/*
 * ext4_ext_insert_index:
 * insert new index [@logical;@ptr] into the block at @curp;
 * check where to insert: before @curp or after @curp
 */
int ext4_ext_insert_index(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *curp,
				int logical, ext4_fsblk_t ptr)
{
	struct ext4_extent_idx *ix;
	int len, err;

	err = ext4_ext_get_access(handle, inode, curp);
	if (err)
		return err;

	BUG_ON(logical == le32_to_cpu(curp->p_idx->ei_block));
	len = EXT_MAX_INDEX(curp->p_hdr) - curp->p_idx;
	if (logical > le32_to_cpu(curp->p_idx->ei_block)) {
		/* insert after */
		if (curp->p_idx != EXT_LAST_INDEX(curp->p_hdr)) {
			len = (len - 1) * sizeof(struct ext4_extent_idx);
			len = len < 0 ? 0 : len;
			ext_debug("insert new index %d after: %llu. "
					"move %d from 0x%p to 0x%p\n",
					logical, ptr, len,
					(curp->p_idx + 1), (curp->p_idx + 2));
			memmove(curp->p_idx + 2, curp->p_idx + 1, len);
		}
		ix = curp->p_idx + 1;
	} else {
		/* insert before */
		len = len * sizeof(struct ext4_extent_idx);
		len = len < 0 ? 0 : len;
		ext_debug("insert new index %d before: %llu. "
				"move %d from 0x%p to 0x%p\n",
				logical, ptr, len,
				curp->p_idx, (curp->p_idx + 1));
		memmove(curp->p_idx + 1, curp->p_idx, len);
		ix = curp->p_idx;
	}

	ix->ei_block = cpu_to_le32(logical);
	ext4_idx_store_pblock(ix, ptr);
	le16_add_cpu(&curp->p_hdr->eh_entries, 1);

	BUG_ON(le16_to_cpu(curp->p_hdr->eh_entries)
			     > le16_to_cpu(curp->p_hdr->eh_max));
	BUG_ON(ix > EXT_LAST_INDEX(curp->p_hdr));

	err = ext4_ext_dirty(handle, inode, curp);
	ext4_std_error(inode->i_sb, err);

	return err;
}

/*
 * ext4_ext_split:
 * inserts new subtree into the path, using free index entry
 * at depth @at:
 * - allocates all needed blocks (new leaf and all intermediate index blocks)
 * - makes decision where to split
 * - moves remaining extents and index entries (right to the split point)
 *   into the newly allocated blocks
 * - initializes subtree
 */
static int ext4_ext_split(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_extent *newext, int at)
{
	struct buffer_head *bh = NULL;
	int depth = ext_depth(inode);
	struct ext4_extent_header *neh;
	struct ext4_extent_idx *fidx;
	struct ext4_extent *ex;
	int i = at, k, m, a;
	ext4_fsblk_t newblock, oldblock;
	__le32 border;
	ext4_fsblk_t *ablocks = NULL; /* array of allocated blocks */
	int err = 0;

	/* make decision: where to split? */
	/* FIXME: now decision is simplest: at current extent */

	/* if current leaf will be split, then we should use
	 * border from split point */
	BUG_ON(path[depth].p_ext > EXT_MAX_EXTENT(path[depth].p_hdr));
	if (path[depth].p_ext != EXT_MAX_EXTENT(path[depth].p_hdr)) {
		border = path[depth].p_ext[1].ee_block;
		ext_debug("leaf will be split."
				" next leaf starts at %d\n",
				  le32_to_cpu(border));
	} else {
		border = newext->ee_block;
		ext_debug("leaf will be added."
				" next leaf starts at %d\n",
				le32_to_cpu(border));
	}

	/*
	 * If error occurs, then we break processing
	 * and mark filesystem read-only. index won't
	 * be inserted and tree will be in consistent
	 * state. Next mount will repair buffers too.
	 */

	/*
	 * Get array to track all allocated blocks.
	 * We need this to handle errors and free blocks
	 * upon them.
	 */
	ablocks = kzalloc(sizeof(ext4_fsblk_t) * depth, GFP_NOFS);
	if (!ablocks)
		return -ENOMEM;

	/* allocate all needed blocks */
	ext_debug("allocate %d blocks for indexes/leaf\n", depth - at);
	for (a = 0; a < depth - at; a++) {
		newblock = ext4_ext_new_meta_block(handle, inode, path,
						   newext, &err);
		if (newblock == 0)
			goto cleanup;
		ablocks[a] = newblock;
	}

	/* initialize new leaf */
	newblock = ablocks[--a];
	BUG_ON(newblock == 0);
	bh = sb_getblk(inode->i_sb, newblock);
	if (!bh) {
		err = -EIO;
		goto cleanup;
	}
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, bh);
	if (err)
		goto cleanup;

	neh = ext_block_hdr(bh);
	neh->eh_entries = 0;
	neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode, 0));
	neh->eh_magic = EXT4_EXT_MAGIC;
	neh->eh_depth = 0;
	ex = EXT_FIRST_EXTENT(neh);

	/* move remainder of path[depth] to the new leaf */
	BUG_ON(path[depth].p_hdr->eh_entries != path[depth].p_hdr->eh_max);
	/* start copy from next extent */
	/* TODO: we could do it by single memmove */
	m = 0;
	path[depth].p_ext++;
	while (path[depth].p_ext <=
			EXT_MAX_EXTENT(path[depth].p_hdr)) {
		ext_debug("move %d:%llu:[%d]%d in new leaf %llu\n",
				le32_to_cpu(path[depth].p_ext->ee_block),
				ext_pblock(path[depth].p_ext),
				ext4_ext_is_uninitialized(path[depth].p_ext),
				ext4_ext_get_actual_len(path[depth].p_ext),
				newblock);
		/*memmove(ex++, path[depth].p_ext++,
				sizeof(struct ext4_extent));
		neh->eh_entries++;*/
		path[depth].p_ext++;
		m++;
	}
	if (m) {
		memmove(ex, path[depth].p_ext-m, sizeof(struct ext4_extent)*m);
		le16_add_cpu(&neh->eh_entries, m);
	}

	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_handle_dirty_metadata(handle, inode, bh);
	if (err)
		goto cleanup;
	brelse(bh);
	bh = NULL;

	/* correct old leaf */
	if (m) {
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto cleanup;
		le16_add_cpu(&path[depth].p_hdr->eh_entries, -m);
		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto cleanup;

	}

	/* create intermediate indexes */
	k = depth - at - 1;
	BUG_ON(k < 0);
	if (k)
		ext_debug("create %d intermediate indices\n", k);
	/* insert new index into current index block */
	/* current depth stored in i var */
	i = depth - 1;
	while (k--) {
		oldblock = newblock;
		newblock = ablocks[--a];
		bh = sb_getblk(inode->i_sb, newblock);
		if (!bh) {
			err = -EIO;
			goto cleanup;
		}
		lock_buffer(bh);

		err = ext4_journal_get_create_access(handle, bh);
		if (err)
			goto cleanup;

		neh = ext_block_hdr(bh);
		neh->eh_entries = cpu_to_le16(1);
		neh->eh_magic = EXT4_EXT_MAGIC;
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode, 0));
		neh->eh_depth = cpu_to_le16(depth - i);
		fidx = EXT_FIRST_INDEX(neh);
		fidx->ei_block = border;
		ext4_idx_store_pblock(fidx, oldblock);

		ext_debug("int.index at %d (block %llu): %u -> %llu\n",
				i, newblock, le32_to_cpu(border), oldblock);
		/* copy indexes */
		m = 0;
		path[i].p_idx++;

		ext_debug("cur 0x%p, last 0x%p\n", path[i].p_idx,
				EXT_MAX_INDEX(path[i].p_hdr));
		BUG_ON(EXT_MAX_INDEX(path[i].p_hdr) !=
				EXT_LAST_INDEX(path[i].p_hdr));
		while (path[i].p_idx <= EXT_MAX_INDEX(path[i].p_hdr)) {
			ext_debug("%d: move %d:%llu in new index %llu\n", i,
					le32_to_cpu(path[i].p_idx->ei_block),
					idx_pblock(path[i].p_idx),
					newblock);
			/*memmove(++fidx, path[i].p_idx++,
					sizeof(struct ext4_extent_idx));
			neh->eh_entries++;
			BUG_ON(neh->eh_entries > neh->eh_max);*/
			path[i].p_idx++;
			m++;
		}
		if (m) {
			memmove(++fidx, path[i].p_idx - m,
				sizeof(struct ext4_extent_idx) * m);
			le16_add_cpu(&neh->eh_entries, m);
		}
		set_buffer_uptodate(bh);
		unlock_buffer(bh);

		err = ext4_handle_dirty_metadata(handle, inode, bh);
		if (err)
			goto cleanup;
		brelse(bh);
		bh = NULL;

		/* correct old index */
		if (m) {
			err = ext4_ext_get_access(handle, inode, path + i);
			if (err)
				goto cleanup;
			le16_add_cpu(&path[i].p_hdr->eh_entries, -m);
			err = ext4_ext_dirty(handle, inode, path + i);
			if (err)
				goto cleanup;
		}

		i--;
	}

	/* insert new index */
	err = ext4_ext_insert_index(handle, inode, path + at,
				    le32_to_cpu(border), newblock);

cleanup:
	if (bh) {
		if (buffer_locked(bh))
			unlock_buffer(bh);
		brelse(bh);
	}

	if (err) {
		/* free all allocated blocks in error case */
		for (i = 0; i < depth; i++) {
			if (!ablocks[i])
				continue;
			ext4_free_blocks(handle, inode, ablocks[i], 1, 1);
		}
	}
	kfree(ablocks);

	return err;
}

/*
 * ext4_ext_grow_indepth:
 * implements tree growing procedure:
 * - allocates new block
 * - moves top-level data (index block or leaf) into the new block
 * - initializes new top-level, creating index that points to the
 *   just created block
 */
static int ext4_ext_grow_indepth(handle_t *handle, struct inode *inode,
					struct ext4_ext_path *path,
					struct ext4_extent *newext)
{
	struct ext4_ext_path *curp = path;
	struct ext4_extent_header *neh;
	struct ext4_extent_idx *fidx;
	struct buffer_head *bh;
	ext4_fsblk_t newblock;
	int err = 0;

	newblock = ext4_ext_new_meta_block(handle, inode, path, newext, &err);
	if (newblock == 0)
		return err;

	bh = sb_getblk(inode->i_sb, newblock);
	if (!bh) {
		err = -EIO;
		ext4_std_error(inode->i_sb, err);
		return err;
	}
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, bh);
	if (err) {
		unlock_buffer(bh);
		goto out;
	}

	/* move top-level index/leaf into new block */
	memmove(bh->b_data, curp->p_hdr, sizeof(EXT4_I(inode)->i_data));

	/* set size of new block */
	neh = ext_block_hdr(bh);
	/* old root could have indexes or leaves
	 * so calculate e_max right way */
	if (ext_depth(inode))
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode, 0));
	else
		neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode, 0));
	neh->eh_magic = EXT4_EXT_MAGIC;
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_handle_dirty_metadata(handle, inode, bh);
	if (err)
		goto out;

	/* create index in new top-level index: num,max,pointer */
	err = ext4_ext_get_access(handle, inode, curp);
	if (err)
		goto out;

	curp->p_hdr->eh_magic = EXT4_EXT_MAGIC;
	curp->p_hdr->eh_max = cpu_to_le16(ext4_ext_space_root_idx(inode, 0));
	curp->p_hdr->eh_entries = cpu_to_le16(1);
	curp->p_idx = EXT_FIRST_INDEX(curp->p_hdr);

	if (path[0].p_hdr->eh_depth)
		curp->p_idx->ei_block =
			EXT_FIRST_INDEX(path[0].p_hdr)->ei_block;
	else
		curp->p_idx->ei_block =
			EXT_FIRST_EXTENT(path[0].p_hdr)->ee_block;
	ext4_idx_store_pblock(curp->p_idx, newblock);

	neh = ext_inode_hdr(inode);
	fidx = EXT_FIRST_INDEX(neh);
	ext_debug("new root: num %d(%d), lblock %d, ptr %llu\n",
		  le16_to_cpu(neh->eh_entries), le16_to_cpu(neh->eh_max),
		  le32_to_cpu(fidx->ei_block), idx_pblock(fidx));

	neh->eh_depth = cpu_to_le16(path->p_depth + 1);
	err = ext4_ext_dirty(handle, inode, curp);
out:
	brelse(bh);

	return err;
}

/*
 * ext4_ext_create_new_leaf:
 * finds empty index and adds new leaf.
 * if no free index is found, then it requests in-depth growing.
 */
static int ext4_ext_create_new_leaf(handle_t *handle, struct inode *inode,
					struct ext4_ext_path *path,
					struct ext4_extent *newext)
{
	struct ext4_ext_path *curp;
	int depth, i, err = 0;

repeat:
	i = depth = ext_depth(inode);

	/* walk up to the tree and look for free index entry */
	curp = path + depth;
	while (i > 0 && !EXT_HAS_FREE_INDEX(curp)) {
		i--;
		curp--;
	}

	/* we use already allocated block for index block,
	 * so subsequent data blocks should be contiguous */
	if (EXT_HAS_FREE_INDEX(curp)) {
		/* if we found index with free entry, then use that
		 * entry: create all needed subtree and add new leaf */
		err = ext4_ext_split(handle, inode, path, newext, i);
		if (err)
			goto out;

		/* refill path */
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode,
				    (ext4_lblk_t)le32_to_cpu(newext->ee_block),
				    path);
		if (IS_ERR(path))
			err = PTR_ERR(path);
	} else {
		/* tree is full, time to grow in depth */
		err = ext4_ext_grow_indepth(handle, inode, path, newext);
		if (err)
			goto out;

		/* refill path */
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode,
				   (ext4_lblk_t)le32_to_cpu(newext->ee_block),
				    path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto out;
		}

		/*
		 * only first (depth 0 -> 1) produces free space;
		 * in all other cases we have to split the grown tree
		 */
		depth = ext_depth(inode);
		if (path[depth].p_hdr->eh_entries == path[depth].p_hdr->eh_max) {
			/* now we need to split */
			goto repeat;
		}
	}

out:
	return err;
}

/*
 * search the closest allocated block to the left for *logical
 * and returns it at @logical + it's physical address at @phys
 * if *logical is the smallest allocated block, the function
 * returns 0 at @phys
 * return value contains 0 (success) or error code
 */
int
ext4_ext_search_left(struct inode *inode, struct ext4_ext_path *path,
			ext4_lblk_t *logical, ext4_fsblk_t *phys)
{
	struct ext4_extent_idx *ix;
	struct ext4_extent *ex;
	int depth, ee_len;

	BUG_ON(path == NULL);
	depth = path->p_depth;
	*phys = 0;

	if (depth == 0 && path->p_ext == NULL)
		return 0;

	/* usually extent in the path covers blocks smaller
	 * then *logical, but it can be that extent is the
	 * first one in the file */

	ex = path[depth].p_ext;
	ee_len = ext4_ext_get_actual_len(ex);
	if (*logical < le32_to_cpu(ex->ee_block)) {
		BUG_ON(EXT_FIRST_EXTENT(path[depth].p_hdr) != ex);
		while (--depth >= 0) {
			ix = path[depth].p_idx;
			BUG_ON(ix != EXT_FIRST_INDEX(path[depth].p_hdr));
		}
		return 0;
	}

	BUG_ON(*logical < (le32_to_cpu(ex->ee_block) + ee_len));

	*logical = le32_to_cpu(ex->ee_block) + ee_len - 1;
	*phys = ext_pblock(ex) + ee_len - 1;
	return 0;
}

/*
 * search the closest allocated block to the right for *logical
 * and returns it at @logical + it's physical address at @phys
 * if *logical is the smallest allocated block, the function
 * returns 0 at @phys
 * return value contains 0 (success) or error code
 */
int
ext4_ext_search_right(struct inode *inode, struct ext4_ext_path *path,
			ext4_lblk_t *logical, ext4_fsblk_t *phys)
{
	struct buffer_head *bh = NULL;
	struct ext4_extent_header *eh;
	struct ext4_extent_idx *ix;
	struct ext4_extent *ex;
	ext4_fsblk_t block;
	int depth;	/* Note, NOT eh_depth; depth from top of tree */
	int ee_len;

	BUG_ON(path == NULL);
	depth = path->p_depth;
	*phys = 0;

	if (depth == 0 && path->p_ext == NULL)
		return 0;

	/* usually extent in the path covers blocks smaller
	 * then *logical, but it can be that extent is the
	 * first one in the file */

	ex = path[depth].p_ext;
	ee_len = ext4_ext_get_actual_len(ex);
	if (*logical < le32_to_cpu(ex->ee_block)) {
		BUG_ON(EXT_FIRST_EXTENT(path[depth].p_hdr) != ex);
		while (--depth >= 0) {
			ix = path[depth].p_idx;
			BUG_ON(ix != EXT_FIRST_INDEX(path[depth].p_hdr));
		}
		*logical = le32_to_cpu(ex->ee_block);
		*phys = ext_pblock(ex);
		return 0;
	}

	BUG_ON(*logical < (le32_to_cpu(ex->ee_block) + ee_len));

	if (ex != EXT_LAST_EXTENT(path[depth].p_hdr)) {
		/* next allocated block in this leaf */
		ex++;
		*logical = le32_to_cpu(ex->ee_block);
		*phys = ext_pblock(ex);
		return 0;
	}

	/* go up and search for index to the right */
	while (--depth >= 0) {
		ix = path[depth].p_idx;
		if (ix != EXT_LAST_INDEX(path[depth].p_hdr))
			goto got_index;
	}

	/* we've gone up to the root and found no index to the right */
	return 0;

got_index:
	/* we've found index to the right, let's
	 * follow it and find the closest allocated
	 * block to the right */
	ix++;
	block = idx_pblock(ix);
	while (++depth < path->p_depth) {
		bh = sb_bread(inode->i_sb, block);
		if (bh == NULL)
			return -EIO;
		eh = ext_block_hdr(bh);
		/* subtract from p_depth to get proper eh_depth */
		if (ext4_ext_check(inode, eh, path->p_depth - depth)) {
			put_bh(bh);
			return -EIO;
		}
		ix = EXT_FIRST_INDEX(eh);
		block = idx_pblock(ix);
		put_bh(bh);
	}

	bh = sb_bread(inode->i_sb, block);
	if (bh == NULL)
		return -EIO;
	eh = ext_block_hdr(bh);
	if (ext4_ext_check(inode, eh, path->p_depth - depth)) {
		put_bh(bh);
		return -EIO;
	}
	ex = EXT_FIRST_EXTENT(eh);
	*logical = le32_to_cpu(ex->ee_block);
	*phys = ext_pblock(ex);
	put_bh(bh);
	return 0;
}

/*
 * ext4_ext_next_allocated_block:
 * returns allocated block in subsequent extent or EXT_MAX_BLOCK.
 * NOTE: it considers block number from index entry as
 * allocated block. Thus, index entries have to be consistent
 * with leaves.
 */
static ext4_lblk_t
ext4_ext_next_allocated_block(struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

	if (depth == 0 && path->p_ext == NULL)
		return EXT_MAX_BLOCK;

	while (depth >= 0) {
		if (depth == path->p_depth) {
			/* leaf */
			if (path[depth].p_ext !=
					EXT_LAST_EXTENT(path[depth].p_hdr))
			  return le32_to_cpu(path[depth].p_ext[1].ee_block);
		} else {
			/* index */
			if (path[depth].p_idx !=
					EXT_LAST_INDEX(path[depth].p_hdr))
			  return le32_to_cpu(path[depth].p_idx[1].ei_block);
		}
		depth--;
	}

	return EXT_MAX_BLOCK;
}

/*
 * ext4_ext_next_leaf_block:
 * returns first allocated block from next leaf or EXT_MAX_BLOCK
 */
static ext4_lblk_t ext4_ext_next_leaf_block(struct inode *inode,
					struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

	/* zero-tree has no leaf blocks at all */
	if (depth == 0)
		return EXT_MAX_BLOCK;

	/* go to index block */
	depth--;

	while (depth >= 0) {
		if (path[depth].p_idx !=
				EXT_LAST_INDEX(path[depth].p_hdr))
			return (ext4_lblk_t)
				le32_to_cpu(path[depth].p_idx[1].ei_block);
		depth--;
	}

	return EXT_MAX_BLOCK;
}

/*
 * ext4_ext_correct_indexes:
 * if leaf gets modified and modified extent is first in the leaf,
 * then we have to correct all indexes above.
 * TODO: do we need to correct tree in all cases?
 */
static int ext4_ext_correct_indexes(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	struct ext4_extent_header *eh;
	int depth = ext_depth(inode);
	struct ext4_extent *ex;
	__le32 border;
	int k, err = 0;

	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;
	BUG_ON(ex == NULL);
	BUG_ON(eh == NULL);

	if (depth == 0) {
		/* there is no tree at all */
		return 0;
	}

	if (ex != EXT_FIRST_EXTENT(eh)) {
		/* we correct tree if first leaf got modified only */
		return 0;
	}

	/*
	 * TODO: we need correction if border is smaller than current one
	 */
	k = depth - 1;
	border = path[depth].p_ext->ee_block;
	err = ext4_ext_get_access(handle, inode, path + k);
	if (err)
		return err;
	path[k].p_idx->ei_block = border;
	err = ext4_ext_dirty(handle, inode, path + k);
	if (err)
		return err;

	while (k--) {
		/* change all left-side indexes */
		if (path[k+1].p_idx != EXT_FIRST_INDEX(path[k+1].p_hdr))
			break;
		err = ext4_ext_get_access(handle, inode, path + k);
		if (err)
			break;
		path[k].p_idx->ei_block = border;
		err = ext4_ext_dirty(handle, inode, path + k);
		if (err)
			break;
	}

	return err;
}

int
ext4_can_extents_be_merged(struct inode *inode, struct ext4_extent *ex1,
				struct ext4_extent *ex2)
{
	unsigned short ext1_ee_len, ext2_ee_len, max_len;

	/*
	 * Make sure that either both extents are uninitialized, or
	 * both are _not_.
	 */
	if (ext4_ext_is_uninitialized(ex1) ^ ext4_ext_is_uninitialized(ex2))
		return 0;

	if (ext4_ext_is_uninitialized(ex1))
		max_len = EXT_UNINIT_MAX_LEN;
	else
		max_len = EXT_INIT_MAX_LEN;

	ext1_ee_len = ext4_ext_get_actual_len(ex1);
	ext2_ee_len = ext4_ext_get_actual_len(ex2);

	if (le32_to_cpu(ex1->ee_block) + ext1_ee_len !=
			le32_to_cpu(ex2->ee_block))
		return 0;

	/*
	 * To allow future support for preallocated extents to be added
	 * as an RO_COMPAT feature, refuse to merge to extents if
	 * this can result in the top bit of ee_len being set.
	 */
	if (ext1_ee_len + ext2_ee_len > max_len)
		return 0;
#ifdef AGGRESSIVE_TEST
	if (ext1_ee_len >= 4)
		return 0;
#endif

	if (ext_pblock(ex1) + ext1_ee_len == ext_pblock(ex2))
		return 1;
	return 0;
}

/*
 * This function tries to merge the "ex" extent to the next extent in the tree.
 * It always tries to merge towards right. If you want to merge towards
 * left, pass "ex - 1" as argument instead of "ex".
 * Returns 0 if the extents (ex and ex+1) were _not_ merged and returns
 * 1 if they got merged.
 */
int ext4_ext_try_to_merge(struct inode *inode,
			  struct ext4_ext_path *path,
			  struct ext4_extent *ex)
{
	struct ext4_extent_header *eh;
	unsigned int depth, len;
	int merge_done = 0;
	int uninitialized = 0;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

	while (ex < EXT_LAST_EXTENT(eh)) {
		if (!ext4_can_extents_be_merged(inode, ex, ex + 1))
			break;
		/* merge with next extent! */
		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
		ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
				+ ext4_ext_get_actual_len(ex + 1));
		if (uninitialized)
			ext4_ext_mark_uninitialized(ex);

		if (ex + 1 < EXT_LAST_EXTENT(eh)) {
			len = (EXT_LAST_EXTENT(eh) - ex - 1)
				* sizeof(struct ext4_extent);
			memmove(ex + 1, ex + 2, len);
		}
		le16_add_cpu(&eh->eh_entries, -1);
		merge_done = 1;
		WARN_ON(eh->eh_entries == 0);
		if (!eh->eh_entries)
			ext4_error(inode->i_sb, "ext4_ext_try_to_merge",
			   "inode#%lu, eh->eh_entries = 0!", inode->i_ino);
	}

	return merge_done;
}

/*
 * check if a portion of the "newext" extent overlaps with an
 * existing extent.
 *
 * If there is an overlap discovered, it updates the length of the newext
 * such that there will be no overlap, and then returns 1.
 * If there is no overlap found, it returns 0.
 */
unsigned int ext4_ext_check_overlap(struct inode *inode,
				    struct ext4_extent *newext,
				    struct ext4_ext_path *path)
{
	ext4_lblk_t b1, b2;
	unsigned int depth, len1;
	unsigned int ret = 0;

	b1 = le32_to_cpu(newext->ee_block);
	len1 = ext4_ext_get_actual_len(newext);
	depth = ext_depth(inode);
	if (!path[depth].p_ext)
		goto out;
	b2 = le32_to_cpu(path[depth].p_ext->ee_block);

	/*
	 * get the next allocated block if the extent in the path
	 * is before the requested block(s)
	 */
	if (b2 < b1) {
		b2 = ext4_ext_next_allocated_block(path);
		if (b2 == EXT_MAX_BLOCK)
			goto out;
	}

	/* check for wrap through zero on extent logical start block*/
	if (b1 + len1 < b1) {
		len1 = EXT_MAX_BLOCK - b1;
		newext->ee_len = cpu_to_le16(len1);
		ret = 1;
	}

	/* check for overlap */
	if (b1 + len1 > b2) {
		newext->ee_len = cpu_to_le16(b2 - b1);
		ret = 1;
	}
out:
	return ret;
}

/*
 * ext4_ext_insert_extent:
 * tries to merge requsted extent into the existing extent or
 * inserts requested extent as new one into the tree,
 * creating new leaf in the no-space case.
 */
int ext4_ext_insert_extent(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_extent *newext, int flag)
{
	struct ext4_extent_header *eh;
	struct ext4_extent *ex, *fex;
	struct ext4_extent *nearex; /* nearest extent */
	struct ext4_ext_path *npath = NULL;
	int depth, len, err;
	ext4_lblk_t next;
	unsigned uninitialized = 0;

	BUG_ON(ext4_ext_get_actual_len(newext) == 0);
	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	BUG_ON(path[depth].p_hdr == NULL);

	/* try to insert block into found extent and return */
	if (ex && (flag != EXT4_GET_BLOCKS_DIO_CREATE_EXT)
		&& ext4_can_extents_be_merged(inode, ex, newext)) {
		ext_debug("append [%d]%d block to %d:[%d]%d (from %llu)\n",
				ext4_ext_is_uninitialized(newext),
				ext4_ext_get_actual_len(newext),
				le32_to_cpu(ex->ee_block),
				ext4_ext_is_uninitialized(ex),
				ext4_ext_get_actual_len(ex), ext_pblock(ex));
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			return err;

		/*
		 * ext4_can_extents_be_merged should have checked that either
		 * both extents are uninitialized, or both aren't. Thus we
		 * need to check only one of them here.
		 */
		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
		ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
					+ ext4_ext_get_actual_len(newext));
		if (uninitialized)
			ext4_ext_mark_uninitialized(ex);
		eh = path[depth].p_hdr;
		nearex = ex;
		goto merge;
	}

repeat:
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))
		goto has_space;

	/* probably next leaf has space for us? */
	fex = EXT_LAST_EXTENT(eh);
	next = ext4_ext_next_leaf_block(inode, path);
	if (le32_to_cpu(newext->ee_block) > le32_to_cpu(fex->ee_block)
	    && next != EXT_MAX_BLOCK) {
		ext_debug("next leaf block - %d\n", next);
		BUG_ON(npath != NULL);
		npath = ext4_ext_find_extent(inode, next, NULL);
		if (IS_ERR(npath))
			return PTR_ERR(npath);
		BUG_ON(npath->p_depth != path->p_depth);
		eh = npath[depth].p_hdr;
		if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max)) {
			ext_debug("next leaf isnt full(%d)\n",
				  le16_to_cpu(eh->eh_entries));
			path = npath;
			goto repeat;
		}
		ext_debug("next leaf has no free space(%d,%d)\n",
			  le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));
	}

	/*
	 * There is no free space in the found leaf.
	 * We're gonna add a new leaf in the tree.
	 */
	err = ext4_ext_create_new_leaf(handle, inode, path, newext);
	if (err)
		goto cleanup;
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;

has_space:
	nearex = path[depth].p_ext;

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto cleanup;

	if (!nearex) {
		/* there is no extent in this leaf, create first one */
		ext_debug("first extent in the leaf: %d:%llu:[%d]%d\n",
				le32_to_cpu(newext->ee_block),
				ext_pblock(newext),
				ext4_ext_is_uninitialized(newext),
				ext4_ext_get_actual_len(newext));
		path[depth].p_ext = EXT_FIRST_EXTENT(eh);
	} else if (le32_to_cpu(newext->ee_block)
			   > le32_to_cpu(nearex->ee_block)) {
/*		BUG_ON(newext->ee_block == nearex->ee_block); */
		if (nearex != EXT_LAST_EXTENT(eh)) {
			len = EXT_MAX_EXTENT(eh) - nearex;
			len = (len - 1) * sizeof(struct ext4_extent);
			len = len < 0 ? 0 : len;
			ext_debug("insert %d:%llu:[%d]%d after: nearest 0x%p, "
					"move %d from 0x%p to 0x%p\n",
					le32_to_cpu(newext->ee_block),
					ext_pblock(newext),
					ext4_ext_is_uninitialized(newext),
					ext4_ext_get_actual_len(newext),
					nearex, len, nearex + 1, nearex + 2);
			memmove(nearex + 2, nearex + 1, len);
		}
		path[depth].p_ext = nearex + 1;
	} else {
		BUG_ON(newext->ee_block == nearex->ee_block);
		len = (EXT_MAX_EXTENT(eh) - nearex) * sizeof(struct ext4_extent);
		len = len < 0 ? 0 : len;
		ext_debug("insert %d:%llu:[%d]%d before: nearest 0x%p, "
				"move %d from 0x%p to 0x%p\n",
				le32_to_cpu(newext->ee_block),
				ext_pblock(newext),
				ext4_ext_is_uninitialized(newext),
				ext4_ext_get_actual_len(newext),
				nearex, len, nearex + 1, nearex + 2);
		memmove(nearex + 1, nearex, len);
		path[depth].p_ext = nearex;
	}

	le16_add_cpu(&eh->eh_entries, 1);
	nearex = path[depth].p_ext;
	nearex->ee_block = newext->ee_block;
	ext4_ext_store_pblock(nearex, ext_pblock(newext));
	nearex->ee_len = newext->ee_len;

merge:
	/* try to merge extents to the right */
	if (flag != EXT4_GET_BLOCKS_DIO_CREATE_EXT)
		ext4_ext_try_to_merge(inode, path, nearex);

	/* try to merge extents to the left */

	/* time to correct all indexes above */
	err = ext4_ext_correct_indexes(handle, inode, path);
	if (err)
		goto cleanup;

	err = ext4_ext_dirty(handle, inode, path + depth);

cleanup:
	if (npath) {
		ext4_ext_drop_refs(npath);
		kfree(npath);
	}
	ext4_ext_invalidate_cache(inode);
	return err;
}

int ext4_ext_walk_space(struct inode *inode, ext4_lblk_t block,
			ext4_lblk_t num, ext_prepare_callback func,
			void *cbdata)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_ext_cache cbex;
	struct ext4_extent *ex;
	ext4_lblk_t next, start = 0, end = 0;
	ext4_lblk_t last = block + num;
	int depth, exists, err = 0;

	BUG_ON(func == NULL);
	BUG_ON(inode == NULL);

	while (block < last && block != EXT_MAX_BLOCK) {
		num = last - block;
		/* find extent for this block */
		path = ext4_ext_find_extent(inode, block, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			path = NULL;
			break;
		}

		depth = ext_depth(inode);
		BUG_ON(path[depth].p_hdr == NULL);
		ex = path[depth].p_ext;
		next = ext4_ext_next_allocated_block(path);

		exists = 0;
		if (!ex) {
			/* there is no extent yet, so try to allocate
			 * all requested space */
			start = block;
			end = block + num;
		} else if (le32_to_cpu(ex->ee_block) > block) {
			/* need to allocate space before found extent */
			start = block;
			end = le32_to_cpu(ex->ee_block);
			if (block + num < end)
				end = block + num;
		} else if (block >= le32_to_cpu(ex->ee_block)
					+ ext4_ext_get_actual_len(ex)) {
			/* need to allocate space after found extent */
			start = block;
			end = block + num;
			if (end >= next)
				end = next;
		} else if (block >= le32_to_cpu(ex->ee_block)) {
			/*
			 * some part of requested space is covered
			 * by found extent
			 */
			start = block;
			end = le32_to_cpu(ex->ee_block)
				+ ext4_ext_get_actual_len(ex);
			if (block + num < end)
				end = block + num;
			exists = 1;
		} else {
			BUG();
		}
		BUG_ON(end <= start);

		if (!exists) {
			cbex.ec_block = start;
			cbex.ec_len = end - start;
			cbex.ec_start = 0;
			cbex.ec_type = EXT4_EXT_CACHE_GAP;
		} else {
			cbex.ec_block = le32_to_cpu(ex->ee_block);
			cbex.ec_len = ext4_ext_get_actual_len(ex);
			cbex.ec_start = ext_pblock(ex);
			cbex.ec_type = EXT4_EXT_CACHE_EXTENT;
		}

		BUG_ON(cbex.ec_len == 0);
		err = func(inode, path, &cbex, ex, cbdata);
		ext4_ext_drop_refs(path);

		if (err < 0)
			break;

		if (err == EXT_REPEAT)
			continue;
		else if (err == EXT_BREAK) {
			err = 0;
			break;
		}

		if (ext_depth(inode) != depth) {
			/* depth was changed. we have to realloc path */
			kfree(path);
			path = NULL;
		}

		block = cbex.ec_block + cbex.ec_len;
	}

	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}

	return err;
}

static void
ext4_ext_put_in_cache(struct inode *inode, ext4_lblk_t block,
			__u32 len, ext4_fsblk_t start, int type)
{
	struct ext4_ext_cache *cex;
	BUG_ON(len == 0);
	spin_lock(&EXT4_I(inode)->i_block_reservation_lock);
	cex = &EXT4_I(inode)->i_cached_extent;
	cex->ec_type = type;
	cex->ec_block = block;
	cex->ec_len = len;
	cex->ec_start = start;
	spin_unlock(&EXT4_I(inode)->i_block_reservation_lock);
}

/*
 * ext4_ext_put_gap_in_cache:
 * calculate boundaries of the gap that the requested block fits into
 * and cache this gap
 */
static void
ext4_ext_put_gap_in_cache(struct inode *inode, struct ext4_ext_path *path,
				ext4_lblk_t block)
{
	int depth = ext_depth(inode);
	unsigned long len;
	ext4_lblk_t lblock;
	struct ext4_extent *ex;

	ex = path[depth].p_ext;
	if (ex == NULL) {
		/* there is no extent yet, so gap is [0;-] */
		lblock = 0;
		len = EXT_MAX_BLOCK;
		ext_debug("cache gap(whole file):");
	} else if (block < le32_to_cpu(ex->ee_block)) {
		lblock = block;
		len = le32_to_cpu(ex->ee_block) - block;
		ext_debug("cache gap(before): %u [%u:%u]",
				block,
				le32_to_cpu(ex->ee_block),
				 ext4_ext_get_actual_len(ex));
	} else if (block >= le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex)) {
		ext4_lblk_t next;
		lblock = le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex);

		next = ext4_ext_next_allocated_block(path);
		ext_debug("cache gap(after): [%u:%u] %u",
				le32_to_cpu(ex->ee_block),
				ext4_ext_get_actual_len(ex),
				block);
		BUG_ON(next == lblock);
		len = next - lblock;
	} else {
		lblock = len = 0;
		BUG();
	}

	ext_debug(" -> %u:%lu\n", lblock, len);
	ext4_ext_put_in_cache(inode, lblock, len, 0, EXT4_EXT_CACHE_GAP);
}

static int
ext4_ext_in_cache(struct inode *inode, ext4_lblk_t block,
			struct ext4_extent *ex)
{
	struct ext4_ext_cache *cex;
	int ret = EXT4_EXT_CACHE_NO;

	/* 
	 * We borrow i_block_reservation_lock to protect i_cached_extent
	 */
	spin_lock(&EXT4_I(inode)->i_block_reservation_lock);
	cex = &EXT4_I(inode)->i_cached_extent;

	/* has cache valid data? */
	if (cex->ec_type == EXT4_EXT_CACHE_NO)
		goto errout;

	BUG_ON(cex->ec_type != EXT4_EXT_CACHE_GAP &&
			cex->ec_type != EXT4_EXT_CACHE_EXTENT);
	if (in_range(block, cex->ec_block, cex->ec_len)) {
		ex->ee_block = cpu_to_le32(cex->ec_block);
		ext4_ext_store_pblock(ex, cex->ec_start);
		ex->ee_len = cpu_to_le16(cex->ec_len);
		ext_debug("%u cached by %u:%u:%llu\n",
				block,
				cex->ec_block, cex->ec_len, cex->ec_start);
		ret = cex->ec_type;
	}
errout:
	spin_unlock(&EXT4_I(inode)->i_block_reservation_lock);
	return ret;
}

/*
 * ext4_ext_rm_idx:
 * removes index from the index block.
 * It's used in truncate case only, thus all requests are for
 * last index in the block only.
 */
static int ext4_ext_rm_idx(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path)
{
	struct buffer_head *bh;
	int err;
	ext4_fsblk_t leaf;

	/* free index block */
	path--;
	leaf = idx_pblock(path->p_idx);
	BUG_ON(path->p_hdr->eh_entries == 0);
	err = ext4_ext_get_access(handle, inode, path);
	if (err)
		return err;
	le16_add_cpu(&path->p_hdr->eh_entries, -1);
	err = ext4_ext_dirty(handle, inode, path);
	if (err)
		return err;
	ext_debug("index is empty, remove it, free block %llu\n", leaf);
	bh = sb_find_get_block(inode->i_sb, leaf);
	ext4_forget(handle, 1, inode, bh, leaf);
	ext4_free_blocks(handle, inode, leaf, 1, 1);
	return err;
}

/*
 * ext4_ext_calc_credits_for_single_extent:
 * This routine returns max. credits that needed to insert an extent
 * to the extent tree.
 * When pass the actual path, the caller should calculate credits
 * under i_data_sem.
 */
int ext4_ext_calc_credits_for_single_extent(struct inode *inode, int nrblocks,
						struct ext4_ext_path *path)
{
	if (path) {
		int depth = ext_depth(inode);
		int ret = 0;

		/* probably there is space in leaf? */
		if (le16_to_cpu(path[depth].p_hdr->eh_entries)
				< le16_to_cpu(path[depth].p_hdr->eh_max)) {

			/*
			 *  There are some space in the leaf tree, no
			 *  need to account for leaf block credit
			 *
			 *  bitmaps and block group descriptor blocks
			 *  and other metadat blocks still need to be
			 *  accounted.
			 */
			/* 1 bitmap, 1 block group descriptor */
			ret = 2 + EXT4_META_TRANS_BLOCKS(inode->i_sb);
			return ret;
		}
	}

	return ext4_chunk_trans_blocks(inode, nrblocks);
}

/*
 * How many index/leaf blocks need to change/allocate to modify nrblocks?
 *
 * if nrblocks are fit in a single extent (chunk flag is 1), then
 * in the worse case, each tree level index/leaf need to be changed
 * if the tree split due to insert a new extent, then the old tree
 * index/leaf need to be updated too
 *
 * If the nrblocks are discontiguous, they could cause
 * the whole tree split more than once, but this is really rare.
 */
int ext4_ext_index_trans_blocks(struct inode *inode, int nrblocks, int chunk)
{
	int index;
	int depth = ext_depth(inode);

	if (chunk)
		index = depth * 2;
	else
		index = depth * 3;

	return index;
}

static int ext4_remove_blocks(handle_t *handle, struct inode *inode,
				struct ext4_extent *ex,
				ext4_lblk_t from, ext4_lblk_t to)
{
	struct buffer_head *bh;
	unsigned short ee_len =  ext4_ext_get_actual_len(ex);
	int i, metadata = 0;

	if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))
		metadata = 1;
#ifdef EXTENTS_STATS
	{
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
		spin_lock(&sbi->s_ext_stats_lock);
		sbi->s_ext_blocks += ee_len;
		sbi->s_ext_extents++;
		if (ee_len < sbi->s_ext_min)
			sbi->s_ext_min = ee_len;
		if (ee_len > sbi->s_ext_max)
			sbi->s_ext_max = ee_len;
		if (ext_depth(inode) > sbi->s_depth_max)
			sbi->s_depth_max = ext_depth(inode);
		spin_unlock(&sbi->s_ext_stats_lock);
	}
#endif
	if (from >= le32_to_cpu(ex->ee_block)
	    && to == le32_to_cpu(ex->ee_block) + ee_len - 1) {
		/* tail removal */
		ext4_lblk_t num;
		ext4_fsblk_t start;

		num = le32_to_cpu(ex->ee_block) + ee_len - from;
		start = ext_pblock(ex) + ee_len - num;
		ext_debug("free last %u blocks starting %llu\n", num, start);
		for (i = 0; i < num; i++) {
			bh = sb_find_get_block(inode->i_sb, start + i);
			ext4_forget(handle, 0, inode, bh, start + i);
		}
		ext4_free_blocks(handle, inode, start, num, metadata);
	} else if (from == le32_to_cpu(ex->ee_block)
		   && to <= le32_to_cpu(ex->ee_block) + ee_len - 1) {
		printk(KERN_INFO "strange request: removal %u-%u from %u:%u\n",
			from, to, le32_to_cpu(ex->ee_block), ee_len);
	} else {
		printk(KERN_INFO "strange request: removal(2) "
				"%u-%u from %u:%u\n",
				from, to, le32_to_cpu(ex->ee_block), ee_len);
	}
	return 0;
}

static int
ext4_ext_rm_leaf(handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path, ext4_lblk_t start)
{
	int err = 0, correct_index = 0;
	int depth = ext_depth(inode), credits;
	struct ext4_extent_header *eh;
	ext4_lblk_t a, b, block;
	unsigned num;
	ext4_lblk_t ex_ee_block;
	unsigned short ex_ee_len;
	unsigned uninitialized = 0;
	struct ext4_extent *ex;

	/* the header must be checked already in ext4_ext_remove_space() */
	ext_debug("truncate since %u in leaf\n", start);
	if (!path[depth].p_hdr)
		path[depth].p_hdr = ext_block_hdr(path[depth].p_bh);
	eh = path[depth].p_hdr;
	BUG_ON(eh == NULL);

	/* find where to start removing */
	ex = EXT_LAST_EXTENT(eh);

	ex_ee_block = le32_to_cpu(ex->ee_block);
	ex_ee_len = ext4_ext_get_actual_len(ex);

	while (ex >= EXT_FIRST_EXTENT(eh) &&
			ex_ee_block + ex_ee_len > start) {

		if (ext4_ext_is_uninitialized(ex))
			uninitialized = 1;
		else
			uninitialized = 0;

		ext_debug("remove ext %u:[%d]%d\n", ex_ee_block,
			 uninitialized, ex_ee_len);
		path[depth].p_ext = ex;

		a = ex_ee_block > start ? ex_ee_block : start;
		b = ex_ee_block + ex_ee_len - 1 < EXT_MAX_BLOCK ?
			ex_ee_block + ex_ee_len - 1 : EXT_MAX_BLOCK;

		ext_debug("  border %u:%u\n", a, b);

		if (a != ex_ee_block && b != ex_ee_block + ex_ee_len - 1) {
			block = 0;
			num = 0;
			BUG();
		} else if (a != ex_ee_block) {
			/* remove tail of the extent */
			block = ex_ee_block;
			num = a - block;
		} else if (b != ex_ee_block + ex_ee_len - 1) {
			/* remove head of the extent */
			block = a;
			num = b - a;
			/* there is no "make a hole" API yet */
			BUG();
		} else {
			/* remove whole extent: excellent! */
			block = ex_ee_block;
			num = 0;
			BUG_ON(a != ex_ee_block);
			BUG_ON(b != ex_ee_block + ex_ee_len - 1);
		}

		/*
		 * 3 for leaf, sb, and inode plus 2 (bmap and group
		 * descriptor) for each block group; assume two block
		 * groups plus ex_ee_len/blocks_per_block_group for
		 * the worst case
		 */
		credits = 7 + 2*(ex_ee_len/EXT4_BLOCKS_PER_GROUP(inode->i_sb));
		if (ex == EXT_FIRST_EXTENT(eh)) {
			correct_index = 1;
			credits += (ext_depth(inode)) + 1;
		}
		credits += 2 * EXT4_QUOTA_TRANS_BLOCKS(inode->i_sb);

		err = ext4_ext_truncate_extend_restart(handle, inode, credits);
		if (err)
			goto out;

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		err = ext4_remove_blocks(handle, inode, ex, a, b);
		if (err)
			goto out;

		if (num == 0) {
			/* this extent is removed; mark slot entirely unused */
			ext4_ext_store_pblock(ex, 0);
			le16_add_cpu(&eh->eh_entries, -1);
		}

		ex->ee_block = cpu_to_le32(block);
		ex->ee_len = cpu_to_le16(num);
		/*
		 * Do not mark uninitialized if all the blocks in the
		 * extent have been removed.
		 */
		if (uninitialized && num)
			ext4_ext_mark_uninitialized(ex);

		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto out;

		ext_debug("new extent: %u:%u:%llu\n", block, num,
				ext_pblock(ex));
		ex--;
		ex_ee_block = le32_to_cpu(ex->ee_block);
		ex_ee_len = ext4_ext_get_actual_len(ex);
	}

	if (correct_index && eh->eh_entries)
		err = ext4_ext_correct_indexes(handle, inode, path);

	/* if this leaf is free, then we should
	 * remove it from index block above */
	if (err == 0 && eh->eh_entries == 0 && path[depth].p_bh != NULL)
		err = ext4_ext_rm_idx(handle, inode, path + depth);

out:
	return err;
}

/*
 * ext4_ext_more_to_rm:
 * returns 1 if current index has to be freed (even partial)
 */
static int
ext4_ext_more_to_rm(struct ext4_ext_path *path)
{
	BUG_ON(path->p_idx == NULL);

	if (path->p_idx < EXT_FIRST_INDEX(path->p_hdr))
		return 0;

	/*
	 * if truncate on deeper level happened, it wasn't partial,
	 * so we have to consider current index for truncation
	 */
	if (le16_to_cpu(path->p_hdr->eh_entries) == path->p_block)
		return 0;
	return 1;
}

static int ext4_ext_remove_space(struct inode *inode, ext4_lblk_t start)
{
	struct super_block *sb = inode->i_sb;
	int depth = ext_depth(inode);
	struct ext4_ext_path *path;
	handle_t *handle;
	int i = 0, err = 0;

	ext_debug("truncate since %u\n", start);

	/* probably first extent we're gonna free will be last in block */
	handle = ext4_journal_start(inode, depth + 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext4_ext_invalidate_cache(inode);

	/*
	 * We start scanning from right side, freeing all the blocks
	 * after i_size and walking into the tree depth-wise.
	 */
	path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 1), GFP_NOFS);
	if (path == NULL) {
		ext4_journal_stop(handle);
		return -ENOMEM;
	}
	path[0].p_hdr = ext_inode_hdr(inode);
	if (ext4_ext_check(inode, path[0].p_hdr, depth)) {
		err = -EIO;
		goto out;
	}
	path[0].p_depth = depth;

	while (i >= 0 && err == 0) {
		if (i == depth) {
			/* this is leaf block */
			err = ext4_ext_rm_leaf(handle, inode, path, start);
			/* root level has p_bh == NULL, brelse() eats this */
			brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			continue;
		}

		/* this is index block */
		if (!path[i].p_hdr) {
			ext_debug("initialize header\n");
			path[i].p_hdr = ext_block_hdr(path[i].p_bh);
		}

		if (!path[i].p_idx) {
			/* this level hasn't been touched yet */
			path[i].p_idx = EXT_LAST_INDEX(path[i].p_hdr);
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries)+1;
			ext_debug("init index ptr: hdr 0x%p, num %d\n",
				  path[i].p_hdr,
				  le16_to_cpu(path[i].p_hdr->eh_entries));
		} else {
			/* we were already here, see at next index */
			path[i].p_idx--;
		}

		ext_debug("level %d - index, first 0x%p, cur 0x%p\n",
				i, EXT_FIRST_INDEX(path[i].p_hdr),
				path[i].p_idx);
		if (ext4_ext_more_to_rm(path + i)) {
			struct buffer_head *bh;
			/* go to the next level */
			ext_debug("move to level %d (block %llu)\n",
				  i + 1, idx_pblock(path[i].p_idx));
			memset(path + i + 1, 0, sizeof(*path));
			bh = sb_bread(sb, idx_pblock(path[i].p_idx));
			if (!bh) {
				/* should we reset i_size? */
				err = -EIO;
				break;
			}
			WARN_ON(i + 1 > depth);
			if (i + 1 > depth) {
				err = -EIO;
				break;
			}
			if (ext4_ext_check(inode, ext_block_hdr(bh),
							depth - i - 1)) {
				err = -EIO;
				break;
			}
			path[i + 1].p_bh = bh;

			/* save actual number of indexes since this
			 * number is changed at the next iteration */
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries);
			i++;
		} else {
			/* we finished processing this index, go up */
			if (path[i].p_hdr->eh_entries == 0 && i > 0) {
				/* index is empty, remove it;
				 * handle must be already prepared by the
				 * truncatei_leaf() */
				err = ext4_ext_rm_idx(handle, inode, path + i);
			}
			/* root level has p_bh == NULL, brelse() eats this */
			brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			ext_debug("return to level %d\n", i);
		}
	}

	/* TODO: flexible tree reduction should be here */
	if (path->p_hdr->eh_entries == 0) {
		/*
		 * truncate to zero freed all the tree,
		 * so we need to correct eh_depth
		 */
		err = ext4_ext_get_access(handle, inode, path);
		if (err == 0) {
			ext_inode_hdr(inode)->eh_depth = 0;
			ext_inode_hdr(inode)->eh_max =
				cpu_to_le16(ext4_ext_space_root(inode, 0));
			err = ext4_ext_dirty(handle, inode, path);
		}
	}
out:
	ext4_ext_drop_refs(path);
	kfree(path);
	ext4_journal_stop(handle);

	return err;
}

/*
 * called at mount time
 */
void ext4_ext_init(struct super_block *sb)
{
	/*
	 * possible initialization would be here
	 */

	if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
#if defined(AGGRESSIVE_TEST) || defined(CHECK_BINSEARCH) || defined(EXTENTS_STATS)
		printk(KERN_INFO "EXT4-fs: file extents enabled");
#ifdef AGGRESSIVE_TEST
		printk(", aggressive tests");
#endif
#ifdef CHECK_BINSEARCH
		printk(", check binsearch");
#endif
#ifdef EXTENTS_STATS
		printk(", stats");
#endif
		printk("\n");
#endif
#ifdef EXTENTS_STATS
		spin_lock_init(&EXT4_SB(sb)->s_ext_stats_lock);
		EXT4_SB(sb)->s_ext_min = 1 << 30;
		EXT4_SB(sb)->s_ext_max = 0;
#endif
	}
}

/*
 * called at umount time
 */
void ext4_ext_release(struct super_block *sb)
{
	if (!EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_EXTENTS))
		return;

#ifdef EXTENTS_STATS
	if (EXT4_SB(sb)->s_ext_blocks && EXT4_SB(sb)->s_ext_extents) {
		struct ext4_sb_info *sbi = EXT4_SB(sb);
		printk(KERN_ERR "EXT4-fs: %lu blocks in %lu extents (%lu ave)\n",
			sbi->s_ext_blocks, sbi->s_ext_extents,
			sbi->s_ext_blocks / sbi->s_ext_extents);
		printk(KERN_ERR "EXT4-fs: extents: %lu min, %lu max, max depth %lu\n",
			sbi->s_ext_min, sbi->s_ext_max, sbi->s_depth_max);
	}
#endif
}

static int bi_complete(struct bio *bio, unsigned int bytes, int error)
{
	complete((struct completion *)bio->bi_private);
	return 0;
}

/* FIXME!! we need to try to merge to left or right after zero-out  */
static int ext4_ext_zeroout(struct inode *inode, struct ext4_extent *ex)
{
	int ret = -EIO;
	struct bio *bio;
	int blkbits, blocksize;
	sector_t ee_pblock;
	struct completion event;
	unsigned int ee_len, len, done, offset;


	blkbits   = inode->i_blkbits;
	blocksize = inode->i_sb->s_blocksize;
	ee_len    = ext4_ext_get_actual_len(ex);
	ee_pblock = ext_pblock(ex);

	/* convert ee_pblock to 512 byte sectors */
	ee_pblock = ee_pblock << (blkbits - 9);

	while (ee_len > 0) {

		if (ee_len > BIO_MAX_PAGES)
			len = BIO_MAX_PAGES;
		else
			len = ee_len;

		bio = bio_alloc(GFP_NOIO, len);
		bio->bi_sector = ee_pblock;
		bio->bi_bdev   = inode->i_sb->s_bdev;

		done = 0;
		offset = 0;
		while (done < len) {
			ret = bio_add_page(bio, ext4_zero_page,
							blocksize, offset);
			if (ret != blocksize) {
				/*
				 * We can't add any more pages because of
				 * hardware limitations.  Start a new bio.
				 */
				break;
			}
			done++;
			offset += blocksize;
			if (offset >= PAGE_CACHE_SIZE)
				offset = 0;
		}

		init_completion(&event);
		bio->bi_private = &event;
		bio->bi_end_io = bi_complete;
		submit_bio(WRITE, bio);
		wait_for_completion(&event);

		if (test_bit(BIO_UPTODATE, &bio->bi_flags))
			ret = 0;
		else {
			ret = -EIO;
			break;
		}
		bio_put(bio);
		ee_len    -= done;
		ee_pblock += done  << (blkbits - 9);
	}
	return ret;
}

#define EXT4_EXT_ZERO_LEN 7
/*
 * This function is called by ext4_ext_get_blocks() if someone tries to write
 * to an uninitialized extent. It may result in splitting the uninitialized
 * extent into multiple extents (upto three - one initialized and two
 * uninitialized).
 * There are three possibilities:
 *   a> There is no split required: Entire extent should be initialized
 *   b> Splits in two extents: Write is happening at either end of the extent
 *   c> Splits in three extents: Somone is writing in middle of the extent
 */
static int ext4_ext_convert_to_initialized(handle_t *handle,
						struct inode *inode,
						struct ext4_ext_path *path,
						ext4_lblk_t iblock,
						unsigned int max_blocks)
{
	struct ext4_extent *ex, newex, orig_ex;
	struct ext4_extent *ex1 = NULL;
	struct ext4_extent *ex2 = NULL;
	struct ext4_extent *ex3 = NULL;
	struct ext4_extent_header *eh;
	ext4_lblk_t ee_block;
	unsigned int allocated, ee_len, depth;
	ext4_fsblk_t newblock;
	int err = 0;
	int ret = 0;

	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	allocated = ee_len - (iblock - ee_block);
	newblock = iblock - ee_block + ext_pblock(ex);
	ex2 = ex;
	orig_ex.ee_block = ex->ee_block;
	orig_ex.ee_len   = cpu_to_le16(ee_len);
	ext4_ext_store_pblock(&orig_ex, ext_pblock(ex));

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;
	/* If extent has less than 2*EXT4_EXT_ZERO_LEN zerout directly */
	if (ee_len <= 2*EXT4_EXT_ZERO_LEN) {
		err =  ext4_ext_zeroout(inode, &orig_ex);
		if (err)
			goto fix_extent_len;
		/* update the extent length and mark as initialized */
		ex->ee_block = orig_ex.ee_block;
		ex->ee_len   = orig_ex.ee_len;
		ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
		ext4_ext_dirty(handle, inode, path + depth);
		/* zeroed the full extent */
		return allocated;
	}

	/* ex1: ee_block to iblock - 1 : uninitialized */
	if (iblock > ee_block) {
		ex1 = ex;
		ex1->ee_len = cpu_to_le16(iblock - ee_block);
		ext4_ext_mark_uninitialized(ex1);
		ex2 = &newex;
	}
	/*
	 * for sanity, update the length of the ex2 extent before
	 * we insert ex3, if ex1 is NULL. This is to avoid temporary
	 * overlap of blocks.
	 */
	if (!ex1 && allocated > max_blocks)
		ex2->ee_len = cpu_to_le16(max_blocks);
	/* ex3: to ee_block + ee_len : uninitialised */
	if (allocated > max_blocks) {
		unsigned int newdepth;
		/* If extent has less than EXT4_EXT_ZERO_LEN zerout directly */
		if (allocated <= EXT4_EXT_ZERO_LEN) {
			/*
			 * iblock == ee_block is handled by the zerouout
			 * at the beginning.
			 * Mark first half uninitialized.
			 * Mark second half initialized and zero out the
			 * initialized extent
			 */
			ex->ee_block = orig_ex.ee_block;
			ex->ee_len   = cpu_to_le16(ee_len - allocated);
			ext4_ext_mark_uninitialized(ex);
			ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
			ext4_ext_dirty(handle, inode, path + depth);

			ex3 = &newex;
			ex3->ee_block = cpu_to_le32(iblock);
			ext4_ext_store_pblock(ex3, newblock);
			ex3->ee_len = cpu_to_le16(allocated);
			err = ext4_ext_insert_extent(handle, inode, path,
							ex3, 0);
			if (err == -ENOSPC) {
				err =  ext4_ext_zeroout(inode, &orig_ex);
				if (err)
					goto fix_extent_len;
				ex->ee_block = orig_ex.ee_block;
				ex->ee_len   = orig_ex.ee_len;
				ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
				ext4_ext_dirty(handle, inode, path + depth);
				/* blocks available from iblock */
				return allocated;

			} else if (err)
				goto fix_extent_len;

			/*
			 * We need to zero out the second half because
			 * an fallocate request can update file size and
			 * converting the second half to initialized extent
			 * implies that we can leak some junk data to user
			 * space.
			 */
			err =  ext4_ext_zeroout(inode, ex3);
			if (err) {
				/*
				 * We should actually mark the
				 * second half as uninit and return error
				 * Insert would have changed the extent
				 */
				depth = ext_depth(inode);
				ext4_ext_drop_refs(path);
				path = ext4_ext_find_extent(inode,
								iblock, path);
				if (IS_ERR(path)) {
					err = PTR_ERR(path);
					return err;
				}
				/* get the second half extent details */
				ex = path[depth].p_ext;
				err = ext4_ext_get_access(handle, inode,
								path + depth);
				if (err)
					return err;
				ext4_ext_mark_uninitialized(ex);
				ext4_ext_dirty(handle, inode, path + depth);
				return err;
			}

			/* zeroed the second half */
			return allocated;
		}
		ex3 = &newex;
		ex3->ee_block = cpu_to_le32(iblock + max_blocks);
		ext4_ext_store_pblock(ex3, newblock + max_blocks);
		ex3->ee_len = cpu_to_le16(allocated - max_blocks);
		ext4_ext_mark_uninitialized(ex3);
		err = ext4_ext_insert_extent(handle, inode, path, ex3, 0);
		if (err == -ENOSPC) {
			err =  ext4_ext_zeroout(inode, &orig_ex);
			if (err)
				goto fix_extent_len;
			/* update the extent length and mark as initialized */
			ex->ee_block = orig_ex.ee_block;
			ex->ee_len   = orig_ex.ee_len;
			ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
			ext4_ext_dirty(handle, inode, path + depth);
			/* zeroed the full extent */
			/* blocks available from iblock */
			return allocated;

		} else if (err)
			goto fix_extent_len;
		/*
		 * The depth, and hence eh & ex might change
		 * as part of the insert above.
		 */
		newdepth = ext_depth(inode);
		/*
		 * update the extent length after successful insert of the
		 * split extent
		 */
		orig_ex.ee_len = cpu_to_le16(ee_len -
						ext4_ext_get_actual_len(ex3));
		depth = newdepth;
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode, iblock, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto out;
		}
		eh = path[depth].p_hdr;
		ex = path[depth].p_ext;
		if (ex2 != &newex)
			ex2 = ex;

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		allocated = max_blocks;

		/* If extent has less than EXT4_EXT_ZERO_LEN and we are trying
		 * to insert a extent in the middle zerout directly
		 * otherwise give the extent a chance to merge to left
		 */
		if (le16_to_cpu(orig_ex.ee_len) <= EXT4_EXT_ZERO_LEN &&
							iblock != ee_block) {
			err =  ext4_ext_zeroout(inode, &orig_ex);
			if (err)
				goto fix_extent_len;
			/* update the extent length and mark as initialized */
			ex->ee_block = orig_ex.ee_block;
			ex->ee_len   = orig_ex.ee_len;
			ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
			ext4_ext_dirty(handle, inode, path + depth);
			/* zero out the first half */
			/* blocks available from iblock */
			return allocated;
		}
	}
	/*
	 * If there was a change of depth as part of the
	 * insertion of ex3 above, we need to update the length
	 * of the ex1 extent again here
	 */
	if (ex1 && ex1 != ex) {
		ex1 = ex;
		ex1->ee_len = cpu_to_le16(iblock - ee_block);
		ext4_ext_mark_uninitialized(ex1);
		ex2 = &newex;
	}
	/* ex2: iblock to iblock + maxblocks-1 : initialised */
	ex2->ee_block = cpu_to_le32(iblock);
	ext4_ext_store_pblock(ex2, newblock);
	ex2->ee_len = cpu_to_le16(allocated);
	if (ex2 != ex)
		goto insert;
	/*
	 * New (initialized) extent starts from the first block
	 * in the current extent. i.e., ex2 == ex
	 * We have to see if it can be merged with the extent
	 * on the left.
	 */
	if (ex2 > EXT_FIRST_EXTENT(eh)) {
		/*
		 * To merge left, pass "ex2 - 1" to try_to_merge(),
		 * since it merges towards right _only_.
		 */
		ret = ext4_ext_try_to_merge(inode, path, ex2 - 1);
		if (ret) {
			err = ext4_ext_correct_indexes(handle, inode, path);
			if (err)
				goto out;
			depth = ext_depth(inode);
			ex2--;
		}
	}
	/*
	 * Try to Merge towards right. This might be required
	 * only when the whole extent is being written to.
	 * i.e. ex2 == ex and ex3 == NULL.
	 */
	if (!ex3) {
		ret = ext4_ext_try_to_merge(inode, path, ex2);
		if (ret) {
			err = ext4_ext_correct_indexes(handle, inode, path);
			if (err)
				goto out;
		}
	}
	/* Mark modified extent as dirty */
	err = ext4_ext_dirty(handle, inode, path + depth);
	goto out;
insert:
	err = ext4_ext_insert_extent(handle, inode, path, &newex, 0);
	if (err == -ENOSPC) {
		err =  ext4_ext_zeroout(inode, &orig_ex);
		if (err)
			goto fix_extent_len;
		/* update the extent length and mark as initialized */
		ex->ee_block = orig_ex.ee_block;
		ex->ee_len   = orig_ex.ee_len;
		ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
		ext4_ext_dirty(handle, inode, path + depth);
		/* zero out the first half */
		return allocated;
	} else if (err)
		goto fix_extent_len;
out:
	ext4_ext_show_leaf(inode, path);
	return err ? err : allocated;

fix_extent_len:
	ex->ee_block = orig_ex.ee_block;
	ex->ee_len   = orig_ex.ee_len;
	ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
	ext4_ext_mark_uninitialized(ex);
	ext4_ext_dirty(handle, inode, path + depth);
	return err;
}

/*
 * This function is called by ext4_ext_get_blocks() from
 * ext4_get_blocks_dio_write() when DIO to write
 * to an uninitialized extent.
 *
 * Writing to an uninitized extent may result in splitting the uninitialized
 * extent into multiple /intialized unintialized extents (up to three)
 * There are three possibilities:
 *   a> There is no split required: Entire extent should be uninitialized
 *   b> Splits in two extents: Write is happening at either end of the extent
 *   c> Splits in three extents: Somone is writing in middle of the extent
 *
 * One of more index blocks maybe needed if the extent tree grow after
 * the unintialized extent split. To prevent ENOSPC occur at the IO
 * complete, we need to split the uninitialized extent before DIO submit
 * the IO. The uninitilized extent called at this time will be split
 * into three uninitialized extent(at most). After IO complete, the part
 * being filled will be convert to initialized by the end_io callback function
 * via ext4_convert_unwritten_extents().
 *
 * Returns the size of uninitialized extent to be written on success.
 */
static int ext4_split_unwritten_extents(handle_t *handle,
					struct inode *inode,
					struct ext4_ext_path *path,
					ext4_lblk_t iblock,
					unsigned int max_blocks,
					int flags)
{
	struct ext4_extent *ex, newex, orig_ex;
	struct ext4_extent *ex1 = NULL;
	struct ext4_extent *ex2 = NULL;
	struct ext4_extent *ex3 = NULL;
	struct ext4_extent_header *eh;
	ext4_lblk_t ee_block;
	unsigned int allocated, ee_len, depth;
	ext4_fsblk_t newblock;
	int err = 0;

	ext_debug("ext4_split_unwritten_extents: inode %lu,"
		  "iblock %llu, max_blocks %u\n", inode->i_ino,
		  (unsigned long long)iblock, max_blocks);
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	allocated = ee_len - (iblock - ee_block);
	newblock = iblock - ee_block + ext_pblock(ex);
	ex2 = ex;
	orig_ex.ee_block = ex->ee_block;
	orig_ex.ee_len   = cpu_to_le16(ee_len);
	ext4_ext_store_pblock(&orig_ex, ext_pblock(ex));

	/*
 	 * If the uninitialized extent begins at the same logical
 	 * block where the write begins, and the write completely
 	 * covers the extent, then we don't need to split it.
 	 */
	if ((iblock == ee_block) && (allocated <= max_blocks))
		return allocated;

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;
	/* ex1: ee_block to iblock - 1 : uninitialized */
	if (iblock > ee_block) {
		ex1 = ex;
		ex1->ee_len = cpu_to_le16(iblock - ee_block);
		ext4_ext_mark_uninitialized(ex1);
		ex2 = &newex;
	}
	/*
	 * for sanity, update the length of the ex2 extent before
	 * we insert ex3, if ex1 is NULL. This is to avoid temporary
	 * overlap of blocks.
	 */
	if (!ex1 && allocated > max_blocks)
		ex2->ee_len = cpu_to_le16(max_blocks);
	/* ex3: to ee_block + ee_len : uninitialised */
	if (allocated > max_blocks) {
		unsigned int newdepth;
		ex3 = &newex;
		ex3->ee_block = cpu_to_le32(iblock + max_blocks);
		ext4_ext_store_pblock(ex3, newblock + max_blocks);
		ex3->ee_len = cpu_to_le16(allocated - max_blocks);
		ext4_ext_mark_uninitialized(ex3);
		err = ext4_ext_insert_extent(handle, inode, path, ex3, flags);
		if (err == -ENOSPC) {
			err =  ext4_ext_zeroout(inode, &orig_ex);
			if (err)
				goto fix_extent_len;
			/* update the extent length and mark as initialized */
			ex->ee_block = orig_ex.ee_block;
			ex->ee_len   = orig_ex.ee_len;
			ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
			ext4_ext_dirty(handle, inode, path + depth);
			/* zeroed the full extent */
			/* blocks available from iblock */
			return allocated;

		} else if (err)
			goto fix_extent_len;
		/*
		 * The depth, and hence eh & ex might change
		 * as part of the insert above.
		 */
		newdepth = ext_depth(inode);
		/*
		 * update the extent length after successful insert of the
		 * split extent
		 */
		orig_ex.ee_len = cpu_to_le16(ee_len -
						ext4_ext_get_actual_len(ex3));
		depth = newdepth;
		ext4_ext_drop_refs(path);
		path = ext4_ext_find_extent(inode, iblock, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			goto out;
		}
		eh = path[depth].p_hdr;
		ex = path[depth].p_ext;
		if (ex2 != &newex)
			ex2 = ex;

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		allocated = max_blocks;
	}
	/*
	 * If there was a change of depth as part of the
	 * insertion of ex3 above, we need to update the length
	 * of the ex1 extent again here
	 */
	if (ex1 && ex1 != ex) {
		ex1 = ex;
		ex1->ee_len = cpu_to_le16(iblock - ee_block);
		ext4_ext_mark_uninitialized(ex1);
		ex2 = &newex;
	}
	/*
	 * ex2: iblock to iblock + maxblocks-1 : to be direct IO written,
	 * uninitialised still.
	 */
	ex2->ee_block = cpu_to_le32(iblock);
	ext4_ext_store_pblock(ex2, newblock);
	ex2->ee_len = cpu_to_le16(allocated);
	ext4_ext_mark_uninitialized(ex2);
	if (ex2 != ex)
		goto insert;
	/* Mark modified extent as dirty */
	err = ext4_ext_dirty(handle, inode, path + depth);
	ext_debug("out here\n");
	goto out;
insert:
	err = ext4_ext_insert_extent(handle, inode, path, &newex, flags);
	if (err == -ENOSPC) {
		err =  ext4_ext_zeroout(inode, &orig_ex);
		if (err)
			goto fix_extent_len;
		/* update the extent length and mark as initialized */
		ex->ee_block = orig_ex.ee_block;
		ex->ee_len   = orig_ex.ee_len;
		ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
		ext4_ext_dirty(handle, inode, path + depth);
		/* zero out the first half */
		return allocated;
	} else if (err)
		goto fix_extent_len;
out:
	ext4_ext_show_leaf(inode, path);
	return err ? err : allocated;

fix_extent_len:
	ex->ee_block = orig_ex.ee_block;
	ex->ee_len   = orig_ex.ee_len;
	ext4_ext_store_pblock(ex, ext_pblock(&orig_ex));
	ext4_ext_mark_uninitialized(ex);
	ext4_ext_dirty(handle, inode, path + depth);
	return err;
}
static int ext4_convert_unwritten_extents_dio(handle_t *handle,
					      struct inode *inode,
					      struct ext4_ext_path *path)
{
	struct ext4_extent *ex;
	struct ext4_extent_header *eh;
	int depth;
	int err = 0;
	int ret = 0;

	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;
	/* first mark the extent as initialized */
	ext4_ext_mark_initialized(ex);

	/*
	 * We have to see if it can be merged with the extent
	 * on the left.
	 */
	if (ex > EXT_FIRST_EXTENT(eh)) {
		/*
		 * To merge left, pass "ex - 1" to try_to_merge(),
		 * since it merges towards right _only_.
		 */
		ret = ext4_ext_try_to_merge(inode, path, ex - 1);
		if (ret) {
			err = ext4_ext_correct_indexes(handle, inode, path);
			if (err)
				goto out;
			depth = ext_depth(inode);
			ex--;
		}
	}
	/*
	 * Try to Merge towards right.
	 */
	ret = ext4_ext_try_to_merge(inode, path, ex);
	if (ret) {
		err = ext4_ext_correct_indexes(handle, inode, path);
		if (err)
			goto out;
		depth = ext_depth(inode);
	}
	/* Mark modified extent as dirty */
	err = ext4_ext_dirty(handle, inode, path + depth);
out:
	ext4_ext_show_leaf(inode, path);
	return err;
}

static int
ext4_ext_handle_uninitialized_extents(handle_t *handle, struct inode *inode,
			ext4_lblk_t iblock, unsigned int max_blocks,
			struct ext4_ext_path *path, int flags,
			unsigned int allocated, struct buffer_head *bh_result,
			ext4_fsblk_t newblock)
{
	int ret = 0;
	int err = 0;
	ext4_io_end_t *io = EXT4_I(inode)->cur_aio_dio;

	ext_debug("ext4_ext_handle_uninitialized_extents: inode %lu, logical"
		  "block %llu, max_blocks %u, flags %d, allocated %u",
		  inode->i_ino, (unsigned long long)iblock, max_blocks,
		  flags, allocated);
	ext4_ext_show_leaf(inode, path);

	/* DIO get_block() before submit the IO, split the extent */
	if (flags == EXT4_GET_BLOCKS_DIO_CREATE_EXT) {
		ret = ext4_split_unwritten_extents(handle,
						inode, path, iblock,
						max_blocks, flags);
		/*
		 * Flag the inode(non aio case) or end_io struct (aio case)
		 * that this IO needs to convertion to written when IO is
		 * completed
		 */
		if (io)
			io->flag = DIO_AIO_UNWRITTEN;
		else
			EXT4_I(inode)->i_state |= EXT4_STATE_DIO_UNWRITTEN;
		goto out;
	}
	/* async DIO end_io complete, convert the filled extent to written */
	if (flags == EXT4_GET_BLOCKS_DIO_CONVERT_EXT) {
		ret = ext4_convert_unwritten_extents_dio(handle, inode,
							path);
		goto out2;
	}
	/* buffered IO case */
	/*
	 * repeat fallocate creation request
	 * we already have an unwritten extent
	 */
	if (flags & EXT4_GET_BLOCKS_UNINIT_EXT)
		goto map_out;

	/* buffered READ or buffered write_begin() lookup */
	if ((flags & EXT4_GET_BLOCKS_CREATE) == 0) {
		/*
		 * We have blocks reserved already.  We
		 * return allocated blocks so that delalloc
		 * won't do block reservation for us.  But
		 * the buffer head will be unmapped so that
		 * a read from the block returns 0s.
		 */
		set_buffer_unwritten(bh_result);
		goto out1;
	}

	/* buffered write, writepage time, convert*/
	ret = ext4_ext_convert_to_initialized(handle, inode,
						path, iblock,
						max_blocks);
out:
	if (ret <= 0) {
		err = ret;
		goto out2;
	} else
		allocated = ret;
	set_buffer_new(bh_result);
map_out:
	set_buffer_mapped(bh_result);
out1:
	if (allocated > max_blocks)
		allocated = max_blocks;
	ext4_ext_show_leaf(inode, path);
	bh_result->b_bdev = inode->i_sb->s_bdev;
	bh_result->b_blocknr = newblock;
out2:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}
	return err ? err : allocated;
}
/*
 * Block allocation/map/preallocation routine for extents based files
 *
 *
 * Need to be called with
 * down_read(&EXT4_I(inode)->i_data_sem) if not allocating file system block
 * (ie, create is zero). Otherwise down_write(&EXT4_I(inode)->i_data_sem)
 *
 * return > 0, number of of blocks already mapped/allocated
 *          if create == 0 and these are pre-allocated blocks
 *          	buffer head is unmapped
 *          otherwise blocks are mapped
 *
 * return = 0, if plain look up failed (blocks have not been allocated)
 *          buffer head is unmapped
 *
 * return < 0, error case.
 */
int ext4_ext_get_blocks(handle_t *handle, struct inode *inode,
			ext4_lblk_t iblock,
			unsigned int max_blocks, struct buffer_head *bh_result,
			int flags)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent_header *eh;
	struct ext4_extent newex, *ex;
	ext4_fsblk_t newblock;
	int err = 0, depth, ret, cache_type;
	unsigned int allocated = 0;
	struct ext4_allocation_request ar;
	ext4_io_end_t *io = EXT4_I(inode)->cur_aio_dio;

	__clear_bit(BH_New, &bh_result->b_state);
	ext_debug("blocks %u/%u requested for inode %lu\n",
			iblock, max_blocks, inode->i_ino);

	/* check in cache */
	cache_type = ext4_ext_in_cache(inode, iblock, &newex);
	if (cache_type) {
		if (cache_type == EXT4_EXT_CACHE_GAP) {
			if ((flags & EXT4_GET_BLOCKS_CREATE) == 0) {
				/*
				 * block isn't allocated yet and
				 * user doesn't want to allocate it
				 */
				goto out2;
			}
			/* we should allocate requested block */
		} else if (cache_type == EXT4_EXT_CACHE_EXTENT) {
			/* block is already allocated */
			newblock = iblock
				   - le32_to_cpu(newex.ee_block)
				   + ext_pblock(&newex);
			/* number of remaining blocks in the extent */
			allocated = ext4_ext_get_actual_len(&newex) -
					(iblock - le32_to_cpu(newex.ee_block));
			goto out;
		} else {
			BUG();
		}
	}

	/* find extent for this block */
	path = ext4_ext_find_extent(inode, iblock, NULL);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
		goto out2;
	}

	depth = ext_depth(inode);

	/*
	 * consistent leaf must not be empty;
	 * this situation is possible, though, _during_ tree modification;
	 * this is why assert can't be put in ext4_ext_find_extent()
	 */
	BUG_ON(path[depth].p_ext == NULL && depth != 0);
	eh = path[depth].p_hdr;

	ex = path[depth].p_ext;
	if (ex) {
		ext4_lblk_t ee_block = le32_to_cpu(ex->ee_block);
		ext4_fsblk_t ee_start = ext_pblock(ex);
		unsigned short ee_len;

		/*
		 * Uninitialized extents are treated as holes, except that
		 * we split out initialized portions during a write.
		 */
		ee_len = ext4_ext_get_actual_len(ex);
		/* if found extent covers block, simply return it */
		if (in_range(iblock, ee_block, ee_len)) {
			newblock = iblock - ee_block + ee_start;
			/* number of remaining blocks in the extent */
			allocated = ee_len - (iblock - ee_block);
			ext_debug("%u fit into %u:%d -> %llu\n", iblock,
					ee_block, ee_len, newblock);

			/* Do not put uninitialized extent in the cache */
			if (!ext4_ext_is_uninitialized(ex)) {
				ext4_ext_put_in_cache(inode, ee_block,
							ee_len, ee_start,
							EXT4_EXT_CACHE_EXTENT);
				goto out;
			}
			ret = ext4_ext_handle_uninitialized_extents(handle,
					inode, iblock, max_blocks, path,
					flags, allocated, bh_result, newblock);
			return ret;
		}
	}

	/*
	 * requested block isn't allocated yet;
	 * we couldn't try to create block if create flag is zero
	 */
	if ((flags & EXT4_GET_BLOCKS_CREATE) == 0) {
		/*
		 * put just found gap into cache to speed up
		 * subsequent requests
		 */
		ext4_ext_put_gap_in_cache(inode, path, iblock);
		goto out2;
	}
	/*
	 * Okay, we need to do block allocation.
	 */

	/* find neighbour allocated blocks */
	ar.lleft = iblock;
	err = ext4_ext_search_left(inode, path, &ar.lleft, &ar.pleft);
	if (err)
		goto out2;
	ar.lright = iblock;
	err = ext4_ext_search_right(inode, path, &ar.lright, &ar.pright);
	if (err)
		goto out2;

	/*
	 * See if request is beyond maximum number of blocks we can have in
	 * a single extent. For an initialized extent this limit is
	 * EXT_INIT_MAX_LEN and for an uninitialized extent this limit is
	 * EXT_UNINIT_MAX_LEN.
	 */
	if (max_blocks > EXT_INIT_MAX_LEN &&
	    !(flags & EXT4_GET_BLOCKS_UNINIT_EXT))
		max_blocks = EXT_INIT_MAX_LEN;
	else if (max_blocks > EXT_UNINIT_MAX_LEN &&
		 (flags & EXT4_GET_BLOCKS_UNINIT_EXT))
		max_blocks = EXT_UNINIT_MAX_LEN;

	/* Check if we can really insert (iblock)::(iblock+max_blocks) extent */
	newex.ee_block = cpu_to_le32(iblock);
	newex.ee_len = cpu_to_le16(max_blocks);
	err = ext4_ext_check_overlap(inode, &newex, path);
	if (err)
		allocated = ext4_ext_get_actual_len(&newex);
	else
		allocated = max_blocks;

	/* allocate new block */
	ar.inode = inode;
	ar.goal = ext4_ext_find_goal(inode, path, iblock);
	ar.logical = iblock;
	ar.len = allocated;
	if (S_ISREG(inode->i_mode))
		ar.flags = EXT4_MB_HINT_DATA;
	else
		/* disable in-core preallocation for non-regular files */
		ar.flags = 0;
	newblock = ext4_mb_new_blocks(handle, &ar, &err);
	if (!newblock)
		goto out2;
	ext_debug("allocate new block: goal %llu, found %llu/%u\n",
		  ar.goal, newblock, allocated);

	/* try to insert new extent into found leaf and return */
	ext4_ext_store_pblock(&newex, newblock);
	newex.ee_len = cpu_to_le16(ar.len);
	/* Mark uninitialized */
	if (flags & EXT4_GET_BLOCKS_UNINIT_EXT){
		ext4_ext_mark_uninitialized(&newex);
		/*
		 * io_end structure was created for every async
		 * direct IO write to the middle of the file.
		 * To avoid unecessary convertion for every aio dio rewrite
		 * to the mid of file, here we flag the IO that is really
		 * need the convertion.
		 * For non asycn direct IO case, flag the inode state
		 * that we need to perform convertion when IO is done.
		 */
		if (flags == EXT4_GET_BLOCKS_DIO_CREATE_EXT) {
			if (io)
				io->flag = DIO_AIO_UNWRITTEN;
			else
				EXT4_I(inode)->i_state |=
					EXT4_STATE_DIO_UNWRITTEN;;
		}
	}
	err = ext4_ext_insert_extent(handle, inode, path, &newex, flags);
	if (err) {
		/* free data blocks we just allocated */
		/* not a good idea to call discard here directly,
		 * but otherwise we'd need to call it every free() */
		ext4_discard_preallocations(inode);
		ext4_free_blocks(handle, inode, ext_pblock(&newex),
					ext4_ext_get_actual_len(&newex), 0);
		goto out2;
	}

	/* previous routine could use block we allocated */
	newblock = ext_pblock(&newex);
	allocated = ext4_ext_get_actual_len(&newex);
	set_buffer_new(bh_result);

	/* Cache only when it is _not_ an uninitialized extent */
	if ((flags & EXT4_GET_BLOCKS_UNINIT_EXT) == 0)
		ext4_ext_put_in_cache(inode, iblock, allocated, newblock,
						EXT4_EXT_CACHE_EXTENT);
out:
	if (allocated > max_blocks)
		allocated = max_blocks;
	ext4_ext_show_leaf(inode, path);
	set_buffer_mapped(bh_result);
	bh_result->b_bdev = inode->i_sb->s_bdev;
	bh_result->b_blocknr = newblock;
out2:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}
	return err ? err : allocated;
}

void ext4_ext_truncate(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	struct super_block *sb = inode->i_sb;
	ext4_lblk_t last_block;
	handle_t *handle;
	int err = 0;

	/*
	 * probably first extent we're gonna free will be last in block
	 */
	err = ext4_writepage_trans_blocks(inode);
	handle = ext4_journal_start(inode, err);
	if (IS_ERR(handle))
		return;

	if (inode->i_size & (sb->s_blocksize - 1))
		ext4_block_truncate_page(handle, mapping, inode->i_size);

	if (ext4_orphan_add(handle, inode))
		goto out_stop;

	down_write(&EXT4_I(inode)->i_data_sem);
	ext4_ext_invalidate_cache(inode);

	ext4_discard_preallocations(inode);

	/*
	 * TODO: optimization is possible here.
	 * Probably we need not scan at all,
	 * because page truncation is enough.
	 */

	/* we have to know where to truncate from in crash case */
	EXT4_I(inode)->i_disksize = inode->i_size;
	ext4_mark_inode_dirty(handle, inode);

	last_block = (inode->i_size + sb->s_blocksize - 1)
			>> EXT4_BLOCK_SIZE_BITS(sb);
	err = ext4_ext_remove_space(inode, last_block);

	/* In a multi-transaction truncate, we only make the final
	 * transaction synchronous.
	 */
	if (IS_SYNC(inode))
		ext4_handle_sync(handle);

out_stop:
	up_write(&EXT4_I(inode)->i_data_sem);
	/*
	 * If this was a simple ftruncate() and the file will remain alive,
	 * then we need to clear up the orphan record which we created above.
	 * However, if this was a real unlink then we were called by
	 * ext4_delete_inode(), and we allow that function to clean up the
	 * orphan info for us.
	 */
	if (inode->i_nlink)
		ext4_orphan_del(handle, inode);

	inode->i_mtime = inode->i_ctime = ext4_current_time(inode);
	ext4_mark_inode_dirty(handle, inode);
	ext4_journal_stop(handle);
}

static void ext4_falloc_update_inode(struct inode *inode,
				int mode, loff_t new_size, int update_ctime)
{
	struct timespec now;

	if (update_ctime) {
		now = current_fs_time(inode->i_sb);
		if (!timespec_equal(&inode->i_ctime, &now))
			inode->i_ctime = now;
	}
	/*
	 * Update only when preallocation was requested beyond
	 * the file size.
	 */
	if (!(mode & FALLOC_FL_KEEP_SIZE)) {
		if (new_size > i_size_read(inode))
			i_size_write(inode, new_size);
		if (new_size > EXT4_I(inode)->i_disksize)
			ext4_update_i_disksize(inode, new_size);
	}

}

/*
 * preallocate space for a file. This implements ext4's fallocate inode
 * operation, which gets called from sys_fallocate system call.
 * For block-mapped files, posix_fallocate should fall back to the method
 * of writing zeroes to the required new blocks (the same behavior which is
 * expected for file systems which do not support fallocate() system call).
 */
long ext4_fallocate(struct inode *inode, int mode, loff_t offset, loff_t len)
{
	handle_t *handle;
	ext4_lblk_t block;
	loff_t new_size;
	unsigned int max_blocks;
	int ret = 0;
	int ret2 = 0;
	int retries = 0;
	struct buffer_head map_bh;
	unsigned int credits, blkbits = inode->i_blkbits;

	/*
	 * currently supporting (pre)allocate mode for extent-based
	 * files _only_
	 */
	if (!(EXT4_I(inode)->i_flags & EXT4_EXTENTS_FL))
		return -EOPNOTSUPP;

	/* preallocation to directories is currently not supported */
	if (S_ISDIR(inode->i_mode))
		return -ENODEV;

	block = offset >> blkbits;
	/*
	 * We can't just convert len to max_blocks because
	 * If blocksize = 4096 offset = 3072 and len = 2048
	 */
	max_blocks = (EXT4_BLOCK_ALIGN(len + offset, blkbits) >> blkbits)
							- block;
	/*
	 * credits to insert 1 extent into extent tree
	 */
	credits = ext4_chunk_trans_blocks(inode, max_blocks);
	mutex_lock(&inode->i_mutex);
retry:
	while (ret >= 0 && ret < max_blocks) {
		block = block + ret;
		max_blocks = max_blocks - ret;
		handle = ext4_journal_start(inode, credits);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			break;
		}
		map_bh.b_state = 0;
		ret = ext4_get_blocks(handle, inode, block,
				      max_blocks, &map_bh,
				      EXT4_GET_BLOCKS_CREATE_UNINIT_EXT);
		if (ret <= 0) {
#ifdef EXT4FS_DEBUG
			WARN_ON(ret <= 0);
			printk(KERN_ERR "%s: ext4_ext_get_blocks "
				    "returned error inode#%lu, block=%u, "
				    "max_blocks=%u", __func__,
				    inode->i_ino, block, max_blocks);
#endif
			ext4_mark_inode_dirty(handle, inode);
			ret2 = ext4_journal_stop(handle);
			break;
		}
		if ((block + ret) >= (EXT4_BLOCK_ALIGN(offset + len,
						blkbits) >> blkbits))
			new_size = offset + len;
		else
			new_size = (block + ret) << blkbits;

		ext4_falloc_update_inode(inode, mode, new_size,
						buffer_new(&map_bh));
		ext4_mark_inode_dirty(handle, inode);
		ret2 = ext4_journal_stop(handle);
		if (ret2)
			break;
	}
	if (ret == -ENOSPC &&
			ext4_should_retry_alloc(inode->i_sb, &retries)) {
		ret = 0;
		goto retry;
	}
	mutex_unlock(&inode->i_mutex);
	return ret > 0 ? ret2 : ret;
}

/*
 * This function convert a range of blocks to written extents
 * The caller of this function will pass the start offset and the size.
 * all unwritten extents within this range will be converted to
 * written extents.
 *
 * This function is called from the direct IO end io call back
 * function, to convert the fallocated extents after IO is completed.
 * Returns 0 on success.
 */
int ext4_convert_unwritten_extents(struct inode *inode, loff_t offset,
				    loff_t len)
{
	handle_t *handle;
	ext4_lblk_t block;
	unsigned int max_blocks;
	int ret = 0;
	int ret2 = 0;
	struct buffer_head map_bh;
	unsigned int credits, blkbits = inode->i_blkbits;

	block = offset >> blkbits;
	/*
	 * We can't just convert len to max_blocks because
	 * If blocksize = 4096 offset = 3072 and len = 2048
	 */
	max_blocks = (EXT4_BLOCK_ALIGN(len + offset, blkbits) >> blkbits)
							- block;
	/*
	 * credits to insert 1 extent into extent tree
	 */
	credits = ext4_chunk_trans_blocks(inode, max_blocks);
	while (ret >= 0 && ret < max_blocks) {
		block = block + ret;
		max_blocks = max_blocks - ret;
		handle = ext4_journal_start(inode, credits);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			break;
		}
		map_bh.b_state = 0;
		ret = ext4_get_blocks(handle, inode, block,
				      max_blocks, &map_bh,
				      EXT4_GET_BLOCKS_DIO_CONVERT_EXT);
		if (ret <= 0) {
			WARN_ON(ret <= 0);
			printk(KERN_ERR "%s: ext4_ext_get_blocks "
				    "returned error inode#%lu, block=%u, "
				    "max_blocks=%u", __func__,
				    inode->i_ino, block, max_blocks);
		}
		ext4_mark_inode_dirty(handle, inode);
		ret2 = ext4_journal_stop(handle);
		if (ret <= 0 || ret2 )
			break;
	}
	return ret > 0 ? ret2 : ret;
}
/*
 * Callback function called for each extent to gather FIEMAP information.
 */
static int ext4_ext_fiemap_cb(struct inode *inode, struct ext4_ext_path *path,
		       struct ext4_ext_cache *newex, struct ext4_extent *ex,
		       void *data)
{
	struct fiemap_extent_info *fieinfo = data;
	unsigned char blksize_bits = inode->i_sb->s_blocksize_bits;
	__u64	logical;
	__u64	physical;
	__u64	length;
	__u32	flags = 0;
	int	error;

	logical =  (__u64)newex->ec_block << blksize_bits;

	if (newex->ec_type == EXT4_EXT_CACHE_GAP) {
		pgoff_t offset;
		struct page *page;
		struct buffer_head *bh = NULL;

		offset = logical >> PAGE_SHIFT;
		page = find_get_page(inode->i_mapping, offset);
		if (!page || !page_has_buffers(page))
			return EXT_CONTINUE;

		bh = page_buffers(page);

		if (!bh)
			return EXT_CONTINUE;

		if (buffer_delay(bh)) {
			flags |= FIEMAP_EXTENT_DELALLOC;
			page_cache_release(page);
		} else {
			page_cache_release(page);
			return EXT_CONTINUE;
		}
	}

	physical = (__u64)newex->ec_start << blksize_bits;
	length =   (__u64)newex->ec_len << blksize_bits;

	if (ex && ext4_ext_is_uninitialized(ex))
		flags |= FIEMAP_EXTENT_UNWRITTEN;

	/*
	 * If this extent reaches EXT_MAX_BLOCK, it must be last.
	 *
	 * Or if ext4_ext_next_allocated_block is EXT_MAX_BLOCK,
	 * this also indicates no more allocated blocks.
	 *
	 * XXX this might miss a single-block extent at EXT_MAX_BLOCK
	 */
	if (ext4_ext_next_allocated_block(path) == EXT_MAX_BLOCK ||
	    newex->ec_block + newex->ec_len - 1 == EXT_MAX_BLOCK) {
		loff_t size = i_size_read(inode);
		loff_t bs = EXT4_BLOCK_SIZE(inode->i_sb);

		flags |= FIEMAP_EXTENT_LAST;
		if ((flags & FIEMAP_EXTENT_DELALLOC) &&
		    logical+length > size)
			length = (size - logical + bs - 1) & ~(bs-1);
	}

	error = fiemap_fill_next_extent(fieinfo, logical, physical,
					length, flags);
	if (error < 0)
		return error;
	if (error == 1)
		return EXT_BREAK;

	return EXT_CONTINUE;
}

/* fiemap flags we can handle specified here */
#define EXT4_FIEMAP_FLAGS	(FIEMAP_FLAG_SYNC|FIEMAP_FLAG_XATTR)

static int ext4_xattr_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo)
{
	__u64 physical = 0;
	__u64 length;
	__u32 flags = FIEMAP_EXTENT_LAST;
	int blockbits = inode->i_sb->s_blocksize_bits;
	int error = 0;

	/* in-inode? */
	if (EXT4_I(inode)->i_state & EXT4_STATE_XATTR) {
		struct ext4_iloc iloc;
		int offset;	/* offset of xattr in inode */

		error = ext4_get_inode_loc(inode, &iloc);
		if (error)
			return error;
		physical = iloc.bh->b_blocknr << blockbits;
		offset = EXT4_GOOD_OLD_INODE_SIZE +
				EXT4_I(inode)->i_extra_isize;
		physical += offset;
		length = EXT4_SB(inode->i_sb)->s_inode_size - offset;
		flags |= FIEMAP_EXTENT_DATA_INLINE;
	} else { /* external block */
		physical = EXT4_I(inode)->i_file_acl << blockbits;
		length = inode->i_sb->s_blocksize;
	}

	if (physical)
		error = fiemap_fill_next_extent(fieinfo, 0, physical,
						length, flags);
	return (error < 0 ? error : 0);
}

int ext4_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		__u64 start, __u64 len)
{
	ext4_lblk_t start_blk;
	ext4_lblk_t len_blks;
	int error = 0;

	/* fallback to generic here if not in extents fmt */
	if (!(EXT4_I(inode)->i_flags & EXT4_EXTENTS_FL))
		return generic_block_fiemap(inode, fieinfo, start, len,
			ext4_get_block);

	if (fiemap_check_flags(fieinfo, EXT4_FIEMAP_FLAGS))
		return -EBADR;

	if (fieinfo->fi_flags & FIEMAP_FLAG_XATTR) {
		error = ext4_xattr_fiemap(inode, fieinfo);
	} else {
		start_blk = start >> inode->i_sb->s_blocksize_bits;
		len_blks = len >> inode->i_sb->s_blocksize_bits;

		/*
		 * Walk the extent tree gathering extent information.
		 * ext4_ext_fiemap_cb will push extents back to user.
		 */
		down_read(&EXT4_I(inode)->i_data_sem);
		error = ext4_ext_walk_space(inode, start_blk, len_blks,
					  ext4_ext_fiemap_cb, fieinfo);
		up_read(&EXT4_I(inode)->i_data_sem);
	}

	return error;
}

