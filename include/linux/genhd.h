#ifndef _LINUX_GENHD_H
#define _LINUX_GENHD_H

/*
 * 	genhd.h Copyright (C) 1992 Drew Eckhardt
 *	Generic hard disk header file by  
 * 		Drew Eckhardt
 *
 *		<drew@colorado.edu>
 */

#include <linux/types.h>

enum {
/* These three have identical behaviour; use the second one if DOS FDISK gets
   confused about extended/logical partitions starting past cylinder 1023. */
	DOS_EXTENDED_PARTITION = 5,
	LINUX_EXTENDED_PARTITION = 0x85,
	WIN98_EXTENDED_PARTITION = 0x0f,

	LINUX_SWAP_PARTITION = 0x82,
	LINUX_RAID_PARTITION = 0xfd,	/* autodetect RAID partition */

	SOLARIS_X86_PARTITION =	LINUX_SWAP_PARTITION,
	NEW_SOLARIS_X86_PARTITION = 0xbf,

	DM6_AUX1PARTITION = 0x51,	/* no DDO:  use xlated geom */
	DM6_AUX3PARTITION = 0x53,	/* no DDO:  use xlated geom */
	DM6_PARTITION =	0x54,		/* has DDO: use xlated geom & offset */
	EZD_PARTITION =	0x55,		/* EZ-DRIVE */

	FREEBSD_PARTITION = 0xa5,	/* FreeBSD Partition ID */
	OPENBSD_PARTITION = 0xa6,	/* OpenBSD Partition ID */
	NETBSD_PARTITION = 0xa9,	/* NetBSD Partition ID */
	BSDI_PARTITION = 0xb7,		/* BSDI Partition ID */
	MINIX_PARTITION = 0x81,		/* Minix Partition ID */
	UNIXWARE_PARTITION = 0x63,	/* Same as GNU_HURD and SCO Unix */
};

#ifndef __KERNEL__

struct partition {
	unsigned char boot_ind;		/* 0x80 - active */
	unsigned char head;		/* starting head */
	unsigned char sector;		/* starting sector */
	unsigned char cyl;		/* starting cylinder */
	unsigned char sys_ind;		/* What partition type */
	unsigned char end_head;		/* end head */
	unsigned char end_sector;	/* end sector */
	unsigned char end_cyl;		/* end cylinder */
	unsigned int start_sect;	/* starting sector counting from 0 */
	unsigned int nr_sects;		/* nr of sectors in partition */
} __attribute__((packed));

#endif

#ifdef __KERNEL__
#include <linux/major.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/fs.h>

struct partition {
	unsigned char boot_ind;		/* 0x80 - active */
	unsigned char head;		/* starting head */
	unsigned char sector;		/* starting sector */
	unsigned char cyl;		/* starting cylinder */
	unsigned char sys_ind;		/* What partition type */
	unsigned char end_head;		/* end head */
	unsigned char end_sector;	/* end sector */
	unsigned char end_cyl;		/* end cylinder */
	__le32 start_sect;	/* starting sector counting from 0 */
	__le32 nr_sects;		/* nr of sectors in partition */
} __attribute__((packed));

struct disk_stats {
	unsigned long sectors[2];	/* READs and WRITEs */
	unsigned long ios[2];
	unsigned long merges[2];
	unsigned long ticks[2];
	unsigned long io_ticks;
	unsigned long time_in_queue;
};

/*
 * Auxiliary struct to avoid kABI breakage
 */
struct partstats {
	struct hlist_node hlist;
	struct rcu_head rcu;
	void *addr;
	unsigned long stamp, stamp_idle;
	int in_flight;
#ifdef	CONFIG_SMP
	struct disk_stats *dkstats;
#else
	struct disk_stats dkstats;
#endif
};

extern struct partstats *get_partstats(struct hd_struct *);

struct hd_struct {
	sector_t start_sect;
	sector_t nr_sects;
	struct kobject kobj;
	struct kobject *holder_dir;
	unsigned ios[2], sectors[2];	/* READs and WRITEs */
	int policy, partno;
};

#define GENHD_FL_REMOVABLE			1
#define GENHD_FL_DRIVERFS			2
#define GENHD_FL_CD				8
#define GENHD_FL_UP				16
#define GENHD_FL_SUPPRESS_PARTITION_INFO	32
	
struct gendisk {
	int major;			/* major number of driver */
	int first_minor;
	int minors;                     /* maximum number of minors, =1 for
                                         * disks that can't be partitioned. */
	char disk_name[32];		/* name of major driver */
	struct hd_struct **part;	/* [indexed by minor] */
	int part_uevent_suppress;
	struct block_device_operations *fops;
	struct request_queue *queue;
	void *private_data;
	sector_t capacity;

	int flags;
	struct device *driverfs_dev;
	struct kobject kobj;
	struct kobject *holder_dir;
	struct kobject *slave_dir;

	struct timer_rand_state *random;
	int policy;

	atomic_t sync_io;		/* RAID */
	unsigned long stamp;
	int in_flight;
#ifdef	CONFIG_SMP
	struct disk_stats *dkstats;
#else
	struct disk_stats dkstats;
#endif
};

/* Structure for sysfs attributes on block devices */
struct disk_attribute {
	struct attribute attr;
	ssize_t (*show)(struct gendisk *, char *);
	ssize_t (*store)(struct gendisk *, const char *, size_t);
};

/* 
 * Macros to operate on percpu disk statistics:
 *
 * The __ variants should only be called in critical sections. The full
 * variants disable/enable preemption.
 */
static inline struct hd_struct *get_part(struct gendisk *gendiskp,
					 sector_t sector)
{
	struct hd_struct *part;
	int i;
	for (i = 0; i < gendiskp->minors - 1; i++) {
		part = gendiskp->part[i];
		if (part && part->start_sect <= sector
		    && sector < part->start_sect + part->nr_sects)
			return part;
	}
	return NULL;
}

#define disk_stats_index(field) (offsetof(struct disk_stats, field))
/*
 * Maintain old stats field of struct hd_struct to avoid kABI breakage
 */
static inline void part_stat_add_old(struct hd_struct *part, int addnd,
							int offset)
{
	switch( offset ) {
	case disk_stats_index(ios[0]):
		part->ios[0] += addnd;
		break;
	case disk_stats_index(ios[1]):
		part->ios[1] += addnd;
		break;
	case disk_stats_index(sectors[0]):
		part->sectors[0] += addnd;
		break;
	case disk_stats_index(sectors[1]):
		part->sectors[1] += addnd;
		break;
	}
}

#ifdef	CONFIG_SMP
#define __disk_stat_add(gendiskp, field, addnd) 	\
	(per_cpu_ptr(gendiskp->dkstats, smp_processor_id())->field += addnd)

#define disk_stat_read(gendiskp, field)					\
({									\
	typeof(gendiskp->dkstats->field) res = 0;			\
	int i;								\
	for_each_possible_cpu(i)					\
		res += per_cpu_ptr(gendiskp->dkstats, i)->field;	\
	res;								\
})

static inline void disk_stat_set_all(struct gendisk *gendiskp, int value)	{
	int i;
	for_each_possible_cpu(i)
		memset(per_cpu_ptr(gendiskp->dkstats, i), value,
				sizeof (struct disk_stats));
}		

#define part_stat_add(part, field, addnd)				\
({									\
	struct partstats *ps;						\
	rcu_read_lock();						\
	ps = get_partstats(part);					\
	per_cpu_ptr(ps->dkstats, smp_processor_id())->field += addnd;	\
	rcu_read_unlock();						\
	part_stat_add_old(part, addnd, disk_stats_index(field));	\
})

#define __all_stat_add(gendiskp, field, addnd, sector)		\
({								\
	struct hd_struct *part = get_part(gendiskp, sector);	\
	if (part)						\
		part_stat_add(part, field, addnd);		\
	__disk_stat_add(gendiskp, field, addnd);		\
})

#define all_stat_add(gendiskp, field, addnd, sector)		\
({								\
	struct hd_struct *part = get_part(gendiskp, sector);	\
	if (part)						\
		part_stat_add(part, field, addnd);		\
	disk_stat_add(gendiskp, field, addnd);			\
})

#define part_stat_read(part, field)				\
({								\
	struct partstats *ps;					\
	typeof(ps->dkstats->field) res = 0;			\
	int i;							\
	rcu_read_lock();					\
	ps = get_partstats(part);				\
	for_each_possible_cpu(i)				\
		res += per_cpu_ptr(ps->dkstats, i)->field;	\
	rcu_read_unlock();					\
	res;							\
})

static inline void part_stat_reset(struct hd_struct *part)
{
	struct partstats *ps;
	int i;
	rcu_read_lock();
	ps = get_partstats(part);
	for_each_possible_cpu(i)
		memset(per_cpu_ptr(ps->dkstats, i), 0,
		       sizeof(struct disk_stats));
	rcu_read_unlock();
}
				
#else
#define __disk_stat_add(gendiskp, field, addnd) \
				(gendiskp->dkstats.field += addnd)
#define disk_stat_read(gendiskp, field)	(gendiskp->dkstats.field)

static inline void disk_stat_set_all(struct gendisk *gendiskp, int value)	{
	memset(&gendiskp->dkstats, value, sizeof (struct disk_stats));
}
#define part_stat_add(part, field, addnd) 			\
({								\
	rcu_read_lock();					\
	get_partstats(part)->dkstats.field += addnd;		\
	rcu_read_unlock();					\
	part_stat_add_old(part, addnd, disk_stats_index(field));\
})

#define __all_stat_add(gendiskp, field, addnd, sector)		\
({								\
	struct hd_struct *part = get_part(gendiskp, sector);	\
	if (part) {						\
		rcu_read_lock();				\
		get_partstats(part)->dkstats.field += addnd;	\
		rcu_read_unlock();				\
	}							\
	__disk_stat_add(gendiskp, field, addnd);		\
})

#define all_stat_add(gendiskp, field, addnd, sector)		\
({								\
	struct hd_struct *part = get_part(gendiskp, sector);	\
	if (part) {						\
		rcu_read_lock();				\
		get_partstats(part)->dkstats.field += addnd;	\
		rcu_read_unlock();				\
	}							\
	disk_stat_add(gendiskp, field, addnd);			\
})

#define part_stat_read(part, field)	 		\
({							\
	typeof(((struct disk_stats *)0)->field) res;	\
	rcu_read_lock();				\
	res = get_partstats(part)->dkstats.field;	\
	rcu_read_unlock();				\
	res;						\
})

static inline void part_stat_reset(struct hd_struct *part)
{
	rcu_read_lock();
	memset(&get_partstats(part)->dkstats, 0, sizeof(struct disk_stats));
	rcu_read_unlock();
}
#endif

#define disk_stat_add(gendiskp, field, addnd)			\
	do {							\
		preempt_disable();				\
		__disk_stat_add(gendiskp, field, addnd);	\
		preempt_enable();				\
	} while (0)

#define __disk_stat_dec(gendiskp, field) __disk_stat_add(gendiskp, field, -1)
#define disk_stat_dec(gendiskp, field) disk_stat_add(gendiskp, field, -1)

#define __disk_stat_inc(gendiskp, field) __disk_stat_add(gendiskp, field, 1)
#define disk_stat_inc(gendiskp, field) disk_stat_add(gendiskp, field, 1)

#define __disk_stat_sub(gendiskp, field, subnd) \
		__disk_stat_add(gendiskp, field, -subnd)
#define disk_stat_sub(gendiskp, field, subnd) \
		disk_stat_add(gendiskp, field, -subnd)

#define part_stat_inc(gendiskp, field) part_stat_add(gendiskp, field, 1)
#define part_stat_dec(gendiskp, field) part_stat_add(gendiskp, field, -1)
#define part_stat_sub(gendiskp, field, subnd) \
		part_stat_add(gendiskp, field, -subnd)

#define __all_stat_inc(gendiskp, field, sector) \
		__all_stat_add(gendiskp, field, 1, sector)
#define all_stat_inc(gendiskp, field, sector) \
		all_stat_add(gendiskp, field, 1, sector)
#define __all_stat_dec(gendiskp, field, sector) \
		__all_stat_add(gendiskp, field, -1, sector)
#define all_stat_dec(gendiskp, field, sector) \
		all_stat_add(gendiskp, field, -1, sector)
#define __all_stat_sub(gendiskp, field, subnd, sector) \
		__all_stat_add(gendiskp, field, -subnd, sector)
#define all_stat_sub(gendiskp, field, subnd, sector) \
		all_stat_add(gendiskp, field, -subnd, sector)

/* Inlines to alloc and free disk stats in struct gendisk */
#ifdef  CONFIG_SMP
static inline int init_disk_stats(struct gendisk *disk)
{
	disk->dkstats = alloc_percpu(struct disk_stats);
	if (!disk->dkstats)
		return 0;
	return 1;
}

static inline void free_disk_stats(struct gendisk *disk)
{
	free_percpu(disk->dkstats);
}

static inline int init_part_stats(struct partstats *ps)
{
	ps->dkstats = alloc_percpu(struct disk_stats);
	if (!ps->dkstats)
		return 0;
	return 1;
}

static inline void free_part_stats(struct partstats *ps)
{
	free_percpu(ps->dkstats);
}

#else	/* CONFIG_SMP */
static inline int init_disk_stats(struct gendisk *disk)
{
	return 1;
}

static inline void free_disk_stats(struct gendisk *disk)
{
}

static inline int init_part_stats(struct partstats *ps)
{
	return 1;
}

static inline void free_part_stats(struct partstats *ps)
{
}
#endif	/* CONFIG_SMP */

/* drivers/block/ll_rw_blk.c */
extern void disk_round_stats(struct gendisk *disk);
extern void part_round_stats(struct hd_struct *part);

/* drivers/block/genhd.c */
extern int get_blkdev_list(char *, int);
extern void add_disk(struct gendisk *disk);
extern void del_gendisk(struct gendisk *gp);
extern void unlink_gendisk(struct gendisk *gp);
extern struct gendisk *get_gendisk(dev_t dev, int *part);
int init_partstats(struct hd_struct *part);
void free_partstats(struct hd_struct *part);

extern void set_device_ro(struct block_device *bdev, int flag);
extern void set_disk_ro(struct gendisk *disk, int flag);

/* drivers/char/random.c */
extern void add_disk_randomness(struct gendisk *disk);
extern void rand_initialize_disk(struct gendisk *disk);

static inline sector_t get_start_sect(struct block_device *bdev)
{
	return bdev->bd_contains == bdev ? 0 : bdev->bd_part->start_sect;
}
static inline sector_t get_capacity(struct gendisk *disk)
{
	return disk->capacity;
}
static inline void set_capacity(struct gendisk *disk, sector_t size)
{
	disk->capacity = size;
}

#endif  /*  __KERNEL__  */

#ifdef CONFIG_SOLARIS_X86_PARTITION

#define SOLARIS_X86_NUMSLICE	8
#define SOLARIS_X86_VTOC_SANE	(0x600DDEEEUL)

struct solaris_x86_slice {
	__le16 s_tag;		/* ID tag of partition */
	__le16 s_flag;		/* permission flags */
	__le32 s_start;		/* start sector no of partition */
	__le32 s_size;		/* # of blocks in partition */
};

struct solaris_x86_vtoc {
	unsigned int v_bootinfo[3];	/* info needed by mboot (unsupported) */
	__le32 v_sanity;		/* to verify vtoc sanity */
	__le32 v_version;		/* layout version */
	char	v_volume[8];		/* volume name */
	__le16	v_sectorsz;		/* sector size in bytes */
	__le16	v_nparts;		/* number of partitions */
	unsigned int v_reserved[10];	/* free space */
	struct solaris_x86_slice
		v_slice[SOLARIS_X86_NUMSLICE]; /* slice headers */
	unsigned int timestamp[SOLARIS_X86_NUMSLICE]; /* timestamp (unsupported) */
	char	v_asciilabel[128];	/* for compatibility */
};

#endif /* CONFIG_SOLARIS_X86_PARTITION */

#ifdef CONFIG_BSD_DISKLABEL
/*
 * BSD disklabel support by Yossi Gottlieb <yogo@math.tau.ac.il>
 * updated by Marc Espie <Marc.Espie@openbsd.org>
 */

/* check against BSD src/sys/sys/disklabel.h for consistency */

#define BSD_DISKMAGIC	(0x82564557UL)	/* The disk magic number */
#define BSD_MAXPARTITIONS	16
#define OPENBSD_MAXPARTITIONS	16
#define BSD_FS_UNUSED		0	/* disklabel unused partition entry ID */
struct bsd_disklabel {
	__le32	d_magic;		/* the magic number */
	__s16	d_type;			/* drive type */
	__s16	d_subtype;		/* controller/d_type specific */
	char	d_typename[16];		/* type name, e.g. "eagle" */
	char	d_packname[16];			/* pack identifier */ 
	__u32	d_secsize;		/* # of bytes per sector */
	__u32	d_nsectors;		/* # of data sectors per track */
	__u32	d_ntracks;		/* # of tracks per cylinder */
	__u32	d_ncylinders;		/* # of data cylinders per unit */
	__u32	d_secpercyl;		/* # of data sectors per cylinder */
	__u32	d_secperunit;		/* # of data sectors per unit */
	__u16	d_sparespertrack;	/* # of spare sectors per track */
	__u16	d_sparespercyl;		/* # of spare sectors per cylinder */
	__u32	d_acylinders;		/* # of alt. cylinders per unit */
	__u16	d_rpm;			/* rotational speed */
	__u16	d_interleave;		/* hardware sector interleave */
	__u16	d_trackskew;		/* sector 0 skew, per track */
	__u16	d_cylskew;		/* sector 0 skew, per cylinder */
	__u32	d_headswitch;		/* head switch time, usec */
	__u32	d_trkseek;		/* track-to-track seek, usec */
	__u32	d_flags;		/* generic flags */
#define NDDATA 5
	__u32	d_drivedata[NDDATA];	/* drive-type specific information */
#define NSPARE 5
	__u32	d_spare[NSPARE];	/* reserved for future use */
	__le32	d_magic2;		/* the magic number (again) */
	__le16	d_checksum;		/* xor of data incl. partitions */

			/* filesystem and partition information: */
	__le16	d_npartitions;		/* number of partitions in following */
	__le32	d_bbsize;		/* size of boot area at sn0, bytes */
	__le32	d_sbsize;		/* max size of fs superblock, bytes */
	struct	bsd_partition {		/* the partition table */
		__le32	p_size;		/* number of sectors in partition */
		__le32	p_offset;	/* starting sector */
		__le32	p_fsize;	/* filesystem basic fragment size */
		__u8	p_fstype;	/* filesystem type, see below */
		__u8	p_frag;		/* filesystem fragments per block */
		__le16	p_cpg;		/* filesystem cylinders per group */
	} d_partitions[BSD_MAXPARTITIONS];	/* actually may be more */
};

#endif	/* CONFIG_BSD_DISKLABEL */

#ifdef CONFIG_UNIXWARE_DISKLABEL
/*
 * Unixware slices support by Andrzej Krzysztofowicz <ankry@mif.pg.gda.pl>
 * and Krzysztof G. Baranowski <kgb@knm.org.pl>
 */

#define UNIXWARE_DISKMAGIC     (0xCA5E600DUL)	/* The disk magic number */
#define UNIXWARE_DISKMAGIC2    (0x600DDEEEUL)	/* The slice table magic nr */
#define UNIXWARE_NUMSLICE      16
#define UNIXWARE_FS_UNUSED     0		/* Unused slice entry ID */

struct unixware_slice {
	__le16   s_label;	/* label */
	__le16   s_flags;	/* permission flags */
	__le32   start_sect;	/* starting sector */
	__le32   nr_sects;	/* number of sectors in slice */
};

struct unixware_disklabel {
	__le32   d_type;               	/* drive type */
	__le32   d_magic;                /* the magic number */
	__le32   d_version;              /* version number */
	char    d_serial[12];           /* serial number of the device */
	__le32   d_ncylinders;           /* # of data cylinders per device */
	__le32   d_ntracks;              /* # of tracks per cylinder */
	__le32   d_nsectors;             /* # of data sectors per track */
	__le32   d_secsize;              /* # of bytes per sector */
	__le32   d_part_start;           /* # of first sector of this partition */
	__le32   d_unknown1[12];         /* ? */
 	__le32	d_alt_tbl;              /* byte offset of alternate table */
 	__le32	d_alt_len;              /* byte length of alternate table */
 	__le32	d_phys_cyl;             /* # of physical cylinders per device */
 	__le32	d_phys_trk;             /* # of physical tracks per cylinder */
 	__le32	d_phys_sec;             /* # of physical sectors per track */
 	__le32	d_phys_bytes;           /* # of physical bytes per sector */
 	__le32	d_unknown2;             /* ? */
	__le32   d_unknown3;             /* ? */
	__le32	d_pad[8];               /* pad */

	struct unixware_vtoc {
		__le32	v_magic;		/* the magic number */
		__le32	v_version;		/* version number */
		char	v_name[8];		/* volume name */
		__le16	v_nslices;		/* # of slices */
		__le16	v_unknown1;		/* ? */
		__le32	v_reserved[10];		/* reserved */
		struct unixware_slice
			v_slice[UNIXWARE_NUMSLICE];	/* slice headers */
	} vtoc;

};  /* 408 */

#endif /* CONFIG_UNIXWARE_DISKLABEL */

#ifdef CONFIG_MINIX_SUBPARTITION
#   define MINIX_NR_SUBPARTITIONS  4
#endif /* CONFIG_MINIX_SUBPARTITION */

#ifdef __KERNEL__

char *disk_name (struct gendisk *hd, int part, char *buf);

extern int rescan_partitions(struct gendisk *disk, struct block_device *bdev);
extern void add_partition(struct gendisk *, int, sector_t, sector_t);
extern void delete_partition(struct gendisk *, int);

extern struct gendisk *alloc_disk_node(int minors, int node_id);
extern struct gendisk *alloc_disk(int minors);
extern struct kobject *get_disk(struct gendisk *disk);
extern void put_disk(struct gendisk *disk);

extern void blk_register_region(dev_t dev, unsigned long range,
			struct module *module,
			struct kobject *(*probe)(dev_t, int *, void *),
			int (*lock)(dev_t, void *),
			void *data);
extern void blk_unregister_region(dev_t dev, unsigned long range);

static inline struct block_device *bdget_disk(struct gendisk *disk, int index)
{
	return bdget(MKDEV(disk->major, disk->first_minor) + index);
}

#endif

#endif
