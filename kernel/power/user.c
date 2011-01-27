/*
 * linux/kernel/power/user.c
 *
 * This file provides the user space interface for software suspend/resume.
 *
 * Copyright (C) 2006 Rafael J. Wysocki <rjw@sisk.pl>
 *
 * This file is released under the GPLv2.
 *
 */

#include <linux/suspend.h>
#include <linux/syscalls.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pm.h>
#include <linux/fs.h>
#include <linux/cpu.h>

#include <asm/uaccess.h>

#include "power.h"

#define SNAPSHOT_MINOR	231

static struct snapshot_data {
	struct snapshot_handle handle;
	int swap;
	struct bitmap_page *bitmap;
	int mode;
	char frozen;
	char ready;
} snapshot_state;

static atomic_t device_available = ATOMIC_INIT(1);

static int snapshot_open(struct inode *inode, struct file *filp)
{
	struct snapshot_data *data;

	if (!atomic_add_unless(&device_available, -1, 0))
		return -EBUSY;

	if ((filp->f_flags & O_ACCMODE) == O_RDWR)
		return -ENOSYS;

	nonseekable_open(inode, filp);
	data = &snapshot_state;
	filp->private_data = data;
	memset(&data->handle, 0, sizeof(struct snapshot_handle));
	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		data->swap = swsusp_resume_device ? swap_type_of(swsusp_resume_device) : -1;
		data->mode = O_RDONLY;
	} else {
		data->swap = -1;
		data->mode = O_WRONLY;
	}
	data->bitmap = NULL;
	data->frozen = 0;
	data->ready = 0;

	return 0;
}

static int snapshot_release(struct inode *inode, struct file *filp)
{
	struct snapshot_data *data;

	swsusp_free();
	data = filp->private_data;
	free_all_swap_pages(data->swap, data->bitmap);
	free_bitmap(data->bitmap);
	if (data->frozen) {
		down(&pm_sem);
		thaw_processes();
		enable_nonboot_cpus();
		up(&pm_sem);
	}
	atomic_inc(&device_available);
	return 0;
}

static ssize_t snapshot_read(struct file *filp, char __user *buf,
                             size_t count, loff_t *offp)
{
	struct snapshot_data *data;
	ssize_t res;

	data = filp->private_data;
	res = snapshot_read_next(&data->handle, count);
	if (res > 0) {
		if (copy_to_user(buf, data_of(data->handle), res))
			res = -EFAULT;
		else
			*offp = data->handle.offset;
	}
	return res;
}

static ssize_t snapshot_write(struct file *filp, const char __user *buf,
                              size_t count, loff_t *offp)
{
	struct snapshot_data *data;
	ssize_t res;

	data = filp->private_data;
	res = snapshot_write_next(&data->handle, count);
	if (res > 0) {
		if (copy_from_user(data_of(data->handle), buf, res))
			res = -EFAULT;
		else
			*offp = data->handle.offset;
	}
	return res;
}

static int snapshot_ioctl(struct inode *inode, struct file *filp,
                          unsigned int cmd, unsigned long arg)
{
	int error = 0;
	struct snapshot_data *data;
	loff_t offset, avail;

	if (_IOC_TYPE(cmd) != SNAPSHOT_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SNAPSHOT_IOC_MAXNR)
		return -ENOTTY;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	data = filp->private_data;

	switch (cmd) {

	case SNAPSHOT_FREEZE:
		if (data->frozen)
			break;
		down(&pm_sem);
		error = disable_nonboot_cpus();
		if (!error) {
			error = freeze_processes();
			if (error) {
				thaw_processes();
				error = -EBUSY;
			}
		}
		enable_nonboot_cpus();
		up(&pm_sem);
		if (!error)
			data->frozen = 1;
		break;

	case SNAPSHOT_UNFREEZE:
		if (!data->frozen)
			break;
		down(&pm_sem);
		thaw_processes();
		enable_nonboot_cpus();
		up(&pm_sem);
		data->frozen = 0;
		break;

	case SNAPSHOT_ATOMIC_SNAPSHOT:
		if (data->mode != O_RDONLY || !data->frozen  || data->ready) {
			error = -EPERM;
			break;
		}
		down(&pm_sem);
		/* Free memory before shutting down devices. */
		error = swsusp_shrink_memory();
		if (!error) {
			error = device_suspend(PMSG_FREEZE);
			if (!error) {
				in_suspend = 1;
				error = swsusp_suspend();
				device_resume();
			}
		}
		up(&pm_sem);
		if (!error)
			error = put_user(in_suspend, (unsigned int __user *)arg);
		if (!error)
			data->ready = 1;
		break;

	case SNAPSHOT_ATOMIC_RESTORE:
		if (data->mode != O_WRONLY || !data->frozen ||
		    !snapshot_image_loaded(&data->handle)) {
			error = -EPERM;
			break;
		}
		down(&pm_sem);
		pm_prepare_console();
		error = device_suspend(PMSG_FREEZE);
		if (!error) {
			error = swsusp_resume();
			device_resume();
		}
		pm_restore_console();
		up(&pm_sem);
		break;

	case SNAPSHOT_FREE:
		swsusp_free();
		memset(&data->handle, 0, sizeof(struct snapshot_handle));
		data->ready = 0;
		break;

	case SNAPSHOT_SET_IMAGE_SIZE:
		image_size = arg;
		break;

	case SNAPSHOT_AVAIL_SWAP:
		avail = count_swap_pages(data->swap, 1);
		avail <<= PAGE_SHIFT;
		error = put_user(avail, (loff_t __user *)arg);
		break;

	case SNAPSHOT_GET_SWAP_PAGE:
		if (data->swap < 0 || data->swap >= MAX_SWAPFILES) {
			error = -ENODEV;
			break;
		}
		if (!data->bitmap) {
			data->bitmap = alloc_bitmap(count_swap_pages(data->swap, 0));
			if (!data->bitmap) {
				error = -ENOMEM;
				break;
			}
		}
		offset = alloc_swap_page(data->swap, data->bitmap);
		if (offset) {
			offset <<= PAGE_SHIFT;
			error = put_user(offset, (loff_t __user *)arg);
		} else {
			error = -ENOSPC;
		}
		break;

	case SNAPSHOT_FREE_SWAP_PAGES:
		if (data->swap < 0 || data->swap >= MAX_SWAPFILES) {
			error = -ENODEV;
			break;
		}
		free_all_swap_pages(data->swap, data->bitmap);
		free_bitmap(data->bitmap);
		data->bitmap = NULL;
		break;

	case SNAPSHOT_SET_SWAP_FILE:
		if (!data->bitmap) {
			/*
			 * User space encodes device types as two-byte values,
			 * so we need to recode them
			 */
			if (old_decode_dev(arg)) {
				data->swap = swap_type_of(old_decode_dev(arg));
				if (data->swap < 0)
					error = -ENODEV;
			} else {
				data->swap = -1;
				error = -EINVAL;
			}
		} else {
			error = -EPERM;
		}
		break;

	case SNAPSHOT_S2RAM:
		if (!data->frozen) {
			error = -EPERM;
			break;
		}

		if (down_trylock(&pm_sem)) {
			error = -EBUSY;
			break;
		}

		if (pm_ops->prepare) {
			error = pm_ops->prepare(PM_SUSPEND_MEM);
			if (error)
				goto OutS3;
		}

		/* Put devices to sleep */
		error = device_suspend(PMSG_SUSPEND);
		if (error) {
			printk(KERN_ERR "Failed to suspend some devices.\n");
		} else {
			/* Enter S3, system is already frozen */
			suspend_enter(PM_SUSPEND_MEM);

			/* Wake up devices */
			device_resume();
		}

		if (pm_ops->finish)
			pm_ops->finish(PM_SUSPEND_MEM);

OutS3:
		up(&pm_sem);
		break;

	default:
		error = -ENOTTY;

	}

	return error;
}

static struct file_operations snapshot_fops = {
	.open = snapshot_open,
	.release = snapshot_release,
	.read = snapshot_read,
	.write = snapshot_write,
	.llseek = no_llseek,
	.ioctl = snapshot_ioctl,
};

static struct miscdevice snapshot_device = {
	.minor = SNAPSHOT_MINOR,
	.name = "snapshot",
	.fops = &snapshot_fops,
};

static int __init snapshot_device_init(void)
{
	return misc_register(&snapshot_device);
};

device_initcall(snapshot_device_init);
