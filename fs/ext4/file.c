/*
 *  linux/fs/ext4/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/mount.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"

/*
 * Called when an inode is released. Note that this is different
 * from ext4_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int ext4_release_file(struct inode *inode, struct file *filp)
{
	if (EXT4_I(inode)->i_state & EXT4_STATE_DA_ALLOC_CLOSE) {
		ext4_alloc_da_blocks(inode);
		EXT4_I(inode)->i_state &= ~EXT4_STATE_DA_ALLOC_CLOSE;
	}
	/* if we are the last writer on the inode, drop the block reservation */
	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1) &&
		        !EXT4_I(inode)->i_reserved_data_blocks)
	{
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_discard_preallocations(inode);
		up_write(&EXT4_I(inode)->i_data_sem);
	}
	if (is_dx(inode) && filp->private_data)
		ext4_htree_free_dir_info(filp->private_data);

	return 0;
}

static ssize_t
ext4_file_write(struct kiocb *iocb, const char __user *buf,
		size_t count, loff_t pos)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_dentry->d_inode;
	ssize_t ret;
	int err;

	/*
	 * If we have encountered a bitmap-format file, the size limit
	 * is smaller than s_maxbytes, which is for extent-mapped files.
	 */

	if (!(EXT4_I(inode)->i_flags & EXT4_EXTENTS_FL)) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

		if (pos > sbi->s_bitmap_maxbytes)
			return -EFBIG;

		if (pos + count > sbi->s_bitmap_maxbytes)
			count = sbi->s_bitmap_maxbytes - pos;
	}

	ret = generic_file_aio_write(iocb, buf, count, pos);
	/*
	 * Skip flushing if there was an error, or if nothing was written.
	 */
	if (ret <= 0)
		return ret;

	/*
	 * If the inode is IS_SYNC, or is O_SYNC and we are doing data
	 * journalling then we need to make sure that we force the transaction
	 * to disk to keep all metadata uptodate synchronously.
	 */
	if (file->f_flags & O_SYNC) {
		/*
		 * If we are non-data-journaled, then the dirty data has
		 * already been flushed to backing store by generic_osync_inode,
		 * and the inode has been flushed too if there have been any
		 * modifications other than mere timestamp updates.
		 *
		 * Open question --- do we care about flushing timestamps too
		 * if the inode is IS_SYNC?
		 */
		if (!ext4_should_journal_data(inode))
			return ret;

		goto force_commit;
	}

	/*
	 * So we know that there has been no forced data flush.  If the inode
	 * is marked IS_SYNC, we need to force one ourselves.
	 */
	if (!IS_SYNC(inode))
		return ret;

	/*
	 * Open question #2 --- should we force data to disk here too?  If we
	 * don't, the only impact is that data=writeback filesystems won't
	 * flush data to disk automatically on IS_SYNC, only metadata (but
	 * historically, that is what ext2 has done.)
	 */

force_commit:
	err = ext4_force_commit(inode->i_sb);
	if (err)
		return err;
	return ret;
}

static struct vm_operations_struct ext4_file_vm_ops = {
	.nopage		= filemap_nopage,
	.populate	= filemap_populate,
	.page_mkwrite   = ext4_page_mkwrite,
};

static int ext4_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &ext4_file_vm_ops;
	return 0;
}

static int ext4_file_open(struct inode * inode, struct file * filp)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct vfsmount *mnt;
	struct dentry *mntpt;
	char buf[64], *cp;

	if (unlikely(!(sbi->s_mount_flags & EXT4_MF_MNTDIR_SAMPLED) &&
		     !(sb->s_flags & MS_RDONLY))) {
		sbi->s_mount_flags |= EXT4_MF_MNTDIR_SAMPLED;
		/*
		 * Sample where the filesystem has been mounted and
		 * store it in the superblock for sysadmin convenience
		 * when trying to sort through large numbers of block
		 * devices or filesystem images.
		 */
		memset(buf, 0, sizeof(buf));
		mnt = mntget(filp->f_vfsmnt);
		mntpt = dget(mnt->mnt_mountpoint);
		cp = d_path(mntpt, mnt, buf, sizeof(buf));
		dput(mntpt);
		mntput(mnt);
		if (!IS_ERR(cp)) {
			memcpy(sbi->s_es->s_last_mounted, cp,
			       sizeof(sbi->s_es->s_last_mounted));
			sb->s_dirt = 1;
		}
	}
	return generic_file_open(inode, filp);
}

struct file_operations ext4_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= generic_file_aio_read,
	.aio_write	= ext4_file_write,
	.readv		= generic_file_readv,
	.writev		= generic_file_writev,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.mmap		= ext4_file_mmap,
	.open		= ext4_file_open,
	.release	= ext4_release_file,
	.fsync		= ext4_sync_file,
	.sendfile	= generic_file_sendfile,
	.splice_read	= generic_file_splice_read,
	.splice_write	= generic_file_splice_write,
};

struct inode_operations ext4_file_inode_operations = {
	.truncate	= ext4_truncate,
	.setattr	= ext4_setattr,
	.getattr	= ext4_getattr,
#ifdef CONFIG_EXT4_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext4_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.permission	= ext4_permission,
	.fallocate	= ext4_fallocate,
	.fiemap		= ext4_fiemap,
};

