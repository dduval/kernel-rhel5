#ifndef _ASM_POWERPC_HVCALL_H
#define _ASM_POWERPC_HVCALL_H
#ifdef __KERNEL__

#define HVSC			.long 0x44000022

#define H_SUCCESS	0
#define H_BUSY		1	/* Hardware busy -- retry later */
#define H_CLOSED	2	/* Resource closed */
#define H_NOT_AVAILABLE 3
#define H_CONSTRAINED	4	/* Resource request constrained to max allowed */
#define H_PARTIAL       5
#define H_IN_PROGRESS	14	/* Kind of like busy */
#define H_PAGE_REGISTERED 15
#define H_PARTIAL_STORE   16
#define H_PENDING	17	/* returned from H_POLL_PENDING */
#define H_CONTINUE	18	/* Returned from H_Join on success */
#define H_LONG_BUSY_START_RANGE		9900  /* Start of long busy range */
#define H_LONG_BUSY_ORDER_1_MSEC	9900  /* Long busy, hint that 1msec \
						 is a good time to retry */
#define H_LONG_BUSY_ORDER_10_MSEC	9901  /* Long busy, hint that 10msec \
						 is a good time to retry */
#define H_LONG_BUSY_ORDER_100_MSEC 	9902  /* Long busy, hint that 100msec \
						 is a good time to retry */
#define H_LONG_BUSY_ORDER_1_SEC		9903  /* Long busy, hint that 1sec \
						 is a good time to retry */
#define H_LONG_BUSY_ORDER_10_SEC	9904  /* Long busy, hint that 10sec \
						 is a good time to retry */
#define H_LONG_BUSY_ORDER_100_SEC	9905  /* Long busy, hint that 100sec \
						 is a good time to retry */
#define H_LONG_BUSY_END_RANGE		9905  /* End of long busy range */
#define H_HARDWARE	-1	/* Hardware error */
#define H_FUNCTION	-2	/* Function not supported */
#define H_PRIVILEGE	-3	/* Caller not privileged */
#define H_PARAMETER	-4	/* Parameter invalid, out-of-range or conflicting */
#define H_BAD_MODE	-5	/* Illegal msr value */
#define H_PTEG_FULL	-6	/* PTEG is full */
#define H_NOT_FOUND	-7	/* PTE was not found" */
#define H_RESERVED_DABR	-8	/* DABR address is reserved by the hypervisor on this processor" */
#define H_NO_MEM	-9
#define H_AUTHORITY	-10
#define H_PERMISSION	-11
#define H_DROPPED	-12
#define H_SOURCE_PARM	-13
#define H_DEST_PARM	-14
#define H_REMOTE_PARM	-15
#define H_RESOURCE	-16
#define H_ADAPTER_PARM  -17
#define H_RH_PARM       -18
#define H_RCQ_PARM      -19
#define H_SCQ_PARM      -20
#define H_EQ_PARM       -21
#define H_RT_PARM       -22
#define H_ST_PARM       -23
#define H_SIGT_PARM     -24
#define H_TOKEN_PARM    -25
#define H_MLENGTH_PARM  -27
#define H_MEM_PARM      -28
#define H_MEM_ACCESS_PARM -29
#define H_ATTR_PARM     -30
#define H_PORT_PARM     -31
#define H_MCG_PARM      -32
#define H_VL_PARM       -33
#define H_TSIZE_PARM    -34
#define H_TRACE_PARM    -35

#define H_MASK_PARM     -37
#define H_MCG_FULL      -38
#define H_ALIAS_EXIST   -39
#define H_P_COUNTER     -40
#define H_TABLE_FULL    -41
#define H_ALT_TABLE     -42
#define H_MR_CONDITION  -43
#define H_NOT_ENOUGH_RESOURCES -44
#define H_R_STATE       -45
#define H_RESCINDEND    -46


/* Long Busy is a condition that can be returned by the firmware
 * when a call cannot be completed now, but the identical call
 * should be retried later.  This prevents calls blocking in the
 * firmware for long periods of time.  Annoyingly the firmware can return
 * a range of return codes, hinting at how long we should wait before
 * retrying.  If you don't care for the hint, the macro below is a good
 * way to check for the long_busy return codes
 */
#define H_IS_LONG_BUSY(x)  ((x >= H_LONG_BUSY_START_RANGE) \
			     && (x <= H_LONG_BUSY_END_RANGE))

/* Flags */
#define H_LARGE_PAGE		(1UL<<(63-16))
#define H_EXACT			(1UL<<(63-24))	/* Use exact PTE or return H_PTEG_FULL */
#define H_R_XLATE		(1UL<<(63-25))	/* include a valid logical page num in the pte if the valid bit is set */
#define H_READ_4		(1UL<<(63-26))	/* Return 4 PTEs */
#define H_AVPN			(1UL<<(63-32))	/* An avpn is provided as a sanity test */
#define H_ANDCOND		(1UL<<(63-33))
#define H_ICACHE_INVALIDATE	(1UL<<(63-40))	/* icbi, etc.  (ignored for IO pages) */
#define H_ICACHE_SYNCHRONIZE	(1UL<<(63-41))	/* dcbst, icbi, etc (ignored for IO pages */
#define H_ZERO_PAGE		(1UL<<(63-48))	/* zero the page before mapping (ignored for IO pages) */
#define H_COPY_PAGE		(1UL<<(63-49))
#define H_N			(1UL<<(63-61))
#define H_PP1			(1UL<<(63-62))
#define H_PP2			(1UL<<(63-63))

/* VASI States */
#define H_VASI_INVALID          0
#define H_VASI_ENABLED          1
#define H_VASI_ABORTED          2
#define H_VASI_SUSPENDING       3
#define H_VASI_SUSPENDED        4
#define H_VASI_RESUMED          5
#define H_VASI_COMPLETED        6

/* DABRX flags */
#define H_DABRX_HYPERVISOR	(1UL<<(63-61))
#define H_DABRX_KERNEL		(1UL<<(63-62))
#define H_DABRX_USER		(1UL<<(63-63))

/* Each control block has to be on a 4K bondary */
#define H_CB_ALIGNMENT          4096

/* pSeries hypervisor opcodes */
#define H_REMOVE		0x04
#define H_ENTER			0x08
#define H_READ			0x0c
#define H_CLEAR_MOD		0x10
#define H_CLEAR_REF		0x14
#define H_PROTECT		0x18
#define H_GET_TCE		0x1c
#define H_PUT_TCE		0x20
#define H_SET_SPRG0		0x24
#define H_SET_DABR		0x28
#define H_PAGE_INIT		0x2c
#define H_SET_ASR		0x30
#define H_ASR_ON		0x34
#define H_ASR_OFF		0x38
#define H_LOGICAL_CI_LOAD	0x3c
#define H_LOGICAL_CI_STORE	0x40
#define H_LOGICAL_CACHE_LOAD	0x44
#define H_LOGICAL_CACHE_STORE	0x48
#define H_LOGICAL_ICBI		0x4c
#define H_LOGICAL_DCBF		0x50
#define H_GET_TERM_CHAR		0x54
#define H_PUT_TERM_CHAR		0x58
#define H_REAL_TO_LOGICAL	0x5c
#define H_HYPERVISOR_DATA	0x60
#define H_EOI			0x64
#define H_CPPR			0x68
#define H_IPI			0x6c
#define H_IPOLL			0x70
#define H_XIRR			0x74
#define H_PERFMON		0x7c
#define H_MIGRATE_DMA		0x78
#define H_REGISTER_VPA		0xDC
#define H_CEDE			0xE0
#define H_CONFER		0xE4
#define H_PROD			0xE8
#define H_GET_PPP		0xEC
#define H_SET_PPP		0xF0
#define H_PURR			0xF4
#define H_PIC			0xF8
#define H_REG_CRQ		0xFC
#define H_FREE_CRQ		0x100
#define H_VIO_SIGNAL		0x104
#define H_SEND_CRQ		0x108
#define H_COPY_RDMA		0x110
#define H_SET_XDABR		0x134
#define H_STUFF_TCE		0x138
#define H_PUT_TCE_INDIRECT	0x13C
#define H_VTERM_PARTNER_INFO	0x150
#define H_REGISTER_VTERM	0x154
#define H_FREE_VTERM		0x158
#define H_RESET_EVENTS          0x15C
#define H_ALLOC_RESOURCE        0x160
#define H_FREE_RESOURCE         0x164
#define H_MODIFY_QP             0x168
#define H_QUERY_QP              0x16C
#define H_REREGISTER_PMR        0x170
#define H_REGISTER_SMR          0x174
#define H_QUERY_MR              0x178
#define H_QUERY_MW              0x17C
#define H_QUERY_HCA             0x180
#define H_QUERY_PORT            0x184
#define H_MODIFY_PORT           0x188
#define H_DEFINE_AQP1           0x18C
#define H_GET_TRACE_BUFFER      0x190
#define H_DEFINE_AQP0           0x194
#define H_RESIZE_MR             0x198
#define H_ATTACH_MCQP           0x19C
#define H_DETACH_MCQP           0x1A0
#define H_CREATE_RPT            0x1A4
#define H_REMOVE_RPT            0x1A8
#define H_REGISTER_RPAGES       0x1AC
#define H_DISABLE_AND_GETC      0x1B0
#define H_ERROR_DATA            0x1B4
#define H_GET_HCA_INFO          0x1B8
#define H_GET_PERF_COUNT        0x1BC
#define H_MANAGE_TRACE          0x1C0
#define H_QUERY_INT_STATE       0x1E4
#define H_POLL_PENDING		0x1D8
#define H_JOIN			0x298
#define H_VASI_STATE            0x2A4
#define H_ENABLE_CRQ		0x2B0

#ifndef __ASSEMBLY__

/* plpar_hcall() -- Generic call interface using above opcodes
 *
 * The actual call interface is a hypervisor call instruction with
 * the opcode in R3 and input args in R4-R7.
 * Status is returned in R3 with variable output values in R4-R11.
 * Only H_PTE_READ with H_READ_4 uses R6-R11 so we ignore it for now
 * and return only two out args which MUST ALWAYS BE PROVIDED.
 */
long plpar_hcall(unsigned long opcode,
		 unsigned long arg1,
		 unsigned long arg2,
		 unsigned long arg3,
		 unsigned long arg4,
		 unsigned long *out1,
		 unsigned long *out2,
		 unsigned long *out3);

/* Same as plpar_hcall but for those opcodes that return no values
 * other than status.  Slightly more efficient.
 */
long plpar_hcall_norets(unsigned long opcode, ...);

/*
 * Special hcall interface for ibmveth support.
 * Takes 8 input parms. Returns a rc and stores the
 * R4 return value in *out1.
 */
long plpar_hcall_8arg_2ret(unsigned long opcode,
			   unsigned long arg1,
			   unsigned long arg2,
			   unsigned long arg3,
			   unsigned long arg4,
			   unsigned long arg5,
			   unsigned long arg6,
			   unsigned long arg7,
			   unsigned long arg8,
			   unsigned long *out1);

/* plpar_hcall_4out()
 *
 * same as plpar_hcall except with 4 output arguments.
 *
 */
long plpar_hcall_4out(unsigned long opcode,
		      unsigned long arg1,
		      unsigned long arg2,
		      unsigned long arg3,
		      unsigned long arg4,
		      unsigned long *out1,
		      unsigned long *out2,
		      unsigned long *out3,
		      unsigned long *out4);

long plpar_hcall_7arg_7ret(unsigned long opcode,
			   unsigned long arg1,
			   unsigned long arg2,
			   unsigned long arg3,
			   unsigned long arg4,
			   unsigned long arg5,
			   unsigned long arg6,
			   unsigned long arg7,
			   unsigned long *out1,
			   unsigned long *out2,
			   unsigned long *out3,
			   unsigned long *out4,
			   unsigned long *out5,
			   unsigned long *out6,
			   unsigned long *out7);

long plpar_hcall_9arg_9ret(unsigned long opcode,
			   unsigned long arg1,
			   unsigned long arg2,
			   unsigned long arg3,
			   unsigned long arg4,
			   unsigned long arg5,
			   unsigned long arg6,
			   unsigned long arg7,
			   unsigned long arg8,
			   unsigned long arg9,
			   unsigned long *out1,
			   unsigned long *out2,
			   unsigned long *out3,
			   unsigned long *out4,
			   unsigned long *out5,
			   unsigned long *out6,
			   unsigned long *out7,
			   unsigned long *out8,
			   unsigned long *out9);

#define PLPAR_HCALL9_BUFSIZE 9
inline static long plpar_hcall9(unsigned long opcode,
				unsigned long *retbuf,
				unsigned long arg1,
				unsigned long arg2,
				unsigned long arg3,
				unsigned long arg4,
				unsigned long arg5,
				unsigned long arg6,
				unsigned long arg7,
				unsigned long arg8,
				unsigned long arg9)
{
	return plpar_hcall_9arg_9ret(opcode,
				     arg1, arg2, arg3, arg4, arg5,
				     arg6, arg7, arg8, arg9,
				     retbuf,
				     retbuf+1, retbuf+2, retbuf+3, retbuf+4,
				     retbuf+5, retbuf+6, retbuf+7, retbuf+8);
}

#endif /* __ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* _ASM_POWERPC_HVCALL_H */
