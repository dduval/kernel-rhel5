/*
    i2c-i801.c - Part of lm_sensors, Linux kernel modules for hardware
              monitoring
    Copyright (c) 1998 - 2002  Frodo Looijaard <frodol@dds.nl>,
    Philip Edelbrock <phil@netroedge.com>, and Mark D. Studebaker
    <mdsxyz123@yahoo.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
    SUPPORTED DEVICES	PCI ID
    82801AA		2413           
    82801AB		2423           
    82801BA		2443           
    82801CA/CAM		2483           
    82801DB		24C3   (HW PEC supported, 32 byte buffer not supported)
    82801EB		24D3   (HW PEC supported, 32 byte buffer not supported)
    6300ESB		25A4
    ICH6		266A
    ICH7		27DA
    ESB2		269B
    ICH8		283E
    ICH9		2930
    Tolapai		5032
    ICH10		3A30
    ICH10		3A60
    PCH                 3B30

    This driver supports several versions of Intel's I/O Controller Hubs (ICH).
    For SMBus support, they are similar to the PIIX4 and are part
    of Intel's '810' and other chipsets.
    See the file Documentation/i2c/busses/i2c-i801 for details.
    I2C Block Read and Process Call are not supported.
*/

/* Note: we assume there can only be one I801, with one SMBus interface */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <asm/io.h>

/* I801 SMBus address offsets */
#define SMBHSTSTS	(0 + i801_smba)
#define SMBHSTCNT	(2 + i801_smba)
#define SMBHSTCMD	(3 + i801_smba)
#define SMBHSTADD	(4 + i801_smba)
#define SMBHSTDAT0	(5 + i801_smba)
#define SMBHSTDAT1	(6 + i801_smba)
#define SMBBLKDAT	(7 + i801_smba)
#define SMBPEC		(8 + i801_smba)	/* ICH4 only */
#define SMBAUXSTS	(12 + i801_smba)	/* ICH4 only */
#define SMBAUXCTL	(13 + i801_smba)	/* ICH4 only */

/* PCI Address Constants */
#define SMBBAR		4
#define SMBHSTCFG	0x040

/* Host configuration bits for SMBHSTCFG */
#define SMBHSTCFG_HST_EN	1
#define SMBHSTCFG_SMB_SMI_EN	2
#define SMBHSTCFG_I2C_EN	4

/* Other settings */
#define MAX_TIMEOUT		100
#define ENABLE_INT9		0	/* set to 0x01 to enable - untested */

/* I801 command constants */
#define I801_QUICK		0x00
#define I801_BYTE		0x04
#define I801_BYTE_DATA		0x08
#define I801_WORD_DATA		0x0C
#define I801_PROC_CALL		0x10	/* later chips only, unimplemented */
#define I801_BLOCK_DATA		0x14
#define I801_I2C_BLOCK_DATA	0x18	/* unimplemented */
#define I801_BLOCK_LAST		0x34
#define I801_I2C_BLOCK_LAST	0x38	/* unimplemented */
#define I801_START		0x40
#define I801_PEC_EN		0x80	/* ICH4 only */


static int i801_transaction(void);
static int i801_block_transaction(union i2c_smbus_data *data, char read_write,
				  int command, int hwpec);

static unsigned long i801_smba;
static struct pci_driver i801_driver;
static struct pci_dev *I801_dev;
static int isich4;

static int i801_transaction(void)
{
	int temp;
	int result = 0;
	int timeout = 0;

	dev_dbg(&I801_dev->dev, "Transaction (pre): CNT=%02x, CMD=%02x, "
		"ADD=%02x, DAT0=%02x, DAT1=%02x\n", inb_p(SMBHSTCNT),
		inb_p(SMBHSTCMD), inb_p(SMBHSTADD), inb_p(SMBHSTDAT0),
		inb_p(SMBHSTDAT1));

	/* Make sure the SMBus host is ready to start transmitting */
	/* 0x1f = Failed, Bus_Err, Dev_Err, Intr, Host_Busy */
	if ((temp = (0x1f & inb_p(SMBHSTSTS))) != 0x00) {
		dev_dbg(&I801_dev->dev, "SMBus busy (%02x). Resetting...\n",
			temp);
		outb_p(temp, SMBHSTSTS);
		if ((temp = (0x1f & inb_p(SMBHSTSTS))) != 0x00) {
			dev_dbg(&I801_dev->dev, "Failed! (%02x)\n", temp);
			return -1;
		} else {
			dev_dbg(&I801_dev->dev, "Successfull!\n");
		}
	}

	outb_p(inb(SMBHSTCNT) | I801_START, SMBHSTCNT);

	/* We will always wait for a fraction of a second! */
	do {
		msleep(1);
		temp = inb_p(SMBHSTSTS);
	} while ((temp & 0x01) && (timeout++ < MAX_TIMEOUT));

	/* If the SMBus is still busy, we give up */
	if (timeout >= MAX_TIMEOUT) {
		dev_dbg(&I801_dev->dev, "SMBus Timeout!\n");
		result = -1;
	}

	if (temp & 0x10) {
		result = -1;
		dev_dbg(&I801_dev->dev, "Error: Failed bus transaction\n");
	}

	if (temp & 0x08) {
		result = -1;
		dev_err(&I801_dev->dev, "Bus collision! SMBus may be locked "
			"until next hard reset. (sorry!)\n");
		/* Clock stops and slave is stuck in mid-transmission */
	}

	if (temp & 0x04) {
		result = -1;
		dev_dbg(&I801_dev->dev, "Error: no response!\n");
	}

	if ((inb_p(SMBHSTSTS) & 0x1f) != 0x00)
		outb_p(inb(SMBHSTSTS), SMBHSTSTS);

	if ((temp = (0x1f & inb_p(SMBHSTSTS))) != 0x00) {
		dev_dbg(&I801_dev->dev, "Failed reset at end of transaction "
			"(%02x)\n", temp);
	}
	dev_dbg(&I801_dev->dev, "Transaction (post): CNT=%02x, CMD=%02x, "
		"ADD=%02x, DAT0=%02x, DAT1=%02x\n", inb_p(SMBHSTCNT),
		inb_p(SMBHSTCMD), inb_p(SMBHSTADD), inb_p(SMBHSTDAT0),
		inb_p(SMBHSTDAT1));
	return result;
}

/* All-inclusive block transaction function */
static int i801_block_transaction(union i2c_smbus_data *data, char read_write,
				  int command, int hwpec)
{
	int i, len;
	int smbcmd;
	int temp;
	int result = 0;
	int timeout;
	unsigned char hostc, errmask;

	if (command == I2C_SMBUS_I2C_BLOCK_DATA) {
		if (read_write == I2C_SMBUS_WRITE) {
			/* set I2C_EN bit in configuration register */
			pci_read_config_byte(I801_dev, SMBHSTCFG, &hostc);
			pci_write_config_byte(I801_dev, SMBHSTCFG,
					      hostc | SMBHSTCFG_I2C_EN);
		} else {
			dev_err(&I801_dev->dev,
				"I2C_SMBUS_I2C_BLOCK_READ not DB!\n");
			return -1;
		}
	}

	if (read_write == I2C_SMBUS_WRITE) {
		len = data->block[0];
		if (len < 1)
			len = 1;
		if (len > 32)
			len = 32;
		outb_p(len, SMBHSTDAT0);
		outb_p(data->block[1], SMBBLKDAT);
	} else {
		len = 32;	/* max for reads */
	}

	if(isich4 && command != I2C_SMBUS_I2C_BLOCK_DATA) {
		/* set 32 byte buffer */
	}

	for (i = 1; i <= len; i++) {
		if (i == len && read_write == I2C_SMBUS_READ)
			smbcmd = I801_BLOCK_LAST;
		else
			smbcmd = I801_BLOCK_DATA;
		outb_p(smbcmd | ENABLE_INT9, SMBHSTCNT);

		dev_dbg(&I801_dev->dev, "Block (pre %d): CNT=%02x, CMD=%02x, "
			"ADD=%02x, DAT0=%02x, BLKDAT=%02x\n", i,
			inb_p(SMBHSTCNT), inb_p(SMBHSTCMD), inb_p(SMBHSTADD),
			inb_p(SMBHSTDAT0), inb_p(SMBBLKDAT));

		/* Make sure the SMBus host is ready to start transmitting */
		temp = inb_p(SMBHSTSTS);
		if (i == 1) {
			/* Erronenous conditions before transaction: 
			 * Byte_Done, Failed, Bus_Err, Dev_Err, Intr, Host_Busy */
			errmask=0x9f; 
		} else {
			/* Erronenous conditions during transaction: 
			 * Failed, Bus_Err, Dev_Err, Intr */
			errmask=0x1e; 
		}
		if (temp & errmask) {
			dev_dbg(&I801_dev->dev, "SMBus busy (%02x). "
				"Resetting...\n", temp);
			outb_p(temp, SMBHSTSTS);
			if (((temp = inb_p(SMBHSTSTS)) & errmask) != 0x00) {
				dev_err(&I801_dev->dev,
					"Reset failed! (%02x)\n", temp);
				result = -1;
                                goto END;
			}
			if (i != 1) {
				/* if die in middle of block transaction, fail */
				result = -1;
				goto END;
			}
		}

		if (i == 1)
			outb_p(inb(SMBHSTCNT) | I801_START, SMBHSTCNT);

		/* We will always wait for a fraction of a second! */
		timeout = 0;
		do {
			msleep(1);
			temp = inb_p(SMBHSTSTS);
		}
		    while ((!(temp & 0x80))
			   && (timeout++ < MAX_TIMEOUT));

		/* If the SMBus is still busy, we give up */
		if (timeout >= MAX_TIMEOUT) {
			result = -1;
			dev_dbg(&I801_dev->dev, "SMBus Timeout!\n");
		}

		if (temp & 0x10) {
			result = -1;
			dev_dbg(&I801_dev->dev,
				"Error: Failed bus transaction\n");
		} else if (temp & 0x08) {
			result = -1;
			dev_err(&I801_dev->dev, "Bus collision!\n");
		} else if (temp & 0x04) {
			result = -1;
			dev_dbg(&I801_dev->dev, "Error: no response!\n");
		}

		if (i == 1 && read_write == I2C_SMBUS_READ) {
			len = inb_p(SMBHSTDAT0);
			if (len < 1)
				len = 1;
			if (len > 32)
				len = 32;
			data->block[0] = len;
		}

		/* Retrieve/store value in SMBBLKDAT */
		if (read_write == I2C_SMBUS_READ)
			data->block[i] = inb_p(SMBBLKDAT);
		if (read_write == I2C_SMBUS_WRITE && i+1 <= len)
			outb_p(data->block[i+1], SMBBLKDAT);
		if ((temp & 0x9e) != 0x00)
			outb_p(temp, SMBHSTSTS);  /* signals SMBBLKDAT ready */

		if ((temp = (0x1e & inb_p(SMBHSTSTS))) != 0x00) {
			dev_dbg(&I801_dev->dev,
				"Bad status (%02x) at end of transaction\n",
				temp);
		}
		dev_dbg(&I801_dev->dev, "Block (post %d): CNT=%02x, CMD=%02x, "
			"ADD=%02x, DAT0=%02x, BLKDAT=%02x\n", i,
			inb_p(SMBHSTCNT), inb_p(SMBHSTCMD), inb_p(SMBHSTADD),
			inb_p(SMBHSTDAT0), inb_p(SMBBLKDAT));

		if (result < 0)
			goto END;
	}

	if (hwpec) {
		/* wait for INTR bit as advised by Intel */
		timeout = 0;
		do {
			msleep(1);
			temp = inb_p(SMBHSTSTS);
		} while ((!(temp & 0x02))
			   && (timeout++ < MAX_TIMEOUT));

		if (timeout >= MAX_TIMEOUT) {
			dev_dbg(&I801_dev->dev, "PEC Timeout!\n");
		}
		outb_p(temp, SMBHSTSTS); 
	}
	result = 0;
END:
	if (command == I2C_SMBUS_I2C_BLOCK_DATA) {
		/* restore saved configuration register value */
		pci_write_config_byte(I801_dev, SMBHSTCFG, hostc);
	}
	return result;
}

/* Return -1 on error. */
static s32 i801_access(struct i2c_adapter * adap, u16 addr,
		       unsigned short flags, char read_write, u8 command,
		       int size, union i2c_smbus_data * data)
{
	int hwpec;
	int block = 0;
	int ret, xact = 0;

	hwpec = isich4 && (flags & I2C_CLIENT_PEC)
		&& size != I2C_SMBUS_QUICK
		&& size != I2C_SMBUS_I2C_BLOCK_DATA;

	switch (size) {
	case I2C_SMBUS_QUICK:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD);
		xact = I801_QUICK;
		break;
	case I2C_SMBUS_BYTE:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD);
		if (read_write == I2C_SMBUS_WRITE)
			outb_p(command, SMBHSTCMD);
		xact = I801_BYTE;
		break;
	case I2C_SMBUS_BYTE_DATA:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD);
		outb_p(command, SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE)
			outb_p(data->byte, SMBHSTDAT0);
		xact = I801_BYTE_DATA;
		break;
	case I2C_SMBUS_WORD_DATA:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD);
		outb_p(command, SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE) {
			outb_p(data->word & 0xff, SMBHSTDAT0);
			outb_p((data->word & 0xff00) >> 8, SMBHSTDAT1);
		}
		xact = I801_WORD_DATA;
		break;
	case I2C_SMBUS_BLOCK_DATA:
	case I2C_SMBUS_I2C_BLOCK_DATA:
		outb_p(((addr & 0x7f) << 1) | (read_write & 0x01),
		       SMBHSTADD);
		outb_p(command, SMBHSTCMD);
		block = 1;
		break;
	case I2C_SMBUS_PROC_CALL:
	default:
		dev_err(&I801_dev->dev, "Unsupported transaction %d\n", size);
		return -1;
	}

	outb_p(hwpec, SMBAUXCTL);	/* enable/disable hardware PEC */

	if(block)
		ret = i801_block_transaction(data, read_write, size, hwpec);
	else {
		outb_p(xact | ENABLE_INT9, SMBHSTCNT);
		ret = i801_transaction();
	}

	/* Some BIOSes don't like it when PEC is enabled at reboot or resume
	   time, so we forcibly disable it after every transaction. */
	if (hwpec)
		outb_p(0, SMBAUXCTL);

	if(block)
		return ret;
	if(ret)
		return -1;
	if ((read_write == I2C_SMBUS_WRITE) || (xact == I801_QUICK))
		return 0;

	switch (xact & 0x7f) {
	case I801_BYTE:	/* Result put in SMBHSTDAT0 */
	case I801_BYTE_DATA:
		data->byte = inb_p(SMBHSTDAT0);
		break;
	case I801_WORD_DATA:
		data->word = inb_p(SMBHSTDAT0) + (inb_p(SMBHSTDAT1) << 8);
		break;
	}
	return 0;
}


static u32 i801_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
	    I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	    I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_WRITE_I2C_BLOCK
	     | (isich4 ? I2C_FUNC_SMBUS_HWPEC_CALC : 0);
}

static struct i2c_algorithm smbus_algorithm = {
	.smbus_xfer	= i801_access,
	.functionality	= i801_func,
};

static struct i2c_adapter i801_adapter = {
	.owner		= THIS_MODULE,
	.class		= I2C_CLASS_HWMON,
	.algo		= &smbus_algorithm,
};

static struct pci_device_id i801_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AA_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801AB_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801BA_2) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801CA_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801DB_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801EB_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ESB_4) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH6_16) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH7_17) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ESB2_17) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH8_5) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH9_6) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_TOLAPAI_1) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH10_4) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_ICH10_5) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PCH_SMBUS) },
	{ 0, }
};

MODULE_DEVICE_TABLE (pci, i801_ids);

static int __devinit i801_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned char temp;
	int err;

	I801_dev = dev;
	if ((dev->device == PCI_DEVICE_ID_INTEL_82801DB_3) ||
	    (dev->device == PCI_DEVICE_ID_INTEL_82801EB_3) ||
	    (dev->device == PCI_DEVICE_ID_INTEL_ESB_4)     ||
	    (dev->device == PCI_DEVICE_ID_INTEL_TOLAPAI_1))
		isich4 = 1;
	else
		isich4 = 0;

	err = pci_enable_device(dev);
	if (err) {
		dev_err(&dev->dev, "Failed to enable SMBus PCI device (%d)\n",
			err);
		goto exit;
	}

	/* Determine the address of the SMBus area */
	i801_smba = pci_resource_start(dev, SMBBAR);
	if (!i801_smba) {
		dev_err(&dev->dev, "SMBus base address uninitialized, "
			"upgrade BIOS\n");
		err = -ENODEV;
		goto exit;
	}

	err = pci_request_region(dev, SMBBAR, i801_driver.name);
	if (err) {
		dev_err(&dev->dev, "Failed to request SMBus region "
			"0x%lx-0x%Lx\n", i801_smba,
			(unsigned long long)pci_resource_end(dev, SMBBAR));
		goto exit;
	}

	pci_read_config_byte(I801_dev, SMBHSTCFG, &temp);
	temp &= ~SMBHSTCFG_I2C_EN;	/* SMBus timing */
	if (!(temp & SMBHSTCFG_HST_EN)) {
		dev_info(&dev->dev, "Enabling SMBus device\n");
		temp |= SMBHSTCFG_HST_EN;
	}
	pci_write_config_byte(I801_dev, SMBHSTCFG, temp);

	if (temp & SMBHSTCFG_SMB_SMI_EN)
		dev_dbg(&dev->dev, "SMBus using interrupt SMI#\n");
	else
		dev_dbg(&dev->dev, "SMBus using PCI Interrupt\n");

	/* set up the driverfs linkage to our parent device */
	i801_adapter.dev.parent = &dev->dev;

	snprintf(i801_adapter.name, I2C_NAME_SIZE,
		"SMBus I801 adapter at %04lx", i801_smba);
	err = i2c_add_adapter(&i801_adapter);
	if (err) {
		dev_err(&dev->dev, "Failed to add SMBus adapter\n");
		goto exit_release;
	}
	return 0;

exit_release:
	pci_release_region(dev, SMBBAR);
exit:
	return err;
}

static void __devexit i801_remove(struct pci_dev *dev)
{
	i2c_del_adapter(&i801_adapter);
	pci_release_region(dev, SMBBAR);
	/*
	 * do not call pci_disable_device(dev) since it can cause hard hangs on
	 * some systems during power-off (eg. Fujitsu-Siemens Lifebook E8010)
	 */
}

static struct pci_driver i801_driver = {
	.name		= "i801_smbus",
	.id_table	= i801_ids,
	.probe		= i801_probe,
	.remove		= __devexit_p(i801_remove),
};

static int __init i2c_i801_init(void)
{
	return pci_register_driver(&i801_driver);
}

static void __exit i2c_i801_exit(void)
{
	pci_unregister_driver(&i801_driver);
}

MODULE_AUTHOR ("Frodo Looijaard <frodol@dds.nl>, "
		"Philip Edelbrock <phil@netroedge.com>, "
		"and Mark D. Studebaker <mdsxyz123@yahoo.com>");
MODULE_DESCRIPTION("I801 SMBus driver");
MODULE_LICENSE("GPL");

module_init(i2c_i801_init);
module_exit(i2c_i801_exit);
