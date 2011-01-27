/* cg3.c: CGTHREE frame buffer driver
 *
 * Copyright (C) 2003, 2006 David S. Miller (davem@davemloft.net)
 * Copyright (C) 1996,1998 Jakub Jelinek (jj@ultra.linux.cz)
 * Copyright (C) 1996 Miguel de Icaza (miguel@nuclecu.unam.mx)
 * Copyright (C) 1997 Eddie C. Dost (ecd@skynet.be)
 *
 * Driver layout based loosely on tgafb.c, see that file for credits.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/mm.h>

#include <asm/io.h>
#include <asm/oplib.h>
#include <asm/prom.h>
#include <asm/of_device.h>
#include <asm/fbio.h>

#include "sbuslib.h"

/*
 * Local functions.
 */

static int cg3_setcolreg(unsigned, unsigned, unsigned, unsigned,
			 unsigned, struct fb_info *);
static int cg3_blank(int, struct fb_info *);

static int cg3_mmap(struct fb_info *, struct vm_area_struct *);
static int cg3_ioctl(struct fb_info *, unsigned int, unsigned long);

/*
 *  Frame buffer operations
 */

static struct fb_ops cg3_ops = {
	.owner			= THIS_MODULE,
	.fb_setcolreg		= cg3_setcolreg,
	.fb_blank		= cg3_blank,
	.fb_fillrect		= cfb_fillrect,
	.fb_copyarea		= cfb_copyarea,
	.fb_imageblit		= cfb_imageblit,
	.fb_mmap		= cg3_mmap,
	.fb_ioctl		= cg3_ioctl,
#ifdef CONFIG_COMPAT
	.fb_compat_ioctl	= sbusfb_compat_ioctl,
#endif
};


/* Control Register Constants */
#define CG3_CR_ENABLE_INTS      0x80
#define CG3_CR_ENABLE_VIDEO     0x40
#define CG3_CR_ENABLE_TIMING    0x20
#define CG3_CR_ENABLE_CURCMP    0x10
#define CG3_CR_XTAL_MASK        0x0c
#define CG3_CR_DIVISOR_MASK     0x03

/* Status Register Constants */
#define CG3_SR_PENDING_INT      0x80
#define CG3_SR_RES_MASK         0x70
#define CG3_SR_1152_900_76_A    0x40
#define CG3_SR_1152_900_76_B    0x60
#define CG3_SR_ID_MASK          0x0f
#define CG3_SR_ID_COLOR         0x01
#define CG3_SR_ID_MONO          0x02
#define CG3_SR_ID_MONO_ECL      0x03

enum cg3_type {
	CG3_AT_66HZ = 0,
	CG3_AT_76HZ,
	CG3_RDI
};

struct bt_regs {
	u32 addr;
	u32 color_map;
	u32 control;
	u32 cursor;
};

struct cg3_regs {
	struct bt_regs	cmap;
	u8	control;
	u8	status;
	u8	cursor_start;
	u8	cursor_end;
	u8	h_blank_start;
	u8	h_blank_end;
	u8	h_sync_start;
	u8	h_sync_end;
	u8	comp_sync_end;
	u8	v_blank_start_high;
	u8	v_blank_start_low;
	u8	v_blank_end;
	u8	v_sync_start;
	u8	v_sync_end;
	u8	xfer_holdoff_start;
	u8	xfer_holdoff_end;
};

/* Offset of interesting structures in the OBIO space */
#define CG3_REGS_OFFSET	     0x400000UL
#define CG3_RAM_OFFSET	     0x800000UL

struct cg3_par {
	spinlock_t		lock;
	struct cg3_regs		__iomem *regs;
	u32			sw_cmap[((256 * 3) + 3) / 4];

	u32			flags;
#define CG3_FLAG_BLANKED	0x00000001
#define CG3_FLAG_RDI		0x00000002

	unsigned long		physbase;
	unsigned long		which_io;
	unsigned long		fbsize;
};

/**
 *      cg3_setcolreg - Optional function. Sets a color register.
 *      @regno: boolean, 0 copy local, 1 get_user() function
 *      @red: frame buffer colormap structure
 *      @green: The green value which can be up to 16 bits wide
 *      @blue:  The blue value which can be up to 16 bits wide.
 *      @transp: If supported the alpha value which can be up to 16 bits wide.
 *      @info: frame buffer info structure
 *
 * The cg3 palette is loaded with 4 color values at each time
 * so you end up with: (rgb)(r), (gb)(rg), (b)(rgb), and so on.
 * We keep a sw copy of the hw cmap to assist us in this esoteric
 * loading procedure.
 */
static int cg3_setcolreg(unsigned regno,
			 unsigned red, unsigned green, unsigned blue,
			 unsigned transp, struct fb_info *info)
{
	struct cg3_par *par = (struct cg3_par *) info->par;
	struct bt_regs __iomem *bt = &par->regs->cmap;
	unsigned long flags;
	u32 *p32;
	u8 *p8;
	int count;

	if (regno >= 256)
		return 1;

	red >>= 8;
	green >>= 8;
	blue >>= 8;

	spin_lock_irqsave(&par->lock, flags);

	p8 = (u8 *)par->sw_cmap + (regno * 3);
	p8[0] = red;
	p8[1] = green;
	p8[2] = blue;

#define D4M3(x) ((((x)>>2)<<1) + ((x)>>2))      /* (x/4)*3 */
#define D4M4(x) ((x)&~0x3)                      /* (x/4)*4 */

	count = 3;
	p32 = &par->sw_cmap[D4M3(regno)];
	sbus_writel(D4M4(regno), &bt->addr);
	while (count--)
		sbus_writel(*p32++, &bt->color_map);

#undef D4M3
#undef D4M4

	spin_unlock_irqrestore(&par->lock, flags);

	return 0;
}

/**
 *      cg3_blank - Optional function.  Blanks the display.
 *      @blank_mode: the blank mode we want.
 *      @info: frame buffer structure that represents a single frame buffer
 */
static int
cg3_blank(int blank, struct fb_info *info)
{
	struct cg3_par *par = (struct cg3_par *) info->par;
	struct cg3_regs __iomem *regs = par->regs;
	unsigned long flags;
	u8 val;

	spin_lock_irqsave(&par->lock, flags);

	switch (blank) {
	case FB_BLANK_UNBLANK: /* Unblanking */
		val = sbus_readb(&regs->control);
		val |= CG3_CR_ENABLE_VIDEO;
		sbus_writeb(val, &regs->control);
		par->flags &= ~CG3_FLAG_BLANKED;
		break;

	case FB_BLANK_NORMAL: /* Normal blanking */
	case FB_BLANK_VSYNC_SUSPEND: /* VESA blank (vsync off) */
	case FB_BLANK_HSYNC_SUSPEND: /* VESA blank (hsync off) */
	case FB_BLANK_POWERDOWN: /* Poweroff */
		val = sbus_readb(&regs->control);
		val &= ~CG3_CR_ENABLE_VIDEO;
		sbus_writeb(val, &regs->control);
		par->flags |= CG3_FLAG_BLANKED;
		break;
	}

	spin_unlock_irqrestore(&par->lock, flags);

	return 0;
}

static struct sbus_mmap_map cg3_mmap_map[] = {
	{
		.voff	= CG3_MMAP_OFFSET,
		.poff	= CG3_RAM_OFFSET,
		.size	= SBUS_MMAP_FBSIZE(1)
	},
	{ .size = 0 }
};

static int cg3_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct cg3_par *par = (struct cg3_par *)info->par;

	return sbusfb_mmap_helper(cg3_mmap_map,
				  par->physbase, par->fbsize,
				  par->which_io,
				  vma);
}

static int cg3_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	struct cg3_par *par = (struct cg3_par *) info->par;

	return sbusfb_ioctl_helper(cmd, arg, info,
				   FBTYPE_SUN3COLOR, 8, par->fbsize);
}

/*
 *  Initialisation
 */

static void
cg3_init_fix(struct fb_info *info, int linebytes, struct device_node *dp)
{
	strlcpy(info->fix.id, dp->name, sizeof(info->fix.id));

	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_PSEUDOCOLOR;

	info->fix.line_length = linebytes;

	info->fix.accel = FB_ACCEL_SUN_CGTHREE;
}

static void cg3_rdi_maybe_fixup_var(struct fb_var_screeninfo *var,
				    struct device_node *dp)
{
	char *params;
	char *p;
	int ww, hh;

	params = of_get_property(dp, "params", NULL);
	if (params) {
		ww = simple_strtoul(params, &p, 10);
		if (ww && *p == 'x') {
			hh = simple_strtoul(p + 1, &p, 10);
			if (hh && *p == '-') {
				if (var->xres != ww ||
				    var->yres != hh) {
					var->xres = var->xres_virtual = ww;
					var->yres = var->yres_virtual = hh;
				}
			}
		}
	}
}

static u8 cg3regvals_66hz[] __initdata = {	/* 1152 x 900, 66 Hz */
	0x14, 0xbb,	0x15, 0x2b,	0x16, 0x04,	0x17, 0x14,
	0x18, 0xae,	0x19, 0x03,	0x1a, 0xa8,	0x1b, 0x24,
	0x1c, 0x01,	0x1d, 0x05,	0x1e, 0xff,	0x1f, 0x01,
	0x10, 0x20,	0
};

static u8 cg3regvals_76hz[] __initdata = {	/* 1152 x 900, 76 Hz */
	0x14, 0xb7,	0x15, 0x27,	0x16, 0x03,	0x17, 0x0f,
	0x18, 0xae,	0x19, 0x03,	0x1a, 0xae,	0x1b, 0x2a,
	0x1c, 0x01,	0x1d, 0x09,	0x1e, 0xff,	0x1f, 0x01,
	0x10, 0x24,	0
};

static u8 cg3regvals_rdi[] __initdata = {	/* 640 x 480, cgRDI */
	0x14, 0x70,	0x15, 0x20,	0x16, 0x08,	0x17, 0x10,
	0x18, 0x06,	0x19, 0x02,	0x1a, 0x31,	0x1b, 0x51,
	0x1c, 0x06,	0x1d, 0x0c,	0x1e, 0xff,	0x1f, 0x01,
	0x10, 0x22,	0
};

static u8 *cg3_regvals[] __initdata = {
	cg3regvals_66hz, cg3regvals_76hz, cg3regvals_rdi
};

static u_char cg3_dacvals[] __initdata = {
	4, 0xff,	5, 0x00,	6, 0x70,	7, 0x00,	0
};

static void cg3_do_default_mode(struct cg3_par *par)
{
	enum cg3_type type;
	u8 *p;

	if (par->flags & CG3_FLAG_RDI)
		type = CG3_RDI;
	else {
		u8 status = sbus_readb(&par->regs->status), mon;
		if ((status & CG3_SR_ID_MASK) == CG3_SR_ID_COLOR) {
			mon = status & CG3_SR_RES_MASK;
			if (mon == CG3_SR_1152_900_76_A ||
			    mon == CG3_SR_1152_900_76_B)
				type = CG3_AT_76HZ;
			else
				type = CG3_AT_66HZ;
		} else {
			prom_printf("cgthree: can't handle SR %02x\n",
				    status);
			prom_halt();
			return;
		}
	}

	for (p = cg3_regvals[type]; *p; p += 2) {
		u8 __iomem *regp = &((u8 __iomem *)par->regs)[p[0]];
		sbus_writeb(p[1], regp);
	}
	for (p = cg3_dacvals; *p; p += 2) {
		u8 __iomem *regp;

		regp = (u8 __iomem *)&par->regs->cmap.addr;
		sbus_writeb(p[0], regp);
		regp = (u8 __iomem *)&par->regs->cmap.control;
		sbus_writeb(p[1], regp);
	}
}

struct all_info {
	struct fb_info info;
	struct cg3_par par;
};

static int __devinit cg3_init_one(struct of_device *op)
{
	struct device_node *dp = op->node;
	struct all_info *all;
	int linebytes, err;

	all = kzalloc(sizeof(*all), GFP_KERNEL);
	if (!all)
		return -ENOMEM;

	spin_lock_init(&all->par.lock);

	all->par.physbase = op->resource[0].start;
	all->par.which_io = op->resource[0].flags & IORESOURCE_BITS;

	sbusfb_fill_var(&all->info.var, dp->node, 8);
	all->info.var.red.length = 8;
	all->info.var.green.length = 8;
	all->info.var.blue.length = 8;
	if (!strcmp(dp->name, "cgRDI"))
		all->par.flags |= CG3_FLAG_RDI;
	if (all->par.flags & CG3_FLAG_RDI)
		cg3_rdi_maybe_fixup_var(&all->info.var, dp);

	linebytes = of_getintprop_default(dp, "linebytes",
					  all->info.var.xres);
	all->par.fbsize = PAGE_ALIGN(linebytes * all->info.var.yres);

	all->par.regs = of_ioremap(&op->resource[0], CG3_REGS_OFFSET,
				   sizeof(struct cg3_regs), "cg3 regs");

	all->info.flags = FBINFO_DEFAULT;
	all->info.fbops = &cg3_ops;
	all->info.screen_base =
		of_ioremap(&op->resource[0], CG3_RAM_OFFSET,
			   all->par.fbsize, "cg3 ram");
	all->info.par = &all->par;

	cg3_blank(0, &all->info);

	if (!of_find_property(dp, "width", NULL))
		cg3_do_default_mode(&all->par);

	if (fb_alloc_cmap(&all->info.cmap, 256, 0)) {
		of_iounmap(all->par.regs, sizeof(struct cg3_regs));
		of_iounmap(all->info.screen_base, all->par.fbsize);
		kfree(all);
		return -ENOMEM;
	}
	fb_set_cmap(&all->info.cmap, &all->info);

	cg3_init_fix(&all->info, linebytes, dp);

	err = register_framebuffer(&all->info);
	if (err < 0) {
		fb_dealloc_cmap(&all->info.cmap);
		of_iounmap(all->par.regs, sizeof(struct cg3_regs));
		of_iounmap(all->info.screen_base, all->par.fbsize);
		kfree(all);
		return err;
	}

	dev_set_drvdata(&op->dev, all);

	printk("%s: cg3 at %lx:%lx\n",
	       dp->full_name, all->par.which_io, all->par.physbase);

	return 0;
}

static int __devinit cg3_probe(struct of_device *dev, const struct of_device_id *match)
{
	struct of_device *op = to_of_device(&dev->dev);

	return cg3_init_one(op);
}

static int __devexit cg3_remove(struct of_device *dev)
{
	struct all_info *all = dev_get_drvdata(&dev->dev);

	unregister_framebuffer(&all->info);
	fb_dealloc_cmap(&all->info.cmap);

	of_iounmap(all->par.regs, sizeof(struct cg3_regs));
	of_iounmap(all->info.screen_base, all->par.fbsize);

	kfree(all);

	dev_set_drvdata(&dev->dev, NULL);

	return 0;
}

static struct of_device_id cg3_match[] = {
	{
		.name = "cgthree",
	},
	{
		.name = "cgRDI",
	},
	{},
};
MODULE_DEVICE_TABLE(of, cg3_match);

static struct of_platform_driver cg3_driver = {
	.name		= "cg3",
	.match_table	= cg3_match,
	.probe		= cg3_probe,
	.remove		= __devexit_p(cg3_remove),
};

static int __init cg3_init(void)
{
	if (fb_get_options("cg3fb", NULL))
		return -ENODEV;

	return of_register_driver(&cg3_driver, &of_bus_type);
}

static void __exit cg3_exit(void)
{
	of_unregister_driver(&cg3_driver);
}

module_init(cg3_init);
module_exit(cg3_exit);

MODULE_DESCRIPTION("framebuffer driver for CGthree chipsets");
MODULE_AUTHOR("David S. Miller <davem@davemloft.net>");
MODULE_VERSION("2.0");
MODULE_LICENSE("GPL");
