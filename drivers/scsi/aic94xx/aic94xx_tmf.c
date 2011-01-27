/*
 * Aic94xx Task Management Functions
 *
 * Copyright (C) 2005 Adaptec, Inc.  All rights reserved.
 * Copyright (C) 2005 Luben Tuikov <luben_tuikov@adaptec.com>
 *
 * This file is licensed under GPLv2.
 *
 * This file is part of the aic94xx driver.
 *
 * The aic94xx driver is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * The aic94xx driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the aic94xx driver; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/spinlock.h>
#include "aic94xx.h"
#include "aic94xx_sas.h"
#include "aic94xx_hwi.h"

/* ---------- Internal enqueue ---------- */

static int asd_enqueue_internal(struct asd_ascb *ascb,
		void (*tasklet_complete)(struct asd_ascb *,
					 struct done_list_struct *),
				void (*timed_out)(unsigned long))
{
	int res;

	ascb->tasklet_complete = tasklet_complete;
	ascb->uldd_timer = 1;

	ascb->timer.data = (unsigned long) ascb;
	ascb->timer.function = timed_out;
	ascb->timer.expires = jiffies + AIC94XX_SCB_TIMEOUT;

	add_timer(&ascb->timer);

	res = asd_post_ascb_list(ascb->ha, ascb, 1);
	if (unlikely(res))
		del_timer(&ascb->timer);
	return res;
}

static inline void asd_timedout_common(unsigned long data)
{
	struct asd_ascb *ascb = (void *) data;
	struct asd_seq_data *seq = &ascb->ha->seq;
        unsigned long flags;

	spin_lock_irqsave(&seq->pend_q_lock, flags);
        seq->pending--;
        list_del_init(&ascb->list);
        spin_unlock_irqrestore(&seq->pend_q_lock, flags);
}

/* ---------- CLEAR NEXUS ---------- */

static void asd_clear_nexus_tasklet_complete(struct asd_ascb *ascb,
					     struct done_list_struct *dl)
{
	ASD_DPRINTK("%s: here\n", __FUNCTION__);
	if (!del_timer(&ascb->timer)) {
		ASD_DPRINTK("%s: couldn't delete timer\n", __FUNCTION__);
		return;
	}
	ASD_DPRINTK("%s: opcode: 0x%x\n", __FUNCTION__, dl->opcode);
	ascb->uldd_task = (void *) (unsigned long) dl->opcode;
	complete(&ascb->completion);
}

static void asd_clear_nexus_timedout(unsigned long data)
{
	struct asd_ascb *ascb = (void *) data;

	ASD_DPRINTK("%s: here\n", __FUNCTION__);
	asd_timedout_common(data);
	ascb->uldd_task = (void *) TMF_RESP_FUNC_FAILED;
	complete(&ascb->completion);
}

#define CLEAR_NEXUS_PRE         \
	ASD_DPRINTK("%s: PRE\n", __FUNCTION__); \
        res = 1;                \
	ascb = asd_ascb_alloc_list(asd_ha, &res, GFP_KERNEL); \
	if (!ascb)              \
		return -ENOMEM; \
                                \
	scb = ascb->scb;        \
	scb->header.opcode = CLEAR_NEXUS

#define CLEAR_NEXUS_POST        \
	ASD_DPRINTK("%s: POST\n", __FUNCTION__); \
	res = asd_enqueue_internal(ascb, asd_clear_nexus_tasklet_complete, \
				   asd_clear_nexus_timedout);              \
	if (res)                \
		goto out_err;   \
	ASD_DPRINTK("%s: clear nexus posted, waiting...\n", __FUNCTION__); \
	wait_for_completion(&ascb->completion); \
	res = (int) (unsigned long) ascb->uldd_task; \
	if (res == TC_NO_ERROR) \
		res = TMF_RESP_FUNC_COMPLETE;   \
out_err:                        \
	asd_ascb_free(ascb);    \
	return res

int asd_clear_nexus_ha(struct sas_ha_struct *sas_ha)
{
	struct asd_ha_struct *asd_ha = sas_ha->lldd_ha;
	struct asd_ascb *ascb;
	struct scb *scb;
	int res;

	CLEAR_NEXUS_PRE;
	scb->clear_nexus.nexus = NEXUS_ADAPTER;
	CLEAR_NEXUS_POST;
}

int asd_clear_nexus_port(struct asd_sas_port *port)
{
	struct asd_ha_struct *asd_ha = port->ha->lldd_ha;
	struct asd_ascb *ascb;
	struct scb *scb;
	int res;

	CLEAR_NEXUS_PRE;
	scb->clear_nexus.nexus = NEXUS_PORT;
	scb->clear_nexus.conn_mask = port->phy_mask;
	CLEAR_NEXUS_POST;
}

#if 0
static int asd_clear_nexus_I_T(struct domain_device *dev)
{
	struct asd_ha_struct *asd_ha = dev->port->ha->lldd_ha;
	struct asd_ascb *ascb;
	struct scb *scb;
	int res;

	CLEAR_NEXUS_PRE;
	scb->clear_nexus.nexus = NEXUS_I_T;
	scb->clear_nexus.flags = SEND_Q | EXEC_Q | NOTINQ;
	if (dev->tproto)
		scb->clear_nexus.flags |= SUSPEND_TX;
	scb->clear_nexus.conn_handle = cpu_to_le16((u16)(unsigned long)
						   dev->lldd_dev);
	CLEAR_NEXUS_POST;
}
#endif

static int asd_clear_nexus_I_T_L(struct domain_device *dev, u8 *lun)
{
	struct asd_ha_struct *asd_ha = dev->port->ha->lldd_ha;
	struct asd_ascb *ascb;
	struct scb *scb;
	int res;

	CLEAR_NEXUS_PRE;
	scb->clear_nexus.nexus = NEXUS_I_T_L;
	scb->clear_nexus.flags = SEND_Q | EXEC_Q | NOTINQ;
	if (dev->tproto)
		scb->clear_nexus.flags |= SUSPEND_TX;
	memcpy(scb->clear_nexus.ssp_task.lun, lun, 8);
	scb->clear_nexus.conn_handle = cpu_to_le16((u16)(unsigned long)
						   dev->lldd_dev);
	CLEAR_NEXUS_POST;
}

static int asd_clear_nexus_tag(struct sas_task *task)
{
	struct asd_ha_struct *asd_ha = task->dev->port->ha->lldd_ha;
	struct asd_ascb *tascb = task->lldd_task;
	struct asd_ascb *ascb;
	struct scb *scb;
	int res;

	CLEAR_NEXUS_PRE;
	scb->clear_nexus.nexus = NEXUS_TAG;
	memcpy(scb->clear_nexus.ssp_task.lun, task->ssp_task.LUN, 8);
	scb->clear_nexus.ssp_task.tag = tascb->tag;
	if (task->dev->tproto)
		scb->clear_nexus.conn_handle = cpu_to_le16((u16)(unsigned long)
							  task->dev->lldd_dev);
	CLEAR_NEXUS_POST;
}

static int asd_clear_nexus_index(struct sas_task *task)
{
	struct asd_ha_struct *asd_ha = task->dev->port->ha->lldd_ha;
	struct asd_ascb *tascb = task->lldd_task;
	struct asd_ascb *ascb;
	struct scb *scb;
	int res;

	CLEAR_NEXUS_PRE;
	scb->clear_nexus.nexus = NEXUS_TRANS_CX;
	if (task->dev->tproto)
		scb->clear_nexus.conn_handle = cpu_to_le16((u16)(unsigned long)
							  task->dev->lldd_dev);
	scb->clear_nexus.index = cpu_to_le16(tascb->tc_index);
	CLEAR_NEXUS_POST;
}

/* ---------- TMFs ---------- */

static void asd_tmf_timedout(unsigned long data)
{
	struct asd_ascb *ascb = (void *) data;

	ASD_DPRINTK("tmf timed out\n");
	asd_timedout_common(data);
	ascb->uldd_task = (void *) TMF_RESP_FUNC_FAILED;
	complete(&ascb->completion);
}

static int asd_get_tmf_resp_tasklet(struct asd_ascb *ascb,
				    struct done_list_struct *dl)
{
	struct asd_ha_struct *asd_ha = ascb->ha;
	unsigned long flags;
	struct tc_resp_sb_struct {
		__le16 index_escb;
		u8     len_lsb;
		u8     flags;
	} __attribute__ ((packed)) *resp_sb = (void *) dl->status_block;

	int  edb_id = ((resp_sb->flags & 0x70) >> 4)-1;
	struct asd_ascb *escb;
	struct asd_dma_tok *edb;
	struct ssp_frame_hdr *fh;
	struct ssp_response_iu   *ru;
	int res = TMF_RESP_FUNC_FAILED;

	ASD_DPRINTK("tmf resp tasklet\n");

	spin_lock_irqsave(&asd_ha->seq.tc_index_lock, flags);
	escb = asd_tc_index_find(&asd_ha->seq,
				 (int)le16_to_cpu(resp_sb->index_escb));
	spin_unlock_irqrestore(&asd_ha->seq.tc_index_lock, flags);

	if (!escb) {
		ASD_DPRINTK("Uh-oh! No escb for this dl?!\n");
		return res;
	}

	edb = asd_ha->seq.edb_arr[edb_id + escb->edb_index];
	ascb->tag = *(__be16 *)(edb->vaddr+4);
	fh = edb->vaddr + 16;
	ru = edb->vaddr + 16 + sizeof(*fh);
	res = ru->status;
	if (ru->datapres == 1)	  /* Response data present */
		res = ru->resp_data[3];
#if 0
	ascb->tag = fh->tag;
#endif
	ascb->tag_valid = 1;

	asd_invalidate_edb(escb, edb_id);
	return res;
}

static void asd_tmf_tasklet_complete(struct asd_ascb *ascb,
				     struct done_list_struct *dl)
{
	if (!del_timer(&ascb->timer))
		return;

	ASD_DPRINTK("tmf tasklet complete\n");

	if (dl->opcode == TC_SSP_RESP)
		ascb->uldd_task = (void *) (unsigned long)
			asd_get_tmf_resp_tasklet(ascb, dl);
	else
		ascb->uldd_task = (void *) 0xFF00 + (unsigned long) dl->opcode;

	complete(&ascb->completion);
}

static inline int asd_clear_nexus(struct sas_task *task)
{
	int res = TMF_RESP_FUNC_FAILED;
	struct asd_ascb *tascb = task->lldd_task;
	unsigned long flags;

	ASD_DPRINTK("task not done, clearing nexus\n");
	if (tascb->tag_valid)
		res = asd_clear_nexus_tag(task);
	else
		res = asd_clear_nexus_index(task);
	wait_for_completion_timeout(&tascb->completion,
				    AIC94XX_SCB_TIMEOUT);
	ASD_DPRINTK("came back from clear nexus\n");
	spin_lock_irqsave(&task->task_state_lock, flags);
	if (task->task_state_flags & SAS_TASK_STATE_DONE)
		res = TMF_RESP_FUNC_COMPLETE;
	spin_unlock_irqrestore(&task->task_state_lock, flags);

	return res;
}

/**
 * asd_abort_task -- ABORT TASK TMF
 * @task: the task to be aborted
 *
 * Before calling ABORT TASK the task state flags should be ORed with
 * SAS_TASK_STATE_ABORTED (unless SAS_TASK_STATE_DONE is set) under
 * the task_state_lock IRQ spinlock, then ABORT TASK *must* be called.
 *
 * Implements the ABORT TASK TMF, I_T_L_Q nexus.
 * Returns: SAS TMF responses (see sas_task.h),
 *          -ENOMEM,
 *          -SAS_QUEUE_FULL.
 *
 * When ABORT TASK returns, the caller of ABORT TASK checks first the
 * task->task_state_flags, and then the return value of ABORT TASK.
 *
 * If the task has task state bit SAS_TASK_STATE_DONE set, then the
 * task was completed successfully prior to it being aborted.  The
 * caller of ABORT TASK has responsibility to call task->task_done()
 * xor free the task, depending on their framework.  The return code
 * is TMF_RESP_FUNC_FAILED in this case.
 *
 * Else the SAS_TASK_STATE_DONE bit is not set,
 * 	If the return code is TMF_RESP_FUNC_COMPLETE, then
 * 		the task was aborted successfully.  The caller of
 * 		ABORT TASK has responsibility to call task->task_done()
 *              to finish the task, xor free the task depending on their
 *		framework.
 *	else
 * 		the ABORT TASK returned some kind of error. The task
 *              was _not_ cancelled.  Nothing can be assumed.
 *		The caller of ABORT TASK may wish to retry.
 */
int asd_abort_task(struct sas_task *task)
{
	struct asd_ascb *tascb = task->lldd_task;
	struct asd_ha_struct *asd_ha = tascb->ha;
	int res = 1;
	unsigned long flags;
	struct asd_ascb *ascb = NULL;
	struct scb *scb;

	spin_lock_irqsave(&task->task_state_lock, flags);
	if (task->task_state_flags & SAS_TASK_STATE_DONE) {
		spin_unlock_irqrestore(&task->task_state_lock, flags);
		res = TMF_RESP_FUNC_COMPLETE;
		ASD_DPRINTK("%s: task 0x%p done\n", __FUNCTION__, task);
		goto out_done;
	}
	spin_unlock_irqrestore(&task->task_state_lock, flags);

	ascb = asd_ascb_alloc_list(asd_ha, &res, GFP_KERNEL);
	if (!ascb)
		return -ENOMEM;
	scb = ascb->scb;

	scb->header.opcode = ABORT_TASK;

	switch (task->task_proto) {
	case SATA_PROTO:
	case SAS_PROTO_STP:
		scb->abort_task.proto_conn_rate = (1 << 5); /* STP */
		break;
	case SAS_PROTO_SSP:
		scb->abort_task.proto_conn_rate  = (1 << 4); /* SSP */
		scb->abort_task.proto_conn_rate |= task->dev->linkrate;
		break;
	case SAS_PROTO_SMP:
		break;
	default:
		break;
	}

	if (task->task_proto == SAS_PROTO_SSP) {
		scb->abort_task.ssp_frame.frame_type = SSP_TASK;
		memcpy(scb->abort_task.ssp_frame.hashed_dest_addr,
		       task->dev->hashed_sas_addr, HASHED_SAS_ADDR_SIZE);
		memcpy(scb->abort_task.ssp_frame.hashed_src_addr,
		       task->dev->port->ha->hashed_sas_addr,
		       HASHED_SAS_ADDR_SIZE);
		scb->abort_task.ssp_frame.tptt = cpu_to_be16(0xFFFF);

		memcpy(scb->abort_task.ssp_task.lun, task->ssp_task.LUN, 8);
		scb->abort_task.ssp_task.tmf = TMF_ABORT_TASK;
		scb->abort_task.ssp_task.tag = cpu_to_be16(0xFFFF);
	}

	scb->abort_task.sister_scb = cpu_to_le16(0xFFFF);
	scb->abort_task.conn_handle = cpu_to_le16(
		(u16)(unsigned long)task->dev->lldd_dev);
	scb->abort_task.retry_count = 1;
	scb->abort_task.index = cpu_to_le16((u16)tascb->tc_index);
	scb->abort_task.itnl_to = cpu_to_le16(ITNL_TIMEOUT_CONST);

	res = asd_enqueue_internal(ascb, asd_tmf_tasklet_complete,
				   asd_tmf_timedout);
	if (res)
		goto out;
	wait_for_completion(&ascb->completion);
	ASD_DPRINTK("tmf came back\n");

	res = (int) (unsigned long) ascb->uldd_task;
	tascb->tag = ascb->tag;
	tascb->tag_valid = ascb->tag_valid;

	spin_lock_irqsave(&task->task_state_lock, flags);
	if (task->task_state_flags & SAS_TASK_STATE_DONE) {
		spin_unlock_irqrestore(&task->task_state_lock, flags);
		res = TMF_RESP_FUNC_COMPLETE;
		ASD_DPRINTK("%s: task 0x%p done\n", __FUNCTION__, task);
		goto out_done;
	}
	spin_unlock_irqrestore(&task->task_state_lock, flags);

	switch (res) {
	/* The task to be aborted has been sent to the device.
	 * We got a Response IU for the ABORT TASK TMF. */
	case TC_NO_ERROR + 0xFF00:
	case TMF_RESP_FUNC_COMPLETE:
	case TMF_RESP_FUNC_FAILED:
		res = asd_clear_nexus(task);
		break;
	case TMF_RESP_INVALID_FRAME:
	case TMF_RESP_OVERLAPPED_TAG:
	case TMF_RESP_FUNC_ESUPP:
	case TMF_RESP_NO_LUN:
		goto out_done; break;
	}
	/* In the following we assume that the managing layer
	 * will _never_ make a mistake, when issuing ABORT TASK.
	 */
	switch (res) {
	default:
		res = asd_clear_nexus(task);
		/* fallthrough */
	case TC_NO_ERROR + 0xFF00:
	case TMF_RESP_FUNC_COMPLETE:
		break;
	/* The task hasn't been sent to the device xor we never got
	 * a (sane) Response IU for the ABORT TASK TMF.
	 */
	case TF_NAK_RECV + 0xFF00:
		res = TMF_RESP_INVALID_FRAME;
		break;
	case TF_TMF_TASK_DONE + 0xFF00:	/* done but not reported yet */
		res = TMF_RESP_FUNC_FAILED;
		wait_for_completion_timeout(&tascb->completion,
					    AIC94XX_SCB_TIMEOUT);
		spin_lock_irqsave(&task->task_state_lock, flags);
		if (task->task_state_flags & SAS_TASK_STATE_DONE)
			res = TMF_RESP_FUNC_COMPLETE;
		spin_unlock_irqrestore(&task->task_state_lock, flags);
		goto out_done;
	case TF_TMF_NO_TAG + 0xFF00:
	case TF_TMF_TAG_FREE + 0xFF00: /* the tag is in the free list */
	case TF_TMF_NO_CONN_HANDLE + 0xFF00: /* no such device */
		res = TMF_RESP_FUNC_COMPLETE;
		goto out_done;
	case TF_TMF_NO_CTX + 0xFF00: /* not in seq, or proto != SSP */
		res = TMF_RESP_FUNC_ESUPP;
		goto out;
	}
out_done:
	if (res == TMF_RESP_FUNC_COMPLETE) {
		task->lldd_task = NULL;
		mb();
		asd_ascb_free(tascb);
	}
out:
	asd_ascb_free(ascb);
	ASD_DPRINTK("task 0x%p aborted, res: 0x%x\n", task, res);
	return res;
}

/**
 * asd_initiate_ssp_tmf -- send a TMF to an I_T_L or I_T_L_Q nexus
 * @dev: pointer to struct domain_device of interest
 * @lun: pointer to u8[8] which is the LUN
 * @tmf: the TMF to be performed (see sas_task.h or the SAS spec)
 * @index: the transaction context of the task to be queried if QT TMF
 *
 * This function is used to send ABORT TASK SET, CLEAR ACA,
 * CLEAR TASK SET, LU RESET and QUERY TASK TMFs.
 *
 * No SCBs should be queued to the I_T_L nexus when this SCB is
 * pending.
 *
 * Returns: TMF response code (see sas_task.h or the SAS spec)
 */
static int asd_initiate_ssp_tmf(struct domain_device *dev, u8 *lun,
				int tmf, int index)
{
	struct asd_ha_struct *asd_ha = dev->port->ha->lldd_ha;
	struct asd_ascb *ascb;
	int res = 1;
	struct scb *scb;

	if (!(dev->tproto & SAS_PROTO_SSP))
		return TMF_RESP_FUNC_ESUPP;

	ascb = asd_ascb_alloc_list(asd_ha, &res, GFP_KERNEL);
	if (!ascb)
		return -ENOMEM;
	scb = ascb->scb;

	if (tmf == TMF_QUERY_TASK)
		scb->header.opcode = QUERY_SSP_TASK;
	else
		scb->header.opcode = INITIATE_SSP_TMF;

	scb->ssp_tmf.proto_conn_rate  = (1 << 4); /* SSP */
	scb->ssp_tmf.proto_conn_rate |= dev->linkrate;
	/* SSP frame header */
	scb->ssp_tmf.ssp_frame.frame_type = SSP_TASK;
	memcpy(scb->ssp_tmf.ssp_frame.hashed_dest_addr,
	       dev->hashed_sas_addr, HASHED_SAS_ADDR_SIZE);
	memcpy(scb->ssp_tmf.ssp_frame.hashed_src_addr,
	       dev->port->ha->hashed_sas_addr, HASHED_SAS_ADDR_SIZE);
	scb->ssp_tmf.ssp_frame.tptt = cpu_to_be16(0xFFFF);
	/* SSP Task IU */
	memcpy(scb->ssp_tmf.ssp_task.lun, lun, 8);
	scb->ssp_tmf.ssp_task.tmf = tmf;

	scb->ssp_tmf.sister_scb = cpu_to_le16(0xFFFF);
	scb->ssp_tmf.conn_handle= cpu_to_le16((u16)(unsigned long)
					      dev->lldd_dev);
	scb->ssp_tmf.retry_count = 1;
	scb->ssp_tmf.itnl_to = cpu_to_le16(ITNL_TIMEOUT_CONST);
	if (tmf == TMF_QUERY_TASK)
		scb->ssp_tmf.index = cpu_to_le16(index);

	res = asd_enqueue_internal(ascb, asd_tmf_tasklet_complete,
				   asd_tmf_timedout);
	if (res)
		goto out_err;
	wait_for_completion(&ascb->completion);
	res = (int) (unsigned long) ascb->uldd_task;

	switch (res) {
	case TC_NO_ERROR + 0xFF00:
		res = TMF_RESP_FUNC_COMPLETE;
		break;
	case TF_NAK_RECV + 0xFF00:
		res = TMF_RESP_INVALID_FRAME;
		break;
	case TF_TMF_TASK_DONE + 0xFF00:
		res = TMF_RESP_FUNC_FAILED;
		break;
	case TF_TMF_NO_TAG + 0xFF00:
	case TF_TMF_TAG_FREE + 0xFF00: /* the tag is in the free list */
	case TF_TMF_NO_CONN_HANDLE + 0xFF00: /* no such device */
		res = TMF_RESP_FUNC_COMPLETE;
		break;
	case TF_TMF_NO_CTX + 0xFF00: /* not in seq, or proto != SSP */
		res = TMF_RESP_FUNC_ESUPP;
		break;
	default:
		ASD_DPRINTK("%s: converting result 0x%x to TMF_RESP_FUNC_FAILED\n",
			    __FUNCTION__, res);
		res = TMF_RESP_FUNC_FAILED;
		break;
	}
out_err:
	asd_ascb_free(ascb);
	return res;
}

int asd_abort_task_set(struct domain_device *dev, u8 *lun)
{
	int res = asd_initiate_ssp_tmf(dev, lun, TMF_ABORT_TASK_SET, 0);

	if (res == TMF_RESP_FUNC_COMPLETE)
		asd_clear_nexus_I_T_L(dev, lun);
	return res;
}

int asd_clear_aca(struct domain_device *dev, u8 *lun)
{
	int res = asd_initiate_ssp_tmf(dev, lun, TMF_CLEAR_ACA, 0);

	if (res == TMF_RESP_FUNC_COMPLETE)
		asd_clear_nexus_I_T_L(dev, lun);
	return res;
}

int asd_clear_task_set(struct domain_device *dev, u8 *lun)
{
	int res = asd_initiate_ssp_tmf(dev, lun, TMF_CLEAR_TASK_SET, 0);

	if (res == TMF_RESP_FUNC_COMPLETE)
		asd_clear_nexus_I_T_L(dev, lun);
	return res;
}

int asd_lu_reset(struct domain_device *dev, u8 *lun)
{
	int res = asd_initiate_ssp_tmf(dev, lun, TMF_LU_RESET, 0);

	if (res == TMF_RESP_FUNC_COMPLETE)
		asd_clear_nexus_I_T_L(dev, lun);
	return res;
}

/**
 * asd_query_task -- send a QUERY TASK TMF to an I_T_L_Q nexus
 * task: pointer to sas_task struct of interest
 *
 * Returns: TMF_RESP_FUNC_COMPLETE if the task is not in the task set,
 * or TMF_RESP_FUNC_SUCC if the task is in the task set.
 *
 * Normally the management layer sets the task to aborted state,
 * and then calls query task and then abort task.
 */
int asd_query_task(struct sas_task *task)
{
	struct asd_ascb *ascb = task->lldd_task;
	int index;

	if (ascb) {
		index = ascb->tc_index;
		return asd_initiate_ssp_tmf(task->dev, task->ssp_task.LUN,
					    TMF_QUERY_TASK, index);
	}
	return TMF_RESP_FUNC_COMPLETE;
}
