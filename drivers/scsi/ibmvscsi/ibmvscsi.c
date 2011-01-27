/* ------------------------------------------------------------
 * ibmvscsi.c
 * (C) Copyright IBM Corporation 1994, 2004
 * Authors: Colin DeVilbiss (devilbis@us.ibm.com)
 *          Santiago Leon (santil@us.ibm.com)
 *          Dave Boutcher (sleddog@us.ibm.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 *
 * ------------------------------------------------------------
 * Emulation of a SCSI host adapter for Virtual I/O devices
 *
 * This driver supports the SCSI adapter implemented by the IBM
 * Power5 firmware.  That SCSI adapter is not a physical adapter,
 * but allows Linux SCSI peripheral drivers to directly
 * access devices in another logical partition on the physical system.
 *
 * The virtual adapter(s) are present in the open firmware device
 * tree just like real adapters.
 *
 * One of the capabilities provided on these systems is the ability
 * to DMA between partitions.  The architecture states that for VSCSI,
 * the server side is allowed to DMA to and from the client.  The client
 * is never trusted to DMA to or from the server directly.
 *
 * Messages are sent between partitions on a "Command/Response Queue" 
 * (CRQ), which is just a buffer of 16 byte entries in the receiver's 
 * Senders cannot access the buffer directly, but send messages by
 * making a hypervisor call and passing in the 16 bytes.  The hypervisor
 * puts the message in the next 16 byte space in round-robbin fashion,
 * turns on the high order bit of the message (the valid bit), and 
 * generates an interrupt to the receiver (if interrupts are turned on.) 
 * The receiver just turns off the valid bit when they have copied out
 * the message.
 *
 * The VSCSI client builds a SCSI Remote Protocol (SRP) Information Unit
 * (IU) (as defined in the T10 standard available at www.t10.org), gets 
 * a DMA address for the message, and sends it to the server as the
 * payload of a CRQ message.  The server DMAs the SRP IU and processes it,
 * including doing any additional data transfers.  When it is done, it
 * DMAs the SRP response back to the same address as the request came from,
 * and sends a CRQ message back to inform the client that the request has
 * completed.
 *
 * Note that some of the underlying infrastructure is different between
 * machines conforming to the "RS/6000 Platform Architecture" (RPA) and
 * the older iSeries hypervisor models.  To support both, some low level
 * routines have been broken out into rpa_vscsi.c and iseries_vscsi.c.
 * The Makefile should pick one, not two, not zero, of these.
 *
 * TODO: This is currently pretty tied to the IBM i/pSeries hypervisor
 * interfaces.  It would be really nice to abstract this above an RDMA
 * layer.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <asm/vio.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include "ibmvscsi.h"

/* The values below are somewhat arbitrary default values, but 
 * OS/400 will use 3 busses (disks, CDs, tapes, I think.)
 * Note that there are 3 bits of channel value, 6 bits of id, and
 * 5 bits of LUN.
 */
static int max_id = 64;
static int max_channel = 3;
static int init_timeout = 5;
static int max_requests = 50;
/* host data buffer size */
#define buff_size	4096

#define IBMVSCSI_VERSION "1.5.9"

MODULE_DESCRIPTION("IBM Virtual SCSI");
MODULE_AUTHOR("Dave Boutcher");
MODULE_LICENSE("GPL");
MODULE_VERSION(IBMVSCSI_VERSION);

module_param_named(max_id, max_id, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(max_id, "Largest ID value for each channel");
module_param_named(max_channel, max_channel, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(max_channel, "Largest channel value");
module_param_named(init_timeout, init_timeout, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(init_timeout, "Initialization timeout in seconds");
module_param_named(max_requests, max_requests, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(max_requests, "Maximum requests for this adapter");

/* ------------------------------------------------------------
 * Routines for the event pool and event structs
 */
/**
 * initialize_event_pool: - Allocates and initializes the event pool for a host
 * @pool:	event_pool to be initialized
 * @size:	Number of events in pool
 * @hostdata:	ibmvscsi_host_data who owns the event pool
 *
 * Returns zero on success.
*/
static int initialize_event_pool(struct event_pool *pool,
				 int size, struct ibmvscsi_host_data *hostdata)
{
	int i;

	pool->size = size;
	pool->next = 0;
	pool->events = kcalloc(pool->size, sizeof(*pool->events), GFP_KERNEL);
	if (!pool->events)
		return -ENOMEM;

	pool->iu_storage =
	    dma_alloc_coherent(hostdata->dev,
			       pool->size * sizeof(*pool->iu_storage),
			       &pool->iu_token, 0);
	if (!pool->iu_storage) {
		kfree(pool->events);
		return -ENOMEM;
	}

	for (i = 0; i < pool->size; ++i) {
		struct srp_event_struct *evt = &pool->events[i];
		memset(&evt->crq, 0x00, sizeof(evt->crq));
		atomic_set(&evt->free, 1);
		evt->crq.valid = 0x80;
		evt->crq.IU_length = sizeof(*evt->xfer_iu);
		evt->crq.IU_data_ptr = pool->iu_token + 
			sizeof(*evt->xfer_iu) * i;
		evt->xfer_iu = pool->iu_storage + i;
		evt->hostdata = hostdata;
		evt->ext_list = NULL;
		evt->ext_list_token = 0;
	}

	return 0;
}

/**
 * release_event_pool: - Frees memory of an event pool of a host
 * @pool:	event_pool to be released
 * @hostdata:	ibmvscsi_host_data who owns the even pool
 *
 * Returns zero on success.
*/
static void release_event_pool(struct event_pool *pool,
			       struct ibmvscsi_host_data *hostdata)
{
	int i, in_use = 0;
	for (i = 0; i < pool->size; ++i) {
		if (atomic_read(&pool->events[i].free) != 1)
			++in_use;
		if (pool->events[i].ext_list) {
			dma_free_coherent(hostdata->dev,
				  SG_ALL * sizeof(struct srp_direct_buf),
				  pool->events[i].ext_list,
				  pool->events[i].ext_list_token);
		}
	}
	if (in_use)
		printk(KERN_WARNING
		       "ibmvscsi: releasing event pool with %d "
		       "events still in use?\n", in_use);
	kfree(pool->events);
	dma_free_coherent(hostdata->dev,
			  pool->size * sizeof(*pool->iu_storage),
			  pool->iu_storage, pool->iu_token);
}

/**
 * valid_event_struct: - Determines if event is valid.
 * @pool:	event_pool that contains the event
 * @evt:	srp_event_struct to be checked for validity
 *
 * Returns zero if event is invalid, one otherwise.
*/
static int valid_event_struct(struct event_pool *pool,
				struct srp_event_struct *evt)
{
	int index = evt - pool->events;
	if (index < 0 || index >= pool->size)	/* outside of bounds */
		return 0;
	if (evt != pool->events + index)	/* unaligned */
		return 0;
	return 1;
}

/**
 * ibmvscsi_free-event_struct: - Changes status of event to "free"
 * @pool:	event_pool that contains the event
 * @evt:	srp_event_struct to be modified
 *
*/
static void free_event_struct(struct event_pool *pool,
				       struct srp_event_struct *evt)
{
	if (!valid_event_struct(pool, evt)) {
		printk(KERN_ERR
		       "ibmvscsi: Freeing invalid event_struct %p "
		       "(not in pool %p)\n", evt, pool->events);
		return;
	}
	if (atomic_inc_return(&evt->free) != 1) {
		printk(KERN_ERR
		       "ibmvscsi: Freeing event_struct %p "
		       "which is not in use!\n", evt);
		return;
	}
}

/**
 * get_evt_struct: - Gets the next free event in pool
 * @pool:	event_pool that contains the events to be searched
 *
 * Returns the next event in "free" state, and NULL if none are free.
 * Note that no synchronization is done here, we assume the host_lock
 * will syncrhonze things.
*/
static struct srp_event_struct *get_event_struct(struct event_pool *pool)
{
	int i;
	int poolsize = pool->size;
	int offset = pool->next;

	for (i = 0; i < poolsize; i++) {
		offset = (offset + 1) % poolsize;
		if (!atomic_dec_if_positive(&pool->events[offset].free)) {
			pool->next = offset;
			return &pool->events[offset];
		}
	}

	printk(KERN_ERR "ibmvscsi: found no event struct in pool!\n");
	return NULL;
}

/**
 * init_event_struct: Initialize fields in an event struct that are always 
 *                    required.
 * @evt:        The event
 * @done:       Routine to call when the event is responded to
 * @format:     SRP or MAD format
 * @timeout:    timeout value set in the CRQ
 */
static void init_event_struct(struct srp_event_struct *evt_struct,
			      void (*done) (struct srp_event_struct *),
			      u8 format,
			      int timeout)
{
	evt_struct->cmnd = NULL;
	evt_struct->cmnd_done = NULL;
	evt_struct->sync_srp = NULL;
	evt_struct->crq.format = format;
	evt_struct->crq.timeout = timeout;
	evt_struct->done = done;
}

/* ------------------------------------------------------------
 * Routines for receiving SCSI responses from the hosting partition
 */

/**
 * set_srp_direction: Set the fields in the srp related to data
 *     direction and number of buffers based on the direction in
 *     the scsi_cmnd and the number of buffers
 */
static void set_srp_direction(struct scsi_cmnd *cmd,
			      struct srp_cmd *srp_cmd, 
			      int numbuf)
{
	u8 fmt;

	if (numbuf == 0)
		return;
	
	if (numbuf == 1)
		fmt = SRP_DATA_DESC_DIRECT;
	else {
		fmt = SRP_DATA_DESC_INDIRECT;
		numbuf = min(numbuf, MAX_INDIRECT_BUFS);

		if (cmd->sc_data_direction == DMA_TO_DEVICE)
			srp_cmd->data_out_desc_cnt = numbuf;
		else
			srp_cmd->data_in_desc_cnt = numbuf;
	}

	if (cmd->sc_data_direction == DMA_TO_DEVICE)
		srp_cmd->buf_fmt = fmt << 4;
	else
		srp_cmd->buf_fmt = fmt;
}

static void unmap_sg_list(int num_entries,
		struct device *dev,
		struct srp_direct_buf *md)
{
	int i;

	for (i = 0; i < num_entries; ++i)
		dma_unmap_single(dev, md[i].va, md[i].len, DMA_BIDIRECTIONAL);
}

/**
 * unmap_cmd_data: - Unmap data pointed in srp_cmd based on the format
 * @cmd:	srp_cmd whose additional_data member will be unmapped
 * @dev:	device for which the memory is mapped
 *
*/
static void unmap_cmd_data(struct srp_cmd *cmd,
			   struct srp_event_struct *evt_struct,
			   struct device *dev)
{
	u8 out_fmt, in_fmt;

	out_fmt = cmd->buf_fmt >> 4;
	in_fmt = cmd->buf_fmt & ((1U << 4) - 1);

	if (out_fmt == SRP_NO_DATA_DESC && in_fmt == SRP_NO_DATA_DESC)
		return;
	else if (out_fmt == SRP_DATA_DESC_DIRECT ||
		 in_fmt == SRP_DATA_DESC_DIRECT) {
		struct srp_direct_buf *data =
			(struct srp_direct_buf *) cmd->add_data;
		dma_unmap_single(dev, data->va, data->len, DMA_BIDIRECTIONAL);
	} else {
		struct srp_indirect_buf *indirect =
			(struct srp_indirect_buf *) cmd->add_data;
		int num_mapped = indirect->table_desc.len /
			sizeof(struct srp_direct_buf);

		if (num_mapped <= MAX_INDIRECT_BUFS) {
			unmap_sg_list(num_mapped, dev, &indirect->desc_list[0]);
			return;
		}

		unmap_sg_list(num_mapped, dev, evt_struct->ext_list);
	}
}

static int map_sg_list(int num_entries, 
		       struct scatterlist *sg,
		       struct srp_direct_buf *md)
{
	int i;
	u64 total_length = 0;

	for (i = 0; i < num_entries; ++i) {
		struct srp_direct_buf *descr = md + i;
		struct scatterlist *sg_entry = &sg[i];
		descr->va = sg_dma_address(sg_entry);
		descr->len = sg_dma_len(sg_entry);
		descr->key = 0;
		total_length += sg_dma_len(sg_entry);
 	}
	return total_length;
}

/**
 * map_sg_data: - Maps dma for a scatterlist and initializes decriptor fields
 * @cmd:	Scsi_Cmnd with the scatterlist
 * @srp_cmd:	srp_cmd that contains the memory descriptor
 * @dev:	device for which to map dma memory
 *
 * Called by map_data_for_srp_cmd() when building srp cmd from scsi cmd.
 * Returns 1 on success.
*/
static int map_sg_data(struct scsi_cmnd *cmd,
		       struct srp_event_struct *evt_struct,
		       struct srp_cmd *srp_cmd, struct device *dev)
{

	int sg_mapped;
	u64 total_length = 0;
	struct scatterlist *sg = cmd->request_buffer;
	struct srp_direct_buf *data =
		(struct srp_direct_buf *) srp_cmd->add_data;
	struct srp_indirect_buf *indirect =
		(struct srp_indirect_buf *) data;

	sg_mapped = dma_map_sg(dev, sg, cmd->use_sg, DMA_BIDIRECTIONAL);

	if (sg_mapped == 0)
		return 0;

	set_srp_direction(cmd, srp_cmd, sg_mapped);

	/* special case; we can use a single direct descriptor */
	if (sg_mapped == 1) {
		data->va = sg_dma_address(&sg[0]);
		data->len = sg_dma_len(&sg[0]);
		data->key = 0;
		return 1;
	}

	if (sg_mapped > SG_ALL) {
		printk(KERN_ERR
		       "ibmvscsi: More than %d mapped sg entries, got %d\n",
		       SG_ALL, sg_mapped);
		return 0;
	}

	indirect->table_desc.va = 0;
	indirect->table_desc.len = sg_mapped * sizeof(struct srp_direct_buf);
	indirect->table_desc.key = 0;

	if (sg_mapped <= MAX_INDIRECT_BUFS) {
		total_length = map_sg_list(sg_mapped, sg,
					   &indirect->desc_list[0]);
		indirect->len = total_length;
		return 1;
	}

	/* get indirect table */
	if (!evt_struct->ext_list) {
		evt_struct->ext_list = (struct srp_direct_buf *)
			dma_alloc_coherent(dev, 
					   SG_ALL * sizeof(struct srp_direct_buf),
					   &evt_struct->ext_list_token, 0);
		if (!evt_struct->ext_list) {
			printk(KERN_ERR
			       "ibmvscsi: Can't allocate memory for indirect table\n");
			return 0;
			
		}
	}

	total_length = map_sg_list(sg_mapped, sg, evt_struct->ext_list);	

	indirect->len = total_length;
	indirect->table_desc.va = evt_struct->ext_list_token;
	indirect->table_desc.len = sg_mapped * sizeof(indirect->desc_list[0]);
	memcpy(indirect->desc_list, evt_struct->ext_list,
	       MAX_INDIRECT_BUFS * sizeof(struct srp_direct_buf));
	
 	return 1;
}

/**
 * map_single_data: - Maps memory and initializes memory decriptor fields
 * @cmd:	struct scsi_cmnd with the memory to be mapped
 * @srp_cmd:	srp_cmd that contains the memory descriptor
 * @dev:	device for which to map dma memory
 *
 * Called by map_data_for_srp_cmd() when building srp cmd from scsi cmd.
 * Returns 1 on success.
*/
static int map_single_data(struct scsi_cmnd *cmd,
			   struct srp_cmd *srp_cmd, struct device *dev)
{
	struct srp_direct_buf *data =
		(struct srp_direct_buf *) srp_cmd->add_data;

	data->va =
		dma_map_single(dev, cmd->request_buffer,
			       cmd->request_bufflen,
			       DMA_BIDIRECTIONAL);
	if (dma_mapping_error(data->va)) {
		printk(KERN_ERR
		       "ibmvscsi: Unable to map request_buffer for command!\n");
		return 0;
	}
	data->len = cmd->request_bufflen;
	data->key = 0;

	set_srp_direction(cmd, srp_cmd, 1);

	return 1;
}

/**
 * map_data_for_srp_cmd: - Calls functions to map data for srp cmds
 * @cmd:	struct scsi_cmnd with the memory to be mapped
 * @srp_cmd:	srp_cmd that contains the memory descriptor
 * @dev:	dma device for which to map dma memory
 *
 * Called by scsi_cmd_to_srp_cmd() when converting scsi cmds to srp cmds 
 * Returns 1 on success.
*/
static int map_data_for_srp_cmd(struct scsi_cmnd *cmd,
				struct srp_event_struct *evt_struct,
				struct srp_cmd *srp_cmd, struct device *dev)
{
	switch (cmd->sc_data_direction) {
	case DMA_FROM_DEVICE:
	case DMA_TO_DEVICE:
		break;
	case DMA_NONE:
		return 1;
	case DMA_BIDIRECTIONAL:
		printk(KERN_ERR
		       "ibmvscsi: Can't map DMA_BIDIRECTIONAL to read/write\n");
		return 0;
	default:
		printk(KERN_ERR
		       "ibmvscsi: Unknown data direction 0x%02x; can't map!\n",
		       cmd->sc_data_direction);
		return 0;
	}

	if (!cmd->request_buffer)
		return 1;
	if (cmd->use_sg)
		return map_sg_data(cmd, evt_struct, srp_cmd, dev);
	return map_single_data(cmd, srp_cmd, dev);
}

/* ------------------------------------------------------------
 * Routines for sending and receiving SRPs
 */
/**
 * ibmvscsi_send_srp_event: - Transforms event to u64 array and calls send_crq()
 * @evt_struct:	evt_struct to be sent
 * @hostdata:	ibmvscsi_host_data of host
 *
 * Returns the value returned from ibmvscsi_send_crq(). (Zero for success)
 * Note that this routine assumes that host_lock is held for synchronization
*/
static int ibmvscsi_send_srp_event(struct srp_event_struct *evt_struct,
				   struct ibmvscsi_host_data *hostdata)
{
	u64 *crq_as_u64 = (u64 *) &evt_struct->crq;
	int request_status;
	int rc;

	/* If we have exhausted our request limit, just fail this request.
	 * Note that there are rare cases involving driver generated requests 
	 * (such as task management requests) that the mid layer may think we
	 * can handle more requests (can_queue) when we actually can't
	 */
	if (evt_struct->crq.format == VIOSRP_SRP_FORMAT) {
		request_status =
			atomic_dec_if_positive(&hostdata->request_limit);
		/* If request limit was -1 when we started, it is now even
		 * less than that
		 */
		if (request_status < -1)
			goto send_error;
		/* Otherwise, if we have run out of requests */
		else if (request_status < 0)
			goto send_busy;
	}

	/* Copy the IU into the transfer area */
	*evt_struct->xfer_iu = evt_struct->iu;
	evt_struct->xfer_iu->srp.rsp.tag = (u64)evt_struct;

	/* Add this to the sent list.  We need to do this 
	 * before we actually send 
	 * in case it comes back REALLY fast
	 */
	list_add_tail(&evt_struct->list, &hostdata->sent);

	if ((rc =
	     ibmvscsi_send_crq(hostdata, crq_as_u64[0], crq_as_u64[1])) != 0) {
		list_del(&evt_struct->list);

		printk(KERN_ERR "ibmvscsi: send error %d\n",
		       rc);
		goto send_error;
	}

	return 0;

 send_busy:
	unmap_cmd_data(&evt_struct->iu.srp.cmd, evt_struct, hostdata->dev);

	free_event_struct(&hostdata->pool, evt_struct);
 	return SCSI_MLQUEUE_HOST_BUSY;

 send_error:
	unmap_cmd_data(&evt_struct->iu.srp.cmd, evt_struct, hostdata->dev);

	if (evt_struct->cmnd != NULL) {
		evt_struct->cmnd->result = DID_ERROR << 16;
		evt_struct->cmnd_done(evt_struct->cmnd);
	} else if (evt_struct->done)
		evt_struct->done(evt_struct);

	free_event_struct(&hostdata->pool, evt_struct);
	return 0;
}

/**
 * handle_cmd_rsp: -  Handle responses from commands
 * @evt_struct:	srp_event_struct to be handled
 *
 * Used as a callback by when sending scsi cmds.
 * Gets called by ibmvscsi_handle_crq()
*/
static void handle_cmd_rsp(struct srp_event_struct *evt_struct)
{
	struct srp_rsp *rsp = &evt_struct->xfer_iu->srp.rsp;
	struct scsi_cmnd *cmnd = evt_struct->cmnd;

	if (unlikely(rsp->opcode != SRP_RSP)) {
		if (printk_ratelimit())
			printk(KERN_WARNING 
			       "ibmvscsi: bad SRP RSP type %d\n",
			       rsp->opcode);
	}
	
	if (cmnd) {
		cmnd->result = rsp->status;
		if (((cmnd->result >> 1) & 0x1f) == CHECK_CONDITION)
			memcpy(cmnd->sense_buffer,
			       rsp->data,
			       rsp->sense_data_len);
		unmap_cmd_data(&evt_struct->iu.srp.cmd, 
			       evt_struct, 
			       evt_struct->hostdata->dev);

		if (rsp->flags & SRP_RSP_FLAG_DOOVER)
			cmnd->resid = rsp->data_out_res_cnt;
		else if (rsp->flags & SRP_RSP_FLAG_DIOVER)
			cmnd->resid = rsp->data_in_res_cnt;
	}

	if (evt_struct->cmnd_done)
		evt_struct->cmnd_done(cmnd);
}

/**
 * lun_from_dev: - Returns the lun of the scsi device
 * @dev:	struct scsi_device
 *
*/
static inline u16 lun_from_dev(struct scsi_device *dev)
{
	return (0x2 << 14) | (dev->id << 8) | (dev->channel << 5) | dev->lun;
}

/**
 * ibmvscsi_queue: - The queuecommand function of the scsi template 
 * @cmd:	struct scsi_cmnd to be executed
 * @done:	Callback function to be called when cmd is completed
*/
static int ibmvscsi_queuecommand(struct scsi_cmnd *cmnd,
				 void (*done) (struct scsi_cmnd *))
{
	struct srp_cmd *srp_cmd;
	struct srp_event_struct *evt_struct;
	struct srp_indirect_buf *indirect;
	struct ibmvscsi_host_data *hostdata =
		(struct ibmvscsi_host_data *)&cmnd->device->host->hostdata;
	u16 lun = lun_from_dev(cmnd->device);
	u8 out_fmt, in_fmt;

	evt_struct = get_event_struct(&hostdata->pool);
	if (!evt_struct)
		return SCSI_MLQUEUE_HOST_BUSY;

	/* Set up the actual SRP IU */
	srp_cmd = &evt_struct->iu.srp.cmd;
	memset(srp_cmd, 0x00, SRP_MAX_IU_LEN);
	srp_cmd->opcode = SRP_CMD;
	memcpy(srp_cmd->cdb, cmnd->cmnd, sizeof(cmnd->cmnd));
	srp_cmd->lun = ((u64) lun) << 48;

	if (!map_data_for_srp_cmd(cmnd, evt_struct, srp_cmd, hostdata->dev)) {
		printk(KERN_ERR "ibmvscsi: couldn't convert cmd to srp_cmd\n");
		free_event_struct(&hostdata->pool, evt_struct);
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	init_event_struct(evt_struct,
			  handle_cmd_rsp,
			  VIOSRP_SRP_FORMAT,
			  cmnd->timeout_per_command/HZ);

	evt_struct->cmnd = cmnd;
	evt_struct->cmnd_done = done;

	/* Fix up dma address of the buffer itself */
	indirect = (struct srp_indirect_buf *) srp_cmd->add_data;
	out_fmt = srp_cmd->buf_fmt >> 4;
	in_fmt = srp_cmd->buf_fmt & ((1U << 4) - 1);
	if ((in_fmt == SRP_DATA_DESC_INDIRECT ||
	     out_fmt == SRP_DATA_DESC_INDIRECT) &&
	    indirect->table_desc.va == 0) {
		indirect->table_desc.va = evt_struct->crq.IU_data_ptr +
			offsetof(struct srp_cmd, add_data) +
			offsetof(struct srp_indirect_buf, desc_list);
	}

	return ibmvscsi_send_srp_event(evt_struct, hostdata);
}

/* ------------------------------------------------------------
 * Routines for driver initialization
 */
/**
 * adapter_info_rsp: - Handle response to MAD adapter info request
 * @evt_struct:	srp_event_struct with the response
 *
 * Used as a "done" callback by when sending adapter_info. Gets called
 * by ibmvscsi_handle_crq()
*/
static void adapter_info_rsp(struct srp_event_struct *evt_struct)
{
	struct ibmvscsi_host_data *hostdata = evt_struct->hostdata;
	dma_unmap_single(hostdata->dev,
			 evt_struct->iu.mad.adapter_info.buffer,
			 evt_struct->iu.mad.adapter_info.common.length,
			 DMA_BIDIRECTIONAL);

	if (evt_struct->xfer_iu->mad.adapter_info.common.status) {
		printk("ibmvscsi: error %d getting adapter info\n",
		       evt_struct->xfer_iu->mad.adapter_info.common.status);
	} else {
		printk("ibmvscsi: host srp version: %s, "
		       "host partition %s (%d), OS %d, max io %u\n",
		       hostdata->madapter_info.srp_version,
		       hostdata->madapter_info.partition_name,
		       hostdata->madapter_info.partition_number,
		       hostdata->madapter_info.os_type,
		       hostdata->madapter_info.port_max_txu[0]);
		
		if (hostdata->madapter_info.port_max_txu[0]) 
			hostdata->host->max_sectors = 
				hostdata->madapter_info.port_max_txu[0] >> 9;
		
		if (hostdata->madapter_info.os_type == 3 &&
		    strcmp(hostdata->madapter_info.srp_version, "1.6a") <= 0) {
			printk("ibmvscsi: host (Ver. %s) doesn't support large"
			       "transfers\n",
			       hostdata->madapter_info.srp_version);
			printk("ibmvscsi: limiting scatterlists to %d\n",
			       MAX_INDIRECT_BUFS);
			hostdata->host->sg_tablesize = MAX_INDIRECT_BUFS;
		}
	}
}

/**
 * send_mad_adapter_info: - Sends the mad adapter info request
 *      and stores the result so it can be retrieved with
 *      sysfs.  We COULD consider causing a failure if the
 *      returned SRP version doesn't match ours.
 * @hostdata:	ibmvscsi_host_data of host
 * 
 * Returns zero if successful.
*/
static void send_mad_adapter_info(struct ibmvscsi_host_data *hostdata)
{
	struct viosrp_adapter_info *req;
	struct srp_event_struct *evt_struct;
	dma_addr_t addr;

	evt_struct = get_event_struct(&hostdata->pool);
	if (!evt_struct) {
		printk(KERN_ERR "ibmvscsi: couldn't allocate an event "
		       "for ADAPTER_INFO_REQ!\n");
		return;
	}

	init_event_struct(evt_struct,
			  adapter_info_rsp,
			  VIOSRP_MAD_FORMAT,
			  init_timeout * HZ);
	
	req = &evt_struct->iu.mad.adapter_info;
	memset(req, 0x00, sizeof(*req));
	
	req->common.type = VIOSRP_ADAPTER_INFO_TYPE;
	req->common.length = sizeof(hostdata->madapter_info);
	req->buffer = addr = dma_map_single(hostdata->dev,
					    &hostdata->madapter_info,
					    sizeof(hostdata->madapter_info),
					    DMA_BIDIRECTIONAL);

	if (dma_mapping_error(req->buffer)) {
		printk(KERN_ERR
		       "ibmvscsi: Unable to map request_buffer "
		       "for adapter_info!\n");
		free_event_struct(&hostdata->pool, evt_struct);
		return;
	}
	
	if (ibmvscsi_send_srp_event(evt_struct, hostdata)) {
		printk(KERN_ERR "ibmvscsi: couldn't send ADAPTER_INFO_REQ!\n");
		dma_unmap_single(hostdata->dev,
				 addr,
				 sizeof(hostdata->madapter_info),
				 DMA_BIDIRECTIONAL);
	}
};

/**
 * login_rsp: - Handle response to SRP login request
 * @evt_struct:	srp_event_struct with the response
 *
 * Used as a "done" callback by when sending srp_login. Gets called
 * by ibmvscsi_handle_crq()
*/
static void login_rsp(struct srp_event_struct *evt_struct)
{
	struct ibmvscsi_host_data *hostdata = evt_struct->hostdata;
	switch (evt_struct->xfer_iu->srp.login_rsp.opcode) {
	case SRP_LOGIN_RSP:	/* it worked! */
		break;
	case SRP_LOGIN_REJ:	/* refused! */
		printk(KERN_INFO "ibmvscsi: SRP_LOGIN_REJ reason %u\n",
		       evt_struct->xfer_iu->srp.login_rej.reason);
		/* Login failed.  */
		atomic_set(&hostdata->request_limit, -1);
		return;
	default:
		printk(KERN_ERR
		       "ibmvscsi: Invalid login response typecode 0x%02x!\n",
		       evt_struct->xfer_iu->srp.login_rsp.opcode);
		/* Login failed.  */
		atomic_set(&hostdata->request_limit, -1);
		return;
	}

	printk(KERN_INFO "ibmvscsi: SRP_LOGIN succeeded\n");

	if (evt_struct->xfer_iu->srp.login_rsp.req_lim_delta >
	    (max_requests - 2))
		evt_struct->xfer_iu->srp.login_rsp.req_lim_delta =
		    max_requests - 2;

	/* Now we know what the real request-limit is */
	atomic_set(&hostdata->request_limit,
		   evt_struct->xfer_iu->srp.login_rsp.req_lim_delta);

	hostdata->host->can_queue =
	    evt_struct->xfer_iu->srp.login_rsp.req_lim_delta - 2;

	if (hostdata->host->can_queue < 1) {
		printk(KERN_ERR "ibmvscsi: Invalid request_limit_delta\n");
		return;
	}

	/* If we had any pending I/Os, kick them */
	scsi_unblock_requests(hostdata->host);

	send_mad_adapter_info(hostdata);
	return;
}

/**
 * send_srp_login: - Sends the srp login
 * @hostdata:	ibmvscsi_host_data of host
 * 
 * Returns zero if successful.
*/
static int send_srp_login(struct ibmvscsi_host_data *hostdata)
{
	int rc;
	unsigned long flags;
	struct srp_login_req *login;
	struct srp_event_struct *evt_struct = get_event_struct(&hostdata->pool);
	if (!evt_struct) {
		printk(KERN_ERR
		       "ibmvscsi: couldn't allocate an event for login req!\n");
		return FAILED;
	}

	init_event_struct(evt_struct,
			  login_rsp,
			  VIOSRP_SRP_FORMAT,
			  init_timeout * HZ);

	login = &evt_struct->iu.srp.login_req;
	memset(login, 0x00, sizeof(struct srp_login_req));
	login->opcode = SRP_LOGIN_REQ;
	login->req_it_iu_len = sizeof(union srp_iu);
	login->req_buf_fmt = SRP_BUF_FORMAT_DIRECT | SRP_BUF_FORMAT_INDIRECT;
	
	spin_lock_irqsave(hostdata->host->host_lock, flags);
	/* Start out with a request limit of 1, since this is negotiated in
	 * the login request we are just sending
	 */
	atomic_set(&hostdata->request_limit, 1);

	rc = ibmvscsi_send_srp_event(evt_struct, hostdata);
	spin_unlock_irqrestore(hostdata->host->host_lock, flags);
	printk("ibmvscsic: sent SRP login\n");
	return rc;
};

/**
 * sync_completion: Signal that a synchronous command has completed
 * Note that after returning from this call, the evt_struct is freed.
 * the caller waiting on this completion shouldn't touch the evt_struct
 * again.
 */
static void sync_completion(struct srp_event_struct *evt_struct)
{
	/* copy the response back */
	if (evt_struct->sync_srp)
		*evt_struct->sync_srp = *evt_struct->xfer_iu;
	
	complete(&evt_struct->comp);
}

/**
 * ibmvscsi_abort: Abort a command...from scsi host template
 * send this over to the server and wait synchronously for the response
 */
static int ibmvscsi_eh_abort_handler(struct scsi_cmnd *cmd)
{
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)cmd->device->host->hostdata;
	struct srp_tsk_mgmt *tsk_mgmt;
	struct srp_event_struct *evt;
	struct srp_event_struct *tmp_evt, *found_evt;
	union viosrp_iu srp_rsp;
	int rsp_rc;
	unsigned long flags;
	u16 lun = lun_from_dev(cmd->device);

	/* First, find this command in our sent list so we can figure
	 * out the correct tag
	 */
	spin_lock_irqsave(hostdata->host->host_lock, flags);
	found_evt = NULL;
	list_for_each_entry(tmp_evt, &hostdata->sent, list) {
		if (tmp_evt->cmnd == cmd) {
			found_evt = tmp_evt;
			break;
		}
	}

	if (!found_evt) {
		spin_unlock_irqrestore(hostdata->host->host_lock, flags);
		return FAILED;
	}

	evt = get_event_struct(&hostdata->pool);
	if (evt == NULL) {
		spin_unlock_irqrestore(hostdata->host->host_lock, flags);
		printk(KERN_ERR "ibmvscsi: failed to allocate abort event\n");
		return FAILED;
	}
	
	init_event_struct(evt,
			  sync_completion,
			  VIOSRP_SRP_FORMAT,
			  init_timeout * HZ);

	tsk_mgmt = &evt->iu.srp.tsk_mgmt;
	
	/* Set up an abort SRP command */
	memset(tsk_mgmt, 0x00, sizeof(*tsk_mgmt));
	tsk_mgmt->opcode = SRP_TSK_MGMT;
	tsk_mgmt->lun = ((u64) lun) << 48;
	tsk_mgmt->tsk_mgmt_func = SRP_TSK_ABORT_TASK;
	tsk_mgmt->task_tag = (u64) found_evt;

	printk(KERN_INFO "ibmvscsi: aborting command. lun 0x%lx, tag 0x%lx\n",
	       tsk_mgmt->lun, tsk_mgmt->task_tag);

	evt->sync_srp = &srp_rsp;
	init_completion(&evt->comp);
	rsp_rc = ibmvscsi_send_srp_event(evt, hostdata);
	spin_unlock_irqrestore(hostdata->host->host_lock, flags);
	if (rsp_rc != 0) {
		printk(KERN_ERR "ibmvscsi: failed to send abort() event\n");
		return FAILED;
	}

	wait_for_completion(&evt->comp);

	/* make sure we got a good response */
	if (unlikely(srp_rsp.srp.rsp.opcode != SRP_RSP)) {
		if (printk_ratelimit())
			printk(KERN_WARNING 
			       "ibmvscsi: abort bad SRP RSP type %d\n",
			       srp_rsp.srp.rsp.opcode);
		return FAILED;
	}

	if (srp_rsp.srp.rsp.flags & SRP_RSP_FLAG_RSPVALID)
		rsp_rc = *((int *)srp_rsp.srp.rsp.data);
	else
		rsp_rc = srp_rsp.srp.rsp.status;

	if (rsp_rc) {
		if (printk_ratelimit())
			printk(KERN_WARNING 
			       "ibmvscsi: abort code %d for task tag 0x%lx\n",
			       rsp_rc,
			       tsk_mgmt->task_tag);
		return FAILED;
	}

	/* Because we dropped the spinlock above, it's possible
	 * The event is no longer in our list.  Make sure it didn't
	 * complete while we were aborting
	 */
	spin_lock_irqsave(hostdata->host->host_lock, flags);
	found_evt = NULL;
	list_for_each_entry(tmp_evt, &hostdata->sent, list) {
		if (tmp_evt->cmnd == cmd) {
			found_evt = tmp_evt;
			break;
		}
	}

	if (found_evt == NULL) {
		spin_unlock_irqrestore(hostdata->host->host_lock, flags);
		printk(KERN_INFO
		       "ibmvscsi: aborted task tag 0x%lx completed\n",
		       tsk_mgmt->task_tag);
		return SUCCESS;
	}

	printk(KERN_INFO
	       "ibmvscsi: successfully aborted task tag 0x%lx\n",
	       tsk_mgmt->task_tag);

	cmd->result = (DID_ABORT << 16);
	list_del(&found_evt->list);
	unmap_cmd_data(&found_evt->iu.srp.cmd, found_evt,
		       found_evt->hostdata->dev);
	free_event_struct(&found_evt->hostdata->pool, found_evt);
	spin_unlock_irqrestore(hostdata->host->host_lock, flags);
	atomic_inc(&hostdata->request_limit);
	return SUCCESS;
}

/**
 * ibmvscsi_eh_device_reset_handler: Reset a single LUN...from scsi host 
 * template send this over to the server and wait synchronously for the 
 * response
 */
static int ibmvscsi_eh_device_reset_handler(struct scsi_cmnd *cmd)
{
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)cmd->device->host->hostdata;

	struct srp_tsk_mgmt *tsk_mgmt;
	struct srp_event_struct *evt;
	struct srp_event_struct *tmp_evt, *pos;
	union viosrp_iu srp_rsp;
	int rsp_rc;
	unsigned long flags;
	u16 lun = lun_from_dev(cmd->device);

	spin_lock_irqsave(hostdata->host->host_lock, flags);
	evt = get_event_struct(&hostdata->pool);
	if (evt == NULL) {
		spin_unlock_irqrestore(hostdata->host->host_lock, flags);
		printk(KERN_ERR "ibmvscsi: failed to allocate reset event\n");
		return FAILED;
	}
	
	init_event_struct(evt,
			  sync_completion,
			  VIOSRP_SRP_FORMAT,
			  init_timeout * HZ);

	tsk_mgmt = &evt->iu.srp.tsk_mgmt;

	/* Set up a lun reset SRP command */
	memset(tsk_mgmt, 0x00, sizeof(*tsk_mgmt));
	tsk_mgmt->opcode = SRP_TSK_MGMT;
	tsk_mgmt->lun = ((u64) lun) << 48;
	tsk_mgmt->tsk_mgmt_func = SRP_TSK_LUN_RESET;

	printk(KERN_INFO "ibmvscsi: resetting device. lun 0x%lx\n",
	       tsk_mgmt->lun);

	evt->sync_srp = &srp_rsp;
	init_completion(&evt->comp);
	rsp_rc = ibmvscsi_send_srp_event(evt, hostdata);
	spin_unlock_irqrestore(hostdata->host->host_lock, flags);
	if (rsp_rc != 0) {
		printk(KERN_ERR "ibmvscsi: failed to send reset event\n");
		return FAILED;
	}

	wait_for_completion(&evt->comp);

	/* make sure we got a good response */
	if (unlikely(srp_rsp.srp.rsp.opcode != SRP_RSP)) {
		if (printk_ratelimit())
			printk(KERN_WARNING 
			       "ibmvscsi: reset bad SRP RSP type %d\n",
			       srp_rsp.srp.rsp.opcode);
		return FAILED;
	}

	if (srp_rsp.srp.rsp.flags & SRP_RSP_FLAG_RSPVALID)
		rsp_rc = *((int *)srp_rsp.srp.rsp.data);
	else
		rsp_rc = srp_rsp.srp.rsp.status;

	if (rsp_rc) {
		if (printk_ratelimit())
			printk(KERN_WARNING 
			       "ibmvscsi: reset code %d for task tag 0x%lx\n",
			       rsp_rc, tsk_mgmt->task_tag);
		return FAILED;
	}

	/* We need to find all commands for this LUN that have not yet been
	 * responded to, and fail them with DID_RESET
	 */
	spin_lock_irqsave(hostdata->host->host_lock, flags);
	list_for_each_entry_safe(tmp_evt, pos, &hostdata->sent, list) {
		if ((tmp_evt->cmnd) && (tmp_evt->cmnd->device == cmd->device)) {
			if (tmp_evt->cmnd)
				tmp_evt->cmnd->result = (DID_RESET << 16);
			list_del(&tmp_evt->list);
			unmap_cmd_data(&tmp_evt->iu.srp.cmd, tmp_evt,
				       tmp_evt->hostdata->dev);
			free_event_struct(&tmp_evt->hostdata->pool,
						   tmp_evt);
			atomic_inc(&hostdata->request_limit);
			if (tmp_evt->cmnd_done)
				tmp_evt->cmnd_done(tmp_evt->cmnd);
			else if (tmp_evt->done)
				tmp_evt->done(tmp_evt);
		}
	}
	spin_unlock_irqrestore(hostdata->host->host_lock, flags);
	return SUCCESS;
}

/**
 * purge_requests: Our virtual adapter just shut down.  purge any sent requests
 * @hostdata:    the adapter
 */
static void purge_requests(struct ibmvscsi_host_data *hostdata, int error_code)
{
	struct srp_event_struct *tmp_evt, *pos;
	unsigned long flags;

	spin_lock_irqsave(hostdata->host->host_lock, flags);
	list_for_each_entry_safe(tmp_evt, pos, &hostdata->sent, list) {
		list_del(&tmp_evt->list);
		if (tmp_evt->cmnd) {
			tmp_evt->cmnd->result = (error_code << 16);
			unmap_cmd_data(&tmp_evt->iu.srp.cmd, 
				       tmp_evt,	
				       tmp_evt->hostdata->dev);
			if (tmp_evt->cmnd_done)
				tmp_evt->cmnd_done(tmp_evt->cmnd);
		} else {
			if (tmp_evt->done) {
				tmp_evt->done(tmp_evt);
			}
		}
		free_event_struct(&tmp_evt->hostdata->pool, tmp_evt);
	}
	spin_unlock_irqrestore(hostdata->host->host_lock, flags);
}

/**
 * ibmvscsi_handle_crq: - Handles and frees received events in the CRQ
 * @crq:	Command/Response queue
 * @hostdata:	ibmvscsi_host_data of host
 *
*/
void ibmvscsi_handle_crq(struct viosrp_crq *crq,
			 struct ibmvscsi_host_data *hostdata)
{
	unsigned long flags;
	struct srp_event_struct *evt_struct =
	    (struct srp_event_struct *)crq->IU_data_ptr;
	switch (crq->valid) {
	case 0xC0:		/* initialization */
		switch (crq->format) {
		case 0x01:	/* Initialization message */
			printk(KERN_INFO "ibmvscsi: partner initialized\n");
			/* Send back a response */
			if (ibmvscsi_send_crq(hostdata,
					      0xC002000000000000LL, 0) == 0) {
				/* Now login */
				send_srp_login(hostdata);
			} else {
				printk(KERN_ERR
				       "ibmvscsi: Unable to send init rsp\n");
			}

			break;
		case 0x02:	/* Initialization response */
			printk(KERN_INFO
			       "ibmvscsi: partner initialization complete\n");

			/* Now login */
			send_srp_login(hostdata);
			break;
		default:
			printk(KERN_ERR "ibmvscsi: unknown crq message type\n");
		}
		return;
	case 0xFF:	/* Hypervisor telling us the connection is closed */
		scsi_block_requests(hostdata->host);
		atomic_set(&hostdata->request_limit, 0);
		if (crq->format == 0x06) {
			/* We need to re-setup the interpartition connection */
			printk(KERN_INFO
			       "ibmvscsi: Re-enabling adapter!\n");
			purge_requests(hostdata, DID_REQUEUE);
			if ((ibmvscsi_reenable_crq_queue(&hostdata->queue,
							hostdata)) ||
			    (ibmvscsi_send_crq(hostdata,
					       0xC001000000000000LL, 0))) {
					atomic_set(&hostdata->request_limit,
						   -1);
					printk(KERN_ERR
					       "ibmvscsi: error after"
					       " enable\n");
			}
		} else {
			printk(KERN_INFO
			       "ibmvscsi: Virtual adapter failed rc %d!\n",
			       crq->format);

			purge_requests(hostdata, DID_ERROR);
			if ((ibmvscsi_reset_crq_queue(&hostdata->queue,
							hostdata)) ||
			    (ibmvscsi_send_crq(hostdata,
					       0xC001000000000000LL, 0))) {
					atomic_set(&hostdata->request_limit,
						   -1);
					printk(KERN_ERR
					       "ibmvscsi: error after reset\n");
			}
		}
		scsi_unblock_requests(hostdata->host);
		return;
	case 0x80:		/* real payload */
		break;
	default:
		printk(KERN_ERR
		       "ibmvscsi: got an invalid message type 0x%02x\n",
		       crq->valid);
		return;
	}

	/* The only kind of payload CRQs we should get are responses to
	 * things we send. Make sure this response is to something we
	 * actually sent
	 */
	if (!valid_event_struct(&hostdata->pool, evt_struct)) {
		printk(KERN_ERR
		       "ibmvscsi: returned correlation_token 0x%p is invalid!\n",
		       (void *)crq->IU_data_ptr);
		return;
	}

	if (atomic_read(&evt_struct->free)) {
		printk(KERN_ERR
		       "ibmvscsi: received duplicate  correlation_token 0x%p!\n",
		       (void *)crq->IU_data_ptr);
		return;
	}

	if (crq->format == VIOSRP_SRP_FORMAT)
		atomic_add(evt_struct->xfer_iu->srp.rsp.req_lim_delta,
			   &hostdata->request_limit);

	if (evt_struct->done)
		evt_struct->done(evt_struct);
	else
		printk(KERN_ERR
		       "ibmvscsi: returned done() is NULL; not running it!\n");

	/*
	 * Lock the host_lock before messing with these structures, since we
	 * are running in a task context
	 */
	spin_lock_irqsave(evt_struct->hostdata->host->host_lock, flags);
	list_del(&evt_struct->list);
	free_event_struct(&evt_struct->hostdata->pool, evt_struct);
	spin_unlock_irqrestore(evt_struct->hostdata->host->host_lock, flags);
}

/**
 * ibmvscsi_get_host_config: Send the command to the server to get host
 * configuration data.  The data is opaque to us.
 */
static int ibmvscsi_do_host_config(struct ibmvscsi_host_data *hostdata,
				   unsigned char *buffer, int length)
{
	struct viosrp_host_config *host_config;
	struct srp_event_struct *evt_struct;
	dma_addr_t addr;
	int rc;

	evt_struct = get_event_struct(&hostdata->pool);
	if (!evt_struct) {
		printk(KERN_ERR
		       "ibmvscsi: could't allocate event for HOST_CONFIG!\n");
		return -1;
	}

	init_event_struct(evt_struct,
			  sync_completion,
			  VIOSRP_MAD_FORMAT,
			  init_timeout * HZ);

	host_config = &evt_struct->iu.mad.host_config;

	/* Set up a lun reset SRP command */
	memset(host_config, 0x00, sizeof(*host_config));
	host_config->common.type = VIOSRP_HOST_CONFIG_TYPE;
	host_config->common.length = length;
	host_config->buffer = addr = dma_map_single(hostdata->dev, buffer,
						    length,
						    DMA_BIDIRECTIONAL);

	if (dma_mapping_error(host_config->buffer)) {
		printk(KERN_ERR
		       "ibmvscsi: dma_mapping error " "getting host config\n");
		free_event_struct(&hostdata->pool, evt_struct);
		return -1;
	}

	init_completion(&evt_struct->comp);
	rc = ibmvscsi_send_srp_event(evt_struct, hostdata);
	if (rc == 0)
		wait_for_completion(&evt_struct->comp);
	dma_unmap_single(hostdata->dev, addr, length, DMA_BIDIRECTIONAL);

	return rc;
}

/* ------------------------------------------------------------
 * sysfs attributes
 */
static ssize_t show_host_srp_version(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)shost->hostdata;
	int len;

	len = snprintf(buf, buff_size, "%s\n",
		       hostdata->madapter_info.srp_version);
	return len;
}

static struct class_device_attribute ibmvscsi_host_srp_version = {
	.attr = {
		 .name = "srp_version",
		 .mode = S_IRUGO,
		 },
	.show = show_host_srp_version,
};

static ssize_t show_host_partition_name(struct class_device *class_dev,
					char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)shost->hostdata;
	int len;

	len = snprintf(buf, buff_size, "%s\n",
		       hostdata->madapter_info.partition_name);
	return len;
}

static struct class_device_attribute ibmvscsi_host_partition_name = {
	.attr = {
		 .name = "partition_name",
		 .mode = S_IRUGO,
		 },
	.show = show_host_partition_name,
};

static ssize_t show_host_partition_number(struct class_device *class_dev,
					  char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)shost->hostdata;
	int len;

	len = snprintf(buf, buff_size, "%d\n",
		       hostdata->madapter_info.partition_number);
	return len;
}

static struct class_device_attribute ibmvscsi_host_partition_number = {
	.attr = {
		 .name = "partition_number",
		 .mode = S_IRUGO,
		 },
	.show = show_host_partition_number,
};

static ssize_t show_host_mad_version(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)shost->hostdata;
	int len;

	len = snprintf(buf, buff_size, "%d\n",
		       hostdata->madapter_info.mad_version);
	return len;
}

static struct class_device_attribute ibmvscsi_host_mad_version = {
	.attr = {
		 .name = "mad_version",
		 .mode = S_IRUGO,
		 },
	.show = show_host_mad_version,
};

static ssize_t show_host_os_type(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)shost->hostdata;
	int len;

	len = snprintf(buf, buff_size, "%d\n", hostdata->madapter_info.os_type);
	return len;
}

static struct class_device_attribute ibmvscsi_host_os_type = {
	.attr = {
		 .name = "os_type",
		 .mode = S_IRUGO,
		 },
	.show = show_host_os_type,
};

static ssize_t show_host_config(struct class_device *class_dev, char *buf)
{
	struct Scsi_Host *shost = class_to_shost(class_dev);
	struct ibmvscsi_host_data *hostdata =
	    (struct ibmvscsi_host_data *)shost->hostdata;

	/* returns null-terminated host config data */
	if (ibmvscsi_do_host_config(hostdata, buf, buff_size) == 0)
		return strlen(buf);
	else
		return 0;
}

static struct class_device_attribute ibmvscsi_host_config = {
	.attr = {
		 .name = "config",
		 .mode = S_IRUGO,
		 },
	.show = show_host_config,
};

static struct class_device_attribute *ibmvscsi_attrs[] = {
	&ibmvscsi_host_srp_version,
	&ibmvscsi_host_partition_name,
	&ibmvscsi_host_partition_number,
	&ibmvscsi_host_mad_version,
	&ibmvscsi_host_os_type,
	&ibmvscsi_host_config,
	NULL
};

/* ------------------------------------------------------------
 * SCSI driver registration
 */
static struct scsi_host_template driver_template = {
	.module = THIS_MODULE,
	.name = "IBM POWER Virtual SCSI Adapter " IBMVSCSI_VERSION,
	.proc_name = "ibmvscsi",
	.queuecommand = ibmvscsi_queuecommand,
	.eh_abort_handler = ibmvscsi_eh_abort_handler,
	.eh_device_reset_handler = ibmvscsi_eh_device_reset_handler,
	.cmd_per_lun = 16,
	.can_queue = 1,		/* Updated after SRP_LOGIN */
	.this_id = -1,
	.sg_tablesize = SG_ALL,
	.use_clustering = ENABLE_CLUSTERING,
	.shost_attrs = ibmvscsi_attrs,
};

/**
 * Called by bus code for each adapter
 */
static int ibmvscsi_probe(struct vio_dev *vdev, const struct vio_device_id *id)
{
	struct ibmvscsi_host_data *hostdata;
	struct Scsi_Host *host;
	struct device *dev = &vdev->dev;
	unsigned long wait_switch = 0;
	int rc;

	vdev->dev.driver_data = NULL;

	host = scsi_host_alloc(&driver_template, sizeof(*hostdata));
	if (!host) {
		printk(KERN_ERR "ibmvscsi: couldn't allocate host data\n");
		goto scsi_host_alloc_failed;
	}

	hostdata = (struct ibmvscsi_host_data *)host->hostdata;
	memset(hostdata, 0x00, sizeof(*hostdata));
	INIT_LIST_HEAD(&hostdata->sent);
	hostdata->host = host;
	hostdata->dev = dev;
	atomic_set(&hostdata->request_limit, -1);
	hostdata->host->max_sectors = 32 * 8; /* default max I/O 32 pages */

	rc = ibmvscsi_init_crq_queue(&hostdata->queue, hostdata, max_requests);
	if (rc != 0 && rc != H_RESOURCE) {
		printk(KERN_ERR "ibmvscsi: couldn't initialize crq\n");
		goto init_crq_failed;
	}
	if (initialize_event_pool(&hostdata->pool, max_requests, hostdata) != 0) {
		printk(KERN_ERR "ibmvscsi: couldn't initialize event pool\n");
		goto init_pool_failed;
	}

	host->max_lun = 8;
	host->max_id = max_id;
	host->max_channel = max_channel;

	if (scsi_add_host(hostdata->host, hostdata->dev))
		goto add_host_failed;

	/* Try to send an initialization message.  Note that this is allowed
	 * to fail if the other end is not acive.  In that case we don't
	 * want to scan
	 */
	if (ibmvscsi_send_crq(hostdata, 0xC001000000000000LL, 0) == 0
	    || rc == H_RESOURCE) {
		/*
		 * Wait around max init_timeout secs for the adapter to finish
		 * initializing. When we are done initializing, we will have a
		 * valid request_limit.  We don't want Linux scanning before
		 * we are ready.
		 */
		for (wait_switch = jiffies + (init_timeout * HZ);
		     time_before(jiffies, wait_switch) &&
		     atomic_read(&hostdata->request_limit) < 2;) {

			msleep(10);
		}

		/* if we now have a valid request_limit, initiate a scan */
		if (atomic_read(&hostdata->request_limit) > 0)
			scsi_scan_host(host);
	}

	vdev->dev.driver_data = hostdata;
	return 0;

      add_host_failed:
	release_event_pool(&hostdata->pool, hostdata);
      init_pool_failed:
	ibmvscsi_release_crq_queue(&hostdata->queue, hostdata, max_requests);
      init_crq_failed:
	scsi_host_put(host);
      scsi_host_alloc_failed:
	return -1;
}

static int ibmvscsi_remove(struct vio_dev *vdev)
{
	struct ibmvscsi_host_data *hostdata = vdev->dev.driver_data;
	release_event_pool(&hostdata->pool, hostdata);
	ibmvscsi_release_crq_queue(&hostdata->queue, hostdata,
				   max_requests);
	
	scsi_remove_host(hostdata->host);
	scsi_host_put(hostdata->host);

	return 0;
}

/**
 * ibmvscsi_device_table: Used by vio.c to match devices in the device tree we 
 * support.
 */
static struct vio_device_id ibmvscsi_device_table[] __devinitdata = {
	{"vscsi", "IBM,v-scsi"},
	{ "", "" }
};
MODULE_DEVICE_TABLE(vio, ibmvscsi_device_table);

static struct vio_driver ibmvscsi_driver = {
	.id_table = ibmvscsi_device_table,
	.probe = ibmvscsi_probe,
	.remove = ibmvscsi_remove,
	.driver = {
		.name = "ibmvscsi",
		.owner = THIS_MODULE,
	}
};

int __init ibmvscsi_module_init(void)
{
	return vio_register_driver(&ibmvscsi_driver);
}

void __exit ibmvscsi_module_exit(void)
{
	vio_unregister_driver(&ibmvscsi_driver);
}

module_init(ibmvscsi_module_init);
module_exit(ibmvscsi_module_exit);
