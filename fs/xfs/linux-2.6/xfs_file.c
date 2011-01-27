/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "xfs.h"
#include "xfs_bit.h"
#include "xfs_log.h"
#include "xfs_inum.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "xfs_dir2.h"
#include "xfs_trans.h"
#include "xfs_dmapi.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_alloc.h"
#include "xfs_btree.h"
#include "xfs_attr_sf.h"
#include "xfs_dir2_sf.h"
#include "xfs_dinode.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_rw.h"
#ifdef CONFIG_COMPAT
#include "xfs_ioctl32.h"
#endif
#include "xfs_vnodeops.h"

#include <linux/dcache.h>
#include <linux/smp_lock.h>

static struct vm_operations_struct xfs_file_vm_ops;

STATIC_INLINE ssize_t
__xfs_file_read(
	struct kiocb		*iocb,
	char			__user *buf,
	int			ioflags,
	size_t			count,
	loff_t			pos)
{
	struct iovec		iov = {buf, count};
	struct file		*file = iocb->ki_filp;

	BUG_ON(iocb->ki_pos != pos);
	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;
	return xfs_read(XFS_I(file->f_dentry->d_inode), iocb, &iov, 1, &iocb->ki_pos, ioflags);
}

STATIC ssize_t
xfs_file_aio_read(
	struct kiocb		*iocb,
	char			__user *buf,
	size_t			count,
	loff_t			pos)
{
	return __xfs_file_read(iocb, buf, IO_ISAIO, count, pos);
}

STATIC ssize_t
xfs_file_aio_read_invis(
	struct kiocb		*iocb,
	char			__user *buf,
	size_t			count,
	loff_t			pos)
{
	return __xfs_file_read(iocb, buf, IO_ISAIO|IO_INVIS, count, pos);
}

STATIC_INLINE ssize_t
__xfs_file_write(
	struct kiocb		*iocb,
	const char		__user *buf,
	int			ioflags,
	size_t			count,
	loff_t			pos)
{
	struct iovec	iov = {(void __user *)buf, count};
	struct file	*file = iocb->ki_filp;

	BUG_ON(iocb->ki_pos != pos);
	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;
	return xfs_write(XFS_I(file->f_mapping->host), iocb, &iov, 1,
				&iocb->ki_pos, ioflags);
}

STATIC ssize_t
xfs_file_aio_write(
	struct kiocb		*iocb,
	const char		__user *buf,
	size_t			count,
	loff_t			pos)
{
	return __xfs_file_write(iocb, buf, IO_ISAIO, count, pos);
}

STATIC ssize_t
xfs_file_aio_write_invis(
	struct kiocb		*iocb,
	const char		__user *buf,
	size_t			count,
	loff_t			pos)
{
	return __xfs_file_write(iocb, buf, IO_ISAIO|IO_INVIS, count, pos);
}

STATIC_INLINE ssize_t
__xfs_file_readv(
	struct file		*file,
	const struct iovec	*iov,
	int			ioflags,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	struct kiocb	kiocb;
	ssize_t		rval;

	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = *ppos;

	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;
	rval = xfs_read(XFS_I(file->f_dentry->d_inode), &kiocb, iov,
			nr_segs, &kiocb.ki_pos, ioflags);

	*ppos = kiocb.ki_pos;
	return rval;
}

STATIC ssize_t
xfs_file_readv(
	struct file		*file,
	const struct iovec	*iov,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	return __xfs_file_readv(file, iov, 0, nr_segs, ppos);
}

STATIC_INLINE ssize_t
__xfs_file_writev(
	struct file		*file,
	const struct iovec	*iov,
	int			ioflags,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	struct kiocb	kiocb;
	ssize_t		rval;

	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = *ppos;
	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;

	rval = xfs_write(XFS_I(file->f_mapping->host), &kiocb, iov, nr_segs,
			 &kiocb.ki_pos, ioflags);

	*ppos = kiocb.ki_pos;
	return rval;
}

STATIC ssize_t
xfs_file_writev(
	struct file		*file,
	const struct iovec	*iov,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	return __xfs_file_writev(file, iov, 0, nr_segs, ppos);
}

ssize_t
xfs_sendfile(
	struct xfs_inode	*xip,
	struct file		*filp,
	loff_t			*offset,
	int			ioflags,
	size_t			count,
	read_actor_t		actor,
	void			*target,
	cred_t			*credp)
{
	xfs_mount_t		*mp = xip->i_mount;
	ssize_t			ret;

	XFS_STATS_INC(xs_read_calls);
	if (XFS_FORCED_SHUTDOWN(mp))
		return -EIO;

	xfs_ilock(xip, XFS_IOLOCK_SHARED);

	if (DM_EVENT_ENABLED(xip, DM_EVENT_READ) &&
	    (!(ioflags & IO_INVIS))) {
		int locktype = XFS_IOLOCK_SHARED;
		int error;

		error = XFS_SEND_DATA(mp, DM_EVENT_READ, xip,
				      *offset, count,
				      FILP_DELAY_FLAG(filp), &locktype);
		if (error) {
			xfs_iunlock(xip, XFS_IOLOCK_SHARED);
			return -error;
		}
	}
	xfs_rw_enter_trace(XFS_SENDFILE_ENTER, xip,
		   (void *)(unsigned long)target, count, *offset, ioflags);
	ret = generic_file_sendfile(filp, offset, count, actor, target);
	if (ret > 0)
		XFS_STATS_ADD(xs_read_bytes, ret);

	xfs_iunlock(xip, XFS_IOLOCK_SHARED);
	return ret;
}

STATIC ssize_t
xfs_file_sendfile(
	struct file		*filp,
	loff_t			*pos,
	size_t			count,
	read_actor_t		actor,
	void			*target)
{
	return xfs_sendfile(XFS_I(filp->f_dentry->d_inode), filp, pos, 0,
			    count, actor, target, NULL);
}

STATIC ssize_t
xfs_file_sendfile_invis(
	struct file		*filp,
	loff_t			*pos,
	size_t			count,
	read_actor_t		actor,
	void			*target)
{
	return xfs_sendfile(XFS_I(filp->f_dentry->d_inode), filp, pos, IO_INVIS,
			    count, actor, target, NULL);
}

STATIC ssize_t
xfs_file_splice_read(
	struct file		*infilp,
	loff_t			*ppos,
	struct pipe_inode_info	*pipe,
	size_t			len,
	unsigned int		flags)
{
	return xfs_splice_read(XFS_I(infilp->f_dentry->d_inode),
				   infilp, ppos, pipe, len, flags, 0);
}

STATIC ssize_t
xfs_file_splice_read_invis(
	struct file		*infilp,
	loff_t			*ppos,
	struct pipe_inode_info	*pipe,
	size_t			len,
	unsigned int		flags)
{
	return xfs_splice_read(XFS_I(infilp->f_dentry->d_inode),
				   infilp, ppos, pipe, len, flags, IO_INVIS);
}

STATIC ssize_t
xfs_file_splice_write(
	struct pipe_inode_info	*pipe,
	struct file		*outfilp,
	loff_t			*ppos,
	size_t			len,
	unsigned int		flags)
{
	return xfs_splice_write(XFS_I(outfilp->f_dentry->d_inode),
				    pipe, outfilp, ppos, len, flags, 0);
}

STATIC ssize_t
xfs_file_splice_write_invis(
	struct pipe_inode_info	*pipe,
	struct file		*outfilp,
	loff_t			*ppos,
	size_t			len,
	unsigned int		flags)
{
	return xfs_splice_write(XFS_I(outfilp->f_dentry->d_inode),
				    pipe, outfilp, ppos, len, flags, IO_INVIS);
}

STATIC int
xfs_file_open(
	struct inode	*inode,
	struct file	*filp)
{
	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EFBIG;
	return -xfs_open(XFS_I(inode));
}

STATIC int
xfs_file_release(
	struct inode	*inode,
	struct file	*filp)
{
	return -xfs_release(XFS_I(inode));
}

/*
 * We ignore the datasync flag here because a datasync is effectively
 * identical to an fsync. That is, datasync implies that we need to write
 * only the metadata needed to be able to access the data that is written
 * if we crash after the call completes. Hence if we are writing beyond
 * EOF we have to log the inode size change as well, which makes it a
 * full fsync. If we don't write beyond EOF, the inode core will be
 * clean in memory and so we don't need to log the inode, just like
 * fsync.
 */
STATIC int
xfs_file_fsync(
	struct file	*filp,
	struct dentry	*dentry,
	int		datasync)
{
	xfs_iflags_clear(XFS_I(dentry->d_inode), XFS_ITRUNCATED);
	return -xfs_fsync(XFS_I(dentry->d_inode));
}

/*
 * Unfortunately we can't just use the clean and simple readdir implementation
 * below, because nfs might call back into ->lookup from the filldir callback
 * and that will deadlock the low-level btree code.
 *
 * Hopefully we'll find a better workaround that allows to use the optimal
 * version at least for local readdirs for 2.6.25.
 */
#if 0
STATIC int
xfs_file_readdir(
	struct file	*filp,
	void		*dirent,
	filldir_t	filldir)
{
	struct inode	*inode = filp->f_dentry->d_inode;
	xfs_inode_t	*ip = XFS_I(inode);
	int		error;
	size_t		bufsize;

	/*
	 * The Linux API doesn't pass down the total size of the buffer
	 * we read into down to the filesystem.  With the filldir concept
	 * it's not needed for correct information, but the XFS dir2 leaf
	 * code wants an estimate of the buffer size to calculate it's
	 * readahead window and size the buffers used for mapping to
	 * physical blocks.
	 *
	 * Try to give it an estimate that's good enough, maybe at some
	 * point we can change the ->readdir prototype to include the
	 * buffer size.
	 */
	bufsize = (size_t)min_t(loff_t, PAGE_SIZE, inode->i_size);

	error = xfs_readdir(ip, dirent, bufsize,
				(xfs_off_t *)&filp->f_pos, filldir);
	if (error)
		return -error;
	return 0;
}
#else

struct hack_dirent {
	u64		ino;
	loff_t		offset;
	int		namlen;
	unsigned int	d_type;
	char		name[];
};

struct hack_callback {
	char		*dirent;
	size_t		len;
	size_t		used;
};

STATIC int
xfs_hack_filldir(
	void		*__buf,
	const char	*name,
	int		namlen,
	loff_t		offset,
	u64		ino,
	unsigned int	d_type)
{
	struct hack_callback *buf = __buf;
	struct hack_dirent *de = (struct hack_dirent *)(buf->dirent + buf->used);
	unsigned int reclen;

	reclen = ALIGN(sizeof(struct hack_dirent) + namlen, sizeof(u64));
	if (buf->used + reclen > buf->len)
		return -EINVAL;

	de->namlen = namlen;
	de->offset = offset;
	de->ino = ino;
	de->d_type = d_type;
	memcpy(de->name, name, namlen);
	buf->used += reclen;
	return 0;
}

STATIC int
xfs_file_readdir(
	struct file	*filp,
	void		*dirent,
	filldir_t	filldir)
{
	struct inode	*inode = filp->f_dentry->d_inode;
	xfs_inode_t	*ip = XFS_I(inode);
	struct hack_callback buf;
	struct hack_dirent *de;
	int		error;
	loff_t		size;
	int		eof = 0;
	xfs_off_t       start_offset, curr_offset, offset;

	/*
	 * Try fairly hard to get memory
	 */
	buf.len = PAGE_CACHE_SIZE;
	do {
		buf.dirent = kmalloc(buf.len, GFP_KERNEL);
		if (buf.dirent)
			break;
		buf.len >>= 1;
	} while (buf.len >= 1024);

	if (!buf.dirent)
		return -ENOMEM;

	curr_offset = filp->f_pos;
	if (curr_offset == 0x7fffffff)
		offset = 0xffffffff;
	else
		offset = filp->f_pos;

	while (!eof) {
		unsigned int reclen;

		start_offset = offset;

		buf.used = 0;
		error = -xfs_readdir(ip, &buf, buf.len, &offset,
				     xfs_hack_filldir);
		if (error || offset == start_offset) {
			size = 0;
			break;
		}

		size = buf.used;
		de = (struct hack_dirent *)buf.dirent;
		while (size > 0) {
			curr_offset = de->offset /* & 0x7fffffff */;
			if (filldir(dirent, de->name, de->namlen,
					curr_offset & 0x7fffffff,
					de->ino, de->d_type)) {
				goto done;
			}

			reclen = ALIGN(sizeof(struct hack_dirent) + de->namlen,
				       sizeof(u64));
			size -= reclen;
			de = (struct hack_dirent *)((char *)de + reclen);
		}
	}

 done:
	if (!error) {
		if (size == 0)
			filp->f_pos = offset & 0x7fffffff;
		else if (de)
			filp->f_pos = curr_offset;
	}

	kfree(buf.dirent);
	return error;
}
#endif

STATIC int
xfs_file_mmap(
	struct file	*filp,
	struct vm_area_struct *vma)
{
	vma->vm_ops = &xfs_file_vm_ops;

	file_accessed(filp);
	return 0;
}

STATIC long
xfs_file_ioctl(
	struct file	*filp,
	unsigned int	cmd,
	unsigned long	p)
{
	int		error;
	struct inode	*inode = filp->f_dentry->d_inode;

	error = xfs_ioctl(XFS_I(inode), filp, 0, cmd, (void __user *)p);
	xfs_iflags_set(XFS_I(inode), XFS_IMODIFIED);

	/* NOTE:  some of the ioctl's return positive #'s as a
	 *	  byte count indicating success, such as
	 *	  readlink_by_handle.  So we don't "sign flip"
	 *	  like most other routines.  This means true
	 *	  errors need to be returned as a negative value.
	 */
	return error;
}

STATIC long
xfs_file_ioctl_invis(
	struct file	*filp,
	unsigned int	cmd,
	unsigned long	p)
{
	int		error;
	struct inode	*inode = filp->f_dentry->d_inode;

	error = xfs_ioctl(XFS_I(inode), filp, IO_INVIS, cmd, (void __user *)p);
	xfs_iflags_set(XFS_I(inode), XFS_IMODIFIED);

	/* NOTE:  some of the ioctl's return positive #'s as a
	 *	  byte count indicating success, such as
	 *	  readlink_by_handle.  So we don't "sign flip"
	 *	  like most other routines.  This means true
	 *	  errors need to be returned as a negative value.
	 */
	return error;
}

/*
 * mmap()d file has taken write protection fault and is being made
 * writable. We can set the page state up correctly for a writable
 * page, which means we can do correct delalloc accounting (ENOSPC
 * checking!) and unwritten extent mapping.
 */
STATIC int
xfs_vm_page_mkwrite(
	struct vm_area_struct	*vma,
	struct page		*page)
{
	return block_page_mkwrite(vma, page, xfs_get_blocks);
}

const struct file_operations xfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.readv		= xfs_file_readv,
	.writev		= xfs_file_writev,
	.aio_read	= xfs_file_aio_read,
	.aio_write	= xfs_file_aio_write,
	.splice_read	= xfs_file_splice_read,
	.splice_write	= xfs_file_splice_write,
	.sendfile	= xfs_file_sendfile,
	.unlocked_ioctl	= xfs_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xfs_file_compat_ioctl,
#endif
	.mmap		= xfs_file_mmap,
	.open		= xfs_file_open,
	.release	= xfs_file_release,
	.fsync		= xfs_file_fsync,
#ifdef HAVE_FOP_OPEN_EXEC
	.open_exec	= xfs_file_open_exec,
#endif
};

const struct file_operations xfs_invis_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= xfs_file_aio_read_invis,
	.aio_write	= xfs_file_aio_write_invis,
	.splice_read	= xfs_file_splice_read_invis,
	.splice_write	= xfs_file_splice_write_invis,
	.sendfile	= xfs_file_sendfile_invis,
	.unlocked_ioctl	= xfs_file_ioctl_invis,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xfs_file_compat_invis_ioctl,
#endif
	.mmap		= xfs_file_mmap,
	.open		= xfs_file_open,
	.release	= xfs_file_release,
	.fsync		= xfs_file_fsync,
};


const struct file_operations xfs_dir_file_operations = {
	.read		= generic_read_dir,
	.readdir	= xfs_file_readdir,
	.llseek		= generic_file_llseek,
	.unlocked_ioctl	= xfs_file_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= xfs_file_compat_ioctl,
#endif
	.fsync		= xfs_file_fsync,
};

static struct vm_operations_struct xfs_file_vm_ops = {
	.nopage		= filemap_nopage,
	.populate	= filemap_populate,
	.page_mkwrite	= xfs_vm_page_mkwrite,
};
