/*
 * Copyright (c) 2005-2009 Brocade Communications Systems, Inc.
 * All rights reserved
 * www.brocade.com
 *
 * Linux driver for Brocade Fibre Channel Host Bus Adapter.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License (GPL) Version 2 as
 * published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
/*
 *      Copyright (c) 2007-2009 Brocade Communications Systems, Inc.
 *      All rights reserved.
 *
 *      @file bfa_port.h Port common module header file.
 */

#ifndef __BFA_PORT_H__
#define __BFA_PORT_H__

#include <defs/bfa_defs_port.h>
#include <bfa_ioc.h>
#include <cs/bfa_trc.h>
#include <cs/bfa_log.h>

typedef void (*bfa_port_stats_cbfn_t) (void *dev, bfa_status_t status);
typedef void (*bfa_port_endis_cbfn_t) (void *dev, bfa_status_t status);

struct bfa_port_s {
	void				*dev;
	struct bfa_ioc_s		*ioc;
	struct bfa_trc_mod_s 		*trcmod;
	struct bfa_log_mod_s		*logmod;
	u32			msgtag;
	bfa_boolean_t			stats_busy;
	struct bfa_mbox_cmd_s 		stats_mb;
	bfa_port_stats_cbfn_t		stats_cbfn;
	void				*stats_cbarg;
	bfa_status_t			stats_status;
	union bfa_pport_stats_u		*stats;
	struct bfa_dma_s		stats_dma;
	bfa_boolean_t			endis_pending;
	struct bfa_mbox_cmd_s		endis_mb;
	bfa_port_endis_cbfn_t		endis_cbfn;
	void				*endis_cbarg;
	bfa_status_t			endis_status;
	struct bfa_ioc_hbfail_notify_s	hbfail;
};

void	     bfa_port_attach(struct bfa_port_s *port, struct bfa_ioc_s *ioc,
				void *dev, struct bfa_trc_mod_s *trcmod,
				struct bfa_log_mod_s *logmod);
void	     bfa_port_detach(struct bfa_port_s *port);
void	     bfa_port_hbfail(void *arg);

bfa_status_t bfa_port_get_stats(struct bfa_port_s *port,
				 union bfa_pport_stats_u *stats,
				 bfa_port_stats_cbfn_t cbfn, void *cbarg);
bfa_status_t bfa_port_clear_stats(struct bfa_port_s *port,
				   bfa_port_stats_cbfn_t cbfn, void *cbarg);
bfa_status_t bfa_port_enable(struct bfa_port_s *port,
			      bfa_port_endis_cbfn_t cbfn, void *cbarg);
bfa_status_t bfa_port_disable(struct bfa_port_s *port,
			       bfa_port_endis_cbfn_t cbfn, void *cbarg);
u32     bfa_port_meminfo(void);
void	     bfa_port_mem_claim(struct bfa_port_s *port,
				 u8 *dma_kva, u64 dma_pa);

#endif	/* __BFA_PORT_H__ */
