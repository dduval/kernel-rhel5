/* 
 * This file is part of the zfcp device driver for
 * FCP adapters for IBM System z9 and zSeries.
 *
 * (C) Copyright IBM Corp. 2002, 2006
 * 
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 2, or (at your option) 
 * any later version. 
 * 
 * This program is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 */

#define ZFCP_LOG_AREA			ZFCP_LOG_AREA_ERP

#include "zfcp_ext.h"

static int zfcp_erp_adisc(struct zfcp_port *);
static void zfcp_erp_adisc_handler(unsigned long);

static int zfcp_erp_adapter_reopen_internal(struct zfcp_adapter *, int, u8,
					    void *);
static int zfcp_erp_port_forced_reopen_internal(struct zfcp_port *, int, u8,
						void *);
static int zfcp_erp_port_reopen_internal(struct zfcp_port *, int, u8, void *);
static int zfcp_erp_unit_reopen_internal(struct zfcp_unit *, int, u8, void *);

static int zfcp_erp_port_reopen_all_internal(struct zfcp_adapter *, int, u8,
					     void *);
static int zfcp_erp_unit_reopen_all_internal(struct zfcp_port *, int, u8,
					     void *);

static void zfcp_erp_adapter_block(struct zfcp_adapter *, int);
static void zfcp_erp_adapter_unblock(struct zfcp_adapter *);
static void zfcp_erp_port_block(struct zfcp_port *, int);
static void zfcp_erp_port_unblock(struct zfcp_port *);
static void zfcp_erp_unit_block(struct zfcp_unit *, int);
static void zfcp_erp_unit_unblock(struct zfcp_unit *);

static int zfcp_erp_thread(void *);

static int zfcp_erp_strategy(struct zfcp_erp_action *);

static int zfcp_erp_strategy_do_action(struct zfcp_erp_action *);
static int zfcp_erp_strategy_memwait(struct zfcp_erp_action *);
static int zfcp_erp_strategy_check_target(struct zfcp_erp_action *, int);
static int zfcp_erp_strategy_check_unit(struct zfcp_unit *, int);
static int zfcp_erp_strategy_check_port(struct zfcp_port *, int);
static int zfcp_erp_strategy_check_adapter(struct zfcp_adapter *, int);
static int zfcp_erp_strategy_statechange(int, u32, struct zfcp_adapter *,
					 struct zfcp_port *,
					 struct zfcp_unit *, int);
static inline int zfcp_erp_strategy_statechange_detected(atomic_t *, u32);
static int zfcp_erp_strategy_followup_actions(int, struct zfcp_adapter *,
					      struct zfcp_port *,
					      struct zfcp_unit *, int);
static int zfcp_erp_strategy_check_queues(struct zfcp_adapter *);
static int zfcp_erp_strategy_check_action(struct zfcp_erp_action *, int);

static int zfcp_erp_adapter_strategy(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_generic(struct zfcp_erp_action *, int);
static int zfcp_erp_adapter_strategy_close(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_open(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_open_qdio(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_open_fsf(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_open_fsf_xconfig(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_open_fsf_xport(struct zfcp_erp_action *);
static int zfcp_erp_adapter_strategy_open_fsf_statusread(
	struct zfcp_erp_action *);

static int zfcp_erp_port_forced_strategy(struct zfcp_erp_action *);
static int zfcp_erp_port_forced_strategy_close(struct zfcp_erp_action *);

static int zfcp_erp_port_strategy(struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_clearstati(struct zfcp_port *);
static int zfcp_erp_port_strategy_close(struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_open(struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_open_nameserver(struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_open_nameserver_wakeup(
	struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_open_common(struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_open_common_lookup(struct zfcp_erp_action *);
static int zfcp_erp_port_strategy_open_port(struct zfcp_erp_action *);

static int zfcp_erp_unit_strategy(struct zfcp_erp_action *);
static int zfcp_erp_unit_strategy_clearstati(struct zfcp_unit *);
static int zfcp_erp_unit_strategy_close(struct zfcp_erp_action *);
static int zfcp_erp_unit_strategy_open(struct zfcp_erp_action *);

static void zfcp_erp_action_dismiss_adapter(struct zfcp_adapter *);
static void zfcp_erp_action_dismiss_port(struct zfcp_port *);
static void zfcp_erp_action_dismiss_unit(struct zfcp_unit *);
static void zfcp_erp_action_dismiss(struct zfcp_erp_action *);

static int zfcp_erp_action_enqueue(int, struct zfcp_adapter *,
				   struct zfcp_port *, struct zfcp_unit *,
				   u8 id, void *ref);
static int zfcp_erp_action_dequeue(struct zfcp_erp_action *);
static void zfcp_erp_action_cleanup(int, struct zfcp_adapter *,
				    struct zfcp_port *, struct zfcp_unit *,
				    int);

static void zfcp_erp_action_ready(struct zfcp_erp_action *);
static int  zfcp_erp_action_exists(struct zfcp_erp_action *);

static inline void zfcp_erp_action_to_ready(struct zfcp_erp_action *);
static inline void zfcp_erp_action_to_running(struct zfcp_erp_action *);

static void zfcp_erp_memwait_handler(unsigned long);

/**
 * zfcp_close_qdio - close qdio queues for an adapter
 */
static void zfcp_close_qdio(struct zfcp_adapter *adapter)
{
	struct zfcp_qdio_queue *req_queue;
	int first, count;

	if (!atomic_test_mask(ZFCP_STATUS_ADAPTER_QDIOUP, &adapter->status))
		return;

	/* clear QDIOUP flag, thus do_QDIO is not called during qdio_shutdown */
	req_queue = &adapter->request_queue;
	write_lock_irq(&req_queue->queue_lock);
	atomic_clear_mask(ZFCP_STATUS_ADAPTER_QDIOUP, &adapter->status);
	write_unlock_irq(&req_queue->queue_lock);

	while (qdio_shutdown(adapter->ccw_device,
			     QDIO_FLAG_CLEANUP_USING_CLEAR) == -EINPROGRESS)
		msleep(1000);

	/* cleanup used outbound sbals */
	count = atomic_read(&req_queue->free_count);
	if (count < QDIO_MAX_BUFFERS_PER_Q) {
		first = (req_queue->free_index+count) % QDIO_MAX_BUFFERS_PER_Q;
		count = QDIO_MAX_BUFFERS_PER_Q - count;
		zfcp_qdio_zero_sbals(req_queue->buffer, first, count);
	}
	req_queue->free_index = 0;
	atomic_set(&req_queue->free_count, 0);
	req_queue->distance_from_int = 0;
	adapter->response_queue.free_index = 0;
	atomic_set(&adapter->response_queue.free_count, 0);
}

/**
 * zfcp_close_fsf - stop FSF operations for an adapter
 *
 * Dismiss and cleanup all pending fsf_reqs (this wakes up all initiators of
 * requests waiting for completion; especially this returns SCSI commands
 * with error state).
 */
static void zfcp_close_fsf(struct zfcp_adapter *adapter)
{
	/* close queues to ensure that buffers are not accessed by adapter */
	zfcp_close_qdio(adapter);
	zfcp_fsf_req_dismiss_all(adapter);
	/* reset FSF request sequence number */
	adapter->fsf_req_seq_no = 0;
	/* all ports and units are closed */
	zfcp_erp_modify_adapter_status(adapter, 24, 0,
				       ZFCP_STATUS_COMMON_OPEN, ZFCP_CLEAR);
}

/**
 * zfcp_fsf_request_timeout_handler - called if a request timed out
 * @data: pointer to adapter for handler function
 *
 * This function needs to be called if requests (ELS, Generic Service,
 * or SCSI commands) exceed a certain time limit. The assumption is
 * that after the time limit the adapter get stuck. So we trigger a reopen of
 * the adapter.
 */
static void zfcp_fsf_request_timeout_handler(unsigned long data)
{
	struct zfcp_adapter *adapter = (struct zfcp_adapter *) data;
	zfcp_erp_adapter_reopen(adapter, ZFCP_STATUS_COMMON_ERP_FAILED, 62, 0);
}

void zfcp_fsf_start_timer(struct zfcp_fsf_req *fsf_req, unsigned long timeout)
{
	fsf_req->timer.function = zfcp_fsf_request_timeout_handler;
	fsf_req->timer.data = (unsigned long) fsf_req->adapter;
	fsf_req->timer.expires = jiffies + timeout;
	add_timer(&fsf_req->timer);
}

/*
 * function:	
 *
 * purpose:	called if an adapter failed,
 *		initiates adapter recovery which is done
 *		asynchronously
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
int zfcp_erp_adapter_reopen_internal(struct zfcp_adapter *adapter,
				     int clear_mask, u8 id, void *ref)
{
	int retval;

	ZFCP_LOG_DEBUG("reopen adapter %s\n",
		       zfcp_get_busid_by_adapter(adapter));

	zfcp_erp_adapter_block(adapter, clear_mask);

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &adapter->status)) {
		ZFCP_LOG_DEBUG("skipped reopen of failed adapter %s\n",
			       zfcp_get_busid_by_adapter(adapter));
		/* ensure propagation of failed status to new devices */
		zfcp_erp_adapter_failed(adapter, 13, 0);
		retval = -EIO;
		goto out;
	}
	retval = zfcp_erp_action_enqueue(ZFCP_ERP_ACTION_REOPEN_ADAPTER,
					 adapter, NULL, NULL, id, ref);

 out:
	return retval;
}

/*
 * function:	
 *
 * purpose:	Wrappper for zfcp_erp_adapter_reopen_internal
 *              used to ensure the correct locking
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
int zfcp_erp_adapter_reopen(struct zfcp_adapter *adapter, int clear_mask,
			    u8 id, void *ref)
{
	int retval;
	unsigned long flags;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);
	retval = zfcp_erp_adapter_reopen_internal(adapter, clear_mask, id, ref);
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	return retval;
}

int zfcp_erp_adapter_shutdown(struct zfcp_adapter *adapter, int clear_mask,
			      u8 id, void *ref)
{
	int retval;

	retval = zfcp_erp_adapter_reopen(adapter,
					 ZFCP_STATUS_COMMON_RUNNING |
					 ZFCP_STATUS_COMMON_ERP_FAILED |
					 clear_mask, id, ref);

	return retval;
}

int zfcp_erp_port_shutdown(struct zfcp_port *port, int clear_mask, u8 id,
			   void *ref)
{
	int retval;

	retval = zfcp_erp_port_reopen(port,
				      ZFCP_STATUS_COMMON_RUNNING |
				      ZFCP_STATUS_COMMON_ERP_FAILED |
				      clear_mask, id, ref);

	return retval;
}

int zfcp_erp_unit_shutdown(struct zfcp_unit *unit, int clear_mask, u8 id,
			   void *ref)
{
	int retval;

	retval = zfcp_erp_unit_reopen(unit,
				      ZFCP_STATUS_COMMON_RUNNING |
				      ZFCP_STATUS_COMMON_ERP_FAILED |
				      clear_mask, id, ref);

	return retval;
}


/**
 * zfcp_erp_adisc - send ADISC ELS command
 * @port: port structure
 */
int
zfcp_erp_adisc(struct zfcp_port *port)
{
	struct zfcp_adapter *adapter = port->adapter;
	struct zfcp_send_els *send_els;
	struct zfcp_ls_adisc *adisc;
	void *address = NULL;
	int retval = 0;

	send_els = kzalloc(sizeof(struct zfcp_send_els), GFP_ATOMIC);
	if (send_els == NULL)
		goto nomem;

	send_els->req = kzalloc(sizeof(struct scatterlist), GFP_ATOMIC);
	if (send_els->req == NULL)
		goto nomem;

	send_els->resp = kzalloc(sizeof(struct scatterlist), GFP_ATOMIC);
	if (send_els->resp == NULL)
		goto nomem;

	address = (void *) get_zeroed_page(GFP_ATOMIC);
	if (address == NULL)
		goto nomem;

	zfcp_address_to_sg(address, send_els->req);
	address += PAGE_SIZE >> 1;
	zfcp_address_to_sg(address, send_els->resp);
	send_els->req_count = send_els->resp_count = 1;

	send_els->adapter = adapter;
	send_els->port = port;
	send_els->d_id = port->d_id;
	send_els->handler = zfcp_erp_adisc_handler;
	send_els->handler_data = (unsigned long) send_els;

	adisc = zfcp_sg_to_address(send_els->req);
	send_els->ls_code = adisc->code = ZFCP_LS_ADISC;

	send_els->req->length = sizeof(struct zfcp_ls_adisc);
	send_els->resp->length = sizeof(struct zfcp_ls_adisc_acc);

	/* acc. to FC-FS, hard_nport_id in ADISC should not be set for ports
	   without FC-AL-2 capability, so we don't set it */
	adisc->wwpn = fc_host_port_name(adapter->scsi_host);
	adisc->wwnn = fc_host_node_name(adapter->scsi_host);
	adisc->nport_id = fc_host_port_id(adapter->scsi_host);
	ZFCP_LOG_INFO("ADISC request from s_id 0x%08x to d_id 0x%08x "
		      "(wwpn=0x%016Lx, wwnn=0x%016Lx, "
		      "hard_nport_id=0x%08x, nport_id=0x%08x)\n",
		      adisc->nport_id, send_els->d_id, (wwn_t) adisc->wwpn,
		      (wwn_t) adisc->wwnn, adisc->hard_nport_id,
		      adisc->nport_id);

	retval = zfcp_fsf_send_els(send_els);
	if (retval != 0)
		goto freemem;

	goto out;

 nomem:
	retval = -ENOMEM;
 freemem:
	if (address != NULL)
		__free_pages(send_els->req->page, 0);
	if (send_els != NULL) {
		kfree(send_els->req);
		kfree(send_els->resp);
		kfree(send_els);
	}
 out:
	return retval;
}


/**
 * zfcp_erp_adisc_handler - handler for ADISC ELS command
 * @data: pointer to struct zfcp_send_els
 *
 * If ADISC failed (LS_RJT or timed out) forced reopen of the port is triggered.
 */
void
zfcp_erp_adisc_handler(unsigned long data)
{
	struct zfcp_send_els *send_els;
	struct zfcp_port *port;
	struct zfcp_adapter *adapter;
	u32 d_id;
	struct zfcp_ls_adisc_acc *adisc;

	send_els = (struct zfcp_send_els *) data;
	adapter = send_els->adapter;
	port = send_els->port;
	d_id = send_els->d_id;

	/* request rejected or timed out */
	if (send_els->status != 0) {
		zfcp_erp_port_forced_reopen(port, 0, 63, 0);
		goto out;
	}

	adisc = zfcp_sg_to_address(send_els->resp);

	ZFCP_LOG_INFO("ADISC response from d_id 0x%08x to s_id "
		      "0x%08x (wwpn=0x%016Lx, wwnn=0x%016Lx, "
		      "hard_nport_id=0x%08x, nport_id=0x%08x)\n",
		      d_id, fc_host_port_id(adapter->scsi_host),
		      (wwn_t) adisc->wwpn, (wwn_t) adisc->wwnn,
		      adisc->hard_nport_id, adisc->nport_id);

	/* set wwnn for port */
	if (port->wwnn == 0)
		port->wwnn = adisc->wwnn;

	if (port->wwpn != adisc->wwpn) {
		ZFCP_LOG_NORMAL("d_id assignment changed, reopening "
				"port (adapter %s, wwpn=0x%016Lx, "
				"adisc_resp_wwpn=0x%016Lx)\n",
				zfcp_get_busid_by_port(port),
				port->wwpn, (wwn_t) adisc->wwpn);
		if (zfcp_erp_port_reopen(port, 0, 64, 0))
			ZFCP_LOG_NORMAL("failed reopen of port "
					"(adapter %s, wwpn=0x%016Lx)\n",
					zfcp_get_busid_by_port(port),
					port->wwpn);
	}

 out:
	zfcp_port_put(port);
	__free_pages(send_els->req->page, 0);
	kfree(send_els->req);
	kfree(send_els->resp);
	kfree(send_els);
}


/**
 * zfcp_test_link - lightweight link test procedure
 * @port: port to be tested
 *
 * Test status of a link to a remote port using the ELS command ADISC.
 */
int
zfcp_test_link(struct zfcp_port *port)
{
	int retval;

	zfcp_port_get(port);
	retval = zfcp_erp_adisc(port);
	if (retval != 0 && retval != -EBUSY) {
		zfcp_port_put(port);
		retval = zfcp_erp_port_forced_reopen(port, 0, 65, 0);
		if (retval != 0)
			retval = -EPERM;
	}

	return retval;
}


/*
 * function:	
 *
 * purpose:	called if a port failed to be opened normally
 *		initiates Forced Reopen recovery which is done
 *		asynchronously
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
static int zfcp_erp_port_forced_reopen_internal(struct zfcp_port *port,
						int clear_mask, u8 id, void *ref)
{
	int retval;

	ZFCP_LOG_DEBUG("forced reopen of port 0x%016Lx on adapter %s\n",
		       port->wwpn, zfcp_get_busid_by_port(port));

	zfcp_erp_port_block(port, clear_mask);

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &port->status)) {
		ZFCP_LOG_DEBUG("skipped forced reopen of failed port 0x%016Lx "
			       "on adapter %s\n", port->wwpn,
			       zfcp_get_busid_by_port(port));
		retval = -EIO;
		goto out;
	}

	retval = zfcp_erp_action_enqueue(ZFCP_ERP_ACTION_REOPEN_PORT_FORCED,
					 port->adapter, port, NULL, id, ref);

 out:
	return retval;
}

/*
 * function:	
 *
 * purpose:	Wrappper for zfcp_erp_port_forced_reopen_internal
 *              used to ensure the correct locking
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
int zfcp_erp_port_forced_reopen(struct zfcp_port *port, int clear_mask, u8 id,
				void *ref)
{
	int retval;
	unsigned long flags;
	struct zfcp_adapter *adapter;

	adapter = port->adapter;
	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);
	retval = zfcp_erp_port_forced_reopen_internal(port, clear_mask, id,
						      ref);
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	return retval;
}

/*
 * function:	
 *
 * purpose:	called if a port is to be opened
 *		initiates Reopen recovery which is done
 *		asynchronously
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
static int zfcp_erp_port_reopen_internal(struct zfcp_port *port, int clear_mask,
					 u8 id, void *ref)
{
	int retval;

	ZFCP_LOG_DEBUG("reopen of port 0x%016Lx on adapter %s\n",
		       port->wwpn, zfcp_get_busid_by_port(port));

	zfcp_erp_port_block(port, clear_mask);

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &port->status)) {
		ZFCP_LOG_DEBUG("skipped reopen of failed port 0x%016Lx "
			       "on adapter %s\n", port->wwpn,
			       zfcp_get_busid_by_port(port));
		/* ensure propagation of failed status to new devices */
		zfcp_erp_port_failed(port, 14, 0);
		retval = -EIO;
		goto out;
	}

	retval = zfcp_erp_action_enqueue(ZFCP_ERP_ACTION_REOPEN_PORT,
					 port->adapter, port, NULL, id, ref);

 out:
	return retval;
}

/**
 * zfcp_erp_port_reopen - initiate reopen of a remote port
 * @port: port to be reopened
 * @clear_mask: specifies flags in port status to be cleared
 * Return: 0 on success, < 0 on error
 *
 * This is a wrappper function for zfcp_erp_port_reopen_internal. It ensures
 * correct locking. An error recovery task is initiated to do the reopen.
 * To wait for the completion of the reopen zfcp_erp_wait should be used.
 */
int zfcp_erp_port_reopen(struct zfcp_port *port, int clear_mask, u8 id, void *ref)
{
	int retval;
	unsigned long flags;
	struct zfcp_adapter *adapter = port->adapter;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);
	retval = zfcp_erp_port_reopen_internal(port, clear_mask, id, ref);
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	return retval;
}

/*
 * function:	
 *
 * purpose:	called if a unit is to be opened
 *		initiates Reopen recovery which is done
 *		asynchronously
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
static int zfcp_erp_unit_reopen_internal(struct zfcp_unit *unit, int clear_mask,
					 u8 id, void *ref)
{
	int retval;
	struct zfcp_adapter *adapter = unit->port->adapter;

	ZFCP_LOG_DEBUG("reopen of unit 0x%016Lx on port 0x%016Lx "
		       "on adapter %s\n", unit->fcp_lun,
		       unit->port->wwpn, zfcp_get_busid_by_unit(unit));

	zfcp_erp_unit_block(unit, clear_mask);

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &unit->status)) {
		ZFCP_LOG_DEBUG("skipped reopen of failed unit 0x%016Lx "
			       "on port 0x%016Lx on adapter %s\n",
			       unit->fcp_lun, unit->port->wwpn,
			       zfcp_get_busid_by_unit(unit));
		retval = -EIO;
		goto out;
	}

	retval = zfcp_erp_action_enqueue(ZFCP_ERP_ACTION_REOPEN_UNIT,
					 adapter, unit->port, unit, id, ref);
 out:
	return retval;
}

/**
 * zfcp_erp_unit_reopen - initiate reopen of a unit
 * @unit: unit to be reopened
 * @clear_mask: specifies flags in unit status to be cleared
 * Return: 0 on success, < 0 on error
 *
 * This is a wrappper for zfcp_erp_unit_reopen_internal. It ensures correct
 * locking. An error recovery task is initiated to do the reopen.
 * To wait for the completion of the reopen zfcp_erp_wait should be used.
 */
int zfcp_erp_unit_reopen(struct zfcp_unit *unit, int clear_mask, u8 id, void *ref)
{
	int retval;
	unsigned long flags;
	struct zfcp_adapter *adapter;
	struct zfcp_port *port;

	port = unit->port;
	adapter = port->adapter;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);
	retval = zfcp_erp_unit_reopen_internal(unit, clear_mask, id, ref);
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	return retval;
}

/**
 * zfcp_erp_adapter_block - mark adapter as blocked, block scsi requests
 */
static void zfcp_erp_adapter_block(struct zfcp_adapter *adapter, int clear_mask)
{
	zfcp_erp_modify_adapter_status(adapter, 15, 0,
				       ZFCP_STATUS_COMMON_UNBLOCKED |
				       clear_mask, ZFCP_CLEAR);
}

/* FIXME: isn't really atomic */
/*
 * returns the mask which has not been set so far, i.e.
 * 0 if no bit has been changed, !0 if some bit has been changed
 */
static int atomic_test_and_set_mask(unsigned long mask, atomic_t *v)
{
	int changed_bits = (atomic_read(v) /*XOR*/^ mask) & mask;
	atomic_set_mask(mask, v);
	return changed_bits;
}

/* FIXME: isn't really atomic */
/*
 * returns the mask which has not been cleared so far, i.e.
 * 0 if no bit has been changed, !0 if some bit has been changed
 */
static int atomic_test_and_clear_mask(unsigned long mask, atomic_t *v)
{
	int changed_bits = atomic_read(v) & mask;
	atomic_clear_mask(mask, v);
	return changed_bits;
}

/**
 * zfcp_erp_adapter_unblock - mark adapter as unblocked, allow scsi requests
 */
static void zfcp_erp_adapter_unblock(struct zfcp_adapter *adapter)
{
	if (atomic_test_and_set_mask(ZFCP_STATUS_COMMON_UNBLOCKED,
				     &adapter->status))
		zfcp_rec_dbf_event_adapter(16, 0, adapter);
}

/*
 * function:	
 *
 * purpose:	disable I/O,
 *		return any open requests and clean them up,
 *		aim: no pending and incoming I/O
 *
 * returns:
 */
static void
zfcp_erp_port_block(struct zfcp_port *port, int clear_mask)
{
	zfcp_erp_modify_port_status(port, 17, 0,
				    ZFCP_STATUS_COMMON_UNBLOCKED | clear_mask,
				    ZFCP_CLEAR);
}

/*
 * function:	
 *
 * purpose:	enable I/O
 *
 * returns:
 */
static void
zfcp_erp_port_unblock(struct zfcp_port *port)
{
	if (atomic_test_and_set_mask(ZFCP_STATUS_COMMON_UNBLOCKED,
				     &port->status))
		zfcp_rec_dbf_event_port(18, 0, port);
}

/*
 * function:	
 *
 * purpose:	disable I/O,
 *		return any open requests and clean them up,
 *		aim: no pending and incoming I/O
 *
 * returns:
 */
static void
zfcp_erp_unit_block(struct zfcp_unit *unit, int clear_mask)
{
	zfcp_erp_modify_unit_status(unit, 19, 0,
				    ZFCP_STATUS_COMMON_UNBLOCKED | clear_mask,
				    ZFCP_CLEAR);
}

/*
 * function:	
 *
 * purpose:	enable I/O
 *
 * returns:
 */
static void
zfcp_erp_unit_unblock(struct zfcp_unit *unit)
{
	if (atomic_test_and_set_mask(ZFCP_STATUS_COMMON_UNBLOCKED,
				     &unit->status))
		zfcp_rec_dbf_event_unit(20, 0, unit);
}

static void
zfcp_erp_action_ready(struct zfcp_erp_action *erp_action)
{
	struct zfcp_adapter *adapter = erp_action->adapter;

	zfcp_erp_action_to_ready(erp_action);
	up(&adapter->erp_ready_sem);
	zfcp_rec_dbf_event_thread(2, adapter, 0);
}

/*
 * function:	
 *
 * purpose:
 *
 * returns:	<0			erp_action not found in any list
 *		ZFCP_ERP_ACTION_READY	erp_action is in ready list
 *		ZFCP_ERP_ACTION_RUNNING	erp_action is in running list
 *
 * locks:	erp_lock must be held
 */
static int
zfcp_erp_action_exists(struct zfcp_erp_action *erp_action)
{
	int retval = -EINVAL;
	struct list_head *entry;
	struct zfcp_erp_action *entry_erp_action;
	struct zfcp_adapter *adapter = erp_action->adapter;

	/* search in running list */
	list_for_each(entry, &adapter->erp_running_head) {
		entry_erp_action =
		    list_entry(entry, struct zfcp_erp_action, list);
		if (entry_erp_action == erp_action) {
			retval = ZFCP_ERP_ACTION_RUNNING;
			goto out;
		}
	}
	/* search in ready list */
	list_for_each(entry, &adapter->erp_ready_head) {
		entry_erp_action =
		    list_entry(entry, struct zfcp_erp_action, list);
		if (entry_erp_action == erp_action) {
			retval = ZFCP_ERP_ACTION_READY;
			goto out;
		}
	}

 out:
	return retval;
}

/*
 * purpose:	checks current status of action (timed out, dismissed, ...)
 *		and does appropriate preparations (dismiss fsf request, ...)
 *
 * locks:	called under erp_lock (disabled interrupts)
 *
 * returns:	0
 */
static void
zfcp_erp_strategy_check_fsfreq(struct zfcp_erp_action *erp_action)
{
	struct zfcp_adapter *adapter = erp_action->adapter;

	if (erp_action->fsf_req) {
		/* take lock to ensure that request is not deleted meanwhile */
		spin_lock(&adapter->req_list_lock);
		if (zfcp_reqlist_find_safe(adapter, erp_action->fsf_req) &&
		    erp_action->fsf_req->erp_action == erp_action) {
			/* fsf_req still exists */
			/* dismiss fsf_req of timed out/dismissed erp_action */
			if (erp_action->status & (ZFCP_STATUS_ERP_DISMISSED |
						  ZFCP_STATUS_ERP_TIMEDOUT)) {
				erp_action->fsf_req->status |=
					ZFCP_STATUS_FSFREQ_DISMISSED;
				zfcp_rec_dbf_event_action(142, erp_action);
			}
			if (erp_action->status & ZFCP_STATUS_ERP_TIMEDOUT) {
				zfcp_rec_dbf_event_action(143, erp_action);
				ZFCP_LOG_NORMAL("error: erp step timed out "
						"(action=%d, fsf_req=%p)\n ",
						erp_action->action,
						erp_action->fsf_req);
			}
			/*
			 * If fsf_req is neither dismissed nor completed
			 * then keep it running asynchronously and don't mess
			 * with the association of erp_action and fsf_req.
			 */
			if (erp_action->fsf_req->status &
					(ZFCP_STATUS_FSFREQ_COMPLETED |
					       ZFCP_STATUS_FSFREQ_DISMISSED)) {
				/* forget about association between fsf_req
				   and erp_action */
				erp_action->fsf_req = NULL;
			}
		} else {
			/*
			 * even if this fsf_req has gone, forget about
			 * association between erp_action and fsf_req
			 */
			erp_action->fsf_req = NULL;
		}
		spin_unlock(&adapter->req_list_lock);
	}
}

/**
 * zfcp_erp_async_handler_nolock - complete erp_action
 *
 * Used for normal completion, time-out, dismissal and failure after
 * low memory condition.
 */
static void zfcp_erp_async_handler_nolock(struct zfcp_erp_action *erp_action,
					  unsigned long set_mask)
{
	if (zfcp_erp_action_exists(erp_action) == ZFCP_ERP_ACTION_RUNNING) {
		erp_action->status |= set_mask;
		zfcp_erp_action_ready(erp_action);
	} else {
		/* action is ready or gone - nothing to do */
	}
}

/**
 * zfcp_erp_async_handler - wrapper for erp_async_handler_nolock w/ locking
 */
void zfcp_erp_async_handler(struct zfcp_erp_action *erp_action,
			    unsigned long set_mask)
{
	struct zfcp_adapter *adapter = erp_action->adapter;
	unsigned long flags;

	write_lock_irqsave(&adapter->erp_lock, flags);
	zfcp_erp_async_handler_nolock(erp_action, set_mask);
	write_unlock_irqrestore(&adapter->erp_lock, flags);
}

/*
 * purpose:	is called for erp_action which was slept waiting for
 *		memory becoming avaliable,
 *		will trigger that this action will be continued
 */
static void
zfcp_erp_memwait_handler(unsigned long data)
{
	struct zfcp_erp_action *erp_action = (struct zfcp_erp_action *) data;

	zfcp_erp_async_handler(erp_action, 0);
}

/*
 * purpose:	is called if an asynchronous erp step timed out,
 *		action gets an appropriate flag and will be processed
 *		accordingly
 */
void zfcp_erp_timeout_handler(unsigned long data)
{
	struct zfcp_erp_action *erp_action = (struct zfcp_erp_action *) data;

	zfcp_erp_async_handler(erp_action, ZFCP_STATUS_ERP_TIMEDOUT);
}

/**
 * zfcp_erp_action_dismiss - dismiss an erp_action
 *
 * adapter->erp_lock must be held
 * 
 * Dismissal of an erp_action is usually required if an erp_action of
 * higher priority is generated.
 */
static void zfcp_erp_action_dismiss(struct zfcp_erp_action *erp_action)
{
	erp_action->status |= ZFCP_STATUS_ERP_DISMISSED;
	if (zfcp_erp_action_exists(erp_action) == ZFCP_ERP_ACTION_RUNNING)
		zfcp_erp_action_ready(erp_action);
}

int
zfcp_erp_thread_setup(struct zfcp_adapter *adapter)
{
	int retval = 0;

	atomic_clear_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_UP, &adapter->status);

	retval = kernel_thread(zfcp_erp_thread, adapter, SIGCHLD);
	if (retval < 0) {
		ZFCP_LOG_NORMAL("error: creation of erp thread failed for "
				"adapter %s\n",
				zfcp_get_busid_by_adapter(adapter));
	} else {
		wait_event(adapter->erp_thread_wqh,
			   atomic_test_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_UP,
					    &adapter->status));
	}

	return (retval < 0);
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:
 *
 * context:	process (i.e. proc-fs or rmmod/insmod)
 *
 * note:	The caller of this routine ensures that the specified
 *		adapter has been shut down and that this operation
 *		has been completed. Thus, there are no pending erp_actions
 *		which would need to be handled here.
 */
int
zfcp_erp_thread_kill(struct zfcp_adapter *adapter)
{
	int retval = 0;

	atomic_set_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_KILL, &adapter->status);
	up(&adapter->erp_ready_sem);
	zfcp_rec_dbf_event_thread(2, adapter, 1);

	wait_event(adapter->erp_thread_wqh,
		   !atomic_test_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_UP,
				     &adapter->status));

	atomic_clear_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_KILL,
			  &adapter->status);

	return retval;
}

/*
 * purpose:	is run as a kernel thread,
 *		goes through list of error recovery actions of associated adapter
 *		and delegates single action to execution
 *
 * returns:	0
 */
static int
zfcp_erp_thread(void *data)
{
	struct zfcp_adapter *adapter = (struct zfcp_adapter *) data;
	struct list_head *next;
	struct zfcp_erp_action *erp_action;
	unsigned long flags;

	daemonize("zfcperp%s", zfcp_get_busid_by_adapter(adapter));
	/* Block all signals */
	siginitsetinv(&current->blocked, 0);
	atomic_set_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_UP, &adapter->status);
	wake_up(&adapter->erp_thread_wqh);

	while (!atomic_test_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_KILL,
				 &adapter->status)) {

		write_lock_irqsave(&adapter->erp_lock, flags);
		next = adapter->erp_ready_head.next;
		write_unlock_irqrestore(&adapter->erp_lock, flags);

		if (next != &adapter->erp_ready_head) {
			erp_action =
			    list_entry(next, struct zfcp_erp_action, list);
			/*
			 * process action (incl. [re]moving it
			 * from 'ready' queue)
			 */
			zfcp_erp_strategy(erp_action);
		}

		/*
		 * sleep as long as there is nothing to do, i.e.
		 * no action in 'ready' queue to be processed and
		 * thread is not to be killed
		 */
		zfcp_rec_dbf_event_thread(4, adapter, 1);
		down_interruptible(&adapter->erp_ready_sem);
		zfcp_rec_dbf_event_thread(5, adapter, 1);
	}

	atomic_clear_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_UP, &adapter->status);
	wake_up(&adapter->erp_thread_wqh);

	return 0;
}

/*
 * function:	
 *
 * purpose:	drives single error recovery action and schedules higher and
 *		subordinate actions, if necessary
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_SUCCEEDED	- action finished successfully (deqd)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully (deqd)
 *		ZFCP_ERP_EXIT		- action finished (dequeued), offline
 *		ZFCP_ERP_DISMISSED	- action canceled (dequeued)
 */
static int
zfcp_erp_strategy(struct zfcp_erp_action *erp_action)
{
	int retval = 0;
	struct zfcp_adapter *adapter = erp_action->adapter;
	struct zfcp_port *port = erp_action->port;
	struct zfcp_unit *unit = erp_action->unit;
	int action = erp_action->action;
	u32 status = erp_action->status;
	unsigned long flags;

	/* serialise dismissing, timing out, moving, enqueueing */
	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);

	/* dequeue dismissed action and leave, if required */
	retval = zfcp_erp_strategy_check_action(erp_action, retval);
	if (retval == ZFCP_ERP_DISMISSED) {
		goto unlock;
	}

	/*
	 * move action to 'running' queue before processing it
	 * (to avoid a race condition regarding moving the
	 * action to the 'running' queue and back)
	 */
	zfcp_erp_action_to_running(erp_action);

	/*
	 * try to process action as far as possible,
	 * no lock to allow for blocking operations (kmalloc, qdio, ...),
	 * afterwards the lock is required again for the following reasons:
	 * - dequeueing of finished action and enqueueing of
	 *   follow-up actions must be atomic so that any other
	 *   reopen-routine does not believe there is nothing to do
	 *   and that it is safe to enqueue something else,
	 * - we want to force any control thread which is dismissing
	 *   actions to finish this before we decide about
	 *   necessary steps to be taken here further
	 */
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);
	retval = zfcp_erp_strategy_do_action(erp_action);
	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);

	/*
	 * check for dismissed status again to avoid follow-up actions,
	 * failing of targets and so on for dismissed actions,
	 * we go through down() here because there has been an up()
	 */
	if (erp_action->status & ZFCP_STATUS_ERP_DISMISSED)
		retval = ZFCP_ERP_CONTINUES;

	switch (retval) {
	case ZFCP_ERP_NOMEM:
		/* no memory to continue immediately, let it sleep */
		if (!(erp_action->status & ZFCP_STATUS_ERP_LOWMEM)) {
			++adapter->erp_low_mem_count;
			erp_action->status |= ZFCP_STATUS_ERP_LOWMEM;
		}
		/* This condition is true if there is no memory available
		   for any erp_action on this adapter. This implies that there
		   are no elements in the memory pool(s) left for erp_actions.
		   This might happen if an erp_action that used a memory pool
		   element was timed out.
		 */
		if (adapter->erp_total_count == adapter->erp_low_mem_count) {
			ZFCP_LOG_NORMAL("error: no mempool elements available, "
					"restarting I/O on adapter %s "
					"to free mempool\n",
					zfcp_get_busid_by_adapter(adapter));
			zfcp_erp_adapter_reopen_internal(adapter, 0, 66, 0);
		} else {
		retval = zfcp_erp_strategy_memwait(erp_action);
		}
		goto unlock;
	case ZFCP_ERP_CONTINUES:
		/* leave since this action runs asynchronously */
		if (erp_action->status & ZFCP_STATUS_ERP_LOWMEM) {
			--adapter->erp_low_mem_count;
			erp_action->status &= ~ZFCP_STATUS_ERP_LOWMEM;
		}
		goto unlock;
	}
	/* ok, finished action (whatever its result is) */

	/* check for unrecoverable targets */
	retval = zfcp_erp_strategy_check_target(erp_action, retval);

	/* action must be dequeued (here to allow for further ones) */
	zfcp_erp_action_dequeue(erp_action);

	/*
	 * put this target through the erp mill again if someone has
	 * requested to change the status of a target being online 
	 * to offline or the other way around
	 * (old retval is preserved if nothing has to be done here)
	 */
	retval = zfcp_erp_strategy_statechange(action, status, adapter,
					       port, unit, retval);

	/*
	 * leave if target is in permanent error state or if
	 * action is repeated in order to process state change
	 */
	if (retval == ZFCP_ERP_EXIT) {
		goto unlock;
	}

	/* trigger follow up actions */
	zfcp_erp_strategy_followup_actions(action, adapter, port, unit, retval);

 unlock:
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);
	
	if (retval != ZFCP_ERP_CONTINUES)
		zfcp_erp_action_cleanup(action, adapter, port, unit, retval);

	/*
	 * a few tasks remain when the erp queues are empty
	 * (don't do that if the last action evaluated was dismissed
	 * since this clearly indicates that there is more to come) :
	 * - close the name server port if it is open yet
	 *   (enqueues another [probably] final action)
	 * - otherwise, wake up whoever wants to be woken when we are
	 *   done with erp
	 */
	if (retval != ZFCP_ERP_DISMISSED)
		zfcp_erp_strategy_check_queues(adapter);

	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_DISMISSED	- if action has been dismissed
 *		retval			- otherwise
 */
static int
zfcp_erp_strategy_check_action(struct zfcp_erp_action *erp_action, int retval)
{
	zfcp_erp_strategy_check_fsfreq(erp_action);

	if (erp_action->status & ZFCP_STATUS_ERP_DISMISSED) {
		zfcp_erp_action_dequeue(erp_action);
		retval = ZFCP_ERP_DISMISSED;
	}

	return retval;
}

static int
zfcp_erp_strategy_do_action(struct zfcp_erp_action *erp_action)
{
	int retval = ZFCP_ERP_FAILED;

	/*
	 * try to execute/continue action as far as possible,
	 * note: no lock in subsequent strategy routines
	 * (this allows these routine to call schedule, e.g.
	 * kmalloc with such flags or qdio_initialize & friends)
	 * Note: in case of timeout, the seperate strategies will fail
	 * anyhow. No need for a special action. Even worse, a nameserver
	 * failure would not wake up waiting ports without the call.
	 */
	switch (erp_action->action) {

	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		retval = zfcp_erp_adapter_strategy(erp_action);
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
		retval = zfcp_erp_port_forced_strategy(erp_action);
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT:
		retval = zfcp_erp_port_strategy(erp_action);
		break;

	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		retval = zfcp_erp_unit_strategy(erp_action);
		break;

	default:
		ZFCP_LOG_NORMAL("bug: unknown erp action requested on "
				"adapter %s (action=%d)\n",
				zfcp_get_busid_by_adapter(erp_action->adapter),
				erp_action->action);
	}

	return retval;
}

/*
 * function:	
 *
 * purpose:	triggers retry of this action after a certain amount of time
 *		by means of timer provided by erp_action
 *
 * returns:	ZFCP_ERP_CONTINUES - erp_action sleeps in erp running queue
 */
static int
zfcp_erp_strategy_memwait(struct zfcp_erp_action *erp_action)
{
	int retval = ZFCP_ERP_CONTINUES;

	init_timer(&erp_action->timer);
	erp_action->timer.function = zfcp_erp_memwait_handler;
	erp_action->timer.data = (unsigned long) erp_action;
	erp_action->timer.expires = jiffies + ZFCP_ERP_MEMWAIT_TIMEOUT;
	add_timer(&erp_action->timer);

	return retval;
}

/* 
 * function:    zfcp_erp_adapter_failed
 *
 * purpose:     sets the adapter and all underlying devices to ERP_FAILED
 *
 */
void
zfcp_erp_adapter_failed(struct zfcp_adapter *adapter, u8 id, void *ref)
{
	zfcp_erp_modify_adapter_status(adapter, id, ref,
				       ZFCP_STATUS_COMMON_ERP_FAILED, ZFCP_SET);
	ZFCP_LOG_NORMAL("adapter erp failed on adapter %s\n",
			zfcp_get_busid_by_adapter(adapter));
}

/* 
 * function:    zfcp_erp_port_failed
 *
 * purpose:     sets the port and all underlying devices to ERP_FAILED
 *
 */
void
zfcp_erp_port_failed(struct zfcp_port *port, u8 id, void *ref)
{
	zfcp_erp_modify_port_status(port, id, ref,
				    ZFCP_STATUS_COMMON_ERP_FAILED, ZFCP_SET);

	if (atomic_test_mask(ZFCP_STATUS_PORT_WKA, &port->status))
		ZFCP_LOG_NORMAL("port erp failed (adapter %s, "
				"port d_id=0x%08x)\n",
				zfcp_get_busid_by_port(port), port->d_id);
	else
		ZFCP_LOG_NORMAL("port erp failed (adapter %s, wwpn=0x%016Lx)\n",
				zfcp_get_busid_by_port(port), port->wwpn);
}

/* 
 * function:    zfcp_erp_unit_failed
 *
 * purpose:     sets the unit to ERP_FAILED
 *
 */
void
zfcp_erp_unit_failed(struct zfcp_unit *unit, u8 id, void *ref)
{
	zfcp_erp_modify_unit_status(unit, id, ref,
				    ZFCP_STATUS_COMMON_ERP_FAILED, ZFCP_SET);

	ZFCP_LOG_NORMAL("unit erp failed on unit 0x%016Lx on port 0x%016Lx "
			" on adapter %s\n", unit->fcp_lun,
			unit->port->wwpn, zfcp_get_busid_by_unit(unit));
}

/*
 * function:	zfcp_erp_strategy_check_target
 *
 * purpose:	increments the erp action count on the device currently in
 *              recovery if the action failed or resets the count in case of
 *              success. If a maximum count is exceeded the device is marked
 *              as ERP_FAILED.
 *		The 'blocked' state of a target which has been recovered
 *              successfully is reset.
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (not considered)
 *		ZFCP_ERP_SUCCEEDED	- action finished successfully 
 *		ZFCP_ERP_EXIT		- action failed and will not continue
 */
static int
zfcp_erp_strategy_check_target(struct zfcp_erp_action *erp_action, int result)
{
	struct zfcp_adapter *adapter = erp_action->adapter;
	struct zfcp_port *port = erp_action->port;
	struct zfcp_unit *unit = erp_action->unit;

	switch (erp_action->action) {

	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		result = zfcp_erp_strategy_check_unit(unit, result);
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
	case ZFCP_ERP_ACTION_REOPEN_PORT:
		result = zfcp_erp_strategy_check_port(port, result);
		break;

	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		result = zfcp_erp_strategy_check_adapter(adapter, result);
		break;
	}

	return result;
}

static int
zfcp_erp_strategy_statechange(int action,
			      u32 status,
			      struct zfcp_adapter *adapter,
			      struct zfcp_port *port,
			      struct zfcp_unit *unit, int retval)
{
	switch (action) {

	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		if (zfcp_erp_strategy_statechange_detected(&adapter->status,
							   status)) {
			zfcp_erp_adapter_reopen_internal(adapter,
						ZFCP_STATUS_COMMON_ERP_FAILED,
						67, 0);
			retval = ZFCP_ERP_EXIT;
		}
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
	case ZFCP_ERP_ACTION_REOPEN_PORT:
		if (zfcp_erp_strategy_statechange_detected(&port->status,
							   status)) {
			zfcp_erp_port_reopen_internal(port,
						ZFCP_STATUS_COMMON_ERP_FAILED,
						68, 0);
			retval = ZFCP_ERP_EXIT;
		}
		break;

	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		if (zfcp_erp_strategy_statechange_detected(&unit->status,
							   status)) {
			zfcp_erp_unit_reopen_internal(unit,
						ZFCP_STATUS_COMMON_ERP_FAILED,
						69, 0);
			retval = ZFCP_ERP_EXIT;
		}
		break;
	}

	return retval;
}

static inline int
zfcp_erp_strategy_statechange_detected(atomic_t * target_status, u32 erp_status)
{
	return
	    /* take it online */
	    (atomic_test_mask(ZFCP_STATUS_COMMON_RUNNING, target_status) &&
	     (ZFCP_STATUS_ERP_CLOSE_ONLY & erp_status)) ||
	    /* take it offline */
	    (!atomic_test_mask(ZFCP_STATUS_COMMON_RUNNING, target_status) &&
	     !(ZFCP_STATUS_ERP_CLOSE_ONLY & erp_status));
}

static int
zfcp_erp_strategy_check_unit(struct zfcp_unit *unit, int result)
{
	switch (result) {
	case ZFCP_ERP_SUCCEEDED :
		atomic_set(&unit->erp_counter, 0);
		zfcp_erp_unit_unblock(unit);
		break;
	case ZFCP_ERP_FAILED :
		atomic_inc(&unit->erp_counter);
		if (atomic_read(&unit->erp_counter) > ZFCP_MAX_ERPS)
			zfcp_erp_unit_failed(unit, 21, 0);
		break;
	case ZFCP_ERP_EXIT :
		/* nothing */
		break;
	}

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &unit->status)) {
		zfcp_erp_unit_block(unit, 0); /* for ZFCP_ERP_SUCCEEDED */
		result = ZFCP_ERP_EXIT;
	}

	return result;
}

static int
zfcp_erp_strategy_check_port(struct zfcp_port *port, int result)
{
	switch (result) {
	case ZFCP_ERP_SUCCEEDED :
		atomic_set(&port->erp_counter, 0);
		zfcp_erp_port_unblock(port);
		break;
	case ZFCP_ERP_FAILED :
		atomic_inc(&port->erp_counter);
		if (atomic_read(&port->erp_counter) > ZFCP_MAX_ERPS)
			zfcp_erp_port_failed(port, 22, 0);
		break;
	case ZFCP_ERP_EXIT :
		/* nothing */
		break;
	}

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &port->status)) {
		zfcp_erp_port_block(port, 0); /* for ZFCP_ERP_SUCCEEDED */
		result = ZFCP_ERP_EXIT;
	}

	return result;
}

static int
zfcp_erp_strategy_check_adapter(struct zfcp_adapter *adapter, int result)
{
	switch (result) {
	case ZFCP_ERP_SUCCEEDED :
		atomic_set(&adapter->erp_counter, 0);
		zfcp_erp_adapter_unblock(adapter);
		break;
	case ZFCP_ERP_FAILED :
		atomic_inc(&adapter->erp_counter);
		if (atomic_read(&adapter->erp_counter) > ZFCP_MAX_ERPS)
			zfcp_erp_adapter_failed(adapter, 23, 0);
		break;
	case ZFCP_ERP_EXIT :
		/* nothing */
		break;
	}

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_FAILED, &adapter->status)) {
		zfcp_erp_adapter_block(adapter, 0); /* for ZFCP_ERP_SUCCEEDED */
		result = ZFCP_ERP_EXIT;
	}

	return result;
}

struct zfcp_erp_add_work {
	struct zfcp_unit  *unit;
	struct work_struct work;
};

/**
 * zfcp_erp_scsi_scan
 * @data: pointer to a struct zfcp_erp_add_work
 *
 * Registers a logical unit with the SCSI stack.
 */
static void zfcp_erp_scsi_scan(void *data)
{
	struct zfcp_erp_add_work *p = data;
	struct zfcp_unit *unit = p->unit;
	struct fc_rport *rport = unit->port->rport;
	scsi_scan_target(&rport->dev, 0, rport->scsi_target_id,
			 unit->scsi_lun, 0);
	atomic_clear_mask(ZFCP_STATUS_UNIT_SCSI_WORK_PENDING, &unit->status);
	wake_up(&unit->scsi_scan_wq);
	zfcp_unit_put(unit);
	kfree(p);
}

/**
 * zfcp_erp_schedule_work
 * @unit: pointer to unit which should be registered with SCSI stack
 *
 * Schedules work which registers a unit with the SCSI stack
 */
static void
zfcp_erp_schedule_work(struct zfcp_unit *unit)
{
	struct zfcp_erp_add_work *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		ZFCP_LOG_NORMAL("error: Out of resources. Could not register "
				"the FCP-LUN 0x%Lx connected to "
				"the port with WWPN 0x%Lx connected to "
				"the adapter %s with the SCSI stack.\n",
				unit->fcp_lun,
				unit->port->wwpn,
				zfcp_get_busid_by_unit(unit));
		return;
	}

	zfcp_unit_get(unit);
	atomic_set_mask(ZFCP_STATUS_UNIT_SCSI_WORK_PENDING, &unit->status);
	INIT_WORK(&p->work, zfcp_erp_scsi_scan, p);
	p->unit = unit;
	schedule_work(&p->work);
}

/*
 * function:	
 *
 * purpose:	remaining things in good cases,
 *		escalation in bad cases
 *
 * returns:
 */
static int
zfcp_erp_strategy_followup_actions(int action,
				   struct zfcp_adapter *adapter,
				   struct zfcp_port *port,
				   struct zfcp_unit *unit, int status)
{
	/* initiate follow-up actions depending on success of finished action */
	switch (action) {

	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		if (status == ZFCP_ERP_SUCCEEDED)
			zfcp_erp_port_reopen_all_internal(adapter, 0, 70, 0);
		else
			zfcp_erp_adapter_reopen_internal(adapter, 0, 71, 0);
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
		if (status == ZFCP_ERP_SUCCEEDED)
			zfcp_erp_port_reopen_internal(port, 0, 72, 0);
		else
			zfcp_erp_adapter_reopen_internal(adapter, 0, 73, 0);
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT:
		if (status == ZFCP_ERP_SUCCEEDED)
			zfcp_erp_unit_reopen_all_internal(port, 0, 74, 0);
		else
			zfcp_erp_port_forced_reopen_internal(port, 0, 75, 0);
		break;

	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		if (status == ZFCP_ERP_SUCCEEDED) ;	/* no further action */
		else
			zfcp_erp_port_reopen_internal(unit->port, 0, 76, 0);
		break;
	}

	return 0;
}

static int
zfcp_erp_strategy_check_queues(struct zfcp_adapter *adapter)
{
	unsigned long flags;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	read_lock(&adapter->erp_lock);
	if (list_empty(&adapter->erp_ready_head) &&
	    list_empty(&adapter->erp_running_head)) {
			atomic_clear_mask(ZFCP_STATUS_ADAPTER_ERP_PENDING,
					  &adapter->status);
			wake_up(&adapter->erp_done_wqh);
	}
	read_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	return 0;
}

/**
 * zfcp_erp_wait - wait for completion of error recovery on an adapter
 * @adapter: adapter for which to wait for completion of its error recovery
 * Return: 0
 */
int
zfcp_erp_wait(struct zfcp_adapter *adapter)
{
	int retval = 0;

	wait_event(adapter->erp_done_wqh,
		   !atomic_test_mask(ZFCP_STATUS_ADAPTER_ERP_PENDING,
				     &adapter->status));

	return retval;
}

void zfcp_erp_modify_adapter_status(struct zfcp_adapter *adapter, u8 id,
				    void *ref, u32 mask, int set_or_clear)
{
	struct zfcp_port *port;
	u32 changed, common_mask = mask & ZFCP_COMMON_FLAGS;

	if (set_or_clear == ZFCP_SET) {
		changed = atomic_test_and_set_mask(mask, &adapter->status);
	} else {
		changed = atomic_test_and_clear_mask(mask, &adapter->status);
		if (mask & ZFCP_STATUS_COMMON_ERP_FAILED)
			atomic_set(&adapter->erp_counter, 0);
	}
	if (changed)
		zfcp_rec_dbf_event_adapter(id, ref, adapter);

	/* Deal with all underlying devices, only pass common_mask */
	if (common_mask)
		list_for_each_entry(port, &adapter->port_list_head, list)
			zfcp_erp_modify_port_status(port, id, ref, common_mask,
						    set_or_clear);
}

/*
 * function:	zfcp_erp_modify_port_status
 *
 * purpose:	sets the port and all underlying devices to ERP_FAILED
 *
 */
void zfcp_erp_modify_port_status(struct zfcp_port *port, u8 id, void *ref,
				 u32 mask, int set_or_clear)
{
	struct zfcp_unit *unit;
	u32 changed, common_mask = mask & ZFCP_COMMON_FLAGS;

	if (set_or_clear == ZFCP_SET) {
		changed = atomic_test_and_set_mask(mask, &port->status);
	} else {
		changed = atomic_test_and_clear_mask(mask, &port->status);
		if (mask & ZFCP_STATUS_COMMON_ERP_FAILED)
			atomic_set(&port->erp_counter, 0);
	}
	if (changed)
		zfcp_rec_dbf_event_port(id, ref, port);

	/* Modify status of all underlying devices, only pass common mask */
	if (common_mask)
		list_for_each_entry(unit, &port->unit_list_head, list)
			zfcp_erp_modify_unit_status(unit, id, ref, common_mask,
						    set_or_clear);
}

/*
 * function:	zfcp_erp_modify_unit_status
 *
 * purpose:	sets the unit to ERP_FAILED
 *
 */
void zfcp_erp_modify_unit_status(struct zfcp_unit *unit, u8 id, void *ref,
				 u32 mask, int set_or_clear)
{
	u32 changed;

	if (set_or_clear == ZFCP_SET) {
		changed = atomic_test_and_set_mask(mask, &unit->status);
	} else {
		changed = atomic_test_and_clear_mask(mask, &unit->status);
		if (mask & ZFCP_STATUS_COMMON_ERP_FAILED) {
			atomic_set(&unit->erp_counter, 0);
		}
	}
	if (changed)
		zfcp_rec_dbf_event_unit(id, ref, unit);
}

/*
 * function:	
 *
 * purpose:	Wrappper for zfcp_erp_port_reopen_all_internal
 *              used to ensure the correct locking
 *
 * returns:	0	- initiated action successfully
 *		<0	- failed to initiate action
 */
int zfcp_erp_port_reopen_all(struct zfcp_adapter *adapter, int clear_mask,
			     u8 id, void *ref)
{
	int retval;
	unsigned long flags;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	write_lock(&adapter->erp_lock);
	retval = zfcp_erp_port_reopen_all_internal(adapter, clear_mask, id,
						   ref);
	write_unlock(&adapter->erp_lock);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);

	return retval;
}

static int zfcp_erp_port_reopen_all_internal(struct zfcp_adapter *adapter,
					     int clear_mask, u8 id, void *ref)
{
	int retval = 0;
	struct zfcp_port *port;

	list_for_each_entry(port, &adapter->port_list_head, list)
		if (!atomic_test_mask(ZFCP_STATUS_PORT_WKA, &port->status))
			zfcp_erp_port_reopen_internal(port, clear_mask, id,
						      ref);

	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	FIXME
 */
static int zfcp_erp_unit_reopen_all_internal(struct zfcp_port *port,
					     int clear_mask, u8 id, void *ref)
{
	int retval = 0;
	struct zfcp_unit *unit;

	list_for_each_entry(unit, &port->unit_list_head, list)
		zfcp_erp_unit_reopen_internal(unit, clear_mask, id, ref);

	return retval;
}

/*
 * function:	
 *
 * purpose:	this routine executes the 'Reopen Adapter' action
 *		(the entire action is processed synchronously, since
 *		there are no actions which might be run concurrently
 *		per definition)
 *
 * returns:	ZFCP_ERP_SUCCEEDED	- action finished successfully
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_adapter_strategy(struct zfcp_erp_action *erp_action)
{
	int retval;
	struct zfcp_adapter *adapter = erp_action->adapter;

	retval = zfcp_erp_adapter_strategy_close(erp_action);
	if (erp_action->status & ZFCP_STATUS_ERP_CLOSE_ONLY)
		retval = ZFCP_ERP_EXIT;
	else
		retval = zfcp_erp_adapter_strategy_open(erp_action);

	if (retval == ZFCP_ERP_FAILED) {
		ZFCP_LOG_INFO("Waiting to allow the adapter %s "
			      "to recover itself\n",
			      zfcp_get_busid_by_adapter(adapter));
		msleep(jiffies_to_msecs(ZFCP_TYPE2_RECOVERY_TIME));
	}

	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_SUCCEEDED      - action finished successfully
 *              ZFCP_ERP_FAILED         - action finished unsuccessfully
 */
static int
zfcp_erp_adapter_strategy_close(struct zfcp_erp_action *erp_action)
{
	int retval;

	atomic_set_mask(ZFCP_STATUS_COMMON_CLOSING,
			&erp_action->adapter->status);
	retval = zfcp_erp_adapter_strategy_generic(erp_action, 1);
	atomic_clear_mask(ZFCP_STATUS_COMMON_CLOSING,
			  &erp_action->adapter->status);

	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_SUCCEEDED      - action finished successfully
 *              ZFCP_ERP_FAILED         - action finished unsuccessfully
 */
static int
zfcp_erp_adapter_strategy_open(struct zfcp_erp_action *erp_action)
{
	int retval;

	atomic_set_mask(ZFCP_STATUS_COMMON_OPENING,
			&erp_action->adapter->status);
	retval = zfcp_erp_adapter_strategy_generic(erp_action, 0);
	atomic_clear_mask(ZFCP_STATUS_COMMON_OPENING,
			  &erp_action->adapter->status);

	return retval;
}

/*
 * function:    zfcp_register_adapter
 *
 * purpose:	allocate the irq associated with this devno and register
 *		the FSF adapter with the SCSI stack
 *
 * returns:	
 */
static int
zfcp_erp_adapter_strategy_generic(struct zfcp_erp_action *erp_action, int close)
{
	int retval = ZFCP_ERP_SUCCEEDED;

	if (close)
		goto close_only;

	retval = zfcp_erp_adapter_strategy_open_qdio(erp_action);
	if (retval != ZFCP_ERP_SUCCEEDED)
		goto failed_qdio;

	retval = zfcp_erp_adapter_strategy_open_fsf(erp_action);
	if (retval != ZFCP_ERP_SUCCEEDED)
		goto failed_openfcp;

	atomic_set_mask(ZFCP_STATUS_COMMON_OPEN, &erp_action->adapter->status);
	goto out;

 close_only:
	atomic_clear_mask(ZFCP_STATUS_COMMON_OPEN,
			  &erp_action->adapter->status);

 failed_openfcp:
	zfcp_close_fsf(erp_action->adapter);
 failed_qdio:
	atomic_clear_mask(ZFCP_STATUS_ADAPTER_XCONFIG_OK |
			  ZFCP_STATUS_ADAPTER_LINK_UNPLUGGED |
			  ZFCP_STATUS_ADAPTER_XPORT_OK,
			  &erp_action->adapter->status);
 out:
	return retval;
}

/*
 * function:    zfcp_qdio_init
 *
 * purpose:	setup QDIO operation for specified adapter
 *
 * returns:	0 - successful setup
 *		!0 - failed setup
 */
int
zfcp_erp_adapter_strategy_open_qdio(struct zfcp_erp_action *erp_action)
{
	int retval;
	int i;
	volatile struct qdio_buffer_element *sbale;
	struct zfcp_adapter *adapter = erp_action->adapter;

	if (atomic_test_mask(ZFCP_STATUS_ADAPTER_QDIOUP, &adapter->status)) {
		ZFCP_LOG_NORMAL("bug: second attempt to set up QDIO on "
				"adapter %s\n",
				zfcp_get_busid_by_adapter(adapter));
		goto failed_sanity;
	}

	if (qdio_establish(&adapter->qdio_init_data) != 0) {
		ZFCP_LOG_INFO("error: establishment of QDIO queues failed "
			      "on adapter %s\n",
			      zfcp_get_busid_by_adapter(adapter));
		goto failed_qdio_establish;
	}

	if (qdio_activate(adapter->ccw_device, 0) != 0) {
		ZFCP_LOG_INFO("error: activation of QDIO queues failed "
			      "on adapter %s\n",
			      zfcp_get_busid_by_adapter(adapter));
		goto failed_qdio_activate;
	}

	/*
	 * put buffers into response queue,
	 */
	for (i = 0; i < QDIO_MAX_BUFFERS_PER_Q; i++) {
		sbale = &(adapter->response_queue.buffer[i]->element[0]);
		sbale->length = 0;
		sbale->flags = SBAL_FLAGS_LAST_ENTRY;
		sbale->addr = 0;
	}

	ZFCP_LOG_TRACE("calling do_QDIO on adapter %s (flags=0x%x, "
		       "queue_no=%i, index_in_queue=%i, count=%i)\n",
		       zfcp_get_busid_by_adapter(adapter),
		       QDIO_FLAG_SYNC_INPUT, 0, 0, QDIO_MAX_BUFFERS_PER_Q);

	retval = do_QDIO(adapter->ccw_device,
			 QDIO_FLAG_SYNC_INPUT,
			 0, 0, QDIO_MAX_BUFFERS_PER_Q, NULL);

	if (retval) {
		ZFCP_LOG_NORMAL("bug: setup of QDIO failed (retval=%d)\n",
				retval);
		goto failed_do_qdio;
	} else {
		adapter->response_queue.free_index = 0;
		atomic_set(&adapter->response_queue.free_count, 0);
		ZFCP_LOG_DEBUG("%i buffers successfully enqueued to "
			       "response queue\n", QDIO_MAX_BUFFERS_PER_Q);
	}
	/* set index of first avalable SBALS / number of available SBALS */
	adapter->request_queue.free_index = 0;
	atomic_set(&adapter->request_queue.free_count, QDIO_MAX_BUFFERS_PER_Q);
	adapter->request_queue.distance_from_int = 0;

	/* initialize waitqueue used to wait for free SBALs in requests queue */
	init_waitqueue_head(&adapter->request_wq);

	/* ok, we did it - skip all cleanups for different failures */
	atomic_set_mask(ZFCP_STATUS_ADAPTER_QDIOUP, &adapter->status);
	retval = ZFCP_ERP_SUCCEEDED;
	goto out;

 failed_do_qdio:
	/* NOP */

 failed_qdio_activate:
	while (qdio_shutdown(adapter->ccw_device,
			     QDIO_FLAG_CLEANUP_USING_CLEAR) == -EINPROGRESS)
		msleep(1000);

 failed_qdio_establish:
 failed_sanity:
	retval = ZFCP_ERP_FAILED;

 out:
	return retval;
}


static int
zfcp_erp_adapter_strategy_open_fsf(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_erp_adapter_strategy_open_fsf_xconfig(erp_action);
	if (retval == ZFCP_ERP_FAILED)
		return ZFCP_ERP_FAILED;

	retval = zfcp_erp_adapter_strategy_open_fsf_xport(erp_action);
	if (retval == ZFCP_ERP_FAILED)
		return ZFCP_ERP_FAILED;

	return zfcp_erp_adapter_strategy_open_fsf_statusread(erp_action);
}

static int
zfcp_erp_adapter_strategy_open_fsf_xconfig(struct zfcp_erp_action *erp_action)
{
	int retval = ZFCP_ERP_SUCCEEDED;
	int retries;
	int sleep = ZFCP_EXCHANGE_CONFIG_DATA_FIRST_SLEEP;
	struct zfcp_adapter *adapter = erp_action->adapter;

	atomic_clear_mask(ZFCP_STATUS_ADAPTER_XCONFIG_OK, &adapter->status);

	for (retries = ZFCP_EXCHANGE_CONFIG_DATA_RETRIES; retries; retries--) {
		atomic_clear_mask(ZFCP_STATUS_ADAPTER_HOST_CON_INIT,
				  &adapter->status);
		ZFCP_LOG_DEBUG("Doing exchange config data\n");
		write_lock_irq(&adapter->erp_lock);
		zfcp_erp_action_to_running(erp_action);
		write_unlock_irq(&adapter->erp_lock);
		if (zfcp_fsf_exchange_config_data(erp_action)) {
			retval = ZFCP_ERP_FAILED;
			ZFCP_LOG_INFO("error:  initiation of exchange of "
				      "configuration data failed for "
				      "adapter %s\n",
				      zfcp_get_busid_by_adapter(adapter));
			break;
		}
		ZFCP_LOG_DEBUG("Xchange underway\n");

		/*
		 * Why this works:
		 * Both the normal completion handler as well as the timeout
		 * handler will do an 'up' when the 'exchange config data'
		 * request completes or times out. Thus, the signal to go on
		 * won't be lost utilizing this semaphore.
		 * Furthermore, this 'adapter_reopen' action is
		 * guaranteed to be the only action being there (highest action
		 * which prevents other actions from being created).
		 * Resulting from that, the wake signal recognized here
		 * _must_ be the one belonging to the 'exchange config
		 * data' request.
		 */
		zfcp_rec_dbf_event_thread(6, adapter, 1);
		down(&adapter->erp_ready_sem);
		zfcp_rec_dbf_event_thread(7, adapter, 1);
		if (erp_action->status & ZFCP_STATUS_ERP_TIMEDOUT) {
			ZFCP_LOG_INFO("error: exchange of configuration data "
				      "for adapter %s timed out\n",
				      zfcp_get_busid_by_adapter(adapter));
			break;
		}

		if (!atomic_test_mask(ZFCP_STATUS_ADAPTER_HOST_CON_INIT,
				     &adapter->status))
			break;

		ZFCP_LOG_DEBUG("host connection still initialising... "
			       "waiting and retrying...\n");
		/* sleep a little bit before retry */
		msleep(jiffies_to_msecs(sleep));
		sleep *= 2;
	}

	atomic_clear_mask(ZFCP_STATUS_ADAPTER_HOST_CON_INIT,
			  &adapter->status);

	if (!atomic_test_mask(ZFCP_STATUS_ADAPTER_XCONFIG_OK,
			      &adapter->status)) {
		ZFCP_LOG_INFO("error: exchange of configuration data for "
			      "adapter %s failed\n",
			      zfcp_get_busid_by_adapter(adapter));
		retval = ZFCP_ERP_FAILED;
	}

	return retval;
}

static int
zfcp_erp_adapter_strategy_open_fsf_xport(struct zfcp_erp_action *erp_action)
{
	int ret;
	struct zfcp_adapter *adapter;

	adapter = erp_action->adapter;
	atomic_clear_mask(ZFCP_STATUS_ADAPTER_XPORT_OK, &adapter->status);

	write_lock_irq(&adapter->erp_lock);
	zfcp_erp_action_to_running(erp_action);
	write_unlock_irq(&adapter->erp_lock);

	ret = zfcp_fsf_exchange_port_data(erp_action, adapter, NULL);
	if (ret == -EOPNOTSUPP) {
		return ZFCP_ERP_SUCCEEDED;
	} else if (ret) {
		return ZFCP_ERP_FAILED;
	}

	ret = ZFCP_ERP_SUCCEEDED;
	zfcp_rec_dbf_event_thread(8, adapter, 1);
	down(&adapter->erp_ready_sem);
	zfcp_rec_dbf_event_thread(9, adapter, 1);
	if (erp_action->status & ZFCP_STATUS_ERP_TIMEDOUT) {
		ZFCP_LOG_INFO("error: exchange port data timed out (adapter "
			      "%s)\n", zfcp_get_busid_by_adapter(adapter));
		ret = ZFCP_ERP_FAILED;
	}

	/* don't treat as error for the sake of compatibility */
	if (!atomic_test_mask(ZFCP_STATUS_ADAPTER_XPORT_OK, &adapter->status))
		ZFCP_LOG_INFO("warning: exchange port data failed (adapter "
			      "%s\n", zfcp_get_busid_by_adapter(adapter));

	return ret;
}

static int
zfcp_erp_adapter_strategy_open_fsf_statusread(struct zfcp_erp_action
					      *erp_action)
{
	struct zfcp_adapter *adapter = erp_action->adapter;

	atomic_set(&adapter->stat_miss, 16);
	return zfcp_status_read_refill(adapter);
}

/*
 * function:	
 *
 * purpose:	this routine executes the 'Reopen Physical Port' action
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_SUCCEEDED	- action finished successfully
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_port_forced_strategy(struct zfcp_erp_action *erp_action)
{
	int retval = ZFCP_ERP_FAILED;
	struct zfcp_port *port = erp_action->port;

	switch (erp_action->step) {

		/*
		 * FIXME:
		 * the ULP spec. begs for waiting for oustanding commands
		 */
	case ZFCP_ERP_STEP_UNINITIALIZED:
		zfcp_erp_port_strategy_clearstati(port);
		/*
		 * it would be sufficient to test only the normal open flag
		 * since the phys. open flag cannot be set if the normal
		 * open flag is unset - however, this is for readabilty ...
		 */
		if (atomic_test_mask((ZFCP_STATUS_PORT_PHYS_OPEN |
				      ZFCP_STATUS_COMMON_OPEN),
			             &port->status)) {
			ZFCP_LOG_DEBUG("port 0x%016Lx is open -> trying "
				       "close physical\n", port->wwpn);
			retval =
			    zfcp_erp_port_forced_strategy_close(erp_action);
		} else
			retval = ZFCP_ERP_FAILED;
		break;

	case ZFCP_ERP_STEP_PHYS_PORT_CLOSING:
		if (atomic_test_mask(ZFCP_STATUS_PORT_PHYS_OPEN,
				     &port->status)) {
			ZFCP_LOG_DEBUG("close physical failed for port "
				       "0x%016Lx\n", port->wwpn);
			retval = ZFCP_ERP_FAILED;
		} else
			retval = ZFCP_ERP_SUCCEEDED;
		break;
	}

	return retval;
}

/*
 * function:	
 *
 * purpose:	this routine executes the 'Reopen Port' action
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_SUCCEEDED	- action finished successfully
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_port_strategy(struct zfcp_erp_action *erp_action)
{
	int retval = ZFCP_ERP_FAILED;
	struct zfcp_port *port = erp_action->port;

	switch (erp_action->step) {

		/*
		 * FIXME:
		 * the ULP spec. begs for waiting for oustanding commands
		 */
	case ZFCP_ERP_STEP_UNINITIALIZED:
		zfcp_erp_port_strategy_clearstati(port);
		if (atomic_test_mask(ZFCP_STATUS_COMMON_OPEN, &port->status)) {
			ZFCP_LOG_DEBUG("port 0x%016Lx is open -> trying "
				       "close\n", port->wwpn);
			retval = zfcp_erp_port_strategy_close(erp_action);
			goto out;
		}		/* else it's already closed, open it */
		break;

	case ZFCP_ERP_STEP_PORT_CLOSING:
		if (atomic_test_mask(ZFCP_STATUS_COMMON_OPEN, &port->status)) {
			ZFCP_LOG_DEBUG("close failed for port 0x%016Lx\n",
				       port->wwpn);
			retval = ZFCP_ERP_FAILED;
			goto out;
		}		/* else it's closed now, open it */
		break;
	}
	if (erp_action->status & ZFCP_STATUS_ERP_CLOSE_ONLY)
		retval = ZFCP_ERP_EXIT;
	else
		retval = zfcp_erp_port_strategy_open(erp_action);

 out:
	return retval;
}

static int
zfcp_erp_port_strategy_open(struct zfcp_erp_action *erp_action)
{
	int retval;

	if (atomic_test_mask(ZFCP_STATUS_PORT_WKA,
			     &erp_action->port->status))
		retval = zfcp_erp_port_strategy_open_nameserver(erp_action);
	else
		retval = zfcp_erp_port_strategy_open_common(erp_action);

	return retval;
}

static int
zfcp_erp_port_strategy_open_common(struct zfcp_erp_action *erp_action)
{
	int retval = 0;
	struct zfcp_adapter *adapter = erp_action->adapter;
	struct zfcp_port *port = erp_action->port;

	switch (erp_action->step) {

	case ZFCP_ERP_STEP_UNINITIALIZED:
	case ZFCP_ERP_STEP_PHYS_PORT_CLOSING:
	case ZFCP_ERP_STEP_PORT_CLOSING:
		if (fc_host_port_type(adapter->scsi_host) == FC_PORTTYPE_PTP) {
			if (port->wwpn != adapter->peer_wwpn) {
				ZFCP_LOG_NORMAL("Failed to open port 0x%016Lx "
						"on adapter %s.\nPeer WWPN "
						"0x%016Lx does not match\n",
						port->wwpn,
						zfcp_get_busid_by_adapter(adapter),
						adapter->peer_wwpn);
				zfcp_erp_port_failed(port, 25, 0);
				retval = ZFCP_ERP_FAILED;
				break;
			}
			port->d_id = adapter->peer_d_id;
			atomic_set_mask(ZFCP_STATUS_PORT_DID_DID, &port->status);
			retval = zfcp_erp_port_strategy_open_port(erp_action);
			break;
		}
		if (!(adapter->nameserver_port)) {
			retval = zfcp_nameserver_enqueue(adapter);
			if (retval != 0) {
				ZFCP_LOG_NORMAL("error: nameserver port "
						"unavailable for adapter %s\n",
						zfcp_get_busid_by_adapter(adapter));
				retval = ZFCP_ERP_FAILED;
				break;
			}
		}
		if (!atomic_test_mask(ZFCP_STATUS_COMMON_UNBLOCKED,
				      &adapter->nameserver_port->status)) {
			ZFCP_LOG_DEBUG("nameserver port is not open -> open "
				       "nameserver port\n");
			/* nameserver port may live again */
			atomic_set_mask(ZFCP_STATUS_COMMON_RUNNING,
					&adapter->nameserver_port->status);
			if (zfcp_erp_port_reopen(adapter->nameserver_port, 0,
						 77, erp_action) >= 0) {
				erp_action->step =
					ZFCP_ERP_STEP_NAMESERVER_OPEN;
				retval = ZFCP_ERP_CONTINUES;
			} else
				retval = ZFCP_ERP_FAILED;
			break;
		}
		/* else nameserver port is already open, fall through */
	case ZFCP_ERP_STEP_NAMESERVER_OPEN:
		if (!atomic_test_mask(ZFCP_STATUS_COMMON_OPEN,
				      &adapter->nameserver_port->status)) {
			ZFCP_LOG_DEBUG("open failed for nameserver port\n");
			retval = ZFCP_ERP_FAILED;
		} else {
			ZFCP_LOG_DEBUG("nameserver port is open -> "
				       "nameserver look-up for port 0x%016Lx\n",
				       port->wwpn);
			retval = zfcp_erp_port_strategy_open_common_lookup
				(erp_action);
		}
		break;

	case ZFCP_ERP_STEP_NAMESERVER_LOOKUP:
		if (!atomic_test_mask(ZFCP_STATUS_PORT_DID_DID, &port->status)) {
			if (atomic_test_mask
			    (ZFCP_STATUS_PORT_INVALID_WWPN, &port->status)) {
				ZFCP_LOG_DEBUG("nameserver look-up failed "
					       "for port 0x%016Lx "
					       "(misconfigured WWPN?)\n",
					       port->wwpn);
				zfcp_erp_port_failed(port, 26, 0);
				retval = ZFCP_ERP_EXIT;
			} else {
				ZFCP_LOG_DEBUG("nameserver look-up failed for "
					       "port 0x%016Lx\n", port->wwpn);
				retval = ZFCP_ERP_FAILED;
			}
		} else {
			ZFCP_LOG_DEBUG("port 0x%016Lx has d_id=0x%08x -> "
				       "trying open\n", port->wwpn, port->d_id);
			retval = zfcp_erp_port_strategy_open_port(erp_action);
		}
		break;

	case ZFCP_ERP_STEP_PORT_OPENING:
		/* D_ID might have changed during open */
		if (atomic_test_mask((ZFCP_STATUS_COMMON_OPEN |
				      ZFCP_STATUS_PORT_DID_DID),
				     &port->status)) {
			ZFCP_LOG_DEBUG("port 0x%016Lx is open\n", port->wwpn);
			retval = ZFCP_ERP_SUCCEEDED;
		} else {
			ZFCP_LOG_DEBUG("open failed for port 0x%016Lx\n",
				       port->wwpn);
			retval = ZFCP_ERP_FAILED;
		}
		break;

	default:
		ZFCP_LOG_NORMAL("bug: unknown erp step 0x%08x\n",
				erp_action->step);
		retval = ZFCP_ERP_FAILED;
	}

	return retval;
}

static int
zfcp_erp_port_strategy_open_nameserver(struct zfcp_erp_action *erp_action)
{
	int retval;
	struct zfcp_port *port = erp_action->port;

	switch (erp_action->step) {

	case ZFCP_ERP_STEP_UNINITIALIZED:
	case ZFCP_ERP_STEP_PHYS_PORT_CLOSING:
	case ZFCP_ERP_STEP_PORT_CLOSING:
		ZFCP_LOG_DEBUG("port 0x%016Lx has d_id=0x%08x -> trying open\n",
			       port->wwpn, port->d_id);
		retval = zfcp_erp_port_strategy_open_port(erp_action);
		break;

	case ZFCP_ERP_STEP_PORT_OPENING:
		if (atomic_test_mask(ZFCP_STATUS_COMMON_OPEN, &port->status)) {
			ZFCP_LOG_DEBUG("WKA port is open\n");
			retval = ZFCP_ERP_SUCCEEDED;
		} else {
			ZFCP_LOG_DEBUG("open failed for WKA port\n");
			retval = ZFCP_ERP_FAILED;
		}
		/* this is needed anyway (dont care for retval of wakeup) */
		ZFCP_LOG_DEBUG("continue other open port operations\n");
		zfcp_erp_port_strategy_open_nameserver_wakeup(erp_action);
		break;

	default:
		ZFCP_LOG_NORMAL("bug: unknown erp step 0x%08x\n",
				erp_action->step);
		retval = ZFCP_ERP_FAILED;
	}

	return retval;
}

/*
 * function:	
 *
 * purpose:	makes the erp thread continue with reopen (physical) port
 *		actions which have been paused until the name server port
 *		is opened (or failed)
 *
 * returns:	0	(a kind of void retval, its not used)
 */
static int
zfcp_erp_port_strategy_open_nameserver_wakeup(struct zfcp_erp_action
					      *ns_erp_action)
{
	int retval = 0;
	unsigned long flags;
	struct zfcp_adapter *adapter = ns_erp_action->adapter;
	struct zfcp_erp_action *erp_action, *tmp;

	read_lock_irqsave(&adapter->erp_lock, flags);
	list_for_each_entry_safe(erp_action, tmp, &adapter->erp_running_head,
				 list) {
		if (erp_action->step == ZFCP_ERP_STEP_NAMESERVER_OPEN) {
			if (atomic_test_mask(
				    ZFCP_STATUS_COMMON_ERP_FAILED,
				    &adapter->nameserver_port->status))
				zfcp_erp_port_failed(erp_action->port, 27, 0);
			zfcp_erp_action_ready(erp_action);
		}
	}
	read_unlock_irqrestore(&adapter->erp_lock, flags);

	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_port_forced_strategy_close(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_fsf_close_physical_port(erp_action);
	if (retval == -ENOMEM) {
		retval = ZFCP_ERP_NOMEM;
		goto out;
	}
	erp_action->step = ZFCP_ERP_STEP_PHYS_PORT_CLOSING;
	if (retval != 0) {
		/* could not send 'open', fail */
		retval = ZFCP_ERP_FAILED;
		goto out;
	}
	retval = ZFCP_ERP_CONTINUES;
 out:
	return retval;
}

static int
zfcp_erp_port_strategy_clearstati(struct zfcp_port *port)
{
	int retval = 0;

	atomic_clear_mask(ZFCP_STATUS_COMMON_OPENING |
			  ZFCP_STATUS_COMMON_CLOSING |
			  ZFCP_STATUS_COMMON_ACCESS_DENIED |
			  ZFCP_STATUS_PORT_DID_DID |
			  ZFCP_STATUS_PORT_PHYS_CLOSING |
			  ZFCP_STATUS_PORT_INVALID_WWPN,
			  &port->status);
	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_port_strategy_close(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_fsf_close_port(erp_action);
	if (retval == -ENOMEM) {
		retval = ZFCP_ERP_NOMEM;
		goto out;
	}
	erp_action->step = ZFCP_ERP_STEP_PORT_CLOSING;
	if (retval != 0) {
		/* could not send 'close', fail */
		retval = ZFCP_ERP_FAILED;
		goto out;
	}
	retval = ZFCP_ERP_CONTINUES;
 out:
	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_port_strategy_open_port(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_fsf_open_port(erp_action);
	if (retval == -ENOMEM) {
		retval = ZFCP_ERP_NOMEM;
		goto out;
	}
	erp_action->step = ZFCP_ERP_STEP_PORT_OPENING;
	if (retval != 0) {
		/* could not send 'open', fail */
		retval = ZFCP_ERP_FAILED;
		goto out;
	}
	retval = ZFCP_ERP_CONTINUES;
 out:
	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_port_strategy_open_common_lookup(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_ns_gid_pn_request(erp_action);
	if (retval == -ENOMEM) {
		retval = ZFCP_ERP_NOMEM;
		goto out;
	}
	erp_action->step = ZFCP_ERP_STEP_NAMESERVER_LOOKUP;
	if (retval != 0) {
		/* could not send nameserver request, fail */
		retval = ZFCP_ERP_FAILED;
		goto out;
	}
	retval = ZFCP_ERP_CONTINUES;
 out:
	return retval;
}

/*
 * function:	
 *
 * purpose:	this routine executes the 'Reopen Unit' action
 *		currently no retries
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_SUCCEEDED	- action finished successfully
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_unit_strategy(struct zfcp_erp_action *erp_action)
{
	int retval = ZFCP_ERP_FAILED;
	struct zfcp_unit *unit = erp_action->unit;

	switch (erp_action->step) {

		/*
		 * FIXME:
		 * the ULP spec. begs for waiting for oustanding commands
		 */
	case ZFCP_ERP_STEP_UNINITIALIZED:
		zfcp_erp_unit_strategy_clearstati(unit);
		if (atomic_test_mask(ZFCP_STATUS_COMMON_OPEN, &unit->status)) {
			ZFCP_LOG_DEBUG("unit 0x%016Lx is open -> "
				       "trying close\n", unit->fcp_lun);
			retval = zfcp_erp_unit_strategy_close(erp_action);
			break;
		}
		/* else it's already closed, fall through */
	case ZFCP_ERP_STEP_UNIT_CLOSING:
		if (atomic_test_mask(ZFCP_STATUS_COMMON_OPEN, &unit->status)) {
			ZFCP_LOG_DEBUG("close failed for unit 0x%016Lx\n",
				       unit->fcp_lun);
			retval = ZFCP_ERP_FAILED;
		} else {
			if (erp_action->status & ZFCP_STATUS_ERP_CLOSE_ONLY)
				retval = ZFCP_ERP_EXIT;
			else {
				ZFCP_LOG_DEBUG("unit 0x%016Lx is not open -> "
					       "trying open\n", unit->fcp_lun);
				retval =
				    zfcp_erp_unit_strategy_open(erp_action);
			}
		}
		break;

	case ZFCP_ERP_STEP_UNIT_OPENING:
		if (atomic_test_mask(ZFCP_STATUS_COMMON_OPEN, &unit->status)) {
			ZFCP_LOG_DEBUG("unit 0x%016Lx is open\n",
				       unit->fcp_lun);
			retval = ZFCP_ERP_SUCCEEDED;
		} else {
			ZFCP_LOG_DEBUG("open failed for unit 0x%016Lx\n",
				       unit->fcp_lun);
			retval = ZFCP_ERP_FAILED;
		}
		break;
	}

	return retval;
}

static int
zfcp_erp_unit_strategy_clearstati(struct zfcp_unit *unit)
{
	int retval = 0;

	atomic_clear_mask(ZFCP_STATUS_COMMON_OPENING |
			  ZFCP_STATUS_COMMON_CLOSING |
			  ZFCP_STATUS_COMMON_ACCESS_DENIED |
			  ZFCP_STATUS_UNIT_SHARED |
			  ZFCP_STATUS_UNIT_READONLY,
			  &unit->status);

	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_unit_strategy_close(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_fsf_close_unit(erp_action);
	if (retval == -ENOMEM) {
		retval = ZFCP_ERP_NOMEM;
		goto out;
	}
	erp_action->step = ZFCP_ERP_STEP_UNIT_CLOSING;
	if (retval != 0) {
		/* could not send 'close', fail */
		retval = ZFCP_ERP_FAILED;
		goto out;
	}
	retval = ZFCP_ERP_CONTINUES;

 out:
	return retval;
}

/*
 * function:	
 *
 * purpose:	
 *
 * returns:	ZFCP_ERP_CONTINUES	- action continues (asynchronously)
 *		ZFCP_ERP_FAILED		- action finished unsuccessfully
 */
static int
zfcp_erp_unit_strategy_open(struct zfcp_erp_action *erp_action)
{
	int retval;

	retval = zfcp_fsf_open_unit(erp_action);
	if (retval == -ENOMEM) {
		retval = ZFCP_ERP_NOMEM;
		goto out;
	}
	erp_action->step = ZFCP_ERP_STEP_UNIT_OPENING;
	if (retval != 0) {
		/* could not send 'open', fail */
		retval = ZFCP_ERP_FAILED;
		goto out;
	}
	retval = ZFCP_ERP_CONTINUES;
 out:
	return retval;
}

void zfcp_erp_start_timer(struct zfcp_fsf_req *fsf_req)
{
	BUG_ON(!fsf_req->erp_action);
	fsf_req->timer.function = zfcp_erp_timeout_handler;
	fsf_req->timer.data = (unsigned long) fsf_req->erp_action;
	fsf_req->timer.expires = jiffies + ZFCP_ERP_FSFREQ_TIMEOUT;
	add_timer(&fsf_req->timer);
}

/*
 * function:	
 *
 * purpose:	enqueue the specified error recovery action, if needed
 *
 * returns:
 */
static int zfcp_erp_action_enqueue(int want, struct zfcp_adapter *adapter,
				   struct zfcp_port *port,
				   struct zfcp_unit *unit, u8 id, void *ref)
{
	int retval = 1, need = want;
	struct zfcp_erp_action *erp_action = NULL;
	u32 status = 0;

	/*
	 * We need some rules here which check whether we really need
	 * this action or whether we should just drop it.
	 * E.g. if there is a unfinished 'Reopen Port' request then we drop a
	 * 'Reopen Unit' request for an associated unit since we can't
	 * satisfy this request now. A 'Reopen Port' action will trigger
	 * 'Reopen Unit' actions when it completes.
	 * Thus, there are only actions in the queue which can immediately be
	 * executed. This makes the processing of the action queue more
	 * efficient.
	 */

	if (!atomic_test_mask(ZFCP_STATUS_ADAPTER_ERP_THREAD_UP,
			      &adapter->status))
		return -EIO;

	/* check whether we really need this */
	switch (want) {
	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		if (atomic_test_mask
		    (ZFCP_STATUS_COMMON_ERP_INUSE, &unit->status)) {
			goto out;
		}
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_RUNNING, &port->status) ||
		    atomic_test_mask
		    (ZFCP_STATUS_COMMON_ERP_FAILED, &port->status)) {
			goto out;
		}
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_UNBLOCKED, &port->status))
			need = ZFCP_ERP_ACTION_REOPEN_PORT;
		/* fall through !!! */

	case ZFCP_ERP_ACTION_REOPEN_PORT:
		if (atomic_test_mask
		    (ZFCP_STATUS_COMMON_ERP_INUSE, &port->status)) {
			goto out;
		}
		/* fall through !!! */

	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
		if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_INUSE,
				     &port->status)) {
			if (port->erp_action.action !=
			    ZFCP_ERP_ACTION_REOPEN_PORT_FORCED) {
				ZFCP_LOG_INFO("dropped erp action %i (port "
					      "0x%016Lx, action in use: %i)\n",
					      want, port->wwpn,
					      port->erp_action.action);
			}
			goto out;
		}
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_RUNNING, &adapter->status) ||
		    atomic_test_mask
		    (ZFCP_STATUS_COMMON_ERP_FAILED, &adapter->status)) {
			goto out;
		}
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_UNBLOCKED, &adapter->status))
			need = ZFCP_ERP_ACTION_REOPEN_ADAPTER;
		/* fall through !!! */

	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		if (atomic_test_mask
		    (ZFCP_STATUS_COMMON_ERP_INUSE, &adapter->status)) {
			goto out;
		}
		break;

	default:
		ZFCP_LOG_NORMAL("bug: unknown erp action requested "
				"on adapter %s (action=%d)\n",
				zfcp_get_busid_by_adapter(adapter), want);
		goto out;
	}

	/* check whether we need something stronger first */
	if (need) {
		ZFCP_LOG_DEBUG("stronger erp action %d needed before "
			       "erp action %d on adapter %s\n",
			       need, want, zfcp_get_busid_by_adapter(adapter));
	}

	/* mark adapter to have some error recovery pending */
	atomic_set_mask(ZFCP_STATUS_ADAPTER_ERP_PENDING, &adapter->status);

	/* setup error recovery action */
	switch (need) {

	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		zfcp_unit_get(unit);
		atomic_set_mask(ZFCP_STATUS_COMMON_ERP_INUSE, &unit->status);
		erp_action = &unit->erp_action;
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_RUNNING, &unit->status))
			status = ZFCP_STATUS_ERP_CLOSE_ONLY;
		break;

	case ZFCP_ERP_ACTION_REOPEN_PORT:
	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
		zfcp_port_get(port);
		zfcp_erp_action_dismiss_port(port);
		atomic_set_mask(ZFCP_STATUS_COMMON_ERP_INUSE, &port->status);
		erp_action = &port->erp_action;
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_RUNNING, &port->status))
			status = ZFCP_STATUS_ERP_CLOSE_ONLY;
		break;

	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		zfcp_adapter_get(adapter);
		zfcp_erp_action_dismiss_adapter(adapter);
		atomic_set_mask(ZFCP_STATUS_COMMON_ERP_INUSE, &adapter->status);
		erp_action = &adapter->erp_action;
		if (!atomic_test_mask
		    (ZFCP_STATUS_COMMON_RUNNING, &adapter->status))
			status = ZFCP_STATUS_ERP_CLOSE_ONLY;
		break;
	}

	memset(erp_action, 0, sizeof (struct zfcp_erp_action));
	erp_action->adapter = adapter;
	erp_action->port = port;
	erp_action->unit = unit;
	erp_action->action = need;
	erp_action->status = status;

	++adapter->erp_total_count;

	/* finally put it into 'ready' queue and kick erp thread */
	list_add_tail(&erp_action->list, &adapter->erp_ready_head);
	up(&adapter->erp_ready_sem);
	zfcp_rec_dbf_event_thread(1, adapter, 0);
	retval = 0;
 out:
	zfcp_rec_dbf_event_trigger(id, ref, want, need, erp_action,
				   adapter, port, unit);
	return retval;
}

static int
zfcp_erp_action_dequeue(struct zfcp_erp_action *erp_action)
{
	int retval = 0;
	struct zfcp_adapter *adapter = erp_action->adapter;

	--adapter->erp_total_count;
	if (erp_action->status & ZFCP_STATUS_ERP_LOWMEM) {
		--adapter->erp_low_mem_count;
		erp_action->status &= ~ZFCP_STATUS_ERP_LOWMEM;
	}

	list_del(&erp_action->list);
	zfcp_rec_dbf_event_action(144, erp_action);

	switch (erp_action->action) {
	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		atomic_clear_mask(ZFCP_STATUS_COMMON_ERP_INUSE,
				  &erp_action->unit->status);
		break;
	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
	case ZFCP_ERP_ACTION_REOPEN_PORT:
		atomic_clear_mask(ZFCP_STATUS_COMMON_ERP_INUSE,
				  &erp_action->port->status);
		break;
	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		atomic_clear_mask(ZFCP_STATUS_COMMON_ERP_INUSE,
				  &erp_action->adapter->status);
		break;
	default:
		/* bug */
		break;
	}
	return retval;
}

/**
 * zfcp_erp_action_cleanup
 *
 * Register unit with scsi stack if appropriate and fix reference counts.
 * Note: Temporary units are not registered with scsi stack.
 */
static void
zfcp_erp_action_cleanup(int action, struct zfcp_adapter *adapter,
			struct zfcp_port *port, struct zfcp_unit *unit,
			int result)
{
	switch (action) {
	case ZFCP_ERP_ACTION_REOPEN_UNIT:
		if ((result == ZFCP_ERP_SUCCEEDED)
		    && (!atomic_test_mask(ZFCP_STATUS_UNIT_TEMPORARY,
					  &unit->status))
		    && !unit->device
		    && port->rport) {
			atomic_set_mask(ZFCP_STATUS_UNIT_REGISTERED,
					&unit->status);
 			scsi_scan_target(&port->rport->dev, 0,
					 port->rport->scsi_target_id,
					 unit->scsi_lun, 0);
			if (atomic_test_mask(ZFCP_STATUS_UNIT_SCSI_WORK_PENDING,
					     &unit->status) == 0)
					     zfcp_erp_schedule_work(unit);
		}
		zfcp_unit_put(unit);
		break;
	case ZFCP_ERP_ACTION_REOPEN_PORT_FORCED:
	case ZFCP_ERP_ACTION_REOPEN_PORT:
		if (atomic_test_mask(ZFCP_STATUS_PORT_NO_WWPN,
				     &port->status)) {
			zfcp_port_put(port);
			break;
		}

		if ((result == ZFCP_ERP_SUCCEEDED)
		    && !port->rport) {
			struct fc_rport_identifiers ids;
			ids.node_name = port->wwnn;
			ids.port_name = port->wwpn;
			ids.port_id = port->d_id;
			ids.roles = FC_RPORT_ROLE_FCP_TARGET;
			port->rport =
				fc_remote_port_add(adapter->scsi_host, 0, &ids);
			if (!port->rport)
				ZFCP_LOG_NORMAL("failed registration of rport"
						"(adapter %s, wwpn=0x%016Lx)\n",
						zfcp_get_busid_by_port(port),
						port->wwpn);
			else {
				scsi_target_unblock(&port->rport->dev);
				port->rport->maxframe_size = port->maxframe_size;
				port->rport->supported_classes =
					port->supported_classes;
			}
		}
		if ((result != ZFCP_ERP_SUCCEEDED) && port->rport) {
			fc_remote_port_delete(port->rport);
			port->rport = NULL;
		}
		zfcp_port_put(port);
		break;
	case ZFCP_ERP_ACTION_REOPEN_ADAPTER:
		if (result != ZFCP_ERP_SUCCEEDED) {
			struct zfcp_port *port;
			unregister_service_level(&adapter->service_level);
			list_for_each_entry(port, &adapter->port_list_head, list)
				if (port->rport &&
				    !atomic_test_mask(ZFCP_STATUS_PORT_WKA,
						      &port->status)) {
					fc_remote_port_delete(port->rport);
					port->rport = NULL;
				}
		} else
			register_service_level(&adapter->service_level);
		zfcp_adapter_put(adapter);
		break;
	default:
		break;
	}
}


static void zfcp_erp_action_dismiss_adapter(struct zfcp_adapter *adapter)
{
	struct zfcp_port *port;

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_INUSE, &adapter->status))
		zfcp_erp_action_dismiss(&adapter->erp_action);
	else
		list_for_each_entry(port, &adapter->port_list_head, list)
		    zfcp_erp_action_dismiss_port(port);
}

static void zfcp_erp_action_dismiss_port(struct zfcp_port *port)
{
	struct zfcp_unit *unit;

	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_INUSE, &port->status))
		zfcp_erp_action_dismiss(&port->erp_action);
	else
		list_for_each_entry(unit, &port->unit_list_head, list)
		    zfcp_erp_action_dismiss_unit(unit);
}

static void zfcp_erp_action_dismiss_unit(struct zfcp_unit *unit)
{
	if (atomic_test_mask(ZFCP_STATUS_COMMON_ERP_INUSE, &unit->status))
		zfcp_erp_action_dismiss(&unit->erp_action);
}

static inline void
zfcp_erp_action_to_running(struct zfcp_erp_action *erp_action)
{
	list_move(&erp_action->list, &erp_action->adapter->erp_running_head);
	zfcp_rec_dbf_event_action(145, erp_action);
}

static inline void
zfcp_erp_action_to_ready(struct zfcp_erp_action *erp_action)
{
	list_move(&erp_action->list, &erp_action->adapter->erp_ready_head);
	zfcp_rec_dbf_event_action(146, erp_action);
}

void zfcp_erp_port_boxed(struct zfcp_port *port, u8 id, void *ref)
{
	unsigned long flags;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	zfcp_erp_modify_port_status(port, id, ref,
				    ZFCP_STATUS_COMMON_ACCESS_BOXED, ZFCP_SET);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);
	zfcp_erp_port_reopen(port, ZFCP_STATUS_COMMON_ERP_FAILED, id, ref);
}

void zfcp_erp_unit_boxed(struct zfcp_unit *unit, u8 id, void *ref)
{
	zfcp_erp_modify_unit_status(unit, id, ref,
				    ZFCP_STATUS_COMMON_ACCESS_BOXED, ZFCP_SET);
	zfcp_erp_unit_reopen(unit, ZFCP_STATUS_COMMON_ERP_FAILED, id, ref);
}

void zfcp_erp_port_access_denied(struct zfcp_port *port, u8 id, void *ref)
{
	unsigned long flags;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	zfcp_erp_modify_port_status(port, id, ref,
				    ZFCP_STATUS_COMMON_ERP_FAILED |
				    ZFCP_STATUS_COMMON_ACCESS_DENIED, ZFCP_SET);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);
}

void zfcp_erp_unit_access_denied(struct zfcp_unit *unit, u8 id, void *ref)
{
	zfcp_erp_modify_unit_status(unit, id, ref,
				    ZFCP_STATUS_COMMON_ERP_FAILED |
				    ZFCP_STATUS_COMMON_ACCESS_DENIED, ZFCP_SET);
}

void zfcp_erp_adapter_access_changed(struct zfcp_adapter *adapter, u8 id,
				     void *ref)
{
	struct zfcp_port *port;
	unsigned long flags;

	if (adapter->connection_features & FSF_FEATURE_NPIV_MODE)
		return;

	read_lock_irqsave(&zfcp_data.config_lock, flags);
	if (adapter->nameserver_port)
		zfcp_erp_port_access_changed(adapter->nameserver_port, id, ref);
	list_for_each_entry(port, &adapter->port_list_head, list)
		if (port != adapter->nameserver_port)
			zfcp_erp_port_access_changed(port, id, ref);
	read_unlock_irqrestore(&zfcp_data.config_lock, flags);
}

void zfcp_erp_port_access_changed(struct zfcp_port *port, u8 id, void *ref)
{
	struct zfcp_adapter *adapter = port->adapter;
	struct zfcp_unit *unit;

	if (!atomic_test_mask(ZFCP_STATUS_COMMON_ACCESS_DENIED,
			      &port->status) &&
	    !atomic_test_mask(ZFCP_STATUS_COMMON_ACCESS_BOXED,
			      &port->status)) {
		if (!atomic_test_mask(ZFCP_STATUS_PORT_WKA, &port->status))
			list_for_each_entry(unit, &port->unit_list_head, list)
				zfcp_erp_unit_access_changed(unit, id, ref);
		return;
	}

	ZFCP_LOG_NORMAL("reopen of port 0x%016Lx on adapter %s "
			"(due to ACT update)\n",
			port->wwpn, zfcp_get_busid_by_adapter(adapter));
	if (zfcp_erp_port_reopen(port, ZFCP_STATUS_COMMON_ERP_FAILED, id, ref))
		ZFCP_LOG_NORMAL("failed reopen of port"
				"(adapter %s, wwpn=0x%016Lx)\n",
				zfcp_get_busid_by_adapter(adapter), port->wwpn);
}

void zfcp_erp_unit_access_changed(struct zfcp_unit *unit, u8 id, void *ref)
{
	struct zfcp_adapter *adapter = unit->port->adapter;

	if (!atomic_test_mask(ZFCP_STATUS_COMMON_ACCESS_DENIED,
			      &unit->status) &&
	    !atomic_test_mask(ZFCP_STATUS_COMMON_ACCESS_BOXED,
			      &unit->status))
		return;

	ZFCP_LOG_NORMAL("reopen of unit 0x%016Lx on port 0x%016Lx "
			" on adapter %s (due to ACT update)\n",
			unit->fcp_lun, unit->port->wwpn,
			zfcp_get_busid_by_adapter(adapter));
	if (zfcp_erp_unit_reopen(unit, ZFCP_STATUS_COMMON_ERP_FAILED, id, ref))
		ZFCP_LOG_NORMAL("failed reopen of unit (adapter %s, "
				"wwpn=0x%016Lx, fcp_lun=0x%016Lx)\n",
				zfcp_get_busid_by_adapter(adapter),
				unit->port->wwpn, unit->fcp_lun);
}

#undef ZFCP_LOG_AREA
