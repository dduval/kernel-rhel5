/*
 *  thinkpad_acpi.c - ThinkPad ACPI Extras
 *
 *
 *  Copyright (C) 2004-2005 Borislav Deianov <borislav@users.sf.net>
 *  Copyright (C) 2006-2008 Henrique de Moraes Holschuh <hmh@hmh.eng.br>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */

#define TPACPI_VERSION "0.19"
#define TPACPI_SYSFS_VERSION 0x020200

/*
 *  Changelog:
 *  2007-10-20		changelog trimmed down
 *
 *  2007-03-27  0.14	renamed to thinkpad_acpi and moved to
 *  			drivers/misc.
 *
 *  2006-11-22	0.13	new maintainer
 *  			changelog now lives in git commit history, and will
 *  			not be updated further in-file.
 *
 *  2005-03-17	0.11	support for 600e, 770x
 *			    thanks to Jamie Lentin <lentinj@dial.pipex.com>
 *
 *  2005-01-16	0.9	use MODULE_VERSION
 *			    thanks to Henrik Brix Andersen <brix@gentoo.org>
 *			fix parameter passing on module loading
 *			    thanks to Rusty Russell <rusty@rustcorp.com.au>
 *			    thanks to Jim Radford <radford@blackbean.org>
 *  2004-11-08	0.8	fix init error case, don't return from a macro
 *			    thanks to Chris Wright <chrisw@osdl.org>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include <linux/nvram.h>
#include <linux/proc_fs.h>
#include <linux/sysfs.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/input.h>
#include <asm/uaccess.h>

#include <linux/dmi.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#include <acpi/acpi_drivers.h>
#include <acpi/acnamesp.h>

#include <linux/pci_ids.h>

#define PCI_VENDOR_ID_LENOVO 0x17aa

/* ThinkPad CMOS commands */
#define TP_CMOS_VOLUME_DOWN	0
#define TP_CMOS_VOLUME_UP	1
#define TP_CMOS_VOLUME_MUTE	2
#define TP_CMOS_BRIGHTNESS_UP	4
#define TP_CMOS_BRIGHTNESS_DOWN	5

/* NVRAM Addresses */
enum tp_nvram_addr {
	TP_NVRAM_ADDR_HK2		= 0x57,
	TP_NVRAM_ADDR_THINKLIGHT	= 0x58,
	TP_NVRAM_ADDR_VIDEO		= 0x59,
	TP_NVRAM_ADDR_BRIGHTNESS	= 0x5e,
	TP_NVRAM_ADDR_MIXER		= 0x60,
};

/* NVRAM bit masks */
enum {
	TP_NVRAM_MASK_HKT_THINKPAD	= 0x08,
	TP_NVRAM_MASK_HKT_ZOOM		= 0x20,
	TP_NVRAM_MASK_HKT_DISPLAY	= 0x40,
	TP_NVRAM_MASK_HKT_HIBERNATE	= 0x80,
	TP_NVRAM_MASK_THINKLIGHT	= 0x10,
	TP_NVRAM_MASK_HKT_DISPEXPND	= 0x30,
	TP_NVRAM_MASK_HKT_BRIGHTNESS	= 0x20,
	TP_NVRAM_MASK_LEVEL_BRIGHTNESS	= 0x0f,
	TP_NVRAM_POS_LEVEL_BRIGHTNESS	= 0,
	TP_NVRAM_MASK_MUTE		= 0x40,
	TP_NVRAM_MASK_HKT_VOLUME	= 0x80,
	TP_NVRAM_MASK_LEVEL_VOLUME	= 0x0f,
	TP_NVRAM_POS_LEVEL_VOLUME	= 0,
};

/* ACPI HIDs */
#define TPACPI_ACPI_HKEY_HID		"IBM0068"

/* Input IDs */
#define TPACPI_HKEY_INPUT_PRODUCT	0x5054 /* "TP" */
#define TPACPI_HKEY_INPUT_VERSION	0x4101


/****************************************************************************
 * Main driver
 */

#define TPACPI_NAME "thinkpad"
#define TPACPI_DESC "ThinkPad ACPI Extras"
#define TPACPI_FILE TPACPI_NAME "_acpi"
#define TPACPI_URL "http://ibm-acpi.sf.net/"
#define TPACPI_MAIL "ibm-acpi-devel@lists.sourceforge.net"

#define TPACPI_PROC_DIR "ibm"
#define TPACPI_ACPI_EVENT_PREFIX "ibm"
#define TPACPI_DRVR_NAME TPACPI_FILE
#define TPACPI_HWMON_DRVR_NAME TPACPI_NAME "_hwmon"

#define TPACPI_MAX_ACPI_ARGS 3

/* Debugging */
#define TPACPI_LOG TPACPI_FILE ": "
#define TPACPI_ERR	   KERN_ERR    TPACPI_LOG
#define TPACPI_NOTICE KERN_NOTICE TPACPI_LOG
#define TPACPI_INFO   KERN_INFO   TPACPI_LOG
#define TPACPI_DEBUG  KERN_DEBUG  TPACPI_LOG

#define TPACPI_DBG_ALL		0xffff
#define TPACPI_DBG_ALL		0xffff
#define TPACPI_DBG_INIT		0x0001
#define TPACPI_DBG_EXIT		0x0002
#define dbg_printk(a_dbg_level, format, arg...) \
	do { if (dbg_level & a_dbg_level) \
		printk(TPACPI_DEBUG "%s: " format, __func__ , ## arg); \
	} while (0)
#ifdef CONFIG_THINKPAD_ACPI_DEBUG
#define vdbg_printk(a_dbg_level, format, arg...) \
	dbg_printk(a_dbg_level, format, ## arg)
static const char *str_supported(int is_supported);
#else
#define vdbg_printk(a_dbg_level, format, arg...)
#endif

#define onoff(status, bit) ((status) & (1 << (bit)) ? "on" : "off")
#define enabled(status, bit) ((status) & (1 << (bit)) ? "enabled" : "disabled")
#define strlencmp(a, b) (strncmp((a), (b), strlen(b)))


/****************************************************************************
 * Driver-wide structs and misc. variables
 */

struct ibm_struct;

struct tp_acpi_drv_struct {
	char *hid;
	struct acpi_driver *driver;

	void (*notify) (struct ibm_struct *, u32);
	acpi_handle *handle;
	u32 type;
	struct acpi_device *device;
};

struct ibm_struct {
	char *name;

	int (*read) (char *);
	int (*write) (char *);
	void (*exit) (void);
	void (*resume) (void);
	void (*suspend) (pm_message_t state);

	struct list_head all_drivers;

	struct tp_acpi_drv_struct *acpi;

	struct {
		u8 acpi_driver_registered:1;
		u8 acpi_notify_installed:1;
		u8 proc_created:1;
		u8 init_called:1;
		u8 experimental:1;
	} flags;
};

struct ibm_init_struct {
	char param[32];

	int (*init) (struct ibm_init_struct *);
	struct ibm_struct *data;
};

static struct {
#ifdef CONFIG_THINKPAD_ACPI_BAY
	u32 bay_status:1;
	u32 bay_eject:1;
	u32 bay_status2:1;
	u32 bay_eject2:1;
#endif
	u32 bluetooth:1;
	u32 hotkey:1;
	u32 hotkey_mask:1;
	u32 hotkey_wlsw:1;
	u32 hotkey_tablet:1;
	u32 light:1;
	u32 light_status:1;
	u32 bright_16levels:1;
	u32 wan:1;
	u32 fan_ctrl_status_undef:1;
	u32 input_device_registered:1;
	u32 platform_drv_registered:1;
	u32 platform_drv_attrs_registered:1;
	u32 sensors_pdrv_registered:1;
	u32 sensors_pdrv_attrs_registered:1;
	u32 sensors_pdev_attrs_registered:1;
	u32 hotkey_poll_active:1;
} tp_features;

struct thinkpad_id_data {
	unsigned int vendor;	/* ThinkPad vendor:
				 * PCI_VENDOR_ID_IBM/PCI_VENDOR_ID_LENOVO */

	char *bios_version_str;	/* Something like 1ZET51WW (1.03z) */
	char *ec_version_str;	/* Something like 1ZHT51WW-1.04a */

	u16 bios_model;		/* Big Endian, TP-1Y = 0x5931, 0 = unknown */
	u16 ec_model;

	char *model_str;
};
static struct thinkpad_id_data thinkpad_id;

static enum {
	TPACPI_LIFE_INIT = 0,
	TPACPI_LIFE_RUNNING,
	TPACPI_LIFE_EXITING,
} tpacpi_lifecycle;

static int experimental;
static u32 dbg_level;

/****************************************************************************
 ****************************************************************************
 *
 * ACPI Helpers and device model
 *
 ****************************************************************************
 ****************************************************************************/

/*************************************************************************
 * ACPI basic handles
 */

static acpi_handle root_handle;

#define TPACPI_HANDLE(object, parent, paths...)			\
	static acpi_handle  object##_handle;			\
	static acpi_handle *object##_parent = &parent##_handle;	\
	static char        *object##_path;			\
	static char        *object##_paths[] = { paths }

TPACPI_HANDLE(ec, root, "\\_SB.PCI0.ISA.EC0",	/* 240, 240x */
	   "\\_SB.PCI.ISA.EC",	/* 570 */
	   "\\_SB.PCI0.ISA0.EC0",	/* 600e/x, 770e, 770x */
	   "\\_SB.PCI0.ISA.EC",	/* A21e, A2xm/p, T20-22, X20-21 */
	   "\\_SB.PCI0.AD4S.EC0",	/* i1400, R30 */
	   "\\_SB.PCI0.ICH3.EC0",	/* R31 */
	   "\\_SB.PCI0.LPC.EC",	/* all others */
	   );

TPACPI_HANDLE(ecrd, ec, "ECRD");	/* 570 */
TPACPI_HANDLE(ecwr, ec, "ECWR");	/* 570 */

TPACPI_HANDLE(cmos, root, "\\UCMS",	/* R50, R50e, R50p, R51, */
					/* T4x, X31, X40 */
	   "\\CMOS",		/* A3x, G4x, R32, T23, T30, X22-24, X30 */
	   "\\CMS",		/* R40, R40e */
	   );			/* all others */

TPACPI_HANDLE(hkey, ec, "\\_SB.HKEY",	/* 600e/x, 770e, 770x */
	   "^HKEY",		/* R30, R31 */
	   "HKEY",		/* all others */
	   );			/* 570 */

TPACPI_HANDLE(vid, root, "\\_SB.PCI.AGP.VGA",	/* 570 */
	   "\\_SB.PCI0.AGP0.VID0",	/* 600e/x, 770x */
	   "\\_SB.PCI0.VID0",	/* 770e */
	   "\\_SB.PCI0.VID",	/* A21e, G4x, R50e, X30, X40 */
	   "\\_SB.PCI0.AGP.VID",	/* all others */
	   );				/* R30, R31 */


/*************************************************************************
 * ACPI helpers
 */

static int acpi_evalf(acpi_handle handle,
		      void *res, char *method, char *fmt, ...)
{
	char *fmt0 = fmt;
	struct acpi_object_list params;
	union acpi_object in_objs[TPACPI_MAX_ACPI_ARGS];
	struct acpi_buffer result, *resultp;
	union acpi_object out_obj;
	acpi_status status;
	va_list ap;
	char res_type;
	int success;
	int quiet;

	if (!*fmt) {
		printk(TPACPI_ERR "acpi_evalf() called with empty format\n");
		return 0;
	}

	if (*fmt == 'q') {
		quiet = 1;
		fmt++;
	} else
		quiet = 0;

	res_type = *(fmt++);

	params.count = 0;
	params.pointer = &in_objs[0];

	va_start(ap, fmt);
	while (*fmt) {
		char c = *(fmt++);
		switch (c) {
		case 'd':	/* int */
			in_objs[params.count].integer.value = va_arg(ap, int);
			in_objs[params.count++].type = ACPI_TYPE_INTEGER;
			break;
			/* add more types as needed */
		default:
			printk(TPACPI_ERR "acpi_evalf() called "
			       "with invalid format character '%c'\n", c);
			return 0;
		}
	}
	va_end(ap);

	if (res_type != 'v') {
		result.length = sizeof(out_obj);
		result.pointer = &out_obj;
		resultp = &result;
	} else
		resultp = NULL;

	status = acpi_evaluate_object(handle, method, &params, resultp);

	switch (res_type) {
	case 'd':		/* int */
		if (res)
			*(int *)res = out_obj.integer.value;
		success = status == AE_OK && out_obj.type == ACPI_TYPE_INTEGER;
		break;
	case 'v':		/* void */
		success = status == AE_OK;
		break;
		/* add more types as needed */
	default:
		printk(TPACPI_ERR "acpi_evalf() called "
		       "with invalid format character '%c'\n", res_type);
		return 0;
	}

	if (!success && !quiet)
		printk(TPACPI_ERR "acpi_evalf(%s, %s, ...) failed: %d\n",
		       method, fmt0, status);

	return success;
}

static int acpi_ec_read(int i, u8 *p)
{
	int v;

	if (ecrd_handle) {
		if (!acpi_evalf(ecrd_handle, &v, NULL, "dd", i))
			return 0;
		*p = v;
	} else {
		if (ec_read(i, p) < 0)
			return 0;
	}

	return 1;
}

static int acpi_ec_write(int i, u8 v)
{
	if (ecwr_handle) {
		if (!acpi_evalf(ecwr_handle, NULL, NULL, "vdd", i, v))
			return 0;
	} else {
		if (ec_write(i, v) < 0)
			return 0;
	}

	return 1;
}

#if defined(CONFIG_THINKPAD_ACPI_DOCK) || defined(CONFIG_THINKPAD_ACPI_BAY)
static int _sta(acpi_handle handle)
{
	int status;

	if (!handle || !acpi_evalf(handle, &status, "_STA", "d"))
		status = 0;

	return status;
}
#endif

static int issue_thinkpad_cmos_command(int cmos_cmd)
{
	if (!cmos_handle)
		return -ENXIO;

	if (!acpi_evalf(cmos_handle, NULL, NULL, "vd", cmos_cmd))
		return -EIO;

	return 0;
}

/*************************************************************************
 * ACPI device model
 */

#define TPACPI_ACPIHANDLE_INIT(object) \
	drv_acpi_handle_init(#object, &object##_handle, *object##_parent, \
		object##_paths, ARRAY_SIZE(object##_paths), &object##_path)

static void drv_acpi_handle_init(char *name,
			   acpi_handle *handle, acpi_handle parent,
			   char **paths, int num_paths, char **path)
{
	int i;
	acpi_status status;

	vdbg_printk(TPACPI_DBG_INIT, "trying to locate ACPI handle for %s\n",
		name);

	for (i = 0; i < num_paths; i++) {
		status = acpi_get_handle(parent, paths[i], handle);
		if (ACPI_SUCCESS(status)) {
			*path = paths[i];
			dbg_printk(TPACPI_DBG_INIT,
				   "Found ACPI handle %s for %s\n",
				   *path, name);
			return;
		}
	}

	vdbg_printk(TPACPI_DBG_INIT, "ACPI handle for %s not found\n",
		    name);
	*handle = NULL;
}

static void dispatch_acpi_notify(acpi_handle handle, u32 event, void *data)
{
	struct ibm_struct *ibm = data;

	if (tpacpi_lifecycle != TPACPI_LIFE_RUNNING)
		return;

	if (!ibm || !ibm->acpi || !ibm->acpi->notify)
		return;

	ibm->acpi->notify(ibm, event);
}

static int __init setup_acpi_notify(struct ibm_struct *ibm)
{
	acpi_status status;
	int rc;

	BUG_ON(!ibm->acpi);

	if (!*ibm->acpi->handle)
		return 0;

	vdbg_printk(TPACPI_DBG_INIT,
		"setting up ACPI notify for %s\n", ibm->name);

	rc = acpi_bus_get_device(*ibm->acpi->handle, &ibm->acpi->device);
	if (rc < 0) {
		printk(TPACPI_ERR "acpi_bus_get_device(%s) failed: %d\n",
			ibm->name, rc);
		return -ENODEV;
	}

	acpi_driver_data(ibm->acpi->device) = ibm;
	sprintf(acpi_device_class(ibm->acpi->device), "%s/%s",
		TPACPI_ACPI_EVENT_PREFIX,
		ibm->name);

	status = acpi_install_notify_handler(*ibm->acpi->handle,
			ibm->acpi->type, dispatch_acpi_notify, ibm);
	if (ACPI_FAILURE(status)) {
		if (status == AE_ALREADY_EXISTS) {
			printk(TPACPI_NOTICE
			       "another device driver is already "
			       "handling %s events\n", ibm->name);
		} else {
			printk(TPACPI_ERR
			       "acpi_install_notify_handler(%s) failed: %d\n",
			       ibm->name, status);
		}
		return -ENODEV;
	}
	ibm->flags.acpi_notify_installed = 1;
	return 0;
}

static int __init tpacpi_device_add(struct acpi_device *device)
{
	return 0;
}

static int __init register_tpacpi_subdriver(struct ibm_struct *ibm)
{
	int rc;

	dbg_printk(TPACPI_DBG_INIT,
		"registering %s as an ACPI driver\n", ibm->name);

	BUG_ON(!ibm->acpi);

	ibm->acpi->driver = kzalloc(sizeof(struct acpi_driver), GFP_KERNEL);
	if (!ibm->acpi->driver) {
		printk(TPACPI_ERR "kzalloc(ibm->driver) failed\n");
		return -ENOMEM;
	}

	sprintf(ibm->acpi->driver->name, "%s_%s", TPACPI_NAME, ibm->name);
	ibm->acpi->driver->ids = ibm->acpi->hid;

	ibm->acpi->driver->ops.add = &tpacpi_device_add;

	rc = acpi_bus_register_driver(ibm->acpi->driver);
	if (rc < 0) {
		printk(TPACPI_ERR "acpi_bus_register_driver(%s) failed: %d\n",
		       ibm->name, rc);
		kfree(ibm->acpi->driver);
		ibm->acpi->driver = NULL;
	} else if (!rc)
		ibm->flags.acpi_driver_registered = 1;

	return rc;
}


/****************************************************************************
 ****************************************************************************
 *
 * Procfs Helpers
 *
 ****************************************************************************
 ****************************************************************************/

static int dispatch_procfs_read(char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	struct ibm_struct *ibm = data;
	int len;

	if (!ibm || !ibm->read)
		return -EINVAL;

	len = ibm->read(page);
	if (len < 0)
		return len;

	if (len <= off + count)
		*eof = 1;
	*start = page + off;
	len -= off;
	if (len > count)
		len = count;
	if (len < 0)
		len = 0;

	return len;
}

static int dispatch_procfs_write(struct file *file,
			const char __user *userbuf,
			unsigned long count, void *data)
{
	struct ibm_struct *ibm = data;
	char *kernbuf;
	int ret;

	if (!ibm || !ibm->write)
		return -EINVAL;

	kernbuf = kmalloc(count + 2, GFP_KERNEL);
	if (!kernbuf)
		return -ENOMEM;

	if (copy_from_user(kernbuf, userbuf, count)) {
		kfree(kernbuf);
		return -EFAULT;
	}

	kernbuf[count] = 0;
	strcat(kernbuf, ",");
	ret = ibm->write(kernbuf);
	if (ret == 0)
		ret = count;

	kfree(kernbuf);

	return ret;
}

static char *next_cmd(char **cmds)
{
	char *start = *cmds;
	char *end;

	while ((end = strchr(start, ',')) && end == start)
		start = end + 1;

	if (!end)
		return NULL;

	*end = 0;
	*cmds = end + 1;
	return start;
}


/****************************************************************************
 ****************************************************************************
 *
 * Device model: input, hwmon and platform
 *
 ****************************************************************************
 ****************************************************************************/

static struct platform_device *tpacpi_pdev;
static struct platform_device *tpacpi_sensors_pdev;
static struct class_device *tpacpi_hwmon;
static struct input_dev *tpacpi_inputdev;
static struct mutex tpacpi_inputdev_send_mutex;
static LIST_HEAD(tpacpi_all_drivers);

static int tpacpi_suspend_handler(struct platform_device *pdev,
				  pm_message_t state)
{
	struct ibm_struct *ibm, *itmp;

	list_for_each_entry_safe(ibm, itmp,
				 &tpacpi_all_drivers,
				 all_drivers) {
		if (ibm->suspend)
			(ibm->suspend)(state);
	}

	return 0;
}

static int tpacpi_resume_handler(struct platform_device *pdev)
{
	struct ibm_struct *ibm, *itmp;

	list_for_each_entry_safe(ibm, itmp,
				 &tpacpi_all_drivers,
				 all_drivers) {
		if (ibm->resume)
			(ibm->resume)();
	}

	return 0;
}

static struct platform_driver tpacpi_pdriver = {
	.driver = {
		.name = TPACPI_DRVR_NAME,
		.owner = THIS_MODULE,
	},
	.suspend = tpacpi_suspend_handler,
	.resume = tpacpi_resume_handler,
};

static struct platform_driver tpacpi_hwmon_pdriver = {
	.driver = {
		.name = TPACPI_HWMON_DRVR_NAME,
		.owner = THIS_MODULE,
	},
};

/*************************************************************************
 * sysfs support helpers
 */

struct attribute_set {
	unsigned int members, max_members;
	struct attribute_group group;
};

struct attribute_set_obj {
	struct attribute_set s;
	struct attribute *a;
} __attribute__((packed));

static struct attribute_set *create_attr_set(unsigned int max_members,
						const char *name)
{
	struct attribute_set_obj *sobj;

	if (max_members == 0)
		return NULL;

	/* Allocates space for implicit NULL at the end too */
	sobj = kzalloc(sizeof(struct attribute_set_obj) +
		    max_members * sizeof(struct attribute *),
		    GFP_KERNEL);
	if (!sobj)
		return NULL;
	sobj->s.max_members = max_members;
	sobj->s.group.attrs = &sobj->a;
	sobj->s.group.name = name;

	return &sobj->s;
}

#define destroy_attr_set(_set) \
	kfree(_set);

/* not multi-threaded safe, use it in a single thread per set */
static int add_to_attr_set(struct attribute_set *s, struct attribute *attr)
{
	if (!s || !attr)
		return -EINVAL;

	if (s->members >= s->max_members)
		return -ENOMEM;

	s->group.attrs[s->members] = attr;
	s->members++;

	return 0;
}

static int add_many_to_attr_set(struct attribute_set *s,
			struct attribute **attr,
			unsigned int count)
{
	int i, res;

	for (i = 0; i < count; i++) {
		res = add_to_attr_set(s, attr[i]);
		if (res)
			return res;
	}

	return 0;
}

static void delete_attr_set(struct attribute_set *s, struct kobject *kobj)
{
	sysfs_remove_group(kobj, &s->group);
	destroy_attr_set(s);
}

#define register_attr_set_with_sysfs(_attr_set, _kobj) \
	sysfs_create_group(_kobj, &_attr_set->group)

static int parse_strtoul(const char *buf,
		unsigned long max, unsigned long *value)
{
	char *endp;

	while (*buf && isspace(*buf))
		buf++;
	*value = simple_strtoul(buf, &endp, 0);
	while (*endp && isspace(*endp))
		endp++;
	if (*endp || *value > max)
		return -EINVAL;

	return 0;
}

/*************************************************************************
 * thinkpad-acpi driver attributes
 */

/* interface_version --------------------------------------------------- */
static ssize_t tpacpi_driver_interface_version_show(
				struct device_driver *drv,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%08x\n", TPACPI_SYSFS_VERSION);
}

static DRIVER_ATTR(interface_version, S_IRUGO,
		tpacpi_driver_interface_version_show, NULL);

/* debug_level --------------------------------------------------------- */
static ssize_t tpacpi_driver_debug_show(struct device_driver *drv,
						char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%04x\n", dbg_level);
}

static ssize_t tpacpi_driver_debug_store(struct device_driver *drv,
						const char *buf, size_t count)
{
	unsigned long t;

	if (parse_strtoul(buf, 0xffff, &t))
		return -EINVAL;

	dbg_level = t;

	return count;
}

static DRIVER_ATTR(debug_level, S_IWUSR | S_IRUGO,
		tpacpi_driver_debug_show, tpacpi_driver_debug_store);

/* version ------------------------------------------------------------- */
static ssize_t tpacpi_driver_version_show(struct device_driver *drv,
						char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s v%s\n",
			TPACPI_DESC, TPACPI_VERSION);
}

static DRIVER_ATTR(version, S_IRUGO,
		tpacpi_driver_version_show, NULL);

/* --------------------------------------------------------------------- */

static struct driver_attribute *tpacpi_driver_attributes[] = {
	&driver_attr_debug_level, &driver_attr_version,
	&driver_attr_interface_version,
};

static int __init tpacpi_create_driver_attributes(struct device_driver *drv)
{
	int i, res;

	i = 0;
	res = 0;
	while (!res && i < ARRAY_SIZE(tpacpi_driver_attributes)) {
		res = driver_create_file(drv, tpacpi_driver_attributes[i]);
		i++;
	}

	return res;
}

static void tpacpi_remove_driver_attributes(struct device_driver *drv)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tpacpi_driver_attributes); i++)
		driver_remove_file(drv, tpacpi_driver_attributes[i]);
}

/****************************************************************************
 ****************************************************************************
 *
 * Subdrivers
 *
 ****************************************************************************
 ****************************************************************************/

/*************************************************************************
 * thinkpad-acpi init subdriver
 */

static int __init thinkpad_acpi_driver_init(struct ibm_init_struct *iibm)
{
	printk(TPACPI_INFO "%s v%s\n", TPACPI_DESC, TPACPI_VERSION);
	printk(TPACPI_INFO "%s\n", TPACPI_URL);

	printk(TPACPI_INFO "ThinkPad BIOS %s, EC %s\n",
		(thinkpad_id.bios_version_str) ?
			thinkpad_id.bios_version_str : "unknown",
		(thinkpad_id.ec_version_str) ?
			thinkpad_id.ec_version_str : "unknown");

	if (thinkpad_id.vendor && thinkpad_id.model_str)
		printk(TPACPI_INFO "%s %s\n",
			(thinkpad_id.vendor == PCI_VENDOR_ID_IBM) ?
				"IBM" : ((thinkpad_id.vendor ==
						PCI_VENDOR_ID_LENOVO) ?
					"Lenovo" : "Unknown vendor"),
			thinkpad_id.model_str);

	return 0;
}

static int thinkpad_acpi_driver_read(char *p)
{
	int len = 0;

	len += sprintf(p + len, "driver:\t\t%s\n", TPACPI_DESC);
	len += sprintf(p + len, "version:\t%s\n", TPACPI_VERSION);

	return len;
}

static struct ibm_struct thinkpad_acpi_driver_data = {
	.name = "driver",
	.read = thinkpad_acpi_driver_read,
};

/*************************************************************************
 * Hotkey subdriver
 */

enum {	/* hot key scan codes (derived from ACPI DSDT) */
	TP_ACPI_HOTKEYSCAN_FNF1		= 0,
	TP_ACPI_HOTKEYSCAN_FNF2,
	TP_ACPI_HOTKEYSCAN_FNF3,
	TP_ACPI_HOTKEYSCAN_FNF4,
	TP_ACPI_HOTKEYSCAN_FNF5,
	TP_ACPI_HOTKEYSCAN_FNF6,
	TP_ACPI_HOTKEYSCAN_FNF7,
	TP_ACPI_HOTKEYSCAN_FNF8,
	TP_ACPI_HOTKEYSCAN_FNF9,
	TP_ACPI_HOTKEYSCAN_FNF10,
	TP_ACPI_HOTKEYSCAN_FNF11,
	TP_ACPI_HOTKEYSCAN_FNF12,
	TP_ACPI_HOTKEYSCAN_FNBACKSPACE,
	TP_ACPI_HOTKEYSCAN_FNINSERT,
	TP_ACPI_HOTKEYSCAN_FNDELETE,
	TP_ACPI_HOTKEYSCAN_FNHOME,
	TP_ACPI_HOTKEYSCAN_FNEND,
	TP_ACPI_HOTKEYSCAN_FNPAGEUP,
	TP_ACPI_HOTKEYSCAN_FNPAGEDOWN,
	TP_ACPI_HOTKEYSCAN_FNSPACE,
	TP_ACPI_HOTKEYSCAN_VOLUMEUP,
	TP_ACPI_HOTKEYSCAN_VOLUMEDOWN,
	TP_ACPI_HOTKEYSCAN_MUTE,
	TP_ACPI_HOTKEYSCAN_THINKPAD,
};

enum {	/* Keys available through NVRAM polling */
	TPACPI_HKEY_NVRAM_KNOWN_MASK = 0x00fb88c0U,
	TPACPI_HKEY_NVRAM_GOOD_MASK  = 0x00fb8000U,
};

enum {	/* Positions of some of the keys in hotkey masks */
	TP_ACPI_HKEY_DISPSWTCH_MASK	= 1 << TP_ACPI_HOTKEYSCAN_FNF7,
	TP_ACPI_HKEY_DISPXPAND_MASK	= 1 << TP_ACPI_HOTKEYSCAN_FNF8,
	TP_ACPI_HKEY_HIBERNATE_MASK	= 1 << TP_ACPI_HOTKEYSCAN_FNF12,
	TP_ACPI_HKEY_BRGHTUP_MASK	= 1 << TP_ACPI_HOTKEYSCAN_FNHOME,
	TP_ACPI_HKEY_BRGHTDWN_MASK	= 1 << TP_ACPI_HOTKEYSCAN_FNEND,
	TP_ACPI_HKEY_THNKLGHT_MASK	= 1 << TP_ACPI_HOTKEYSCAN_FNPAGEUP,
	TP_ACPI_HKEY_ZOOM_MASK		= 1 << TP_ACPI_HOTKEYSCAN_FNSPACE,
	TP_ACPI_HKEY_VOLUP_MASK		= 1 << TP_ACPI_HOTKEYSCAN_VOLUMEUP,
	TP_ACPI_HKEY_VOLDWN_MASK	= 1 << TP_ACPI_HOTKEYSCAN_VOLUMEDOWN,
	TP_ACPI_HKEY_MUTE_MASK		= 1 << TP_ACPI_HOTKEYSCAN_MUTE,
	TP_ACPI_HKEY_THINKPAD_MASK	= 1 << TP_ACPI_HOTKEYSCAN_THINKPAD,
};

enum {	/* NVRAM to ACPI HKEY group map */
	TP_NVRAM_HKEY_GROUP_HK2		= TP_ACPI_HKEY_THINKPAD_MASK |
					  TP_ACPI_HKEY_ZOOM_MASK |
					  TP_ACPI_HKEY_DISPSWTCH_MASK |
					  TP_ACPI_HKEY_HIBERNATE_MASK,
	TP_NVRAM_HKEY_GROUP_BRIGHTNESS	= TP_ACPI_HKEY_BRGHTUP_MASK |
					  TP_ACPI_HKEY_BRGHTDWN_MASK,
	TP_NVRAM_HKEY_GROUP_VOLUME	= TP_ACPI_HKEY_VOLUP_MASK |
					  TP_ACPI_HKEY_VOLDWN_MASK |
					  TP_ACPI_HKEY_MUTE_MASK,
};

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
struct tp_nvram_state {
       u16 thinkpad_toggle:1;
       u16 zoom_toggle:1;
       u16 display_toggle:1;
       u16 thinklight_toggle:1;
       u16 hibernate_toggle:1;
       u16 displayexp_toggle:1;
       u16 display_state:1;
       u16 brightness_toggle:1;
       u16 volume_toggle:1;
       u16 mute:1;

       u8 brightness_level;
       u8 volume_level;
};

static struct task_struct *tpacpi_hotkey_task;
static u32 hotkey_source_mask;		/* bit mask 0=ACPI,1=NVRAM */
static int hotkey_poll_freq = 10;	/* Hz */
static struct mutex hotkey_thread_mutex;
static struct mutex hotkey_thread_data_mutex;
static unsigned int hotkey_config_change;

#else /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */

#define hotkey_source_mask 0U

#endif /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */

static struct mutex hotkey_mutex;

static enum {	/* Reasons for waking up */
	TP_ACPI_WAKEUP_NONE = 0,	/* None or unknown */
	TP_ACPI_WAKEUP_BAYEJ,		/* Bay ejection request */
	TP_ACPI_WAKEUP_UNDOCK,		/* Undock request */
} hotkey_wakeup_reason;

static int hotkey_autosleep_ack;

static int hotkey_orig_status;
static u32 hotkey_orig_mask;
static u32 hotkey_all_mask;
static u32 hotkey_reserved_mask;
static u32 hotkey_mask;

static unsigned int hotkey_report_mode;

static u16 *hotkey_keycode_map;

static struct attribute_set *hotkey_dev_attributes;

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
#define HOTKEY_CONFIG_CRITICAL_START \
	do { \
		mutex_lock(&hotkey_thread_data_mutex); \
		hotkey_config_change++; \
	} while (0);
#define HOTKEY_CONFIG_CRITICAL_END \
	mutex_unlock(&hotkey_thread_data_mutex);
#else
#define HOTKEY_CONFIG_CRITICAL_START
#define HOTKEY_CONFIG_CRITICAL_END
#endif /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */

/* HKEY.MHKG() return bits */
#define TP_HOTKEY_TABLET_MASK (1 << 3)

static int hotkey_get_wlsw(int *status)
{
	if (!acpi_evalf(hkey_handle, status, "WLSW", "d"))
		return -EIO;
	return 0;
}

static int hotkey_get_tablet_mode(int *status)
{
	int s;

	if (!acpi_evalf(hkey_handle, &s, "MHKG", "d"))
		return -EIO;

	*status = ((s & TP_HOTKEY_TABLET_MASK) != 0);
	return 0;
}

/*
 * Call with hotkey_mutex held
 */
static int hotkey_mask_get(void)
{
	u32 m = 0;

	if (tp_features.hotkey_mask) {
		if (!acpi_evalf(hkey_handle, &m, "DHKN", "d"))
			return -EIO;
	}
	hotkey_mask = m | (hotkey_source_mask & hotkey_mask);

	return 0;
}

/*
 * Call with hotkey_mutex held
 */
static int hotkey_mask_set(u32 mask)
{
	int i;
	int rc = 0;

	if (tp_features.hotkey_mask) {
		HOTKEY_CONFIG_CRITICAL_START
		for (i = 0; i < 32; i++) {
			u32 m = 1 << i;
			/* enable in firmware mask only keys not in NVRAM
			 * mode, but enable the key in the cached hotkey_mask
			 * regardless of mode, or the key will end up
			 * disabled by hotkey_mask_get() */
			if (!acpi_evalf(hkey_handle,
					NULL, "MHKM", "vdd", i + 1,
					!!((mask & ~hotkey_source_mask) & m))) {
				rc = -EIO;
				break;
			} else {
				hotkey_mask = (hotkey_mask & ~m) | (mask & m);
			}
		}
		HOTKEY_CONFIG_CRITICAL_END

		/* hotkey_mask_get must be called unconditionally below */
		if (!hotkey_mask_get() && !rc &&
		    (hotkey_mask & ~hotkey_source_mask) !=
		     (mask & ~hotkey_source_mask)) {
			printk(TPACPI_NOTICE
			       "requested hot key mask 0x%08x, but "
			       "firmware forced it to 0x%08x\n",
			       mask, hotkey_mask);
		}
	} else {
#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
		HOTKEY_CONFIG_CRITICAL_START
		hotkey_mask = mask & hotkey_source_mask;
		HOTKEY_CONFIG_CRITICAL_END
		hotkey_mask_get();
		if (hotkey_mask != mask) {
			printk(TPACPI_NOTICE
			       "requested hot key mask 0x%08x, "
			       "forced to 0x%08x (NVRAM poll mask is "
			       "0x%08x): no firmware mask support\n",
			       mask, hotkey_mask, hotkey_source_mask);
		}
#else
		hotkey_mask_get();
		rc = -ENXIO;
#endif /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */
	}

	return rc;
}

static int hotkey_status_get(int *status)
{
	if (!acpi_evalf(hkey_handle, status, "DHKC", "d"))
		return -EIO;

	return 0;
}

static int hotkey_status_set(int status)
{
	if (!acpi_evalf(hkey_handle, NULL, "MHKC", "vd", status))
		return -EIO;

	return 0;
}

static void tpacpi_input_send_radiosw(void)
{
	int wlsw;

	if (tp_features.hotkey_wlsw && !hotkey_get_wlsw(&wlsw)) {
		mutex_lock(&tpacpi_inputdev_send_mutex);

		input_report_switch(tpacpi_inputdev,
				    SW_RADIO, !!wlsw);
		input_sync(tpacpi_inputdev);

		mutex_unlock(&tpacpi_inputdev_send_mutex);
	}
}

static void tpacpi_input_send_tabletsw(void)
{
	int state;

	if (tp_features.hotkey_tablet &&
	    !hotkey_get_tablet_mode(&state)) {
		mutex_lock(&tpacpi_inputdev_send_mutex);

		input_report_switch(tpacpi_inputdev,
				    SW_TABLET_MODE, !!state);
		input_sync(tpacpi_inputdev);

		mutex_unlock(&tpacpi_inputdev_send_mutex);
	}
}

static void tpacpi_input_send_key(unsigned int scancode)
{
	unsigned int keycode;

	keycode = hotkey_keycode_map[scancode];

	if (keycode != KEY_RESERVED) {
		mutex_lock(&tpacpi_inputdev_send_mutex);

		input_report_key(tpacpi_inputdev, keycode, 1);
		if (keycode == KEY_UNKNOWN)
			input_event(tpacpi_inputdev, EV_MSC, MSC_SCAN,
				    scancode);
		input_sync(tpacpi_inputdev);

		input_report_key(tpacpi_inputdev, keycode, 0);
		if (keycode == KEY_UNKNOWN)
			input_event(tpacpi_inputdev, EV_MSC, MSC_SCAN,
				    scancode);
		input_sync(tpacpi_inputdev);

		mutex_unlock(&tpacpi_inputdev_send_mutex);
	}
}

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
static struct tp_acpi_drv_struct ibm_hotkey_acpidriver;

static void tpacpi_hotkey_send_key(unsigned int scancode)
{
	tpacpi_input_send_key(scancode);
	if (hotkey_report_mode < 2) {
		acpi_bus_generate_event(ibm_hotkey_acpidriver.device,
						0x80, 0x1001 + scancode);
	}
}

static void hotkey_read_nvram(struct tp_nvram_state *n, u32 m)
{
	u8 d;

	if (m & TP_NVRAM_HKEY_GROUP_HK2) {
		d = nvram_read_byte(TP_NVRAM_ADDR_HK2);
		n->thinkpad_toggle = !!(d & TP_NVRAM_MASK_HKT_THINKPAD);
		n->zoom_toggle = !!(d & TP_NVRAM_MASK_HKT_ZOOM);
		n->display_toggle = !!(d & TP_NVRAM_MASK_HKT_DISPLAY);
		n->hibernate_toggle = !!(d & TP_NVRAM_MASK_HKT_HIBERNATE);
	}
	if (m & TP_ACPI_HKEY_THNKLGHT_MASK) {
		d = nvram_read_byte(TP_NVRAM_ADDR_THINKLIGHT);
		n->thinklight_toggle = !!(d & TP_NVRAM_MASK_THINKLIGHT);
	}
	if (m & TP_ACPI_HKEY_DISPXPAND_MASK) {
		d = nvram_read_byte(TP_NVRAM_ADDR_VIDEO);
		n->displayexp_toggle =
				!!(d & TP_NVRAM_MASK_HKT_DISPEXPND);
	}
	if (m & TP_NVRAM_HKEY_GROUP_BRIGHTNESS) {
		d = nvram_read_byte(TP_NVRAM_ADDR_BRIGHTNESS);
		n->brightness_level = (d & TP_NVRAM_MASK_LEVEL_BRIGHTNESS)
				>> TP_NVRAM_POS_LEVEL_BRIGHTNESS;
		n->brightness_toggle =
				!!(d & TP_NVRAM_MASK_HKT_BRIGHTNESS);
	}
	if (m & TP_NVRAM_HKEY_GROUP_VOLUME) {
		d = nvram_read_byte(TP_NVRAM_ADDR_MIXER);
		n->volume_level = (d & TP_NVRAM_MASK_LEVEL_VOLUME)
				>> TP_NVRAM_POS_LEVEL_VOLUME;
		n->mute = !!(d & TP_NVRAM_MASK_MUTE);
		n->volume_toggle = !!(d & TP_NVRAM_MASK_HKT_VOLUME);
	}
}

#define TPACPI_COMPARE_KEY(__scancode, __member) \
	do { \
		if ((mask & (1 << __scancode)) && \
		    oldn->__member != newn->__member) \
		tpacpi_hotkey_send_key(__scancode); \
	} while (0)

#define TPACPI_MAY_SEND_KEY(__scancode) \
	do { if (mask & (1 << __scancode)) \
		tpacpi_hotkey_send_key(__scancode); } while (0)

static void hotkey_compare_and_issue_event(struct tp_nvram_state *oldn,
					   struct tp_nvram_state *newn,
					   u32 mask)
{
	TPACPI_COMPARE_KEY(TP_ACPI_HOTKEYSCAN_THINKPAD, thinkpad_toggle);
	TPACPI_COMPARE_KEY(TP_ACPI_HOTKEYSCAN_FNSPACE, zoom_toggle);
	TPACPI_COMPARE_KEY(TP_ACPI_HOTKEYSCAN_FNF7, display_toggle);
	TPACPI_COMPARE_KEY(TP_ACPI_HOTKEYSCAN_FNF12, hibernate_toggle);

	TPACPI_COMPARE_KEY(TP_ACPI_HOTKEYSCAN_FNPAGEUP, thinklight_toggle);

	TPACPI_COMPARE_KEY(TP_ACPI_HOTKEYSCAN_FNF8, displayexp_toggle);

	/* handle volume */
	if (oldn->volume_toggle != newn->volume_toggle) {
		if (oldn->mute != newn->mute) {
			TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_MUTE);
		}
		if (oldn->volume_level > newn->volume_level) {
			TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_VOLUMEDOWN);
		} else if (oldn->volume_level < newn->volume_level) {
			TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_VOLUMEUP);
		} else if (oldn->mute == newn->mute) {
			/* repeated key presses that didn't change state */
			if (newn->mute) {
				TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_MUTE);
			} else if (newn->volume_level != 0) {
				TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_VOLUMEUP);
			} else {
				TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_VOLUMEDOWN);
			}
		}
	}

	/* handle brightness */
	if (oldn->brightness_toggle != newn->brightness_toggle) {
		if (oldn->brightness_level < newn->brightness_level) {
			TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_FNHOME);
		} else if (oldn->brightness_level > newn->brightness_level) {
			TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_FNEND);
		} else {
			/* repeated key presses that didn't change state */
			if (newn->brightness_level != 0) {
				TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_FNHOME);
			} else {
				TPACPI_MAY_SEND_KEY(TP_ACPI_HOTKEYSCAN_FNEND);
			}
		}
	}
}

#undef TPACPI_COMPARE_KEY
#undef TPACPI_MAY_SEND_KEY

static int hotkey_kthread(void *data)
{
	struct tp_nvram_state s[2];
	u32 mask;
	unsigned int si, so;
	unsigned long t;
	unsigned int change_detector, must_reset;

	mutex_lock(&hotkey_thread_mutex);

	if (tpacpi_lifecycle == TPACPI_LIFE_EXITING)
		goto exit;

	current->flags &= ~PF_NOFREEZE;

	so = 0;
	si = 1;
	t = 0;

	/* Initial state for compares */
	mutex_lock(&hotkey_thread_data_mutex);
	change_detector = hotkey_config_change;
	mask = hotkey_source_mask & hotkey_mask;
	mutex_unlock(&hotkey_thread_data_mutex);
	hotkey_read_nvram(&s[so], mask);

	while (!kthread_should_stop() && hotkey_poll_freq) {
		if (t == 0)
			t = 1000/hotkey_poll_freq;
		t = msleep_interruptible(t);
		if (unlikely(kthread_should_stop()))
			break;
		must_reset = try_to_freeze();
		if (t > 0 && !must_reset)
			continue;

		mutex_lock(&hotkey_thread_data_mutex);
		if (must_reset || hotkey_config_change != change_detector) {
			/* forget old state on thaw or config change */
			si = so;
			t = 0;
			change_detector = hotkey_config_change;
		}
		mask = hotkey_source_mask & hotkey_mask;
		mutex_unlock(&hotkey_thread_data_mutex);

		if (likely(mask)) {
			hotkey_read_nvram(&s[si], mask);
			if (likely(si != so)) {
				hotkey_compare_and_issue_event(&s[so], &s[si],
								mask);
			}
		}

		so = si;
		si ^= 1;
	}

exit:
	mutex_unlock(&hotkey_thread_mutex);
	return 0;
}

static void hotkey_poll_stop_sync(void)
{
	if (tpacpi_hotkey_task) {
		if (frozen(tpacpi_hotkey_task) ||
		    freezing(tpacpi_hotkey_task))
			thaw_process(tpacpi_hotkey_task);

		kthread_stop(tpacpi_hotkey_task);
		tpacpi_hotkey_task = NULL;
		mutex_lock(&hotkey_thread_mutex);
		/* at this point, the thread did exit */
		mutex_unlock(&hotkey_thread_mutex);
	}
}

/* call with hotkey_mutex held */
static void hotkey_poll_setup(int may_warn)
{
	if ((hotkey_source_mask & hotkey_mask) != 0 &&
	    hotkey_poll_freq > 0 &&
	    (tpacpi_inputdev->users > 0 || hotkey_report_mode < 2)) {
		if (!tpacpi_hotkey_task) {
			tpacpi_hotkey_task = kthread_run(hotkey_kthread,
							 NULL,
							 TPACPI_FILE "d");
			if (IS_ERR(tpacpi_hotkey_task)) {
				tpacpi_hotkey_task = NULL;
				printk(TPACPI_ERR
				       "could not create kernel thread "
				       "for hotkey polling\n");
			}
		}
	} else {
		hotkey_poll_stop_sync();
		if (may_warn &&
		    hotkey_source_mask != 0 && hotkey_poll_freq == 0) {
			printk(TPACPI_NOTICE
				"hot keys 0x%08x require polling, "
				"which is currently disabled\n",
				hotkey_source_mask);
		}
	}
}

static void hotkey_poll_setup_safe(int may_warn)
{
	mutex_lock(&hotkey_mutex);
	hotkey_poll_setup(may_warn);
	mutex_unlock(&hotkey_mutex);
}

#else /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */

static void hotkey_poll_setup_safe(int __unused)
{
}

#endif /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */

static int hotkey_inputdev_open(struct input_dev *dev)
{
	switch (tpacpi_lifecycle) {
	case TPACPI_LIFE_INIT:
		/*
		 * hotkey_init will call hotkey_poll_setup_safe
		 * at the appropriate moment
		 */
		return 0;
	case TPACPI_LIFE_EXITING:
		return -EBUSY;
	case TPACPI_LIFE_RUNNING:
		hotkey_poll_setup_safe(0);
		return 0;
	}

	/* Should only happen if tpacpi_lifecycle is corrupt */
	BUG();
	return -EBUSY;
}

static void hotkey_inputdev_close(struct input_dev *dev)
{
	/* disable hotkey polling when possible */
	if (tpacpi_lifecycle == TPACPI_LIFE_RUNNING)
		hotkey_poll_setup_safe(0);
}

/* sysfs hotkey enable ------------------------------------------------- */
static ssize_t hotkey_enable_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int res, status;

	res = hotkey_status_get(&status);
	if (res)
		return res;

	return snprintf(buf, PAGE_SIZE, "%d\n", status);
}

static ssize_t hotkey_enable_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;
	int res;

	if (parse_strtoul(buf, 1, &t))
		return -EINVAL;

	res = hotkey_status_set(t);

	return (res) ? res : count;
}

static struct device_attribute dev_attr_hotkey_enable =
	__ATTR(hotkey_enable, S_IWUSR | S_IRUGO,
		hotkey_enable_show, hotkey_enable_store);

/* sysfs hotkey mask --------------------------------------------------- */
static ssize_t hotkey_mask_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int res;

	if (mutex_lock_interruptible(&hotkey_mutex))
		return -ERESTARTSYS;
	res = hotkey_mask_get();
	mutex_unlock(&hotkey_mutex);

	return (res)?
		res : snprintf(buf, PAGE_SIZE, "0x%08x\n", hotkey_mask);
}

static ssize_t hotkey_mask_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;
	int res;

	if (parse_strtoul(buf, 0xffffffffUL, &t))
		return -EINVAL;

	if (mutex_lock_interruptible(&hotkey_mutex))
		return -ERESTARTSYS;

	res = hotkey_mask_set(t);

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
	hotkey_poll_setup(1);
#endif

	mutex_unlock(&hotkey_mutex);

	return (res) ? res : count;
}

static struct device_attribute dev_attr_hotkey_mask =
	__ATTR(hotkey_mask, S_IWUSR | S_IRUGO,
		hotkey_mask_show, hotkey_mask_store);

/* sysfs hotkey bios_enabled ------------------------------------------- */
static ssize_t hotkey_bios_enabled_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", hotkey_orig_status);
}

static struct device_attribute dev_attr_hotkey_bios_enabled =
	__ATTR(hotkey_bios_enabled, S_IRUGO, hotkey_bios_enabled_show, NULL);

/* sysfs hotkey bios_mask ---------------------------------------------- */
static ssize_t hotkey_bios_mask_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%08x\n", hotkey_orig_mask);
}

static struct device_attribute dev_attr_hotkey_bios_mask =
	__ATTR(hotkey_bios_mask, S_IRUGO, hotkey_bios_mask_show, NULL);

/* sysfs hotkey all_mask ----------------------------------------------- */
static ssize_t hotkey_all_mask_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%08x\n",
				hotkey_all_mask | hotkey_source_mask);
}

static struct device_attribute dev_attr_hotkey_all_mask =
	__ATTR(hotkey_all_mask, S_IRUGO, hotkey_all_mask_show, NULL);

/* sysfs hotkey recommended_mask --------------------------------------- */
static ssize_t hotkey_recommended_mask_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%08x\n",
			(hotkey_all_mask | hotkey_source_mask)
			& ~hotkey_reserved_mask);
}

static struct device_attribute dev_attr_hotkey_recommended_mask =
	__ATTR(hotkey_recommended_mask, S_IRUGO,
		hotkey_recommended_mask_show, NULL);

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL

/* sysfs hotkey hotkey_source_mask ------------------------------------- */
static ssize_t hotkey_source_mask_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%08x\n", hotkey_source_mask);
}

static ssize_t hotkey_source_mask_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;

	if (parse_strtoul(buf, 0xffffffffUL, &t) ||
		((t & ~TPACPI_HKEY_NVRAM_KNOWN_MASK) != 0))
		return -EINVAL;

	if (mutex_lock_interruptible(&hotkey_mutex))
		return -ERESTARTSYS;

	HOTKEY_CONFIG_CRITICAL_START
	hotkey_source_mask = t;
	HOTKEY_CONFIG_CRITICAL_END

	hotkey_poll_setup(1);

	mutex_unlock(&hotkey_mutex);

	return count;
}

static struct device_attribute dev_attr_hotkey_source_mask =
	__ATTR(hotkey_source_mask, S_IWUSR | S_IRUGO,
		hotkey_source_mask_show, hotkey_source_mask_store);

/* sysfs hotkey hotkey_poll_freq --------------------------------------- */
static ssize_t hotkey_poll_freq_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", hotkey_poll_freq);
}

static ssize_t hotkey_poll_freq_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;

	if (parse_strtoul(buf, 25, &t))
		return -EINVAL;

	if (mutex_lock_interruptible(&hotkey_mutex))
		return -ERESTARTSYS;

	hotkey_poll_freq = t;

	hotkey_poll_setup(1);
	mutex_unlock(&hotkey_mutex);

	return count;
}

static struct device_attribute dev_attr_hotkey_poll_freq =
	__ATTR(hotkey_poll_freq, S_IWUSR | S_IRUGO,
		hotkey_poll_freq_show, hotkey_poll_freq_store);

#endif /* CONFIG_THINKPAD_ACPI_HOTKEY_POLL */

/* sysfs hotkey radio_sw (pollable) ------------------------------------ */
static ssize_t hotkey_radio_sw_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int res, s;
	res = hotkey_get_wlsw(&s);
	if (res < 0)
		return res;

	return snprintf(buf, PAGE_SIZE, "%d\n", !!s);
}

static struct device_attribute dev_attr_hotkey_radio_sw =
	__ATTR(hotkey_radio_sw, S_IRUGO, hotkey_radio_sw_show, NULL);

static void hotkey_radio_sw_notify_change(void)
{
	if (tp_features.hotkey_wlsw)
		sysfs_notify(&tpacpi_pdev->dev.kobj, NULL,
			     "hotkey_radio_sw");
}

/* sysfs hotkey tablet mode (pollable) --------------------------------- */
static ssize_t hotkey_tablet_mode_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int res, s;
	res = hotkey_get_tablet_mode(&s);
	if (res < 0)
		return res;

	return snprintf(buf, PAGE_SIZE, "%d\n", !!s);
}

static struct device_attribute dev_attr_hotkey_tablet_mode =
	__ATTR(hotkey_tablet_mode, S_IRUGO, hotkey_tablet_mode_show, NULL);

static void hotkey_tablet_mode_notify_change(void)
{
	if (tp_features.hotkey_tablet)
		sysfs_notify(&tpacpi_pdev->dev.kobj, NULL,
			     "hotkey_tablet_mode");
}

/* sysfs hotkey report_mode -------------------------------------------- */
static ssize_t hotkey_report_mode_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
		(hotkey_report_mode != 0) ? hotkey_report_mode : 1);
}

static struct device_attribute dev_attr_hotkey_report_mode =
	__ATTR(hotkey_report_mode, S_IRUGO, hotkey_report_mode_show, NULL);

/* sysfs wakeup reason (pollable) -------------------------------------- */
static ssize_t hotkey_wakeup_reason_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", hotkey_wakeup_reason);
}

static struct device_attribute dev_attr_hotkey_wakeup_reason =
	__ATTR(wakeup_reason, S_IRUGO, hotkey_wakeup_reason_show, NULL);

static void hotkey_wakeup_reason_notify_change(void)
{
	if (tp_features.hotkey_mask)
		sysfs_notify(&tpacpi_pdev->dev.kobj, NULL,
			     "wakeup_reason");
}

/* sysfs wakeup hotunplug_complete (pollable) -------------------------- */
static ssize_t hotkey_wakeup_hotunplug_complete_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", hotkey_autosleep_ack);
}

static struct device_attribute dev_attr_hotkey_wakeup_hotunplug_complete =
	__ATTR(wakeup_hotunplug_complete, S_IRUGO,
	       hotkey_wakeup_hotunplug_complete_show, NULL);

static void hotkey_wakeup_hotunplug_complete_notify_change(void)
{
	if (tp_features.hotkey_mask)
		sysfs_notify(&tpacpi_pdev->dev.kobj, NULL,
			     "wakeup_hotunplug_complete");
}

/* --------------------------------------------------------------------- */

static struct attribute *hotkey_attributes[] __initdata = {
	&dev_attr_hotkey_enable.attr,
	&dev_attr_hotkey_bios_enabled.attr,
	&dev_attr_hotkey_report_mode.attr,
#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
	&dev_attr_hotkey_mask.attr,
	&dev_attr_hotkey_all_mask.attr,
	&dev_attr_hotkey_recommended_mask.attr,
	&dev_attr_hotkey_source_mask.attr,
	&dev_attr_hotkey_poll_freq.attr,
#endif
};

static struct attribute *hotkey_mask_attributes[] __initdata = {
	&dev_attr_hotkey_bios_mask.attr,
#ifndef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
	&dev_attr_hotkey_mask.attr,
	&dev_attr_hotkey_all_mask.attr,
	&dev_attr_hotkey_recommended_mask.attr,
#endif
	&dev_attr_hotkey_wakeup_reason.attr,
	&dev_attr_hotkey_wakeup_hotunplug_complete.attr,
};

static int __init hotkey_init(struct ibm_init_struct *iibm)
{
	/* Requirements for changing the default keymaps:
	 *
	 * 1. Many of the keys are mapped to KEY_RESERVED for very
	 *    good reasons.  Do not change them unless you have deep
	 *    knowledge on the IBM and Lenovo ThinkPad firmware for
	 *    the various ThinkPad models.  The driver behaves
	 *    differently for KEY_RESERVED: such keys have their
	 *    hot key mask *unset* in mask_recommended, and also
	 *    in the initial hot key mask programmed into the
	 *    firmware at driver load time, which means the firm-
	 *    ware may react very differently if you change them to
	 *    something else;
	 *
	 * 2. You must be subscribed to the linux-thinkpad and
	 *    ibm-acpi-devel mailing lists, and you should read the
	 *    list archives since 2007 if you want to change the
	 *    keymaps.  This requirement exists so that you will
	 *    know the past history of problems with the thinkpad-
	 *    acpi driver keymaps, and also that you will be
	 *    listening to any bug reports;
	 *
	 * 3. Do not send thinkpad-acpi specific patches directly to
	 *    for merging, *ever*.  Send them to the linux-acpi
	 *    mailinglist for comments.  Merging is to be done only
	 *    through acpi-test and the ACPI maintainer.
	 *
	 * If the above is too much to ask, don't change the keymap.
	 * Ask the thinkpad-acpi maintainer to do it, instead.
	 */
	static u16 ibm_keycode_map[] __initdata = {
		/* Scan Codes 0x00 to 0x0B: ACPI HKEY FN+F1..F12 */
		KEY_FN_F1,	KEY_FN_F2,	KEY_COFFEE,	KEY_SLEEP,
		KEY_WLAN,	KEY_FN_F6, KEY_SWITCHVIDEOMODE, KEY_FN_F8,
		KEY_FN_F9,	KEY_FN_F10,	KEY_FN_F11,	KEY_SUSPEND,

		/* Scan codes 0x0C to 0x1F: Other ACPI HKEY hot keys */
		KEY_UNKNOWN,	/* 0x0C: FN+BACKSPACE */
		KEY_UNKNOWN,	/* 0x0D: FN+INSERT */
		KEY_UNKNOWN,	/* 0x0E: FN+DELETE */

		/* brightness: firmware always reacts to them, unless
		 * X.org did some tricks in the radeon BIOS scratch
		 * registers of *some* models */
		KEY_RESERVED,	/* 0x0F: FN+HOME (brightness up) */
		KEY_RESERVED,	/* 0x10: FN+END (brightness down) */

		/* Thinklight: firmware always react to it */
		KEY_RESERVED,	/* 0x11: FN+PGUP (thinklight toggle) */

		KEY_UNKNOWN,	/* 0x12: FN+PGDOWN */
		KEY_ZOOM,	/* 0x13: FN+SPACE (zoom) */

		/* Volume: firmware always react to it and reprograms
		 * the built-in *extra* mixer.  Never map it to control
		 * another mixer by default. */
		KEY_RESERVED,	/* 0x14: VOLUME UP */
		KEY_RESERVED,	/* 0x15: VOLUME DOWN */
		KEY_RESERVED,	/* 0x16: MUTE */

		KEY_VENDOR,	/* 0x17: Thinkpad/AccessIBM/Lenovo */

		/* (assignments unknown, please report if found) */
		KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN,
		KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN,
	};
	static u16 lenovo_keycode_map[] __initdata = {
		/* Scan Codes 0x00 to 0x0B: ACPI HKEY FN+F1..F12 */
		KEY_FN_F1,	KEY_COFFEE,	KEY_BATTERY,	KEY_SLEEP,
		KEY_WLAN,	KEY_FN_F6, KEY_SWITCHVIDEOMODE, KEY_FN_F8,
		KEY_FN_F9,	KEY_FN_F10,	KEY_FN_F11,	KEY_SUSPEND,

		/* Scan codes 0x0C to 0x1F: Other ACPI HKEY hot keys */
		KEY_UNKNOWN,	/* 0x0C: FN+BACKSPACE */
		KEY_UNKNOWN,	/* 0x0D: FN+INSERT */
		KEY_UNKNOWN,	/* 0x0E: FN+DELETE */

		KEY_RESERVED,	/* 0x0F: FN+HOME (brightness up) */
		KEY_RESERVED,	/* 0x10: FN+END (brightness down) */

		KEY_RESERVED,	/* 0x11: FN+PGUP (thinklight toggle) */

		KEY_UNKNOWN,	/* 0x12: FN+PGDOWN */
		KEY_ZOOM,	/* 0x13: FN+SPACE (zoom) */

		/* Volume: z60/z61, T60 (BIOS version?): firmware always
		 * react to it and reprograms the built-in *extra* mixer.
		 * Never map it to control another mixer by default.
		 *
		 * T60?, T61, R60?, R61: firmware and EC tries to send
		 * these over the regular keyboard, so these are no-ops,
		 * but there are still weird bugs re. MUTE, so do not
		 * change unless you get test reports from all Lenovo
		 * models.  May cause the BIOS to interfere with the
		 * HDA mixer.
		 */
		KEY_RESERVED,	/* 0x14: VOLUME UP */
		KEY_RESERVED,	/* 0x15: VOLUME DOWN */
		KEY_RESERVED,	/* 0x16: MUTE */

		KEY_VENDOR,	/* 0x17: Thinkpad/AccessIBM/Lenovo */

		/* (assignments unknown, please report if found) */
		KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN,
		KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN, KEY_UNKNOWN,
	};

#define TPACPI_HOTKEY_MAP_LEN		ARRAY_SIZE(ibm_keycode_map)
#define TPACPI_HOTKEY_MAP_SIZE		sizeof(ibm_keycode_map)
#define TPACPI_HOTKEY_MAP_TYPESIZE	sizeof(ibm_keycode_map[0])

	int res, i;
	int status;
	int hkeyv;

	vdbg_printk(TPACPI_DBG_INIT, "initializing hotkey subdriver\n");

	BUG_ON(!tpacpi_inputdev);
	BUG_ON(tpacpi_inputdev->open != NULL ||
	       tpacpi_inputdev->close != NULL);

	TPACPI_ACPIHANDLE_INIT(hkey);
	mutex_init(&hotkey_mutex);

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
	mutex_init(&hotkey_thread_mutex);
	mutex_init(&hotkey_thread_data_mutex);
#endif

	/* hotkey not supported on 570 */
	tp_features.hotkey = hkey_handle != NULL;

	vdbg_printk(TPACPI_DBG_INIT, "hotkeys are %s\n",
		str_supported(tp_features.hotkey));

	if (tp_features.hotkey) {
		hotkey_dev_attributes = create_attr_set(13, NULL);
		if (!hotkey_dev_attributes)
			return -ENOMEM;
		res = add_many_to_attr_set(hotkey_dev_attributes,
				hotkey_attributes,
				ARRAY_SIZE(hotkey_attributes));
		if (res)
			return res;

		/* mask not supported on 570, 600e/x, 770e, 770x, A21e, A2xm/p,
		   A30, R30, R31, T20-22, X20-21, X22-24.  Detected by checking
		   for HKEY interface version 0x100 */
		if (acpi_evalf(hkey_handle, &hkeyv, "MHKV", "qd")) {
			if ((hkeyv >> 8) != 1) {
				printk(TPACPI_ERR "unknown version of the "
				       "HKEY interface: 0x%x\n", hkeyv);
				printk(TPACPI_ERR "please report this to %s\n",
				       TPACPI_MAIL);
			} else {
				/*
				 * MHKV 0x100 in A31, R40, R40e,
				 * T4x, X31, and later
				 */
				tp_features.hotkey_mask = 1;
			}
		}

		vdbg_printk(TPACPI_DBG_INIT, "hotkey masks are %s\n",
			str_supported(tp_features.hotkey_mask));

		if (tp_features.hotkey_mask) {
			if (!acpi_evalf(hkey_handle, &hotkey_all_mask,
					"MHKA", "qd")) {
				printk(TPACPI_ERR
				       "missing MHKA handler, "
				       "please report this to %s\n",
				       TPACPI_MAIL);
				/* FN+F12, FN+F4, FN+F3 */
				hotkey_all_mask = 0x080cU;
			}
		}

		/* hotkey_source_mask *must* be zero for
		 * the first hotkey_mask_get */
		res = hotkey_status_get(&hotkey_orig_status);
		if (!res && tp_features.hotkey_mask) {
			res = hotkey_mask_get();
			hotkey_orig_mask = hotkey_mask;
			if (!res) {
				res = add_many_to_attr_set(
					hotkey_dev_attributes,
					hotkey_mask_attributes,
					ARRAY_SIZE(hotkey_mask_attributes));
			}
		}

#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
		if (tp_features.hotkey_mask) {
			hotkey_source_mask = TPACPI_HKEY_NVRAM_GOOD_MASK
						& ~hotkey_all_mask;
		} else {
			hotkey_source_mask = TPACPI_HKEY_NVRAM_GOOD_MASK;
		}

		vdbg_printk(TPACPI_DBG_INIT,
			    "hotkey source mask 0x%08x, polling freq %d\n",
			    hotkey_source_mask, hotkey_poll_freq);
#endif

		/* Not all thinkpads have a hardware radio switch */
		if (!res && acpi_evalf(hkey_handle, &status, "WLSW", "qd")) {
			tp_features.hotkey_wlsw = 1;
			printk(TPACPI_INFO
				"radio switch found; radios are %s\n",
				enabled(status, 0));
			res = add_to_attr_set(hotkey_dev_attributes,
					&dev_attr_hotkey_radio_sw.attr);
		}

		/* For X41t, X60t, X61t Tablets... */
		if (!res && acpi_evalf(hkey_handle, &status, "MHKG", "qd")) {
			tp_features.hotkey_tablet = 1;
			printk(TPACPI_INFO
				"possible tablet mode switch found; "
				"ThinkPad in %s mode\n",
				(status & TP_HOTKEY_TABLET_MASK)?
					"tablet" : "laptop");
			res = add_to_attr_set(hotkey_dev_attributes,
					&dev_attr_hotkey_tablet_mode.attr);
		}

		if (!res)
			res = register_attr_set_with_sysfs(
					hotkey_dev_attributes,
					&tpacpi_pdev->dev.kobj);
		if (res)
			return res;

		/* Set up key map */

		hotkey_keycode_map = kmalloc(TPACPI_HOTKEY_MAP_SIZE,
						GFP_KERNEL);
		if (!hotkey_keycode_map) {
			printk(TPACPI_ERR
				"failed to allocate memory for key map\n");
			return -ENOMEM;
		}

		if (thinkpad_id.vendor == PCI_VENDOR_ID_LENOVO) {
			dbg_printk(TPACPI_DBG_INIT,
				   "using Lenovo default hot key map\n");
			memcpy(hotkey_keycode_map, &lenovo_keycode_map,
				TPACPI_HOTKEY_MAP_SIZE);
		} else {
			dbg_printk(TPACPI_DBG_INIT,
				   "using IBM default hot key map\n");
			memcpy(hotkey_keycode_map, &ibm_keycode_map,
				TPACPI_HOTKEY_MAP_SIZE);
		}

		set_bit(EV_KEY, tpacpi_inputdev->evbit);
		set_bit(EV_MSC, tpacpi_inputdev->evbit);
		set_bit(MSC_SCAN, tpacpi_inputdev->mscbit);
		tpacpi_inputdev->keycodesize = TPACPI_HOTKEY_MAP_TYPESIZE;
		tpacpi_inputdev->keycodemax = TPACPI_HOTKEY_MAP_LEN;
		tpacpi_inputdev->keycode = hotkey_keycode_map;
		for (i = 0; i < TPACPI_HOTKEY_MAP_LEN; i++) {
			if (hotkey_keycode_map[i] != KEY_RESERVED) {
				set_bit(hotkey_keycode_map[i],
					tpacpi_inputdev->keybit);
			} else {
				if (i < sizeof(hotkey_reserved_mask)*8)
					hotkey_reserved_mask |= 1 << i;
			}
		}

		if (tp_features.hotkey_wlsw) {
			set_bit(EV_SW, tpacpi_inputdev->evbit);
			set_bit(SW_RADIO, tpacpi_inputdev->swbit);
		}
		if (tp_features.hotkey_tablet) {
			set_bit(EV_SW, tpacpi_inputdev->evbit);
			set_bit(SW_TABLET_MODE, tpacpi_inputdev->swbit);
		}

		dbg_printk(TPACPI_DBG_INIT,
				"enabling hot key handling\n");
		res = hotkey_status_set(1);
		if (res)
			return res;
		res = hotkey_mask_set(((hotkey_all_mask | hotkey_source_mask)
					& ~hotkey_reserved_mask)
					| hotkey_orig_mask);
		if (res < 0 && res != -ENXIO)
			return res;

		dbg_printk(TPACPI_DBG_INIT,
				"legacy hot key reporting over procfs %s\n",
				(hotkey_report_mode < 2) ?
					"enabled" : "disabled");

		tpacpi_inputdev->open = &hotkey_inputdev_open;
		tpacpi_inputdev->close = &hotkey_inputdev_close;

		hotkey_poll_setup_safe(1);
		tpacpi_input_send_radiosw();
		tpacpi_input_send_tabletsw();
	}

	return (tp_features.hotkey)? 0 : 1;
}

static void hotkey_exit(void)
{
#ifdef CONFIG_THINKPAD_ACPI_HOTKEY_POLL
	hotkey_poll_stop_sync();
#endif

	if (tp_features.hotkey) {
		dbg_printk(TPACPI_DBG_EXIT,
			   "restoring original hot key mask\n");
		/* no short-circuit boolean operator below! */
		if ((hotkey_mask_set(hotkey_orig_mask) |
		     hotkey_status_set(hotkey_orig_status)) != 0)
			printk(TPACPI_ERR
			       "failed to restore hot key mask "
			       "to BIOS defaults\n");
	}

	if (hotkey_dev_attributes) {
		delete_attr_set(hotkey_dev_attributes, &tpacpi_pdev->dev.kobj);
		hotkey_dev_attributes = NULL;
	}
}

static void hotkey_notify(struct ibm_struct *ibm, u32 event)
{
	u32 hkey;
	unsigned int scancode;
	int send_acpi_ev;
	int ignore_acpi_ev;
	int unk_ev;

	if (event != 0x80) {
		printk(TPACPI_ERR
		       "unknown HKEY notification event %d\n", event);
		return;
	}

	while (1) {
		if (!acpi_evalf(hkey_handle, &hkey, "MHKP", "d")) {
			printk(TPACPI_ERR "failed to retrieve HKEY event\n");
			return;
		}

		if (hkey == 0) {
			/* queue empty */
			return;
		}

		send_acpi_ev = 1;
		ignore_acpi_ev = 0;
		unk_ev = 0;

		switch (hkey >> 12) {
		case 1:
			/* 0x1000-0x1FFF: key presses */
			scancode = hkey & 0xfff;
			if (scancode > 0 && scancode < 0x21) {
				scancode--;
				if (!(hotkey_source_mask & (1 << scancode))) {
					tpacpi_input_send_key(scancode);
					send_acpi_ev = 0;
				} else {
					ignore_acpi_ev = 1;
				}
			} else {
				unk_ev = 1;
			}
			break;
		case 2:
			/* Wakeup reason */
			switch (hkey) {
			case 0x2304: /* suspend, undock */
			case 0x2404: /* hibernation, undock */
				hotkey_wakeup_reason = TP_ACPI_WAKEUP_UNDOCK;
				ignore_acpi_ev = 1;
				break;
			case 0x2305: /* suspend, bay eject */
			case 0x2405: /* hibernation, bay eject */
				hotkey_wakeup_reason = TP_ACPI_WAKEUP_BAYEJ;
				ignore_acpi_ev = 1;
				break;
			default:
				unk_ev = 1;
			}
			if (hotkey_wakeup_reason != TP_ACPI_WAKEUP_NONE) {
				printk(TPACPI_INFO
				       "woke up due to a hot-unplug "
				       "request...\n");
				hotkey_wakeup_reason_notify_change();
			}
			break;
		case 3:
			/* bay-related wakeups */
			if (hkey == 0x3003) {
				hotkey_autosleep_ack = 1;
				printk(TPACPI_INFO
				       "bay ejected\n");
				hotkey_wakeup_hotunplug_complete_notify_change();
			} else {
				unk_ev = 1;
			}
			break;
		case 4:
			/* dock-related wakeups */
			if (hkey == 0x4003) {
				hotkey_autosleep_ack = 1;
				printk(TPACPI_INFO
				       "undocked\n");
				hotkey_wakeup_hotunplug_complete_notify_change();
			} else {
				unk_ev = 1;
			}
			break;
		case 5:
			/* 0x5000-0x5FFF: human interface helpers */
			switch (hkey) {
			case 0x5010: /* Lenovo new BIOS: brightness changed */
			case 0x500b: /* X61t: tablet pen inserted into bay */
			case 0x500c: /* X61t: tablet pen removed from bay */
				break;
			case 0x5009: /* X41t-X61t: swivel up (tablet mode) */
			case 0x500a: /* X41t-X61t: swivel down (normal mode) */
				tpacpi_input_send_tabletsw();
				hotkey_tablet_mode_notify_change();
				send_acpi_ev = 0;
				break;
			case 0x5001:
			case 0x5002:
				/* LID switch events.  Do not propagate */
				ignore_acpi_ev = 1;
				break;
			default:
				unk_ev = 1;
			}
			break;
		case 7:
			/* 0x7000-0x7FFF: misc */
			if (tp_features.hotkey_wlsw && hkey == 0x7000) {
				tpacpi_input_send_radiosw();
				hotkey_radio_sw_notify_change();
				send_acpi_ev = 0;
				break;
			}
			/* fallthrough to default */
		default:
			unk_ev = 1;
		}
		if (unk_ev) {
			printk(TPACPI_NOTICE
			       "unhandled HKEY event 0x%04x\n", hkey);
		}

		/* Legacy events */
		if (!ignore_acpi_ev &&
		    (send_acpi_ev || hotkey_report_mode < 2)) {
			acpi_bus_generate_event(ibm->acpi->device,
						     event, hkey);
		}
	}
}

static void hotkey_suspend(pm_message_t state)
{
	/* Do these on suspend, we get the events on early resume! */
	hotkey_wakeup_reason = TP_ACPI_WAKEUP_NONE;
	hotkey_autosleep_ack = 0;
}

static void hotkey_resume(void)
{
	if (hotkey_mask_get())
		printk(TPACPI_ERR
		       "error while trying to read hot key mask "
		       "from firmware\n");
	tpacpi_input_send_radiosw();
	hotkey_radio_sw_notify_change();
	hotkey_tablet_mode_notify_change();
	hotkey_wakeup_reason_notify_change();
	hotkey_wakeup_hotunplug_complete_notify_change();
	hotkey_poll_setup_safe(0);
}

/* procfs -------------------------------------------------------------- */
static int hotkey_read(char *p)
{
	int res, status;
	int len = 0;

	if (!tp_features.hotkey) {
		len += sprintf(p + len, "status:\t\tnot supported\n");
		return len;
	}

	if (mutex_lock_interruptible(&hotkey_mutex))
		return -ERESTARTSYS;
	res = hotkey_status_get(&status);
	if (!res)
		res = hotkey_mask_get();
	mutex_unlock(&hotkey_mutex);
	if (res)
		return res;

	len += sprintf(p + len, "status:\t\t%s\n", enabled(status, 0));
	if (tp_features.hotkey_mask) {
		len += sprintf(p + len, "mask:\t\t0x%08x\n", hotkey_mask);
		len += sprintf(p + len,
			       "commands:\tenable, disable, reset, <mask>\n");
	} else {
		len += sprintf(p + len, "mask:\t\tnot supported\n");
		len += sprintf(p + len, "commands:\tenable, disable, reset\n");
	}

	return len;
}

static int hotkey_write(char *buf)
{
	int res, status;
	u32 mask;
	char *cmd;

	if (!tp_features.hotkey)
		return -ENODEV;

	if (mutex_lock_interruptible(&hotkey_mutex))
		return -ERESTARTSYS;

	status = -1;
	mask = hotkey_mask;

	res = 0;
	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "enable") == 0) {
			status = 1;
		} else if (strlencmp(cmd, "disable") == 0) {
			status = 0;
		} else if (strlencmp(cmd, "reset") == 0) {
			status = hotkey_orig_status;
			mask = hotkey_orig_mask;
		} else if (sscanf(cmd, "0x%x", &mask) == 1) {
			/* mask set */
		} else if (sscanf(cmd, "%x", &mask) == 1) {
			/* mask set */
		} else {
			res = -EINVAL;
			goto errexit;
		}
	}
	if (status != -1)
		res = hotkey_status_set(status);

	if (!res && mask != hotkey_mask)
		res = hotkey_mask_set(mask);

errexit:
	mutex_unlock(&hotkey_mutex);
	return res;
}

static struct tp_acpi_drv_struct ibm_hotkey_acpidriver = {
	.hid = TPACPI_ACPI_HKEY_HID,
	.notify = hotkey_notify,
	.handle = &hkey_handle,
	.type = ACPI_DEVICE_NOTIFY,
};

static struct ibm_struct hotkey_driver_data = {
	.name = "hotkey",
	.read = hotkey_read,
	.write = hotkey_write,
	.exit = hotkey_exit,
	.resume = hotkey_resume,
	.suspend = hotkey_suspend,
	.acpi = &ibm_hotkey_acpidriver,
};

/*************************************************************************
 * Bluetooth subdriver
 */

enum {
	/* ACPI GBDC/SBDC bits */
	TP_ACPI_BLUETOOTH_HWPRESENT	= 0x01,	/* Bluetooth hw available */
	TP_ACPI_BLUETOOTH_RADIOSSW	= 0x02,	/* Bluetooth radio enabled */
	TP_ACPI_BLUETOOTH_UNK		= 0x04,	/* unknown function */
};

static int bluetooth_get_radiosw(void);
static int bluetooth_set_radiosw(int radio_on);

/* sysfs bluetooth enable ---------------------------------------------- */
static ssize_t bluetooth_enable_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int status;

	status = bluetooth_get_radiosw();
	if (status < 0)
		return status;

	return snprintf(buf, PAGE_SIZE, "%d\n", status ? 1 : 0);
}

static ssize_t bluetooth_enable_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;
	int res;

	if (parse_strtoul(buf, 1, &t))
		return -EINVAL;

	res = bluetooth_set_radiosw(t);

	return (res) ? res : count;
}

static struct device_attribute dev_attr_bluetooth_enable =
	__ATTR(bluetooth_enable, S_IWUSR | S_IRUGO,
		bluetooth_enable_show, bluetooth_enable_store);

/* --------------------------------------------------------------------- */

static struct attribute *bluetooth_attributes[] = {
	&dev_attr_bluetooth_enable.attr,
	NULL
};

static const struct attribute_group bluetooth_attr_group = {
	.attrs = bluetooth_attributes,
};

static int __init bluetooth_init(struct ibm_init_struct *iibm)
{
	int res;
	int status = 0;

	vdbg_printk(TPACPI_DBG_INIT, "initializing bluetooth subdriver\n");

	TPACPI_ACPIHANDLE_INIT(hkey);

	/* bluetooth not supported on 570, 600e/x, 770e, 770x, A21e, A2xm/p,
	   G4x, R30, R31, R40e, R50e, T20-22, X20-21 */
	tp_features.bluetooth = hkey_handle &&
	    acpi_evalf(hkey_handle, &status, "GBDC", "qd");

	vdbg_printk(TPACPI_DBG_INIT, "bluetooth is %s, status 0x%02x\n",
		str_supported(tp_features.bluetooth),
		status);

	if (tp_features.bluetooth) {
		if (!(status & TP_ACPI_BLUETOOTH_HWPRESENT)) {
			/* no bluetooth hardware present in system */
			tp_features.bluetooth = 0;
			dbg_printk(TPACPI_DBG_INIT,
				   "bluetooth hardware not installed\n");
		} else {
			res = sysfs_create_group(&tpacpi_pdev->dev.kobj,
					&bluetooth_attr_group);
			if (res)
				return res;
		}
	}

	return (tp_features.bluetooth)? 0 : 1;
}

static void bluetooth_exit(void)
{
	sysfs_remove_group(&tpacpi_pdev->dev.kobj,
			&bluetooth_attr_group);
}

static int bluetooth_get_radiosw(void)
{
	int status;

	if (!tp_features.bluetooth)
		return -ENODEV;

	if (!acpi_evalf(hkey_handle, &status, "GBDC", "d"))
		return -EIO;

	return ((status & TP_ACPI_BLUETOOTH_RADIOSSW) != 0);
}

static int bluetooth_set_radiosw(int radio_on)
{
	int status;

	if (!tp_features.bluetooth)
		return -ENODEV;

	if (!acpi_evalf(hkey_handle, &status, "GBDC", "d"))
		return -EIO;
	if (radio_on)
		status |= TP_ACPI_BLUETOOTH_RADIOSSW;
	else
		status &= ~TP_ACPI_BLUETOOTH_RADIOSSW;
	if (!acpi_evalf(hkey_handle, NULL, "SBDC", "vd", status))
		return -EIO;

	return 0;
}

/* procfs -------------------------------------------------------------- */
static int bluetooth_read(char *p)
{
	int len = 0;
	int status = bluetooth_get_radiosw();

	if (!tp_features.bluetooth)
		len += sprintf(p + len, "status:\t\tnot supported\n");
	else {
		len += sprintf(p + len, "status:\t\t%s\n",
				(status)? "enabled" : "disabled");
		len += sprintf(p + len, "commands:\tenable, disable\n");
	}

	return len;
}

static int bluetooth_write(char *buf)
{
	char *cmd;

	if (!tp_features.bluetooth)
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "enable") == 0) {
			bluetooth_set_radiosw(1);
		} else if (strlencmp(cmd, "disable") == 0) {
			bluetooth_set_radiosw(0);
		} else
			return -EINVAL;
	}

	return 0;
}

static struct ibm_struct bluetooth_driver_data = {
	.name = "bluetooth",
	.read = bluetooth_read,
	.write = bluetooth_write,
	.exit = bluetooth_exit,
};

/*************************************************************************
 * Wan subdriver
 */

enum {
	/* ACPI GWAN/SWAN bits */
	TP_ACPI_WANCARD_HWPRESENT	= 0x01,	/* Wan hw available */
	TP_ACPI_WANCARD_RADIOSSW	= 0x02,	/* Wan radio enabled */
	TP_ACPI_WANCARD_UNK		= 0x04,	/* unknown function */
};

static int wan_get_radiosw(void);
static int wan_set_radiosw(int radio_on);

/* sysfs wan enable ---------------------------------------------------- */
static ssize_t wan_enable_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int status;

	status = wan_get_radiosw();
	if (status < 0)
		return status;

	return snprintf(buf, PAGE_SIZE, "%d\n", status ? 1 : 0);
}

static ssize_t wan_enable_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;
	int res;

	if (parse_strtoul(buf, 1, &t))
		return -EINVAL;

	res = wan_set_radiosw(t);

	return (res) ? res : count;
}

static struct device_attribute dev_attr_wan_enable =
	__ATTR(wwan_enable, S_IWUSR | S_IRUGO,
		wan_enable_show, wan_enable_store);

/* --------------------------------------------------------------------- */

static struct attribute *wan_attributes[] = {
	&dev_attr_wan_enable.attr,
	NULL
};

static const struct attribute_group wan_attr_group = {
	.attrs = wan_attributes,
};

static int __init wan_init(struct ibm_init_struct *iibm)
{
	int res;
	int status = 0;

	vdbg_printk(TPACPI_DBG_INIT, "initializing wan subdriver\n");

	TPACPI_ACPIHANDLE_INIT(hkey);

	tp_features.wan = hkey_handle &&
	    acpi_evalf(hkey_handle, &status, "GWAN", "qd");

	vdbg_printk(TPACPI_DBG_INIT, "wan is %s, status 0x%02x\n",
		str_supported(tp_features.wan),
		status);

	if (tp_features.wan) {
		if (!(status & TP_ACPI_WANCARD_HWPRESENT)) {
			/* no wan hardware present in system */
			tp_features.wan = 0;
			dbg_printk(TPACPI_DBG_INIT,
				   "wan hardware not installed\n");
		} else {
			res = sysfs_create_group(&tpacpi_pdev->dev.kobj,
					&wan_attr_group);
			if (res)
				return res;
		}
	}

	return (tp_features.wan)? 0 : 1;
}

static void wan_exit(void)
{
	sysfs_remove_group(&tpacpi_pdev->dev.kobj,
		&wan_attr_group);
}

static int wan_get_radiosw(void)
{
	int status;

	if (!tp_features.wan)
		return -ENODEV;

	if (!acpi_evalf(hkey_handle, &status, "GWAN", "d"))
		return -EIO;

	return ((status & TP_ACPI_WANCARD_RADIOSSW) != 0);
}

static int wan_set_radiosw(int radio_on)
{
	int status;

	if (!tp_features.wan)
		return -ENODEV;

	if (!acpi_evalf(hkey_handle, &status, "GWAN", "d"))
		return -EIO;
	if (radio_on)
		status |= TP_ACPI_WANCARD_RADIOSSW;
	else
		status &= ~TP_ACPI_WANCARD_RADIOSSW;
	if (!acpi_evalf(hkey_handle, NULL, "SWAN", "vd", status))
		return -EIO;

	return 0;
}

/* procfs -------------------------------------------------------------- */
static int wan_read(char *p)
{
	int len = 0;
	int status = wan_get_radiosw();

	if (!tp_features.wan)
		len += sprintf(p + len, "status:\t\tnot supported\n");
	else {
		len += sprintf(p + len, "status:\t\t%s\n",
				(status)? "enabled" : "disabled");
		len += sprintf(p + len, "commands:\tenable, disable\n");
	}

	return len;
}

static int wan_write(char *buf)
{
	char *cmd;

	if (!tp_features.wan)
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "enable") == 0) {
			wan_set_radiosw(1);
		} else if (strlencmp(cmd, "disable") == 0) {
			wan_set_radiosw(0);
		} else
			return -EINVAL;
	}

	return 0;
}

static struct ibm_struct wan_driver_data = {
	.name = "wan",
	.read = wan_read,
	.write = wan_write,
	.exit = wan_exit,
	.flags.experimental = 1,
};

/*************************************************************************
 * Video subdriver
 */

#ifdef CONFIG_THINKPAD_ACPI_VIDEO

enum video_access_mode {
	TPACPI_VIDEO_NONE = 0,
	TPACPI_VIDEO_570,	/* 570 */
	TPACPI_VIDEO_770,	/* 600e/x, 770e, 770x */
	TPACPI_VIDEO_NEW,	/* all others */
};

enum {	/* video status flags, based on VIDEO_570 */
	TP_ACPI_VIDEO_S_LCD = 0x01,	/* LCD output enabled */
	TP_ACPI_VIDEO_S_CRT = 0x02,	/* CRT output enabled */
	TP_ACPI_VIDEO_S_DVI = 0x08,	/* DVI output enabled */
};

enum {  /* TPACPI_VIDEO_570 constants */
	TP_ACPI_VIDEO_570_PHSCMD = 0x87,	/* unknown magic constant :( */
	TP_ACPI_VIDEO_570_PHSMASK = 0x03,	/* PHS bits that map to
						 * video_status_flags */
	TP_ACPI_VIDEO_570_PHS2CMD = 0x8b,	/* unknown magic constant :( */
	TP_ACPI_VIDEO_570_PHS2SET = 0x80,	/* unknown magic constant :( */
};

static enum video_access_mode video_supported;
static int video_orig_autosw;

static int video_autosw_get(void);
static int video_autosw_set(int enable);

TPACPI_HANDLE(vid2, root, "\\_SB.PCI0.AGPB.VID");	/* G41 */

static int __init video_init(struct ibm_init_struct *iibm)
{
	int ivga;

	vdbg_printk(TPACPI_DBG_INIT, "initializing video subdriver\n");

	TPACPI_ACPIHANDLE_INIT(vid);
	TPACPI_ACPIHANDLE_INIT(vid2);

	if (vid2_handle && acpi_evalf(NULL, &ivga, "\\IVGA", "d") && ivga)
		/* G41, assume IVGA doesn't change */
		vid_handle = vid2_handle;

	if (!vid_handle)
		/* video switching not supported on R30, R31 */
		video_supported = TPACPI_VIDEO_NONE;
	else if (acpi_evalf(vid_handle, &video_orig_autosw, "SWIT", "qd"))
		/* 570 */
		video_supported = TPACPI_VIDEO_570;
	else if (acpi_evalf(vid_handle, &video_orig_autosw, "^VADL", "qd"))
		/* 600e/x, 770e, 770x */
		video_supported = TPACPI_VIDEO_770;
	else
		/* all others */
		video_supported = TPACPI_VIDEO_NEW;

	vdbg_printk(TPACPI_DBG_INIT, "video is %s, mode %d\n",
		str_supported(video_supported != TPACPI_VIDEO_NONE),
		video_supported);

	return (video_supported != TPACPI_VIDEO_NONE)? 0 : 1;
}

static void video_exit(void)
{
	dbg_printk(TPACPI_DBG_EXIT,
		   "restoring original video autoswitch mode\n");
	if (video_autosw_set(video_orig_autosw))
		printk(TPACPI_ERR "error while trying to restore original "
			"video autoswitch mode\n");
}

static int video_outputsw_get(void)
{
	int status = 0;
	int i;

	switch (video_supported) {
	case TPACPI_VIDEO_570:
		if (!acpi_evalf(NULL, &i, "\\_SB.PHS", "dd",
				 TP_ACPI_VIDEO_570_PHSCMD))
			return -EIO;
		status = i & TP_ACPI_VIDEO_570_PHSMASK;
		break;
	case TPACPI_VIDEO_770:
		if (!acpi_evalf(NULL, &i, "\\VCDL", "d"))
			return -EIO;
		if (i)
			status |= TP_ACPI_VIDEO_S_LCD;
		if (!acpi_evalf(NULL, &i, "\\VCDC", "d"))
			return -EIO;
		if (i)
			status |= TP_ACPI_VIDEO_S_CRT;
		break;
	case TPACPI_VIDEO_NEW:
		if (!acpi_evalf(NULL, NULL, "\\VUPS", "vd", 1) ||
		    !acpi_evalf(NULL, &i, "\\VCDC", "d"))
			return -EIO;
		if (i)
			status |= TP_ACPI_VIDEO_S_CRT;

		if (!acpi_evalf(NULL, NULL, "\\VUPS", "vd", 0) ||
		    !acpi_evalf(NULL, &i, "\\VCDL", "d"))
			return -EIO;
		if (i)
			status |= TP_ACPI_VIDEO_S_LCD;
		if (!acpi_evalf(NULL, &i, "\\VCDD", "d"))
			return -EIO;
		if (i)
			status |= TP_ACPI_VIDEO_S_DVI;
		break;
	default:
		return -ENOSYS;
	}

	return status;
}

static int video_outputsw_set(int status)
{
	int autosw;
	int res = 0;

	switch (video_supported) {
	case TPACPI_VIDEO_570:
		res = acpi_evalf(NULL, NULL,
				 "\\_SB.PHS2", "vdd",
				 TP_ACPI_VIDEO_570_PHS2CMD,
				 status | TP_ACPI_VIDEO_570_PHS2SET);
		break;
	case TPACPI_VIDEO_770:
		autosw = video_autosw_get();
		if (autosw < 0)
			return autosw;

		res = video_autosw_set(1);
		if (res)
			return res;
		res = acpi_evalf(vid_handle, NULL,
				 "ASWT", "vdd", status * 0x100, 0);
		if (!autosw && video_autosw_set(autosw)) {
			printk(TPACPI_ERR
			       "video auto-switch left enabled due to error\n");
			return -EIO;
		}
		break;
	case TPACPI_VIDEO_NEW:
		res = acpi_evalf(NULL, NULL, "\\VUPS", "vd", 0x80) &&
		      acpi_evalf(NULL, NULL, "\\VSDS", "vdd", status, 1);
		break;
	default:
		return -ENOSYS;
	}

	return (res)? 0 : -EIO;
}

static int video_autosw_get(void)
{
	int autosw = 0;

	switch (video_supported) {
	case TPACPI_VIDEO_570:
		if (!acpi_evalf(vid_handle, &autosw, "SWIT", "d"))
			return -EIO;
		break;
	case TPACPI_VIDEO_770:
	case TPACPI_VIDEO_NEW:
		if (!acpi_evalf(vid_handle, &autosw, "^VDEE", "d"))
			return -EIO;
		break;
	default:
		return -ENOSYS;
	}

	return autosw & 1;
}

static int video_autosw_set(int enable)
{
	if (!acpi_evalf(vid_handle, NULL, "_DOS", "vd", (enable)? 1 : 0))
		return -EIO;
	return 0;
}

static int video_outputsw_cycle(void)
{
	int autosw = video_autosw_get();
	int res;

	if (autosw < 0)
		return autosw;

	switch (video_supported) {
	case TPACPI_VIDEO_570:
		res = video_autosw_set(1);
		if (res)
			return res;
		res = acpi_evalf(ec_handle, NULL, "_Q16", "v");
		break;
	case TPACPI_VIDEO_770:
	case TPACPI_VIDEO_NEW:
		res = video_autosw_set(1);
		if (res)
			return res;
		res = acpi_evalf(vid_handle, NULL, "VSWT", "v");
		break;
	default:
		return -ENOSYS;
	}
	if (!autosw && video_autosw_set(autosw)) {
		printk(TPACPI_ERR
		       "video auto-switch left enabled due to error\n");
		return -EIO;
	}

	return (res)? 0 : -EIO;
}

static int video_expand_toggle(void)
{
	switch (video_supported) {
	case TPACPI_VIDEO_570:
		return acpi_evalf(ec_handle, NULL, "_Q17", "v")?
			0 : -EIO;
	case TPACPI_VIDEO_770:
		return acpi_evalf(vid_handle, NULL, "VEXP", "v")?
			0 : -EIO;
	case TPACPI_VIDEO_NEW:
		return acpi_evalf(NULL, NULL, "\\VEXP", "v")?
			0 : -EIO;
	default:
		return -ENOSYS;
	}
	/* not reached */
}

static int video_read(char *p)
{
	int status, autosw;
	int len = 0;

	if (video_supported == TPACPI_VIDEO_NONE) {
		len += sprintf(p + len, "status:\t\tnot supported\n");
		return len;
	}

	status = video_outputsw_get();
	if (status < 0)
		return status;

	autosw = video_autosw_get();
	if (autosw < 0)
		return autosw;

	len += sprintf(p + len, "status:\t\tsupported\n");
	len += sprintf(p + len, "lcd:\t\t%s\n", enabled(status, 0));
	len += sprintf(p + len, "crt:\t\t%s\n", enabled(status, 1));
	if (video_supported == TPACPI_VIDEO_NEW)
		len += sprintf(p + len, "dvi:\t\t%s\n", enabled(status, 3));
	len += sprintf(p + len, "auto:\t\t%s\n", enabled(autosw, 0));
	len += sprintf(p + len, "commands:\tlcd_enable, lcd_disable\n");
	len += sprintf(p + len, "commands:\tcrt_enable, crt_disable\n");
	if (video_supported == TPACPI_VIDEO_NEW)
		len += sprintf(p + len, "commands:\tdvi_enable, dvi_disable\n");
	len += sprintf(p + len, "commands:\tauto_enable, auto_disable\n");
	len += sprintf(p + len, "commands:\tvideo_switch, expand_toggle\n");

	return len;
}

static int video_write(char *buf)
{
	char *cmd;
	int enable, disable, status;
	int res;

	if (video_supported == TPACPI_VIDEO_NONE)
		return -ENODEV;

	enable = 0;
	disable = 0;

	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "lcd_enable") == 0) {
			enable |= TP_ACPI_VIDEO_S_LCD;
		} else if (strlencmp(cmd, "lcd_disable") == 0) {
			disable |= TP_ACPI_VIDEO_S_LCD;
		} else if (strlencmp(cmd, "crt_enable") == 0) {
			enable |= TP_ACPI_VIDEO_S_CRT;
		} else if (strlencmp(cmd, "crt_disable") == 0) {
			disable |= TP_ACPI_VIDEO_S_CRT;
		} else if (video_supported == TPACPI_VIDEO_NEW &&
			   strlencmp(cmd, "dvi_enable") == 0) {
			enable |= TP_ACPI_VIDEO_S_DVI;
		} else if (video_supported == TPACPI_VIDEO_NEW &&
			   strlencmp(cmd, "dvi_disable") == 0) {
			disable |= TP_ACPI_VIDEO_S_DVI;
		} else if (strlencmp(cmd, "auto_enable") == 0) {
			res = video_autosw_set(1);
			if (res)
				return res;
		} else if (strlencmp(cmd, "auto_disable") == 0) {
			res = video_autosw_set(0);
			if (res)
				return res;
		} else if (strlencmp(cmd, "video_switch") == 0) {
			res = video_outputsw_cycle();
			if (res)
				return res;
		} else if (strlencmp(cmd, "expand_toggle") == 0) {
			res = video_expand_toggle();
			if (res)
				return res;
		} else
			return -EINVAL;
	}

	if (enable || disable) {
		status = video_outputsw_get();
		if (status < 0)
			return status;
		res = video_outputsw_set((status & ~disable) | enable);
		if (res)
			return res;
	}

	return 0;
}

static struct ibm_struct video_driver_data = {
	.name = "video",
	.read = video_read,
	.write = video_write,
	.exit = video_exit,
};

#endif /* CONFIG_THINKPAD_ACPI_VIDEO */

/*************************************************************************
 * Light (thinklight) subdriver
 */

TPACPI_HANDLE(lght, root, "\\LGHT");	/* A21e, A2xm/p, T20-22, X20-21 */
TPACPI_HANDLE(ledb, ec, "LEDB");		/* G4x */

static int __init light_init(struct ibm_init_struct *iibm)
{
	vdbg_printk(TPACPI_DBG_INIT, "initializing light subdriver\n");

	TPACPI_ACPIHANDLE_INIT(ledb);
	TPACPI_ACPIHANDLE_INIT(lght);
	TPACPI_ACPIHANDLE_INIT(cmos);

	/* light not supported on 570, 600e/x, 770e, 770x, G4x, R30, R31 */
	tp_features.light = (cmos_handle || lght_handle) && !ledb_handle;

	if (tp_features.light)
		/* light status not supported on
		   570, 600e/x, 770e, 770x, G4x, R30, R31, R32, X20 */
		tp_features.light_status =
			acpi_evalf(ec_handle, NULL, "KBLT", "qv");

	vdbg_printk(TPACPI_DBG_INIT, "light is %s\n",
		str_supported(tp_features.light));

	return (tp_features.light)? 0 : 1;
}

static int light_read(char *p)
{
	int len = 0;
	int status = 0;

	if (!tp_features.light) {
		len += sprintf(p + len, "status:\t\tnot supported\n");
	} else if (!tp_features.light_status) {
		len += sprintf(p + len, "status:\t\tunknown\n");
		len += sprintf(p + len, "commands:\ton, off\n");
	} else {
		if (!acpi_evalf(ec_handle, &status, "KBLT", "d"))
			return -EIO;
		len += sprintf(p + len, "status:\t\t%s\n", onoff(status, 0));
		len += sprintf(p + len, "commands:\ton, off\n");
	}

	return len;
}

static int light_write(char *buf)
{
	int cmos_cmd, lght_cmd;
	char *cmd;
	int success;

	if (!tp_features.light)
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "on") == 0) {
			cmos_cmd = 0x0c;
			lght_cmd = 1;
		} else if (strlencmp(cmd, "off") == 0) {
			cmos_cmd = 0x0d;
			lght_cmd = 0;
		} else
			return -EINVAL;

		success = cmos_handle ?
		    acpi_evalf(cmos_handle, NULL, NULL, "vd", cmos_cmd) :
		    acpi_evalf(lght_handle, NULL, NULL, "vd", lght_cmd);
		if (!success)
			return -EIO;
	}

	return 0;
}

static struct ibm_struct light_driver_data = {
	.name = "light",
	.read = light_read,
	.write = light_write,
};

/*************************************************************************
 * Dock subdriver
 */

#ifdef CONFIG_THINKPAD_ACPI_DOCK

static void dock_notify(struct ibm_struct *ibm, u32 event);
static int dock_read(char *p);
static int dock_write(char *buf);

TPACPI_HANDLE(dock, root, "\\_SB.GDCK",	/* X30, X31, X40 */
	   "\\_SB.PCI0.DOCK",	/* 600e/x,770e,770x,A2xm/p,T20-22,X20-21 */
	   "\\_SB.PCI0.PCI1.DOCK",	/* all others */
	   "\\_SB.PCI.ISA.SLCE",	/* 570 */
    );				/* A21e,G4x,R30,R31,R32,R40,R40e,R50e */

/* don't list other alternatives as we install a notify handler on the 570 */
TPACPI_HANDLE(pci, root, "\\_SB.PCI");	/* 570 */

static const struct acpi_device_id ibm_pci_device_ids[] = {
	{PCI_ROOT_HID_STRING, 0},
	{"", 0},
};

static struct tp_acpi_drv_struct ibm_dock_acpidriver[2] = {
	{
	 .notify = dock_notify,
	 .handle = &dock_handle,
	 .type = ACPI_SYSTEM_NOTIFY,
	},
	{
	/* THIS ONE MUST NEVER BE USED FOR DRIVER AUTOLOADING.
	 * We just use it to get notifications of dock hotplug
	 * in very old thinkpads */
	 .hid = ibm_pci_device_ids,
	 .notify = dock_notify,
	 .handle = &pci_handle,
	 .type = ACPI_SYSTEM_NOTIFY,
	},
};

static struct ibm_struct dock_driver_data[2] = {
	{
	 .name = "dock",
	 .read = dock_read,
	 .write = dock_write,
	 .acpi = &ibm_dock_acpidriver[0],
	},
	{
	 .name = "dock",
	 .acpi = &ibm_dock_acpidriver[1],
	},
};

#define dock_docked() (_sta(dock_handle) & 1)

static int __init dock_init(struct ibm_init_struct *iibm)
{
	vdbg_printk(TPACPI_DBG_INIT, "initializing dock subdriver\n");

	TPACPI_ACPIHANDLE_INIT(dock);

	vdbg_printk(TPACPI_DBG_INIT, "dock is %s\n",
		str_supported(dock_handle != NULL));

	return (dock_handle)? 0 : 1;
}

static int __init dock_init2(struct ibm_init_struct *iibm)
{
	int dock2_needed;

	vdbg_printk(TPACPI_DBG_INIT, "initializing dock subdriver part 2\n");

	if (dock_driver_data[0].flags.acpi_driver_registered &&
	    dock_driver_data[0].flags.acpi_notify_installed) {
		TPACPI_ACPIHANDLE_INIT(pci);
		dock2_needed = (pci_handle != NULL);
		vdbg_printk(TPACPI_DBG_INIT,
			    "dock PCI handler for the TP 570 is %s\n",
			    str_supported(dock2_needed));
	} else {
		vdbg_printk(TPACPI_DBG_INIT,
		"dock subdriver part 2 not required\n");
		dock2_needed = 0;
	}

	return (dock2_needed)? 0 : 1;
}

static void dock_notify(struct ibm_struct *ibm, u32 event)
{
	int docked = dock_docked();
	int pci = ibm->acpi->hid && ibm->acpi->device &&
		acpi_match_device_ids(ibm->acpi->device, ibm_pci_device_ids);
	int data;

	if (event == 1 && !pci)	/* 570 */
		data = 1;	/* button */
	else if (event == 1 && pci)	/* 570 */
		data = 3;	/* dock */
	else if (event == 3 && docked)
		data = 1;	/* button */
	else if (event == 3 && !docked)
		data = 2;	/* undock */
	else if (event == 0 && docked)
		data = 3;	/* dock */
	else {
		printk(TPACPI_ERR "unknown dock event %d, status %d\n",
		       event, _sta(dock_handle));
		data = 0;	/* unknown */
	}
	acpi_bus_generate_event(ibm->acpi->device, event, data);
}

static int dock_read(char *p)
{
	int len = 0;
	int docked = dock_docked();

	if (!dock_handle)
		len += sprintf(p + len, "status:\t\tnot supported\n");
	else if (!docked)
		len += sprintf(p + len, "status:\t\tundocked\n");
	else {
		len += sprintf(p + len, "status:\t\tdocked\n");
		len += sprintf(p + len, "commands:\tdock, undock\n");
	}

	return len;
}

static int dock_write(char *buf)
{
	char *cmd;

	if (!dock_docked())
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "undock") == 0) {
			if (!acpi_evalf(dock_handle, NULL, "_DCK", "vd", 0) ||
			    !acpi_evalf(dock_handle, NULL, "_EJ0", "vd", 1))
				return -EIO;
		} else if (strlencmp(cmd, "dock") == 0) {
			if (!acpi_evalf(dock_handle, NULL, "_DCK", "vd", 1))
				return -EIO;
		} else
			return -EINVAL;
	}

	return 0;
}

#endif /* CONFIG_THINKPAD_ACPI_DOCK */

/*************************************************************************
 * Bay subdriver
 */

#ifdef CONFIG_THINKPAD_ACPI_BAY

TPACPI_HANDLE(bay, root, "\\_SB.PCI.IDE.SECN.MAST",	/* 570 */
	   "\\_SB.PCI0.IDE0.IDES.IDSM",	/* 600e/x, 770e, 770x */
	   "\\_SB.PCI0.SATA.SCND.MSTR",	/* T60, X60, Z60 */
	   "\\_SB.PCI0.IDE0.SCND.MSTR",	/* all others */
	   );				/* A21e, R30, R31 */
TPACPI_HANDLE(bay_ej, bay, "_EJ3",	/* 600e/x, A2xm/p, A3x */
	   "_EJ0",		/* all others */
	   );			/* 570,A21e,G4x,R30,R31,R32,R40e,R50e */
TPACPI_HANDLE(bay2, root, "\\_SB.PCI0.IDE0.PRIM.SLAV",	/* A3x, R32 */
	   "\\_SB.PCI0.IDE0.IDEP.IDPS",	/* 600e/x, 770e, 770x */
	   );				/* all others */
TPACPI_HANDLE(bay2_ej, bay2, "_EJ3",	/* 600e/x, 770e, A3x */
	   "_EJ0",			/* 770x */
	   );				/* all others */

static int __init bay_init(struct ibm_init_struct *iibm)
{
	vdbg_printk(TPACPI_DBG_INIT, "initializing bay subdriver\n");

	TPACPI_ACPIHANDLE_INIT(bay);
	if (bay_handle)
		TPACPI_ACPIHANDLE_INIT(bay_ej);
	TPACPI_ACPIHANDLE_INIT(bay2);
	if (bay2_handle)
		TPACPI_ACPIHANDLE_INIT(bay2_ej);

	tp_features.bay_status = bay_handle &&
		acpi_evalf(bay_handle, NULL, "_STA", "qv");
	tp_features.bay_status2 = bay2_handle &&
		acpi_evalf(bay2_handle, NULL, "_STA", "qv");

	tp_features.bay_eject = bay_handle && bay_ej_handle &&
		(strlencmp(bay_ej_path, "_EJ0") == 0 || experimental);
	tp_features.bay_eject2 = bay2_handle && bay2_ej_handle &&
		(strlencmp(bay2_ej_path, "_EJ0") == 0 || experimental);

	vdbg_printk(TPACPI_DBG_INIT,
		"bay 1: status %s, eject %s; bay 2: status %s, eject %s\n",
		str_supported(tp_features.bay_status),
		str_supported(tp_features.bay_eject),
		str_supported(tp_features.bay_status2),
		str_supported(tp_features.bay_eject2));

	return (tp_features.bay_status || tp_features.bay_eject ||
		tp_features.bay_status2 || tp_features.bay_eject2)? 0 : 1;
}

static void bay_notify(struct ibm_struct *ibm, u32 event)
{
	acpi_bus_generate_event(ibm->acpi->device, event, 0);
}

#define bay_occupied(b) (_sta(b##_handle) & 1)

static int bay_read(char *p)
{
	int len = 0;
	int occupied = bay_occupied(bay);
	int occupied2 = bay_occupied(bay2);
	int eject, eject2;

	len += sprintf(p + len, "status:\t\t%s\n",
		tp_features.bay_status ?
			(occupied ? "occupied" : "unoccupied") :
				"not supported");
	if (tp_features.bay_status2)
		len += sprintf(p + len, "status2:\t%s\n", occupied2 ?
			       "occupied" : "unoccupied");

	eject = tp_features.bay_eject && occupied;
	eject2 = tp_features.bay_eject2 && occupied2;

	if (eject && eject2)
		len += sprintf(p + len, "commands:\teject, eject2\n");
	else if (eject)
		len += sprintf(p + len, "commands:\teject\n");
	else if (eject2)
		len += sprintf(p + len, "commands:\teject2\n");

	return len;
}

static int bay_write(char *buf)
{
	char *cmd;

	if (!tp_features.bay_eject && !tp_features.bay_eject2)
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (tp_features.bay_eject && strlencmp(cmd, "eject") == 0) {
			if (!acpi_evalf(bay_ej_handle, NULL, NULL, "vd", 1))
				return -EIO;
		} else if (tp_features.bay_eject2 &&
			   strlencmp(cmd, "eject2") == 0) {
			if (!acpi_evalf(bay2_ej_handle, NULL, NULL, "vd", 1))
				return -EIO;
		} else
			return -EINVAL;
	}

	return 0;
}

static struct tp_acpi_drv_struct ibm_bay_acpidriver = {
	.notify = bay_notify,
	.handle = &bay_handle,
	.type = ACPI_SYSTEM_NOTIFY,
};

static struct ibm_struct bay_driver_data = {
	.name = "bay",
	.read = bay_read,
	.write = bay_write,
	.acpi = &ibm_bay_acpidriver,
};

#endif /* CONFIG_THINKPAD_ACPI_BAY */

/*************************************************************************
 * CMOS subdriver
 */

/* sysfs cmos_command -------------------------------------------------- */
static ssize_t cmos_command_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long cmos_cmd;
	int res;

	if (parse_strtoul(buf, 21, &cmos_cmd))
		return -EINVAL;

	res = issue_thinkpad_cmos_command(cmos_cmd);
	return (res)? res : count;
}

static struct device_attribute dev_attr_cmos_command =
	__ATTR(cmos_command, S_IWUSR, NULL, cmos_command_store);

/* --------------------------------------------------------------------- */

static int __init cmos_init(struct ibm_init_struct *iibm)
{
	int res;

	vdbg_printk(TPACPI_DBG_INIT,
		"initializing cmos commands subdriver\n");

	TPACPI_ACPIHANDLE_INIT(cmos);

	vdbg_printk(TPACPI_DBG_INIT, "cmos commands are %s\n",
		str_supported(cmos_handle != NULL));

	res = device_create_file(&tpacpi_pdev->dev, &dev_attr_cmos_command);
	if (res)
		return res;

	return (cmos_handle)? 0 : 1;
}

static void cmos_exit(void)
{
	device_remove_file(&tpacpi_pdev->dev, &dev_attr_cmos_command);
}

static int cmos_read(char *p)
{
	int len = 0;

	/* cmos not supported on 570, 600e/x, 770e, 770x, A21e, A2xm/p,
	   R30, R31, T20-22, X20-21 */
	if (!cmos_handle)
		len += sprintf(p + len, "status:\t\tnot supported\n");
	else {
		len += sprintf(p + len, "status:\t\tsupported\n");
		len += sprintf(p + len, "commands:\t<cmd> (<cmd> is 0-21)\n");
	}

	return len;
}

static int cmos_write(char *buf)
{
	char *cmd;
	int cmos_cmd, res;

	while ((cmd = next_cmd(&buf))) {
		if (sscanf(cmd, "%u", &cmos_cmd) == 1 &&
		    cmos_cmd >= 0 && cmos_cmd <= 21) {
			/* cmos_cmd set */
		} else
			return -EINVAL;

		res = issue_thinkpad_cmos_command(cmos_cmd);
		if (res)
			return res;
	}

	return 0;
}

static struct ibm_struct cmos_driver_data = {
	.name = "cmos",
	.read = cmos_read,
	.write = cmos_write,
	.exit = cmos_exit,
};

/*************************************************************************
 * LED subdriver
 */

enum led_access_mode {
	TPACPI_LED_NONE = 0,
	TPACPI_LED_570,	/* 570 */
	TPACPI_LED_OLD,	/* 600e/x, 770e, 770x, A21e, A2xm/p, T20-22, X20-21 */
	TPACPI_LED_NEW,	/* all others */
};

enum {	/* For TPACPI_LED_OLD */
	TPACPI_LED_EC_HLCL = 0x0c,	/* EC reg to get led to power on */
	TPACPI_LED_EC_HLBL = 0x0d,	/* EC reg to blink a lit led */
	TPACPI_LED_EC_HLMS = 0x0e,	/* EC reg to select led to command */
};

static enum led_access_mode led_supported;

TPACPI_HANDLE(led, ec, "SLED",	/* 570 */
	   "SYSL",		/* 600e/x, 770e, 770x, A21e, A2xm/p, */
				/* T20-22, X20-21 */
	   "LED",		/* all others */
	   );			/* R30, R31 */

static int __init led_init(struct ibm_init_struct *iibm)
{
	vdbg_printk(TPACPI_DBG_INIT, "initializing LED subdriver\n");

	TPACPI_ACPIHANDLE_INIT(led);

	if (!led_handle)
		/* led not supported on R30, R31 */
		led_supported = TPACPI_LED_NONE;
	else if (strlencmp(led_path, "SLED") == 0)
		/* 570 */
		led_supported = TPACPI_LED_570;
	else if (strlencmp(led_path, "SYSL") == 0)
		/* 600e/x, 770e, 770x, A21e, A2xm/p, T20-22, X20-21 */
		led_supported = TPACPI_LED_OLD;
	else
		/* all others */
		led_supported = TPACPI_LED_NEW;

	vdbg_printk(TPACPI_DBG_INIT, "LED commands are %s, mode %d\n",
		str_supported(led_supported), led_supported);

	return (led_supported != TPACPI_LED_NONE)? 0 : 1;
}

#define led_status(s) ((s) == 0 ? "off" : ((s) == 1 ? "on" : "blinking"))

static int led_read(char *p)
{
	int len = 0;

	if (!led_supported) {
		len += sprintf(p + len, "status:\t\tnot supported\n");
		return len;
	}
	len += sprintf(p + len, "status:\t\tsupported\n");

	if (led_supported == TPACPI_LED_570) {
		/* 570 */
		int i, status;
		for (i = 0; i < 8; i++) {
			if (!acpi_evalf(ec_handle,
					&status, "GLED", "dd", 1 << i))
				return -EIO;
			len += sprintf(p + len, "%d:\t\t%s\n",
				       i, led_status(status));
		}
	}

	len += sprintf(p + len, "commands:\t"
		       "<led> on, <led> off, <led> blink (<led> is 0-7)\n");

	return len;
}

/* off, on, blink */
static const int led_sled_arg1[] = { 0, 1, 3 };
static const int led_exp_hlbl[] = { 0, 0, 1 };	/* led# * */
static const int led_exp_hlcl[] = { 0, 1, 1 };	/* led# * */
static const int led_led_arg1[] = { 0, 0x80, 0xc0 };

static int led_write(char *buf)
{
	char *cmd;
	int led, ind, ret;

	if (!led_supported)
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (sscanf(cmd, "%d", &led) != 1 || led < 0 || led > 7)
			return -EINVAL;

		if (strstr(cmd, "off")) {
			ind = 0;
		} else if (strstr(cmd, "on")) {
			ind = 1;
		} else if (strstr(cmd, "blink")) {
			ind = 2;
		} else
			return -EINVAL;

		if (led_supported == TPACPI_LED_570) {
			/* 570 */
			led = 1 << led;
			if (!acpi_evalf(led_handle, NULL, NULL, "vdd",
					led, led_sled_arg1[ind]))
				return -EIO;
		} else if (led_supported == TPACPI_LED_OLD) {
			/* 600e/x, 770e, 770x, A21e, A2xm/p, T20-22, X20 */
			led = 1 << led;
			ret = ec_write(TPACPI_LED_EC_HLMS, led);
			if (ret >= 0)
				ret = ec_write(TPACPI_LED_EC_HLBL,
						led * led_exp_hlbl[ind]);
			if (ret >= 0)
				ret = ec_write(TPACPI_LED_EC_HLCL,
						led * led_exp_hlcl[ind]);
			if (ret < 0)
				return ret;
		} else {
			/* all others */
			if (!acpi_evalf(led_handle, NULL, NULL, "vdd",
					led, led_led_arg1[ind]))
				return -EIO;
		}
	}

	return 0;
}

static struct ibm_struct led_driver_data = {
	.name = "led",
	.read = led_read,
	.write = led_write,
};

/*************************************************************************
 * Beep subdriver
 */

TPACPI_HANDLE(beep, ec, "BEEP");	/* all except R30, R31 */

static int __init beep_init(struct ibm_init_struct *iibm)
{
	vdbg_printk(TPACPI_DBG_INIT, "initializing beep subdriver\n");

	TPACPI_ACPIHANDLE_INIT(beep);

	vdbg_printk(TPACPI_DBG_INIT, "beep is %s\n",
		str_supported(beep_handle != NULL));

	return (beep_handle)? 0 : 1;
}

static int beep_read(char *p)
{
	int len = 0;

	if (!beep_handle)
		len += sprintf(p + len, "status:\t\tnot supported\n");
	else {
		len += sprintf(p + len, "status:\t\tsupported\n");
		len += sprintf(p + len, "commands:\t<cmd> (<cmd> is 0-17)\n");
	}

	return len;
}

static int beep_write(char *buf)
{
	char *cmd;
	int beep_cmd;

	if (!beep_handle)
		return -ENODEV;

	while ((cmd = next_cmd(&buf))) {
		if (sscanf(cmd, "%u", &beep_cmd) == 1 &&
		    beep_cmd >= 0 && beep_cmd <= 17) {
			/* beep_cmd set */
		} else
			return -EINVAL;
		if (!acpi_evalf(beep_handle, NULL, NULL, "vdd", beep_cmd, 0))
			return -EIO;
	}

	return 0;
}

static struct ibm_struct beep_driver_data = {
	.name = "beep",
	.read = beep_read,
	.write = beep_write,
};

/*************************************************************************
 * Thermal subdriver
 */

enum thermal_access_mode {
	TPACPI_THERMAL_NONE = 0,	/* No thermal support */
	TPACPI_THERMAL_ACPI_TMP07,	/* Use ACPI TMP0-7 */
	TPACPI_THERMAL_ACPI_UPDT,	/* Use ACPI TMP0-7 with UPDT */
	TPACPI_THERMAL_TPEC_8,		/* Use ACPI EC regs, 8 sensors */
	TPACPI_THERMAL_TPEC_16,		/* Use ACPI EC regs, 16 sensors */
};

enum { /* TPACPI_THERMAL_TPEC_* */
	TP_EC_THERMAL_TMP0 = 0x78,	/* ACPI EC regs TMP 0..7 */
	TP_EC_THERMAL_TMP8 = 0xC0,	/* ACPI EC regs TMP 8..15 */
	TP_EC_THERMAL_TMP_NA = -128,	/* ACPI EC sensor not available */
};

#define TPACPI_MAX_THERMAL_SENSORS 16	/* Max thermal sensors supported */
struct ibm_thermal_sensors_struct {
	s32 temp[TPACPI_MAX_THERMAL_SENSORS];
};

static enum thermal_access_mode thermal_read_mode;

/* idx is zero-based */
static int thermal_get_sensor(int idx, s32 *value)
{
	int t;
	s8 tmp;
	char tmpi[5];

	t = TP_EC_THERMAL_TMP0;

	switch (thermal_read_mode) {
#if TPACPI_MAX_THERMAL_SENSORS >= 16
	case TPACPI_THERMAL_TPEC_16:
		if (idx >= 8 && idx <= 15) {
			t = TP_EC_THERMAL_TMP8;
			idx -= 8;
		}
		/* fallthrough */
#endif
	case TPACPI_THERMAL_TPEC_8:
		if (idx <= 7) {
			if (!acpi_ec_read(t + idx, &tmp))
				return -EIO;
			*value = tmp * 1000;
			return 0;
		}
		break;

	case TPACPI_THERMAL_ACPI_UPDT:
		if (idx <= 7) {
			snprintf(tmpi, sizeof(tmpi), "TMP%c", '0' + idx);
			if (!acpi_evalf(ec_handle, NULL, "UPDT", "v"))
				return -EIO;
			if (!acpi_evalf(ec_handle, &t, tmpi, "d"))
				return -EIO;
			*value = (t - 2732) * 100;
			return 0;
		}
		break;

	case TPACPI_THERMAL_ACPI_TMP07:
		if (idx <= 7) {
			snprintf(tmpi, sizeof(tmpi), "TMP%c", '0' + idx);
			if (!acpi_evalf(ec_handle, &t, tmpi, "d"))
				return -EIO;
			if (t > 127 || t < -127)
				t = TP_EC_THERMAL_TMP_NA;
			*value = t * 1000;
			return 0;
		}
		break;

	case TPACPI_THERMAL_NONE:
	default:
		return -ENOSYS;
	}

	return -EINVAL;
}

static int thermal_get_sensors(struct ibm_thermal_sensors_struct *s)
{
	int res, i;
	int n;

	n = 8;
	i = 0;

	if (!s)
		return -EINVAL;

	if (thermal_read_mode == TPACPI_THERMAL_TPEC_16)
		n = 16;

	for (i = 0 ; i < n; i++) {
		res = thermal_get_sensor(i, &s->temp[i]);
		if (res)
			return res;
	}

	return n;
}

/* sysfs temp##_input -------------------------------------------------- */

static ssize_t thermal_temp_input_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct sensor_device_attribute *sensor_attr =
					to_sensor_dev_attr(attr);
	int idx = sensor_attr->index;
	s32 value;
	int res;

	res = thermal_get_sensor(idx, &value);
	if (res)
		return res;
	if (value == TP_EC_THERMAL_TMP_NA * 1000)
		return -ENXIO;

	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}

#define THERMAL_SENSOR_ATTR_TEMP(_idxA, _idxB) \
	 SENSOR_ATTR(temp##_idxA##_input, S_IRUGO, \
		     thermal_temp_input_show, NULL, _idxB)

static struct sensor_device_attribute sensor_dev_attr_thermal_temp_input[] = {
	THERMAL_SENSOR_ATTR_TEMP(1, 0),
	THERMAL_SENSOR_ATTR_TEMP(2, 1),
	THERMAL_SENSOR_ATTR_TEMP(3, 2),
	THERMAL_SENSOR_ATTR_TEMP(4, 3),
	THERMAL_SENSOR_ATTR_TEMP(5, 4),
	THERMAL_SENSOR_ATTR_TEMP(6, 5),
	THERMAL_SENSOR_ATTR_TEMP(7, 6),
	THERMAL_SENSOR_ATTR_TEMP(8, 7),
	THERMAL_SENSOR_ATTR_TEMP(9, 8),
	THERMAL_SENSOR_ATTR_TEMP(10, 9),
	THERMAL_SENSOR_ATTR_TEMP(11, 10),
	THERMAL_SENSOR_ATTR_TEMP(12, 11),
	THERMAL_SENSOR_ATTR_TEMP(13, 12),
	THERMAL_SENSOR_ATTR_TEMP(14, 13),
	THERMAL_SENSOR_ATTR_TEMP(15, 14),
	THERMAL_SENSOR_ATTR_TEMP(16, 15),
};

#define THERMAL_ATTRS(X) \
	&sensor_dev_attr_thermal_temp_input[X].dev_attr.attr

static struct attribute *thermal_temp_input_attr[] = {
	THERMAL_ATTRS(8),
	THERMAL_ATTRS(9),
	THERMAL_ATTRS(10),
	THERMAL_ATTRS(11),
	THERMAL_ATTRS(12),
	THERMAL_ATTRS(13),
	THERMAL_ATTRS(14),
	THERMAL_ATTRS(15),
	THERMAL_ATTRS(0),
	THERMAL_ATTRS(1),
	THERMAL_ATTRS(2),
	THERMAL_ATTRS(3),
	THERMAL_ATTRS(4),
	THERMAL_ATTRS(5),
	THERMAL_ATTRS(6),
	THERMAL_ATTRS(7),
	NULL
};

static const struct attribute_group thermal_temp_input16_group = {
	.attrs = thermal_temp_input_attr
};

static const struct attribute_group thermal_temp_input8_group = {
	.attrs = &thermal_temp_input_attr[8]
};

#undef THERMAL_SENSOR_ATTR_TEMP
#undef THERMAL_ATTRS

/* --------------------------------------------------------------------- */

static int __init thermal_init(struct ibm_init_struct *iibm)
{
	u8 t, ta1, ta2;
	int i;
	int acpi_tmp7;
	int res;

	vdbg_printk(TPACPI_DBG_INIT, "initializing thermal subdriver\n");

	acpi_tmp7 = acpi_evalf(ec_handle, NULL, "TMP7", "qv");

	if (thinkpad_id.ec_model) {
		/*
		 * Direct EC access mode: sensors at registers
		 * 0x78-0x7F, 0xC0-0xC7.  Registers return 0x00 for
		 * non-implemented, thermal sensors return 0x80 when
		 * not available
		 */

		ta1 = ta2 = 0;
		for (i = 0; i < 8; i++) {
			if (acpi_ec_read(TP_EC_THERMAL_TMP0 + i, &t)) {
				ta1 |= t;
			} else {
				ta1 = 0;
				break;
			}
			if (acpi_ec_read(TP_EC_THERMAL_TMP8 + i, &t)) {
				ta2 |= t;
			} else {
				ta1 = 0;
				break;
			}
		}
		if (ta1 == 0) {
			/* This is sheer paranoia, but we handle it anyway */
			if (acpi_tmp7) {
				printk(TPACPI_ERR
				       "ThinkPad ACPI EC access misbehaving, "
				       "falling back to ACPI TMPx access "
				       "mode\n");
				thermal_read_mode = TPACPI_THERMAL_ACPI_TMP07;
			} else {
				printk(TPACPI_ERR
				       "ThinkPad ACPI EC access misbehaving, "
				       "disabling thermal sensors access\n");
				thermal_read_mode = TPACPI_THERMAL_NONE;
			}
		} else {
			thermal_read_mode =
			    (ta2 != 0) ?
			    TPACPI_THERMAL_TPEC_16 : TPACPI_THERMAL_TPEC_8;
		}
	} else if (acpi_tmp7) {
		if (acpi_evalf(ec_handle, NULL, "UPDT", "qv")) {
			/* 600e/x, 770e, 770x */
			thermal_read_mode = TPACPI_THERMAL_ACPI_UPDT;
		} else {
			/* Standard ACPI TMPx access, max 8 sensors */
			thermal_read_mode = TPACPI_THERMAL_ACPI_TMP07;
		}
	} else {
		/* temperatures not supported on 570, G4x, R30, R31, R32 */
		thermal_read_mode = TPACPI_THERMAL_NONE;
	}

	vdbg_printk(TPACPI_DBG_INIT, "thermal is %s, mode %d\n",
		str_supported(thermal_read_mode != TPACPI_THERMAL_NONE),
		thermal_read_mode);

	switch (thermal_read_mode) {
	case TPACPI_THERMAL_TPEC_16:
		res = sysfs_create_group(&tpacpi_sensors_pdev->dev.kobj,
				&thermal_temp_input16_group);
		if (res)
			return res;
		break;
	case TPACPI_THERMAL_TPEC_8:
	case TPACPI_THERMAL_ACPI_TMP07:
	case TPACPI_THERMAL_ACPI_UPDT:
		res = sysfs_create_group(&tpacpi_sensors_pdev->dev.kobj,
				&thermal_temp_input8_group);
		if (res)
			return res;
		break;
	case TPACPI_THERMAL_NONE:
	default:
		return 1;
	}

	return 0;
}

static void thermal_exit(void)
{
	switch (thermal_read_mode) {
	case TPACPI_THERMAL_TPEC_16:
		sysfs_remove_group(&tpacpi_sensors_pdev->dev.kobj,
				   &thermal_temp_input16_group);
		break;
	case TPACPI_THERMAL_TPEC_8:
	case TPACPI_THERMAL_ACPI_TMP07:
	case TPACPI_THERMAL_ACPI_UPDT:
		sysfs_remove_group(&tpacpi_sensors_pdev->dev.kobj,
				   &thermal_temp_input16_group);
		break;
	case TPACPI_THERMAL_NONE:
	default:
		break;
	}
}

static int thermal_read(char *p)
{
	int len = 0;
	int n, i;
	struct ibm_thermal_sensors_struct t;

	n = thermal_get_sensors(&t);
	if (unlikely(n < 0))
		return n;

	len += sprintf(p + len, "temperatures:\t");

	if (n > 0) {
		for (i = 0; i < (n - 1); i++)
			len += sprintf(p + len, "%d ", t.temp[i] / 1000);
		len += sprintf(p + len, "%d\n", t.temp[i] / 1000);
	} else
		len += sprintf(p + len, "not supported\n");

	return len;
}

static struct ibm_struct thermal_driver_data = {
	.name = "thermal",
	.read = thermal_read,
	.exit = thermal_exit,
};

/*************************************************************************
 * EC Dump subdriver
 */

static u8 ecdump_regs[256];

static int ecdump_read(char *p)
{
	int len = 0;
	int i, j;
	u8 v;

	len += sprintf(p + len, "EC      "
		       " +00 +01 +02 +03 +04 +05 +06 +07"
		       " +08 +09 +0a +0b +0c +0d +0e +0f\n");
	for (i = 0; i < 256; i += 16) {
		len += sprintf(p + len, "EC 0x%02x:", i);
		for (j = 0; j < 16; j++) {
			if (!acpi_ec_read(i + j, &v))
				break;
			if (v != ecdump_regs[i + j])
				len += sprintf(p + len, " *%02x", v);
			else
				len += sprintf(p + len, "  %02x", v);
			ecdump_regs[i + j] = v;
		}
		len += sprintf(p + len, "\n");
		if (j != 16)
			break;
	}

	/* These are way too dangerous to advertise openly... */
#if 0
	len += sprintf(p + len, "commands:\t0x<offset> 0x<value>"
		       " (<offset> is 00-ff, <value> is 00-ff)\n");
	len += sprintf(p + len, "commands:\t0x<offset> <value>  "
		       " (<offset> is 00-ff, <value> is 0-255)\n");
#endif
	return len;
}

static int ecdump_write(char *buf)
{
	char *cmd;
	int i, v;

	while ((cmd = next_cmd(&buf))) {
		if (sscanf(cmd, "0x%x 0x%x", &i, &v) == 2) {
			/* i and v set */
		} else if (sscanf(cmd, "0x%x %u", &i, &v) == 2) {
			/* i and v set */
		} else
			return -EINVAL;
		if (i >= 0 && i < 256 && v >= 0 && v < 256) {
			if (!acpi_ec_write(i, v))
				return -EIO;
		} else
			return -EINVAL;
	}

	return 0;
}

static struct ibm_struct ecdump_driver_data = {
	.name = "ecdump",
	.read = ecdump_read,
	.write = ecdump_write,
	.flags.experimental = 1,
};

/*************************************************************************
 * Backlight/brightness subdriver
 */

#define TPACPI_BACKLIGHT_DEV_NAME "thinkpad_screen"

static struct backlight_device *ibm_backlight_device;
static int brightness_offset = 0x31;
static int brightness_mode;
static unsigned int brightness_enable = 2; /* 2 = auto, 0 = no, 1 = yes */

static struct mutex brightness_mutex;

/*
 * ThinkPads can read brightness from two places: EC 0x31, or
 * CMOS NVRAM byte 0x5E, bits 0-3.
 */
static int brightness_get(struct backlight_device *bd)
{
	u8 lec = 0, lcmos = 0, level = 0;

	if (brightness_mode & 1) {
		if (!acpi_ec_read(brightness_offset, &lec))
			return -EIO;
		lec &= (tp_features.bright_16levels)? 0x0f : 0x07;
		level = lec;
	};
	if (brightness_mode & 2) {
		lcmos = (nvram_read_byte(TP_NVRAM_ADDR_BRIGHTNESS)
			 & TP_NVRAM_MASK_LEVEL_BRIGHTNESS)
			>> TP_NVRAM_POS_LEVEL_BRIGHTNESS;
		lcmos &= (tp_features.bright_16levels)? 0x0f : 0x07;
		level = lcmos;
	}

	if (brightness_mode == 3 && lec != lcmos) {
		printk(TPACPI_ERR
			"CMOS NVRAM (%u) and EC (%u) do not agree "
			"on display brightness level\n",
			(unsigned int) lcmos,
			(unsigned int) lec);
		return -EIO;
	}

	return level;
}

/* May return EINTR which can always be mapped to ERESTARTSYS */
static int brightness_set(int value)
{
	int cmos_cmd, inc, i, res;
	int current_value;

	if (value > ((tp_features.bright_16levels)? 15 : 7))
		return -EINVAL;

	res = mutex_lock_interruptible(&brightness_mutex);
	if (res < 0)
		return res;

	current_value = brightness_get(NULL);
	if (current_value < 0) {
		res = current_value;
		goto errout;
	}

	cmos_cmd = value > current_value ?
			TP_CMOS_BRIGHTNESS_UP :
			TP_CMOS_BRIGHTNESS_DOWN;
	inc = (value > current_value)? 1 : -1;

	res = 0;
	for (i = current_value; i != value; i += inc) {
		if ((brightness_mode & 2) &&
		    issue_thinkpad_cmos_command(cmos_cmd)) {
			res = -EIO;
			goto errout;
		}
		if ((brightness_mode & 1) &&
		    !acpi_ec_write(brightness_offset, i + inc)) {
			res = -EIO;
			goto errout;;
		}
	}

errout:
	mutex_unlock(&brightness_mutex);
	return res;
}

/* sysfs backlight class ----------------------------------------------- */

static int brightness_update_status(struct backlight_device *bd)
{
	/* it is the backlight class's job (caller) to handle
	 * EINTR and other errors properly */
	return brightness_set(
		(bd->props->fb_blank == FB_BLANK_UNBLANK &&
		 bd->props->power == FB_BLANK_UNBLANK) ?
				bd->props->brightness : 0);
}

static struct backlight_properties ibm_backlight_data = {
	.get_brightness = brightness_get,
	.update_status  = brightness_update_status,
};

/* --------------------------------------------------------------------- */

static int __init tpacpi_query_bcll_levels(acpi_handle handle)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	int rc;

	if (ACPI_SUCCESS(acpi_evaluate_object(handle, NULL, NULL, &buffer))) {
		obj = (union acpi_object *)buffer.pointer;
		if (!obj || (obj->type != ACPI_TYPE_PACKAGE)) {
			printk(TPACPI_ERR "Unknown BCLL data, "
			       "please report this to %s\n", TPACPI_MAIL);
			rc = 0;
		} else {
			rc = obj->package.count;
		}
	} else {
		return 0;
	}

	kfree(buffer.pointer);
	return rc;
}

static acpi_status __init brightness_find_bcll(acpi_handle handle, u32 lvl,
					void *context, void **rv)
{
	char name[ACPI_PATH_SEGMENT_LENGTH];
	struct acpi_buffer buffer = { sizeof(name), &name };

	if (ACPI_SUCCESS(acpi_get_name(handle, ACPI_SINGLE_NAME, &buffer)) &&
	    !strncmp("BCLL", name, sizeof(name) - 1)) {
		if (tpacpi_query_bcll_levels(handle) == 16) {
			*rv = handle;
			return AE_CTRL_TERMINATE;
		} else {
			return AE_OK;
		}
	} else {
		return AE_OK;
	}
}

static int __init brightness_check_levels(void)
{
	int status;
	void *found_node = NULL;

	if (!vid_handle) {
		TPACPI_ACPIHANDLE_INIT(vid);
	}
	if (!vid_handle)
		return 0;

	/* Search for a BCLL package with 16 levels */
	status = acpi_walk_namespace(ACPI_TYPE_PACKAGE, vid_handle, 3,
					brightness_find_bcll, NULL,
					&found_node);

	return (ACPI_SUCCESS(status) && found_node != NULL);
}

static int __init brightness_mode_quirk(void)
{
	switch (thinkpad_id.bios_model) {
	case 0x5531:
	case 0x5631:
	case 0x5731:
	case 0x3037:
	case 0x3437:
	case 0x3537:
		return 1;
	}
	return 0;
}

static int __init brightness_init(struct ibm_init_struct *iibm)
{
	int b;

	vdbg_printk(TPACPI_DBG_INIT, "initializing brightness subdriver\n");

	mutex_init(&brightness_mutex);

	if (!brightness_enable) {
		dbg_printk(TPACPI_DBG_INIT,
			   "brightness support disabled by "
			   "module parameter\n");
		return 1;
	}

	if (!brightness_mode) {
		if (thinkpad_id.vendor == PCI_VENDOR_ID_LENOVO)
			brightness_mode = 2;
		else {
			if (brightness_mode_quirk())
				brightness_mode = 2;
			else
				brightness_mode = 3;
		}

		dbg_printk(TPACPI_DBG_INIT, "selected brightness_mode=%d\n",
			brightness_mode);
	}

	if (brightness_mode > 3)
		return -EINVAL;

	tp_features.bright_16levels =
			thinkpad_id.vendor == PCI_VENDOR_ID_LENOVO &&
			brightness_check_levels();

	b = brightness_get(NULL);
	if (b < 0)
		return 1;

	if (tp_features.bright_16levels)
		printk(TPACPI_INFO
		       "detected a 16-level brightness capable ThinkPad\n");

	ibm_backlight_device = backlight_device_register(
					TPACPI_BACKLIGHT_DEV_NAME, NULL,
					&ibm_backlight_data);
	if (IS_ERR(ibm_backlight_device)) {
		printk(TPACPI_ERR "Could not register backlight device\n");
		return PTR_ERR(ibm_backlight_device);
	}
	vdbg_printk(TPACPI_DBG_INIT, "brightness is supported\n");

	ibm_backlight_device->props->max_brightness =
				(tp_features.bright_16levels)? 15 : 7;
	ibm_backlight_device->props->brightness = b;
	backlight_update_status(ibm_backlight_device);

	return 0;
}

static void brightness_exit(void)
{
	if (ibm_backlight_device) {
		vdbg_printk(TPACPI_DBG_EXIT,
			    "calling backlight_device_unregister()\n");
		backlight_device_unregister(ibm_backlight_device);
		ibm_backlight_device = NULL;
	}
}

static int brightness_read(char *p)
{
	int len = 0;
	int level;

	level = brightness_get(NULL);
	if (level < 0) {
		len += sprintf(p + len, "level:\t\tunreadable\n");
	} else {
		len += sprintf(p + len, "level:\t\t%d\n", level);
		len += sprintf(p + len, "commands:\tup, down\n");
		len += sprintf(p + len, "commands:\tlevel <level>"
			       " (<level> is 0-%d)\n",
			       (tp_features.bright_16levels) ? 15 : 7);
	}

	return len;
}

static int brightness_write(char *buf)
{
	int level;
	int rc;
	char *cmd;
	int max_level = (tp_features.bright_16levels) ? 15 : 7;

	level = brightness_get(NULL);
	if (level < 0)
		return level;

	while ((cmd = next_cmd(&buf))) {
		if (strlencmp(cmd, "up") == 0) {
			if (level < max_level)
				level++;
		} else if (strlencmp(cmd, "down") == 0) {
			if (level > 0)
				level--;
		} else if (sscanf(cmd, "level %d", &level) == 1 &&
			   level >= 0 && level <= max_level) {
			/* new level set */
		} else
			return -EINVAL;
	}

	/*
	 * Now we know what the final level should be, so we try to set it.
	 * Doing it this way makes the syscall restartable in case of EINTR
	 */
	rc = brightness_set(level);
	return (rc == -EINTR)? ERESTARTSYS : rc;
}

static struct ibm_struct brightness_driver_data = {
	.name = "brightness",
	.read = brightness_read,
	.write = brightness_write,
	.exit = brightness_exit,
};

/*************************************************************************
 * Volume subdriver
 */

static int volume_offset = 0x30;

static int volume_read(char *p)
{
	int len = 0;
	u8 level;

	if (!acpi_ec_read(volume_offset, &level)) {
		len += sprintf(p + len, "level:\t\tunreadable\n");
	} else {
		len += sprintf(p + len, "level:\t\t%d\n", level & 0xf);
		len += sprintf(p + len, "mute:\t\t%s\n", onoff(level, 6));
		len += sprintf(p + len, "commands:\tup, down, mute\n");
		len += sprintf(p + len, "commands:\tlevel <level>"
			       " (<level> is 0-15)\n");
	}

	return len;
}

static int volume_write(char *buf)
{
	int cmos_cmd, inc, i;
	u8 level, mute;
	int new_level, new_mute;
	char *cmd;

	while ((cmd = next_cmd(&buf))) {
		if (!acpi_ec_read(volume_offset, &level))
			return -EIO;
		new_mute = mute = level & 0x40;
		new_level = level = level & 0xf;

		if (strlencmp(cmd, "up") == 0) {
			if (mute)
				new_mute = 0;
			else
				new_level = level == 15 ? 15 : level + 1;
		} else if (strlencmp(cmd, "down") == 0) {
			if (mute)
				new_mute = 0;
			else
				new_level = level == 0 ? 0 : level - 1;
		} else if (sscanf(cmd, "level %d", &new_level) == 1 &&
			   new_level >= 0 && new_level <= 15) {
			/* new_level set */
		} else if (strlencmp(cmd, "mute") == 0) {
			new_mute = 0x40;
		} else
			return -EINVAL;

		if (new_level != level) {
			/* mute doesn't change */

			cmos_cmd = (new_level > level) ?
					TP_CMOS_VOLUME_UP : TP_CMOS_VOLUME_DOWN;
			inc = new_level > level ? 1 : -1;

			if (mute && (issue_thinkpad_cmos_command(cmos_cmd) ||
				     !acpi_ec_write(volume_offset, level)))
				return -EIO;

			for (i = level; i != new_level; i += inc)
				if (issue_thinkpad_cmos_command(cmos_cmd) ||
				    !acpi_ec_write(volume_offset, i + inc))
					return -EIO;

			if (mute &&
			    (issue_thinkpad_cmos_command(TP_CMOS_VOLUME_MUTE) ||
			     !acpi_ec_write(volume_offset, new_level + mute))) {
				return -EIO;
			}
		}

		if (new_mute != mute) {
			/* level doesn't change */

			cmos_cmd = (new_mute) ?
				   TP_CMOS_VOLUME_MUTE : TP_CMOS_VOLUME_UP;

			if (issue_thinkpad_cmos_command(cmos_cmd) ||
			    !acpi_ec_write(volume_offset, level + new_mute))
				return -EIO;
		}
	}

	return 0;
}

static struct ibm_struct volume_driver_data = {
	.name = "volume",
	.read = volume_read,
	.write = volume_write,
};

/*************************************************************************
 * Fan subdriver
 */

/*
 * FAN ACCESS MODES
 *
 * TPACPI_FAN_RD_ACPI_GFAN:
 * 	ACPI GFAN method: returns fan level
 *
 * 	see TPACPI_FAN_WR_ACPI_SFAN
 * 	EC 0x2f (HFSP) not available if GFAN exists
 *
 * TPACPI_FAN_WR_ACPI_SFAN:
 * 	ACPI SFAN method: sets fan level, 0 (stop) to 7 (max)
 *
 * 	EC 0x2f (HFSP) might be available *for reading*, but do not use
 * 	it for writing.
 *
 * TPACPI_FAN_WR_TPEC:
 * 	ThinkPad EC register 0x2f (HFSP): fan control loop mode
 * 	Supported on almost all ThinkPads
 *
 * 	Fan speed changes of any sort (including those caused by the
 * 	disengaged mode) are usually done slowly by the firmware as the
 * 	maximum ammount of fan duty cycle change per second seems to be
 * 	limited.
 *
 * 	Reading is not available if GFAN exists.
 * 	Writing is not available if SFAN exists.
 *
 * 	Bits
 *	 7	automatic mode engaged;
 *  		(default operation mode of the ThinkPad)
 * 		fan level is ignored in this mode.
 *	 6	full speed mode (takes precedence over bit 7);
 *		not available on all thinkpads.  May disable
 *		the tachometer while the fan controller ramps up
 *		the speed (which can take up to a few *minutes*).
 *		Speeds up fan to 100% duty-cycle, which is far above
 *		the standard RPM levels.  It is not impossible that
 *		it could cause hardware damage.
 *	5-3	unused in some models.  Extra bits for fan level
 *		in others, but still useless as all values above
 *		7 map to the same speed as level 7 in these models.
 *	2-0	fan level (0..7 usually)
 *			0x00 = stop
 * 			0x07 = max (set when temperatures critical)
 * 		Some ThinkPads may have other levels, see
 * 		TPACPI_FAN_WR_ACPI_FANS (X31/X40/X41)
 *
 *	FIRMWARE BUG: on some models, EC 0x2f might not be initialized at
 *	boot. Apparently the EC does not intialize it, so unless ACPI DSDT
 *	does so, its initial value is meaningless (0x07).
 *
 *	For firmware bugs, refer to:
 *	http://thinkwiki.org/wiki/Embedded_Controller_Firmware#Firmware_Issues
 *
 * 	----
 *
 *	ThinkPad EC register 0x84 (LSB), 0x85 (MSB):
 *	Main fan tachometer reading (in RPM)
 *
 *	This register is present on all ThinkPads with a new-style EC, and
 *	it is known not to be present on the A21m/e, and T22, as there is
 *	something else in offset 0x84 according to the ACPI DSDT.  Other
 *	ThinkPads from this same time period (and earlier) probably lack the
 *	tachometer as well.
 *
 *	Unfortunately a lot of ThinkPads with new-style ECs but whose firwmare
 *	was never fixed by IBM to report the EC firmware version string
 *	probably support the tachometer (like the early X models), so
 *	detecting it is quite hard.  We need more data to know for sure.
 *
 *	FIRMWARE BUG: always read 0x84 first, otherwise incorrect readings
 *	might result.
 *
 *	FIRMWARE BUG: may go stale while the EC is switching to full speed
 *	mode.
 *
 *	For firmware bugs, refer to:
 *	http://thinkwiki.org/wiki/Embedded_Controller_Firmware#Firmware_Issues
 *
 * TPACPI_FAN_WR_ACPI_FANS:
 *	ThinkPad X31, X40, X41.  Not available in the X60.
 *
 *	FANS ACPI handle: takes three arguments: low speed, medium speed,
 *	high speed.  ACPI DSDT seems to map these three speeds to levels
 *	as follows: STOP LOW LOW MED MED HIGH HIGH HIGH HIGH
 *	(this map is stored on FAN0..FAN8 as "0,1,1,2,2,3,3,3,3")
 *
 * 	The speeds are stored on handles
 * 	(FANA:FAN9), (FANC:FANB), (FANE:FAND).
 *
 * 	There are three default speed sets, acessible as handles:
 * 	FS1L,FS1M,FS1H; FS2L,FS2M,FS2H; FS3L,FS3M,FS3H
 *
 * 	ACPI DSDT switches which set is in use depending on various
 * 	factors.
 *
 * 	TPACPI_FAN_WR_TPEC is also available and should be used to
 * 	command the fan.  The X31/X40/X41 seems to have 8 fan levels,
 * 	but the ACPI tables just mention level 7.
 */

enum {					/* Fan control constants */
	fan_status_offset = 0x2f,	/* EC register 0x2f */
	fan_rpm_offset = 0x84,		/* EC register 0x84: LSB, 0x85 MSB (RPM)
					 * 0x84 must be read before 0x85 */

	TP_EC_FAN_FULLSPEED = 0x40,	/* EC fan mode: full speed */
	TP_EC_FAN_AUTO	    = 0x80,	/* EC fan mode: auto fan control */

	TPACPI_FAN_LAST_LEVEL = 0x100,	/* Use cached last-seen fan level */
};

enum fan_status_access_mode {
	TPACPI_FAN_NONE = 0,		/* No fan status or control */
	TPACPI_FAN_RD_ACPI_GFAN,	/* Use ACPI GFAN */
	TPACPI_FAN_RD_TPEC,		/* Use ACPI EC regs 0x2f, 0x84-0x85 */
};

enum fan_control_access_mode {
	TPACPI_FAN_WR_NONE = 0,		/* No fan control */
	TPACPI_FAN_WR_ACPI_SFAN,	/* Use ACPI SFAN */
	TPACPI_FAN_WR_TPEC,		/* Use ACPI EC reg 0x2f */
	TPACPI_FAN_WR_ACPI_FANS,	/* Use ACPI FANS and EC reg 0x2f */
};

enum fan_control_commands {
	TPACPI_FAN_CMD_SPEED 	= 0x0001,	/* speed command */
	TPACPI_FAN_CMD_LEVEL 	= 0x0002,	/* level command  */
	TPACPI_FAN_CMD_ENABLE	= 0x0004,	/* enable/disable cmd,
						 * and also watchdog cmd */
};

static int fan_control_allowed;

static enum fan_status_access_mode fan_status_access_mode;
static enum fan_control_access_mode fan_control_access_mode;
static enum fan_control_commands fan_control_commands;

static u8 fan_control_initial_status;
static u8 fan_control_desired_level;
static int fan_watchdog_maxinterval;

static struct mutex fan_mutex;

static void fan_watchdog_fire(void *ignored);
static DECLARE_WORK(fan_watchdog_task, fan_watchdog_fire, NULL);

TPACPI_HANDLE(fans, ec, "FANS");	/* X31, X40, X41 */
TPACPI_HANDLE(gfan, ec, "GFAN",	/* 570 */
	   "\\FSPD",		/* 600e/x, 770e, 770x */
	   );			/* all others */
TPACPI_HANDLE(sfan, ec, "SFAN",	/* 570 */
	   "JFNS",		/* 770x-JL */
	   );			/* all others */

/*
 * Call with fan_mutex held
 */
static void fan_update_desired_level(u8 status)
{
	if ((status &
	     (TP_EC_FAN_AUTO | TP_EC_FAN_FULLSPEED)) == 0) {
		if (status > 7)
			fan_control_desired_level = 7;
		else
			fan_control_desired_level = status;
	}
}

static int fan_get_status(u8 *status)
{
	u8 s;

	/* TODO:
	 * Add TPACPI_FAN_RD_ACPI_FANS ? */

	switch (fan_status_access_mode) {
	case TPACPI_FAN_RD_ACPI_GFAN:
		/* 570, 600e/x, 770e, 770x */

		if (unlikely(!acpi_evalf(gfan_handle, &s, NULL, "d")))
			return -EIO;

		if (likely(status))
			*status = s & 0x07;

		break;

	case TPACPI_FAN_RD_TPEC:
		/* all except 570, 600e/x, 770e, 770x */
		if (unlikely(!acpi_ec_read(fan_status_offset, &s)))
			return -EIO;

		if (likely(status))
			*status = s;

		break;

	default:
		return -ENXIO;
	}

	return 0;
}

static int fan_get_status_safe(u8 *status)
{
	int rc;
	u8 s;

	if (mutex_lock_interruptible(&fan_mutex))
		return -ERESTARTSYS;
	rc = fan_get_status(&s);
	if (!rc)
		fan_update_desired_level(s);
	mutex_unlock(&fan_mutex);

	if (status)
		*status = s;

	return rc;
}

static int fan_get_speed(unsigned int *speed)
{
	u8 hi, lo;

	switch (fan_status_access_mode) {
	case TPACPI_FAN_RD_TPEC:
		/* all except 570, 600e/x, 770e, 770x */
		if (unlikely(!acpi_ec_read(fan_rpm_offset, &lo) ||
			     !acpi_ec_read(fan_rpm_offset + 1, &hi)))
			return -EIO;

		if (likely(speed))
			*speed = (hi << 8) | lo;

		break;

	default:
		return -ENXIO;
	}

	return 0;
}

static int fan_set_level(int level)
{
	if (!fan_control_allowed)
		return -EPERM;

	switch (fan_control_access_mode) {
	case TPACPI_FAN_WR_ACPI_SFAN:
		if (level >= 0 && level <= 7) {
			if (!acpi_evalf(sfan_handle, NULL, NULL, "vd", level))
				return -EIO;
		} else
			return -EINVAL;
		break;

	case TPACPI_FAN_WR_ACPI_FANS:
	case TPACPI_FAN_WR_TPEC:
		if ((level != TP_EC_FAN_AUTO) &&
		    (level != TP_EC_FAN_FULLSPEED) &&
		    ((level < 0) || (level > 7)))
			return -EINVAL;

		/* safety net should the EC not support AUTO
		 * or FULLSPEED mode bits and just ignore them */
		if (level & TP_EC_FAN_FULLSPEED)
			level |= 7;	/* safety min speed 7 */
		else if (level & TP_EC_FAN_AUTO)
			level |= 4;	/* safety min speed 4 */

		if (!acpi_ec_write(fan_status_offset, level))
			return -EIO;
		else
			tp_features.fan_ctrl_status_undef = 0;
		break;

	default:
		return -ENXIO;
	}
	return 0;
}

static int fan_set_level_safe(int level)
{
	int rc;

	if (!fan_control_allowed)
		return -EPERM;

	if (mutex_lock_interruptible(&fan_mutex))
		return -ERESTARTSYS;

	if (level == TPACPI_FAN_LAST_LEVEL)
		level = fan_control_desired_level;

	rc = fan_set_level(level);
	if (!rc)
		fan_update_desired_level(level);

	mutex_unlock(&fan_mutex);
	return rc;
}

static int fan_set_enable(void)
{
	u8 s;
	int rc;

	if (!fan_control_allowed)
		return -EPERM;

	if (mutex_lock_interruptible(&fan_mutex))
		return -ERESTARTSYS;

	switch (fan_control_access_mode) {
	case TPACPI_FAN_WR_ACPI_FANS:
	case TPACPI_FAN_WR_TPEC:
		rc = fan_get_status(&s);
		if (rc < 0)
			break;

		/* Don't go out of emergency fan mode */
		if (s != 7) {
			s &= 0x07;
			s |= TP_EC_FAN_AUTO | 4; /* min fan speed 4 */
		}

		if (!acpi_ec_write(fan_status_offset, s))
			rc = -EIO;
		else {
			tp_features.fan_ctrl_status_undef = 0;
			rc = 0;
		}
		break;

	case TPACPI_FAN_WR_ACPI_SFAN:
		rc = fan_get_status(&s);
		if (rc < 0)
			break;

		s &= 0x07;

		/* Set fan to at least level 4 */
		s |= 4;

		if (!acpi_evalf(sfan_handle, NULL, NULL, "vd", s))
			rc = -EIO;
		else
			rc = 0;
		break;

	default:
		rc = -ENXIO;
	}

	mutex_unlock(&fan_mutex);
	return rc;
}

static int fan_set_disable(void)
{
	int rc;

	if (!fan_control_allowed)
		return -EPERM;

	if (mutex_lock_interruptible(&fan_mutex))
		return -ERESTARTSYS;

	rc = 0;
	switch (fan_control_access_mode) {
	case TPACPI_FAN_WR_ACPI_FANS:
	case TPACPI_FAN_WR_TPEC:
		if (!acpi_ec_write(fan_status_offset, 0x00))
			rc = -EIO;
		else {
			fan_control_desired_level = 0;
			tp_features.fan_ctrl_status_undef = 0;
		}
		break;

	case TPACPI_FAN_WR_ACPI_SFAN:
		if (!acpi_evalf(sfan_handle, NULL, NULL, "vd", 0x00))
			rc = -EIO;
		else
			fan_control_desired_level = 0;
		break;

	default:
		rc = -ENXIO;
	}


	mutex_unlock(&fan_mutex);
	return rc;
}

static int fan_set_speed(int speed)
{
	int rc;

	if (!fan_control_allowed)
		return -EPERM;

	if (mutex_lock_interruptible(&fan_mutex))
		return -ERESTARTSYS;

	rc = 0;
	switch (fan_control_access_mode) {
	case TPACPI_FAN_WR_ACPI_FANS:
		if (speed >= 0 && speed <= 65535) {
			if (!acpi_evalf(fans_handle, NULL, NULL, "vddd",
					speed, speed, speed))
				rc = -EIO;
		} else
			rc = -EINVAL;
		break;

	default:
		rc = -ENXIO;
	}

	mutex_unlock(&fan_mutex);
	return rc;
}

static void fan_watchdog_reset(void)
{
	static int fan_watchdog_active;

	if (fan_control_access_mode == TPACPI_FAN_WR_NONE)
		return;

	if (fan_watchdog_active)
		cancel_delayed_work(&fan_watchdog_task);

	if (fan_watchdog_maxinterval > 0 &&
	    tpacpi_lifecycle != TPACPI_LIFE_EXITING) {
		fan_watchdog_active = 1;
		if (!schedule_delayed_work(&fan_watchdog_task,
				msecs_to_jiffies(fan_watchdog_maxinterval
						 * 1000))) {
			printk(TPACPI_ERR
			       "failed to schedule the fan watchdog, "
			       "watchdog will not trigger\n");
		}
	} else
		fan_watchdog_active = 0;
}

static void fan_watchdog_fire(void *ignored)
{
	int rc;

	if (tpacpi_lifecycle != TPACPI_LIFE_RUNNING)
		return;

	printk(TPACPI_NOTICE "fan watchdog: enabling fan\n");
	rc = fan_set_enable();
	if (rc < 0) {
		printk(TPACPI_ERR "fan watchdog: error %d while enabling fan, "
			"will try again later...\n", -rc);
		/* reschedule for later */
		fan_watchdog_reset();
	}
}

/*
 * SYSFS fan layout: hwmon compatible (device)
 *
 * pwm*_enable:
 * 	0: "disengaged" mode
 * 	1: manual mode
 * 	2: native EC "auto" mode (recommended, hardware default)
 *
 * pwm*: set speed in manual mode, ignored otherwise.
 * 	0 is level 0; 255 is level 7. Intermediate points done with linear
 * 	interpolation.
 *
 * fan*_input: tachometer reading, RPM
 *
 *
 * SYSFS fan layout: extensions
 *
 * fan_watchdog (driver):
 * 	fan watchdog interval in seconds, 0 disables (default), max 120
 */

/* sysfs fan pwm1_enable ----------------------------------------------- */
static ssize_t fan_pwm1_enable_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	int res, mode;
	u8 status;

	res = fan_get_status_safe(&status);
	if (res)
		return res;

	if (unlikely(tp_features.fan_ctrl_status_undef)) {
		if (status != fan_control_initial_status) {
			tp_features.fan_ctrl_status_undef = 0;
		} else {
			/* Return most likely status. In fact, it
			 * might be the only possible status */
			status = TP_EC_FAN_AUTO;
		}
	}

	if (status & TP_EC_FAN_FULLSPEED) {
		mode = 0;
	} else if (status & TP_EC_FAN_AUTO) {
		mode = 2;
	} else
		mode = 1;

	return snprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t fan_pwm1_enable_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long t;
	int res, level;

	if (parse_strtoul(buf, 2, &t))
		return -EINVAL;

	switch (t) {
	case 0:
		level = TP_EC_FAN_FULLSPEED;
		break;
	case 1:
		level = TPACPI_FAN_LAST_LEVEL;
		break;
	case 2:
		level = TP_EC_FAN_AUTO;
		break;
	case 3:
		/* reserved for software-controlled auto mode */
		return -ENOSYS;
	default:
		return -EINVAL;
	}

	res = fan_set_level_safe(level);
	if (res == -ENXIO)
		return -EINVAL;
	else if (res < 0)
		return res;

	fan_watchdog_reset();

	return count;
}

static struct device_attribute dev_attr_fan_pwm1_enable =
	__ATTR(pwm1_enable, S_IWUSR | S_IRUGO,
		fan_pwm1_enable_show, fan_pwm1_enable_store);

/* sysfs fan pwm1 ------------------------------------------------------ */
static ssize_t fan_pwm1_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	int res;
	u8 status;

	res = fan_get_status_safe(&status);
	if (res)
		return res;

	if (unlikely(tp_features.fan_ctrl_status_undef)) {
		if (status != fan_control_initial_status) {
			tp_features.fan_ctrl_status_undef = 0;
		} else {
			status = TP_EC_FAN_AUTO;
		}
	}

	if ((status &
	     (TP_EC_FAN_AUTO | TP_EC_FAN_FULLSPEED)) != 0)
		status = fan_control_desired_level;

	if (status > 7)
		status = 7;

	return snprintf(buf, PAGE_SIZE, "%u\n", (status * 255) / 7);
}

static ssize_t fan_pwm1_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long s;
	int rc;
	u8 status, newlevel;

	if (parse_strtoul(buf, 255, &s))
		return -EINVAL;

	/* scale down from 0-255 to 0-7 */
	newlevel = (s >> 5) & 0x07;

	if (mutex_lock_interruptible(&fan_mutex))
		return -ERESTARTSYS;

	rc = fan_get_status(&status);
	if (!rc && (status &
		    (TP_EC_FAN_AUTO | TP_EC_FAN_FULLSPEED)) == 0) {
		rc = fan_set_level(newlevel);
		if (rc == -ENXIO)
			rc = -EINVAL;
		else if (!rc) {
			fan_update_desired_level(newlevel);
			fan_watchdog_reset();
		}
	}

	mutex_unlock(&fan_mutex);
	return (rc)? rc : count;
}

static struct device_attribute dev_attr_fan_pwm1 =
	__ATTR(pwm1, S_IWUSR | S_IRUGO,
		fan_pwm1_show, fan_pwm1_store);

/* sysfs fan fan1_input ------------------------------------------------ */
static ssize_t fan_fan1_input_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int res;
	unsigned int speed;

	res = fan_get_speed(&speed);
	if (res < 0)
		return res;

	return snprintf(buf, PAGE_SIZE, "%u\n", speed);
}

static struct device_attribute dev_attr_fan_fan1_input =
	__ATTR(fan1_input, S_IRUGO,
		fan_fan1_input_show, NULL);

/* sysfs fan fan_watchdog (hwmon driver) ------------------------------- */
static ssize_t fan_fan_watchdog_show(struct device_driver *drv,
				     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", fan_watchdog_maxinterval);
}

static ssize_t fan_fan_watchdog_store(struct device_driver *drv,
				      const char *buf, size_t count)
{
	unsigned long t;

	if (parse_strtoul(buf, 120, &t))
		return -EINVAL;

	if (!fan_control_allowed)
		return -EPERM;

	fan_watchdog_maxinterval = t;
	fan_watchdog_reset();

	return count;
}

static DRIVER_ATTR(fan_watchdog, S_IWUSR | S_IRUGO,
		fan_fan_watchdog_show, fan_fan_watchdog_store);

/* --------------------------------------------------------------------- */
static struct attribute *fan_attributes[] = {
	&dev_attr_fan_pwm1_enable.attr, &dev_attr_fan_pwm1.attr,
	&dev_attr_fan_fan1_input.attr,
	NULL
};

static const struct attribute_group fan_attr_group = {
	.attrs = fan_attributes,
};

static int __init fan_init(struct ibm_init_struct *iibm)
{
	int rc;

	vdbg_printk(TPACPI_DBG_INIT, "initializing fan subdriver\n");

	mutex_init(&fan_mutex);
	fan_status_access_mode = TPACPI_FAN_NONE;
	fan_control_access_mode = TPACPI_FAN_WR_NONE;
	fan_control_commands = 0;
	fan_watchdog_maxinterval = 0;
	tp_features.fan_ctrl_status_undef = 0;
	fan_control_desired_level = 7;

	TPACPI_ACPIHANDLE_INIT(fans);
	TPACPI_ACPIHANDLE_INIT(gfan);
	TPACPI_ACPIHANDLE_INIT(sfan);

	if (gfan_handle) {
		/* 570, 600e/x, 770e, 770x */
		fan_status_access_mode = TPACPI_FAN_RD_ACPI_GFAN;
	} else {
		/* all other ThinkPads: note that even old-style
		 * ThinkPad ECs supports the fan control register */
		if (likely(acpi_ec_read(fan_status_offset,
					&fan_control_initial_status))) {
			fan_status_access_mode = TPACPI_FAN_RD_TPEC;

			/* In some ThinkPads, neither the EC nor the ACPI
			 * DSDT initialize the fan status, and it ends up
			 * being set to 0x07 when it *could* be either
			 * 0x07 or 0x80.
			 *
			 * Enable for TP-1Y (T43), TP-78 (R51e),
			 * TP-76 (R52), TP-70 (T43, R52), which are known
			 * to be buggy. */
			if (fan_control_initial_status == 0x07) {
				switch (thinkpad_id.ec_model) {
				case 0x5931: /* TP-1Y */
				case 0x3837: /* TP-78 */
				case 0x3637: /* TP-76 */
				case 0x3037: /* TP-70 */
					printk(TPACPI_NOTICE
					       "fan_init: initial fan status "
					       "is unknown, assuming it is "
					       "in auto mode\n");
					tp_features.fan_ctrl_status_undef = 1;
					;;
				}
			}
		} else {
			printk(TPACPI_ERR
			       "ThinkPad ACPI EC access misbehaving, "
			       "fan status and control unavailable\n");
			return 1;
		}
	}

	if (sfan_handle) {
		/* 570, 770x-JL */
		fan_control_access_mode = TPACPI_FAN_WR_ACPI_SFAN;
		fan_control_commands |=
		    TPACPI_FAN_CMD_LEVEL | TPACPI_FAN_CMD_ENABLE;
	} else {
		if (!gfan_handle) {
			/* gfan without sfan means no fan control */
			/* all other models implement TP EC 0x2f control */

			if (fans_handle) {
				/* X31, X40, X41 */
				fan_control_access_mode =
				    TPACPI_FAN_WR_ACPI_FANS;
				fan_control_commands |=
				    TPACPI_FAN_CMD_SPEED |
				    TPACPI_FAN_CMD_LEVEL |
				    TPACPI_FAN_CMD_ENABLE;
			} else {
				fan_control_access_mode = TPACPI_FAN_WR_TPEC;
				fan_control_commands |=
				    TPACPI_FAN_CMD_LEVEL |
				    TPACPI_FAN_CMD_ENABLE;
			}
		}
	}

	vdbg_printk(TPACPI_DBG_INIT, "fan is %s, modes %d, %d\n",
		str_supported(fan_status_access_mode != TPACPI_FAN_NONE ||
		  fan_control_access_mode != TPACPI_FAN_WR_NONE),
		fan_status_access_mode, fan_control_access_mode);

	/* fan control master switch */
	if (!fan_control_allowed) {
		fan_control_access_mode = TPACPI_FAN_WR_NONE;
		fan_control_commands = 0;
		dbg_printk(TPACPI_DBG_INIT,
			   "fan control features disabled by parameter\n");
	}

	/* update fan_control_desired_level */
	if (fan_status_access_mode != TPACPI_FAN_NONE)
		fan_get_status_safe(NULL);

	if (fan_status_access_mode != TPACPI_FAN_NONE ||
	    fan_control_access_mode != TPACPI_FAN_WR_NONE) {
		rc = sysfs_create_group(&tpacpi_sensors_pdev->dev.kobj,
					 &fan_attr_group);
		if (!(rc < 0))
			rc = driver_create_file(&tpacpi_hwmon_pdriver.driver,
					&driver_attr_fan_watchdog);
		if (rc < 0)
			return rc;
		return 0;
	} else
		return 1;
}

static void fan_exit(void)
{
	vdbg_printk(TPACPI_DBG_EXIT,
		    "cancelling any pending fan watchdog tasks\n");

	/* FIXME: can we really do this unconditionally? */
	sysfs_remove_group(&tpacpi_sensors_pdev->dev.kobj, &fan_attr_group);
	driver_remove_file(&tpacpi_hwmon_pdriver.driver,
			   &driver_attr_fan_watchdog);

	cancel_delayed_work(&fan_watchdog_task);
	flush_scheduled_work();
}

static int fan_read(char *p)
{
	int len = 0;
	int rc;
	u8 status;
	unsigned int speed = 0;

	switch (fan_status_access_mode) {
	case TPACPI_FAN_RD_ACPI_GFAN:
		/* 570, 600e/x, 770e, 770x */
		rc = fan_get_status_safe(&status);
		if (rc < 0)
			return rc;

		len += sprintf(p + len, "status:\t\t%s\n"
			       "level:\t\t%d\n",
			       (status != 0) ? "enabled" : "disabled", status);
		break;

	case TPACPI_FAN_RD_TPEC:
		/* all except 570, 600e/x, 770e, 770x */
		rc = fan_get_status_safe(&status);
		if (rc < 0)
			return rc;

		if (unlikely(tp_features.fan_ctrl_status_undef)) {
			if (status != fan_control_initial_status)
				tp_features.fan_ctrl_status_undef = 0;
			else
				/* Return most likely status. In fact, it
				 * might be the only possible status */
				status = TP_EC_FAN_AUTO;
		}

		len += sprintf(p + len, "status:\t\t%s\n",
			       (status != 0) ? "enabled" : "disabled");

		rc = fan_get_speed(&speed);
		if (rc < 0)
			return rc;

		len += sprintf(p + len, "speed:\t\t%d\n", speed);

		if (status & TP_EC_FAN_FULLSPEED)
			/* Disengaged mode takes precedence */
			len += sprintf(p + len, "level:\t\tdisengaged\n");
		else if (status & TP_EC_FAN_AUTO)
			len += sprintf(p + len, "level:\t\tauto\n");
		else
			len += sprintf(p + len, "level:\t\t%d\n", status);
		break;

	case TPACPI_FAN_NONE:
	default:
		len += sprintf(p + len, "status:\t\tnot supported\n");
	}

	if (fan_control_commands & TPACPI_FAN_CMD_LEVEL) {
		len += sprintf(p + len, "commands:\tlevel <level>");

		switch (fan_control_access_mode) {
		case TPACPI_FAN_WR_ACPI_SFAN:
			len += sprintf(p + len, " (<level> is 0-7)\n");
			break;

		default:
			len += sprintf(p + len, " (<level> is 0-7, "
				       "auto, disengaged, full-speed)\n");
			break;
		}
	}

	if (fan_control_commands & TPACPI_FAN_CMD_ENABLE)
		len += sprintf(p + len, "commands:\tenable, disable\n"
			       "commands:\twatchdog <timeout> (<timeout> "
			       "is 0 (off), 1-120 (seconds))\n");

	if (fan_control_commands & TPACPI_FAN_CMD_SPEED)
		len += sprintf(p + len, "commands:\tspeed <speed>"
			       " (<speed> is 0-65535)\n");

	return len;
}

static int fan_write_cmd_level(const char *cmd, int *rc)
{
	int level;

	if (strlencmp(cmd, "level auto") == 0)
		level = TP_EC_FAN_AUTO;
	else if ((strlencmp(cmd, "level disengaged") == 0) |
			(strlencmp(cmd, "level full-speed") == 0))
		level = TP_EC_FAN_FULLSPEED;
	else if (sscanf(cmd, "level %d", &level) != 1)
		return 0;

	*rc = fan_set_level_safe(level);
	if (*rc == -ENXIO)
		printk(TPACPI_ERR "level command accepted for unsupported "
		       "access mode %d", fan_control_access_mode);

	return 1;
}

static int fan_write_cmd_enable(const char *cmd, int *rc)
{
	if (strlencmp(cmd, "enable") != 0)
		return 0;

	*rc = fan_set_enable();
	if (*rc == -ENXIO)
		printk(TPACPI_ERR "enable command accepted for unsupported "
		       "access mode %d", fan_control_access_mode);

	return 1;
}

static int fan_write_cmd_disable(const char *cmd, int *rc)
{
	if (strlencmp(cmd, "disable") != 0)
		return 0;

	*rc = fan_set_disable();
	if (*rc == -ENXIO)
		printk(TPACPI_ERR "disable command accepted for unsupported "
		       "access mode %d", fan_control_access_mode);

	return 1;
}

static int fan_write_cmd_speed(const char *cmd, int *rc)
{
	int speed;

	/* TODO:
	 * Support speed <low> <medium> <high> ? */

	if (sscanf(cmd, "speed %d", &speed) != 1)
		return 0;

	*rc = fan_set_speed(speed);
	if (*rc == -ENXIO)
		printk(TPACPI_ERR "speed command accepted for unsupported "
		       "access mode %d", fan_control_access_mode);

	return 1;
}

static int fan_write_cmd_watchdog(const char *cmd, int *rc)
{
	int interval;

	if (sscanf(cmd, "watchdog %d", &interval) != 1)
		return 0;

	if (interval < 0 || interval > 120)
		*rc = -EINVAL;
	else
		fan_watchdog_maxinterval = interval;

	return 1;
}

static int fan_write(char *buf)
{
	char *cmd;
	int rc = 0;

	while (!rc && (cmd = next_cmd(&buf))) {
		if (!((fan_control_commands & TPACPI_FAN_CMD_LEVEL) &&
		      fan_write_cmd_level(cmd, &rc)) &&
		    !((fan_control_commands & TPACPI_FAN_CMD_ENABLE) &&
		      (fan_write_cmd_enable(cmd, &rc) ||
		       fan_write_cmd_disable(cmd, &rc) ||
		       fan_write_cmd_watchdog(cmd, &rc))) &&
		    !((fan_control_commands & TPACPI_FAN_CMD_SPEED) &&
		      fan_write_cmd_speed(cmd, &rc))
		    )
			rc = -EINVAL;
		else if (!rc)
			fan_watchdog_reset();
	}

	return rc;
}

static struct ibm_struct fan_driver_data = {
	.name = "fan",
	.read = fan_read,
	.write = fan_write,
	.exit = fan_exit,
};

/****************************************************************************
 ****************************************************************************
 *
 * Infrastructure
 *
 ****************************************************************************
 ****************************************************************************/

/* sysfs name ---------------------------------------------------------- */
static ssize_t thinkpad_acpi_pdev_name_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", TPACPI_NAME);
}

static struct device_attribute dev_attr_thinkpad_acpi_pdev_name =
	__ATTR(name, S_IRUGO, thinkpad_acpi_pdev_name_show, NULL);

/* --------------------------------------------------------------------- */

/* /proc support */
static struct proc_dir_entry *proc_dir;

/*
 * Module and infrastructure proble, init and exit handling
 */

static int force_load;

#ifdef CONFIG_THINKPAD_ACPI_DEBUG
static const char * __init str_supported(int is_supported)
{
	static char text_unsupported[] __initdata = "not supported";

	return (is_supported)? &text_unsupported[4] : &text_unsupported[0];
}
#endif /* CONFIG_THINKPAD_ACPI_DEBUG */

static void ibm_exit(struct ibm_struct *ibm)
{
	dbg_printk(TPACPI_DBG_EXIT, "removing %s\n", ibm->name);

	list_del_init(&ibm->all_drivers);

	if (ibm->flags.acpi_notify_installed) {
		dbg_printk(TPACPI_DBG_EXIT,
			"%s: acpi_remove_notify_handler\n", ibm->name);
		BUG_ON(!ibm->acpi);
		acpi_remove_notify_handler(*ibm->acpi->handle,
					   ibm->acpi->type,
					   dispatch_acpi_notify);
		ibm->flags.acpi_notify_installed = 0;
		ibm->flags.acpi_notify_installed = 0;
	}

	if (ibm->flags.proc_created) {
		dbg_printk(TPACPI_DBG_EXIT,
			"%s: remove_proc_entry\n", ibm->name);
		remove_proc_entry(ibm->name, proc_dir);
		ibm->flags.proc_created = 0;
	}

	if (ibm->flags.acpi_driver_registered) {
		dbg_printk(TPACPI_DBG_EXIT,
			"%s: acpi_bus_unregister_driver\n", ibm->name);
		BUG_ON(!ibm->acpi);
		acpi_bus_unregister_driver(ibm->acpi->driver);
		kfree(ibm->acpi->driver);
		ibm->acpi->driver = NULL;
		ibm->flags.acpi_driver_registered = 0;
	}

	if (ibm->flags.init_called && ibm->exit) {
		ibm->exit();
		ibm->flags.init_called = 0;
	}

	dbg_printk(TPACPI_DBG_INIT, "finished removing %s\n", ibm->name);
}

static int __init ibm_init(struct ibm_init_struct *iibm)
{
	int ret;
	struct ibm_struct *ibm = iibm->data;
	struct proc_dir_entry *entry;

	BUG_ON(ibm == NULL);

	INIT_LIST_HEAD(&ibm->all_drivers);

	if (ibm->flags.experimental && !experimental)
		return 0;

	dbg_printk(TPACPI_DBG_INIT,
		"probing for %s\n", ibm->name);

	if (iibm->init) {
		ret = iibm->init(iibm);
		if (ret > 0)
			return 0;	/* probe failed */
		if (ret)
			return ret;

		ibm->flags.init_called = 1;
	}

	if (ibm->acpi) {
		if (ibm->acpi->hid) {
			ret = register_tpacpi_subdriver(ibm);
			if (ret)
				goto err_out;
		}

		if (ibm->acpi->notify) {
			ret = setup_acpi_notify(ibm);
			if (ret == -ENODEV) {
				printk(TPACPI_NOTICE "disabling subdriver %s\n",
					ibm->name);
				ret = 0;
				goto err_out;
			}
			if (ret < 0)
				goto err_out;
		}
	}

	dbg_printk(TPACPI_DBG_INIT,
		"%s installed\n", ibm->name);

	if (ibm->read) {
		entry = create_proc_entry(ibm->name,
					  S_IFREG | S_IRUGO | S_IWUSR,
					  proc_dir);
		if (!entry) {
			printk(TPACPI_ERR "unable to create proc entry %s\n",
			       ibm->name);
			ret = -ENODEV;
			goto err_out;
		}
		entry->owner = THIS_MODULE;
		entry->data = ibm;
		entry->read_proc = &dispatch_procfs_read;
		if (ibm->write)
			entry->write_proc = &dispatch_procfs_write;
		ibm->flags.proc_created = 1;
	}

	list_add_tail(&ibm->all_drivers, &tpacpi_all_drivers);

	return 0;

err_out:
	dbg_printk(TPACPI_DBG_INIT,
		"%s: at error exit path with result %d\n",
		ibm->name, ret);

	ibm_exit(ibm);
	return (ret < 0)? ret : 0;
}

/* Probing */

static void __init get_thinkpad_model_data(struct thinkpad_id_data *tp)
{
	struct dmi_device *dev = NULL;
	char ec_fw_string[18];

	if (!tp)
		return;

	memset(tp, 0, sizeof(*tp));

	if (dmi_name_in_vendors("IBM"))
		tp->vendor = PCI_VENDOR_ID_IBM;
	else if (dmi_name_in_vendors("LENOVO"))
		tp->vendor = PCI_VENDOR_ID_LENOVO;
	else
		return;

	tp->bios_version_str = kstrdup(dmi_get_system_info(DMI_BIOS_VERSION),
					GFP_KERNEL);
	if (!tp->bios_version_str)
		return;
	tp->bios_model = tp->bios_version_str[0]
			 | (tp->bios_version_str[1] << 8);

	/*
	 * ThinkPad T23 or newer, A31 or newer, R50e or newer,
	 * X32 or newer, all Z series;  Some models must have an
	 * up-to-date BIOS or they will not be detected.
	 *
	 * See http://thinkwiki.org/wiki/List_of_DMI_IDs
	 */
	while ((dev = dmi_find_device(DMI_DEV_TYPE_OEM_STRING, NULL, dev))) {
		if (sscanf(dev->name,
			   "IBM ThinkPad Embedded Controller -[%17c",
			   ec_fw_string) == 1) {
			ec_fw_string[sizeof(ec_fw_string) - 1] = 0;
			ec_fw_string[strcspn(ec_fw_string, " ]")] = 0;

			tp->ec_version_str = kstrdup(ec_fw_string, GFP_KERNEL);
			tp->ec_model = ec_fw_string[0]
					| (ec_fw_string[1] << 8);
			break;
		}
	}

	tp->model_str = kstrdup(dmi_get_system_info(DMI_PRODUCT_VERSION),
					GFP_KERNEL);
	if (strnicmp(tp->model_str, "ThinkPad", 8) != 0) {
		kfree(tp->model_str);
		tp->model_str = NULL;
	}
}

static int __init probe_for_thinkpad(void)
{
	int is_thinkpad;

	if (acpi_disabled)
		return -ENODEV;

	/*
	 * Non-ancient models have better DMI tagging, but very old models
	 * don't.
	 */
	is_thinkpad = (thinkpad_id.model_str != NULL);

	/* ec is required because many other handles are relative to it */
	TPACPI_ACPIHANDLE_INIT(ec);
	if (!ec_handle) {
		if (is_thinkpad)
			printk(TPACPI_ERR
				"Not yet supported ThinkPad detected!\n");
		return -ENODEV;
	}

	/*
	 * Risks a regression on very old machines, but reduces potential
	 * false positives a damn great deal
	 */
	if (!is_thinkpad)
		is_thinkpad = (thinkpad_id.vendor == PCI_VENDOR_ID_IBM);

	if (!is_thinkpad && !force_load)
		return -ENODEV;

	return 0;
}


/* Module init, exit, parameters */

static struct ibm_init_struct ibms_init[] __initdata = {
	{
		.init = thinkpad_acpi_driver_init,
		.data = &thinkpad_acpi_driver_data,
	},
	{
		.init = hotkey_init,
		.data = &hotkey_driver_data,
	},
	{
		.init = bluetooth_init,
		.data = &bluetooth_driver_data,
	},
	{
		.init = wan_init,
		.data = &wan_driver_data,
	},
#ifdef CONFIG_THINKPAD_ACPI_VIDEO
	{
		.init = video_init,
		.data = &video_driver_data,
	},
#endif
	{
		.init = light_init,
		.data = &light_driver_data,
	},
#ifdef CONFIG_THINKPAD_ACPI_DOCK
	{
		.init = dock_init,
		.data = &dock_driver_data[0],
	},
	{
		.init = dock_init2,
		.data = &dock_driver_data[1],
	},
#endif
#ifdef CONFIG_THINKPAD_ACPI_BAY
	{
		.init = bay_init,
		.data = &bay_driver_data,
	},
#endif
	{
		.init = cmos_init,
		.data = &cmos_driver_data,
	},
	{
		.init = led_init,
		.data = &led_driver_data,
	},
	{
		.init = beep_init,
		.data = &beep_driver_data,
	},
	{
		.init = thermal_init,
		.data = &thermal_driver_data,
	},
	{
		.data = &ecdump_driver_data,
	},
	{
		.init = brightness_init,
		.data = &brightness_driver_data,
	},
	{
		.data = &volume_driver_data,
	},
	{
		.init = fan_init,
		.data = &fan_driver_data,
	},
};

static int __init set_ibm_param(const char *val, struct kernel_param *kp)
{
	unsigned int i;
	struct ibm_struct *ibm;

	if (!kp || !kp->name || !val)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ibms_init); i++) {
		ibm = ibms_init[i].data;
		WARN_ON(ibm == NULL);

		if (!ibm || !ibm->name)
			continue;

		if (strcmp(ibm->name, kp->name) == 0 && ibm->write) {
			if (strlen(val) > sizeof(ibms_init[i].param) - 2)
				return -ENOSPC;
			strcpy(ibms_init[i].param, val);
			strcat(ibms_init[i].param, ",");
			return 0;
		}
	}

	return -EINVAL;
}

module_param(experimental, int, 0);
MODULE_PARM_DESC(experimental,
		 "Enables experimental features when non-zero");

module_param_named(debug, dbg_level, uint, 0);
MODULE_PARM_DESC(debug, "Sets debug level bit-mask");

module_param(force_load, bool, 0);
MODULE_PARM_DESC(force_load,
		 "Attempts to load the driver even on a "
		 "mis-identified ThinkPad when true");

module_param_named(fan_control, fan_control_allowed, bool, 0);
MODULE_PARM_DESC(fan_control,
		 "Enables setting fan parameters features when true");

module_param_named(brightness_mode, brightness_mode, int, 0);
MODULE_PARM_DESC(brightness_mode,
		 "Selects brightness control strategy: "
		 "0=auto, 1=EC, 2=CMOS, 3=both");

module_param(brightness_enable, uint, 0);
MODULE_PARM_DESC(brightness_enable,
		 "Enables backlight control when 1, disables when 0");

module_param(hotkey_report_mode, uint, 0);
MODULE_PARM_DESC(hotkey_report_mode,
		 "used for backwards compatibility with userspace, "
		 "see documentation");

#define TPACPI_PARAM(feature) \
	module_param_call(feature, set_ibm_param, NULL, NULL, 0); \
	MODULE_PARM_DESC(feature, "Simulates thinkpad-acpi procfs command " \
			 "at module load, see documentation")

TPACPI_PARAM(hotkey);
TPACPI_PARAM(bluetooth);
TPACPI_PARAM(video);
TPACPI_PARAM(light);
#ifdef CONFIG_THINKPAD_ACPI_DOCK
TPACPI_PARAM(dock);
#endif
#ifdef CONFIG_THINKPAD_ACPI_BAY
TPACPI_PARAM(bay);
#endif /* CONFIG_THINKPAD_ACPI_BAY */
TPACPI_PARAM(cmos);
TPACPI_PARAM(led);
TPACPI_PARAM(beep);
TPACPI_PARAM(ecdump);
TPACPI_PARAM(brightness);
TPACPI_PARAM(volume);
TPACPI_PARAM(fan);

static void thinkpad_acpi_module_exit(void)
{
	struct ibm_struct *ibm, *itmp;

	tpacpi_lifecycle = TPACPI_LIFE_EXITING;

	list_for_each_entry_safe_reverse(ibm, itmp,
					 &tpacpi_all_drivers,
					 all_drivers) {
		ibm_exit(ibm);
	}

	dbg_printk(TPACPI_DBG_INIT, "finished subdriver exit path...\n");

	if (tpacpi_inputdev) {
		if (tp_features.input_device_registered)
			input_unregister_device(tpacpi_inputdev);
		else
			input_free_device(tpacpi_inputdev);
	}

	if (tpacpi_hwmon)
		hwmon_device_unregister(tpacpi_hwmon);

	if (tp_features.sensors_pdev_attrs_registered)
		device_remove_file(&tpacpi_sensors_pdev->dev,
				   &dev_attr_thinkpad_acpi_pdev_name);
	if (tpacpi_sensors_pdev)
		platform_device_unregister(tpacpi_sensors_pdev);
	if (tpacpi_pdev)
		platform_device_unregister(tpacpi_pdev);

	if (tp_features.sensors_pdrv_attrs_registered)
		tpacpi_remove_driver_attributes(&tpacpi_hwmon_pdriver.driver);
	if (tp_features.platform_drv_attrs_registered)
		tpacpi_remove_driver_attributes(&tpacpi_pdriver.driver);

	if (tp_features.sensors_pdrv_registered)
		platform_driver_unregister(&tpacpi_hwmon_pdriver);

	if (tp_features.platform_drv_registered)
		platform_driver_unregister(&tpacpi_pdriver);

	if (proc_dir)
		remove_proc_entry(TPACPI_PROC_DIR, acpi_root_dir);

	kfree(thinkpad_id.bios_version_str);
	kfree(thinkpad_id.ec_version_str);
	kfree(thinkpad_id.model_str);
}


static int __init thinkpad_acpi_module_init(void)
{
	int ret, i;

	tpacpi_lifecycle = TPACPI_LIFE_INIT;

	/* Parameter checking */
	if (hotkey_report_mode > 2)
		return -EINVAL;

	/* Driver-level probe */

	get_thinkpad_model_data(&thinkpad_id);
	ret = probe_for_thinkpad();
	if (ret) {
		thinkpad_acpi_module_exit();
		return ret;
	}

	/* Driver initialization */

	TPACPI_ACPIHANDLE_INIT(ecrd);
	TPACPI_ACPIHANDLE_INIT(ecwr);

	proc_dir = proc_mkdir(TPACPI_PROC_DIR, acpi_root_dir);
	if (!proc_dir) {
		printk(TPACPI_ERR
		       "unable to create proc dir " TPACPI_PROC_DIR);
		thinkpad_acpi_module_exit();
		return -ENODEV;
	}
	proc_dir->owner = THIS_MODULE;

	ret = platform_driver_register(&tpacpi_pdriver);
	if (ret) {
		printk(TPACPI_ERR
		       "unable to register main platform driver\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	tp_features.platform_drv_registered = 1;

	ret = platform_driver_register(&tpacpi_hwmon_pdriver);
	if (ret) {
		printk(TPACPI_ERR
		       "unable to register hwmon platform driver\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	tp_features.sensors_pdrv_registered = 1;

	ret = tpacpi_create_driver_attributes(&tpacpi_pdriver.driver);
	if (!ret) {
		tp_features.platform_drv_attrs_registered = 1;
		ret = tpacpi_create_driver_attributes(
					&tpacpi_hwmon_pdriver.driver);
	}
	if (ret) {
		printk(TPACPI_ERR
		       "unable to create sysfs driver attributes\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	tp_features.sensors_pdrv_attrs_registered = 1;


	/* Device initialization */
	tpacpi_pdev = platform_device_register_simple(TPACPI_DRVR_NAME, -1,
							NULL, 0);
	if (IS_ERR(tpacpi_pdev)) {
		ret = PTR_ERR(tpacpi_pdev);
		tpacpi_pdev = NULL;
		printk(TPACPI_ERR "unable to register platform device\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	tpacpi_sensors_pdev = platform_device_register_simple(
						TPACPI_HWMON_DRVR_NAME,
						-1, NULL, 0);
	if (IS_ERR(tpacpi_sensors_pdev)) {
		ret = PTR_ERR(tpacpi_sensors_pdev);
		tpacpi_sensors_pdev = NULL;
		printk(TPACPI_ERR
		       "unable to register hwmon platform device\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	ret = device_create_file(&tpacpi_sensors_pdev->dev,
				 &dev_attr_thinkpad_acpi_pdev_name);
	if (ret) {
		printk(TPACPI_ERR
		       "unable to create sysfs hwmon device attributes\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	tp_features.sensors_pdev_attrs_registered = 1;
	tpacpi_hwmon = hwmon_device_register(&tpacpi_sensors_pdev->dev);
	if (IS_ERR(tpacpi_hwmon)) {
		ret = PTR_ERR(tpacpi_hwmon);
		tpacpi_hwmon = NULL;
		printk(TPACPI_ERR "unable to register hwmon device\n");
		thinkpad_acpi_module_exit();
		return ret;
	}
	mutex_init(&tpacpi_inputdev_send_mutex);
	tpacpi_inputdev = input_allocate_device();
	if (!tpacpi_inputdev) {
		printk(TPACPI_ERR "unable to allocate input device\n");
		thinkpad_acpi_module_exit();
		return -ENOMEM;
	} else {
		/* Prepare input device, but don't register */
		tpacpi_inputdev->name = "ThinkPad Extra Buttons";
		tpacpi_inputdev->phys = TPACPI_DRVR_NAME "/input0";
		tpacpi_inputdev->id.bustype = BUS_HOST;
		tpacpi_inputdev->id.vendor = (thinkpad_id.vendor) ?
						thinkpad_id.vendor :
						PCI_VENDOR_ID_IBM;
		tpacpi_inputdev->id.product = TPACPI_HKEY_INPUT_PRODUCT;
		tpacpi_inputdev->id.version = TPACPI_HKEY_INPUT_VERSION;
	}
	for (i = 0; i < ARRAY_SIZE(ibms_init); i++) {
		ret = ibm_init(&ibms_init[i]);
		if (ret >= 0 && *ibms_init[i].param)
			ret = ibms_init[i].data->write(ibms_init[i].param);
		if (ret < 0) {
			thinkpad_acpi_module_exit();
			return ret;
		}
	}
	ret = input_register_device(tpacpi_inputdev);
	if (ret < 0) {
		printk(TPACPI_ERR "unable to register input device\n");
		thinkpad_acpi_module_exit();
		return ret;
	} else {
		tp_features.input_device_registered = 1;
	}

	tpacpi_lifecycle = TPACPI_LIFE_RUNNING;
	return 0;
}

/* Please remove this in year 2009 */
MODULE_ALIAS("ibm_acpi");

/*
 * DMI matching for module autoloading
 *
 * See http://thinkwiki.org/wiki/List_of_DMI_IDs
 * See http://thinkwiki.org/wiki/BIOS_Upgrade_Downloads
 *
 * Only models listed in thinkwiki will be supported, so add yours
 * if it is not there yet.
 */
#define IBM_BIOS_MODULE_ALIAS(__type) \
	MODULE_ALIAS("dmi:bvnIBM:bvr" __type "ET??WW")

/* Non-ancient thinkpads */
MODULE_ALIAS("dmi:bvnIBM:*:svnIBM:*:pvrThinkPad*:rvnIBM:*");
MODULE_ALIAS("dmi:bvnLENOVO:*:svnLENOVO:*:pvrThinkPad*:rvnLENOVO:*");

/* Ancient thinkpad BIOSes have to be identified by
 * BIOS type or model number, and there are far less
 * BIOS types than model numbers... */
IBM_BIOS_MODULE_ALIAS("I[B,D,H,I,M,N,O,T,W,V,Y,Z]");
IBM_BIOS_MODULE_ALIAS("1[0,3,6,8,A-G,I,K,M-P,S,T]");
IBM_BIOS_MODULE_ALIAS("K[U,X-Z]");

MODULE_AUTHOR("Borislav Deianov, Henrique de Moraes Holschuh");
MODULE_DESCRIPTION(TPACPI_DESC);
MODULE_VERSION(TPACPI_VERSION);
MODULE_LICENSE("GPL");

module_init(thinkpad_acpi_module_init);
module_exit(thinkpad_acpi_module_exit);
