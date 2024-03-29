/*
 * arch/s390/kernel/head64.S
 *
 * Copyright (C) IBM Corp. 1999,2006
 *
 *   Author(s):	Hartmut Penner <hp@de.ibm.com>
 *		Martin Schwidefsky <schwidefsky@de.ibm.com>
 *		Rob van der Heij <rvdhei@iae.nl>
 *		Heiko Carstens <heiko.carstens@de.ibm.com>
 *
 */

#
# startup-code at 0x10000, running in absolute addressing mode
# this is called either by the ipl loader or directly by PSW restart
# or linload or SALIPL
#
	.org  0x10000
startup:basr  %r13,0			 # get base
.LPG0:	l     %r13,0f-.LPG0(%r13)
	b     0(%r13)
0:	.long startup_continue

#
# params at 10400 (setup.h)
#
	.org   PARMAREA
	.quad  0			# IPL_DEVICE
	.quad  RAMDISK_ORIGIN		# INITRD_START
	.quad  RAMDISK_SIZE		# INITRD_SIZE

	.org   COMMAND_LINE
	.byte  "root=/dev/ram0 ro"
	.byte  0

	.org   0x11000

startup_continue:
	basr  %r13,0			 # get base
.LPG1:  sll   %r13,1                     # remove high order bit
        srl   %r13,1

#ifdef CONFIG_ZFCPDUMP

	# check if we have been ipled using zfcp dump:

	tm	0xb9,0x01		# test if subchannel is enabled
	jno	.nodump			# subchannel disabled
	l	%r1,0xb8
	la	%r5,ipl_schib-.LPG1(%r13)
	stsch	0(%r5)			# get schib of subchannel
	jne	.nodump			# schib not available
	tm	5(%r5),0x01		# devno valid?
	jno	.nodump
	tm	4(%r5),0x80		# qdio capable device?
	jno	.nodump
	l	%r2,20(%r0)		# address of ipl parameter block
	lhi	%r3,0
	ic	%r3,0x148(%r2)		# get opt field
	chi	%r3,0x20		# load with dump?
	jne	.nodump

	# store all prefix registers in case of load with dump:

	la	%r7,0			# base register for 0 page
	la	%r8,0			# first cpu
	l	%r11,.Lpref_arr_ptr-.LPG1(%r13)	# address of prefix array
	ahi	%r11,4			# skip boot cpu
	lr	%r12,%r11
	ahi	%r12,(CONFIG_NR_CPUS*4)	# end of prefix array
	stap	.Lcurrent_cpu+2-.LPG1(%r13)	# store current cpu addr
1:
	cl	%r8,.Lcurrent_cpu-.LPG1(%r13)	# is ipl cpu ?
	je	4f				# if yes get next cpu
2:
	lr	%r9,%r7
	sigp	%r9,%r8,0x9		# stop & store status of cpu
	brc	8,3f			# accepted
	brc	4,4f			# status stored: next cpu
	brc	2,2b			# busy: 	 try again
	brc	1,4f			# not op:	 next cpu
3:
	mvc	0(4,%r11),264(%r7)	# copy prefix register to prefix array
	ahi	%r11,4			# next element in prefix array
	clr	%r11,%r12
	je	5f			# no more space in prefix array
4:
	ahi	%r8,1				# next cpu (r8 += 1)
	cl	%r8,.Llast_cpu-.LPG1(%r13)	# is last possible cpu ?
	jl	1b				# jump if not last cpu
5:
	lhi	%r1,2			# mode 2 = esame (dump)
	j	6f
.nodump:
	lhi	%r1,1			# mode 1 = esame (normal ipl)
6:
#else
	lhi	%r1,1			# mode 1 = esame (normal ipl)
#endif /* CONFIG_ZFCPDUMP */
	mvi	__LC_AR_MODE_ID,1	# set esame flag

        lhi   %r1,1                      # mode 1 = esame
	mvi   __LC_AR_MODE_ID,1		 # set esame flag
        slr   %r0,%r0                    # set cpuid to zero
        sigp  %r1,%r0,0x12               # switch to esame mode
	sam64				 # switch to 64 bit mode
	llgfr	%r13,%r13		 # clear high-order half of base reg
	lmh	%r0,%r15,.Lzero64-.LPG1(%r13) # clear high-order half
	lctlg %c0,%c15,.Lctl-.LPG1(%r13) # load control registers
	lg    %r12,.Lparmaddr-.LPG1(%r13)# pointer to parameter area
					 # move IPL device to lowcore
        mvc   __LC_IPLDEV(4),IPL_DEVICE+4-PARMAREA(%r12)

#
# Setup stack
#
	larl	%r15,init_thread_union
	lg	%r14,__TI_task(%r15)	# cache current in lowcore
	stg	%r14,__LC_CURRENT
	aghi	%r15,1<<(PAGE_SHIFT+THREAD_ORDER) # init_task_union + THREAD_SIZE
	stg	%r15,__LC_KERNEL_STACK    # set end of kernel stack
	aghi	%r15,-160
	xc	__SF_BACKCHAIN(4,%r15),__SF_BACKCHAIN(%r15) # clear backchain
#
# Save ipl parameters, clear bss memory, initialize storage key for kernel pages,
# and create a kernel NSS if the SAVESYS= parm is defined
#
	brasl	%r14,startup_init

	l     %r2,.Lrcp-.LPG1(%r13)	# Read SCP forced command word
.Lservicecall:
	stosm .Lpmask-.LPG1(%r13),0x01	# authorize ext interrupts

	stctg %r0,%r0,.Lcr-.LPG1(%r13)	# get cr0
	la    %r1,0x200			# set bit 22
	og    %r1,.Lcr-.LPG1(%r13)	# or old cr0 with r1
	stg   %r1,.Lcr-.LPG1(%r13)
	lctlg %r0,%r0,.Lcr-.LPG1(%r13)	# load modified cr0

	mvc   __LC_EXT_NEW_PSW(8),.Lpcmsk-.LPG1(%r13) # set postcall psw
	larl  %r1,.Lsclph
	stg   %r1,__LC_EXT_NEW_PSW+8	# set handler

	larl  %r4,.Lsccb		# %r4 is our index for sccb stuff
	lgr   %r1,%r4			# our sccb
	.insn rre,0xb2200000,%r2,%r1	# service call
	ipm   %r1
	srl   %r1,28			# get cc code
	xr    %r3,%r3
	chi   %r1,3
	be    .Lfchunk-.LPG1(%r13)	# leave
	chi   %r1,2
	be    .Lservicecall-.LPG1(%r13)
	lpswe .Lwaitsclp-.LPG1(%r13)
.Lsclph:
	lh    %r1,.Lsccbr-.Lsccb(%r4)
	chi   %r1,0x10			# 0x0010 is the sucess code
	je    .Lprocsccb		# let's process the sccb
	chi   %r1,0x1f0
	bne   .Lfchunk-.LPG1(%r13)	# unhandled error code
	c     %r2,.Lrcp-.LPG1(%r13)	# Did we try Read SCP forced
	bne   .Lfchunk-.LPG1(%r13)	# if no, give up
	l     %r2,.Lrcp2-.LPG1(%r13)	# try with Read SCP
	b     .Lservicecall-.LPG1(%r13)
.Lprocsccb:
	lghi  %r1,0
	icm   %r1,3,.Lscpincr1-.Lsccb(%r4) # use this one if != 0
	jnz   .Lscnd
	lg    %r1,.Lscpincr2-.Lsccb(%r4) # otherwise use this one
.Lscnd:
	xr    %r3,%r3			# same logic
	ic    %r3,.Lscpa1-.Lsccb(%r4)
	chi   %r3,0x00
	jne   .Lcompmem
	l     %r3,.Lscpa2-.Lsccb(%r4)
.Lcompmem:
	mlgr  %r2,%r1			# mem in MB on 128-bit
	l     %r1,.Lonemb-.LPG1(%r13)
	mlgr  %r2,%r1			# mem size in bytes in %r3
	b     .Lfchunk-.LPG1(%r13)

	.align 4
.Lpmask:
	.byte 0
	.align 8
.Lcr:
	.quad 0x00  # place holder for cr0
.Lwaitsclp:
	.quad  0x0102000180000000,.Lsclph
.Lrcp:
	.int 0x00120001 # Read SCP forced code
.Lrcp2:
	.int 0x00020001 # Read SCP code
.Lonemb:
	.int 0x100000

.Lfchunk:
					 # set program check new psw mask
	mvc   __LC_PGM_NEW_PSW(8),.Lpcmsk-.LPG1(%r13)

#
# find memory chunks.
#
	lgr   %r9,%r3			 # end of mem
	larl  %r1,.Lchkmem               # set program check address
	stg   %r1,__LC_PGM_NEW_PSW+8
	la    %r1,1                      # test in increments of 128KB
	sllg  %r1,%r1,17
	larl  %r3,memory_chunk
	slgr  %r4,%r4                    # set start of chunk to zero
	slgr  %r5,%r5                    # set end of chunk to zero
	slr  %r6,%r6			 # set access code to zero
	la    %r10,MEMORY_CHUNKS	 # number of chunks
.Lloop:
	tprot 0(%r5),0			 # test protection of first byte
	ipm   %r7
	srl   %r7,28
	clr   %r6,%r7			 # compare cc with last access code
	je    .Lsame
	lghi  %r8,0			 # no program checks
	j     .Lsavchk
.Lsame:
	algr  %r5,%r1			 # add 128KB to end of chunk
					 # no need to check here,
	brc   12,.Lloop			 # this is the same chunk
.Lchkmem:				 # > 16EB or tprot got a program check
	lghi  %r8,1			 # set program check flag
.Lsavchk:
	clgr  %r4,%r5			 # chunk size > 0?
	je    .Lchkloop
	stg   %r4,0(%r3)		 # store start address of chunk
	lgr   %r0,%r5
	slgr  %r0,%r4
	stg   %r0,8(%r3)		 # store size of chunk
	st    %r6,20(%r3)		 # store type of chunk
	la    %r3,24(%r3)
	ahi   %r10,-1			 # update chunk number
.Lchkloop:
	lr    %r6,%r7			 # set access code to last cc
	# we got an exception or we're starting a new
	# chunk , we must check if we should
	# still try to find valid memory (if we detected
	# the amount of available storage), and if we
	# have chunks left
	lghi  %r4,1
	sllg  %r4,%r4,31
	clgr  %r5,%r4
	je    .Lhsaskip
	xr    %r0, %r0
	clgr  %r0, %r9			 # did we detect memory?
	je    .Ldonemem			 # if not, leave
	chi   %r10, 0			 # do we have chunks left?
	je    .Ldonemem
.Lhsaskip:
	chi   %r8,1			 # program check ?
	je    .Lpgmchk
	lgr   %r4,%r5			 # potential new chunk
	algr  %r5,%r1			 # add 128KB to end of chunk
	j     .Llpcnt
.Lpgmchk:
	algr  %r5,%r1			 # add 128KB to end of chunk
	lgr   %r4,%r5			 # potential new chunk
.Llpcnt:
	clgr  %r5,%r9			 # should we go on?
	jl    .Lloop
.Ldonemem:		

	larl  %r12,machine_flags
#
# find out if we have the MVPG instruction
#
	la     %r1,0f-.LPG1(%r13)       # set program check address
	stg    %r1,__LC_PGM_NEW_PSW+8
	sgr    %r0,%r0
	lghi   %r1,0
	lghi   %r2,0
	mvpg   %r1,%r2                  # test MVPG instruction
	oi     7(%r12),16               # set MVPG flag
0:

#
# find out if the diag 0x44 works in 64 bit mode
#
	la     %r1,0f-.LPG1(%r13)	# set program check address
	stg    %r1,__LC_PGM_NEW_PSW+8
	diag   0,0,0x44			# test diag 0x44
	oi     7(%r12),32		# set diag44 flag
0:	

#
# find out if we have the IDTE instruction
#
	la     %r1,0f-.LPG1(%r13)	# set program check address
	stg    %r1,__LC_PGM_NEW_PSW+8
	.long	0xb2b10000		# store facility list
	tm	0xc8,0x08		# check bit for clearing-by-ASCE
	bno	0f-.LPG1(%r13)
	lhi	%r1,2094
	lhi	%r2,0
	.long	0xb98e2001
	oi	7(%r12),0x80		# set IDTE flag
0:

#
# find out if the diag 0x9c is available
#
	la     %r1,0f-.LPG1(%r13)	# set program check address
	stg    %r1,__LC_PGM_NEW_PSW+8
	stap   __LC_CPUID+4		# store cpu address
	lh     %r1,__LC_CPUID+4
	diag   %r1,0,0x9c		# test diag 0x9c
	oi     6(%r12),1		# set diag9c flag
0:

	la	%r1,0f-.LPG1(%r13)
	stg	%r1,__LC_PGM_NEW_PSW+8
	lhi	%r1,-1
	.short	0xb9af
	.short	0x0011
0:	tm	0x8f,0x6		# specification exception?
	bno	1f-.LPG1(%r13)
	oi	6(%r12),8
1:

        lpswe .Lentry-.LPG1(13)         # jump to _stext in primary-space,
                                        # virtual and never return ...
        .align 16
.Lentry:.quad  0x0000000180000000,_stext
.Lctl:  .quad  0x04b50002               # cr0: various things
        .quad  0                        # cr1: primary space segment table
        .quad  .Lduct                   # cr2: dispatchable unit control table
        .quad  0                        # cr3: instruction authorization
        .quad  0                        # cr4: instruction authorization
        .quad  0xffffffffffffffff       # cr5: primary-aste origin
        .quad  0                        # cr6:  I/O interrupts
        .quad  0                        # cr7:  secondary space segment table
        .quad  0                        # cr8:  access registers translation
        .quad  0                        # cr9:  tracing off
        .quad  0                        # cr10: tracing off
        .quad  0                        # cr11: tracing off
        .quad  0                        # cr12: tracing off
        .quad  0                        # cr13: home space segment table
        .quad  0xc0000000               # cr14: machine check handling off
        .quad  0                        # cr15: linkage stack operations
.Lduct: .long 0,0,0,0,0,0,0,0
	.long 0,0,0,0,0,0,0,0
.Lpcmsk:.quad  0x0000000180000000
.L4malign:.quad 0xffffffffffc00000
.Lscan2g:.quad 0x80000000 + 0x20000 - 8 # 2GB + 128K - 8
.Lnop:	.long  0x07000700
.Lzero64:.fill	16,4,0x0
#ifdef CONFIG_ZFCPDUMP
.Lcurrent_cpu:
	.long 0x0
.Llast_cpu:
	.long 0x0000ffff
.Lpref_arr_ptr:
	.long zfcpdump_prefix_array
#endif /* CONFIG_ZFCPDUMP */
.Lparmaddr:
	.quad	PARMAREA

	.globl	ipl_schib
ipl_schib:
	.rept	13
	.long	0
	.endr

	.org	0x12000
.globl s390_readinfo_sccb
s390_readinfo_sccb:
.Lsccb:
	.hword 0x1000			# length, one page
	.byte 0x00,0x00,0x00
	.byte 0x80			# variable response bit set
.Lsccbr:
	.hword 0x00			# response code
.Lscpincr1:
	.hword 0x00
.Lscpa1:
	.byte 0x00
	.fill 89,1,0
.Lscpa2:
	.int 0x00
.Lscpincr2:
	.quad 0x00
	.fill 3984,1,0
	.org	0x13000

#ifdef CONFIG_SHARED_KERNEL
	.org   0x100000
#endif
	
#
# startup-code, running in absolute addressing mode
#
        .globl _stext
_stext:	basr  %r13,0                    # get base
.LPG3:
# check control registers
        stctg  %c0,%c15,0(%r15)
	oi     6(%r15),0x40             # enable sigp emergency signal
	oi     4(%r15),0x10             # switch on low address proctection
        lctlg  %c0,%c15,0(%r15)

        lam    0,15,.Laregs-.LPG3(%r13) # load access regs needed by uaccess
        brasl  %r14,start_kernel        # go to C code
#
# We returned from start_kernel ?!? PANIK
#
        basr  %r13,0
	lpswe .Ldw-.(%r13)           # load disabled wait psw

            .align 8
.Ldw:       .quad  0x0002000180000000,0x0000000000000000
.Laregs:    .long  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
