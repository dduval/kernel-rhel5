/*
 *  drivers/s390/cio/chsc.c
 *   S/390 common I/O routines -- channel subsystem call
 *
 *    Copyright (C) 1999-2002 IBM Deutschland Entwicklung GmbH,
 *			      IBM Corporation
 *    Author(s): Ingo Adlung (adlung@de.ibm.com)
 *		 Cornelia Huck (cornelia.huck@de.ibm.com)
 *		 Arnd Bergmann (arndb@de.ibm.com)
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>

#include <asm/cio.h>
#include <asm/lowcore.h>
#include <asm/ptrace.h>
#include <asm/chpid.h>

#include "css.h"
#include "cio.h"
#include "cio_debug.h"
#include "ioasm.h"
#include "chp.h"
#include "chsc.h"
#include "../s390mach.h"

static void *sei_page;

/* FIXME: this is _always_ called for every subchannel. shouldn't we
 *	  process more than one at a time? */
static int
chsc_get_sch_desc_irq(struct subchannel *sch, void *page)
{
	int ccode, j;

	struct {
		struct chsc_header request;
		u16 reserved1a:10;
		u16 ssid:2;
		u16 reserved1b:4;
		u16 f_sch;	  /* first subchannel */
		u16 reserved2;
		u16 l_sch;	  /* last subchannel */
		u32 reserved3;
		struct chsc_header response;
		u32 reserved4;
		u8 sch_valid : 1;
		u8 dev_valid : 1;
		u8 st	     : 3; /* subchannel type */
		u8 zeroes    : 3;
		u8  unit_addr;	  /* unit address */
		u16 devno;	  /* device number */
		u8 path_mask;
		u8 fla_valid_mask;
		u16 sch;	  /* subchannel */
		u8 chpid[8];	  /* chpids 0-7 */
		u16 fla[8];	  /* full link addresses 0-7 */
	} *ssd_area;

	ssd_area = page;

	ssd_area->request.length = 0x0010;
	ssd_area->request.code = 0x0004;

	ssd_area->ssid = sch->schid.ssid;
	ssd_area->f_sch = sch->schid.sch_no;
	ssd_area->l_sch = sch->schid.sch_no;

	ccode = chsc(ssd_area);
	if (ccode > 0) {
		pr_debug("chsc returned with ccode = %d\n", ccode);
		return (ccode == 3) ? -ENODEV : -EBUSY;
	}

	switch (ssd_area->response.code) {
	case 0x0001: /* everything ok */
		break;
	case 0x0002:
		CIO_CRW_EVENT(2, "Invalid command!\n");
		return -EINVAL;
	case 0x0003:
		CIO_CRW_EVENT(2, "Error in chsc request block!\n");
		return -EINVAL;
	case 0x0004:
		CIO_CRW_EVENT(2, "Model does not provide ssd\n");
		return -EOPNOTSUPP;
	default:
		CIO_CRW_EVENT(2, "Unknown CHSC response %d\n",
			      ssd_area->response.code);
		return -EIO;
	}

	/*
	 * ssd_area->st stores the type of the detected
	 * subchannel, with the following definitions:
	 *
	 * 0: I/O subchannel:	  All fields have meaning
	 * 1: CHSC subchannel:	  Only sch_val, st and sch
	 *			  have meaning
	 * 2: Message subchannel: All fields except unit_addr
	 *			  have meaning
	 * 3: ADM subchannel:	  Only sch_val, st and sch
	 *			  have meaning
	 *
	 * Other types are currently undefined.
	 */
	if (ssd_area->st > 3) { /* uhm, that looks strange... */
		CIO_CRW_EVENT(0, "Strange subchannel type %d"
			      " for sch 0.%x.%04x\n", ssd_area->st,
			      sch->schid.ssid, sch->schid.sch_no);
		/*
		 * There may have been a new subchannel type defined in the
		 * time since this code was written; since we don't know which
		 * fields have meaning and what to do with it we just jump out
		 */
		return 0;
	} else {
		const char *type[4] = {"I/O", "chsc", "message", "ADM"};
		CIO_CRW_EVENT(6, "ssd: sch 0.%x.%04x is %s subchannel\n",
			      sch->schid.ssid, sch->schid.sch_no,
			      type[ssd_area->st]);

		sch->ssd_info.valid = 1;
		sch->ssd_info.type = ssd_area->st;
	}

	if (ssd_area->st == 0 || ssd_area->st == 2) {
		for (j = 0; j < 8; j++) {
			if (!((0x80 >> j) & ssd_area->path_mask &
			      ssd_area->fla_valid_mask))
				continue;
			sch->ssd_info.chpid[j] = ssd_area->chpid[j];
			sch->ssd_info.fla[j]   = ssd_area->fla[j];
		}
	}
	return 0;
}

int
css_get_ssd_info(struct subchannel *sch)
{
	int ret;
	void *page;

	page = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!page)
		return -ENOMEM;
	spin_lock_irq(&sch->lock);
	ret = chsc_get_sch_desc_irq(sch, page);
	if (ret) {
		static int cio_chsc_err_msg;
		
		if (!cio_chsc_err_msg) {
			printk(KERN_ERR
			       "chsc_get_sch_descriptions:"
			       " Error %d while doing chsc; "
			       "processing some machine checks may "
			       "not work\n", ret);
			cio_chsc_err_msg = 1;
		}
	}
	spin_unlock_irq(&sch->lock);
	free_page((unsigned long)page);
	if (!ret) {
		int j, mask;
		struct chp_id chpid;

		chp_id_init(&chpid);
		/* Allocate channel path structures, if needed. */
		for (j = 0; j < 8; j++) {
			mask = 0x80 >> j;
			chpid.id = sch->ssd_info.chpid[j];
			if ((sch->schib.pmcw.pim & mask) &&
			    !chp_is_registered(chpid))
				chp_new(chpid);
		}
	}
	return ret;
}

static int
check_for_io_on_path(struct subchannel *sch, int mask)
{
	int cc;

	cc = stsch(sch->schid, &sch->schib);
	if (cc)
		return 0;
	if (sch->schib.scsw.actl && sch->schib.pmcw.lpum == mask)
		return 1;
	return 0;
}

static void
terminate_internal_io(struct subchannel *sch)
{
	if (cio_clear(sch)) {
		/* Recheck device in case clear failed */
		sch->lpm = 0;
		if (device_trigger_verify(sch) != 0)
			css_schedule_eval(sch->schid);
		return;
	}
	/* Request retry of internal operation. */
	device_set_intretry(sch);

	/* Call termination handler. */
	if (sch->driver && sch->driver->termination)
		sch->driver->termination(&sch->dev);
}

static int s390_subchannel_remove_chpid(struct subchannel *sch, void *data)
{
	int j;
	int mask;
	struct chp_id *chpid = data;
	struct schib schib;

	for (j = 0; j < 8; j++) {
		mask = 0x80 >> j;
		if ((sch->schib.pmcw.pim & mask) &&
		    (sch->schib.pmcw.chpid[j] == chpid->id))
			break;
	}
	if (j >= 8)
		return 0;

	spin_lock_irq(&sch->lock);

	stsch(sch->schid, &schib);
	if (!schib.pmcw.dnv)
		goto out_unreg;
	memcpy(&sch->schib, &schib, sizeof(struct schib));
	/* Check for single path devices. */
	if (sch->schib.pmcw.pim == 0x80)
		goto out_unreg;

	if (check_for_io_on_path(sch, mask)) {
		if (device_is_online(sch))
			device_kill_io(sch);
		else {
			terminate_internal_io(sch);
			/* Re-start path verification. */
			if (sch->driver && sch->driver->verify)
				sch->driver->verify(&sch->dev);
		}
	} else {
		/* trigger path verification. */
		if (sch->driver && sch->driver->verify)
			sch->driver->verify(&sch->dev);
		else if (sch->lpm == mask)
			goto out_unreg;
	}

	spin_unlock_irq(&sch->lock);
	return 0;

out_unreg:
	sch->lpm = 0;
	spin_unlock_irq(&sch->lock);
	css_schedule_eval(sch->schid);
	return 0;
}

void chsc_chp_offline(struct chp_id chpid)
{
	char dbf_txt[15];

	sprintf(dbf_txt, "chpr%x.%02x", chpid.cssid, chpid.id);
	CIO_TRACE_EVENT(2, dbf_txt);

	if (chp_get_status(chpid) <= 0)
		return;
	/* Wait until previous actions have settled. */
	css_wait_for_slow_path();
	for_each_subchannel_staged(s390_subchannel_remove_chpid, NULL, &chpid);
}

struct res_acc_data {
	struct chp_id chpid;
	u32 fla_mask;
	u16 fla;
};

static int s390_process_res_acc_sch(struct res_acc_data *res_data,
				    struct subchannel *sch)
{
	int found;
	int chp;
	int ccode;

	found = 0;
	for (chp = 0; chp <= 7; chp++)
		/*
		 * check if chpid is in information updated by ssd
		 */
		if (sch->ssd_info.valid &&
		    sch->ssd_info.chpid[chp] == res_data->chpid.id &&
		    (sch->ssd_info.fla[chp] & res_data->fla_mask)
		    == res_data->fla) {
			found = 1;
			break;
		}

	if (found == 0)
		return 0;

	/*
	 * Do a stsch to update our subchannel structure with the
	 * new path information and eventually check for logically
	 * offline chpids.
	 */
	ccode = stsch(sch->schid, &sch->schib);
	if (ccode > 0)
		return 0;

	return 0x80 >> chp;
}

static int s390_process_res_acc_new_sch(struct subchannel_id schid, void *data)
{
	struct schib schib;
	/*
	 * We don't know the device yet, but since a path
	 * may be available now to the device we'll have
	 * to do recognition again.
	 * Since we don't have any idea about which chpid
	 * that beast may be on we'll have to do a stsch
	 * on all devices, grr...
	 */
	if (stsch_err(schid, &schib))
		/* We're through */
		return -ENXIO;

	/* Put it on the slow path. */
	css_schedule_eval(schid);
	return 0;
}

static int __s390_process_res_acc(struct subchannel *sch, void *data)
{
	int chp_mask, old_lpm;
	struct res_acc_data *res_data = data;

	spin_lock_irq(&sch->lock);
	chp_mask = s390_process_res_acc_sch(res_data, sch);

	if (chp_mask == 0) {
		spin_unlock_irq(&sch->lock);
		return 0;
	}
	old_lpm = sch->lpm;
	sch->lpm = ((sch->schib.pmcw.pim &
		     sch->schib.pmcw.pam &
		     sch->schib.pmcw.pom)
		    | chp_mask) & sch->opm;
	if (!old_lpm && sch->lpm)
		device_trigger_reprobe(sch);
	else if (sch->driver && sch->driver->verify)
		sch->driver->verify(&sch->dev);

	spin_unlock_irq(&sch->lock);

	return 0;
}


static void s390_process_res_acc(struct res_acc_data *res_data)
{
	char dbf_txt[15];

	sprintf(dbf_txt, "accpr%x.%02x", res_data->chpid.cssid,
		res_data->chpid.id);
	CIO_TRACE_EVENT( 2, dbf_txt);
	if (res_data->fla != 0) {
		sprintf(dbf_txt, "fla%x", res_data->fla);
		CIO_TRACE_EVENT( 2, dbf_txt);
	}
	/* Wait until previous actions have settled. */
	css_wait_for_slow_path();
	/*
	 * I/O resources may have become accessible.
	 * Scan through all subchannels that may be concerned and
	 * do a validation on those.
	 * The more information we have (info), the less scanning
	 * will we have to do.
	 */
	for_each_subchannel_staged(__s390_process_res_acc,
				   s390_process_res_acc_new_sch, res_data);
}

static int
__get_chpid_from_lir(void *data)
{
	struct lir {
		u8  iq;
		u8  ic;
		u16 sci;
		/* incident-node descriptor */
		u32 indesc[28];
		/* attached-node descriptor */
		u32 andesc[28];
		/* incident-specific information */
		u32 isinfo[28];
	} *lir;

	lir = (struct lir*) data;
	if (!(lir->iq&0x80))
		/* NULL link incident record */
		return -EINVAL;
	if (!(lir->indesc[0]&0xc0000000))
		/* node descriptor not valid */
		return -EINVAL;
	if (!(lir->indesc[0]&0x10000000))
		/* don't handle device-type nodes - FIXME */
		return -EINVAL;
	/* Byte 3 contains the chpid. Could also be CTCA, but we don't care */

	return (u16) (lir->indesc[0]&0x000000ff);
}

struct chp_config_data {
	u8 map[32];
	u8 op;
	u8 pc;
};

struct chsc_sei_area {
	struct chsc_header request;
	u32 reserved1;
	u32 reserved2;
	u32 reserved3;
	struct chsc_header response;
	u32 reserved4;
	u8  flags;
	u8  vf;		/* validity flags */
	u8  rs;		/* reporting source */
	u8  cc;		/* content code */
	u16 fla;	/* full link address */
	u16 rsid;	/* reporting source id */
	u32 reserved5;
	u32 reserved6;
	u32 ccdf[96];	/* content-code dependent field */
	/* ccdf has to be big enough for a link-incident record */
};

static void chsc_process_sei_chp_config(struct chsc_sei_area *sei_area)
{
	struct chp_config_data *data;
	struct chp_id chpid;
	int num;

	CIO_CRW_EVENT(4, "chsc: channel-path-configuration notification\n");
	if (sei_area->rs != 0)
		return;
	data = (struct chp_config_data *) &(sei_area->ccdf);
	chp_id_init(&chpid);
	for (num = 0; num <= __MAX_CHPID; num++) {
		if (!chp_test_bit(data->map, num))
			continue;
		chpid.id = num;
		printk(KERN_WARNING "cio: processing configure event %d for "
		       "chpid %x.%02x\n", data->op, chpid.cssid, chpid.id);
		switch (data->op) {
		case 0:
			chp_cfg_schedule(chpid, 1);
			break;
		case 1:
			chp_cfg_schedule(chpid, 0);
			break;
		case 2:
			chp_cfg_cancel_deconfigure(chpid);
			break;
		}
	}
}

void chsc_process_crw(void)
{
	struct chp_id chpid;
	int id;
	struct res_acc_data res_data;
	struct chsc_sei_area *sei_area;

	if (!sei_page)
		return;
	/*
	 * build the chsc request block for store event information
	 * and do the call
	 * This function is only called by the machine check handler thread,
	 * so we don't need locking for the sei_page.
	 */
	sei_area = sei_page;

	CIO_TRACE_EVENT( 2, "prcss");
	do {
		int ccode, status;
		memset(sei_area, 0, sizeof(*sei_area));
		memset(&res_data, 0, sizeof(struct res_acc_data));
		sei_area->request.length = 0x0010;
		sei_area->request.code = 0x000e;

		ccode = chsc(sei_area);
		if (ccode > 0)
			return;

		switch (sei_area->response.code) {
			/* for debug purposes, check for problems */
		case 0x0001:
			CIO_CRW_EVENT(4, "chsc_process_crw: event information "
					"successfully stored\n");
			break; /* everything ok */
		case 0x0002:
			CIO_CRW_EVENT(2,
				      "chsc_process_crw: invalid command!\n");
			return;
		case 0x0003:
			CIO_CRW_EVENT(2, "chsc_process_crw: error in chsc "
				      "request block!\n");
			return;
		case 0x0005:
			CIO_CRW_EVENT(2, "chsc_process_crw: no event "
				      "information stored\n");
			return;
		default:
			CIO_CRW_EVENT(2, "chsc_process_crw: chsc response %d\n",
				      sei_area->response.code);
			return;
		}

		/* Check if we might have lost some information. */
		if (sei_area->flags & 0x40)
			CIO_CRW_EVENT(2, "chsc_process_crw: Event information "
				       "has been lost due to overflow!\n");

		/* which kind of information was stored? */
		switch (sei_area->cc) {
		case 1: /* link incident*/
			if (sei_area->rs != 4) {
				CIO_CRW_EVENT(2, "chsc_process_crw: reporting "
					"source (%04X) isn't a chpid!\n",
					sei_area->rsid);
				continue;
			}
			CIO_CRW_EVENT(4, "chsc_process_crw: "
				      "channel subsystem reports link incident,"
				      " reporting source is chpid %x\n",
				      sei_area->rsid);
			id = __get_chpid_from_lir(sei_area->ccdf);
			if (id < 0)
				CIO_CRW_EVENT(4, "%s: Invalid LIR, skipping\n",
					      __FUNCTION__);
			else {
				chp_id_init(&chpid);
				chpid.id = id;
				chsc_chp_offline(chpid);
			}
			break;

		case 2: /* i/o resource accessibiliy */
			if (sei_area->rs != 4) {
				CIO_CRW_EVENT(2, "chsc_process_crw: reporting "
					"source (%04X) isn't a chpid!\n",
					sei_area->rsid);
				continue;
			}
			CIO_CRW_EVENT(4, "chsc_process_crw: "
				      "channel subsystem reports some I/O "
				      "devices may have become accessible\n");
			pr_debug("Data received after sei: \n");
			pr_debug("Validity flags: %x\n", sei_area->vf);
			chp_id_init(&chpid);
			chpid.id = sei_area->rsid;
			/* allocate a new channel path structure, if needed */
			status = chp_get_status(chpid);
			if (status < 0)
				chp_new(chpid);
			else if (!status)
				break;
			res_data.chpid = chpid;
			pr_debug("chpid: %x", sei_area->rsid);
			if ((sei_area->vf & 0xc0) != 0) {
				res_data.fla = sei_area->fla;
				if ((sei_area->vf & 0xc0) == 0xc0) {
					pr_debug(" full link addr: %x",
						 sei_area->fla);
					res_data.fla_mask = 0xffff;
				} else {
					pr_debug(" link addr: %x",
						 sei_area->fla);
					res_data.fla_mask = 0xff00;
				}
			}
			s390_process_res_acc(&res_data);
			pr_debug("\n\n");
			break;
		case 8: /* channel-path-configuration notification */
			chsc_process_sei_chp_config(sei_area);
			break;
		default: /* other stuff */
			CIO_CRW_EVENT(4, "chsc_process_crw: event %d\n",
				      sei_area->cc);
			break;
		}
	} while (sei_area->flags & 0x80);
}

static int __chp_add_new_sch(struct subchannel_id schid, void *data)
{
	struct schib schib;

	if (stsch_err(schid, &schib))
		/* We're through */
		return -ENXIO;

	/* Put it on the slow path. */
	css_schedule_eval(schid);
	return 0;
}


static int __chp_add(struct subchannel *sch, void *data)
{
	int i, mask;
	struct chp_id *chpid = data;

	spin_lock_irq(&sch->lock);
	for (i=0; i<8; i++) {
		mask = 0x80 >> i;
		if ((sch->schib.pmcw.pim & mask) &&
		    (sch->schib.pmcw.chpid[i] == chpid->id))
			break;
	}
	if (i==8) {
		spin_unlock_irq(&sch->lock);
		return 0;
	}
	if (stsch(sch->schid, &sch->schib)) {
		spin_unlock_irq(&sch->lock);
		css_schedule_eval(sch->schid);
		return 0;
	}
	sch->lpm = ((sch->schib.pmcw.pim &
		     sch->schib.pmcw.pam &
		     sch->schib.pmcw.pom)
		    | mask) & sch->opm;

	if (sch->driver && sch->driver->verify)
		sch->driver->verify(&sch->dev);

	spin_unlock_irq(&sch->lock);

	return 0;
}

void chsc_chp_online(struct chp_id chpid)
{
	char dbf_txt[15];

	sprintf(dbf_txt, "cadd%x.%02x", chpid.cssid, chpid.id);
	CIO_TRACE_EVENT(2, dbf_txt);

	/* Wait until previous actions have settled. */
	css_wait_for_slow_path();

	if (chp_get_status(chpid) != 0)
		for_each_subchannel_staged(__chp_add, __chp_add_new_sch,
					   &chpid);
}

static void __s390_subchannel_vary_chpid(struct subchannel *sch,
					 struct chp_id chpid, int on)
{
	int chp, old_lpm;
	int mask;
	unsigned long flags;

	if (!sch->ssd_info.valid)
		return;
	
	spin_lock_irqsave(&sch->lock, flags);
	old_lpm = sch->lpm;
	for (chp = 0; chp < 8; chp++) {
		mask = 0x80 >> chp;
		if (sch->ssd_info.chpid[chp] != chpid.id)
			continue;

		if (on) {
			sch->opm |= mask;
			sch->lpm |= mask;
			if (!old_lpm)
				device_trigger_reprobe(sch);
			else if (sch->driver && sch->driver->verify)
				sch->driver->verify(&sch->dev);
			break;
		}
		sch->opm &= ~mask;
		sch->lpm &= ~mask;
		/*
		 * Give running I/O a grace period in which it
		 * can successfully terminate, even using the
		 * just varied off path. Then kill it.
		 */
		if (check_for_io_on_path(sch, mask)) {
			if (device_is_online(sch))
				/* Wait for I/O to finish */
				device_set_waiting(sch);
			else {
				/* Kill and retry internal I/O */
				terminate_internal_io(sch);
				/* Re-start path verification. */
				if (sch->driver && sch->driver->verify)
					sch->driver->verify(&sch->dev);
			}
		} else if (!sch->lpm) {
			if (device_trigger_verify(sch) != 0)
				css_schedule_eval(sch->schid);
		} else if (sch->driver && sch->driver->verify)
			sch->driver->verify(&sch->dev);
		break;
	}
	spin_unlock_irqrestore(&sch->lock, flags);
}

static int s390_subchannel_vary_chpid_off(struct subchannel *sch, void *data)
{
	struct chp_id *chpid = data;

	__s390_subchannel_vary_chpid(sch, *chpid, 0);
	return 0;
}

static int s390_subchannel_vary_chpid_on(struct subchannel *sch, void *data)
{
	struct chp_id *chpid = data;

	__s390_subchannel_vary_chpid(sch, *chpid, 1);
	return 0;
}

static int
__s390_vary_chpid_on(struct subchannel_id schid, void *data)
{
	struct schib schib;

	if (stsch_err(schid, &schib))
		/* We're through */
		return -ENXIO;
	/* Put it on the slow path. */
	css_schedule_eval(schid);
	return 0;
}

/**
 * chsc_chp_vary - propagate channel-path vary operation to subchannels
 * @chpid: channl-path ID
 * @on: non-zero for vary online, zero for vary offline
 */
int chsc_chp_vary(struct chp_id chpid, int on)
{
	/* Wait until previous actions have settled. */
	css_wait_for_slow_path();
	/*
	 * Redo PathVerification on the devices the chpid connects to
	 */

	if (on)
		for_each_subchannel_staged(s390_subchannel_vary_chpid_on,
					   __s390_vary_chpid_on, &chpid);
	else
		for_each_subchannel_staged(s390_subchannel_vary_chpid_off,
					   NULL, &chpid);
	return 0;
}

static void
chsc_remove_cmg_attr(struct channel_subsystem *css)
{
	int i;

	for (i = 0; i <= __MAX_CHPID; i++) {
		if (!css->chps[i])
			continue;
		chp_remove_cmg_attr(css->chps[i]);
	}
}

static int
chsc_add_cmg_attr(struct channel_subsystem *css)
{
	int i, ret;

	ret = 0;
	for (i = 0; i <= __MAX_CHPID; i++) {
		if (!css->chps[i])
			continue;
		ret = chp_add_cmg_attr(css->chps[i]);
		if (ret)
			goto cleanup;
	}
	return ret;
cleanup:
	for (--i; i >= 0; i--) {
		if (!css->chps[i])
			continue;
		chp_remove_cmg_attr(css->chps[i]);
	}
	return ret;
}

static int
__chsc_do_secm(struct channel_subsystem *css, int enable, void *page)
{
	struct {
		struct chsc_header request;
		u32 operation_code : 2;
		u32 : 30;
		u32 key : 4;
		u32 : 28;
		u32 zeroes1;
		u32 cub_addr1;
		u32 zeroes2;
		u32 cub_addr2;
		u32 reserved[13];
		struct chsc_header response;
		u32 status : 8;
		u32 : 4;
		u32 fmt : 4;
		u32 : 16;
	} *secm_area;
	int ret, ccode;

	secm_area = page;
	secm_area->request.length = 0x0050;
	secm_area->request.code = 0x0016;

	secm_area->key = PAGE_DEFAULT_KEY;
	secm_area->cub_addr1 = (u64)(unsigned long)css->cub_addr1;
	secm_area->cub_addr2 = (u64)(unsigned long)css->cub_addr2;

	secm_area->operation_code = enable ? 0 : 1;

	ccode = chsc(secm_area);
	if (ccode > 0)
		return (ccode == 3) ? -ENODEV : -EBUSY;

	switch (secm_area->response.code) {
	case 0x0001: /* Success. */
		ret = 0;
		break;
	case 0x0003: /* Invalid block. */
	case 0x0007: /* Invalid format. */
	case 0x0008: /* Other invalid block. */
		CIO_CRW_EVENT(2, "Error in chsc request block!\n");
		ret = -EINVAL;
		break;
	case 0x0004: /* Command not provided in model. */
		CIO_CRW_EVENT(2, "Model does not provide secm\n");
		ret = -EOPNOTSUPP;
		break;
	case 0x0102: /* cub adresses incorrect */
		CIO_CRW_EVENT(2, "Invalid addresses in chsc request block\n");
		ret = -EINVAL;
		break;
	case 0x0103: /* key error */
		CIO_CRW_EVENT(2, "Access key error in secm\n");
		ret = -EINVAL;
		break;
	case 0x0105: /* error while starting */
		CIO_CRW_EVENT(2, "Error while starting channel measurement\n");
		ret = -EIO;
		break;
	default:
		CIO_CRW_EVENT(2, "Unknown CHSC response %d\n",
			      secm_area->response.code);
		ret = -EIO;
	}
	return ret;
}

int
chsc_secm(struct channel_subsystem *css, int enable)
{
	void  *secm_area;
	int ret;

	secm_area = (void *)get_zeroed_page(GFP_KERNEL |  GFP_DMA);
	if (!secm_area)
		return -ENOMEM;

	if (enable && !css->cm_enabled) {
		css->cub_addr1 = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
		css->cub_addr2 = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
		if (!css->cub_addr1 || !css->cub_addr2) {
			free_page((unsigned long)css->cub_addr1);
			free_page((unsigned long)css->cub_addr2);
			free_page((unsigned long)secm_area);
			return -ENOMEM;
		}
	}
	ret = __chsc_do_secm(css, enable, secm_area);
	if (!ret) {
		css->cm_enabled = enable;
		if (css->cm_enabled) {
			ret = chsc_add_cmg_attr(css);
			if (ret) {
				memset(secm_area, 0, PAGE_SIZE);
				__chsc_do_secm(css, 0, secm_area);
				css->cm_enabled = 0;
			}
		} else
			chsc_remove_cmg_attr(css);
	}
	if (!css->cm_enabled) {
		free_page((unsigned long)css->cub_addr1);
		free_page((unsigned long)css->cub_addr2);
	}
	free_page((unsigned long)secm_area);
	return ret;
}

int chsc_determine_channel_path_description(struct chp_id chpid,
					    struct channel_path_desc *desc)
{
	int ccode, ret;

	struct {
		struct chsc_header request;
		u32 : 24;
		u32 first_chpid : 8;
		u32 : 24;
		u32 last_chpid : 8;
		u32 zeroes1;
		struct chsc_header response;
		u32 zeroes2;
		struct channel_path_desc desc;
	} *scpd_area;

	scpd_area = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!scpd_area)
		return -ENOMEM;

	scpd_area->request.length = 0x0010;
	scpd_area->request.code = 0x0002;

	scpd_area->first_chpid = chpid.id;
	scpd_area->last_chpid = chpid.id;

	ccode = chsc(scpd_area);
	if (ccode > 0) {
		ret = (ccode == 3) ? -ENODEV : -EBUSY;
		goto out;
	}

	switch (scpd_area->response.code) {
	case 0x0001: /* Success. */
		memcpy(desc, &scpd_area->desc,
		       sizeof(struct channel_path_desc));
		ret = 0;
		break;
	case 0x0003: /* Invalid block. */
	case 0x0007: /* Invalid format. */
	case 0x0008: /* Other invalid block. */
		CIO_CRW_EVENT(2, "Error in chsc request block!\n");
		ret = -EINVAL;
		break;
	case 0x0004: /* Command not provided in model. */
		CIO_CRW_EVENT(2, "Model does not provide scpd\n");
		ret = -EOPNOTSUPP;
		break;
	default:
		CIO_CRW_EVENT(2, "Unknown CHSC response %d\n",
			      scpd_area->response.code);
		ret = -EIO;
	}
out:
	free_page((unsigned long)scpd_area);
	return ret;
}

static void
chsc_initialize_cmg_chars(struct channel_path *chp, u8 cmcv,
			  struct cmg_chars *chars)
{
	switch (chp->cmg) {
	case 2:
	case 3:
		chp->cmg_chars = kmalloc(sizeof(struct cmg_chars),
					 GFP_KERNEL);
		if (chp->cmg_chars) {
			int i, mask;
			struct cmg_chars *cmg_chars;

			cmg_chars = chp->cmg_chars;
			for (i = 0; i < NR_MEASUREMENT_CHARS; i++) {
				mask = 0x80 >> (i + 3);
				if (cmcv & mask)
					cmg_chars->values[i] = chars->values[i];
				else
					cmg_chars->values[i] = 0;
			}
		}
		break;
	default:
		/* No cmg-dependent data. */
		break;
	}
}

int chsc_get_channel_measurement_chars(struct channel_path *chp)
{
	int ccode, ret;

	struct {
		struct chsc_header request;
		u32 : 24;
		u32 first_chpid : 8;
		u32 : 24;
		u32 last_chpid : 8;
		u32 zeroes1;
		struct chsc_header response;
		u32 zeroes2;
		u32 not_valid : 1;
		u32 shared : 1;
		u32 : 22;
		u32 chpid : 8;
		u32 cmcv : 5;
		u32 : 11;
		u32 cmgq : 8;
		u32 cmg : 8;
		u32 zeroes3;
		u32 data[NR_MEASUREMENT_CHARS];
	} *scmc_area;

	scmc_area = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!scmc_area)
		return -ENOMEM;

	scmc_area->request.length = 0x0010;
	scmc_area->request.code = 0x0022;

	scmc_area->first_chpid = chp->chpid.id;
	scmc_area->last_chpid = chp->chpid.id;

	ccode = chsc(scmc_area);
	if (ccode > 0) {
		ret = (ccode == 3) ? -ENODEV : -EBUSY;
		goto out;
	}

	switch (scmc_area->response.code) {
	case 0x0001: /* Success. */
		if (!scmc_area->not_valid) {
			chp->cmg = scmc_area->cmg;
			chp->shared = scmc_area->shared;
			chsc_initialize_cmg_chars(chp, scmc_area->cmcv,
						  (struct cmg_chars *)
						  &scmc_area->data);
		} else {
			chp->cmg = -1;
			chp->shared = -1;
		}
		ret = 0;
		break;
	case 0x0003: /* Invalid block. */
	case 0x0007: /* Invalid format. */
	case 0x0008: /* Invalid bit combination. */
		CIO_CRW_EVENT(2, "Error in chsc request block!\n");
		ret = -EINVAL;
		break;
	case 0x0004: /* Command not provided. */
		CIO_CRW_EVENT(2, "Model does not provide scmc\n");
		ret = -EOPNOTSUPP;
		break;
	default:
		CIO_CRW_EVENT(2, "Unknown CHSC response %d\n",
			      scmc_area->response.code);
		ret = -EIO;
	}
out:
	free_page((unsigned long)scmc_area);
	return ret;
}

static int reset_channel_path(struct channel_path *chp)
{
	int cc;

	cc = rchp(chp->chpid);
	switch (cc) {
	case 0:
		return 0;
	case 2:
		return -EBUSY;
	default:
		return -ENODEV;
	}
}

static atomic_t chpid_count;

void do_rchp_mcck(struct pt_regs *regs)
{
	struct crw crw;
	struct mci *mci;

	/* Check for pending channel report word; */
	mci = (struct mci *) &S390_lowcore.mcck_interruption_code;
	if (!mci->cp)
		return;
	/* Process channel report words. */
	while (stcrw(&crw) == 0)
		/* Count RCHP responses. */
		if (crw.slct && crw.rsc == CRW_RSC_CPATH)
			atomic_dec(&chpid_count);
}

#define RCHP_TIMEOUT	(30 * USEC_PER_SEC)

static void reset_channel_paths_css(struct channel_subsystem *css)
{
	int i;

	for (i = 0; i <= __MAX_CHPID; i++)
		if (css->chps[i])
			if (reset_channel_path(css->chps[i]) == 0)
				atomic_inc(&chpid_count);
}

extern void rchp_mcck_int_handler(void);

void cio_reset_channel_paths(void)
{
	int i;
	unsigned long long timeout;

	/* Disable lowcore protection. */
	__ctl_clear_bit(0,28);
	/* Set local machine check handler. */
	local_mcck_disable();
	S390_lowcore.mcck_new_psw.mask =
                PSW_KERNEL_BITS & ~PSW_MASK_MCHECK;
	S390_lowcore.mcck_new_psw.addr =
		PSW_ADDR_AMODE | (unsigned long) &rchp_mcck_int_handler;
	local_mcck_enable();
	/* Reset known channel paths. */
	atomic_set(&chpid_count, 0);
	for (i = 0; i <= __MAX_CSSID; i++) {
		if (css[i] && css[i]->valid)
			reset_channel_paths_css(css[i]);
	}
	/* Wait for reset acknowledgment. */
	timeout = get_clock() + (RCHP_TIMEOUT << 12);
	while (atomic_read(&chpid_count) != 0) {
		if (get_clock() > timeout)
			break;
		cpu_relax();
	}
}

static int __init
chsc_alloc_sei_area(void)
{
	sei_page = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!sei_page)
		printk(KERN_WARNING"Can't allocate page for processing of " \
		       "chsc machine checks!\n");
	return (sei_page ? 0 : -ENOMEM);
}

int __init
chsc_enable_facility(int operation_code)
{
	int ret;
	struct {
		struct chsc_header request;
		u8 reserved1:4;
		u8 format:4;
		u8 reserved2;
		u16 operation_code;
		u32 reserved3;
		u32 reserved4;
		u32 operation_data_area[252];
		struct chsc_header response;
		u32 reserved5:4;
		u32 format2:4;
		u32 reserved6:24;
	} *sda_area;

	sda_area = (void *)get_zeroed_page(GFP_KERNEL|GFP_DMA);
	if (!sda_area)
		return -ENOMEM;
	sda_area->request.length = 0x0400;
	sda_area->request.code = 0x0031;
	sda_area->operation_code = operation_code;

	ret = chsc(sda_area);
	if (ret > 0) {
		ret = (ret == 3) ? -ENODEV : -EBUSY;
		goto out;
	}
	switch (sda_area->response.code) {
	case 0x0001: /* everything ok */
		ret = 0;
		break;
	case 0x0003: /* invalid request block */
	case 0x0007:
		ret = -EINVAL;
		break;
	case 0x0004: /* command not provided */
	case 0x0101: /* facility not provided */
		ret = -EOPNOTSUPP;
		break;
	default: /* something went wrong */
		ret = -EIO;
	}
 out:
	free_page((unsigned long)sda_area);
	return ret;
}

subsys_initcall(chsc_alloc_sei_area);

struct css_general_char css_general_characteristics;
struct css_chsc_char css_chsc_characteristics;

int __init
chsc_determine_css_characteristics(void)
{
	int result;
	struct {
		struct chsc_header request;
		u32 reserved1;
		u32 reserved2;
		u32 reserved3;
		struct chsc_header response;
		u32 reserved4;
		u32 general_char[510];
		u32 chsc_char[518];
	} *scsc_area;

	scsc_area = (void *)get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!scsc_area) {
	        printk(KERN_WARNING"cio: Was not able to determine available" \
		       "CHSCs due to no memory.\n");
		return -ENOMEM;
	}

	scsc_area->request.length = 0x0010;
	scsc_area->request.code = 0x0010;

	result = chsc(scsc_area);
	if (result) {
		printk(KERN_WARNING"cio: Was not able to determine " \
		       "available CHSCs, cc=%i.\n", result);
		result = -EIO;
		goto exit;
	}

	if (scsc_area->response.code != 1) {
		printk(KERN_WARNING"cio: Was not able to determine " \
		       "available CHSCs.\n");
		result = -EIO;
		goto exit;
	}
	memcpy(&css_general_characteristics, scsc_area->general_char,
	       sizeof(css_general_characteristics));
	memcpy(&css_chsc_characteristics, scsc_area->chsc_char,
	       sizeof(css_chsc_characteristics));
exit:
	free_page ((unsigned long) scsc_area);
	return result;
}

EXPORT_SYMBOL_GPL(css_general_characteristics);
EXPORT_SYMBOL_GPL(css_chsc_characteristics);
