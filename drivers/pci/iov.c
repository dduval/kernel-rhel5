/*
 * drivers/pci/iov.c
 *
 * Copyright (C) 2009 Intel Corporation, Yu Zhao <yu.zhao@intel.com>
 *
 * PCI Express I/O Virtualization (IOV) support.
 *   Single Root IOV 1.0
 *   Address Translation Service 1.0
 */

#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/delay.h>
#include "pci.h"

#define VIRTFN_ID_LEN	16

static inline u8 virtfn_bus(struct pci_dev *dev, int id)
{
	return dev->bus->number + ((dev->devfn + dev->sriov->offset +
				    dev->sriov->stride * id) >> 8);
}

static inline u8 virtfn_devfn(struct pci_dev *dev, int id)
{
	return (dev->devfn + dev->sriov->offset +
		dev->sriov->stride * id) & 0xff;
}

static struct pci_bus *virtfn_add_bus(struct pci_bus *bus, int busnr)
{
	struct pci_bus *child;

	if (bus->number == busnr)
		return bus;

	child = pci_find_bus(pci_domain_nr(bus), busnr);
	if (child)
		return child;

	child = pci_add_new_bus(bus, NULL, busnr);
	if (!child)
		return NULL;

	child->subordinate = busnr;

	return child;
}

static void virtfn_remove_bus(struct pci_bus *bus, int busnr)
{
	struct pci_bus *child;

	if (bus->number == busnr)
		return;

	child = pci_find_bus(pci_domain_nr(bus), busnr);
	BUG_ON(!child);

	if (list_empty(&child->devices))
		pci_remove_bus(child);
}

static int virtfn_add(struct pci_dev *dev, int id)
{
	int i;
	int rc;
	u64 size;
	char buf[VIRTFN_ID_LEN];
	struct pci_dev *virtfn;
	struct resource *res;
	struct pci_sriov *iov = dev->sriov;

	virtfn = kzalloc(sizeof(struct pci_dev), GFP_KERNEL);
	if (!virtfn)
		return -ENOMEM;

	mutex_lock(&iov->dev->sriov->lock);
	virtfn->bus = virtfn_add_bus(dev->bus, virtfn_bus(dev, id));
	if (!virtfn->bus) {
		kfree(virtfn);
		mutex_unlock(&iov->dev->sriov->lock);
		return -ENOMEM;
	}
	virtfn->devfn = virtfn_devfn(dev, id);
	virtfn->vendor = dev->vendor;
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_DID, &virtfn->device);
	pci_setup_device(virtfn);
	virtfn->dev.parent = dev->dev.parent;

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		res = iov->res + i;
		if (!res->parent)
			continue;
		virtfn->resource[i].name = pci_name(virtfn);
		virtfn->resource[i].flags = res->flags;
		size = res->end - res->start + 1;
		do_div(size, iov->total);
		virtfn->resource[i].start = res->start + size * id;
		virtfn->resource[i].end = virtfn->resource[i].start + size - 1;
		rc = request_resource(res, &virtfn->resource[i]);
		BUG_ON(rc);
	}

	pci_device_add(virtfn, virtfn->bus);
	mutex_unlock(&iov->dev->sriov->lock);

	virtfn->physfn = pci_dev_get(dev);
	virtfn->is_virtfn = 1;

	pci_bus_add_device(virtfn);
	sprintf(buf, "virtfn%u", id);
	rc = sysfs_create_link(&dev->dev.kobj, &virtfn->dev.kobj, buf);
	if (rc)
		goto failed1;
	rc = sysfs_create_link(&virtfn->dev.kobj, &dev->dev.kobj, "physfn");
	if (rc)
		goto failed2;

	kobject_uevent(&virtfn->dev.kobj, KOBJ_CHANGE);

	return 0;

failed2:
	sysfs_remove_link(&dev->dev.kobj, buf);
failed1:
	pci_dev_put(dev);
	mutex_lock(&iov->dev->sriov->lock);
	pci_remove_bus_device(virtfn);
	virtfn_remove_bus(dev->bus, virtfn_bus(dev, id));
	mutex_unlock(&iov->dev->sriov->lock);

	return rc;
}

static void virtfn_remove(struct pci_dev *dev, int id)
{
	char buf[VIRTFN_ID_LEN];
	struct pci_bus *bus;
	struct pci_dev *virtfn;
	struct pci_sriov *iov = dev->sriov;

	bus = pci_find_bus(pci_domain_nr(dev->bus), virtfn_bus(dev, id));
	if (!bus)
		return;

	virtfn = pci_get_slot(bus, virtfn_devfn(dev, id));
	if (!virtfn)
		return;

	pci_dev_put(virtfn);

	sprintf(buf, "virtfn%u", id);
	sysfs_remove_link(&dev->dev.kobj, buf);
	sysfs_remove_link(&virtfn->dev.kobj, "physfn");

	mutex_lock(&iov->dev->sriov->lock);
	pci_remove_bus_device(virtfn);
	virtfn_remove_bus(dev->bus, virtfn_bus(dev, id));
	mutex_unlock(&iov->dev->sriov->lock);

	pci_dev_put(dev);
}

static int sriov_enable(struct pci_dev *dev, int nr_virtfn)
{
	int rc;
	int i, j;
	u16 offset, stride, initial;
	struct resource *res;
	struct pci_dev *pdev;
	struct pci_sriov *iov = dev->sriov;

	if (!nr_virtfn)
		return 0;

	if (iov->nr_virtfn)
		return -EINVAL;

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_INITIAL_VF, &initial);
	if (initial > iov->total ||
	    (!(iov->cap & PCI_SRIOV_CAP_VFM) && (initial != iov->total)))
		return -EIO;

	if (nr_virtfn < 0 || nr_virtfn > iov->total ||
	    (!(iov->cap & PCI_SRIOV_CAP_VFM) && (nr_virtfn > initial)))
		return -EINVAL;

	pci_write_config_word(dev, iov->pos + PCI_SRIOV_NUM_VF, nr_virtfn);
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_OFFSET, &offset);
	pci_read_config_word(dev, iov->pos + PCI_SRIOV_VF_STRIDE, &stride);
	if (!offset || (nr_virtfn > 1 && !stride))
		return -EIO;

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		res = iov->res + i;
		if (!res->flags)
			continue;
		rc = pci_assign_resource(dev, i + PCI_IOV_RESOURCES);
		if (rc) {
			dev_err(&dev->dev, "not enough MMIO resources for SR-IOV\n");
			goto failed1;
		}
	}

	iov->offset = offset;
	iov->stride = stride;

	if (virtfn_bus(dev, nr_virtfn - 1) > dev->bus->subordinate) {
		dev_err(&dev->dev, "SR-IOV: bus number out of range\n");
		return -ENOMEM;
	}

	if (iov->link != dev->devfn) {
		pdev = pci_get_slot(dev->bus, iov->link);
		if (!pdev)
			return -ENODEV;

		pci_dev_put(pdev);

		if (!pdev->is_physfn)
			return -ENODEV;

		rc = sysfs_create_link(&dev->dev.kobj,
					&pdev->dev.kobj, "dep_link");
		if (rc)
			return rc;
	}

	iov->ctrl |= PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE;
	pci_block_user_cfg_access(dev);
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);
	msleep(100);
	pci_unblock_user_cfg_access(dev);

	iov->initial = initial;
	if (nr_virtfn < initial)
		initial = nr_virtfn;

	for (i = 0; i < initial; i++) {
		rc = virtfn_add(dev, i);
		if (rc)
			goto failed2;
	}

	kobject_uevent(&dev->dev.kobj, KOBJ_CHANGE);
	iov->nr_virtfn = nr_virtfn;

	return 0;

failed2:
	for (j = 0; j < i; j++)
		virtfn_remove(dev, j);

	iov->ctrl &= ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE);
	pci_block_user_cfg_access(dev);
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);
	ssleep(1);
	pci_unblock_user_cfg_access(dev);

	if (iov->link != dev->devfn)
		sysfs_remove_link(&dev->dev.kobj, "dep_link");

failed1:
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		res = iov->res + i;
		if (res->parent)
			release_resource(res);
	}

	return rc;
}

static void sriov_disable(struct pci_dev *dev)
{
	int i;
	struct resource *res;
	struct pci_sriov *iov = dev->sriov;

	if (!iov->nr_virtfn)
		return;

	for (i = 0; i < iov->nr_virtfn; i++)
		virtfn_remove(dev, i);

	iov->ctrl &= ~(PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE);
	pci_block_user_cfg_access(dev);
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);
	ssleep(1);
	pci_unblock_user_cfg_access(dev);

	if (iov->link != dev->devfn)
		sysfs_remove_link(&dev->dev.kobj, "dep_link");

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		res = iov->res + i;
		if (res->parent)
			release_resource(res);
	}

	iov->nr_virtfn = 0;
}

static int sriov_init(struct pci_dev *dev, int pos)
{
	int i;
	int rc;
	u32 pgsz;
	u16 ctrl, total, offset, stride;
	struct pci_sriov *iov;
	struct resource *res;
	struct pci_dev *pdev;

	if (dev->pcie_type != PCI_EXP_TYPE_RC_END &&
	    dev->pcie_type != PCI_EXP_TYPE_ENDPOINT)
		return -ENODEV;

	pci_read_config_word(dev, pos + PCI_SRIOV_CTRL, &ctrl);
	if (ctrl & PCI_SRIOV_CTRL_VFE) {
		pci_write_config_word(dev, pos + PCI_SRIOV_CTRL, 0);
		ssleep(1);
	}

	pci_read_config_word(dev, pos + PCI_SRIOV_TOTAL_VF, &total);
	if (!total)
		return 0;

	ctrl = 0;
	list_for_each_entry(pdev, &dev->bus->devices, bus_list)
		if (pdev->is_physfn)
			goto found;

	pdev = NULL;
	if (pci_ari_enabled(dev->bus))
		ctrl |= PCI_SRIOV_CTRL_ARI;

found:
	pci_write_config_word(dev, pos + PCI_SRIOV_CTRL, ctrl);
	pci_write_config_word(dev, pos + PCI_SRIOV_NUM_VF, total);
	pci_read_config_word(dev, pos + PCI_SRIOV_VF_OFFSET, &offset);
	pci_read_config_word(dev, pos + PCI_SRIOV_VF_STRIDE, &stride);
	if (!offset || (total > 1 && !stride))
		return -EIO;

	pci_read_config_dword(dev, pos + PCI_SRIOV_SUP_PGSIZE, &pgsz);
	i = PAGE_SHIFT > 12 ? PAGE_SHIFT - 12 : 0;
	pgsz &= ~((1 << i) - 1);
	if (!pgsz)
		return -EIO;

	pgsz &= ~(pgsz - 1);
	pci_write_config_dword(dev, pos + PCI_SRIOV_SYS_PGSIZE, pgsz);

	iov = kzalloc(sizeof(*iov), GFP_KERNEL);
	if (!iov)
		return -ENOMEM;

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		res = iov->res + i;
		i += __pci_read_base(dev, pci_bar_unknown, res,
				     pos + PCI_SRIOV_BAR + i * 4);
		if (!res->flags)
			continue;
		if ((res->end - res->start + 1) & (PAGE_SIZE - 1)) {
			rc = -EIO;
			goto failed;
		}
		res->end = res->start + (res->end - res->start + 1) * total - 1;
	}

	iov->pos = pos;
	iov->ctrl = ctrl;
	iov->total = total;
	iov->offset = offset;
	iov->stride = stride;
	iov->pgsz = pgsz;
	iov->self = dev;
	pci_read_config_dword(dev, pos + PCI_SRIOV_CAP, &iov->cap);
	pci_read_config_byte(dev, pos + PCI_SRIOV_FUNC_LINK, &iov->link);
	if (dev->pcie_type == PCI_EXP_TYPE_RC_END)
		iov->link = PCI_DEVFN(PCI_SLOT(dev->devfn), iov->link);

	if (pdev)
		iov->dev = pci_dev_get(pdev);
	else
		iov->dev = dev;

	mutex_init(&iov->lock);

	dev->sriov = iov;
	dev->is_physfn = 1;

	return 0;

failed:
	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++) {
		res = iov->res + i;
		res->flags = 0;
	}
	kfree(iov);

	return rc;
}

static void sriov_release(struct pci_dev *dev)
{
	BUG_ON(dev->sriov->nr_virtfn);

	if (dev != dev->sriov->dev)
		pci_dev_put(dev->sriov->dev);

	mutex_destroy(&dev->sriov->lock);

	kfree(dev->sriov);
	dev->sriov = NULL;
}

static void sriov_restore_state(struct pci_dev *dev)
{
	int i;
	u16 ctrl;
	struct pci_sriov *iov = dev->sriov;

	pci_read_config_word(dev, iov->pos + PCI_SRIOV_CTRL, &ctrl);
	if (ctrl & PCI_SRIOV_CTRL_VFE)
		return;

	for (i = 0; i < PCI_SRIOV_NUM_BARS; i++)
		pci_update_resource(dev, iov->res + i, i + PCI_IOV_RESOURCES);

	pci_write_config_dword(dev, iov->pos + PCI_SRIOV_SYS_PGSIZE, iov->pgsz);
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_NUM_VF, iov->nr_virtfn);
	pci_write_config_word(dev, iov->pos + PCI_SRIOV_CTRL, iov->ctrl);
	if (iov->ctrl & PCI_SRIOV_CTRL_VFE)
		msleep(100);
}

/**
 * pci_iov_init - initialize the IOV capability
 * @dev: the PCI device
 *
 * Returns 0 on success, or negative on failure.
 */
int pci_iov_init(struct pci_dev *dev)
{
	int pos;

	if (!dev->is_pcie)
		return -ENODEV;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV);
	if (pos)
		return sriov_init(dev, pos);

	return -ENODEV;
}

/**
 * pci_iov_release - release resources used by the IOV capability
 * @dev: the PCI device
 */
void pci_iov_release(struct pci_dev *dev)
{
	if (dev->is_physfn)
		sriov_release(dev);
}

/**
 * pci_iov_resource_bar - get position of the SR-IOV BAR
 * @dev: the PCI device
 * @resno: the resource number
 * @type: the BAR type to be filled in
 *
 * Returns position of the BAR encapsulated in the SR-IOV capability.
 */
int pci_iov_resource_bar(struct pci_dev *dev, int resno,
			 enum pci_bar_type *type)
{
	if (resno < PCI_IOV_RESOURCES || resno > PCI_IOV_RESOURCE_END)
		return 0;

	BUG_ON(!dev->is_physfn);

	*type = pci_bar_unknown;

	return dev->sriov->pos + PCI_SRIOV_BAR +
		4 * (resno - PCI_IOV_RESOURCES);
}

/**
 * pci_restore_iov_state - restore the state of the IOV capability
 * @dev: the PCI device
 */
void pci_restore_iov_state(struct pci_dev *dev)
{
	if (dev->is_physfn)
		sriov_restore_state(dev);
}

/**
 * pci_iov_bus_range - find bus range used by Virtual Function
 * @bus: the PCI bus
 *
 * Returns max number of buses (exclude current one) used by Virtual
 * Functions.
 */
int pci_iov_bus_range(struct pci_bus *bus)
{
	int max = 0;
	u8 busnr;
	struct pci_dev *dev;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (!dev->is_physfn)
			continue;
		busnr = virtfn_bus(dev, dev->sriov->total - 1);
		if (busnr > max)
			max = busnr;
	}

	return max ? max - bus->number : 0;
}

/**
 * pci_enable_sriov - enable the SR-IOV capability
 * @dev: the PCI device
 * @nr_virtfn: number of virtual functions to enable
 *
 * Returns 0 on success, or negative on failure.
 */
int pci_enable_sriov(struct pci_dev *dev, int nr_virtfn)
{
	might_sleep();

	if (!dev->is_physfn)
		return -ENODEV;

	return sriov_enable(dev, nr_virtfn);
}
EXPORT_SYMBOL_GPL(pci_enable_sriov);

/**
 * pci_disable_sriov - disable the SR-IOV capability
 * @dev: the PCI device
 */
void pci_disable_sriov(struct pci_dev *dev)
{
	might_sleep();

	if (!dev->is_physfn)
		return;

	sriov_disable(dev);
}
EXPORT_SYMBOL_GPL(pci_disable_sriov);

static int ats_alloc_one(struct pci_dev *dev, int ps)
{
	int pos;
	u16 cap;
	struct pci_ats *ats;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ATS);
	if (!pos)
		return -ENODEV;

	ats = kzalloc(sizeof(*ats), GFP_KERNEL);
	if (!ats)
		return -ENOMEM;

	ats->pos = pos;
	ats->stu = ps;
	pci_read_config_word(dev, pos + PCI_ATS_CAP, &cap);
	ats->qdep = PCI_ATS_CAP_QDEP(cap) ? PCI_ATS_CAP_QDEP(cap) :
					    PCI_ATS_MAX_QDEP;
	dev->ats = ats;

	return 0;
}

static void ats_free_one(struct pci_dev *dev)
{
	kfree(dev->ats);
	dev->ats = NULL;
}

/**
 * pci_enable_ats - enable the ATS capability
 * @dev: the PCI device
 * @ps: the IOMMU page shift
 *
 * Returns 0 on success, or negative on failure.
 */
int pci_enable_ats(struct pci_dev *dev, int ps)
{
	int rc;
	u16 ctrl;

	BUG_ON(dev->ats && dev->ats->is_enabled);

	if (ps < PCI_ATS_MIN_STU)
		return -EINVAL;

	if (dev->is_physfn || dev->is_virtfn) {
		struct pci_dev *pdev = dev->is_physfn ? dev : dev->physfn;

		mutex_lock(&pdev->sriov->lock);
		if (pdev->ats)
			rc = pdev->ats->stu == ps ? 0 : -EINVAL;
		else
			rc = ats_alloc_one(pdev, ps);

		if (!rc)
			pdev->ats->ref_cnt++;
		mutex_unlock(&pdev->sriov->lock);
		if (rc)
			return rc;
	}

	if (!dev->is_physfn) {
		rc = ats_alloc_one(dev, ps);
		if (rc)
			return rc;
	}

	ctrl = PCI_ATS_CTRL_ENABLE;
	if (!dev->is_virtfn)
		ctrl |= PCI_ATS_CTRL_STU(ps - PCI_ATS_MIN_STU);
	pci_write_config_word(dev, dev->ats->pos + PCI_ATS_CTRL, ctrl);

	dev->ats->is_enabled = 1;

	return 0;
}

/**
 * pci_disable_ats - disable the ATS capability
 * @dev: the PCI device
 */
void pci_disable_ats(struct pci_dev *dev)
{
	u16 ctrl;

	BUG_ON(!dev->ats || !dev->ats->is_enabled);

	pci_read_config_word(dev, dev->ats->pos + PCI_ATS_CTRL, &ctrl);
	ctrl &= ~PCI_ATS_CTRL_ENABLE;
	pci_write_config_word(dev, dev->ats->pos + PCI_ATS_CTRL, ctrl);

	dev->ats->is_enabled = 0;

	if (dev->is_physfn || dev->is_virtfn) {
		struct pci_dev *pdev = dev->is_physfn ? dev : dev->physfn;

		mutex_lock(&pdev->sriov->lock);
		pdev->ats->ref_cnt--;
		if (!pdev->ats->ref_cnt)
			ats_free_one(pdev);
		mutex_unlock(&pdev->sriov->lock);
	}

	if (!dev->is_physfn)
		ats_free_one(dev);
}

/**
 * pci_ats_queue_depth - query the ATS Invalidate Queue Depth
 * @dev: the PCI device
 *
 * Returns the queue depth on success, or negative on failure.
 *
 * The ATS spec uses 0 in the Invalidate Queue Depth field to
 * indicate that the function can accept 32 Invalidate Request.
 * But here we use the `real' values (i.e. 1~32) for the Queue
 * Depth; and 0 indicates the function shares the Queue with
 * other functions (doesn't exclusively own a Queue).
 */
int pci_ats_queue_depth(struct pci_dev *dev)
{
	int pos;
	u16 cap;

	if (dev->is_virtfn)
		return 0;

	if (dev->ats)
		return dev->ats->qdep;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_ATS);
	if (!pos)
		return -ENODEV;

	pci_read_config_word(dev, pos + PCI_ATS_CAP, &cap);

	return PCI_ATS_CAP_QDEP(cap) ? PCI_ATS_CAP_QDEP(cap) :
				       PCI_ATS_MAX_QDEP;
}
