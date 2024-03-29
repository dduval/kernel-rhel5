/*******************************************************************
 * This file is part of the Emulex Linux Device Driver for         *
 * Fibre Channel Host Bus Adapters.                                *
 * Copyright (C) 2004-2009 Emulex.  All rights reserved.           *
 * EMULEX and SLI are trademarks of Emulex.                        *
 * www.emulex.com                                                  *
 * Portions Copyright (C) 2004-2005 Christoph Hellwig              *
 *                                                                 *
 * This program is free software; you can redistribute it and/or   *
 * modify it under the terms of version 2 of the GNU General       *
 * Public License as published by the Free Software Foundation.    *
 * This program is distributed in the hope that it will be useful. *
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND          *
 * WARRANTIES, INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,  *
 * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE      *
 * DISCLAIMED, EXCEPT TO THE EXTENT THAT SUCH DISCLAIMERS ARE HELD *
 * TO BE LEGALLY INVALID.  See the GNU General Public License for  *
 * more details, a copy of which can be found in the file COPYING  *
 * included with this package.                                     *
 *******************************************************************/

#include <linux/blkdev.h>
#include <linux/pci.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>

#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_transport_fc.h>

#include "lpfc_hw4.h"
#include "lpfc_hw.h"
#include "lpfc_nl.h"
#include "lpfc_disc.h"
#include "lpfc_sli.h"
#include "lpfc_sli4.h"
#include "lpfc_scsi.h"
#include "lpfc.h"
#include "lpfc_logmsg.h"
#include "lpfc_crtn.h"
#include "lpfc_vport.h"
#include "lpfc_debugfs.h"

/* AlpaArray for assignment of scsid for scan-down and bind_method */
static uint8_t lpfcAlpaArray[] = {
	0xEF, 0xE8, 0xE4, 0xE2, 0xE1, 0xE0, 0xDC, 0xDA, 0xD9, 0xD6,
	0xD5, 0xD4, 0xD3, 0xD2, 0xD1, 0xCE, 0xCD, 0xCC, 0xCB, 0xCA,
	0xC9, 0xC7, 0xC6, 0xC5, 0xC3, 0xBC, 0xBA, 0xB9, 0xB6, 0xB5,
	0xB4, 0xB3, 0xB2, 0xB1, 0xAE, 0xAD, 0xAC, 0xAB, 0xAA, 0xA9,
	0xA7, 0xA6, 0xA5, 0xA3, 0x9F, 0x9E, 0x9D, 0x9B, 0x98, 0x97,
	0x90, 0x8F, 0x88, 0x84, 0x82, 0x81, 0x80, 0x7C, 0x7A, 0x79,
	0x76, 0x75, 0x74, 0x73, 0x72, 0x71, 0x6E, 0x6D, 0x6C, 0x6B,
	0x6A, 0x69, 0x67, 0x66, 0x65, 0x63, 0x5C, 0x5A, 0x59, 0x56,
	0x55, 0x54, 0x53, 0x52, 0x51, 0x4E, 0x4D, 0x4C, 0x4B, 0x4A,
	0x49, 0x47, 0x46, 0x45, 0x43, 0x3C, 0x3A, 0x39, 0x36, 0x35,
	0x34, 0x33, 0x32, 0x31, 0x2E, 0x2D, 0x2C, 0x2B, 0x2A, 0x29,
	0x27, 0x26, 0x25, 0x23, 0x1F, 0x1E, 0x1D, 0x1B, 0x18, 0x17,
	0x10, 0x0F, 0x08, 0x04, 0x02, 0x01
};

extern void lpfc_check_menlo_cfg(struct lpfc_hba *phba);

static void lpfc_disc_timeout_handler(struct lpfc_vport *);
static void lpfc_unregister_fcfi_cmpl(struct lpfc_hba *, LPFC_MBOXQ_t *);

extern void lpfc_dev_loss_delay(unsigned long ptr);

void
lpfc_start_discovery(struct lpfc_vport *vport)
{
	struct lpfc_hba *phba = vport->phba;

	if (vport->auth.security_active &&
	    vport->auth.auth_state != LPFC_AUTH_SUCCESS) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0154 Authentication not complete.\n");
		return;
	}
	if (vport->port_type == LPFC_NPIV_PORT) {
		lpfc_do_scr_ns_plogi(phba, vport);
		return;
	}

	lpfc_start_fdiscs(phba);
	lpfc_do_scr_ns_plogi(phba, vport);
}

void
lpfc_terminate_rport_io(struct fc_rport *rport)
{
	struct lpfc_rport_data *rdata;
	struct lpfc_nodelist * ndlp;
	struct lpfc_hba *phba;

	rdata = rport->dd_data;
	ndlp = rdata->pnode;

	if (!ndlp || !NLP_CHK_NODE_ACT(ndlp)) {
		if (rport->roles & FC_RPORT_ROLE_FCP_TARGET)
			printk(KERN_ERR "Cannot find remote node"
			" to terminate I/O Data x%x\n",
			rport->port_id);
		return;
	}

	phba  = ndlp->phba;

	lpfc_debugfs_disc_trc(ndlp->vport, LPFC_DISC_TRC_RPORT,
		"rport terminate: sid:x%x did:x%x flg:x%x",
		ndlp->nlp_sid, ndlp->nlp_DID, ndlp->nlp_flag);

	if (ndlp->nlp_sid != NLP_NO_SID) {
		lpfc_sli_abort_iocb(ndlp->vport,
			&phba->sli.ring[phba->sli.fcp_ring],
			ndlp->nlp_sid, 0, LPFC_CTX_TGT);
	}
}

/*
 * This function will be called when dev_loss_tmo fire.
 */
void
lpfc_dev_loss_tmo_callbk(struct fc_rport *rport)
{
	struct lpfc_rport_data *rdata;
	struct lpfc_nodelist * ndlp;
	struct lpfc_vport *vport;
	struct lpfc_hba   *phba;
	struct lpfc_work_evt *evtp;
	int  put_node;
	int  put_rport;

	rdata = rport->dd_data;
	ndlp = rdata->pnode;
	if (!ndlp || !NLP_CHK_NODE_ACT(ndlp))
		return;

	vport = ndlp->vport;
	phba  = vport->phba;

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_RPORT,
		"rport devlosscb: sid:x%x did:x%x flg:x%x",
		ndlp->nlp_sid, ndlp->nlp_DID, ndlp->nlp_flag);

	/* Don't defer this if we are in the process of deleting the vport
	 * or unloading the driver. The unload will cleanup the node
	 * appropriately we just need to cleanup the ndlp rport info here.
	 */
	if (vport->load_flag & FC_UNLOADING) {
		put_node = rdata->pnode != NULL;
		put_rport = ndlp->rport != NULL;
		rdata->pnode = NULL;
		ndlp->rport = NULL;
		if (put_node)
			lpfc_nlp_put(ndlp);
		if (put_rport)
			put_device(&rport->dev);
		return;
	}

	if (ndlp->nlp_state == NLP_STE_MAPPED_NODE)
		return;

	evtp = &ndlp->dev_loss_evt;

	if (!list_empty(&evtp->evt_listp))
		return;

	spin_lock_irq(&phba->hbalock);
	/* We need to hold the node by incrementing the reference
	 * count until this queued work is done
	 */
	evtp->evt_arg1 = lpfc_nlp_get(ndlp);
	if (evtp->evt_arg1) {
		evtp->evt = LPFC_EVT_DEV_LOSS;
		list_add_tail(&evtp->evt_listp, &phba->work_list);
		lpfc_worker_wake_up(phba);
	}
	spin_unlock_irq(&phba->hbalock);

	return;
}

/*
 * This function is called from the worker thread when dev_loss_tmo
 * expire.
 */
void
lpfc_dev_loss_tmo_handler(struct lpfc_nodelist *ndlp)
{
	struct lpfc_rport_data *rdata;
	struct fc_rport   *rport;
	struct lpfc_vport *vport;
	struct lpfc_hba   *phba;
	uint8_t *name;
	int  put_node;
	int  put_rport;
	int warn_on = 0;

	rport = ndlp->rport;

	if (!rport)
		return;

	rdata = rport->dd_data;
	name = (uint8_t *) &ndlp->nlp_portname;
	vport = ndlp->vport;
	phba  = vport->phba;

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_RPORT,
		"rport devlosstmo:did:x%x type:x%x id:x%x",
		ndlp->nlp_DID, ndlp->nlp_type, rport->scsi_target_id);

	/* Don't defer this if we are in the process of deleting the vport
	 * or unloading the driver. The unload will cleanup the node
	 * appropriately we just need to cleanup the ndlp rport info here.
	 */
	if (vport->load_flag & FC_UNLOADING) {
		if (ndlp->nlp_sid != NLP_NO_SID) {
			/* flush the target */
			lpfc_sli_abort_iocb(vport,
					&phba->sli.ring[phba->sli.fcp_ring],
					ndlp->nlp_sid, 0, LPFC_CTX_TGT);
		}
		put_node = rdata->pnode != NULL;
		put_rport = ndlp->rport != NULL;
		rdata->pnode = NULL;
		ndlp->rport = NULL;
		if (put_node)
			lpfc_nlp_put(ndlp);
		if (put_rport)
			put_device(&rport->dev);
		return;
	}

	if (ndlp->nlp_state == NLP_STE_MAPPED_NODE) {
		lpfc_printf_vlog(vport, KERN_INFO, LOG_DISCOVERY,
				 "0284 Devloss timeout Ignored on "
				 "WWPN %x:%x:%x:%x:%x:%x:%x:%x "
				 "NPort x%x\n",
				 *name, *(name+1), *(name+2), *(name+3),
				 *(name+4), *(name+5), *(name+6), *(name+7),
				 ndlp->nlp_DID);
		return;
	}
	/*
	 * This is a workaround for an issue seen in the transport. If the
	 * driver removes, then adds, and then removes the rport again whithin
	 * devloss_tmo the transport may call devloss_tmo for the original
	 * remove even though it should have reset the timer.
	 */
	if (ndlp->nlp_sid != NLP_NO_SID &&
	    time_after(ndlp->unreg_time +
		       msecs_to_jiffies(vport->cfg_devloss_tmo * 1000),
		       jiffies)) {
		lpfc_printf_vlog(vport, KERN_WARNING, LOG_DISCOVERY,
				 "0151 Devloss timeout Skipped on "
				 "WWPN %x:%x:%x:%x:%x:%x:%x:%x "
				 "NPort x%x Time %d\n",
				 *name, *(name+1), *(name+2), *(name+3),
				 *(name+4), *(name+5), *(name+6), *(name+7),
				 ndlp->nlp_DID,
				 jiffies_to_msecs(jiffies-ndlp->unreg_time));
		return;
	}

	if (ndlp->nlp_type & NLP_FABRIC) {
		/* We will clean up these Nodes in linkup */
		put_node = rdata->pnode != NULL;
		put_rport = ndlp->rport != NULL;
		rdata->pnode = NULL;
		ndlp->rport = NULL;
		if (put_node)
			lpfc_nlp_put(ndlp);
		if (put_rport)
			put_device(&rport->dev);
		return;
	}

	if (!phba->cfg_dev_loss_initiator && rport->scsi_target_id == -1) {
		/*
		 * Until scsi_transport_fc provides support of timing out remote
		 * initiator ports, provide a parameter to prevent initiator
		 * traffic for CT or IP from failing until dev_loss timeout
		 * expires.
		 * This behavior is keyed off the lpfc_dev_loss_initiator
		 * config parameter.
		 */
		put_node = rdata->pnode != NULL;
		put_rport = ndlp->rport != NULL;
		rdata->pnode = NULL;
		ndlp->rport = NULL;
		if (put_node)
			lpfc_nlp_put(ndlp);
		if (put_rport)
			put_device(&rport->dev);
		mod_timer(&ndlp->nlp_initiator_tmr,
			  jiffies + HZ * vport->cfg_devloss_tmo);
		return;
	}

	if (ndlp->nlp_sid != NLP_NO_SID) {
		warn_on = 1;
		/* flush the target */
		lpfc_sli_abort_iocb(vport, &phba->sli.ring[phba->sli.fcp_ring],
				    ndlp->nlp_sid, 0, LPFC_CTX_TGT);
	}

	if (warn_on) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0203 Devloss timeout on "
				 "WWPN %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x "
				 "NPort x%06x Data: x%x x%x x%x\n",
				 *name, *(name+1), *(name+2), *(name+3),
				 *(name+4), *(name+5), *(name+6), *(name+7),
				 ndlp->nlp_DID, ndlp->nlp_flag,
				 ndlp->nlp_state, ndlp->nlp_rpi);
	} else {
		lpfc_printf_vlog(vport, KERN_INFO, LOG_DISCOVERY,
				 "0204 Devloss timeout on "
				 "WWPN %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x "
				 "NPort x%06x Data: x%x x%x x%x\n",
				 *name, *(name+1), *(name+2), *(name+3),
				 *(name+4), *(name+5), *(name+6), *(name+7),
				 ndlp->nlp_DID, ndlp->nlp_flag,
				 ndlp->nlp_state, ndlp->nlp_rpi);
	}

	put_node = rdata->pnode != NULL;
	put_rport = ndlp->rport != NULL;
	rdata->pnode = NULL;
	ndlp->rport = NULL;
	if (put_node)
		lpfc_nlp_put(ndlp);
	if (put_rport)
		put_device(&rport->dev);

	if (!(vport->load_flag & FC_UNLOADING) &&
	    !(ndlp->nlp_flag & NLP_DELAY_TMO) &&
	    !(ndlp->nlp_flag & NLP_NPR_2B_DISC) &&
	    (ndlp->nlp_state != NLP_STE_UNMAPPED_NODE))
		lpfc_disc_state_machine(vport, ndlp, NULL, NLP_EVT_DEVICE_RM);

	lpfc_unregister_unused_fcf(phba);
}

/**
 * lpfc_alloc_fast_evt - Allocates data structure for posting event
 * @phba: Pointer to hba context object.
 *
 * This function is called from the functions which need to post
 * events from interrupt context. This function allocates data
 * structure required for posting event. It also keeps track of
 * number of events pending and prevent event storm when there are
 * too many events.
 **/
struct lpfc_fast_path_event *
lpfc_alloc_fast_evt(struct lpfc_hba *phba) {
	struct lpfc_fast_path_event *ret;

	/* If there are lot of fast event do not exhaust memory due to this */
	if (atomic_read(&phba->fast_event_count) > LPFC_MAX_EVT_COUNT)
		return NULL;

	ret = kzalloc(sizeof(struct lpfc_fast_path_event),
			GFP_ATOMIC);
	if (ret) {
		atomic_inc(&phba->fast_event_count);
		INIT_LIST_HEAD(&ret->work_evt.evt_listp);
		ret->work_evt.evt = LPFC_EVT_FASTPATH_MGMT_EVT;
	}
	return ret;
}

/**
 * lpfc_free_fast_evt - Frees event data structure
 * @phba: Pointer to hba context object.
 * @evt:  Event object which need to be freed.
 *
 * This function frees the data structure required for posting
 * events.
 **/
void
lpfc_free_fast_evt(struct lpfc_hba *phba,
		struct lpfc_fast_path_event *evt) {

	atomic_dec(&phba->fast_event_count);
	kfree(evt);
}

/**
 * lpfc_send_fastpath_evt - Posts events generated from fast path
 * @phba: Pointer to hba context object.
 * @evtp: Event data structure.
 *
 * This function is called from worker thread, when the interrupt
 * context need to post an event. This function posts the event
 * to fc transport netlink interface.
 **/
static void
lpfc_send_fastpath_evt(struct lpfc_hba *phba,
		struct lpfc_work_evt *evtp)
{
	unsigned long evt_category, evt_sub_category;
	struct lpfc_fast_path_event *fast_evt_data;
	char *evt_data;
	uint32_t evt_data_size;
	struct Scsi_Host *shost;

	fast_evt_data = container_of(evtp, struct lpfc_fast_path_event,
		work_evt);

	evt_category = (unsigned long) fast_evt_data->un.fabric_evt.event_type;
	evt_sub_category = (unsigned long) fast_evt_data->un.
			fabric_evt.subcategory;
	shost = lpfc_shost_from_vport(fast_evt_data->vport);
	if (evt_category == FC_REG_FABRIC_EVENT) {
		if (evt_sub_category == LPFC_EVENT_FCPRDCHKERR) {
			evt_data = (char *) &fast_evt_data->un.read_check_error;
			evt_data_size = sizeof(fast_evt_data->un.
				read_check_error);
		} else if ((evt_sub_category == LPFC_EVENT_FABRIC_BUSY) ||
			(evt_sub_category == LPFC_EVENT_PORT_BUSY)) {
			evt_data = (char *) &fast_evt_data->un.fabric_evt;
			evt_data_size = sizeof(fast_evt_data->un.fabric_evt);
		} else {
			lpfc_free_fast_evt(phba, fast_evt_data);
			return;
		}
	} else if (evt_category == FC_REG_SCSI_EVENT) {
		switch (evt_sub_category) {
		case LPFC_EVENT_QFULL:
		case LPFC_EVENT_DEVBSY:
			evt_data = (char *) &fast_evt_data->un.scsi_evt;
			evt_data_size = sizeof(fast_evt_data->un.scsi_evt);
			break;
		case LPFC_EVENT_CHECK_COND:
			evt_data = (char *) &fast_evt_data->un.check_cond_evt;
			evt_data_size =  sizeof(fast_evt_data->un.
				check_cond_evt);
			break;
		case LPFC_EVENT_VARQUEDEPTH:
			evt_data = (char *) &fast_evt_data->un.queue_depth_evt;
			evt_data_size = sizeof(fast_evt_data->un.
				queue_depth_evt);
			break;
		default:
			lpfc_free_fast_evt(phba, fast_evt_data);
			return;
		}
	} else {
		lpfc_free_fast_evt(phba, fast_evt_data);
		return;
	}

	fc_host_post_vendor_event(shost,
		fc_get_event_number(),
		evt_data_size,
		evt_data,
		LPFC_NL_VENDOR_ID);

	lpfc_free_fast_evt(phba, fast_evt_data);
	return;
}

static void
lpfc_work_list_done(struct lpfc_hba *phba)
{
	struct lpfc_work_evt  *evtp = NULL;
	struct lpfc_nodelist  *ndlp;
	struct lpfc_vport     *vport;
	int free_evt;

	spin_lock_irq(&phba->hbalock);
	while (!list_empty(&phba->work_list)) {
		list_remove_head((&phba->work_list), evtp, typeof(*evtp),
				 evt_listp);
		spin_unlock_irq(&phba->hbalock);
		free_evt = 1;
		switch (evtp->evt) {
		case LPFC_EVT_DEV_LOSS_DELAY:
			free_evt = 0; /* evt is part of ndlp */
			ndlp = (struct lpfc_nodelist *) (evtp->evt_arg1);
			vport = ndlp->vport;
			if (!vport) {
				/* decrement the node reference count held
				 * for this queued work
				 */
				lpfc_nlp_put(ndlp);
				break;
			}
			lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_RPORT,
				"rport devlossdly:did:x%x flg:x%x",
				ndlp->nlp_DID, ndlp->nlp_flag, 0);

			if (!(vport->load_flag & FC_UNLOADING) &&
			    !(ndlp->nlp_flag & NLP_DELAY_TMO) &&
			    !(ndlp->nlp_flag & NLP_NPR_2B_DISC) &&
			    (ndlp->nlp_state != NLP_STE_UNMAPPED_NODE)) {
				lpfc_disc_state_machine(vport, ndlp, NULL,
					NLP_EVT_DEVICE_RM);
			}
			/* decrement the node reference count held
			 * for this queued work
			 */
			lpfc_nlp_put(ndlp);
			break;
		case LPFC_EVT_ELS_RETRY:
			ndlp = (struct lpfc_nodelist *) (evtp->evt_arg1);
			lpfc_els_retry_delay_handler(ndlp);
			free_evt = 0; /* evt is part of ndlp */
			/* decrement the node reference count held
			 * for this queued work
			 */
			lpfc_nlp_put(ndlp);
			break;
		case LPFC_EVT_REAUTH:
			ndlp = (struct lpfc_nodelist *) (evtp->evt_arg1);
			lpfc_reauthentication_handler(ndlp);
			free_evt = 0; /* evt is part of ndlp */
			/* decrement the node reference count held
			 * for this queued work
			 */
			lpfc_nlp_put(ndlp);
			break;
		case LPFC_EVT_DEV_LOSS:
			ndlp = (struct lpfc_nodelist *)(evtp->evt_arg1);
			lpfc_dev_loss_tmo_handler(ndlp);
			free_evt = 0;
			/* decrement the node reference count held for
			 * this queued work
			 */
			lpfc_nlp_put(ndlp);
			break;
		case LPFC_EVT_ONLINE:
			if (phba->link_state < LPFC_LINK_DOWN)
				*(int *) (evtp->evt_arg1) = lpfc_online(phba);
			else
				*(int *) (evtp->evt_arg1) = 0;
			complete((struct completion *)(evtp->evt_arg2));
			break;
		case LPFC_EVT_OFFLINE_PREP:
			if (phba->link_state >= LPFC_LINK_DOWN)
				lpfc_offline_prep(phba);
			*(int *)(evtp->evt_arg1) = 0;
			complete((struct completion *)(evtp->evt_arg2));
			break;
		case LPFC_EVT_OFFLINE:
			lpfc_offline(phba);
			lpfc_sli_brdrestart(phba);
			*(int *)(evtp->evt_arg1) =
				lpfc_sli_brdready(phba, HS_FFRDY | HS_MBRDY);
			lpfc_unblock_mgmt_io(phba);
			complete((struct completion *)(evtp->evt_arg2));
			break;
		case LPFC_EVT_WARM_START:
			lpfc_offline(phba);
			lpfc_reset_barrier(phba);
			lpfc_sli_brdreset(phba);
			lpfc_hba_down_post(phba);
			*(int *)(evtp->evt_arg1) =
				lpfc_sli_brdready(phba, HS_MBRDY);
			lpfc_unblock_mgmt_io(phba);
			complete((struct completion *)(evtp->evt_arg2));
			break;
		case LPFC_EVT_KILL:
			lpfc_offline(phba);
			*(int *)(evtp->evt_arg1)
				= (phba->pport->stopped)
				        ? 0 : lpfc_sli_brdkill(phba);
			lpfc_unblock_mgmt_io(phba);
			complete((struct completion *)(evtp->evt_arg2));
			break;
		case LPFC_EVT_FASTPATH_MGMT_EVT:
			lpfc_send_fastpath_evt(phba, evtp);
			free_evt = 0;
			break;
		}
		if (free_evt)
			kfree(evtp);
		spin_lock_irq(&phba->hbalock);
	}
	spin_unlock_irq(&phba->hbalock);

}

static void
lpfc_work_done(struct lpfc_hba *phba)
{
	struct lpfc_sli_ring *pring;
	uint32_t ha_copy, status, control, work_port_events;
	struct lpfc_vport **vports;
	struct lpfc_vport *vport;
	int i;

	spin_lock_irq(&phba->hbalock);
	ha_copy = phba->work_ha;
	phba->work_ha = 0;
	spin_unlock_irq(&phba->hbalock);

	/* First, try to post the next mailbox command to SLI4 device */
	if (phba->pci_dev_grp == LPFC_PCI_DEV_OC)
		lpfc_sli4_post_async_mbox(phba);

	if (ha_copy & HA_ERATT)
		/* Handle the error attention event */
		lpfc_handle_eratt(phba);

	if (ha_copy & HA_MBATT)
		lpfc_sli_handle_mb_event(phba);

	if (ha_copy & HA_LATT)
		lpfc_handle_latt(phba);

	/* Process SLI4 events */
	if (phba->pci_dev_grp == LPFC_PCI_DEV_OC) {
		if (phba->hba_flag & FCP_XRI_ABORT_EVENT)
			lpfc_sli4_fcp_xri_abort_event_proc(phba);
		if (phba->hba_flag & ELS_XRI_ABORT_EVENT)
			lpfc_sli4_els_xri_abort_event_proc(phba);
		if (phba->hba_flag & ASYNC_EVENT)
			lpfc_sli4_async_event_proc(phba);
		if (phba->hba_flag & HBA_POST_RECEIVE_BUFFER) {
			spin_lock_irq(&phba->hbalock);
			phba->hba_flag &= ~HBA_POST_RECEIVE_BUFFER;
			spin_unlock_irq(&phba->hbalock);
			lpfc_sli_hbqbuf_add_hbqs(phba, LPFC_ELS_HBQ);
		}
	}

	vports = lpfc_create_vport_work_array(phba);
	if (vports != NULL)
		for (i = 0; i <= phba->max_vports; i++) {
			/*
			 * We could have no vports in array if unloading, so if
			 * this happens then just use the pport
			 */
			if (vports[i] == NULL && i == 0)
				vport = phba->pport;
			else
				vport = vports[i];
			if (vport == NULL)
				break;
			spin_lock_irq(&vport->work_port_lock);
			work_port_events = vport->work_port_events;
			vport->work_port_events &= ~work_port_events;
			spin_unlock_irq(&vport->work_port_lock);
			if (work_port_events & WORKER_DISC_TMO)
				lpfc_disc_timeout_handler(vport);
			if (work_port_events & WORKER_ELS_TMO)
				lpfc_els_timeout_handler(vport);
			if (work_port_events & WORKER_HB_TMO)
				lpfc_hb_timeout_handler(phba);
			if (work_port_events & WORKER_MBOX_TMO)
				lpfc_mbox_timeout_handler(phba);
			if (work_port_events & WORKER_FABRIC_BLOCK_TMO)
				lpfc_unblock_fabric_iocbs(phba);
			if (work_port_events & WORKER_FDMI_TMO)
				lpfc_fdmi_timeout_handler(vport);
			if (work_port_events & WORKER_RAMP_DOWN_QUEUE)
				lpfc_ramp_down_queue_handler(phba);
			if (work_port_events & WORKER_RAMP_UP_QUEUE)
				lpfc_ramp_up_queue_handler(phba);
		}
	lpfc_destroy_vport_work_array(phba, vports);

	pring = &phba->sli.ring[LPFC_ELS_RING];
	status = (ha_copy & (HA_RXMASK  << (4*LPFC_ELS_RING)));
	status >>= (4*LPFC_ELS_RING);
	if ((status & HA_RXMASK) ||
	    (pring->flag & LPFC_DEFERRED_RING_EVENT) ||
	    (phba->hba_flag & HBA_SP_QUEUE_EVT)) {
		if (pring->flag & LPFC_STOP_IOCB_EVENT) {
			pring->flag |= LPFC_DEFERRED_RING_EVENT;
			/* Set the lpfc data pending flag */
			set_bit(LPFC_DATA_READY, &phba->data_flags);
		} else {
			pring->flag &= ~LPFC_DEFERRED_RING_EVENT;
			lpfc_sli_handle_slow_ring_event(phba, pring,
							(status &
							 HA_RXMASK));
		}
		/*
		 * Turn on Ring interrupts
		 */
		if (phba->sli_rev <= LPFC_SLI_REV3) {
			spin_lock_irq(&phba->hbalock);
			control = readl(phba->HCregaddr);
			if (!(control & (HC_R0INT_ENA << LPFC_ELS_RING))) {
				lpfc_debugfs_slow_ring_trc(phba,
					"WRK Enable ring: cntl:x%x hacopy:x%x",
					control, ha_copy, 0);

				control |= (HC_R0INT_ENA << LPFC_ELS_RING);
				writel(control, phba->HCregaddr);
				readl(phba->HCregaddr); /* flush */
			} else {
				lpfc_debugfs_slow_ring_trc(phba,
					"WRK Ring ok:     cntl:x%x hacopy:x%x",
					control, ha_copy, 0);
			}
			spin_unlock_irq(&phba->hbalock);
		}
	}
	lpfc_work_list_done(phba);
}

int
lpfc_do_work(void *p)
{
	struct lpfc_hba *phba = p;
	int rc;

	set_user_nice(current, -20);
	phba->data_flags = 0;

	while (!kthread_should_stop()) {
		/* wait and check worker queue activities */
		rc = wait_event_interruptible(phba->work_waitq,
					(test_and_clear_bit(LPFC_DATA_READY,
							    &phba->data_flags)
					 || kthread_should_stop()));
		/* Signal wakeup shall terminate the worker thread */
		if (rc) {
			lpfc_printf_log(phba, KERN_ERR, LOG_ELS,
					"0433 Wakeup on signal: rc=x%x\n", rc);
			break;
		}

		/* Attend pending lpfc data processing */
		lpfc_work_done(phba);
	}
	phba->worker_thread = NULL;
	lpfc_printf_log(phba, KERN_INFO, LOG_ELS,
			"0432 Worker thread stopped.\n");
	return 0;
}

/*
 * This is only called to handle FC worker events. Since this a rare
 * occurance, we allocate a struct lpfc_work_evt structure here instead of
 * embedding it in the IOCB.
 */
int
lpfc_workq_post_event(struct lpfc_hba *phba, void *arg1, void *arg2,
		      uint32_t evt)
{
	struct lpfc_work_evt  *evtp;
	unsigned long flags;

	/*
	 * All Mailbox completions and LPFC_ELS_RING rcv ring IOCB events will
	 * be queued to worker thread for processing
	 */
	evtp = kmalloc(sizeof(struct lpfc_work_evt), GFP_ATOMIC);
	if (!evtp)
		return 0;

	evtp->evt_arg1  = arg1;
	evtp->evt_arg2  = arg2;
	evtp->evt       = evt;

	spin_lock_irqsave(&phba->hbalock, flags);
	list_add_tail(&evtp->evt_listp, &phba->work_list);
	spin_unlock_irqrestore(&phba->hbalock, flags);

	lpfc_worker_wake_up(phba);

	return 1;
}

void
lpfc_cleanup_rpis(struct lpfc_vport *vport, int remove)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_hba  *phba = vport->phba;
	struct lpfc_nodelist *ndlp, *next_ndlp;
	int  rc;

	list_for_each_entry_safe(ndlp, next_ndlp, &vport->fc_nodes, nlp_listp) {
		if (!NLP_CHK_NODE_ACT(ndlp))
			continue;
		if (ndlp->nlp_state == NLP_STE_UNUSED_NODE)
			continue;
		/* Stop re-authentication timer of all nodes. */
		del_timer_sync(&ndlp->nlp_reauth_tmr);

		if ((phba->sli3_options & LPFC_SLI3_VPORT_TEARDOWN) ||
			((vport->port_type == LPFC_NPIV_PORT) &&
			(ndlp->nlp_DID == NameServer_DID)))
			lpfc_unreg_rpi(vport, ndlp);

		/* Leave Fabric nodes alone on link down */
		if ((phba->sli_rev < LPFC_SLI_REV4) &&
		    (!remove && ndlp->nlp_type & NLP_FABRIC))
			continue;
		rc = lpfc_disc_state_machine(vport, ndlp, NULL,
					     remove
					     ? NLP_EVT_DEVICE_RM
					     : NLP_EVT_DEVICE_RECOVERY);
	}
	if (phba->sli3_options & LPFC_SLI3_VPORT_TEARDOWN) {
		rc = lpfc_mbx_unreg_vpi(vport);
		spin_lock_irq(shost->host_lock);
		vport->fc_flag |= FC_VPORT_NEEDS_REG_VPI;
		spin_unlock_irq(shost->host_lock);
	}
}

/*
 * This function can be called due to physical link failure
 * or link authentication failure.
 */
void
lpfc_port_link_failure(struct lpfc_vport *vport)
{
	/* Cleanup any outstanding received buffers */
	lpfc_cleanup_rcv_buffers(vport);

	/* Cleanup any outstanding RSCN activity */
	lpfc_els_flush_rscn(vport);

	/* Cleanup any outstanding ELS commands */
	lpfc_els_flush_cmd(vport);

	lpfc_cleanup_rpis(vport, 0);

	/* Turn off discovery timer if its running */
	lpfc_can_disctmo(vport);
}

void
lpfc_linkdown_port(struct lpfc_vport *vport)
{
	struct Scsi_Host  *shost = lpfc_shost_from_vport(vport);

	fc_host_post_event(shost, fc_get_event_number(), FCH_EVT_LINKDOWN, 0);

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_ELS_CMD,
		"Link Down:       state:x%x rtry:x%x flg:x%x",
		vport->port_state, vport->fc_ns_retry, vport->fc_flag);

	lpfc_port_link_failure(vport);

	vport->auth.auth_state = LPFC_AUTH_UNKNOWN;
	vport->auth.auth_msg_state = LPFC_AUTH_NONE;
}

void
lpfc_port_auth_failed(struct lpfc_nodelist *ndlp)
{
	struct lpfc_vport *vport = ndlp->vport;

	vport->auth.auth_state = LPFC_AUTH_FAIL;
	vport->auth.auth_msg_state = LPFC_AUTH_NONE;
	lpfc_nlp_set_state(vport, ndlp, NLP_STE_NPR_NODE);
	if (ndlp->nlp_type & NLP_FABRIC) {
		lpfc_port_link_failure (vport);
		lpfc_vport_set_state(vport, FC_VPORT_FAILED);
		lpfc_issue_els_logo(vport, ndlp, 0);
	}
}

int
lpfc_linkdown(struct lpfc_hba *phba)
{
	struct lpfc_vport *vport = phba->pport;
	struct Scsi_Host  *shost = lpfc_shost_from_vport(vport);
	struct lpfc_vport **vports;
	LPFC_MBOXQ_t          *mb;
	int i;

	if (phba->link_state == LPFC_LINK_DOWN)
		return 0;

	/* Block all SCSI stack I/Os */
	lpfc_scsi_dev_block(phba);

	spin_lock_irq(&phba->hbalock);
	phba->fcf.fcf_flag &= ~(FCF_AVAILABLE | FCF_DISCOVERED);
	if (phba->link_state > LPFC_LINK_DOWN) {
		phba->link_state = LPFC_LINK_DOWN;
		phba->pport->fc_flag &= ~FC_LBIT;
	}
	spin_unlock_irq(&phba->hbalock);
	vports = lpfc_create_vport_work_array(phba);
	if (vports != NULL)
		for (i = 0; i <= phba->max_vports && vports[i] != NULL; i++) {
			/* Issue a LINK DOWN event to all nodes */
			lpfc_linkdown_port(vports[i]);
		}
	lpfc_destroy_vport_work_array(phba, vports);
	/* Clean up any firmware default rpi's */
	mb = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (mb) {
		lpfc_unreg_did(phba, 0xffff, 0xffffffff, mb);
		mb->vport = vport;
		mb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		if (lpfc_sli_issue_mbox(phba, mb, MBX_NOWAIT)
		    == MBX_NOT_FINISHED) {
			mempool_free(mb, phba->mbox_mem_pool);
		}
	}

	/* Setup myDID for link up if we are in pt2pt mode */
	if (phba->pport->fc_flag & FC_PT2PT) {
		phba->pport->fc_myDID = 0;
		mb = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
		if (mb) {
			lpfc_config_link(phba, mb);
			mb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
			mb->vport = vport;
			if (lpfc_sli_issue_mbox(phba, mb, MBX_NOWAIT)
			    == MBX_NOT_FINISHED) {
				mempool_free(mb, phba->mbox_mem_pool);
			}
		}
		spin_lock_irq(shost->host_lock);
		phba->pport->fc_flag &= ~(FC_PT2PT | FC_PT2PT_PLOGI);
		spin_unlock_irq(shost->host_lock);
	}

	return 0;
}

static void
lpfc_linkup_cleanup_nodes(struct lpfc_vport *vport)
{
	struct lpfc_nodelist *ndlp;

	list_for_each_entry(ndlp, &vport->fc_nodes, nlp_listp) {
		if (!NLP_CHK_NODE_ACT(ndlp))
			continue;
		if (ndlp->nlp_state == NLP_STE_UNUSED_NODE)
			continue;
		if (ndlp->nlp_type & NLP_FABRIC) {
			/* On Linkup its safe to clean up the ndlp
			 * from Fabric connections.
			 */
			if (ndlp->nlp_DID != Fabric_DID)
				lpfc_unreg_rpi(vport, ndlp);
			lpfc_nlp_set_state(vport, ndlp, NLP_STE_NPR_NODE);
		} else if (!(ndlp->nlp_flag & NLP_NPR_ADISC)) {
			/* Fail outstanding IO now since device is
			 * marked for PLOGI.
			 */
			lpfc_unreg_rpi(vport, ndlp);
		}
	}
}

static void
lpfc_linkup_port(struct lpfc_vport *vport)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_hba  *phba = vport->phba;

	if ((vport->load_flag & FC_UNLOADING) != 0)
		return;

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_ELS_CMD,
		"Link Up:         top:x%x speed:x%x flg:x%x",
		phba->fc_topology, phba->fc_linkspeed, phba->link_flag);

	/* If NPIV is not enabled, only bring the physical port up */
	if (!(phba->sli3_options & LPFC_SLI3_NPIV_ENABLED) &&
		(vport != phba->pport))
		return;

	fc_host_post_event(shost, fc_get_event_number(), FCH_EVT_LINKUP, 0);

	spin_lock_irq(shost->host_lock);
	vport->fc_flag &= ~(FC_PT2PT | FC_PT2PT_PLOGI | FC_ABORT_DISCOVERY |
			    FC_RSCN_MODE | FC_NLP_MORE | FC_RSCN_DISCOVERY);
	vport->fc_flag |= FC_NDISC_ACTIVE;
	vport->fc_ns_retry = 0;
	spin_unlock_irq(shost->host_lock);

	if (vport->fc_flag & FC_LBIT)
		lpfc_linkup_cleanup_nodes(vport);

}

static int
lpfc_linkup(struct lpfc_hba *phba)
{
	struct lpfc_vport **vports;
	int i;

	phba->link_state = LPFC_LINK_UP;

	/* Unblock fabric iocbs if they are blocked */
	clear_bit(FABRIC_COMANDS_BLOCKED, &phba->bit_flags);
	del_timer_sync(&phba->fabric_block_timer);

	vports = lpfc_create_vport_work_array(phba);
	if (vports != NULL)
		for (i = 0; i <= phba->max_vports && vports[i] != NULL; i++)
			lpfc_linkup_port(vports[i]);
	lpfc_destroy_vport_work_array(phba, vports);
	if ((phba->sli3_options & LPFC_SLI3_NPIV_ENABLED) &&
	    (phba->sli_rev < LPFC_SLI_REV4))
		lpfc_issue_clear_la(phba, phba->pport);

	return 0;
}

/*
 * This routine handles processing a CLEAR_LA mailbox
 * command upon completion. It is setup in the LPFC_MBOXQ
 * as the completion routine when the command is
 * handed off to the SLI layer.
 */
void
lpfc_mbx_cmpl_clear_la(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_vport *vport = pmb->vport;
	struct Scsi_Host  *shost = lpfc_shost_from_vport(vport);
	struct lpfc_sli   *psli = &phba->sli;
	MAILBOX_t *mb = &pmb->u.mb;
	uint32_t control;

	/* Since we don't do discovery right now, turn these off here */
	psli->ring[psli->extra_ring].flag &= ~LPFC_STOP_IOCB_EVENT;
	psli->ring[psli->fcp_ring].flag &= ~LPFC_STOP_IOCB_EVENT;
	psli->ring[psli->next_ring].flag &= ~LPFC_STOP_IOCB_EVENT;

	/* Check for error */
	if ((mb->mbxStatus) && (mb->mbxStatus != 0x1601)) {
		/* CLEAR_LA mbox error <mbxStatus> state <hba_state> */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
				 "0320 CLEAR_LA mbxStatus error x%x hba "
				 "state x%x\n",
				 mb->mbxStatus, vport->port_state);
		phba->link_state = LPFC_HBA_ERROR;
		goto out;
	}

	if (vport->port_type == LPFC_PHYSICAL_PORT)
		phba->link_state = LPFC_HBA_READY;

	spin_lock_irq(&phba->hbalock);
	psli->sli_flag |= LPFC_PROCESS_LA;
	control = readl(phba->HCregaddr);
	control |= HC_LAINT_ENA;
	writel(control, phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */
	spin_unlock_irq(&phba->hbalock);
	mempool_free(pmb, phba->mbox_mem_pool);
	return;

out:
	/* Device Discovery completes */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_DISCOVERY,
			 "0225 Device Discovery completes\n");
	mempool_free(pmb, phba->mbox_mem_pool);

	spin_lock_irq(shost->host_lock);
	vport->fc_flag &= ~FC_ABORT_DISCOVERY;
	spin_unlock_irq(shost->host_lock);

	lpfc_can_disctmo(vport);

	/* turn on Link Attention interrupts */

	spin_lock_irq(&phba->hbalock);
	psli->sli_flag |= LPFC_PROCESS_LA;
	control = readl(phba->HCregaddr);
	control |= HC_LAINT_ENA;
	writel(control, phba->HCregaddr);
	readl(phba->HCregaddr); /* flush */
	spin_unlock_irq(&phba->hbalock);

	return;
}


static void
lpfc_mbx_cmpl_local_config_link(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_vport *vport = pmb->vport;

	if (pmb->u.mb.mbxStatus)
		goto out;

	mempool_free(pmb, phba->mbox_mem_pool);

	if (phba->fc_topology == TOPOLOGY_LOOP &&
	    vport->fc_flag & FC_PUBLIC_LOOP &&
	    !(vport->fc_flag & FC_LBIT)) {
			/* Need to wait for FAN - use discovery timer
			 * for timeout.  port_state is identically
			 * LPFC_LOCAL_CFG_LINK while waiting for FAN
			 */
			lpfc_set_disctmo(vport);
			return;
	}

	/* Start discovery by sending a FLOGI. port_state is identically
	 * LPFC_FLOGI while waiting for FLOGI cmpl
	 */
	if (vport->port_state != LPFC_FLOGI) {
		lpfc_initial_flogi(vport);
	}
	return;

out:
	lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
			 "0306 CONFIG_LINK mbxStatus error x%x "
			 "HBA state x%x\n",
			 pmb->u.mb.mbxStatus, vport->port_state);
	mempool_free(pmb, phba->mbox_mem_pool);

	lpfc_linkdown(phba);

	lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
			 "0200 CONFIG_LINK bad hba state x%x\n",
			 vport->port_state);

	lpfc_issue_clear_la(phba, vport);
	return;
}

static void
lpfc_mbx_cmpl_reg_fcfi(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	struct lpfc_vport *vport = mboxq->vport;
	unsigned long flags;

	if (mboxq->u.mb.mbxStatus) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
			 "2017 REG_FCFI mbxStatus error x%x "
			 "HBA state x%x\n",
			 mboxq->u.mb.mbxStatus, vport->port_state);
		mempool_free(mboxq, phba->mbox_mem_pool);
		return;
	}

	/* Start FCoE discovery by sending a FLOGI. */
	phba->fcf.fcfi = bf_get(lpfc_reg_fcfi_fcfi, &mboxq->u.mqe.un.reg_fcfi);
	/* Set the FCFI registered flag */
	spin_lock_irqsave(&phba->hbalock, flags);
	phba->fcf.fcf_flag |= FCF_REGISTERED;
	spin_unlock_irqrestore(&phba->hbalock, flags);
	/* If there is a pending FCoE event, restart FCF table scan. */
	if (lpfc_check_pending_fcoe_event(phba, 1)) {
		mempool_free(mboxq, phba->mbox_mem_pool);
		return;
	}
	spin_lock_irqsave(&phba->hbalock, flags);
	phba->fcf.fcf_flag |= (FCF_DISCOVERED | FCF_IN_USE);
	phba->hba_flag &= ~FCF_DISC_INPROGRESS;
	spin_unlock_irqrestore(&phba->hbalock, flags);
	if (vport->port_state != LPFC_FLOGI)
		lpfc_initial_flogi(vport);

	mempool_free(mboxq, phba->mbox_mem_pool);
	return;
}

/**
 * lpfc_fab_name_match - Check if the fcf fabric name match.
 * @fab_name: pointer to fabric name.
 * @new_fcf_record: pointer to fcf record.
 *
 * This routine compare the fcf record's fabric name with provided
 * fabric name. If the fabric name are identical this function
 * returns 1 else return 0.
 **/
static uint32_t
lpfc_fab_name_match(uint8_t *fab_name, struct fcf_record *new_fcf_record)
{
	if ((fab_name[0] ==
		bf_get(lpfc_fcf_record_fab_name_0, new_fcf_record)) &&
	    (fab_name[1] ==
		bf_get(lpfc_fcf_record_fab_name_1, new_fcf_record)) &&
	    (fab_name[2] ==
		bf_get(lpfc_fcf_record_fab_name_2, new_fcf_record)) &&
	    (fab_name[3] ==
		bf_get(lpfc_fcf_record_fab_name_3, new_fcf_record)) &&
	    (fab_name[4] ==
		bf_get(lpfc_fcf_record_fab_name_4, new_fcf_record)) &&
	    (fab_name[5] ==
		bf_get(lpfc_fcf_record_fab_name_5, new_fcf_record)) &&
	    (fab_name[6] ==
		bf_get(lpfc_fcf_record_fab_name_6, new_fcf_record)) &&
	    (fab_name[7] ==
		bf_get(lpfc_fcf_record_fab_name_7, new_fcf_record)))
		return 1;
	else
		return 0;
}

/**
 * lpfc_sw_name_match - Check if the fcf switch name match.
 * @fab_name: pointer to fabric name.
 * @new_fcf_record: pointer to fcf record.
 *
 * This routine compare the fcf record's switch name with provided
 * switch name. If the switch name are identical this function
 * returns 1 else return 0.
 **/
static uint32_t
lpfc_sw_name_match(uint8_t *sw_name, struct fcf_record *new_fcf_record)
{
	if ((sw_name[0] ==
		bf_get(lpfc_fcf_record_switch_name_0, new_fcf_record)) &&
	    (sw_name[1] ==
		bf_get(lpfc_fcf_record_switch_name_1, new_fcf_record)) &&
	    (sw_name[2] ==
		bf_get(lpfc_fcf_record_switch_name_2, new_fcf_record)) &&
	    (sw_name[3] ==
		bf_get(lpfc_fcf_record_switch_name_3, new_fcf_record)) &&
	    (sw_name[4] ==
		bf_get(lpfc_fcf_record_switch_name_4, new_fcf_record)) &&
	    (sw_name[5] ==
		bf_get(lpfc_fcf_record_switch_name_5, new_fcf_record)) &&
	    (sw_name[6] ==
		bf_get(lpfc_fcf_record_switch_name_6, new_fcf_record)) &&
	    (sw_name[7] ==
		bf_get(lpfc_fcf_record_switch_name_7, new_fcf_record)))
		return 1;
	else
		return 0;
}

/**
 * lpfc_mac_addr_match - Check if the fcf mac address match.
 * @phba: pointer to lpfc hba data structure.
 * @new_fcf_record: pointer to fcf record.
 *
 * This routine compare the fcf record's mac address with HBA's
 * FCF mac address. If the mac addresses are identical this function
 * returns 1 else return 0.
 **/
static uint32_t
lpfc_mac_addr_match(struct lpfc_hba *phba, struct fcf_record *new_fcf_record)
{
	if ((phba->fcf.mac_addr[0] ==
		bf_get(lpfc_fcf_record_mac_0, new_fcf_record)) &&
	    (phba->fcf.mac_addr[1] ==
		bf_get(lpfc_fcf_record_mac_1, new_fcf_record)) &&
	    (phba->fcf.mac_addr[2] ==
		bf_get(lpfc_fcf_record_mac_2, new_fcf_record)) &&
	    (phba->fcf.mac_addr[3] ==
		bf_get(lpfc_fcf_record_mac_3, new_fcf_record)) &&
	    (phba->fcf.mac_addr[4] ==
		bf_get(lpfc_fcf_record_mac_4, new_fcf_record)) &&
	    (phba->fcf.mac_addr[5] ==
		bf_get(lpfc_fcf_record_mac_5, new_fcf_record)))
		return 1;
	else
		return 0;
}

/**
 * lpfc_copy_fcf_record - Copy fcf information to lpfc_hba.
 * @phba: pointer to lpfc hba data structure.
 * @new_fcf_record: pointer to fcf record.
 *
 * This routine copies the FCF information from the FCF
 * record to lpfc_hba data structure.
 **/
static void
lpfc_copy_fcf_record(struct lpfc_hba *phba, struct fcf_record *new_fcf_record)
{
	phba->fcf.fabric_name[0] =
		bf_get(lpfc_fcf_record_fab_name_0, new_fcf_record);
	phba->fcf.fabric_name[1] =
		bf_get(lpfc_fcf_record_fab_name_1, new_fcf_record);
	phba->fcf.fabric_name[2] =
		bf_get(lpfc_fcf_record_fab_name_2, new_fcf_record);
	phba->fcf.fabric_name[3] =
		bf_get(lpfc_fcf_record_fab_name_3, new_fcf_record);
	phba->fcf.fabric_name[4] =
		bf_get(lpfc_fcf_record_fab_name_4, new_fcf_record);
	phba->fcf.fabric_name[5] =
		bf_get(lpfc_fcf_record_fab_name_5, new_fcf_record);
	phba->fcf.fabric_name[6] =
		bf_get(lpfc_fcf_record_fab_name_6, new_fcf_record);
	phba->fcf.fabric_name[7] =
		bf_get(lpfc_fcf_record_fab_name_7, new_fcf_record);
	phba->fcf.mac_addr[0] =
		bf_get(lpfc_fcf_record_mac_0, new_fcf_record);
	phba->fcf.mac_addr[1] =
		bf_get(lpfc_fcf_record_mac_1, new_fcf_record);
	phba->fcf.mac_addr[2] =
		bf_get(lpfc_fcf_record_mac_2, new_fcf_record);
	phba->fcf.mac_addr[3] =
		bf_get(lpfc_fcf_record_mac_3, new_fcf_record);
	phba->fcf.mac_addr[4] =
		bf_get(lpfc_fcf_record_mac_4, new_fcf_record);
	phba->fcf.mac_addr[5] =
		bf_get(lpfc_fcf_record_mac_5, new_fcf_record);
	phba->fcf.fcf_indx = bf_get(lpfc_fcf_record_fcf_index, new_fcf_record);
	phba->fcf.priority = new_fcf_record->fip_priority;
	phba->fcf.switch_name[0] =
		bf_get(lpfc_fcf_record_switch_name_0, new_fcf_record);
	phba->fcf.switch_name[1] =
		bf_get(lpfc_fcf_record_switch_name_1, new_fcf_record);
	phba->fcf.switch_name[2] =
		bf_get(lpfc_fcf_record_switch_name_2, new_fcf_record);
	phba->fcf.switch_name[3] =
		bf_get(lpfc_fcf_record_switch_name_3, new_fcf_record);
	phba->fcf.switch_name[4] =
		bf_get(lpfc_fcf_record_switch_name_4, new_fcf_record);
	phba->fcf.switch_name[5] =
		bf_get(lpfc_fcf_record_switch_name_5, new_fcf_record);
	phba->fcf.switch_name[6] =
		bf_get(lpfc_fcf_record_switch_name_6, new_fcf_record);
	phba->fcf.switch_name[7] =
		bf_get(lpfc_fcf_record_switch_name_7, new_fcf_record);
}

/**
 * lpfc_register_fcf - Register the FCF with hba.
 * @phba: pointer to lpfc hba data structure.
 *
 * This routine issues a register fcfi mailbox command to register
 * the fcf with HBA.
 **/
static void
lpfc_register_fcf(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *fcf_mbxq;
	int rc;
	unsigned long flags;

	spin_lock_irqsave(&phba->hbalock, flags);

	/* If the FCF is not availabe do nothing. */
	if (!(phba->fcf.fcf_flag & FCF_AVAILABLE)) {
		phba->hba_flag &= ~FCF_DISC_INPROGRESS;
		spin_unlock_irqrestore(&phba->hbalock, flags);
		return;
	}

	/* The FCF is already registered, start discovery */
	if (phba->fcf.fcf_flag & FCF_REGISTERED) {
		phba->fcf.fcf_flag |= (FCF_DISCOVERED | FCF_IN_USE);
		phba->hba_flag &= ~FCF_DISC_INPROGRESS;
		spin_unlock_irqrestore(&phba->hbalock, flags);
		if (phba->pport->port_state != LPFC_FLOGI)
			lpfc_initial_flogi(phba->pport);
		return;
	}
	spin_unlock_irqrestore(&phba->hbalock, flags);

	fcf_mbxq = mempool_alloc(phba->mbox_mem_pool,
		GFP_KERNEL);
	if (!fcf_mbxq) {
		spin_lock_irqsave(&phba->hbalock, flags);
		phba->hba_flag &= ~FCF_DISC_INPROGRESS;
		spin_unlock_irqrestore(&phba->hbalock, flags);
		return;
	}

	lpfc_reg_fcfi(phba, fcf_mbxq);
	fcf_mbxq->vport = phba->pport;
	fcf_mbxq->mbox_cmpl = lpfc_mbx_cmpl_reg_fcfi;
	rc = lpfc_sli_issue_mbox(phba, fcf_mbxq, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED) {
		spin_lock_irqsave(&phba->hbalock, flags);
		phba->hba_flag &= ~FCF_DISC_INPROGRESS;
		spin_unlock_irqrestore(&phba->hbalock, flags);
		mempool_free(fcf_mbxq, phba->mbox_mem_pool);
	}

	return;
}

/**
 * lpfc_match_fcf_conn_list - Check if the FCF record can be used for discovery.
 * @phba: pointer to lpfc hba data structure.
 * @new_fcf_record: pointer to fcf record.
 * @boot_flag: Indicates if this record used by boot bios.
 * @addr_mode: The address mode to be used by this FCF
 *
 * This routine compare the fcf record with connect list obtained from the
 * config region to decide if this FCF can be used for SAN discovery. It returns
 * 1 if this record can be used for SAN discovery else return zero. If this FCF
 * record can be used for SAN discovery, the boot_flag will indicate if this FCF
 * is used by boot bios and addr_mode will indicate the addressing mode to be
 * used for this FCF when the function returns.
 * If the FCF record need to be used with a particular vlan id, the vlan is
 * set in the vlan_id on return of the function. If not VLAN tagging need to
 * be used with the FCF vlan_id will be set to 0xFFFF;
 **/
static int
lpfc_match_fcf_conn_list(struct lpfc_hba *phba,
			struct fcf_record *new_fcf_record,
			uint32_t *boot_flag, uint32_t *addr_mode,
			uint16_t *vlan_id)
{
	struct lpfc_fcf_conn_entry *conn_entry;
	int i, j, fcf_vlan_id = 0;

	/* Find the lowest VLAN id in the FCF record */
	for (i = 0; i < 512; i++) {
		if (new_fcf_record->vlan_bitmap[i]) {
			fcf_vlan_id = i * 8;
			j = 0;
			while (!((new_fcf_record->vlan_bitmap[i] >> j) & 1)) {
				j++;
				fcf_vlan_id++;
			}
			break;
		}
	}

	/* If FCF not available return 0 */
	if (!bf_get(lpfc_fcf_record_fcf_avail, new_fcf_record) ||
		!bf_get(lpfc_fcf_record_fcf_valid, new_fcf_record))
		return 0;

	if (!(phba->hba_flag & HBA_FIP_SUPPORT)) {
		*boot_flag = 0;
		*addr_mode = bf_get(lpfc_fcf_record_mac_addr_prov,
				new_fcf_record);
		if (phba->valid_vlan)
			*vlan_id = phba->vlan_id;
		else
			*vlan_id = 0xFFFF;
		return 1;
	}

	/*
	 * If there are no FCF connection table entry, driver connect to all
	 * FCFs.
	 */
	if (list_empty(&phba->fcf_conn_rec_list)) {
		*boot_flag = 0;
		*addr_mode = bf_get(lpfc_fcf_record_mac_addr_prov,
			new_fcf_record);

		/*
		 * When there are no FCF connect entries, use driver's default
		 * addressing mode - FPMA.
		 */
		if (*addr_mode & LPFC_FCF_FPMA)
			*addr_mode = LPFC_FCF_FPMA;

		/* If FCF record report a vlan id use that vlan id */
		if (fcf_vlan_id)
			*vlan_id = fcf_vlan_id;
		else
			*vlan_id = 0xFFFF;
		return 1;
	}

	list_for_each_entry(conn_entry, &phba->fcf_conn_rec_list, list) {
		if (!(conn_entry->conn_rec.flags & FCFCNCT_VALID))
			continue;

		if ((conn_entry->conn_rec.flags & FCFCNCT_FBNM_VALID) &&
			!lpfc_fab_name_match(conn_entry->conn_rec.fabric_name,
					     new_fcf_record))
			continue;
		if ((conn_entry->conn_rec.flags & FCFCNCT_SWNM_VALID) &&
			!lpfc_sw_name_match(conn_entry->conn_rec.switch_name,
					    new_fcf_record))
			continue;
		if (conn_entry->conn_rec.flags & FCFCNCT_VLAN_VALID) {
			/*
			 * If the vlan bit map does not have the bit set for the
			 * vlan id to be used, then it is not a match.
			 */
			if (!(new_fcf_record->vlan_bitmap
				[conn_entry->conn_rec.vlan_tag / 8] &
				(1 << (conn_entry->conn_rec.vlan_tag % 8))))
				continue;
		}

		/*
		 * If connection record does not support any addressing mode,
		 * skip the FCF record.
		 */
		if (!(bf_get(lpfc_fcf_record_mac_addr_prov, new_fcf_record)
			& (LPFC_FCF_FPMA | LPFC_FCF_SPMA)))
			continue;

		/*
		 * Check if the connection record specifies a required
		 * addressing mode.
		 */
		if ((conn_entry->conn_rec.flags & FCFCNCT_AM_VALID) &&
			!(conn_entry->conn_rec.flags & FCFCNCT_AM_PREFERRED)) {

			/*
			 * If SPMA required but FCF not support this continue.
			 */
			if ((conn_entry->conn_rec.flags & FCFCNCT_AM_SPMA) &&
				!(bf_get(lpfc_fcf_record_mac_addr_prov,
					new_fcf_record) & LPFC_FCF_SPMA))
				continue;

			/*
			 * If FPMA required but FCF not support this continue.
			 */
			if (!(conn_entry->conn_rec.flags & FCFCNCT_AM_SPMA) &&
				!(bf_get(lpfc_fcf_record_mac_addr_prov,
				new_fcf_record) & LPFC_FCF_FPMA))
				continue;
		}

		/*
		 * This fcf record matches filtering criteria.
		 */
		if (conn_entry->conn_rec.flags & FCFCNCT_BOOT)
			*boot_flag = 1;
		else
			*boot_flag = 0;

		/*
		 * If user did not specify any addressing mode, or if the
		 * prefered addressing mode specified by user is not supported
		 * by FCF, allow fabric to pick the addressing mode.
		 */
		*addr_mode = bf_get(lpfc_fcf_record_mac_addr_prov,
				new_fcf_record);
		/*
		 * If the user specified a required address mode, assign that
		 * address mode
		 */
		if ((conn_entry->conn_rec.flags & FCFCNCT_AM_VALID) &&
			(!(conn_entry->conn_rec.flags & FCFCNCT_AM_PREFERRED)))
			*addr_mode = (conn_entry->conn_rec.flags &
				FCFCNCT_AM_SPMA) ?
				LPFC_FCF_SPMA : LPFC_FCF_FPMA;
		/*
		 * If the user specified a prefered address mode, use the
		 * addr mode only if FCF support the addr_mode.
		 */
		else if ((conn_entry->conn_rec.flags & FCFCNCT_AM_VALID) &&
			(conn_entry->conn_rec.flags & FCFCNCT_AM_PREFERRED) &&
			(conn_entry->conn_rec.flags & FCFCNCT_AM_SPMA) &&
			(*addr_mode & LPFC_FCF_SPMA))
				*addr_mode = LPFC_FCF_SPMA;
		else if ((conn_entry->conn_rec.flags & FCFCNCT_AM_VALID) &&
			(conn_entry->conn_rec.flags & FCFCNCT_AM_PREFERRED) &&
			!(conn_entry->conn_rec.flags & FCFCNCT_AM_SPMA) &&
			(*addr_mode & LPFC_FCF_FPMA))
				*addr_mode = LPFC_FCF_FPMA;

		/* If matching connect list has a vlan id, use it */
		if (conn_entry->conn_rec.flags & FCFCNCT_VLAN_VALID)
			*vlan_id = conn_entry->conn_rec.vlan_tag;
		/*
		 * If no vlan id is specified in connect list, use the vlan id
		 * in the FCF record
		 */
		else if (fcf_vlan_id)
			*vlan_id = fcf_vlan_id;
		else
			*vlan_id = 0xFFFF;

		return 1;
	}

	return 0;
}

/**
 * lpfc_check_pending_fcoe_event - Check if there is pending fcoe event.
 * @phba: pointer to lpfc hba data structure.
 * @unreg_fcf: Unregister FCF if FCF table need to be re-scaned.
 *
 * This function check if there is any fcoe event pending while driver
 * scan FCF entries. If there is any pending event, it will restart the
 * FCF saning and return 1 else return 0.
 */
int
lpfc_check_pending_fcoe_event(struct lpfc_hba *phba, uint8_t unreg_fcf)
{
	LPFC_MBOXQ_t *mbox;
	int rc;
	/*
	 * If the Link is up and no FCoE events while in the
	 * FCF discovery, no need to restart FCF discovery.
	 */
	if ((phba->link_state  >= LPFC_LINK_UP) &&
		(phba->fcoe_eventtag == phba->fcoe_eventtag_at_fcf_scan))
		return 0;

	spin_lock_irq(&phba->hbalock);
	phba->fcf.fcf_flag &= ~FCF_AVAILABLE;
	spin_unlock_irq(&phba->hbalock);

	if (phba->link_state >= LPFC_LINK_UP)
		lpfc_sli4_read_fcf_record(phba, LPFC_FCOE_FCF_GET_FIRST);
	else {
		/*
		 * Do not continue FCF discovery and clear FCF_DISC_INPROGRESS
		 * flag
		 */
		spin_lock_irq(&phba->hbalock);
		phba->hba_flag &= ~FCF_DISC_INPROGRESS;
		spin_unlock_irq(&phba->hbalock);
	}

	if (unreg_fcf) {
		spin_lock_irq(&phba->hbalock);
		phba->fcf.fcf_flag &= ~FCF_REGISTERED;
		spin_unlock_irq(&phba->hbalock);
		mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
		if (!mbox) {
			lpfc_printf_log(phba, KERN_ERR,
				LOG_DISCOVERY|LOG_MBOX,
				"2610 UNREG_FCFI mbox allocation failed\n");
			return 1;
		}
		lpfc_unreg_fcfi(mbox, phba->fcf.fcfi);
		mbox->vport = phba->pport;
		mbox->mbox_cmpl = lpfc_unregister_fcfi_cmpl;
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);
		if (rc == MBX_NOT_FINISHED) {
			lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
				"2611 UNREG_FCFI issue mbox failed\n");
			mempool_free(mbox, phba->mbox_mem_pool);
		}
	}

	return 1;
}

/**
 * lpfc_mbx_cmpl_read_fcf_record - Completion handler for read_fcf mbox.
 * @phba: pointer to lpfc hba data structure.
 * @mboxq: pointer to mailbox object.
 *
 * This function iterate through all the fcf records available in
 * HBA and choose the optimal FCF record for discovery. After finding
 * the FCF for discovery it register the FCF record and kick start
 * discovery.
 * If FCF_IN_USE flag is set in currently used FCF, the routine try to
 * use a FCF record which match fabric name and mac address of the
 * currently used FCF record.
 * If the driver support only one FCF, it will try to use the FCF record
 * used by BOOT_BIOS.
 */
void
lpfc_mbx_cmpl_read_fcf_record(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	void *virt_addr;
	dma_addr_t phys_addr;
	uint8_t *bytep;
	struct lpfc_mbx_sge sge;
	struct lpfc_mbx_read_fcf_tbl *read_fcf;
	uint32_t shdr_status, shdr_add_status;
	union lpfc_sli4_cfg_shdr *shdr;
	struct fcf_record *new_fcf_record;
	int rc;
	uint32_t boot_flag, addr_mode;
	uint32_t next_fcf_index;
	unsigned long flags;
	uint16_t vlan_id;

	/* If there is pending FCoE event restart FCF table scan */
	if (lpfc_check_pending_fcoe_event(phba, 0)) {
		lpfc_sli4_mbox_cmd_free(phba, mboxq);
		return;
	}

	/* Get the first SGE entry from the non-embedded DMA memory. This
	 * routine only uses a single SGE.
	 */
	lpfc_sli4_mbx_sge_get(mboxq, 0, &sge);
	phys_addr = getPaddr(sge.pa_hi, sge.pa_lo);
	if (unlikely(!mboxq->sge_array)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_MBOX,
				"2524 Failed to get the non-embedded SGE "
				"virtual address\n");
		goto out;
	}
	virt_addr = mboxq->sge_array->addr[0];

	shdr = (union lpfc_sli4_cfg_shdr *)virt_addr;
	shdr_status = bf_get(lpfc_mbox_hdr_status, &shdr->response);
	shdr_add_status = bf_get(lpfc_mbox_hdr_add_status,
				 &shdr->response);
	/*
	 * The FCF Record was read and there is no reason for the driver
	 * to maintain the FCF record data or memory. Instead, just need
	 * to book keeping the FCFIs can be used.
	 */
	if (shdr_status || shdr_add_status) {
		if (shdr_status == STATUS_FCF_TABLE_EMPTY) {
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
					"2726 READ_FCF_RECORD Indicates empty "
					"FCF table.\n");
		} else {
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
					"2521 READ_FCF_RECORD mailbox failed "
					"with status x%x add_status x%x, mbx\n",
					shdr_status, shdr_add_status);
		}
		goto out;
	}
	/* Interpreting the returned information of FCF records */
	read_fcf = (struct lpfc_mbx_read_fcf_tbl *)virt_addr;
	lpfc_sli_pcimem_bcopy(read_fcf, read_fcf,
			      sizeof(struct lpfc_mbx_read_fcf_tbl));
	next_fcf_index = bf_get(lpfc_mbx_read_fcf_tbl_nxt_vindx, read_fcf);

	new_fcf_record = (struct fcf_record *)(virt_addr +
			  sizeof(struct lpfc_mbx_read_fcf_tbl));
	lpfc_sli_pcimem_bcopy(new_fcf_record, new_fcf_record,
			      sizeof(struct fcf_record));
	bytep = virt_addr + sizeof(union lpfc_sli4_cfg_shdr);

	rc = lpfc_match_fcf_conn_list(phba, new_fcf_record,
				      &boot_flag, &addr_mode,
					&vlan_id);
	/*
	 * If the fcf record does not match with connect list entries
	 * read the next entry.
	 */
	if (!rc)
		goto read_next_fcf;
	/*
	 * If this is not the first FCF discovery of the HBA, use last
	 * FCF record for the discovery.
	 */
	spin_lock_irqsave(&phba->hbalock, flags);
	if (phba->fcf.fcf_flag & FCF_IN_USE) {
		if (lpfc_fab_name_match(phba->fcf.fabric_name,
					new_fcf_record) &&
		    lpfc_sw_name_match(phba->fcf.switch_name,
					new_fcf_record) &&
		    lpfc_mac_addr_match(phba, new_fcf_record)) {
			phba->fcf.fcf_flag |= FCF_AVAILABLE;
			spin_unlock_irqrestore(&phba->hbalock, flags);
			goto out;
		}
		spin_unlock_irqrestore(&phba->hbalock, flags);
		goto read_next_fcf;
	}
	if (phba->fcf.fcf_flag & FCF_AVAILABLE) {
		/*
		 * If the current FCF record does not have boot flag
		 * set and new fcf record has boot flag set, use the
		 * new fcf record.
		 */
		if (boot_flag && !(phba->fcf.fcf_flag & FCF_BOOT_ENABLE)) {
			/* Use this FCF record */
			lpfc_copy_fcf_record(phba, new_fcf_record);
			phba->fcf.addr_mode = addr_mode;
			phba->fcf.fcf_flag |= FCF_BOOT_ENABLE;
			if (vlan_id != 0xFFFF) {
				phba->fcf.fcf_flag |= FCF_VALID_VLAN;
				phba->fcf.vlan_id = vlan_id;
			}
			spin_unlock_irqrestore(&phba->hbalock, flags);
			goto read_next_fcf;
		}
		/*
		 * If the current FCF record has boot flag set and the
		 * new FCF record does not have boot flag, read the next
		 * FCF record.
		 */
		if (!boot_flag && (phba->fcf.fcf_flag & FCF_BOOT_ENABLE)) {
			spin_unlock_irqrestore(&phba->hbalock, flags);
			goto read_next_fcf;
		}
		/*
		 * If there is a record with lower priority value for
		 * the current FCF, use that record.
		 */
		if (lpfc_fab_name_match(phba->fcf.fabric_name,
					new_fcf_record) &&
		    (new_fcf_record->fip_priority < phba->fcf.priority)) {
			/* Use this FCF record */
			lpfc_copy_fcf_record(phba, new_fcf_record);
			phba->fcf.addr_mode = addr_mode;
			if (vlan_id != 0xFFFF) {
				phba->fcf.fcf_flag |= FCF_VALID_VLAN;
				phba->fcf.vlan_id = vlan_id;
			}
			spin_unlock_irqrestore(&phba->hbalock, flags);
			goto read_next_fcf;
		}
		spin_unlock_irqrestore(&phba->hbalock, flags);
		goto read_next_fcf;
	}
	/*
	 * This is the first available FCF record, use this
	 * record.
	 */
	lpfc_copy_fcf_record(phba, new_fcf_record);
	phba->fcf.addr_mode = addr_mode;
	if (boot_flag)
		phba->fcf.fcf_flag |= FCF_BOOT_ENABLE;
	phba->fcf.fcf_flag |= FCF_AVAILABLE;
	if (vlan_id != 0xFFFF) {
		phba->fcf.fcf_flag |= FCF_VALID_VLAN;
		phba->fcf.vlan_id = vlan_id;
	}
	spin_unlock_irqrestore(&phba->hbalock, flags);
	goto read_next_fcf;

read_next_fcf:
	lpfc_sli4_mbox_cmd_free(phba, mboxq);
	if (next_fcf_index == LPFC_FCOE_FCF_NEXT_NONE || next_fcf_index == 0)
		lpfc_register_fcf(phba);
	else
		lpfc_sli4_read_fcf_record(phba, next_fcf_index);
	return;

out:
	lpfc_sli4_mbox_cmd_free(phba, mboxq);
	lpfc_register_fcf(phba);

	return;
}

/**
 * lpfc_init_vpi_cmpl - Completion handler for init_vpi mbox command.
 * @phba: pointer to lpfc hba data structure.
 * @mboxq: pointer to mailbox data structure.
 *
 * This function handles completion of init vpi mailbox command.
 */
static void
lpfc_init_vpi_cmpl(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	struct lpfc_vport *vport = mboxq->vport;
	if (mboxq->u.mb.mbxStatus) {
		lpfc_printf_vlog(vport, KERN_ERR,
				LOG_MBOX,
				"2609 Init VPI mailbox failed 0x%x\n",
				mboxq->u.mb.mbxStatus);
		mempool_free(mboxq, phba->mbox_mem_pool);
		lpfc_vport_set_state(vport, FC_VPORT_FAILED);
		return;
	}
	spin_lock_irq(&phba->hbalock);
	vport->fc_flag &= ~FC_VPORT_NEEDS_INIT_VPI;
	spin_unlock_irq(&phba->hbalock);

	if (phba->link_flag & LS_NPIV_FAB_SUPPORTED)
		lpfc_initial_fdisc(vport);
	else {
		lpfc_vport_set_state(vport, FC_VPORT_NO_FABRIC_SUPP);
		lpfc_printf_vlog(vport, KERN_ERR, LOG_ELS,
				 "2606 No NPIV Fabric support\n");
	}
	return;
}

/**
 * lpfc_start_fdiscs - send fdiscs for each vports on this port.
 * @phba: pointer to lpfc hba data structure.
 *
 * This function loops through the list of vports on the @phba and issues an
 * FDISC if possible.
 */
void
lpfc_start_fdiscs(struct lpfc_hba *phba)
{
	struct lpfc_vport **vports;
	int i;
	LPFC_MBOXQ_t *mboxq;
	int rc;

	vports = lpfc_create_vport_work_array(phba);
	if (vports != NULL) {
		for (i = 0; i <= phba->max_vports && vports[i] != NULL; i++) {
			if (vports[i]->port_type == LPFC_PHYSICAL_PORT)
				continue;
			/* There are no vpi for this vport */
			if (vports[i]->vpi > phba->max_vpi) {
				lpfc_vport_set_state(vports[i],
						     FC_VPORT_FAILED);
				continue;
			}
			if (phba->fc_topology == TOPOLOGY_LOOP) {
				lpfc_vport_set_state(vports[i],
						     FC_VPORT_LINKDOWN);
				continue;
			}
			if (vports[i]->fc_flag & FC_VPORT_NEEDS_INIT_VPI) {
				mboxq = mempool_alloc(phba->mbox_mem_pool,
					GFP_KERNEL);
				if (!mboxq) {
					lpfc_printf_vlog(vports[i], KERN_ERR,
					LOG_MBOX, "2607 Failed to allocate "
					"init_vpi mailbox\n");
					continue;
				}
				lpfc_init_vpi(phba, mboxq, vports[i]->vpi);
				mboxq->vport = vports[i];
				mboxq->mbox_cmpl = lpfc_init_vpi_cmpl;
				rc = lpfc_sli_issue_mbox(phba, mboxq,
					MBX_NOWAIT);
				if (rc == MBX_NOT_FINISHED) {
					lpfc_printf_vlog(vports[i], KERN_ERR,
					LOG_MBOX, "2608 Failed to issue "
					"init_vpi mailbox\n");
					mempool_free(mboxq,
						phba->mbox_mem_pool);
				}
				continue;
			}
			if (phba->link_flag & LS_NPIV_FAB_SUPPORTED)
				lpfc_initial_fdisc(vports[i]);
			else {
				lpfc_vport_set_state(vports[i],
						     FC_VPORT_NO_FABRIC_SUPP);
				lpfc_printf_vlog(vports[i], KERN_ERR,
						 LOG_ELS,
						 "0259 No NPIV "
						 "Fabric support\n");
			}
		}
	}
	lpfc_destroy_vport_work_array(phba, vports);
}

void
lpfc_mbx_cmpl_reg_vfi(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	struct lpfc_dmabuf *dmabuf = mboxq->context1;
	struct lpfc_vport *vport = mboxq->vport;

	if (mboxq->u.mb.mbxStatus) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
			 "2018 REG_VFI mbxStatus error x%x "
			 "HBA state x%x\n",
			 mboxq->u.mb.mbxStatus, vport->port_state);
		if (phba->fc_topology == TOPOLOGY_LOOP) {
			/* FLOGI failed, use loop map to make discovery list */
			lpfc_disc_list_loopmap(vport);
			/* Start discovery */
			lpfc_disc_start(vport);
			goto fail_free_mem;
		}
		lpfc_vport_set_state(vport, FC_VPORT_FAILED);
		goto fail_free_mem;
	}
	/* The VPI is implicitly registered when the VFI is registered */
	vport->vpi_state |= LPFC_VPI_REGISTERED;

	if (vport->port_state == LPFC_FABRIC_CFG_LINK &&
		!vport->cfg_enable_auth)
		lpfc_start_discovery(vport);

fail_free_mem:
	mempool_free(mboxq, phba->mbox_mem_pool);
	lpfc_mbuf_free(phba, dmabuf->virt, dmabuf->phys);
	kfree(dmabuf);
	return;
}

static void
lpfc_mbx_cmpl_read_sparam(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	MAILBOX_t *mb = &pmb->u.mb;
	struct lpfc_dmabuf *mp = (struct lpfc_dmabuf *) pmb->context1;
	struct lpfc_vport  *vport = pmb->vport;


	/* Check for error */
	if (mb->mbxStatus) {
		/* READ_SPARAM mbox error <mbxStatus> state <hba_state> */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
				 "0319 READ_SPARAM mbxStatus error x%x "
				 "hba state x%x>\n",
				 mb->mbxStatus, vport->port_state);
		lpfc_linkdown(phba);
		goto out;
	}

	memcpy((uint8_t *) &vport->fc_sparam, (uint8_t *) mp->virt,
	       sizeof (struct serv_parm));
	if (phba->cfg_soft_wwnn)
		u64_to_wwn(phba->cfg_soft_wwnn,
			   vport->fc_sparam.nodeName.u.wwn);
	if (phba->cfg_soft_wwpn)
		u64_to_wwn(phba->cfg_soft_wwpn,
			   vport->fc_sparam.portName.u.wwn);
	memcpy(&vport->fc_nodename, &vport->fc_sparam.nodeName,
	       sizeof(vport->fc_nodename));
	memcpy(&vport->fc_portname, &vport->fc_sparam.portName,
	       sizeof(vport->fc_portname));
	if (vport->port_type == LPFC_PHYSICAL_PORT) {
		memcpy(&phba->wwnn, &vport->fc_nodename, sizeof(phba->wwnn));
		memcpy(&phba->wwpn, &vport->fc_portname, sizeof(phba->wwnn));
	}

	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mempool_free(pmb, phba->mbox_mem_pool);
	return;

out:
	pmb->context1 = NULL;
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	lpfc_issue_clear_la(phba, vport);
	mempool_free(pmb, phba->mbox_mem_pool);
	return;
}

static void
lpfc_mbx_process_link_up(struct lpfc_hba *phba, READ_LA_VAR *la)
{
	struct lpfc_vport *vport = phba->pport;
	LPFC_MBOXQ_t *sparam_mbox, *cfglink_mbox = NULL;
	int i;
	struct lpfc_dmabuf *mp;
	int rc;
	struct fcf_record *fcf_record;

	sparam_mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);

	spin_lock_irq(&phba->hbalock);
	switch (la->UlnkSpeed) {
	case LA_1GHZ_LINK:
		phba->fc_linkspeed = LA_1GHZ_LINK;
		break;
	case LA_2GHZ_LINK:
		phba->fc_linkspeed = LA_2GHZ_LINK;
		break;
	case LA_4GHZ_LINK:
		phba->fc_linkspeed = LA_4GHZ_LINK;
		break;
	case LA_8GHZ_LINK:
		phba->fc_linkspeed = LA_8GHZ_LINK;
		break;
	case LA_10GHZ_LINK:
		phba->fc_linkspeed = LA_10GHZ_LINK;
		break;
	default:
		phba->fc_linkspeed = LA_UNKNW_LINK;
		break;
	}

	phba->fc_topology = la->topology;
	phba->link_flag &= ~LS_NPIV_FAB_SUPPORTED;

	if (phba->fc_topology == TOPOLOGY_LOOP) {
		phba->sli3_options &= ~LPFC_SLI3_NPIV_ENABLED;

		/* if npiv is enabled and this adapter supports npiv log
		 * a message that npiv is not supported in this topology
		 */
		if (phba->cfg_enable_npiv && phba->max_vpi)
			lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
				"1310 Link Up Event npiv not supported in loop "
				"topology\n");
		/* Get Loop Map information */
		if (la->il)
			vport->fc_flag |= FC_LBIT;

		vport->fc_myDID = la->granted_AL_PA;
		i = la->un.lilpBde64.tus.f.bdeSize;

		if (i == 0) {
			phba->alpa_map[0] = 0;
		} else {
			if (vport->cfg_log_verbose & LOG_LINK_EVENT) {
				int numalpa, j, k;
				union {
					uint8_t pamap[16];
					struct {
						uint32_t wd1;
						uint32_t wd2;
						uint32_t wd3;
						uint32_t wd4;
					} pa;
				} un;
				numalpa = phba->alpa_map[0];
				j = 0;
				while (j < numalpa) {
					memset(un.pamap, 0, 16);
					for (k = 1; j < numalpa; k++) {
						un.pamap[k - 1] =
							phba->alpa_map[j + 1];
						j++;
						if (k == 16)
							break;
					}
					/* Link Up Event ALPA map */
					lpfc_printf_log(phba,
							KERN_WARNING,
							LOG_LINK_EVENT,
							"1304 Link Up Event "
							"ALPA map Data: x%x "
							"x%x x%x x%x\n",
							un.pa.wd1, un.pa.wd2,
							un.pa.wd3, un.pa.wd4);
				}
			}
		}
	} else {
		if (!(phba->sli3_options & LPFC_SLI3_NPIV_ENABLED)) {
			if (phba->max_vpi && phba->cfg_enable_npiv &&
			   (phba->sli_rev == 3))
				phba->sli3_options |= LPFC_SLI3_NPIV_ENABLED;
		}
		vport->fc_myDID = phba->fc_pref_DID;
		vport->fc_flag |= FC_LBIT;
	}
	spin_unlock_irq(&phba->hbalock);

	lpfc_linkup(phba);
	if (sparam_mbox) {
		lpfc_read_sparam(phba, sparam_mbox, 0);
		sparam_mbox->vport = vport;
		sparam_mbox->mbox_cmpl = lpfc_mbx_cmpl_read_sparam;
		rc = lpfc_sli_issue_mbox(phba, sparam_mbox, MBX_NOWAIT);
		if (rc == MBX_NOT_FINISHED) {
			mp = (struct lpfc_dmabuf *) sparam_mbox->context1;
			lpfc_mbuf_free(phba, mp->virt, mp->phys);
			kfree(mp);
			mempool_free(sparam_mbox, phba->mbox_mem_pool);
			goto out;
		}
	}

	if (!(phba->hba_flag & HBA_FCOE_SUPPORT)) {
		cfglink_mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
		if (!cfglink_mbox)
			goto out;
		vport->port_state = LPFC_LOCAL_CFG_LINK;
		lpfc_config_link(phba, cfglink_mbox);
		cfglink_mbox->vport = vport;
		cfglink_mbox->mbox_cmpl = lpfc_mbx_cmpl_local_config_link;
		rc = lpfc_sli_issue_mbox(phba, cfglink_mbox, MBX_NOWAIT);
		if (rc == MBX_NOT_FINISHED) {
			mempool_free(cfglink_mbox, phba->mbox_mem_pool);
			goto out;
		}
	} else {
		vport->port_state = LPFC_VPORT_UNKNOWN;
		/*
		 * Add the driver's default FCF record at FCF index 0 now. This
		 * is phase 1 implementation that support FCF index 0 and driver
		 * defaults.
		 */
		if (!(phba->hba_flag & HBA_FIP_SUPPORT)) {
			fcf_record = kzalloc(sizeof(struct fcf_record),
					GFP_KERNEL);
			if (unlikely(!fcf_record)) {
				lpfc_printf_log(phba, KERN_ERR,
					LOG_MBOX | LOG_SLI,
					"2554 Could not allocate memmory for "
					"fcf record\n");
				rc = -ENODEV;
				goto out;
			}

			lpfc_sli4_build_dflt_fcf_record(phba, fcf_record,
						LPFC_FCOE_FCF_DEF_INDEX);
			rc = lpfc_sli4_add_fcf_record(phba, fcf_record);
			if (unlikely(rc)) {
				lpfc_printf_log(phba, KERN_ERR,
					LOG_MBOX | LOG_SLI,
					"2013 Could not manually add FCF "
					"record 0, status %d\n", rc);
				rc = -ENODEV;
				kfree(fcf_record);
				goto out;
			}
			kfree(fcf_record);
		}
		/*
		 * The driver is expected to do FIP/FCF. Call the port
		 * and get the FCF Table.
		 */
		spin_lock_irq(&phba->hbalock);
		if (phba->hba_flag & FCF_DISC_INPROGRESS) {
			spin_unlock_irq(&phba->hbalock);
			return;
		}
		spin_unlock_irq(&phba->hbalock);
		rc = lpfc_sli4_read_fcf_record(phba,
					LPFC_FCOE_FCF_GET_FIRST);
		if (rc)
			goto out;
	}

	return;
out:
	lpfc_vport_set_state(vport, FC_VPORT_FAILED);
	lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
			 "0263 Discovery Mailbox error: state: 0x%x : %p %p\n",
			 vport->port_state, sparam_mbox, cfglink_mbox);
	lpfc_issue_clear_la(phba, vport);
	return;
}

static void
lpfc_enable_la(struct lpfc_hba *phba)
{
	uint32_t control;
	struct lpfc_sli *psli = &phba->sli;
	spin_lock_irq(&phba->hbalock);
	psli->sli_flag |= LPFC_PROCESS_LA;
	if (phba->sli_rev <= LPFC_SLI_REV3) {
		control = readl(phba->HCregaddr);
		control |= HC_LAINT_ENA;
		writel(control, phba->HCregaddr);
		readl(phba->HCregaddr); /* flush */
	}
	spin_unlock_irq(&phba->hbalock);
}

static void
lpfc_mbx_issue_link_down(struct lpfc_hba *phba)
{
	lpfc_linkdown(phba);
	lpfc_enable_la(phba);
	lpfc_unregister_unused_fcf(phba);
	/* turn on Link Attention interrupts - no CLEAR_LA needed */
}


/*
 * This routine handles processing a READ_LA mailbox
 * command upon completion. It is setup in the LPFC_MBOXQ
 * as the completion routine when the command is
 * handed off to the SLI layer.
 */
void
lpfc_mbx_cmpl_read_la(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_vport *vport = pmb->vport;
	struct Scsi_Host  *shost = lpfc_shost_from_vport(vport);
	READ_LA_VAR *la;
	MAILBOX_t *mb = &pmb->u.mb;
	struct lpfc_dmabuf *mp = (struct lpfc_dmabuf *) (pmb->context1);

	/* Unblock ELS traffic */
	phba->sli.ring[LPFC_ELS_RING].flag &= ~LPFC_STOP_IOCB_EVENT;
	/* Check for error */
	if (mb->mbxStatus) {
		lpfc_printf_log(phba, KERN_INFO, LOG_LINK_EVENT,
				"1307 READ_LA mbox error x%x state x%x\n",
				mb->mbxStatus, vport->port_state);
		lpfc_mbx_issue_link_down(phba);
		phba->link_state = LPFC_HBA_ERROR;
		goto lpfc_mbx_cmpl_read_la_free_mbuf;
	}

	la = (READ_LA_VAR *) &pmb->u.mb.un.varReadLA;

	memcpy(&phba->alpa_map[0], mp->virt, 128);

	spin_lock_irq(shost->host_lock);
	if (la->pb)
		vport->fc_flag |= FC_BYPASSED_MODE;
	else
		vport->fc_flag &= ~FC_BYPASSED_MODE;
	spin_unlock_irq(shost->host_lock);

	if ((phba->fc_eventTag  < la->eventTag) ||
	    (phba->fc_eventTag == la->eventTag)) {
		phba->fc_stat.LinkMultiEvent++;
		if (la->attType == AT_LINK_UP)
			if (phba->fc_eventTag != 0)
				lpfc_linkdown(phba);
	}

	phba->fc_eventTag = la->eventTag;
	if (la->mm)
		phba->sli.sli_flag |= LPFC_MENLO_MAINT;
	else
		phba->sli.sli_flag &= ~LPFC_MENLO_MAINT;

	phba->link_events++;
	if (la->attType == AT_LINK_UP && (!la->mm)) {
		phba->fc_stat.LinkUp++;
		if (phba->link_flag & LS_LOOPBACK_MODE) {
			lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
					"1306 Link Up Event in loop back mode "
					"x%x received Data: x%x x%x x%x x%x\n",
					la->eventTag, phba->fc_eventTag,
					la->granted_AL_PA, la->UlnkSpeed,
					phba->alpa_map[0]);
		} else {
			lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
					"1303 Link Up Event x%x received "
					"Data: x%x x%x x%x x%x x%x x%x %d\n",
					la->eventTag, phba->fc_eventTag,
					la->granted_AL_PA, la->UlnkSpeed,
					phba->alpa_map[0],
					la->mm, la->fa,
					phba->wait_4_mlo_maint_flg);
		}
		lpfc_mbx_process_link_up(phba, la);
	} else if (la->attType == AT_LINK_DOWN) {
		phba->fc_stat.LinkDown++;
		if (phba->link_flag & LS_LOOPBACK_MODE) {
			lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
				"1300 Link Down Event in loop back mode "
				"x%x received "
				"Data: x%x x%x x%x\n",
				la->eventTag, phba->fc_eventTag,
				phba->pport->port_state, vport->fc_flag);
		}
		else {
			lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
				"1305 Link Down Event x%x received "
				"Data: x%x x%x x%x x%x x%x\n",
				la->eventTag, phba->fc_eventTag,
				phba->pport->port_state, vport->fc_flag,
				la->mm, la->fa);
		}
		lpfc_mbx_issue_link_down(phba);
	}
	if (la->mm && la->attType == AT_LINK_UP) {
		if (phba->link_state != LPFC_LINK_DOWN) {
			phba->fc_stat.LinkDown++;
			lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
				"1309 Link Down Event x%x received "
				"Data: x%x x%x x%x\n",
				la->eventTag, phba->fc_eventTag,
				phba->pport->port_state, vport->fc_flag);
			lpfc_mbx_issue_link_down(phba);
		} else
			lpfc_enable_la(phba);

		lpfc_printf_log(phba, KERN_ERR, LOG_LINK_EVENT,
				"1308 Menlo Maint Mode Link up Event x%x rcvd "
				"Data: x%x x%x x%x\n",
				la->eventTag, phba->fc_eventTag,
				phba->pport->port_state, vport->fc_flag);
		/*
		 * The cmnd that triggered this will be waiting for this
		 * signal.
		 */
		/* WAKEUP for MENLO_SET_MODE or MENLO_RESET command. */
		if ( phba->wait_4_mlo_maint_flg) {
			phba->wait_4_mlo_maint_flg = 0;
			wake_up_interruptible(&phba->wait_4_mlo_m_q);
		}
	}

	if (la->fa ) {
		if (la->mm)
			lpfc_issue_clear_la(phba, vport);
		lpfc_printf_log(phba, KERN_INFO, LOG_LINK_EVENT,
				"1311 fa %d\n", la->fa);
		lpfc_check_menlo_cfg(phba);
	}


lpfc_mbx_cmpl_read_la_free_mbuf:
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mempool_free(pmb, phba->mbox_mem_pool);
	return;
}

/*
 * This routine handles processing a REG_LOGIN mailbox
 * command upon completion. It is setup in the LPFC_MBOXQ
 * as the completion routine when the command is
 * handed off to the SLI layer.
 */
void
lpfc_mbx_cmpl_reg_login(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_vport  *vport = pmb->vport;
	struct lpfc_dmabuf *mp = (struct lpfc_dmabuf *) (pmb->context1);
	struct lpfc_nodelist *ndlp = (struct lpfc_nodelist *) pmb->context2;

	pmb->context1 = NULL;

	/* Good status, call state machine */
	lpfc_disc_state_machine(vport, ndlp, pmb, NLP_EVT_CMPL_REG_LOGIN);
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mempool_free(pmb, phba->mbox_mem_pool);
	/* decrement the node reference count held for this callback
	 * function.
	 */
	lpfc_nlp_put(ndlp);

	return;
}

static void
lpfc_mbx_cmpl_unreg_vpi(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	MAILBOX_t *mb = &pmb->u.mb;
	struct lpfc_vport *vport = pmb->vport;
	struct Scsi_Host  *shost = lpfc_shost_from_vport(vport);

	switch (mb->mbxStatus) {
	case 0x0011:
	case 0x0020:
	case 0x9700:
		lpfc_printf_vlog(vport, KERN_INFO, LOG_NODE,
				 "0911 cmpl_unreg_vpi, mb status = 0x%x\n",
				 mb->mbxStatus);
		break;
	}
	spin_lock_irq(&phba->hbalock);
	vport->vpi_state &= ~LPFC_VPI_REGISTERED;
	vport->fc_flag |= FC_VPORT_NEEDS_REG_VPI;
	spin_unlock_irq(&phba->hbalock);
	vport->unreg_vpi_cmpl = VPORT_OK;
	mempool_free(pmb, phba->mbox_mem_pool);
	/*
	 * This shost reference might have been taken at the beginning of
	 * lpfc_vport_delete()
	 */
	if ((vport->load_flag & FC_UNLOADING) && (vport != phba->pport))
		scsi_host_put(shost);
}

int
lpfc_mbx_unreg_vpi(struct lpfc_vport *vport)
{
	struct lpfc_hba  *phba = vport->phba;
	LPFC_MBOXQ_t *mbox;
	int rc;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox)
		return 1;

	lpfc_unreg_vpi(phba, vport->vpi, mbox);
	mbox->vport = vport;
	mbox->mbox_cmpl = lpfc_mbx_cmpl_unreg_vpi;
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX | LOG_VPORT,
				 "1800 Could not issue unreg_vpi\n");
		mempool_free(mbox, phba->mbox_mem_pool);
		vport->unreg_vpi_cmpl = VPORT_ERROR;
		return rc;
	}
	return 0;
}

static void
lpfc_mbx_cmpl_reg_vpi(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_vport *vport = pmb->vport;
	struct Scsi_Host  *shost = lpfc_shost_from_vport(vport);
	MAILBOX_t *mb = &pmb->u.mb;

	switch (mb->mbxStatus) {
	case 0x0011:
	case 0x9601:
	case 0x9602:
		lpfc_printf_vlog(vport, KERN_INFO, LOG_NODE,
				 "0912 cmpl_reg_vpi, mb status = 0x%x\n",
				 mb->mbxStatus);
		lpfc_vport_set_state(vport, FC_VPORT_FAILED);
		spin_lock_irq(shost->host_lock);
		vport->fc_flag &= ~(FC_FABRIC | FC_PUBLIC_LOOP);
		spin_unlock_irq(shost->host_lock);
		vport->fc_myDID = 0;
		goto out;
	}

	vport->vpi_state |= LPFC_VPI_REGISTERED;
	vport->num_disc_nodes = 0;
	/* go thru NPR list and issue ELS PLOGIs */
	if (vport->fc_npr_cnt)
		lpfc_els_disc_plogi(vport);

	if (!vport->num_disc_nodes) {
		spin_lock_irq(shost->host_lock);
		vport->fc_flag &= ~FC_NDISC_ACTIVE;
		spin_unlock_irq(shost->host_lock);
		lpfc_can_disctmo(vport);
	}
	vport->port_state = LPFC_VPORT_READY;

out:
	mempool_free(pmb, phba->mbox_mem_pool);
	return;
}

/**
 * lpfc_create_static_vport - Read HBA config region to create static vports.
 * @phba: pointer to lpfc hba data structure.
 *
 * This routine issue a DUMP mailbox command for config region 22 to get
 * the list of static vports to be created. The function create vports
 * based on the information returned from the HBA.
 **/
void
lpfc_create_static_vport(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *pmb = NULL;
	MAILBOX_t *mb;
	struct static_vport_info *vport_info;
	int rc = 0, i;
	struct Scsi_Host *shost;
	struct lpfc_vport *vport;
	uint16_t offset = 0;
	uint8_t *vport_buff;
	struct lpfc_dmabuf *mp;
	uint32_t byte_count = 0;
	uint64_t wwpn, wwnn;
	uint8_t vname = '\0';


	pmb = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!pmb) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0542 lpfc_create_static_vport failed to"
				" allocate mailbox memory\n");
		return;
	}

	mb = &pmb->u.mb;

	vport_info = kzalloc(sizeof(struct static_vport_info), GFP_KERNEL);
	if (!vport_info) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"0543 lpfc_create_static_vport failed to"
				" allocate vport_info\n");
		mempool_free(pmb, phba->mbox_mem_pool);
		return;
	}

	vport_buff = (uint8_t *) vport_info;
	do {
		if (lpfc_dump_static_vport(phba, pmb, offset))
			goto out;

		pmb->vport = phba->pport;
		rc = lpfc_sli_issue_mbox_wait(phba, pmb, LPFC_MBOX_TMO);

		if ((rc != MBX_SUCCESS) || mb->mbxStatus) {
			lpfc_printf_log(phba, KERN_WARNING, LOG_INIT,
				"0544 lpfc_create_static_vport failed to"
				" issue dump mailbox command ret 0x%x "
				"status 0x%x\n",
				rc, mb->mbxStatus);
			goto out;
		}

		if (phba->sli_rev == LPFC_SLI_REV4) {
			byte_count = pmb->u.mqe.un.mb_words[5];
			mp = (struct lpfc_dmabuf *) pmb->context2;
			if (byte_count > sizeof(struct static_vport_info) -
					offset)
				byte_count = sizeof(struct static_vport_info)
					- offset;
			memcpy(vport_buff + offset, mp->virt, byte_count);
			offset += byte_count;
		} else {
			if (mb->un.varDmp.word_cnt >
				sizeof(struct static_vport_info) - offset)
				mb->un.varDmp.word_cnt =
					sizeof(struct static_vport_info)
						- offset;
			byte_count = mb->un.varDmp.word_cnt;
			lpfc_sli_pcimem_bcopy(((uint8_t *)mb) + DMP_RSP_OFFSET,
				vport_buff + offset,
				byte_count);

			offset += byte_count;
		}

	} while (byte_count &&
		offset < sizeof(struct static_vport_info));


	if ((le32_to_cpu(vport_info->signature) != VPORT_INFO_SIG) ||
		((le32_to_cpu(vport_info->rev) & VPORT_INFO_REV_MASK)
			!= VPORT_INFO_REV)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"0545 lpfc_create_static_vport bad"
			" information header 0x%x 0x%x\n",
			le32_to_cpu(vport_info->signature),
			le32_to_cpu(vport_info->rev) & VPORT_INFO_REV_MASK);

		goto out;
	}

	shost = lpfc_shost_from_vport(phba->pport);

	for (i = 0; i < MAX_STATIC_VPORT_COUNT; i++) {
		wwpn = wwn_to_u64(vport_info->vport_list[i].wwpn);
		wwnn = wwn_to_u64(vport_info->vport_list[i].wwnn);
		if (!wwpn || !wwnn)
			continue;
		rc = __lpfc_vport_create(shost, vport_info->vport_list[i].wwnn,
						vport_info->vport_list[i].wwpn,
						&vname, &vport);
		if (rc != VPORT_OK) {
			lpfc_printf_log(phba, KERN_WARNING, LOG_INIT,
				"0546 lpfc_create_static_vport failed to"
				" create vport\n");
			continue;
		}

		vport->vport_flag |= STATIC_VPORT;
	}
out:
	kfree(vport_info);
	if (rc != MBX_TIMEOUT) {
		if (pmb->context2) {
			mp = (struct lpfc_dmabuf *) pmb->context2;
			lpfc_mbuf_free(phba, mp->virt, mp->phys);
			kfree(mp);
		}
		mempool_free(pmb, phba->mbox_mem_pool);
	}

	return;
}

/*
 * This routine handles processing a Fabric REG_LOGIN mailbox
 * command upon completion. It is setup in the LPFC_MBOXQ
 * as the completion routine when the command is
 * handed off to the SLI layer.
 */
void
lpfc_mbx_cmpl_fabric_reg_login(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	struct lpfc_vport *vport = pmb->vport;
	MAILBOX_t *mb = &pmb->u.mb;
	struct lpfc_dmabuf *mp = (struct lpfc_dmabuf *) (pmb->context1);
	struct lpfc_nodelist *ndlp;

	ndlp = (struct lpfc_nodelist *) pmb->context2;
	pmb->context1 = NULL;
	pmb->context2 = NULL;
	if (mb->mbxStatus) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX,
				 "0258 Register Fabric login error: 0x%x\n",
				 mb->mbxStatus);
		lpfc_mbuf_free(phba, mp->virt, mp->phys);
		kfree(mp);
		mempool_free(pmb, phba->mbox_mem_pool);

		if (phba->fc_topology == TOPOLOGY_LOOP) {
			/* FLOGI failed, use loop map to make discovery list */
			lpfc_disc_list_loopmap(vport);

			/* Start discovery */
			lpfc_disc_start(vport);
			/* Decrement the reference count to ndlp after the
			 * reference to the ndlp are done.
			 */
			lpfc_nlp_put(ndlp);
			return;
		}

		lpfc_vport_set_state(vport, FC_VPORT_FAILED);
		/* Decrement the reference count to ndlp after the reference
		 * to the ndlp are done.
		 */
		lpfc_nlp_put(ndlp);
		return;
	}

	ndlp->nlp_rpi = mb->un.varWords[0];
	ndlp->nlp_flag |= NLP_RPI_VALID;
	ndlp->nlp_type |= NLP_FABRIC;
	lpfc_nlp_set_state(vport, ndlp, NLP_STE_UNMAPPED_NODE);

	if (vport->port_state == LPFC_FABRIC_CFG_LINK &&
		!vport->cfg_enable_auth)
		lpfc_start_discovery(vport);

	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mempool_free(pmb, phba->mbox_mem_pool);

	/* Drop the reference count from the mbox at the end after
	 * all the current reference to the ndlp have been done.
	 */
	lpfc_nlp_put(ndlp);
	return;
}

/*
 * This routine handles processing a NameServer REG_LOGIN mailbox
 * command upon completion. It is setup in the LPFC_MBOXQ
 * as the completion routine when the command is
 * handed off to the SLI layer.
 */
void
lpfc_mbx_cmpl_ns_reg_login(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	MAILBOX_t *mb = &pmb->u.mb;
	struct lpfc_dmabuf *mp = (struct lpfc_dmabuf *) (pmb->context1);
	struct lpfc_nodelist *ndlp = (struct lpfc_nodelist *) pmb->context2;
	struct lpfc_vport *vport = pmb->vport;

	if (mb->mbxStatus) {
out:
		lpfc_printf_vlog(vport, KERN_ERR, LOG_ELS,
				 "0260 Register NameServer error: 0x%x\n",
				 mb->mbxStatus);
		/* decrement the node reference count held for this
		 * callback function.
		 */
		lpfc_nlp_put(ndlp);
		lpfc_mbuf_free(phba, mp->virt, mp->phys);
		kfree(mp);
		mempool_free(pmb, phba->mbox_mem_pool);

		/* If no other thread is using the ndlp, free it */
		lpfc_nlp_not_used(ndlp);

		if (phba->fc_topology == TOPOLOGY_LOOP) {
			/*
			 * RegLogin failed, use loop map to make discovery
			 * list
			 */
			lpfc_disc_list_loopmap(vport);

			/* Start discovery */
			lpfc_disc_start(vport);
			return;
		}
		lpfc_vport_set_state(vport, FC_VPORT_FAILED);
		return;
	}

	pmb->context1 = NULL;

	ndlp->nlp_rpi = mb->un.varWords[0];
	ndlp->nlp_flag |= NLP_RPI_VALID;
	ndlp->nlp_type |= NLP_FABRIC;
	lpfc_nlp_set_state(vport, ndlp, NLP_STE_UNMAPPED_NODE);

	if (vport->port_state < LPFC_VPORT_READY) {
		/* Link up discovery requires Fabric registration. */
		lpfc_ns_cmd(vport, SLI_CTNS_RFF_ID, 0, 0); /* Do this first! */
		lpfc_ns_cmd(vport, SLI_CTNS_RNN_ID, 0, 0);
		lpfc_ns_cmd(vport, SLI_CTNS_RSNN_NN, 0, 0);
		lpfc_ns_cmd(vport, SLI_CTNS_RSPN_ID, 0, 0);
		lpfc_ns_cmd(vport, SLI_CTNS_RFT_ID, 0, 0);

		/* Issue SCR just before NameServer GID_FT Query */
		lpfc_issue_els_scr(vport, SCR_DID, 0);
	}

	vport->fc_ns_retry = 0;
	/* Good status, issue CT Request to NameServer */
	if (lpfc_ns_cmd(vport, SLI_CTNS_GID_FT, 0, 0)) {
		/* Cannot issue NameServer Query, so finish up discovery */
		goto out;
	}

	/* decrement the node reference count held for this
	 * callback function.
	 */
	lpfc_nlp_put(ndlp);
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mempool_free(pmb, phba->mbox_mem_pool);

	return;
}

static void
lpfc_register_remote_port(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct fc_rport  *rport;
	struct lpfc_rport_data *rdata;
	struct fc_rport_identifiers rport_ids;
	struct lpfc_hba  *phba = vport->phba;

	/* Remote port has reappeared. Re-register w/ FC transport */
	rport_ids.node_name = wwn_to_u64(ndlp->nlp_nodename.u.wwn);
	rport_ids.port_name = wwn_to_u64(ndlp->nlp_portname.u.wwn);
	rport_ids.port_id = ndlp->nlp_DID;
	rport_ids.roles = FC_RPORT_ROLE_UNKNOWN;

	/*
	 * We leave our node pointer in rport->dd_data when we unregister a
	 * FCP target port.  But fc_remote_port_add zeros the space to which
	 * rport->dd_data points.  So, if we're reusing a previously
	 * registered port, drop the reference that we took the last time we
	 * registered the port.
	 */
	if (ndlp->rport && ndlp->rport->dd_data &&
	    ((struct lpfc_rport_data *) ndlp->rport->dd_data)->pnode == ndlp)
		lpfc_nlp_put(ndlp);

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_RPORT,
		"rport add:       did:x%x flg:x%x type x%x",
		ndlp->nlp_DID, ndlp->nlp_flag, ndlp->nlp_type);

	ndlp->rport = rport = fc_remote_port_add(shost, 0, &rport_ids);
	if (!rport || !get_device(&rport->dev)) {
		dev_printk(KERN_WARNING, &phba->pcidev->dev,
			   "Warning: fc_remote_port_add failed\n");
		return;
	}

	/* initialize static port data */
	rport->maxframe_size = ndlp->nlp_maxframe;
	rport->supported_classes = ndlp->nlp_class_sup;
	rdata = rport->dd_data;
	rdata->pnode = lpfc_nlp_get(ndlp);

	if (ndlp->nlp_type & NLP_FCP_TARGET)
		rport_ids.roles |= FC_RPORT_ROLE_FCP_TARGET;
	if (ndlp->nlp_type & NLP_FCP_INITIATOR)
		rport_ids.roles |= FC_RPORT_ROLE_FCP_INITIATOR;
	del_timer_sync(&ndlp->nlp_initiator_tmr);


	if (rport_ids.roles !=  FC_RPORT_ROLE_UNKNOWN)
		fc_remote_port_rolechg(rport, rport_ids.roles);

	if ((rport->scsi_target_id != -1) &&
	    (rport->scsi_target_id < LPFC_MAX_TARGET)) {
		ndlp->nlp_sid = rport->scsi_target_id;
	}
	return;
}

static void
lpfc_unregister_remote_port(struct lpfc_nodelist *ndlp)
{
	struct fc_rport *rport = ndlp->rport;

	lpfc_debugfs_disc_trc(ndlp->vport, LPFC_DISC_TRC_RPORT,
		"rport delete:    did:x%x flg:x%x type x%x",
		ndlp->nlp_DID, ndlp->nlp_flag, ndlp->nlp_type);

	ndlp->unreg_time = jiffies;
	fc_remote_port_delete(rport);

	return;
}

static void
lpfc_nlp_counters(struct lpfc_vport *vport, int state, int count)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);

	spin_lock_irq(shost->host_lock);
	switch (state) {
	case NLP_STE_UNUSED_NODE:
		vport->fc_unused_cnt += count;
		break;
	case NLP_STE_PLOGI_ISSUE:
		vport->fc_plogi_cnt += count;
		break;
	case NLP_STE_ADISC_ISSUE:
		vport->fc_adisc_cnt += count;
		break;
	case NLP_STE_REG_LOGIN_ISSUE:
		vport->fc_reglogin_cnt += count;
		break;
	case NLP_STE_PRLI_ISSUE:
		vport->fc_prli_cnt += count;
		break;
	case NLP_STE_UNMAPPED_NODE:
		vport->fc_unmap_cnt += count;
		break;
	case NLP_STE_MAPPED_NODE:
		vport->fc_map_cnt += count;
		break;
	case NLP_STE_NPR_NODE:
		vport->fc_npr_cnt += count;
		break;
	}
	spin_unlock_irq(shost->host_lock);
}

static void
lpfc_nlp_state_cleanup(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp,
		       int old_state, int new_state)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);

	if (new_state == NLP_STE_UNMAPPED_NODE) {
		ndlp->nlp_type &= ~(NLP_FCP_TARGET | NLP_FCP_INITIATOR);
		ndlp->nlp_flag &= ~NLP_NODEV_REMOVE;
		ndlp->nlp_type |= NLP_FC_NODE;
	}
	if (new_state == NLP_STE_MAPPED_NODE)
		ndlp->nlp_flag &= ~NLP_NODEV_REMOVE;
	if (new_state == NLP_STE_NPR_NODE)
		ndlp->nlp_flag &= ~NLP_RCV_PLOGI;

	/* Transport interface */
	if (ndlp->rport && (old_state == NLP_STE_MAPPED_NODE ||
			    old_state == NLP_STE_UNMAPPED_NODE)) {
		vport->phba->nport_event_cnt++;
		lpfc_unregister_remote_port(ndlp);
	}

	if (new_state ==  NLP_STE_MAPPED_NODE ||
	    new_state == NLP_STE_UNMAPPED_NODE) {
		vport->phba->nport_event_cnt++;
		/*
		 * Tell the fc transport about the port, if we haven't
		 * already. If we have, and it's a scsi entity, be
		 * sure to unblock any attached scsi devices
		 */
		lpfc_register_remote_port(vport, ndlp);
	}
	if ((new_state ==  NLP_STE_MAPPED_NODE) &&
		(vport->stat_data_enabled)) {
		/*
		 * A new target is discovered, if there is no buffer for
		 * statistical data collection allocate buffer.
		 */
		ndlp->lat_data = kcalloc(LPFC_MAX_BUCKET_COUNT,
					 sizeof(struct lpfc_scsicmd_bkt),
					 GFP_KERNEL);

		if (!ndlp->lat_data)
			lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE,
				"0286 lpfc_nlp_state_cleanup failed to "
				"allocate statistical data buffer DID "
				"0x%x\n", ndlp->nlp_DID);
	}
	/*
	 * if we added to Mapped list, but the remote port
	 * registration failed or assigned a target id outside
	 * our presentable range - move the node to the
	 * Unmapped List
	 */
	if (new_state == NLP_STE_MAPPED_NODE &&
	    (!ndlp->rport ||
	     ndlp->rport->scsi_target_id == -1 ||
	     ndlp->rport->scsi_target_id >= LPFC_MAX_TARGET)) {
		spin_lock_irq(shost->host_lock);
		ndlp->nlp_flag |= NLP_TGT_NO_SCSIID;
		spin_unlock_irq(shost->host_lock);
		lpfc_nlp_set_state(vport, ndlp, NLP_STE_UNMAPPED_NODE);
	}
}

static char *
lpfc_nlp_state_name(char *buffer, size_t size, int state)
{
	static char *states[] = {
		[NLP_STE_UNUSED_NODE] = "UNUSED",
		[NLP_STE_PLOGI_ISSUE] = "PLOGI",
		[NLP_STE_ADISC_ISSUE] = "ADISC",
		[NLP_STE_REG_LOGIN_ISSUE] = "REGLOGIN",
		[NLP_STE_PRLI_ISSUE] = "PRLI",
		[NLP_STE_UNMAPPED_NODE] = "UNMAPPED",
		[NLP_STE_MAPPED_NODE] = "MAPPED",
		[NLP_STE_NPR_NODE] = "NPR",
	};

	if (state < NLP_STE_MAX_STATE && states[state])
		strlcpy(buffer, states[state], size);
	else
		snprintf(buffer, size, "unknown (%d)", state);
	return buffer;
}

void
lpfc_nlp_set_state(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp,
		   int state)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	int  old_state = ndlp->nlp_state;
	char name1[16], name2[16];

	lpfc_printf_vlog(vport, KERN_INFO, LOG_NODE,
			 "0904 NPort state transition x%06x, %s -> %s\n",
			 ndlp->nlp_DID,
			 lpfc_nlp_state_name(name1, sizeof(name1), old_state),
			 lpfc_nlp_state_name(name2, sizeof(name2), state));

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_NODE,
		"node statechg    did:x%x old:%d ste:%d",
		ndlp->nlp_DID, old_state, state);

	if (old_state == NLP_STE_NPR_NODE &&
	    state != NLP_STE_NPR_NODE)
		lpfc_cancel_retry_delay_tmo(vport, ndlp);
	if (old_state == NLP_STE_UNMAPPED_NODE) {
		ndlp->nlp_flag &= ~NLP_TGT_NO_SCSIID;
		ndlp->nlp_type &= ~NLP_FC_NODE;
	}

	if (list_empty(&ndlp->nlp_listp)) {
		spin_lock_irq(shost->host_lock);
		list_add_tail(&ndlp->nlp_listp, &vport->fc_nodes);
		spin_unlock_irq(shost->host_lock);
	} else if (old_state)
		lpfc_nlp_counters(vport, old_state, -1);

	ndlp->nlp_state = state;
	lpfc_nlp_counters(vport, state, 1);
	lpfc_nlp_state_cleanup(vport, ndlp, old_state, state);
}

void
lpfc_enqueue_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);

	if (list_empty(&ndlp->nlp_listp)) {
		spin_lock_irq(shost->host_lock);
		list_add_tail(&ndlp->nlp_listp, &vport->fc_nodes);
		spin_unlock_irq(shost->host_lock);
	}
}

void
lpfc_dequeue_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);

	lpfc_cancel_retry_delay_tmo(vport, ndlp);
	if (ndlp->nlp_state && !list_empty(&ndlp->nlp_listp))
		lpfc_nlp_counters(vport, ndlp->nlp_state, -1);
	spin_lock_irq(shost->host_lock);
	list_del_init(&ndlp->nlp_listp);
	spin_unlock_irq(shost->host_lock);
	lpfc_nlp_state_cleanup(vport, ndlp, ndlp->nlp_state,
				NLP_STE_UNUSED_NODE);
}

void
lpfc_disable_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	lpfc_cancel_retry_delay_tmo(vport, ndlp);
	if (ndlp->nlp_state && !list_empty(&ndlp->nlp_listp))
		lpfc_nlp_counters(vport, ndlp->nlp_state, -1);
	lpfc_nlp_state_cleanup(vport, ndlp, ndlp->nlp_state,
				NLP_STE_UNUSED_NODE);
}
/**
 * lpfc_initialize_node - Initialize all fields of node object
 * @vport: Pointer to Virtual Port object.
 * @ndlp: Pointer to FC node object.
 * @did: FC_ID of the node.
 *
 * This function is always called when node object need to be initialized.
 * It initializes all the fields of the node object. Although the reference
 * to phba from @ndlp can be obtained indirectly through it's reference to
 * @vport, a direct reference to phba is taken here by @ndlp. This is due
 * to the life-span of the @ndlp might go beyond the existence of @vport as
 * the final release of ndlp is determined by its reference count. And, the
 * operation on @ndlp needs the reference to phba.
 **/
static inline void
lpfc_initialize_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp,
	uint32_t did)
{
	INIT_LIST_HEAD(&ndlp->els_retry_evt.evt_listp);
	INIT_LIST_HEAD(&ndlp->dev_loss_evt.evt_listp);
	INIT_LIST_HEAD(&ndlp->els_reauth_evt.evt_listp);
	init_timer(&ndlp->nlp_initiator_tmr);
	ndlp->nlp_initiator_tmr.function = lpfc_dev_loss_delay;
	ndlp->nlp_initiator_tmr.data = (unsigned long)ndlp;
	init_timer(&ndlp->nlp_delayfunc);
	ndlp->nlp_delayfunc.function = lpfc_els_retry_delay;
	ndlp->nlp_delayfunc.data = (unsigned long)ndlp;
	ndlp->nlp_DID = did;
	init_timer(&ndlp->nlp_reauth_tmr);
	ndlp->nlp_reauth_tmr.function = lpfc_reauth_node;
	ndlp->nlp_reauth_tmr.data = (unsigned long)ndlp;
	ndlp->unreg_time = jiffies;
	ndlp->vport = vport;
	ndlp->phba = vport->phba;
	ndlp->nlp_sid = NLP_NO_SID;
	kref_init(&ndlp->kref);
	NLP_INT_NODE_ACT(ndlp);
	atomic_set(&ndlp->cmd_pending, 0);
	ndlp->cmd_qdepth = LPFC_MAX_TGT_QDEPTH;
}

struct lpfc_nodelist *
lpfc_enable_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp,
		 int state)
{
	struct lpfc_hba *phba = vport->phba;
	uint32_t did;
	unsigned long flags;

	if (!ndlp)
		return NULL;

	spin_lock_irqsave(&phba->ndlp_lock, flags);
	/* The ndlp should not be in memory free mode */
	if (NLP_CHK_FREE_REQ(ndlp)) {
		spin_unlock_irqrestore(&phba->ndlp_lock, flags);
		lpfc_printf_vlog(vport, KERN_WARNING, LOG_NODE,
				"0277 lpfc_enable_node: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
		return NULL;
	}
	/* The ndlp should not already be in active mode */
	if (NLP_CHK_NODE_ACT(ndlp)) {
		spin_unlock_irqrestore(&phba->ndlp_lock, flags);
		lpfc_printf_vlog(vport, KERN_WARNING, LOG_NODE,
				"0278 lpfc_enable_node: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
		return NULL;
	}

	/* Keep the original DID */
	did = ndlp->nlp_DID;

	/* re-initialize ndlp except of ndlp linked list pointer */
	memset((((char *)ndlp) + sizeof (struct list_head)), 0,
		sizeof (struct lpfc_nodelist) - sizeof (struct list_head));
	lpfc_initialize_node(vport, ndlp, did);

	spin_unlock_irqrestore(&phba->ndlp_lock, flags);

	if (state != NLP_STE_UNUSED_NODE)
		lpfc_nlp_set_state(vport, ndlp, state);

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_NODE,
		"node enable:       did:x%x",
		ndlp->nlp_DID, 0, 0);
	return ndlp;
}

void
lpfc_drop_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	/*
	 * Use of lpfc_drop_node and UNUSED list: lpfc_drop_node should
	 * be used if we wish to issue the "last" lpfc_nlp_put() to remove
	 * the ndlp from the vport. The ndlp marked as UNUSED on the list
	 * until ALL other outstanding threads have completed. We check
	 * that the ndlp not already in the UNUSED state before we proceed.
	 */
	if (ndlp->nlp_state == NLP_STE_UNUSED_NODE)
		return;
	lpfc_nlp_set_state(vport, ndlp, NLP_STE_UNUSED_NODE);
	lpfc_nlp_put(ndlp);
	return;
}

/*
 * Start / ReStart rescue timer for Discovery / RSCN handling
 */
void
lpfc_set_disctmo(struct lpfc_vport *vport)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_hba  *phba = vport->phba;
	uint32_t tmo;

	if (vport->port_state == LPFC_LOCAL_CFG_LINK) {
		/* For FAN, timeout should be greater than edtov */
		tmo = (((phba->fc_edtov + 999) / 1000) + 1);
	} else {
		/* Normal discovery timeout should be > than ELS/CT timeout
		 * FC spec states we need 3 * ratov for CT requests
		 */
		tmo = ((phba->fc_ratov * 3) + 3);
	}


	if (!timer_pending(&vport->fc_disctmo)) {
		lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_ELS_CMD,
			"set disc timer:  tmo:x%x state:x%x flg:x%x",
			tmo, vport->port_state, vport->fc_flag);
	}

	mod_timer(&vport->fc_disctmo, jiffies + HZ * tmo);
	spin_lock_irq(shost->host_lock);
	vport->fc_flag |= FC_DISC_TMO;
	spin_unlock_irq(shost->host_lock);

	/* Start Discovery Timer state <hba_state> */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_DISCOVERY,
			 "0247 Start Discovery Timer state x%x "
			 "Data: x%x x%lx x%x x%x\n",
			 vport->port_state, tmo,
			 (unsigned long)&vport->fc_disctmo, vport->fc_plogi_cnt,
			 vport->fc_adisc_cnt);

	return;
}

/*
 * Cancel rescue timer for Discovery / RSCN handling
 */
int
lpfc_can_disctmo(struct lpfc_vport *vport)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	unsigned long iflags;

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_ELS_CMD,
		"can disc timer:  state:x%x rtry:x%x flg:x%x",
		vport->port_state, vport->fc_ns_retry, vport->fc_flag);

	/* Turn off discovery timer if its running */
	if (vport->fc_flag & FC_DISC_TMO) {
		spin_lock_irqsave(shost->host_lock, iflags);
		vport->fc_flag &= ~FC_DISC_TMO;
		spin_unlock_irqrestore(shost->host_lock, iflags);
		del_timer_sync(&vport->fc_disctmo);
		spin_lock_irqsave(&vport->work_port_lock, iflags);
		vport->work_port_events &= ~WORKER_DISC_TMO;
		spin_unlock_irqrestore(&vport->work_port_lock, iflags);
	}

	/* Cancel Discovery Timer state <hba_state> */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_DISCOVERY,
			 "0248 Cancel Discovery Timer state x%x "
			 "Data: x%x x%x x%x\n",
			 vport->port_state, vport->fc_flag,
			 vport->fc_plogi_cnt, vport->fc_adisc_cnt);
	return 0;
}

/*
 * Check specified ring for outstanding IOCB on the SLI queue
 * Return true if iocb matches the specified nport
 */
int
lpfc_check_sli_ndlp(struct lpfc_hba *phba,
		    struct lpfc_sli_ring *pring,
		    struct lpfc_iocbq *iocb,
		    struct lpfc_nodelist *ndlp)
{
	struct lpfc_sli *psli = &phba->sli;
	IOCB_t *icmd = &iocb->iocb;
	struct lpfc_vport    *vport = ndlp->vport;

	if (iocb->vport != vport)
		return 0;

	if (pring->ringno == LPFC_ELS_RING) {
		switch (icmd->ulpCommand) {
		case CMD_GEN_REQUEST64_CR:
			if (iocb->context_un.ndlp == ndlp)
				return 1;
		case CMD_ELS_REQUEST64_CR:
			if (icmd->un.elsreq64.remoteID == ndlp->nlp_DID)
				return 1;
		case CMD_XMIT_ELS_RSP64_CX:
			if (iocb->context1 == (uint8_t *) ndlp)
				return 1;
		}
	} else if (pring->ringno == psli->extra_ring) {

	} else if (pring->ringno == psli->fcp_ring) {
		/* Skip match check if waiting to relogin to FCP target */
		if ((ndlp->nlp_type & NLP_FCP_TARGET) &&
		    (ndlp->nlp_flag & NLP_DELAY_TMO)) {
			return 0;
		}
		if (icmd->ulpContext == (volatile ushort)ndlp->nlp_rpi) {
			return 1;
		}
	} else if (pring->ringno == psli->next_ring) {

	}
	return 0;
}

/*
 * Free resources / clean up outstanding I/Os
 * associated with nlp_rpi in the LPFC_NODELIST entry.
 */
static int
lpfc_no_rpi(struct lpfc_hba *phba, struct lpfc_nodelist *ndlp)
{
	LIST_HEAD(completions);
	struct lpfc_sli *psli;
	struct lpfc_sli_ring *pring;
	struct lpfc_iocbq *iocb, *next_iocb;
	uint32_t i;

	lpfc_fabric_abort_nport(ndlp);

	/*
	 * Everything that matches on txcmplq will be returned
	 * by firmware with a no rpi error.
	 */
	psli = &phba->sli;
	if (ndlp->nlp_flag & NLP_RPI_VALID) {
		/* Now process each ring */
		for (i = 0; i < psli->num_rings; i++) {
			pring = &psli->ring[i];

			spin_lock_irq(&phba->hbalock);
			list_for_each_entry_safe(iocb, next_iocb, &pring->txq,
						 list) {
				/*
				 * Check to see if iocb matches the nport we are
				 * looking for
				 */
				if ((lpfc_check_sli_ndlp(phba, pring, iocb,
							 ndlp))) {
					/* It matches, so deque and call compl
					   with an error */
					list_move_tail(&iocb->list,
						       &completions);
					pring->txq_cnt--;
				}
			}
			spin_unlock_irq(&phba->hbalock);
		}
	}

	/* Cancel all the IOCBs from the completions list */
	lpfc_sli_cancel_iocbs(phba, &completions, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_ABORTED);

	return 0;
}

/*
 * Free rpi associated with LPFC_NODELIST entry.
 * This routine is called from lpfc_freenode(), when we are removing
 * a LPFC_NODELIST entry. It is also called if the driver initiates a
 * LOGO that completes successfully, and we are waiting to PLOGI back
 * to the remote NPort. In addition, it is called after we receive
 * and unsolicated ELS cmd, send back a rsp, the rsp completes and
 * we are waiting to PLOGI back to the remote NPort.
 */
int
lpfc_unreg_rpi(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	struct lpfc_hba *phba = vport->phba;
	LPFC_MBOXQ_t    *mbox;
	int rc;

	if (ndlp->nlp_flag & NLP_RPI_VALID) {
		mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
		if (mbox) {
			lpfc_unreg_login(phba, vport->vpi, ndlp->nlp_rpi, mbox);
			mbox->vport = vport;
			mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
			rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);
			if (rc == MBX_NOT_FINISHED)
				mempool_free(mbox, phba->mbox_mem_pool);
		}
		lpfc_no_rpi(phba, ndlp);
		ndlp->nlp_rpi = 0;
		ndlp->nlp_flag &= ~NLP_RPI_VALID;
		ndlp->nlp_flag &= ~NLP_NPR_ADISC;
		return 1;
	}
	return 0;
}

void
lpfc_unreg_all_rpis(struct lpfc_vport *vport)
{
	struct lpfc_hba  *phba  = vport->phba;
	LPFC_MBOXQ_t     *mbox;
	int rc;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (mbox) {
		lpfc_unreg_login(phba, vport->vpi, 0xffff, mbox);
		mbox->vport = vport;
		mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		mbox->context1 = NULL;
		rc = lpfc_sli_issue_mbox_wait(phba, mbox, LPFC_MBOX_TMO);
		if (rc != MBX_TIMEOUT)
			mempool_free(mbox, phba->mbox_mem_pool);

		if ((rc == MBX_TIMEOUT) || (rc == MBX_NOT_FINISHED))
			lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX | LOG_VPORT,
				"1836 Could not issue "
				"unreg_login(all_rpis) status %d\n", rc);
	}
}

void
lpfc_unreg_default_rpis(struct lpfc_vport *vport)
{
	struct lpfc_hba  *phba  = vport->phba;
	LPFC_MBOXQ_t     *mbox;
	int rc;

	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (mbox) {
		lpfc_unreg_did(phba, vport->vpi, 0xffffffff, mbox);
		mbox->vport = vport;
		mbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		mbox->context1 = NULL;
		rc = lpfc_sli_issue_mbox_wait(phba, mbox, LPFC_MBOX_TMO);
		if (rc != MBX_TIMEOUT)
			mempool_free(mbox, phba->mbox_mem_pool);

		if ((rc == MBX_TIMEOUT) || (rc == MBX_NOT_FINISHED))
			lpfc_printf_vlog(vport, KERN_ERR, LOG_MBOX | LOG_VPORT,
					 "1815 Could not issue "
					 "unreg_did (default rpis) status %d\n",
					 rc);
	}
}

/*
 * Free resources associated with LPFC_NODELIST entry
 * so it can be freed.
 */
static int
lpfc_cleanup_node(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_hba  *phba = vport->phba;
	LPFC_MBOXQ_t *mb, *nextmb;
	struct lpfc_dmabuf *mp;

	/* Cleanup node for NPort <nlp_DID> */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NODE,
			 "0900 Cleanup node for NPort x%x "
			 "Data: x%x x%x x%x\n",
			 ndlp->nlp_DID, ndlp->nlp_flag,
			 ndlp->nlp_state, ndlp->nlp_rpi);
	if (NLP_CHK_FREE_REQ(ndlp)) {
		lpfc_printf_vlog(vport, KERN_WARNING, LOG_NODE,
				"0280 lpfc_cleanup_node: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
		lpfc_dequeue_node(vport, ndlp);
	} else {
		lpfc_printf_vlog(vport, KERN_WARNING, LOG_NODE,
				"0281 lpfc_cleanup_node: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
		lpfc_disable_node(vport, ndlp);
	}

	/* cleanup any ndlp on mbox q waiting for reglogin cmpl */
	if ((mb = phba->sli.mbox_active)) {
		if ((mb->u.mb.mbxCommand == MBX_REG_LOGIN64) &&
		   (ndlp == (struct lpfc_nodelist *) mb->context2)) {
			mb->context2 = NULL;
			mb->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		}
	}

	spin_lock_irq(&phba->hbalock);
	list_for_each_entry_safe(mb, nextmb, &phba->sli.mboxq, list) {
		if ((mb->u.mb.mbxCommand == MBX_REG_LOGIN64) &&
		    (ndlp == (struct lpfc_nodelist *) mb->context2)) {
			mp = (struct lpfc_dmabuf *) (mb->context1);
			if (mp) {
				__lpfc_mbuf_free(phba, mp->virt, mp->phys);
				kfree(mp);
			}
			list_del(&mb->list);
			mempool_free(mb, phba->mbox_mem_pool);
			/* We shall not invoke the lpfc_nlp_put to decrement
			 * the ndlp reference count as we are in the process
			 * of lpfc_nlp_release.
			 */
		}
	}
	spin_unlock_irq(&phba->hbalock);

	lpfc_els_abort(phba, ndlp);

	spin_lock_irq(shost->host_lock);
	ndlp->nlp_flag &= ~NLP_DELAY_TMO;
	spin_unlock_irq(shost->host_lock);

	ndlp->nlp_last_elscmd = 0;
	del_timer_sync(&ndlp->nlp_delayfunc);
	del_timer_sync(&ndlp->nlp_reauth_tmr);
	del_timer_sync(&ndlp->nlp_initiator_tmr);

	list_del_init(&ndlp->els_retry_evt.evt_listp);
	list_del_init(&ndlp->dev_loss_evt.evt_listp);
	if (!list_empty(&ndlp->els_reauth_evt.evt_listp))
		list_del_init(&ndlp->els_reauth_evt.evt_listp);

	lpfc_unreg_rpi(vport, ndlp);

	return 0;
}

/*
 * Check to see if we can free the nlp back to the freelist.
 * If we are in the middle of using the nlp in the discovery state
 * machine, defer the free till we reach the end of the state machine.
 */
static void
lpfc_nlp_remove(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp)
{
	struct lpfc_hba  *phba = vport->phba;
	struct lpfc_rport_data *rdata;
	LPFC_MBOXQ_t *mbox;
	int rc;

	lpfc_cancel_retry_delay_tmo(vport, ndlp);
	if ((ndlp->nlp_flag & NLP_DEFER_RM) &&
	    !(ndlp->nlp_flag & NLP_RPI_VALID)) {
		/* For this case we need to cleanup the default rpi
		 * allocated by the firmware.
		 */
		if ((mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL))
			!= NULL) {
			rc = lpfc_reg_rpi(phba, vport->vpi, ndlp->nlp_DID,
			    (uint8_t *) &vport->fc_sparam, mbox, 0);
			if (rc) {
				mempool_free(mbox, phba->mbox_mem_pool);
			}
			else {
				mbox->mbox_flag |= LPFC_MBX_IMED_UNREG;
				mbox->mbox_cmpl = lpfc_mbx_cmpl_dflt_rpi;
				mbox->vport = vport;
				mbox->context2 = NULL;
				rc =lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);
				if (rc == MBX_NOT_FINISHED) {
					mempool_free(mbox, phba->mbox_mem_pool);
				}
			}
		}
	}
	lpfc_cleanup_node(vport, ndlp);

	/*
	 * We can get here with a non-NULL ndlp->rport because when we
	 * unregister a rport we don't break the rport/node linkage.  So if we
	 * do, make sure we don't leaving any dangling pointers behind.
	 */
	if (ndlp->rport) {
		rdata = ndlp->rport->dd_data;
		rdata->pnode = NULL;
		ndlp->rport = NULL;
	}
}

static int
lpfc_matchdid(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp,
	      uint32_t did)
{
	D_ID mydid, ndlpdid, matchdid;

	if (did == Bcast_DID)
		return 0;

	/* First check for Direct match */
	if (ndlp->nlp_DID == did)
		return 1;

	/* Next check for area/domain identically equals 0 match */
	mydid.un.word = vport->fc_myDID;
	if ((mydid.un.b.domain == 0) && (mydid.un.b.area == 0)) {
		return 0;
	}

	matchdid.un.word = did;
	ndlpdid.un.word = ndlp->nlp_DID;
	if (matchdid.un.b.id == ndlpdid.un.b.id) {
		if ((mydid.un.b.domain == matchdid.un.b.domain) &&
		    (mydid.un.b.area == matchdid.un.b.area)) {
			if ((ndlpdid.un.b.domain == 0) &&
			    (ndlpdid.un.b.area == 0)) {
				if (ndlpdid.un.b.id)
					return 1;
			}
			return 0;
		}

		matchdid.un.word = ndlp->nlp_DID;
		if ((mydid.un.b.domain == ndlpdid.un.b.domain) &&
		    (mydid.un.b.area == ndlpdid.un.b.area)) {
			if ((matchdid.un.b.domain == 0) &&
			    (matchdid.un.b.area == 0)) {
				if (matchdid.un.b.id)
					return 1;
			}
		}
	}
	return 0;
}

/* Search for a nodelist entry */
static struct lpfc_nodelist *
__lpfc_findnode_did(struct lpfc_vport *vport, uint32_t did)
{
	struct lpfc_nodelist *ndlp;
	uint32_t data1;

	list_for_each_entry(ndlp, &vport->fc_nodes, nlp_listp) {
		if (lpfc_matchdid(vport, ndlp, did)) {
			data1 = (((uint32_t) ndlp->nlp_state << 24) |
				 ((uint32_t) ndlp->nlp_xri << 16) |
				 ((uint32_t) ndlp->nlp_type << 8) |
				 ((uint32_t) ndlp->nlp_rpi & 0xff));
			lpfc_printf_vlog(vport, KERN_INFO, LOG_NODE,
					 "0929 FIND node DID "
					 "Data: x%p x%x x%x x%x\n",
					 ndlp, ndlp->nlp_DID,
					 ndlp->nlp_flag, data1);
			return ndlp;
		}
	}

	/* FIND node did <did> NOT FOUND */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NODE,
			 "0932 FIND node did x%x NOT FOUND.\n", did);
	return NULL;
}

struct lpfc_nodelist *
lpfc_findnode_did(struct lpfc_vport *vport, uint32_t did)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_nodelist *ndlp;

	spin_lock_irq(shost->host_lock);
	ndlp = __lpfc_findnode_did(vport, did);
	spin_unlock_irq(shost->host_lock);
	return ndlp;
}

struct lpfc_nodelist *
lpfc_setup_disc_node(struct lpfc_vport *vport, uint32_t did)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_nodelist *ndlp;

	ndlp = lpfc_findnode_did(vport, did);
	if (!ndlp) {
		if ((vport->fc_flag & FC_RSCN_MODE) != 0 &&
		    lpfc_rscn_payload_check(vport, did) == 0)
			return NULL;
		ndlp = (struct lpfc_nodelist *)
		     mempool_alloc(vport->phba->nlp_mem_pool, GFP_KERNEL);
		if (!ndlp)
			return NULL;
		lpfc_nlp_init(vport, ndlp, did);
		lpfc_nlp_set_state(vport, ndlp, NLP_STE_NPR_NODE);
		spin_lock_irq(shost->host_lock);
		ndlp->nlp_flag |= NLP_NPR_2B_DISC;
		spin_unlock_irq(shost->host_lock);
		return ndlp;
	} else if (!NLP_CHK_NODE_ACT(ndlp)) {
		ndlp = lpfc_enable_node(vport, ndlp, NLP_STE_NPR_NODE);
		if (!ndlp)
			return NULL;
		spin_lock_irq(shost->host_lock);
		ndlp->nlp_flag |= NLP_NPR_2B_DISC;
		spin_unlock_irq(shost->host_lock);
		return ndlp;
	}

	if ((vport->fc_flag & FC_RSCN_MODE) &&
	    !(vport->fc_flag & FC_NDISC_ACTIVE)) {
		if (lpfc_rscn_payload_check(vport, did)) {
			/* If we've already recieved a PLOGI from this NPort
			 * we don't need to try to discover it again.
			 */
			if (ndlp->nlp_flag & NLP_RCV_PLOGI)
				return NULL;

			/* Since this node is marked for discovery,
			 * delay timeout is not needed.
			 */
			lpfc_cancel_retry_delay_tmo(vport, ndlp);
			spin_lock_irq(shost->host_lock);
			ndlp->nlp_flag |= NLP_NPR_2B_DISC;
			spin_unlock_irq(shost->host_lock);
		} else
			ndlp = NULL;
	} else {
		/* If we've already recieved a PLOGI from this NPort,
		 * or we are already in the process of discovery on it,
		 * we don't need to try to discover it again.
		 */
		if (ndlp->nlp_state == NLP_STE_ADISC_ISSUE ||
		    ndlp->nlp_state == NLP_STE_PLOGI_ISSUE ||
		    ndlp->nlp_flag & NLP_RCV_PLOGI)
			return NULL;
		lpfc_nlp_set_state(vport, ndlp, NLP_STE_NPR_NODE);
		spin_lock_irq(shost->host_lock);
		ndlp->nlp_flag |= NLP_NPR_2B_DISC;
		spin_unlock_irq(shost->host_lock);
	}
	return ndlp;
}

/* Build a list of nodes to discover based on the loopmap */
void
lpfc_disc_list_loopmap(struct lpfc_vport *vport)
{
	struct lpfc_hba  *phba = vport->phba;
	int j;
	uint32_t alpa, index;

	if (!lpfc_is_link_up(phba))
		return;

	if (phba->fc_topology != TOPOLOGY_LOOP)
		return;

	if ((PCI_DEVICE_ID_HORNET == phba->pcidev->device)
		&& (phba->link_flag & LS_LOOPBACK_MODE))
		return;

	/* Check for loop map present or not */
	if (phba->alpa_map[0]) {
		for (j = 1; j <= phba->alpa_map[0]; j++) {
			alpa = phba->alpa_map[j];
			if (((vport->fc_myDID & 0xff) == alpa) || (alpa == 0))
				continue;
			lpfc_setup_disc_node(vport, alpa);
		}
	} else {
		/* No alpamap, so try all alpa's */
		for (j = 0; j < FC_MAXLOOP; j++) {
			/* If cfg_scan_down is set, start from highest
			 * ALPA (0xef) to lowest (0x1).
			 */
			if (vport->cfg_scan_down)
				index = j;
			else
				index = FC_MAXLOOP - j - 1;
			alpa = lpfcAlpaArray[index];
			if ((vport->fc_myDID & 0xff) == alpa)
				continue;
			if (!(phba->link_flag & LS_LOOPBACK_MODE))
				lpfc_setup_disc_node(vport, alpa);
		}
	}
	return;
}

void
lpfc_issue_clear_la(struct lpfc_hba *phba, struct lpfc_vport *vport)
{
	LPFC_MBOXQ_t *mbox;
	struct lpfc_sli *psli = &phba->sli;
	struct lpfc_sli_ring *extra_ring = &psli->ring[psli->extra_ring];
	struct lpfc_sli_ring *fcp_ring   = &psli->ring[psli->fcp_ring];
	struct lpfc_sli_ring *next_ring  = &psli->ring[psli->next_ring];
	int  rc;

	/*
	 * if it's not a physical port or if we already send
	 * clear_la then don't send it.
	 */
	if ((phba->link_state >= LPFC_CLEAR_LA) ||
	    (vport->port_type != LPFC_PHYSICAL_PORT) ||
		(phba->sli_rev == LPFC_SLI_REV4))
		return;

			/* Link up discovery */
	if ((mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL)) != NULL) {
		phba->link_state = LPFC_CLEAR_LA;
		lpfc_clear_la(phba, mbox);
		mbox->mbox_cmpl = lpfc_mbx_cmpl_clear_la;
		mbox->vport = vport;
		rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);
		if (rc == MBX_NOT_FINISHED) {
			mempool_free(mbox, phba->mbox_mem_pool);
			lpfc_disc_flush_list(vport);
			extra_ring->flag &= ~LPFC_STOP_IOCB_EVENT;
			fcp_ring->flag &= ~LPFC_STOP_IOCB_EVENT;
			next_ring->flag &= ~LPFC_STOP_IOCB_EVENT;
			phba->link_state = LPFC_HBA_ERROR;
		}
	}
}

/* Reg_vpi to tell firmware to resume normal operations */
void
lpfc_issue_reg_vpi(struct lpfc_hba *phba, struct lpfc_vport *vport)
{
	LPFC_MBOXQ_t *regvpimbox;

	regvpimbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (regvpimbox) {
		lpfc_reg_vpi(vport, regvpimbox);
		regvpimbox->mbox_cmpl = lpfc_mbx_cmpl_reg_vpi;
		regvpimbox->vport = vport;
		if (lpfc_sli_issue_mbox(phba, regvpimbox, MBX_NOWAIT)
					== MBX_NOT_FINISHED) {
			mempool_free(regvpimbox, phba->mbox_mem_pool);
		}
	}
}

/* Start Link up / RSCN discovery on NPR nodes */
void
lpfc_disc_start(struct lpfc_vport *vport)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_hba  *phba = vport->phba;
	uint32_t num_sent;
	uint32_t clear_la_pending;
	int did_changed;

	if (!lpfc_is_link_up(phba))
		return;

	if (phba->link_state == LPFC_CLEAR_LA)
		clear_la_pending = 1;
	else
		clear_la_pending = 0;

	if (vport->port_state < LPFC_VPORT_READY)
		vport->port_state = LPFC_DISC_AUTH;

	lpfc_set_disctmo(vport);

	if (vport->fc_prevDID == vport->fc_myDID)
		did_changed = 0;
	else
		did_changed = 1;

	vport->fc_prevDID = vport->fc_myDID;
	vport->num_disc_nodes = 0;

	/* Start Discovery state <hba_state> */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_DISCOVERY,
			 "0202 Start Discovery hba state x%x "
			 "Data: x%x x%x x%x\n",
			 vport->port_state, vport->fc_flag, vport->fc_plogi_cnt,
			 vport->fc_adisc_cnt);

	/* First do ADISCs - if any */
	num_sent = lpfc_els_disc_adisc(vport);

	if (num_sent)
		return;

	/*
	 * For SLI3, cmpl_reg_vpi will set port_state to READY, and
	 * continue discovery.
	 */
	if ((phba->sli3_options & LPFC_SLI3_NPIV_ENABLED) &&
	    !(vport->fc_flag & FC_PT2PT) &&
	    !(vport->fc_flag & FC_RSCN_MODE) &&
	    (phba->sli_rev < LPFC_SLI_REV4)) {
		lpfc_issue_reg_vpi(phba, vport);
		return;
	}

	/*
	 * For SLI2, we need to set port_state to READY and continue
	 * discovery.
	 */
	if (vport->port_state < LPFC_VPORT_READY && !clear_la_pending) {
		/* If we get here, there is nothing to ADISC */
		if (vport->port_type == LPFC_PHYSICAL_PORT)
			lpfc_issue_clear_la(phba, vport);

		if (!(vport->fc_flag & FC_ABORT_DISCOVERY)) {
			vport->num_disc_nodes = 0;
			/* go thru NPR nodes and issue ELS PLOGIs */
			if (vport->fc_npr_cnt)
				lpfc_els_disc_plogi(vport);

			if (!vport->num_disc_nodes) {
				spin_lock_irq(shost->host_lock);
				vport->fc_flag &= ~FC_NDISC_ACTIVE;
				spin_unlock_irq(shost->host_lock);
				lpfc_can_disctmo(vport);
			}
		}
		vport->port_state = LPFC_VPORT_READY;
	} else {
		/* Next do PLOGIs - if any */
		num_sent = lpfc_els_disc_plogi(vport);

		if (num_sent)
			return;

		if (vport->fc_flag & FC_RSCN_MODE) {
			/* Check to see if more RSCNs came in while we
			 * were processing this one.
			 */
			if ((vport->fc_rscn_id_cnt == 0) &&
			    (!(vport->fc_flag & FC_RSCN_DISCOVERY))) {
				spin_lock_irq(shost->host_lock);
				vport->fc_flag &= ~FC_RSCN_MODE;
				spin_unlock_irq(shost->host_lock);
				lpfc_can_disctmo(vport);
			} else
				lpfc_els_handle_rscn(vport);
		}
	}
	return;
}

/*
 *  Ignore completion for all IOCBs on tx and txcmpl queue for ELS
 *  ring the match the sppecified nodelist.
 */
static void
lpfc_free_tx(struct lpfc_hba *phba, struct lpfc_nodelist *ndlp)
{
	LIST_HEAD(completions);
	struct lpfc_sli *psli;
	IOCB_t     *icmd;
	struct lpfc_iocbq    *iocb, *next_iocb;
	struct lpfc_sli_ring *pring;

	psli = &phba->sli;
	pring = &psli->ring[LPFC_ELS_RING];

	/* Error matching iocb on txq or txcmplq
	 * First check the txq.
	 */
	spin_lock_irq(&phba->hbalock);
	list_for_each_entry_safe(iocb, next_iocb, &pring->txq, list) {
		if (iocb->context1 != ndlp) {
			continue;
		}
		icmd = &iocb->iocb;
		if ((icmd->ulpCommand == CMD_ELS_REQUEST64_CR) ||
		    (icmd->ulpCommand == CMD_XMIT_ELS_RSP64_CX)) {

			list_move_tail(&iocb->list, &completions);
			pring->txq_cnt--;
		}
	}

	/* Next check the txcmplq */
	list_for_each_entry_safe(iocb, next_iocb, &pring->txcmplq, list) {
		if (iocb->context1 != ndlp) {
			continue;
		}
		icmd = &iocb->iocb;
		if (icmd->ulpCommand == CMD_ELS_REQUEST64_CR ||
		    icmd->ulpCommand == CMD_XMIT_ELS_RSP64_CX) {
			lpfc_sli_issue_abort_iotag(phba, pring, iocb);
		}
	}
	spin_unlock_irq(&phba->hbalock);

	/* Cancel all the IOCBs from the completions list */
	lpfc_sli_cancel_iocbs(phba, &completions, IOSTAT_LOCAL_REJECT,
			      IOERR_SLI_ABORTED);
}

void
lpfc_disc_flush_list(struct lpfc_vport *vport)
{
	struct lpfc_nodelist *ndlp, *next_ndlp;
	struct lpfc_hba *phba = vport->phba;

	if (vport->fc_plogi_cnt || vport->fc_adisc_cnt) {
		list_for_each_entry_safe(ndlp, next_ndlp, &vport->fc_nodes,
					 nlp_listp) {
			if (!NLP_CHK_NODE_ACT(ndlp))
				continue;
			if (ndlp->nlp_state == NLP_STE_PLOGI_ISSUE ||
			    ndlp->nlp_state == NLP_STE_ADISC_ISSUE)
				lpfc_free_tx(phba, ndlp);
		}
	}
}

void
lpfc_cleanup_discovery_resources(struct lpfc_vport *vport)
{
	lpfc_els_flush_rscn(vport);
	lpfc_els_flush_cmd(vport);
	lpfc_disc_flush_list(vport);
}

/*****************************************************************************/
/*
 * NAME:     lpfc_disc_timeout
 *
 * FUNCTION: Fibre Channel driver discovery timeout routine.
 *
 * EXECUTION ENVIRONMENT: interrupt only
 *
 * CALLED FROM:
 *      Timer function
 *
 * RETURNS:
 *      none
 */
/*****************************************************************************/
void
lpfc_disc_timeout(unsigned long ptr)
{
	struct lpfc_vport *vport = (struct lpfc_vport *) ptr;
	struct lpfc_hba   *phba = vport->phba;
	uint32_t tmo_posted;
	unsigned long flags = 0;

	if (unlikely(!phba))
		return;

	spin_lock_irqsave(&vport->work_port_lock, flags);
	tmo_posted = vport->work_port_events & WORKER_DISC_TMO;
	if (!tmo_posted)
		vport->work_port_events |= WORKER_DISC_TMO;
	spin_unlock_irqrestore(&vport->work_port_lock, flags);

	if (!tmo_posted)
		lpfc_worker_wake_up(phba);
	return;
}

static void
lpfc_disc_timeout_handler(struct lpfc_vport *vport)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_hba  *phba = vport->phba;
	struct lpfc_sli  *psli = &phba->sli;
	struct lpfc_nodelist *ndlp, *next_ndlp;
	LPFC_MBOXQ_t *initlinkmbox;
	int rc, clrlaerr = 0;

	if (!(vport->fc_flag & FC_DISC_TMO))
		return;

	spin_lock_irq(shost->host_lock);
	vport->fc_flag &= ~FC_DISC_TMO;
	spin_unlock_irq(shost->host_lock);

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_ELS_CMD,
		"disc timeout:    state:x%x rtry:x%x flg:x%x",
		vport->port_state, vport->fc_ns_retry, vport->fc_flag);

	switch (vport->port_state) {

	case LPFC_LOCAL_CFG_LINK:
	/* port_state is identically  LPFC_LOCAL_CFG_LINK while waiting for
	 * FAN
	 */
				/* FAN timeout */
		lpfc_printf_vlog(vport, KERN_WARNING, LOG_DISCOVERY,
				 "0221 FAN timeout\n");
		/* Start discovery by sending FLOGI, clean up old rpis */
		list_for_each_entry_safe(ndlp, next_ndlp, &vport->fc_nodes,
					 nlp_listp) {
			if (!NLP_CHK_NODE_ACT(ndlp))
				continue;
			if (ndlp->nlp_state != NLP_STE_NPR_NODE)
				continue;
			if (ndlp->nlp_type & NLP_FABRIC) {
				/* Clean up the ndlp on Fabric connections */
				lpfc_drop_node(vport, ndlp);

			} else if (!(ndlp->nlp_flag & NLP_NPR_ADISC)) {
				/* Fail outstanding IO now since device
				 * is marked for PLOGI.
				 */
				lpfc_unreg_rpi(vport, ndlp);
			}
		}
		if (vport->port_state != LPFC_FLOGI) {
			lpfc_initial_flogi(vport);
			return;
		}
		break;

	case LPFC_FDISC:
	case LPFC_FLOGI:
	/* port_state is identically LPFC_FLOGI while waiting for FLOGI cmpl */
		/* Initial FLOGI timeout */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0222 Initial %s timeout\n",
				 vport->vpi ? "FDISC" : "FLOGI");

		/* Assume no Fabric and go on with discovery.
		 * Check for outstanding ELS FLOGI to abort.
		 */

		/* FLOGI failed, so just use loop map to make discovery list */
		lpfc_disc_list_loopmap(vport);

		/* Start discovery */
		lpfc_disc_start(vport);
		break;

	case LPFC_FABRIC_CFG_LINK:
	/* hba_state is identically LPFC_FABRIC_CFG_LINK while waiting for
	   NameServer login */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0223 Timeout while waiting for "
				 "NameServer login\n");
		/* Next look for NameServer ndlp */
		ndlp = lpfc_findnode_did(vport, NameServer_DID);
		if (ndlp && NLP_CHK_NODE_ACT(ndlp))
			lpfc_els_abort(phba, ndlp);

		/* ReStart discovery */
		goto restart_disc;

	case LPFC_NS_QRY:
	/* Check for wait for NameServer Rsp timeout */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0224 NameServer Query timeout "
				 "Data: x%x x%x\n",
				 vport->fc_ns_retry, LPFC_MAX_NS_RETRY);

		if (vport->fc_ns_retry < LPFC_MAX_NS_RETRY) {
			/* Try it one more time */
			vport->fc_ns_retry++;
			rc = lpfc_ns_cmd(vport, SLI_CTNS_GID_FT,
					 vport->fc_ns_retry, 0);
			if (rc == 0)
				break;
		}
		vport->fc_ns_retry = 0;

restart_disc:
		/*
		 * Discovery is over.
		 * set port_state to PORT_READY if SLI2.
		 * cmpl_reg_vpi will set port_state to READY for SLI3.
		 */
		if (phba->sli_rev < LPFC_SLI_REV4) {
			if (phba->sli3_options & LPFC_SLI3_NPIV_ENABLED)
				lpfc_issue_reg_vpi(phba, vport);
			else  {	/* NPIV Not enabled */
				lpfc_issue_clear_la(phba, vport);
				vport->port_state = LPFC_VPORT_READY;
			}
		}

		/* Setup and issue mailbox INITIALIZE LINK command */
		initlinkmbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
		if (!initlinkmbox) {
			lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
					 "0206 Device Discovery "
					 "completion error\n");
			phba->link_state = LPFC_HBA_ERROR;
			break;
		}

		lpfc_linkdown(phba);
		lpfc_init_link(phba, initlinkmbox, phba->cfg_topology,
			       phba->cfg_link_speed);
		initlinkmbox->u.mb.un.varInitLnk.lipsr_AL_PA = 0;
		initlinkmbox->vport = vport;
		initlinkmbox->mbox_cmpl = lpfc_sli_def_mbox_cmpl;
		rc = lpfc_sli_issue_mbox(phba, initlinkmbox, MBX_NOWAIT);
		lpfc_set_loopback_flag(phba);
		if (rc == MBX_NOT_FINISHED)
			mempool_free(initlinkmbox, phba->mbox_mem_pool);

		break;

	case LPFC_DISC_AUTH:
	/* Node Authentication timeout */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0227 Node Authentication timeout\n");
		lpfc_disc_flush_list(vport);

		/*
		 * set port_state to PORT_READY if SLI2.
		 * cmpl_reg_vpi will set port_state to READY for SLI3.
		 */
		if (phba->sli_rev < LPFC_SLI_REV4) {
			if (phba->sli3_options & LPFC_SLI3_NPIV_ENABLED)
				lpfc_issue_reg_vpi(phba, vport);
			else  {	/* NPIV Not enabled */
				lpfc_issue_clear_la(phba, vport);
				vport->port_state = LPFC_VPORT_READY;
			}
		}
		break;

	case LPFC_VPORT_READY:
		if (vport->fc_flag & FC_RSCN_MODE) {
			lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
					 "0231 RSCN timeout Data: x%x "
					 "x%x\n",
					 vport->fc_ns_retry, LPFC_MAX_NS_RETRY);

			/* Cleanup any outstanding ELS commands */
			lpfc_els_flush_cmd(vport);

			lpfc_els_flush_rscn(vport);
			lpfc_disc_flush_list(vport);
		}
		break;

	default:
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0273 Unexpected discovery timeout, "
				 "vport State x%x\n", vport->port_state);
		break;
	}

	switch (phba->link_state) {
	case LPFC_CLEAR_LA:
				/* CLEAR LA timeout */
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0228 CLEAR LA timeout\n");
		clrlaerr = 1;
		break;

	case LPFC_LINK_UP:
		lpfc_issue_clear_la(phba, vport);
		/* Drop thru */
	case LPFC_LINK_UNKNOWN:
	case LPFC_WARM_START:
	case LPFC_INIT_START:
	case LPFC_INIT_MBX_CMDS:
	case LPFC_LINK_DOWN:
	case LPFC_HBA_ERROR:
		lpfc_printf_vlog(vport, KERN_ERR, LOG_DISCOVERY,
				 "0230 Unexpected timeout, hba link "
				 "state x%x\n", phba->link_state);
		clrlaerr = 1;
		break;

	case LPFC_HBA_READY:
		break;
	}

	if (clrlaerr) {
		lpfc_disc_flush_list(vport);
		psli->ring[(psli->extra_ring)].flag &= ~LPFC_STOP_IOCB_EVENT;
		psli->ring[(psli->fcp_ring)].flag &= ~LPFC_STOP_IOCB_EVENT;
		psli->ring[(psli->next_ring)].flag &= ~LPFC_STOP_IOCB_EVENT;
		vport->port_state = LPFC_VPORT_READY;
	}

	return;
}

/*
 * This routine handles processing a NameServer REG_LOGIN mailbox
 * command upon completion. It is setup in the LPFC_MBOXQ
 * as the completion routine when the command is
 * handed off to the SLI layer.
 */
void
lpfc_mbx_cmpl_fdmi_reg_login(struct lpfc_hba *phba, LPFC_MBOXQ_t *pmb)
{
	MAILBOX_t *mb = &pmb->u.mb;
	struct lpfc_dmabuf   *mp = (struct lpfc_dmabuf *) (pmb->context1);
	struct lpfc_nodelist *ndlp = (struct lpfc_nodelist *) pmb->context2;
	struct lpfc_vport    *vport = pmb->vport;

	pmb->context1 = NULL;

	ndlp->nlp_rpi = mb->un.varWords[0];
	ndlp->nlp_flag |= NLP_RPI_VALID;
	ndlp->nlp_type |= NLP_FABRIC;
	lpfc_nlp_set_state(vport, ndlp, NLP_STE_UNMAPPED_NODE);

	/*
	 * Start issuing Fabric-Device Management Interface (FDMI) command to
	 * 0xfffffa (FDMI well known port) or Delay issuing FDMI command if
	 * fdmi-on=2 (supporting RPA/hostnmae)
	 */

	if (vport->cfg_fdmi_on == 1)
		lpfc_fdmi_cmd(vport, ndlp, SLI_MGMT_DHBA);
	else
		mod_timer(&vport->fc_fdmitmo, jiffies + HZ * 60);

	/* decrement the node reference count held for this callback
	 * function.
	 */
	lpfc_nlp_put(ndlp);
	lpfc_mbuf_free(phba, mp->virt, mp->phys);
	kfree(mp);
	mempool_free(pmb, phba->mbox_mem_pool);

	return;
}

static int
lpfc_filter_by_rpi(struct lpfc_nodelist *ndlp, void *param)
{
	uint16_t *rpi = param;

	return ndlp->nlp_rpi == *rpi;
}

static int
lpfc_filter_by_wwpn(struct lpfc_nodelist *ndlp, void *param)
{
	return memcmp(&ndlp->nlp_portname, param,
		      sizeof(ndlp->nlp_portname)) == 0;
}

static int
lpfc_filter_by_wwnn(struct lpfc_nodelist *ndlp, void *param)
{
	return memcmp(&ndlp->nlp_nodename, param,
		      sizeof(ndlp->nlp_nodename)) == 0;
}

struct lpfc_nodelist *
__lpfc_find_node(struct lpfc_vport *vport, node_filter filter, void *param)
{
	struct lpfc_nodelist *ndlp;

	list_for_each_entry(ndlp, &vport->fc_nodes, nlp_listp) {
		if (filter(ndlp, param))
			return ndlp;
	}
	return NULL;
}

/*
 * Search node lists for a remote port matching filter criteria
 * Caller needs to hold host_lock before calling this routine.
 */
struct lpfc_nodelist *
lpfc_find_node(struct lpfc_vport *vport, node_filter filter, void *param)
{
	struct Scsi_Host     *shost = lpfc_shost_from_vport(vport);
	struct lpfc_nodelist *ndlp;

	spin_lock_irq(shost->host_lock);
	ndlp = __lpfc_find_node(vport, filter, param);
	spin_unlock_irq(shost->host_lock);
	return ndlp;
}

/*
 * This routine looks up the ndlp lists for the given RPI. If rpi found it
 * returns the node list element pointer else return NULL.
 */
struct lpfc_nodelist *
__lpfc_findnode_rpi(struct lpfc_vport *vport, uint16_t rpi)
{
	return __lpfc_find_node(vport, lpfc_filter_by_rpi, &rpi);
}

struct lpfc_nodelist *
lpfc_findnode_rpi(struct lpfc_vport *vport, uint16_t rpi)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_nodelist *ndlp;

	spin_lock_irq(shost->host_lock);
	ndlp = __lpfc_findnode_rpi(vport, rpi);
	spin_unlock_irq(shost->host_lock);
	return ndlp;
}

/*
 * This routine looks up the ndlp lists for the given WWPN. If WWPN found it
 * returns the node element list pointer else return NULL.
 */
struct lpfc_nodelist *
lpfc_findnode_wwpn(struct lpfc_vport *vport, struct lpfc_name *wwpn)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_nodelist *ndlp;

	spin_lock_irq(shost->host_lock);
	ndlp = __lpfc_find_node(vport, lpfc_filter_by_wwpn, wwpn);
	spin_unlock_irq(shost->host_lock);
	return ndlp;
}

/*
 * This routine looks up the ndlp lists for the given WWNN. If WWNN found it
 * returns the node element list pointer else return NULL.
 */
struct lpfc_nodelist *
lpfc_findnode_wwnn(struct lpfc_vport *vport, struct lpfc_name *wwnn)
{
	struct Scsi_Host *shost = lpfc_shost_from_vport(vport);
	struct lpfc_nodelist *ndlp;

	spin_lock_irq(shost->host_lock);
	ndlp = __lpfc_find_node(vport, lpfc_filter_by_wwnn, wwnn);
	spin_unlock_irq(shost->host_lock);
	return ndlp;
}

void
lpfc_dev_loss_delay(unsigned long ptr)
{
	struct lpfc_nodelist *ndlp = (struct lpfc_nodelist *) ptr;
	struct lpfc_vport *vport = ndlp->vport;
	struct lpfc_hba   *phba = vport->phba;
	struct lpfc_work_evt  *evtp = &ndlp->dev_loss_evt;
	unsigned long flags;

	evtp = &ndlp->dev_loss_evt;

	spin_lock_irqsave(&phba->hbalock, flags);
	if (!list_empty(&evtp->evt_listp)) {
		spin_unlock_irqrestore(&phba->hbalock, flags);
		return;
	}

	/* We need to hold the node by incrementing the reference
	 * count until this queued work is done
	 */
	evtp->evt_arg1  = lpfc_nlp_get(ndlp);
	if (evtp->evt_arg1) {
		evtp->evt = LPFC_EVT_DEV_LOSS_DELAY;
		list_add_tail(&evtp->evt_listp, &phba->work_list);
		lpfc_worker_wake_up(phba);
	}
	spin_unlock_irqrestore(&phba->hbalock, flags);
	return;
}

void
lpfc_nlp_init(struct lpfc_vport *vport, struct lpfc_nodelist *ndlp,
	      uint32_t did)
{
	memset(ndlp, 0, sizeof (struct lpfc_nodelist));
	lpfc_initialize_node(vport, ndlp, did);
	INIT_LIST_HEAD(&ndlp->nlp_listp);

	lpfc_debugfs_disc_trc(vport, LPFC_DISC_TRC_NODE,
		"node init:       did:x%x",
		ndlp->nlp_DID, 0, 0);

	return;
}

/* This routine releases all resources associated with a specifc NPort's ndlp
 * and mempool_free's the nodelist.
 */
static void
lpfc_nlp_release(struct kref *kref)
{
	struct lpfc_hba *phba;
	unsigned long flags;
	struct lpfc_nodelist *ndlp = container_of(kref, struct lpfc_nodelist,
						  kref);

	lpfc_debugfs_disc_trc(ndlp->vport, LPFC_DISC_TRC_NODE,
		"node release:    did:x%x flg:x%x type:x%x",
		ndlp->nlp_DID, ndlp->nlp_flag, ndlp->nlp_type);

	lpfc_printf_vlog(ndlp->vport, KERN_INFO, LOG_NODE,
			"0279 lpfc_nlp_release: ndlp:x%p "
			"usgmap:x%x refcnt:%d\n",
			(void *)ndlp, ndlp->nlp_usg_map,
			atomic_read(&ndlp->kref.refcount));

	/* remove ndlp from action. */
	lpfc_nlp_remove(ndlp->vport, ndlp);

	/* clear the ndlp active flag for all release cases */
	phba = ndlp->phba;
	spin_lock_irqsave(&phba->ndlp_lock, flags);
	NLP_CLR_NODE_ACT(ndlp);
	spin_unlock_irqrestore(&phba->ndlp_lock, flags);

	/* free ndlp memory for final ndlp release */
	if (NLP_CHK_FREE_REQ(ndlp)) {
		kfree(ndlp->lat_data);
		mempool_free(ndlp, ndlp->phba->nlp_mem_pool);
	}
}

/* This routine bumps the reference count for a ndlp structure to ensure
 * that one discovery thread won't free a ndlp while another discovery thread
 * is using it.
 */
struct lpfc_nodelist *
lpfc_nlp_get(struct lpfc_nodelist *ndlp)
{
	struct lpfc_hba *phba;
	unsigned long flags;

	if (ndlp) {
		lpfc_debugfs_disc_trc(ndlp->vport, LPFC_DISC_TRC_NODE,
			"node get:        did:x%x flg:x%x refcnt:x%x",
			ndlp->nlp_DID, ndlp->nlp_flag,
			atomic_read(&ndlp->kref.refcount));
		/* The check of ndlp usage to prevent incrementing the
		 * ndlp reference count that is in the process of being
		 * released.
		 */
		phba = ndlp->phba;
		spin_lock_irqsave(&phba->ndlp_lock, flags);
		if (!NLP_CHK_NODE_ACT(ndlp) || NLP_CHK_FREE_ACK(ndlp)) {
			spin_unlock_irqrestore(&phba->ndlp_lock, flags);
			lpfc_printf_vlog(ndlp->vport, KERN_WARNING, LOG_NODE,
				"0276 lpfc_nlp_get: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
			return NULL;
		} else
			kref_get(&ndlp->kref);
		spin_unlock_irqrestore(&phba->ndlp_lock, flags);
	}
	return ndlp;
}

/* This routine decrements the reference count for a ndlp structure. If the
 * count goes to 0, this indicates the the associated nodelist should be
 * freed. Returning 1 indicates the ndlp resource has been released; on the
 * other hand, returning 0 indicates the ndlp resource has not been released
 * yet.
 */
int
lpfc_nlp_put(struct lpfc_nodelist *ndlp)
{
	struct lpfc_hba *phba;
	unsigned long flags;

	if (!ndlp)
		return 1;

	lpfc_debugfs_disc_trc(ndlp->vport, LPFC_DISC_TRC_NODE,
	"node put:        did:x%x flg:x%x refcnt:x%x",
		ndlp->nlp_DID, ndlp->nlp_flag,
		atomic_read(&ndlp->kref.refcount));
	phba = ndlp->phba;
	spin_lock_irqsave(&phba->ndlp_lock, flags);
	/* Check the ndlp memory free acknowledge flag to avoid the
	 * possible race condition that kref_put got invoked again
	 * after previous one has done ndlp memory free.
	 */
	if (NLP_CHK_FREE_ACK(ndlp)) {
		spin_unlock_irqrestore(&phba->ndlp_lock, flags);
		lpfc_printf_vlog(ndlp->vport, KERN_WARNING, LOG_NODE,
				"0274 lpfc_nlp_put: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
		return 1;
	}
	/* Check the ndlp inactivate log flag to avoid the possible
	 * race condition that kref_put got invoked again after ndlp
	 * is already in inactivating state.
	 */
	if (NLP_CHK_IACT_REQ(ndlp)) {
		spin_unlock_irqrestore(&phba->ndlp_lock, flags);
		lpfc_printf_vlog(ndlp->vport, KERN_WARNING, LOG_NODE,
				"0275 lpfc_nlp_put: ndlp:x%p "
				"usgmap:x%x refcnt:%d\n",
				(void *)ndlp, ndlp->nlp_usg_map,
				atomic_read(&ndlp->kref.refcount));
		return 1;
	}
	/* For last put, mark the ndlp usage flags to make sure no
	 * other kref_get and kref_put on the same ndlp shall get
	 * in between the process when the final kref_put has been
	 * invoked on this ndlp.
	 */
	if (atomic_read(&ndlp->kref.refcount) == 1) {
		/* Indicate ndlp is put to inactive state. */
		NLP_SET_IACT_REQ(ndlp);
		/* Acknowledge ndlp memory free has been seen. */
		if (NLP_CHK_FREE_REQ(ndlp))
			NLP_SET_FREE_ACK(ndlp);
	}
	spin_unlock_irqrestore(&phba->ndlp_lock, flags);
	/* Note, the kref_put returns 1 when decrementing a reference
	 * count that was 1, it invokes the release callback function,
	 * but it still left the reference count as 1 (not actually
	 * performs the last decrementation). Otherwise, it actually
	 * decrements the reference count and returns 0.
	 */
	return kref_put(&ndlp->kref, lpfc_nlp_release);
}

/* This routine free's the specified nodelist if it is not in use
 * by any other discovery thread. This routine returns 1 if the
 * ndlp has been freed. A return value of 0 indicates the ndlp is
 * not yet been released.
 */
int
lpfc_nlp_not_used(struct lpfc_nodelist *ndlp)
{
	lpfc_debugfs_disc_trc(ndlp->vport, LPFC_DISC_TRC_NODE,
		"node not used:   did:x%x flg:x%x refcnt:x%x",
		ndlp->nlp_DID, ndlp->nlp_flag,
		atomic_read(&ndlp->kref.refcount));
	if (atomic_read(&ndlp->kref.refcount) == 1)
		if (lpfc_nlp_put(ndlp))
			return 1;
	return 0;
}

/**
 * lpfc_fcf_inuse - Check if FCF can be unregistered.
 * @phba: Pointer to hba context object.
 *
 * This function iterate through all FC nodes associated
 * will all vports to check if there is any node with
 * fc_rports associated with it. If there is an fc_rport
 * associated with the node, then the node is either in
 * discovered state or its devloss_timer is pending.
 */
static int
lpfc_fcf_inuse(struct lpfc_hba *phba)
{
	struct lpfc_vport **vports;
	int i, ret = 0;
	struct lpfc_nodelist *ndlp;
	struct Scsi_Host  *shost;

	vports = lpfc_create_vport_work_array(phba);

	for (i = 0; i <= phba->max_vports && vports[i] != NULL; i++) {
		shost = lpfc_shost_from_vport(vports[i]);
		spin_lock_irq(shost->host_lock);
		list_for_each_entry(ndlp, &vports[i]->fc_nodes, nlp_listp) {
			if (NLP_CHK_NODE_ACT(ndlp) && ndlp->rport &&
			  (ndlp->rport->roles & FC_RPORT_ROLE_FCP_TARGET)) {
				ret = 1;
				spin_unlock_irq(shost->host_lock);
				goto out;
			} else {
				lpfc_printf_log(phba, KERN_INFO, LOG_ELS,
					"2624 RPI %x DID %x flg %x still "
					"logged in\n",
					ndlp->nlp_rpi, ndlp->nlp_DID,
					ndlp->nlp_flag);
				if (ndlp->nlp_flag & NLP_RPI_VALID)
					ret = 1;
			}
		}
		spin_unlock_irq(shost->host_lock);
	}
out:
	lpfc_destroy_vport_work_array(phba, vports);
	return ret;
}

/**
 * lpfc_unregister_vfi_cmpl - Completion handler for unreg vfi.
 * @phba: Pointer to hba context object.
 * @mboxq: Pointer to mailbox object.
 *
 * This function frees memory associated with the mailbox command.
 */
static void
lpfc_unregister_vfi_cmpl(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	struct lpfc_vport *vport = mboxq->vport;

	if (mboxq->u.mb.mbxStatus) {
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2555 UNREG_VFI mbxStatus error x%x "
			"HBA state x%x\n",
			mboxq->u.mb.mbxStatus, vport->port_state);
	}
	mempool_free(mboxq, phba->mbox_mem_pool);
	return;
}

/**
 * lpfc_unregister_fcfi_cmpl - Completion handler for unreg fcfi.
 * @phba: Pointer to hba context object.
 * @mboxq: Pointer to mailbox object.
 *
 * This function frees memory associated with the mailbox command.
 */
static void
lpfc_unregister_fcfi_cmpl(struct lpfc_hba *phba, LPFC_MBOXQ_t *mboxq)
{
	struct lpfc_vport *vport = mboxq->vport;

	if (mboxq->u.mb.mbxStatus) {
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2550 UNREG_FCFI mbxStatus error x%x "
			"HBA state x%x\n",
			mboxq->u.mb.mbxStatus, vport->port_state);
	}
	mempool_free(mboxq, phba->mbox_mem_pool);
	return;
}

/**
 * lpfc_unregister_unused_fcf - Unregister FCF if all devices are disconnected.
 * @phba: Pointer to hba context object.
 *
 * This function check if there are any connected remote port for the FCF and
 * if all the devices are disconnected, this function unregister FCFI.
 * This function also tries to use another FCF for discovery.
 */
void
lpfc_unregister_unused_fcf(struct lpfc_hba *phba)
{
	LPFC_MBOXQ_t *mbox;
	int rc;
	struct lpfc_vport **vports;
	int i;

	spin_lock_irq(&phba->hbalock);
	/*
	 * If HBA is not running in FIP mode or
	 * If HBA does not support FCoE or
	 * If FCF is not registered.
	 * do nothing.
	 */
	if (!(phba->hba_flag & HBA_FCOE_SUPPORT) ||
		!(phba->fcf.fcf_flag & FCF_REGISTERED) ||
		(!(phba->hba_flag & HBA_FIP_SUPPORT))) {
		spin_unlock_irq(&phba->hbalock);
		return;
	}
	spin_unlock_irq(&phba->hbalock);

	if (lpfc_fcf_inuse(phba))
		return;

	/* At this point, all discovery is aborted */
	phba->pport->port_state = LPFC_VPORT_UNKNOWN;

	/* Unregister VPIs */
	vports = lpfc_create_vport_work_array(phba);
	if (vports &&
		(phba->sli3_options & LPFC_SLI3_NPIV_ENABLED))
		for (i = 0; i <= phba->max_vports && vports[i] != NULL; i++) {
			lpfc_mbx_unreg_vpi(vports[i]);
			spin_lock_irq(&phba->hbalock);
			vports[i]->fc_flag |= FC_VPORT_NEEDS_INIT_VPI;
			vports[i]->vpi_state &= ~LPFC_VPI_REGISTERED;
			spin_unlock_irq(&phba->hbalock);
		}
	lpfc_destroy_vport_work_array(phba, vports);

	/* Cleanup any outstanding ELS commands */
	lpfc_els_flush_all_cmd(phba);

	/* Unregister VFI */
	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox) {
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2556 UNREG_VFI mbox allocation failed"
			"HBA state x%x\n",
			phba->pport->port_state);
		return;
	}

	lpfc_unreg_vfi(mbox, phba->pport);
	mbox->vport = phba->pport;
	mbox->mbox_cmpl = lpfc_unregister_vfi_cmpl;

	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);
	if (rc == MBX_NOT_FINISHED) {
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2557 UNREG_VFI issue mbox failed rc x%x "
			"HBA state x%x\n",
			rc, phba->pport->port_state);
		mempool_free(mbox, phba->mbox_mem_pool);
		return;
	}

	/* Unregister FCF */
	mbox = mempool_alloc(phba->mbox_mem_pool, GFP_KERNEL);
	if (!mbox) {
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2551 UNREG_FCFI mbox allocation failed"
			"HBA state x%x\n",
			phba->pport->port_state);
		return;
	}

	lpfc_unreg_fcfi(mbox, phba->fcf.fcfi);
	mbox->vport = phba->pport;
	mbox->mbox_cmpl = lpfc_unregister_fcfi_cmpl;
	rc = lpfc_sli_issue_mbox(phba, mbox, MBX_NOWAIT);

	if (rc == MBX_NOT_FINISHED) {
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2552 UNREG_FCFI issue mbox failed rc x%x "
			"HBA state x%x\n",
			rc, phba->pport->port_state);
		mempool_free(mbox, phba->mbox_mem_pool);
		return;
	}

	spin_lock_irq(&phba->hbalock);
	phba->fcf.fcf_flag &= ~(FCF_AVAILABLE | FCF_REGISTERED |
		FCF_DISCOVERED | FCF_BOOT_ENABLE | FCF_IN_USE |
		FCF_VALID_VLAN);
	spin_unlock_irq(&phba->hbalock);

	/*
	 * If driver is not unloading, check if there is any other
	 * FCF record that can be used for discovery.
	 */
	if ((phba->pport->load_flag & FC_UNLOADING) ||
		(phba->link_state < LPFC_LINK_UP))
		return;

	rc = lpfc_sli4_read_fcf_record(phba, LPFC_FCOE_FCF_GET_FIRST);

	if (rc)
		lpfc_printf_log(phba, KERN_ERR, LOG_DISCOVERY|LOG_MBOX,
			"2553 lpfc_unregister_unused_fcf failed to read FCF"
			" record HBA state x%x\n",
			phba->pport->port_state);
}

/**
 * lpfc_read_fcf_conn_tbl - Create driver FCF connection table.
 * @phba: Pointer to hba context object.
 * @buff: Buffer containing the FCF connection table as in the config
 *         region.
 * This function create driver data structure for the FCF connection
 * record table read from config region 23.
 */
static void
lpfc_read_fcf_conn_tbl(struct lpfc_hba *phba,
	uint8_t *buff)
{
	struct lpfc_fcf_conn_entry *conn_entry, *next_conn_entry;
	struct lpfc_fcf_conn_hdr *conn_hdr;
	struct lpfc_fcf_conn_rec *conn_rec;
	uint32_t record_count;
	int i;

	/* Free the current connect table */
	list_for_each_entry_safe(conn_entry, next_conn_entry,
		&phba->fcf_conn_rec_list, list) {
		list_del_init(&conn_entry->list);
		kfree(conn_entry);
	}

	conn_hdr = (struct lpfc_fcf_conn_hdr *) buff;
	record_count = conn_hdr->length * sizeof(uint32_t)/
		sizeof(struct lpfc_fcf_conn_rec);

	conn_rec = (struct lpfc_fcf_conn_rec *)
		(buff + sizeof(struct lpfc_fcf_conn_hdr));

	for (i = 0; i < record_count; i++) {
		if (!(conn_rec[i].flags & FCFCNCT_VALID))
			continue;
		conn_entry = kzalloc(sizeof(struct lpfc_fcf_conn_entry),
			GFP_KERNEL);
		if (!conn_entry) {
			lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
				"2566 Failed to allocate connection"
				" table entry\n");
			return;
		}

		memcpy(&conn_entry->conn_rec, &conn_rec[i],
			sizeof(struct lpfc_fcf_conn_rec));
		conn_entry->conn_rec.vlan_tag =
			le16_to_cpu(conn_entry->conn_rec.vlan_tag) & 0xFFF;
		conn_entry->conn_rec.flags =
			le16_to_cpu(conn_entry->conn_rec.flags);
		list_add_tail(&conn_entry->list,
			&phba->fcf_conn_rec_list);
	}
}

/**
 * lpfc_read_fcoe_param - Read FCoe parameters from conf region..
 * @phba: Pointer to hba context object.
 * @buff: Buffer containing the FCoE parameter data structure.
 *
 *  This function update driver data structure with config
 *  parameters read from config region 23.
 */
static void
lpfc_read_fcoe_param(struct lpfc_hba *phba,
			uint8_t *buff)
{
	struct lpfc_fip_param_hdr *fcoe_param_hdr;
	struct lpfc_fcoe_params *fcoe_param;

	fcoe_param_hdr = (struct lpfc_fip_param_hdr *)
		buff;
	fcoe_param = (struct lpfc_fcoe_params *)
		(buff + sizeof(struct lpfc_fip_param_hdr));

	if ((fcoe_param_hdr->parm_version != FIPP_VERSION) ||
		(fcoe_param_hdr->length != FCOE_PARAM_LENGTH))
		return;

	if (fcoe_param_hdr->parm_flags & FIPP_VLAN_VALID) {
		phba->valid_vlan = 1;
		phba->vlan_id = le16_to_cpu(fcoe_param->vlan_tag) &
			0xFFF;
	}

	phba->fc_map[0] = fcoe_param->fc_map[0];
	phba->fc_map[1] = fcoe_param->fc_map[1];
	phba->fc_map[2] = fcoe_param->fc_map[2];
	return;
}

/**
 * lpfc_get_rec_conf23 - Get a record type in config region data.
 * @buff: Buffer containing config region 23 data.
 * @size: Size of the data buffer.
 * @rec_type: Record type to be searched.
 *
 * This function searches config region data to find the begining
 * of the record specified by record_type. If record found, this
 * function return pointer to the record else return NULL.
 */
static uint8_t *
lpfc_get_rec_conf23(uint8_t *buff, uint32_t size, uint8_t rec_type)
{
	uint32_t offset = 0, rec_length;

	if ((buff[0] == LPFC_REGION23_LAST_REC) ||
		(size < sizeof(uint32_t)))
		return NULL;

	rec_length = buff[offset + 1];

	/*
	 * One TLV record has one word header and number of data words
	 * specified in the rec_length field of the record header.
	 */
	while ((offset + rec_length * sizeof(uint32_t) + sizeof(uint32_t))
		<= size) {
		if (buff[offset] == rec_type)
			return &buff[offset];

		if (buff[offset] == LPFC_REGION23_LAST_REC)
			return NULL;

		offset += rec_length * sizeof(uint32_t) + sizeof(uint32_t);
		rec_length = buff[offset + 1];
	}
	return NULL;
}

/**
 * lpfc_parse_fcoe_conf - Parse FCoE config data read from config region 23.
 * @phba: Pointer to lpfc_hba data structure.
 * @buff: Buffer containing config region 23 data.
 * @size: Size of the data buffer.
 *
 * This fuction parse the FCoE config parameters in config region 23 and
 * populate driver data structure with the parameters.
 */
void
lpfc_parse_fcoe_conf(struct lpfc_hba *phba,
		uint8_t *buff,
		uint32_t size)
{
	uint32_t offset = 0, rec_length;
	uint8_t *rec_ptr;

	/*
	 * If data size is less than 2 words signature and version cannot be
	 * verified.
	 */
	if (size < 2*sizeof(uint32_t))
		return;

	/* Check the region signature first */
	if (memcmp(buff, LPFC_REGION23_SIGNATURE, 4)) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2567 Config region 23 has bad signature\n");
		return;
	}

	offset += 4;

	/* Check the data structure version */
	if (buff[offset] != LPFC_REGION23_VERSION) {
		lpfc_printf_log(phba, KERN_ERR, LOG_INIT,
			"2568 Config region 23 has bad version\n");
		return;
	}
	offset += 4;

	rec_length = buff[offset + 1];

	/* Read FCoE param record */
	rec_ptr = lpfc_get_rec_conf23(&buff[offset],
			size - offset, FCOE_PARAM_TYPE);
	if (rec_ptr)
		lpfc_read_fcoe_param(phba, rec_ptr);

	/* Read FCF connection table */
	rec_ptr = lpfc_get_rec_conf23(&buff[offset],
		size - offset, FCOE_CONN_TBL_TYPE);
	if (rec_ptr)
		lpfc_read_fcf_conn_tbl(phba, rec_ptr);

}
