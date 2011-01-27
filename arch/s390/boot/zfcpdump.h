/*
 * zfcpdump userspace tool
 *
 * Copyright IBM Corp. 2003, 2007.
 * Author(s): Michael Holzheu
 */

#ifndef _ZFCPDUMP_H
#define _ZFCPDUMP_H

#include <stdio.h>
#include <signal.h>
#include <stdint.h>

#define ZFCPDUMP_VERSION "2.0"

#define PRINT_TRACE(x...) \
	do { \
		if (g.parm_debug >= 3) { \
			fprintf(stderr, "TRACE: "); \
			fprintf(stderr, ##x); \
		} \
	} while (0)

#define PRINT_ERR(x...) \
	do { \
		fprintf(stderr, "ERROR: "); \
		fprintf(stderr, ##x); \
	} while (0)

#define PRINT_WARN(x...) \
	do { \
		fprintf(stderr, "WARNING: "); \
		fprintf(stderr, ##x); \
	} while (0)

#define PRINT_PERR(x...) \
	do { \
		fprintf(stderr, "ERROR: "); \
		fprintf(stderr, ##x); \
		perror(""); \
	} while (0)

#define PRINT(x...) fprintf(stdout, ##x)
#define CMDLINE_MAX_LEN 1024
#define KERN_PARM_MAX 100

#define DUMP_FLAGS (O_CREAT | O_RDWR | O_TRUNC)
#define DUMP_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

struct globals {
	char	*parm_compress;
	char	*parm_dir;
	char	*parm_part;
	int	parm_debug;
	int	parm_mode;
	__u64	parm_mem;
	char	parmline[CMDLINE_MAX_LEN];
	char	dump_dir[1024];
	int	dump_nr;
	int	last_progress;
	struct	sigaction sigact;
	char	dump_devno[16];
	char	dump_wwpn[32];
	char	dump_lun[32];
	char	dump_bootprog[32];
};

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#define PROC_CMDLINE	"/proc/cmdline"
#define PROC_MISC	"/proc/misc"
#define DEV_ZCORE	"/sys/kernel/debug/zcore/mem"
#define DEV_SCSI	"/dev/sda"
#define DUMP_DIR	"/mnt"

#define IPL_WWPN	"/sys/firmware/ipl/wwpn"
#define IPL_DEVNO	"/sys/firmware/ipl/device"
#define IPL_LUN		"/sys/firmware/ipl/lun"

#define PARM_DIR	"dump_dir"
#define PARM_DIR_DFLT	"/"

#define PARM_PART	"dump_part"
#define PARM_PART_DFLT	"1"

#define PARM_COMP	"dump_compress"
#define PARM_COMP_GZIP	"gzip"
#define PARM_COMP_NONE	"none"
#define PARM_COMP_DFLT	PARM_COMP_NONE

#define PARM_MEM	"dump_mem"
#ifdef __s390x__
#define PARM_MEM_DFLT	0xffffffffffffffff
#else
#define PARM_MEM_DFLT	0xffffffff
#endif

#define PARM_DEBUG	"dump_debug"
#define PARM_DEBUG_DFLT	2
#define PARM_DEBUG_MIN	1
#define PARM_DEBUG_MAX	6

#define PARM_MODE		"dump_mode"
#define PARM_MODE_INTERACT	"interactive"
#define PARM_MODE_INTERACT_NUM	0
#define PARM_MODE_AUTO		"auto"
#define PARM_MODE_AUTO_NUM	1
#define PARM_MODE_DFLT		PARM_MODE_INTERACT
#define PARM_MODE_NUM_DFLT	PARM_MODE_INTERACT_NUM

#define DUMP_FIRST	0
#define DUMP_LAST	1
#define NO_DUMP		-1

#define WAIT_TIME_ERASE		5 /* seconds */
#define WAIT_TIME_END		3 /* seconds */
#define WAIT_TIME_ONLINE	2 /* seconds */

#define UTS_LEN		65

#define DUMP_BUF_SIZE	(64 * 1024)

/* header definitions for dumps from s390 standalone dump tools */
#define DUMP_MAGIC_S390SA	0xa8190173618f23fdULL /* s390sa magic number */
#define DUMP_HEADER_SZ_S390SA	4096

/* standard header definitions */
#define DUMP_MAGIC_NUMBER	0xa8190173618f23edULL  /* dump magic number  */
#define DUMP_VERSION_NUMBER	0x8      /* dump version number             */
#define DUMP_PANIC_LEN		0x100    /* dump panic string length        */

/* dump compression options -- add as necessary */
#define DUMP_COMPRESS_NONE	0x0   /* don't compress this dump      */
#define DUMP_COMPRESS_GZIP	0x2   /* use GZIP compression          */

/* dump header flags -- add as necessary */
#define DUMP_DH_RAW		0x1   /* raw page (no compression)        */
#define DUMP_DH_COMPRESSED	0x2   /* page is compressed               */
#define DUMP_DH_END		0x4   /* end marker on a full dump        */

#define PAGE_SIZE		4096

/*
 * This is the header dumped at the top of every valid crash dump.
 */
struct dump_hdr_lkcd {
	__u64 magic_number;
	__u32 version;
	__u32 header_size;
	__u32 dump_level;
	__u32 page_size;
	__u64 memory_size;
	__u64 memory_start;
	__u64 memory_end;
	__u32 num_dump_pages;
	char panic_string[DUMP_PANIC_LEN];
	struct {
		__u64 tv_sec;
		__u64 tv_usec;
	} time;
	char utsname_sysname[UTS_LEN];
	char utsname_nodename[UTS_LEN];
	char utsname_release[UTS_LEN];
	char utsname_version[UTS_LEN];
	char utsname_machine[UTS_LEN];
	char utsname_domainname[UTS_LEN];
	__u64 current_task;
	__u32 dump_compress;
	__u32 dump_flags;
	__u32 dump_device;
} __attribute__((packed));

#define DH_ARCH_ID_S390X 2
#define DH_ARCH_ID_S390  1

/*
 * This is the header used by zcore
 */
struct dump_hdr_s390 {
	__u64 magic_number;
	__u32 version;
	__u32 header_size;
	__u32 dump_level;
	__u32 page_size;
	__u64 memory_size;
	__u64 memory_start;
	__u64 memory_end;
	__u32 num_pages;
	__u32 pad;
	__u64 tod;
	__u64 cpu_id;
	__u32 arch_id;
	__u32 build_arch_id;
} __attribute__((packed));

/*
 * Header associated to each physical page of memory saved in the system
 * crash dump.
 */
struct dump_page {
	__u64 address; /* the address of this dump page */
	__u32 size;    /* the size of this dump page */
	__u32 flags;   /* flags (DUMP_COMPRESSED, DUMP_RAW or DUMP_END) */
} __attribute__((packed));

/* Compression function */
typedef int (*compress_fn_t)(const unsigned char *old, __u32 old_size,
			     unsigned char *new, __u32 size);

#endif /* _ZFCPDUMP_H */