#include "amd64_edac.h"
#include <asm/k8.h>

static int report_gart_errors;
module_param(report_gart_errors, int, 0644);

/*
 * Set by command line parameter. If BIOS has enabled the ECC, this override is
 * cleared to prevent re-enabling the hardware by this driver.
 */
static int ecc_enable_override;
module_param(ecc_enable_override, int, 0644);

/* Lookup table for all possible MC control instances */
struct amd64_pvt;
static struct mem_ctl_info *mci_lookup[EDAC_MAX_NUMNODES];
static struct amd64_pvt *pvt_lookup[EDAC_MAX_NUMNODES];

/*
 * Address to DRAM bank mapping: see F2x[1,0]80 for Fam10 and later.
 */
static int ddr2_dbam[] = { [0]		= 128,
			   [1]		= 256,
			   [2 ... 4]	= 512,
			   [5 ... 6]	= 1024,
			   [7 ... 8]	= 2048,
			   [9 ... 10]	= 4096,
			   [11]		= 8192,
};

static int ddr3_dbam[] = { [0]		= -1,
			   [1]		= 256,
			   [2]		= 512,
			   [3 ... 4]	= -1,
			   [5 ... 6]	= 1024,
			   [7 ... 8]	= 2048,
			   [9 ... 10]	= 4096,
			   [11]	= 8192,
};
u32 revf_quad_ddr2_shift[] = {
       0,      /* 0000b NULL DIMM (128mb) */
       28,     /* 0001b 256mb */
       29,     /* 0010b 512mb */
       29,     /* 0011b 512mb */
       29,     /* 0100b 512mb */
       30,     /* 0101b 1gb */
       30,     /* 0110b 1gb */
       31,     /* 0111b 2gb */
       31,     /* 1000b 2gb */
       32,     /* 1001b 4gb */
       32,     /* 1010b 4gb */
       33,     /* 1011b 8gb */
       0,      /* 1100b future */
       0,      /* 1101b future */
       0,      /* 1110b future */
       0       /* 1111b future */
};

/* Map from a CSROW entry to the mask entry that operates on it */
static inline u32 amd64_map_to_dcs_mask(struct amd64_pvt *pvt, int csrow)
{
       if (boot_cpu_data.x86 == 0xf && pvt->ext_model < K8_REV_F)
               return csrow;
       else
               return csrow >> 1;
}

/* return the 'base' address the i'th CS entry of the 'dct' DRAM controller */
static u32 amd64_get_dct_base(struct amd64_pvt *pvt, int dct, int csrow)
{
       if (dct == 0)
               return pvt->dcsb0[csrow];
       else
               return pvt->dcsb1[csrow];
}

/*
 * Return the 'mask' address the i'th CS entry. This function is needed because
 * there number of DCSM registers on Rev E and prior vs Rev F and later is
 * different.
 */
static u32 amd64_get_dct_mask(struct amd64_pvt *pvt, int dct, int csrow)
{
       if (dct == 0)
               return pvt->dcsm0[amd64_map_to_dcs_mask(pvt, csrow)];
       else
               return pvt->dcsm1[amd64_map_to_dcs_mask(pvt, csrow)];
}


/*
 * In *base and *limit, pass back the full 40-bit base and limit physical
 * addresses for the node given by node_id.  This information is obtained from
 * DRAM Base (section 3.4.4.1) and DRAM Limit (section 3.4.4.2) registers. The
 * base and limit addresses are of type SysAddr, as defined at the start of
 * section 3.4.4 (p. 70).  They are the lowest and highest physical addresses
 * in the address range they represent.
 */
static void amd64_get_base_and_limit(struct amd64_pvt *pvt, int node_id,
                              u64 *base, u64 *limit)
{
       *base = pvt->dram_base[node_id];
       *limit = pvt->dram_limit[node_id];
}

/*
 * Return 1 if the SysAddr given by sys_addr matches the base/limit associated
 * with node_id
 */
static int amd64_base_limit_match(struct amd64_pvt *pvt,
                                       u64 sys_addr, int node_id)
{
       u64 base, limit, addr;

       amd64_get_base_and_limit(pvt, node_id, &base, &limit);

       /* The K8 treats this as a 40-bit value.  However, bits 63-40 will be
        * all ones if the most significant implemented address bit is 1.
        * Here we discard bits 63-40.  See section 3.4.2 of AMD publication
        * 24592: AMD x86-64 Architecture Programmer's Manual Volume 1
        * Application Programming.
        */
       addr = sys_addr & 0x000000ffffffffffull;

       return (addr >= base) && (addr <= limit);
}

/*
 * Attempt to map a SysAddr to a node. On success, return a pointer to the
 * mem_ctl_info structure for the node that the SysAddr maps to.
 *
 * On failure, return NULL.
 */
static struct mem_ctl_info *find_mc_by_sys_addr(struct mem_ctl_info *mci,
                                               u64 sys_addr)
{
       struct amd64_pvt *pvt;
       int node_id;
       u32 intlv_en, bits;

       /*
        * Here we use the DRAM Base (section 3.4.4.1) and DRAM Limit (section
        * 3.4.4.2) registers to map the SysAddr to a node ID.
        */
       pvt = mci->pvt_info;

       /*
        * The value of this field should be the same for all DRAM Base
        * registers.  Therefore we arbitrarily choose to read it from the
        * register for node 0.
        */
       intlv_en = pvt->dram_IntlvEn[0];

       if (intlv_en == 0) {
               for (node_id = 0; node_id < DRAM_REG_COUNT; node_id++) {
                       if (amd64_base_limit_match(pvt, sys_addr, node_id))
                               goto found;
               }
               goto err_no_match;
       }

       if (unlikely((intlv_en != 0x01) &&
                    (intlv_en != 0x03) &&
                    (intlv_en != 0x07))) {
               amd64_printk(KERN_WARNING, "junk value of 0x%x extracted from "
                            "IntlvEn field of DRAM Base Register for node 0: "
                            "this probably indicates a BIOS bug.\n", intlv_en);
               return NULL;
       }

       bits = (((u32) sys_addr) >> 12) & intlv_en;

       for (node_id = 0; ; ) {
               if ((pvt->dram_IntlvSel[node_id] & intlv_en) == bits)
                       break;  /* intlv_sel field matches */

               if (++node_id >= DRAM_REG_COUNT)
                       goto err_no_match;
       }

       /* sanity test for sys_addr */
       if (unlikely(!amd64_base_limit_match(pvt, sys_addr, node_id))) {
               amd64_printk(KERN_WARNING,
                            "%s(): sys_addr 0x%llx falls outside base/limit "
                            "address range for node %d with node interleaving "
                            "enabled.\n",
                            __func__, sys_addr, node_id);
               return NULL;
       }

found:
       return edac_mc_find(node_id);

err_no_match:
       debugf2("sys_addr 0x%lx doesn't match any node\n",
               (unsigned long)sys_addr);

       return NULL;
}

/*
 * Extract the DRAM CS base address from selected csrow register.
 */
static u64 base_from_dct_base(struct amd64_pvt *pvt, int csrow)
{
       return ((u64) (amd64_get_dct_base(pvt, 0, csrow) & pvt->dcsb_base)) <<
                               pvt->dcs_shift;
}

/*
 * Extract the mask from the dcsb0[csrow] entry in a CPU revision-specific way.
 */
static u64 mask_from_dct_mask(struct amd64_pvt *pvt, int csrow)
{
       u64 dcsm_bits, other_bits;
       u64 mask;

       /* Extract bits from DRAM CS Mask. */
       dcsm_bits = amd64_get_dct_mask(pvt, 0, csrow) & pvt->dcsm_mask;

       other_bits = pvt->dcsm_mask;
       other_bits = ~(other_bits << pvt->dcs_shift);

       /*
        * The extracted bits from DCSM belong in the spaces represented by
        * the cleared bits in other_bits.
        */
       mask = (dcsm_bits << pvt->dcs_shift) | other_bits;

       return mask;
}

/*
 * @input_addr is an InputAddr associated with the node given by mci. Return the
 * csrow that input_addr maps to, or -1 on failure (no csrow claims input_addr).
 */
static int input_addr_to_csrow(struct mem_ctl_info *mci, u64 input_addr)
{
       struct amd64_pvt *pvt;
       int csrow;
       u64 base, mask;

       pvt = mci->pvt_info;

       /*
        * Here we use the DRAM CS Base and DRAM CS Mask registers. For each CS
        * base/mask register pair, test the condition shown near the start of
        * section 3.5.4 (p. 84, BKDG #26094, K8, revA-E).
        */
       for (csrow = 0; csrow < pvt->cs_count; csrow++) {

               /* This DRAM chip select is disabled on this node */
               if ((pvt->dcsb0[csrow] & K8_DCSB_CS_ENABLE) == 0)
                       continue;

               base = base_from_dct_base(pvt, csrow);
               mask = ~mask_from_dct_mask(pvt, csrow);

               if ((input_addr & mask) == (base & mask)) {
                       debugf2("InputAddr 0x%lx matches csrow %d (node %d)\n",
                               (unsigned long)input_addr, csrow,
                               pvt->mc_node_id);

                       return csrow;
               }
       }

       debugf2("no matching csrow for InputAddr 0x%lx (MC node %d)\n",
               (unsigned long)input_addr, pvt->mc_node_id);

       return -1;
}

/*
 * Return the base value defined by the DRAM Base register for the node
 * represented by mci.  This function returns the full 40-bit value despite the
 * fact that the register only stores bits 39-24 of the value. See section
 * 3.4.4.1 (BKDG #26094, K8, revA-E)
 */
static inline u64 get_dram_base(struct mem_ctl_info *mci)
{
       struct amd64_pvt *pvt = mci->pvt_info;

       return pvt->dram_base[pvt->mc_node_id];
}

/*
 * Obtain info from the DRAM Hole Address Register (section 3.4.8, pub #26094)
 * for the node represented by mci. Info is passed back in *hole_base,
 * *hole_offset, and *hole_size.  Function returns 0 if info is valid or 1 if
 * info is invalid. Info may be invalid for either of the following reasons:
 *
 * - The revision of the node is not E or greater.  In this case, the DRAM Hole
 *   Address Register does not exist.
 *
 * - The DramHoleValid bit is cleared in the DRAM Hole Address Register,
 *   indicating that its contents are not valid.
 *
 * The values passed back in *hole_base, *hole_offset, and *hole_size are
 * complete 32-bit values despite the fact that the bitfields in the DHAR
 * only represent bits 31-24 of the base and offset values.
 */
int amd64_get_dram_hole_info(struct mem_ctl_info *mci, u64 *hole_base,
                            u64 *hole_offset, u64 *hole_size)
{
       struct amd64_pvt *pvt = mci->pvt_info;
       u64 base;

       /* only revE and later have the DRAM Hole Address Register */
       if (boot_cpu_data.x86 == 0xf && pvt->ext_model < K8_REV_E) {
               debugf1("  revision %d for node %d does not support DHAR\n",
                       pvt->ext_model, pvt->mc_node_id);
               return 1;
       }

       /* only valid for Fam10h */
       if (boot_cpu_data.x86 == 0x10 &&
           (pvt->dhar & F10_DRAM_MEM_HOIST_VALID) == 0) {
               debugf1("  Dram Memory Hoisting is DISABLED on this system\n");
               return 1;
       }

       if ((pvt->dhar & DHAR_VALID) == 0) {
               debugf1("  Dram Memory Hoisting is DISABLED on this node %d\n",
                       pvt->mc_node_id);
               return 1;
       }

       /* This node has Memory Hoisting */

       /* +------------------+--------------------+--------------------+-----
        * | memory           | DRAM hole          | relocated          |
        * | [0, (x - 1)]     | [x, 0xffffffff]    | addresses from     |
        * |                  |                    | DRAM hole          |
        * |                  |                    | [0x100000000,      |
        * |                  |                    |  (0x100000000+     |
        * |                  |                    |   (0xffffffff-x))] |
        * +------------------+--------------------+--------------------+-----
        *
        * Above is a diagram of physical memory showing the DRAM hole and the
        * relocated addresses from the DRAM hole.  As shown, the DRAM hole
        * starts at address x (the base address) and extends through address
        * 0xffffffff.  The DRAM Hole Address Register (DHAR) relocates the
        * addresses in the hole so that they start at 0x100000000.
        */

       base = dhar_base(pvt->dhar);

       *hole_base = base;
       *hole_size = (0x1ull << 32) - base;

       if (boot_cpu_data.x86 > 0xf)
               *hole_offset = f10_dhar_offset(pvt->dhar);
       else
               *hole_offset = k8_dhar_offset(pvt->dhar);

       debugf1("  DHAR info for node %d base 0x%lx offset 0x%lx size 0x%lx\n",
               pvt->mc_node_id, (unsigned long)*hole_base,
               (unsigned long)*hole_offset, (unsigned long)*hole_size);

       return 0;
}
EXPORT_SYMBOL_GPL(amd64_get_dram_hole_info);

/*
 * Return the DramAddr that the SysAddr given by @sys_addr maps to.  It is
 * assumed that sys_addr maps to the node given by mci.
 *
 * The first part of section 3.4.4 (p. 70) shows how the DRAM Base (section
 * 3.4.4.1) and DRAM Limit (section 3.4.4.2) registers are used to translate a
 * SysAddr to a DramAddr. If the DRAM Hole Address Register (DHAR) is enabled,
 * then it is also involved in translating a SysAddr to a DramAddr. Sections
 * 3.4.8 and 3.5.8.2 describe the DHAR and how it is used for memory hoisting.
 * These parts of the documentation are unclear. I interpret them as follows:
 *
 * When node n receives a SysAddr, it processes the SysAddr as follows:
 *
 * 1. It extracts the DRAMBase and DRAMLimit values from the DRAM Base and DRAM
 *    Limit registers for node n. If the SysAddr is not within the range
 *    specified by the base and limit values, then node n ignores the Sysaddr
 *    (since it does not map to node n). Otherwise continue to step 2 below.
 *
 * 2. If the DramHoleValid bit of the DHAR for node n is clear, the DHAR is
 *    disabled so skip to step 3 below. Otherwise see if the SysAddr is within
 *    the range of relocated addresses (starting at 0x100000000) from the DRAM
 *    hole. If not, skip to step 3 below. Else get the value of the
 *    DramHoleOffset field from the DHAR. To obtain the DramAddr, subtract the
 *    offset defined by this value from the SysAddr.
 *
 * 3. Obtain the base address for node n from the DRAMBase field of the DRAM
 *    Base register for node n. To obtain the DramAddr, subtract the base
 *    address from the SysAddr, as shown near the start of section 3.4.4 (p.70).
 */
static u64 sys_addr_to_dram_addr(struct mem_ctl_info *mci, u64 sys_addr)
{
       u64 dram_base, hole_base, hole_offset, hole_size, dram_addr;
       int ret = 0;

       dram_base = get_dram_base(mci);

       ret = amd64_get_dram_hole_info(mci, &hole_base, &hole_offset,
                                     &hole_size);
       if (!ret) {
               if ((sys_addr >= (1ull << 32)) &&
                   (sys_addr < ((1ull << 32) + hole_size))) {
                       /* use DHAR to translate SysAddr to DramAddr */
                       dram_addr = sys_addr - hole_offset;

                       debugf2("using DHAR to translate SysAddr 0x%lx to "
                               "DramAddr 0x%lx\n",
                               (unsigned long)sys_addr,
                               (unsigned long)dram_addr);

                       return dram_addr;
               }
       }

       /*
        * Translate the SysAddr to a DramAddr as shown near the start of
        * section 3.4.4 (p. 70).  Although sys_addr is a 64-bit value, the k8
        * only deals with 40-bit values.  Therefore we discard bits 63-40 of
        * sys_addr below.  If bit 39 of sys_addr is 1 then the bits we
        * discard are all 1s.  Otherwise the bits we discard are all 0s.  See
        * section 3.4.2 of AMD publication 24592: AMD x86-64 Architecture
        * Programmer's Manual Volume 1 Application Programming.
        */
       dram_addr = (sys_addr & 0xffffffffffull) - dram_base;

       debugf2("using DRAM Base register to translate SysAddr 0x%lx to "
               "DramAddr 0x%lx\n", (unsigned long)sys_addr,
               (unsigned long)dram_addr);
       return dram_addr;
}

/*
 * @intlv_en is the value of the IntlvEn field from a DRAM Base register
 * (section 3.4.4.1).  Return the number of bits from a SysAddr that are used
 * for node interleaving.
 */
static int num_node_interleave_bits(unsigned intlv_en)
{
       static const int intlv_shift_table[] = { 0, 1, 0, 2, 0, 0, 0, 3 };
       int n;

       BUG_ON(intlv_en > 7);
       n = intlv_shift_table[intlv_en];
       return n;
}

/* Translate the DramAddr given by @dram_addr to an InputAddr. */
static u64 dram_addr_to_input_addr(struct mem_ctl_info *mci, u64 dram_addr)
{
       struct amd64_pvt *pvt;
       int intlv_shift;
       u64 input_addr;

       pvt = mci->pvt_info;

       /*
        * See the start of section 3.4.4 (p. 70, BKDG #26094, K8, revA-E)
        * concerning translating a DramAddr to an InputAddr.
        */
       intlv_shift = num_node_interleave_bits(pvt->dram_IntlvEn[0]);
       input_addr = ((dram_addr >> intlv_shift) & 0xffffff000ull) +
           (dram_addr & 0xfff);

       debugf2("  Intlv Shift=%d DramAddr=0x%lx maps to InputAddr=0x%lx\n",
               intlv_shift, (unsigned long)dram_addr,
               (unsigned long)input_addr);

       return input_addr;
}

/*
 * Translate the SysAddr represented by @sys_addr to an InputAddr.  It is
 * assumed that @sys_addr maps to the node given by mci.
 */
static u64 sys_addr_to_input_addr(struct mem_ctl_info *mci, u64 sys_addr)
{
       u64 input_addr;

       input_addr =
           dram_addr_to_input_addr(mci, sys_addr_to_dram_addr(mci, sys_addr));

       debugf2("SysAdddr 0x%lx translates to InputAddr 0x%lx\n",
               (unsigned long)sys_addr, (unsigned long)input_addr);

       return input_addr;
}


/*
 * @input_addr is an InputAddr associated with the node represented by mci.
 * Translate @input_addr to a DramAddr and return the result.
 */
static u64 input_addr_to_dram_addr(struct mem_ctl_info *mci, u64 input_addr)
{
       struct amd64_pvt *pvt;
       int node_id, intlv_shift;
       u64 bits, dram_addr;
       u32 intlv_sel;

       /*
        * Near the start of section 3.4.4 (p. 70, BKDG #26094, K8, revA-E)
        * shows how to translate a DramAddr to an InputAddr. Here we reverse
        * this procedure. When translating from a DramAddr to an InputAddr, the
        * bits used for node interleaving are discarded.  Here we recover these
        * bits from the IntlvSel field of the DRAM Limit register (section
        * 3.4.4.2) for the node that input_addr is associated with.
        */
       pvt = mci->pvt_info;
       node_id = pvt->mc_node_id;
       BUG_ON((node_id < 0) || (node_id > 7));

       intlv_shift = num_node_interleave_bits(pvt->dram_IntlvEn[0]);

       if (intlv_shift == 0) {
               debugf1("    InputAddr 0x%lx translates to DramAddr of "
                       "same value\n", (unsigned long)input_addr);

               return input_addr;
       }

       bits = ((input_addr & 0xffffff000ull) << intlv_shift) +
           (input_addr & 0xfff);

       intlv_sel = pvt->dram_IntlvSel[node_id] & ((1 << intlv_shift) - 1);
       dram_addr = bits + (intlv_sel << 12);

       debugf1("InputAddr 0x%lx translates to DramAddr 0x%lx "
               "(%d node interleave bits)\n", (unsigned long)input_addr,
               (unsigned long)dram_addr, intlv_shift);

       return dram_addr;
}

/*
 * @dram_addr is a DramAddr that maps to the node represented by mci. Convert
 * @dram_addr to a SysAddr.
 */
static u64 dram_addr_to_sys_addr(struct mem_ctl_info *mci, u64 dram_addr)
{
       struct amd64_pvt *pvt = mci->pvt_info;
       u64 hole_base, hole_offset, hole_size, base, limit, sys_addr;
       int ret = 0;

       ret = amd64_get_dram_hole_info(mci, &hole_base, &hole_offset,
                                     &hole_size);
       if (!ret) {
               if ((dram_addr >= hole_base) &&
                   (dram_addr < (hole_base + hole_size))) {
                       sys_addr = dram_addr + hole_offset;

                       debugf1("using DHAR to translate DramAddr 0x%lx to "
                               "SysAddr 0x%lx\n", (unsigned long)dram_addr,
                               (unsigned long)sys_addr);

                       return sys_addr;
               }
       }

       amd64_get_base_and_limit(pvt, pvt->mc_node_id, &base, &limit);
       sys_addr = dram_addr + base;

       /*
        * The sys_addr we have computed up to this point is a 40-bit value
        * because the k8 deals with 40-bit values.  However, the value we are
        * supposed to return is a full 64-bit physical address.  The AMD
        * x86-64 architecture specifies that the most significant implemented
        * address bit through bit 63 of a physical address must be either all
        * 0s or all 1s.  Therefore we sign-extend the 40-bit sys_addr to a
        * 64-bit value below.  See section 3.4.2 of AMD publication 24592:
        * AMD x86-64 Architecture Programmer's Manual Volume 1 Application
        * Programming.
        */
       sys_addr |= ~((sys_addr & (1ull << 39)) - 1);

       debugf1("    Node %d, DramAddr 0x%lx to SysAddr 0x%lx\n",
               pvt->mc_node_id, (unsigned long)dram_addr,
               (unsigned long)sys_addr);

       return sys_addr;
}

/*
 * @input_addr is an InputAddr associated with the node given by mci. Translate
 * @input_addr to a SysAddr.
 */
static inline u64 input_addr_to_sys_addr(struct mem_ctl_info *mci,
                                        u64 input_addr)
{
       return dram_addr_to_sys_addr(mci,
                                    input_addr_to_dram_addr(mci, input_addr));
}

/*
 * Find the minimum and maximum InputAddr values that map to the given @csrow.
 * Pass back these values in *input_addr_min and *input_addr_max.
 */
static void find_csrow_limits(struct mem_ctl_info *mci, int csrow,
                             u64 *input_addr_min, u64 *input_addr_max)
{
       struct amd64_pvt *pvt;
       u64 base, mask;

       pvt = mci->pvt_info;
       BUG_ON((csrow < 0) || (csrow >= pvt->cs_count));

       base = base_from_dct_base(pvt, csrow);
       mask = mask_from_dct_mask(pvt, csrow);

       *input_addr_min = base & ~mask;
       *input_addr_max = base | mask | pvt->dcs_mask_notused;
}

/*
 * Extract error address from MCA NB Address Low (section 3.6.4.5) and MCA NB
 * Address High (section 3.6.4.6) register values and return the result. Address
 * is located in the info structure (nbeah and nbeal), the encoding is device
 * specific.
 */
static u64 extract_error_address(struct mem_ctl_info *mci,
                                struct err_regs *info)
{
       struct amd64_pvt *pvt = mci->pvt_info;

       return pvt->ops->get_error_address(mci, info);
}


/* Map the Error address to a PAGE and PAGE OFFSET. */
static inline void error_address_to_page_and_offset(u64 error_address,
                                                   u32 *page, u32 *offset)
{
       *page = (u32) (error_address >> PAGE_SHIFT);
       *offset = ((u32) error_address) & ~PAGE_MASK;
}

/*
 * @sys_addr is an error address (a SysAddr) extracted from the MCA NB Address
 * Low (section 3.6.4.5) and MCA NB Address High (section 3.6.4.6) registers
 * of a node that detected an ECC memory error.  mci represents the node that
 * the error address maps to (possibly different from the node that detected
 * the error).  Return the number of the csrow that sys_addr maps to, or -1 on
 * error.
 */
static int sys_addr_to_csrow(struct mem_ctl_info *mci, u64 sys_addr)
{
       int csrow;

       csrow = input_addr_to_csrow(mci, sys_addr_to_input_addr(mci, sys_addr));

       if (csrow == -1)
               amd64_mc_printk(mci, KERN_ERR,
                            "Failed to translate InputAddr to csrow for "
                            "address 0x%lx\n", (unsigned long)sys_addr);
       return csrow;
}

static int get_channel_from_ecc_syndrome(unsigned short syndrome);

static void amd64_cpu_display_info(struct amd64_pvt *pvt)
{
       if (boot_cpu_data.x86 == 0x11)
               edac_printk(KERN_DEBUG, EDAC_MC, "F11h CPU detected\n");
       else if (boot_cpu_data.x86 == 0x10)
               edac_printk(KERN_DEBUG, EDAC_MC, "F10h CPU detected\n");
       else if (boot_cpu_data.x86 == 0xf)
               edac_printk(KERN_DEBUG, EDAC_MC, "%s detected\n",
                       (pvt->ext_model >= K8_REV_F) ?
                       "Rev F or later" : "Rev E or earlier");
       else
               /* we'll hardly ever ever get here */
               edac_printk(KERN_ERR, EDAC_MC, "Unknown cpu!\n");
}

/*
 * Determine if the DIMMs have ECC enabled. ECC is enabled ONLY if all the DIMMs
 * are ECC capable.
 */
static enum edac_type amd64_determine_edac_cap(struct amd64_pvt *pvt)
{
       int bit;
       enum dev_type edac_cap = EDAC_FLAG_NONE;

       bit = (boot_cpu_data.x86 > 0xf || pvt->ext_model >= K8_REV_F)
               ? 19
               : 17;

       if (pvt->dclr0 & BIT(bit))
               edac_cap = EDAC_FLAG_SECDED;

       return edac_cap;
}


static void f10_debug_display_dimm_sizes(int ctrl, struct amd64_pvt *pvt,
                                        int ganged);

/* Display and decode various NB registers for debug purposes. */
static void amd64_dump_misc_regs(struct amd64_pvt *pvt)
{
       int ganged;

       debugf1("  nbcap:0x%8.08x DctDualCap=%s DualNode=%s 8-Node=%s\n",
               pvt->nbcap,
               (pvt->nbcap & K8_NBCAP_DCT_DUAL) ? "True" : "False",
               (pvt->nbcap & K8_NBCAP_DUAL_NODE) ? "True" : "False",
               (pvt->nbcap & K8_NBCAP_8_NODE) ? "True" : "False");
       debugf1("    ECC Capable=%s   ChipKill Capable=%s\n",
               (pvt->nbcap & K8_NBCAP_SECDED) ? "True" : "False",
               (pvt->nbcap & K8_NBCAP_CHIPKILL) ? "True" : "False");
       debugf1("  DramCfg0-low=0x%08x DIMM-ECC=%s Parity=%s Width=%s\n",
               pvt->dclr0,
               (pvt->dclr0 & BIT(19)) ?  "Enabled" : "Disabled",
               (pvt->dclr0 & BIT(8)) ?  "Enabled" : "Disabled",
               (pvt->dclr0 & BIT(11)) ?  "128b" : "64b");
       debugf1("    DIMM x4 Present: L0=%s L1=%s L2=%s L3=%s  DIMM Type=%s\n",
               (pvt->dclr0 & BIT(12)) ?  "Y" : "N",
               (pvt->dclr0 & BIT(13)) ?  "Y" : "N",
               (pvt->dclr0 & BIT(14)) ?  "Y" : "N",
               (pvt->dclr0 & BIT(15)) ?  "Y" : "N",
               (pvt->dclr0 & BIT(16)) ?  "UN-Buffered" : "Buffered");


       debugf1("  online-spare: 0x%8.08x\n", pvt->online_spare);

       if (boot_cpu_data.x86 == 0xf) {
               debugf1("  dhar: 0x%8.08x Base=0x%08x Offset=0x%08x\n",
                       pvt->dhar, dhar_base(pvt->dhar),
                       k8_dhar_offset(pvt->dhar));
               debugf1("      DramHoleValid=%s\n",
                       (pvt->dhar & DHAR_VALID) ?  "True" : "False");

               debugf1("  dbam-dkt: 0x%8.08x\n", pvt->dbam0);

               /* everything below this point is Fam10h and above */
               return;

       } else {
               debugf1("  dhar: 0x%8.08x Base=0x%08x Offset=0x%08x\n",
                       pvt->dhar, dhar_base(pvt->dhar),
                       f10_dhar_offset(pvt->dhar));
               debugf1("    DramMemHoistValid=%s DramHoleValid=%s\n",
                       (pvt->dhar & F10_DRAM_MEM_HOIST_VALID) ?
                       "True" : "False",
                       (pvt->dhar & DHAR_VALID) ?
                       "True" : "False");
       }

       /* Only if NOT ganged does dcl1 have valid info */
       if (!dct_ganging_enabled(pvt)) {
               debugf1("  DramCfg1-low=0x%08x DIMM-ECC=%s Parity=%s "
                       "Width=%s\n", pvt->dclr1,
                       (pvt->dclr1 & BIT(19)) ?  "Enabled" : "Disabled",
                       (pvt->dclr1 & BIT(8)) ?  "Enabled" : "Disabled",
                       (pvt->dclr1 & BIT(11)) ?  "128b" : "64b");
               debugf1("    DIMM x4 Present: L0=%s L1=%s L2=%s L3=%s  "
                       "DIMM Type=%s\n",
                       (pvt->dclr1 & BIT(12)) ?  "Y" : "N",
                       (pvt->dclr1 & BIT(13)) ?  "Y" : "N",
                       (pvt->dclr1 & BIT(14)) ?  "Y" : "N",
                       (pvt->dclr1 & BIT(15)) ?  "Y" : "N",
                       (pvt->dclr1 & BIT(16)) ?  "UN-Buffered" : "Buffered");
       }

       /*
        * Determine if ganged and then dump memory sizes for first controller,
        * and if NOT ganged dump info for 2nd controller.
        */
       ganged = dct_ganging_enabled(pvt);

       f10_debug_display_dimm_sizes(0, pvt, ganged);

       if (!ganged)
               f10_debug_display_dimm_sizes(1, pvt, ganged);
}

/* Read in both of DBAM registers */
static void amd64_read_dbam_reg(struct amd64_pvt *pvt)
{
       int err = 0;
       unsigned int reg;

       reg = DBAM0;
       err = pci_read_config_dword(pvt->dram_f2_ctl, reg, &pvt->dbam0);
       if (err)
               goto err_reg;

       if (boot_cpu_data.x86 >= 0x10) {
               reg = DBAM1;
               err = pci_read_config_dword(pvt->dram_f2_ctl, reg, &pvt->dbam1);

               if (err)
                       goto err_reg;
       }

       return;

err_reg:
       debugf0("Error reading F2x%03x.\n", reg);
}

/*
 * NOTE: CPU Revision Dependent code: Rev E and Rev F
 *
 * Set the DCSB and DCSM mask values depending on the CPU revision value. Also
 * set the shift factor for the DCSB and DCSM values.
 *
 * ->dcs_mask_notused, RevE:
 *
 * To find the max InputAddr for the csrow, start with the base address and set
 * all bits that are "don't care" bits in the test at the start of section
 * 3.5.4 (p. 84).
 *
 * The "don't care" bits are all set bits in the mask and all bits in the gaps
 * between bit ranges [35:25] and [19:13]. The value REV_E_DCS_NOTUSED_BITS
 * represents bits [24:20] and [12:0], which are all bits in the above-mentioned
 * gaps.
 *
 * ->dcs_mask_notused, RevF and later:
 *
 * To find the max InputAddr for the csrow, start with the base address and set
 * all bits that are "don't care" bits in the test at the start of NPT section
 * 4.5.4 (p. 87).
 *
 * The "don't care" bits are all set bits in the mask and all bits in the gaps
 * between bit ranges [36:27] and [21:13].
 *
 * The value REV_F_F1Xh_DCS_NOTUSED_BITS represents bits [26:22] and [12:0],
 * which are all bits in the above-mentioned gaps.
 */
static void amd64_set_dct_base_and_mask(struct amd64_pvt *pvt)
{

       if (boot_cpu_data.x86 == 0xf && pvt->ext_model < K8_REV_F) {
               pvt->dcsb_base          = REV_E_DCSB_BASE_BITS;
               pvt->dcsm_mask          = REV_E_DCSM_MASK_BITS;
               pvt->dcs_mask_notused   = REV_E_DCS_NOTUSED_BITS;
               pvt->dcs_shift          = REV_E_DCS_SHIFT;
               pvt->cs_count           = 8;
               pvt->num_dcsm           = 8;
       } else {
               pvt->dcsb_base          = REV_F_F1Xh_DCSB_BASE_BITS;
               pvt->dcsm_mask          = REV_F_F1Xh_DCSM_MASK_BITS;
               pvt->dcs_mask_notused   = REV_F_F1Xh_DCS_NOTUSED_BITS;
               pvt->dcs_shift          = REV_F_F1Xh_DCS_SHIFT;

               if (boot_cpu_data.x86 == 0x11) {
                       pvt->cs_count = 4;
                       pvt->num_dcsm = 2;
               } else {
                       pvt->cs_count = 8;
                       pvt->num_dcsm = 4;
               }
       }
}

/*
 * Function 2 Offset F10_DCSB0; read in the DCS Base and DCS Mask hw registers
 */
static void amd64_read_dct_base_mask(struct amd64_pvt *pvt)
{
       int cs, reg, err = 0;

       amd64_set_dct_base_and_mask(pvt);

       for (cs = 0; cs < pvt->cs_count; cs++) {
               reg = K8_DCSB0 + (cs * 4);
               err = pci_read_config_dword(pvt->dram_f2_ctl, reg,
                                               &pvt->dcsb0[cs]);
               if (unlikely(err))
                       debugf0("Reading K8_DCSB0[%d] failed\n", cs);
               else
                       debugf0("  DCSB0[%d]=0x%08x reg: F2x%x\n",
                               cs, pvt->dcsb0[cs], reg);

               /* If DCT are NOT ganged, then read in DCT1's base */
               if (boot_cpu_data.x86 >= 0x10 && !dct_ganging_enabled(pvt)) {
                       reg = F10_DCSB1 + (cs * 4);
                       err = pci_read_config_dword(pvt->dram_f2_ctl, reg,
                                                       &pvt->dcsb1[cs]);
                       if (unlikely(err))
                               debugf0("Reading F10_DCSB1[%d] failed\n", cs);
                       else
                               debugf0("  DCSB1[%d]=0x%08x reg: F2x%x\n",
                                       cs, pvt->dcsb1[cs], reg);
               } else {
                       pvt->dcsb1[cs] = 0;
               }
       }

       for (cs = 0; cs < pvt->num_dcsm; cs++) {
               reg = K8_DCSM0 + (cs * 4);
               err = pci_read_config_dword(pvt->dram_f2_ctl, reg,
                                       &pvt->dcsm0[cs]);
               if (unlikely(err))
                       debugf0("Reading K8_DCSM0 failed\n");
               else
                       debugf0("    DCSM0[%d]=0x%08x reg: F2x%x\n",
                               cs, pvt->dcsm0[cs], reg);

               /* If DCT are NOT ganged, then read in DCT1's mask */
               if (boot_cpu_data.x86 >= 0x10 && !dct_ganging_enabled(pvt)) {
                       reg = F10_DCSM1 + (cs * 4);
                       err = pci_read_config_dword(pvt->dram_f2_ctl, reg,
                                       &pvt->dcsm1[cs]);
                       if (unlikely(err))
                               debugf0("Reading F10_DCSM1[%d] failed\n", cs);
                       else
                               debugf0("    DCSM1[%d]=0x%08x reg: F2x%x\n",
                                       cs, pvt->dcsm1[cs], reg);
               } else
                       pvt->dcsm1[cs] = 0;
       }
}

static enum mem_type amd64_determine_memory_type(struct amd64_pvt *pvt)
{
       enum mem_type type;

       if (boot_cpu_data.x86 >= 0x10 || pvt->ext_model >= K8_REV_F) {
		if (pvt->dchr0 & DDR3_MODE)
			type = (pvt->dclr0 & BIT(16)) ?	MEM_DDR3 : MEM_RDDR3;
		else
			type = (pvt->dclr0 & BIT(16)) ? MEM_DDR2 : MEM_RDDR2;
       } else {
               type = (pvt->dclr0 & BIT(18)) ? MEM_DDR : MEM_RDDR;
       }

       debugf1("  Memory type is: %s\n",
		(type == MEM_DDR3) ? "MEM_DDR3" :
		(type == MEM_RDDR3) ? "MEM_RDDR3" :
               (type == MEM_DDR2) ? "MEM_DDR2" :
               (type == MEM_RDDR2) ? "MEM_RDDR2" :
               (type == MEM_DDR) ? "MEM_DDR" : "MEM_RDDR");

       return type;
}

/*
 * Get the number of DCT channels in use.
 *
 * Return:
 *     number of Memory Channels in operation
 * Pass back:
 *     contents of the DCL0_LOW register
 */
static int f10_early_channel_count(struct amd64_pvt *pvt)
{
       int dbams[] = { DBAM0, DBAM1 };
       int err = 0, channels = 0;
       int i, j;
       u32 dbam;

       err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCLR_0, &pvt->dclr0);
       if (err)
               goto err_reg;

       err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCLR_1, &pvt->dclr1);
       if (err)
               goto err_reg;

       /* If we are in 128 bit mode, then we are using 2 channels */
       if (pvt->dclr0 & F10_WIDTH_128) {
               debugf0("Data WIDTH is 128 bits - 2 channels\n");
               channels = 2;
               return channels;
       }

       /*
        * Need to check if in UN-ganged mode: In such, there are 2 channels,
        * but they are NOT in 128 bit mode and thus the above 'dcl0' status bit
        * will be OFF.
        *
        * Need to check DCT0[0] and DCT1[0] to see if only one of them has
        * their CSEnable bit on. If so, then SINGLE DIMM case.
        */
       debugf0("Data WIDTH is NOT 128 bits - need more decoding\n");

       /*
        * Check DRAM Bank Address Mapping values for each DIMM to see if there
        * is more than just one DIMM present in unganged mode. Need to check
        * both controllers since DIMMs can be placed in either one.
        */
       for (i = 0; i < ARRAY_SIZE(dbams); i++) {
               err = pci_read_config_dword(pvt->dram_f2_ctl, dbams[i], &dbam);
               if (err)
                       goto err_reg;

               for (j = 0; j < 4; j++) {
                       if (DBAM_DIMM(j, dbam) > 0) {
                               channels++;
                               break;
                       }
               }
       }

       debugf0("MCT channel count: %d\n", channels);

       return channels;

err_reg:
       return -1;

}

static int f10_dbam_to_chip_select(struct amd64_pvt *pvt, int cs_mode)
{
	int *dbam_map;

	if (pvt->dchr0 & DDR3_MODE || pvt->dchr1 & DDR3_MODE)
		dbam_map = ddr3_dbam;
	else
		dbam_map = ddr2_dbam;

	return dbam_map[cs_mode];
}

/* Enable extended configuration access via 0xCF8 feature */
static void amd64_setup(struct amd64_pvt *pvt)
{
       u32 reg;

       pci_read_config_dword(pvt->misc_f3_ctl, F10_NB_CFG_HIGH, &reg);

       pvt->flags.cf8_extcfg = !!(reg & F10_NB_CFG_LOW_ENABLE_EXT_CFG);
       reg |= F10_NB_CFG_LOW_ENABLE_EXT_CFG;
       pci_write_config_dword(pvt->misc_f3_ctl, F10_NB_CFG_HIGH, reg);
}

/* Restore the extended configuration access via 0xCF8 feature */
static void amd64_teardown(struct amd64_pvt *pvt)
{
       u32 reg;

       pci_read_config_dword(pvt->misc_f3_ctl, F10_NB_CFG_HIGH, &reg);

       reg &= ~F10_NB_CFG_LOW_ENABLE_EXT_CFG;
       if (pvt->flags.cf8_extcfg)
               reg |= F10_NB_CFG_LOW_ENABLE_EXT_CFG;
       pci_write_config_dword(pvt->misc_f3_ctl, F10_NB_CFG_HIGH, reg);
}

static u64 f10_get_error_address(struct mem_ctl_info *mci,
                       struct err_regs *info)
{
       return (((u64) (info->nbeah & 0xffff)) << 32) +
                       (info->nbeal & ~0x01);
}

/*
 * Read the Base and Limit registers for F10 based Memory controllers. Extract
 * fields from the 'raw' reg into separate data fields.
 *
 * Isolates: BASE, LIMIT, IntlvEn, IntlvSel, RW_EN.
 */
static void f10_read_dram_base_limit(struct amd64_pvt *pvt, int dram)
{
       u32 high_offset, low_offset, high_base, low_base, high_limit, low_limit;

       low_offset = K8_DRAM_BASE_LOW + (dram << 3);
       high_offset = F10_DRAM_BASE_HIGH + (dram << 3);

       /* read the 'raw' DRAM BASE Address register */
       pci_read_config_dword(pvt->addr_f1_ctl, low_offset, &low_base);

       /* Read from the ECS data register */
       pci_read_config_dword(pvt->addr_f1_ctl, high_offset, &high_base);

       /* Extract parts into separate data entries */
       pvt->dram_rw_en[dram] = (low_base & 0x3);

       if (pvt->dram_rw_en[dram] == 0)
               return;

       pvt->dram_IntlvEn[dram] = (low_base >> 8) & 0x7;

       pvt->dram_base[dram] = (((u64)high_base & 0x000000FF) << 40) |
                              (((u64)low_base  & 0xFFFF0000) << 8);

       low_offset = K8_DRAM_LIMIT_LOW + (dram << 3);
       high_offset = F10_DRAM_LIMIT_HIGH + (dram << 3);

       /* read the 'raw' LIMIT registers */
       pci_read_config_dword(pvt->addr_f1_ctl, low_offset, &low_limit);

       /* Read from the ECS data register for the HIGH portion */
       pci_read_config_dword(pvt->addr_f1_ctl, high_offset, &high_limit);

       debugf0("  HW Regs: BASE=0x%08x-%08x      LIMIT=  0x%08x-%08x\n",
               high_base, low_base, high_limit, low_limit);

       pvt->dram_DstNode[dram] = (low_limit & 0x7);
       pvt->dram_IntlvSel[dram] = (low_limit >> 8) & 0x7;

       /*
        * Extract address values and form a LIMIT address. Limit is the HIGHEST
        * memory location of the region, so low 24 bits need to be all ones.
        */
       pvt->dram_limit[dram] = (((u64)high_limit & 0x000000FF) << 40) |
                               (((u64) low_limit & 0xFFFF0000) << 8) |
                               0x00FFFFFF;
}

static void f10_read_dram_ctl_register(struct amd64_pvt *pvt)
{
       int err = 0;

       err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCTL_SEL_LOW,
                                   &pvt->dram_ctl_select_low);
       if (err) {
               debugf0("Reading F10_DCTL_SEL_LOW failed\n");
       } else {
               debugf0("DRAM_DCTL_SEL_LOW=0x%x  DctSelBaseAddr=0x%x\n",
                       pvt->dram_ctl_select_low, dct_sel_baseaddr(pvt));

               debugf0("  DRAM DCTs are=%s DRAM Is=%s DRAM-Ctl-"
                               "sel-hi-range=%s\n",
                       (dct_ganging_enabled(pvt) ? "GANGED" : "NOT GANGED"),
                       (dct_dram_enabled(pvt) ? "Enabled"   : "Disabled"),
                       (dct_high_range_enabled(pvt) ? "Enabled" : "Disabled"));

               debugf0("  DctDatIntLv=%s MemCleared=%s DctSelIntLvAddr=0x%x\n",
                       (dct_data_intlv_enabled(pvt) ? "Enabled" : "Disabled"),
                       (dct_memory_cleared(pvt) ? "True " : "False "),
                       dct_sel_interleave_addr(pvt));
       }

       err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCTL_SEL_HIGH,
                                   &pvt->dram_ctl_select_high);
       if (err)
               debugf0("Reading F10_DCTL_SEL_HIGH failed\n");
}

/*
 * determine channel based on the interleaving mode: F10h BKDG, 2.8.9 Memory
 * Interleaving Modes.
 */
static u32 f10_determine_channel(struct amd64_pvt *pvt, u64 sys_addr,
                               int hi_range_sel, u32 intlv_en)
{
       u32 cs, temp, dct_sel_high = (pvt->dram_ctl_select_low >> 1) & 1;

       if (dct_ganging_enabled(pvt))
               cs = 0;
       else if (hi_range_sel)
               cs = dct_sel_high;
       else if (dct_interleave_enabled(pvt)) {
               /*
                * see F2x110[DctSelIntLvAddr] - channel interleave mode
                */
               if (dct_sel_interleave_addr(pvt) == 0)
                       cs = sys_addr >> 6 & 1;
               else if ((dct_sel_interleave_addr(pvt) >> 1) & 1) {
                       temp = hweight_long((u32) ((sys_addr >> 16) & 0x1F)) % 2;

                       if (dct_sel_interleave_addr(pvt) & 1)
                               cs = (sys_addr >> 9 & 1) ^ temp;
                       else
                               cs = (sys_addr >> 6 & 1) ^ temp;
               } else if (intlv_en & 4)
                       cs = sys_addr >> 15 & 1;
               else if (intlv_en & 2)
                       cs = sys_addr >> 14 & 1;
               else if (intlv_en & 1)
                       cs = sys_addr >> 13 & 1;
               else
                       cs = sys_addr >> 12 & 1;
       } else if (dct_high_range_enabled(pvt) && !dct_ganging_enabled(pvt))
               cs = ~dct_sel_high & 1;
       else
               cs = 0;

       return cs;
}

static inline u32 f10_map_intlv_en_to_shift(u32 intlv_en)
{
       if (intlv_en == 1)
               return 1;
       else if (intlv_en == 3)
               return 2;
       else if (intlv_en == 7)
               return 3;

       return 0;
}

/* See F10h BKDG, 2.8.10.2 DctSelBaseOffset Programming */
static inline u64 f10_get_base_addr_offset(u64 sys_addr, int hi_range_sel,
                                                u32 dct_sel_base_addr,
                                                u64 dct_sel_base_off,
                                                u32 hole_valid, u32 hole_off,
                                                u64 dram_base)
{
       u64 chan_off;

       if (hi_range_sel) {
               if (!(dct_sel_base_addr & 0xFFFFF800) &&
                  hole_valid && (sys_addr >= 0x100000000ULL))
                       chan_off = hole_off << 16;
               else
                       chan_off = dct_sel_base_off;
       } else {
               if (hole_valid && (sys_addr >= 0x100000000ULL))
                       chan_off = hole_off << 16;
               else
                       chan_off = dram_base & 0xFFFFF8000000ULL;
       }

       return (sys_addr & 0x0000FFFFFFFFFFC0ULL) -
                       (chan_off & 0x0000FFFFFF800000ULL);
}

/* Hack for the time being - Can we get this from BIOS?? */
#define        CH0SPARE_RANK   0
#define        CH1SPARE_RANK   1

/*
 * checks if the csrow passed in is marked as SPARED, if so returns the new
 * spare row
 */
static inline int f10_process_possible_spare(int csrow,
                               u32 cs, struct amd64_pvt *pvt)
{
       u32 swap_done;
       u32 bad_dram_cs;

       /* Depending on channel, isolate respective SPARING info */
       if (cs) {
               swap_done = F10_ONLINE_SPARE_SWAPDONE1(pvt->online_spare);
               bad_dram_cs = F10_ONLINE_SPARE_BADDRAM_CS1(pvt->online_spare);
               if (swap_done && (csrow == bad_dram_cs))
                       csrow = CH1SPARE_RANK;
       } else {
               swap_done = F10_ONLINE_SPARE_SWAPDONE0(pvt->online_spare);
               bad_dram_cs = F10_ONLINE_SPARE_BADDRAM_CS0(pvt->online_spare);
               if (swap_done && (csrow == bad_dram_cs))
                       csrow = CH0SPARE_RANK;
       }
       return csrow;
}

/*
 * Iterate over the DRAM DCT "base" and "mask" registers looking for a
 * SystemAddr match on the specified 'ChannelSelect' and 'NodeID'
 *
 * Return:
 *     -EINVAL:  NOT FOUND
 *     0..csrow = Chip-Select Row
 */
static int f10_lookup_addr_in_dct(u32 in_addr, u32 nid, u32 cs)
{
       struct mem_ctl_info *mci;
       struct amd64_pvt *pvt;
       u32 cs_base, cs_mask;
       int cs_found = -EINVAL;
       int csrow;

       mci = mci_lookup[nid];
       if (!mci)
               return cs_found;

       pvt = mci->pvt_info;

       debugf1("InputAddr=0x%x  channelselect=%d\n", in_addr, cs);

       for (csrow = 0; csrow < pvt->cs_count; csrow++) {

               cs_base = amd64_get_dct_base(pvt, cs, csrow);
               if (!(cs_base & K8_DCSB_CS_ENABLE))
                       continue;

               /*
                * We have an ENABLED CSROW, Isolate just the MASK bits of the
                * target: [28:19] and [13:5], which map to [36:27] and [21:13]
                * of the actual address.
                */
               cs_base &= REV_F_F1Xh_DCSB_BASE_BITS;

               /*
                * Get the DCT Mask, and ENABLE the reserved bits: [18:16] and
                * [4:0] to become ON. Then mask off bits [28:0] ([36:8])
                */
               cs_mask = amd64_get_dct_mask(pvt, cs, csrow);

               debugf1("    CSROW=%d CSBase=0x%x RAW CSMask=0x%x\n",
                               csrow, cs_base, cs_mask);

               cs_mask = (cs_mask | 0x0007C01F) & 0x1FFFFFFF;

               debugf1("              Final CSMask=0x%x\n", cs_mask);
               debugf1("    (InputAddr & ~CSMask)=0x%x "
                               "(CSBase & ~CSMask)=0x%x\n",
                               (in_addr & ~cs_mask), (cs_base & ~cs_mask));

               if ((in_addr & ~cs_mask) == (cs_base & ~cs_mask)) {
                       cs_found = f10_process_possible_spare(csrow, cs, pvt);

                       debugf1(" MATCH csrow=%d\n", cs_found);
                       break;
               }
       }
       return cs_found;
}

/* For a given @dram_range, check if @sys_addr falls within it. */
static int f10_match_to_this_node(struct amd64_pvt *pvt, int dram_range,
                                 u64 sys_addr, int *nid, int *chan_sel)
{
       int node_id, cs_found = -EINVAL, high_range = 0;
       u32 intlv_en, intlv_sel, intlv_shift, hole_off;
       u32 hole_valid, tmp, dct_sel_base, channel;
       u64 dram_base, chan_addr, dct_sel_base_off;

       dram_base = pvt->dram_base[dram_range];
       intlv_en = pvt->dram_IntlvEn[dram_range];

       node_id = pvt->dram_DstNode[dram_range];
       intlv_sel = pvt->dram_IntlvSel[dram_range];

       debugf1("(dram=%d) Base=0x%llx SystemAddr= 0x%llx Limit=0x%llx\n",
               dram_range, dram_base, sys_addr, pvt->dram_limit[dram_range]);

       /*
        * This assumes that one node's DHAR is the same as all the other
        * nodes' DHAR.
        */
       hole_off = (pvt->dhar & 0x0000FF80);
       hole_valid = (pvt->dhar & 0x1);
       dct_sel_base_off = (pvt->dram_ctl_select_high & 0xFFFFFC00) << 16;

       debugf1("   HoleOffset=0x%x  HoleValid=0x%x IntlvSel=0x%x\n",
                       hole_off, hole_valid, intlv_sel);

       if (intlv_en ||
           (intlv_sel != ((sys_addr >> 12) & intlv_en)))
               return -EINVAL;

       dct_sel_base = dct_sel_baseaddr(pvt);

       /*
        * check whether addresses >= DctSelBaseAddr[47:27] are to be used to
        * select between DCT0 and DCT1.
        */
       if (dct_high_range_enabled(pvt) &&
          !dct_ganging_enabled(pvt) &&
          ((sys_addr >> 27) >= (dct_sel_base >> 11)))
               high_range = 1;

       channel = f10_determine_channel(pvt, sys_addr, high_range, intlv_en);

       chan_addr = f10_get_base_addr_offset(sys_addr, high_range, dct_sel_base,
                                            dct_sel_base_off, hole_valid,
                                            hole_off, dram_base);

       intlv_shift = f10_map_intlv_en_to_shift(intlv_en);

       /* remove Node ID (in case of memory interleaving) */
       tmp = chan_addr & 0xFC0;

       chan_addr = ((chan_addr >> intlv_shift) & 0xFFFFFFFFF000ULL) | tmp;

       /* remove channel interleave and hash */
       if (dct_interleave_enabled(pvt) &&
          !dct_high_range_enabled(pvt) &&
          !dct_ganging_enabled(pvt)) {
               if (dct_sel_interleave_addr(pvt) != 1)
                       chan_addr = (chan_addr >> 1) & 0xFFFFFFFFFFFFFFC0ULL;
               else {
                       tmp = chan_addr & 0xFC0;
                       chan_addr = ((chan_addr & 0xFFFFFFFFFFFFC000ULL) >> 1)
                                       | tmp;
               }
       }

       debugf1("   (ChannelAddrLong=0x%llx) >> 8 becomes InputAddr=0x%x\n",
               chan_addr, (u32)(chan_addr >> 8));

       cs_found = f10_lookup_addr_in_dct(chan_addr >> 8, node_id, channel);

       if (cs_found >= 0) {
               *nid = node_id;
               *chan_sel = channel;
       }
       return cs_found;
}

static int f10_translate_sysaddr_to_cs(struct amd64_pvt *pvt, u64 sys_addr,
                                      int *node, int *chan_sel)
{
       int dram_range, cs_found = -EINVAL;
       u64 dram_base, dram_limit;

       for (dram_range = 0; dram_range < DRAM_REG_COUNT; dram_range++) {

               if (!pvt->dram_rw_en[dram_range])
                       continue;

               dram_base = pvt->dram_base[dram_range];
               dram_limit = pvt->dram_limit[dram_range];

               if ((dram_base <= sys_addr) && (sys_addr <= dram_limit)) {

                       cs_found = f10_match_to_this_node(pvt, dram_range,
                                                         sys_addr, node,
                                                         chan_sel);
                       if (cs_found >= 0)
                               break;
               }
       }
       return cs_found;
}

/*
 * For reference see "2.8.5 Routing DRAM Requests" in F10 BKDG. This code maps
 * a @sys_addr to NodeID, DCT (channel) and chip select (CSROW).
 *
 * The @sys_addr is usually an error address received from the hardware
 * (MCX_ADDR).
 */
static void f10_map_sysaddr_to_csrow(struct mem_ctl_info *mci,
                                    struct err_regs *info,
                                    u64 sys_addr)
{
       struct amd64_pvt *pvt = mci->pvt_info;
       u32 page, offset;
       unsigned short syndrome;
       int nid, csrow, chan = 0;

       csrow = f10_translate_sysaddr_to_cs(pvt, sys_addr, &nid, &chan);

       if (csrow < 0) {
		edac_mc_handle_ce_no_info(mci, EDAC_MOD_STR);
		return;
	}

	error_address_to_page_and_offset(sys_addr, &page, &offset);

	syndrome  = HIGH_SYNDROME(info->nbsl) << 8;
	syndrome |= LOW_SYNDROME(info->nbsh);

	/*
	 * We need the syndromes for channel detection only when we're
	 * ganged. Otherwise @chan should already contain the channel at
	 * this point.
	 */
	if (dct_ganging_enabled(pvt) && pvt->nbcfg & K8_NBCFG_CHIPKILL)
		chan = get_channel_from_ecc_syndrome(syndrome);

	if (chan >= 0)
		edac_mc_handle_ce(mci, page, offset, syndrome, csrow, chan,
				  EDAC_MOD_STR);
	else
		/*
		 * Channel unknown, report all channels on this CSROW as failed.
		 */
		for (chan = 0; chan < mci->csrows[csrow].nr_channels; chan++)
			edac_mc_handle_ce(mci, page, offset, syndrome, csrow,
					  chan, EDAC_MOD_STR); 
}

/*
 * debug routine to display the memory sizes of all logical DIMMs and its 
 * CSROWs as well
 */
static void f10_debug_display_dimm_sizes(int ctrl, struct amd64_pvt *pvt,
                                        int ganged)
{
       int dimm, size0, size1;
       u32 dbam;
       u32 *dcsb;

	if (boot_cpu_data.x86 == 0xf) {
		/* K8 families < revF not supported yet */
	       if (pvt->ext_model < K8_REV_F)
			return;
	       else
		       WARN_ON(ctrl != 0);
	}

	debugf1("F2x%d80 (DRAM Bank Address Mapping): 0x%08x\n",
		ctrl, ctrl ? pvt->dbam1 : pvt->dbam0);

       dbam = ctrl ? pvt->dbam1 : pvt->dbam0;
       dcsb = ctrl ? pvt->dcsb1 : pvt->dcsb0;

       /* Dump memory sizes for DIMM and its CSROWs */
       for (dimm = 0; dimm < 4; dimm++) {

               size0 = 0;
               if (dcsb[dimm*2] & K8_DCSB_CS_ENABLE)
			size0 = pvt->ops->dbam_to_cs(pvt, DBAM_DIMM(dimm, dbam));

               size1 = 0;
               if (dcsb[dimm*2 + 1] & K8_DCSB_CS_ENABLE)
			size1 = pvt->ops->dbam_to_cs(pvt, DBAM_DIMM(dimm, dbam));

		debugf1("     CTRL-%d CS%d=%5dMB CS%d=%5dMB\n",
                               ctrl,
                               dimm * 2,
                               size0,
                               dimm * 2 + 1,
                               size1);
       }
}

/*
 * There currently are 3 types type of MC devices for AMD Athlon/Opterons
 * (as per PCI DEVICE_IDs):
 *
 * Family K8: That is the Athlon64 and Opteron CPUs. They all have the same PCI
 * DEVICE ID, even though there is differences between the different Revisions
 * (CG,D,E,F).
 *
 * Family F10h and F11h.
 *
 */
static struct amd64_family_type amd64_family_types[] = {
       [F10_CPUS] = {
               .ctl_name = "Family 10h",
               .addr_f1_ctl = PCI_DEVICE_ID_AMD_10H_NB_MAP,
               .misc_f3_ctl = PCI_DEVICE_ID_AMD_10H_NB_MISC,
               .ops = {
			.early_channel_count	= f10_early_channel_count,
			.get_error_address	= f10_get_error_address,
			.read_dram_base_limit	= f10_read_dram_base_limit,
			.read_dram_ctl_register	= f10_read_dram_ctl_register,
			.map_sysaddr_to_csrow	= f10_map_sysaddr_to_csrow,
			.dbam_to_cs		= f10_dbam_to_chip_select,
               }
       },
       [F11_CPUS] = {
               .ctl_name = "Family 11h",
               .addr_f1_ctl = PCI_DEVICE_ID_AMD_11H_NB_MAP,
               .misc_f3_ctl = PCI_DEVICE_ID_AMD_11H_NB_MISC,
               .ops = {
			.early_channel_count	= f10_early_channel_count,
			.get_error_address	= f10_get_error_address,
			.read_dram_base_limit	= f10_read_dram_base_limit,
			.read_dram_ctl_register	= f10_read_dram_ctl_register,
			.map_sysaddr_to_csrow	= f10_map_sysaddr_to_csrow,
			.dbam_to_cs		= f10_dbam_to_chip_select,
               }
       },
};

static struct pci_dev *pci_get_related_function(unsigned int vendor,
                                               unsigned int device,
                                               struct pci_dev *related)
{
       struct pci_dev *dev = NULL;

       dev = pci_get_device(vendor, device, dev);
       while (dev) {
               if ((dev->bus->number == related->bus->number) &&
                   (PCI_SLOT(dev->devfn) == PCI_SLOT(related->devfn)))
                       break;
               dev = pci_get_device(vendor, device, dev);
       }

       return dev;
}

/*
 * syndrome mapping table for ECC ChipKill devices
 *
 * The comment in each row is the token (nibble) number that is in error.
 * The least significant nibble of the syndrome is the mask for the bits
 * that are in error (need to be toggled) for the particular nibble.
 *
 * Each row contains 16 entries.
 * The first entry (0th) is the channel number for that row of syndromes.
 * The remaining 15 entries are the syndromes for the respective Error
 * bit mask index.
 *
 * 1st index entry is 0x0001 mask, indicating that the rightmost bit is the
 * bit in error.
 * The 2nd index entry is 0x0010 that the second bit is damaged.
 * The 3rd index entry is 0x0011 indicating that the rightmost 2 bits
 * are damaged.
 * Thus so on until index 15, 0x1111, whose entry has the syndrome
 * indicating that all 4 bits are damaged.
 *
 * A search is performed on this table looking for a given syndrome.
 *
 * See the AMD documentation for ECC syndromes. This ECC table is valid
 * across all the versions of the AMD64 processors.
 *
 * A fast lookup is to use the LAST four bits of the 16-bit syndrome as a
 * COLUMN index, then search all ROWS of that column, looking for a match
 * with the input syndrome. The ROW value will be the token number.
 *
 * The 0'th entry on that row, can be returned as the CHANNEL (0 or 1) of this
 * error.
 */
#define NUMBER_ECC_ROWS  36
static const unsigned short ecc_chipkill_syndromes[NUMBER_ECC_ROWS][16] = {
       /* Channel 0 syndromes */
       {/*0*/  0, 0xe821, 0x7c32, 0x9413, 0xbb44, 0x5365, 0xc776, 0x2f57,
          0xdd88, 0x35a9, 0xa1ba, 0x499b, 0x66cc, 0x8eed, 0x1afe, 0xf2df },
       {/*1*/  0, 0x5d31, 0xa612, 0xfb23, 0x9584, 0xc8b5, 0x3396, 0x6ea7,
          0xeac8, 0xb7f9, 0x4cda, 0x11eb, 0x7f4c, 0x227d, 0xd95e, 0x846f },
       {/*2*/  0, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
          0x0008, 0x0009, 0x000a, 0x000b, 0x000c, 0x000d, 0x000e, 0x000f },
       {/*3*/  0, 0x2021, 0x3032, 0x1013, 0x4044, 0x6065, 0x7076, 0x5057,
          0x8088, 0xa0a9, 0xb0ba, 0x909b, 0xc0cc, 0xe0ed, 0xf0fe, 0xd0df },
       {/*4*/  0, 0x5041, 0xa082, 0xf0c3, 0x9054, 0xc015, 0x30d6, 0x6097,
          0xe0a8, 0xb0e9, 0x402a, 0x106b, 0x70fc, 0x20bd, 0xd07e, 0x803f },
       {/*5*/  0, 0xbe21, 0xd732, 0x6913, 0x2144, 0x9f65, 0xf676, 0x4857,
          0x3288, 0x8ca9, 0xe5ba, 0x5b9b, 0x13cc, 0xaded, 0xc4fe, 0x7adf },
       {/*6*/  0, 0x4951, 0x8ea2, 0xc7f3, 0x5394, 0x1ac5, 0xdd36, 0x9467,
          0xa1e8, 0xe8b9, 0x2f4a, 0x661b, 0xf27c, 0xbb2d, 0x7cde, 0x358f },
       {/*7*/  0, 0x74e1, 0x9872, 0xec93, 0xd6b4, 0xa255, 0x4ec6, 0x3a27,
          0x6bd8, 0x1f39, 0xf3aa, 0x874b, 0xbd6c, 0xc98d, 0x251e, 0x51ff },
       {/*8*/  0, 0x15c1, 0x2a42, 0x3f83, 0xcef4, 0xdb35, 0xe4b6, 0xf177,
          0x4758, 0x5299, 0x6d1a, 0x78db, 0x89ac, 0x9c6d, 0xa3ee, 0xb62f },
       {/*9*/  0, 0x3d01, 0x1602, 0x2b03, 0x8504, 0xb805, 0x9306, 0xae07,
          0xca08, 0xf709, 0xdc0a, 0xe10b, 0x4f0c, 0x720d, 0x590e, 0x640f },
       {/*a*/  0, 0x9801, 0xec02, 0x7403, 0x6b04, 0xf305, 0x8706, 0x1f07,
          0xbd08, 0x2509, 0x510a, 0xc90b, 0xd60c, 0x4e0d, 0x3a0e, 0xa20f },
       {/*b*/  0, 0xd131, 0x6212, 0xb323, 0x3884, 0xe9b5, 0x5a96, 0x8ba7,
          0x1cc8, 0xcdf9, 0x7eda, 0xafeb, 0x244c, 0xf57d, 0x465e, 0x976f },
       {/*c*/  0, 0xe1d1, 0x7262, 0x93b3, 0xb834, 0x59e5, 0xca56, 0x2b87,
          0xdc18, 0x3dc9, 0xae7a, 0x4fab, 0x542c, 0x85fd, 0x164e, 0xf79f },
       {/*d*/  0, 0x6051, 0xb0a2, 0xd0f3, 0x1094, 0x70c5, 0xa036, 0xc067,
          0x20e8, 0x40b9, 0x904a, 0x601b, 0x307c, 0x502d, 0x80de, 0xe08f },
       {/*e*/  0, 0xa4c1, 0xf842, 0x5c83, 0xe6f4, 0x4235, 0x1eb6, 0xba77,
          0x7b58, 0xdf99, 0x831a, 0x27db, 0x9dac, 0x396d, 0x65ee, 0xc12f },
       {/*f*/  0, 0x11c1, 0x2242, 0x3383, 0xc8f4, 0xd935, 0xeab6, 0xfb77,
          0x4c58, 0x5d99, 0x6e1a, 0x7fdb, 0x84ac, 0x956d, 0xa6ee, 0xb72f },

       /* Channel 1 syndromes */
       {/*10*/ 1, 0x45d1, 0x8a62, 0xcfb3, 0x5e34, 0x1be5, 0xd456, 0x9187,
          0xa718, 0xe2c9, 0x2d7a, 0x68ab, 0xf92c, 0xbcfd, 0x734e, 0x369f },
       {/*11*/ 1, 0x63e1, 0xb172, 0xd293, 0x14b4, 0x7755, 0xa5c6, 0xc627,
          0x28d8, 0x4b39, 0x99aa, 0xfa4b, 0x3c6c, 0x5f8d, 0x8d1e, 0xeeff },
       {/*12*/ 1, 0xb741, 0xd982, 0x6ec3, 0x2254, 0x9515, 0xfbd6, 0x4c97,
          0x33a8, 0x84e9, 0xea2a, 0x5d6b, 0x11fc, 0xa6bd, 0xc87e, 0x7f3f },
       {/*13*/ 1, 0xdd41, 0x6682, 0xbbc3, 0x3554, 0xe815, 0x53d6, 0xce97,
          0x1aa8, 0xc7e9, 0x7c2a, 0xa1fb, 0x2ffc, 0xf2bd, 0x497e, 0x943f },
       {/*14*/ 1, 0x2bd1, 0x3d62, 0x16b3, 0x4f34, 0x64e5, 0x7256, 0x5987,
          0x8518, 0xaec9, 0xb87a, 0x93ab, 0xca2c, 0xe1fd, 0xf74e, 0xdc9f },
       {/*15*/ 1, 0x83c1, 0xc142, 0x4283, 0xa4f4, 0x2735, 0x65b6, 0xe677,
          0xf858, 0x7b99, 0x391a, 0xbadb, 0x5cac, 0xdf6d, 0x9dee, 0x1e2f },
       {/*16*/ 1, 0x8fd1, 0xc562, 0x4ab3, 0xa934, 0x26e5, 0x6c56, 0xe387,
          0xfe18, 0x71c9, 0x3b7a, 0xb4ab, 0x572c, 0xd8fd, 0x924e, 0x1d9f },
       {/*17*/ 1, 0x4791, 0x89e2, 0xce73, 0x5264, 0x15f5, 0xdb86, 0x9c17,
          0xa3b8, 0xe429, 0x2a5a, 0x6dcb, 0xf1dc, 0xb64d, 0x783e, 0x3faf },
       {/*18*/ 1, 0x5781, 0xa9c2, 0xfe43, 0x92a4, 0xc525, 0x3b66, 0x6ce7,
          0xe3f8, 0xb479, 0x4a3a, 0x1dbb, 0x715c, 0x26dd, 0xd89e, 0x8f1f },
       {/*19*/ 1, 0xbf41, 0xd582, 0x6ac3, 0x2954, 0x9615, 0xfcd6, 0x4397,
          0x3ea8, 0x81e9, 0xeb2a, 0x546b, 0x17fc, 0xa8bd, 0xc27e, 0x7d3f },
       {/*1a*/ 1, 0x9891, 0xe1e2, 0x7273, 0x6464, 0xf7f5, 0x8586, 0x1617,
          0xb8b8, 0x2b29, 0x595a, 0xcacb, 0xdcdc, 0x4f4d, 0x3d3e, 0xaeaf },
       {/*1b*/ 1, 0xcce1, 0x4472, 0x8893, 0xfdb4, 0x3f55, 0xb9c6, 0x7527,
          0x56d8, 0x9a39, 0x12aa, 0xde4b, 0xab6c, 0x678d, 0xef1e, 0x23ff },
       {/*1c*/ 1, 0xa761, 0xf9b2, 0x5ed3, 0xe214, 0x4575, 0x1ba6, 0xbcc7,
          0x7328, 0xd449, 0x8a9a, 0x2dfb, 0x913c, 0x365d, 0x688e, 0xcfef },
       {/*1d*/ 1, 0xff61, 0x55b2, 0xaad3, 0x7914, 0x8675, 0x2ca6, 0xd3c7,
          0x9e28, 0x6149, 0xcb9a, 0x34fb, 0xe73c, 0x185d, 0xb28e, 0x4def },
       {/*1e*/ 1, 0x5451, 0xa8a2, 0xfcf3, 0x9694, 0xc2c5, 0x3e36, 0x6a67,
          0xebe8, 0xbfb9, 0x434a, 0x171b, 0x7d7c, 0x292d, 0xd5de, 0x818f },
       {/*1f*/ 1, 0x6fc1, 0xb542, 0xda83, 0x19f4, 0x7635, 0xacb6, 0xc377,
          0x2e58, 0x4199, 0x9b1a, 0xf4db, 0x37ac, 0x586d, 0x82ee, 0xed2f },

       /* ECC bits are also in the set of tokens and they too can go bad
        * first 2 cover channel 0, while the second 2 cover channel 1
        */
       {/*20*/ 0, 0xbe01, 0xd702, 0x6903, 0x2104, 0x9f05, 0xf606, 0x4807,
          0x3208, 0x8c09, 0xe50a, 0x5b0b, 0x130c, 0xad0d, 0xc40e, 0x7a0f },
       {/*21*/ 0, 0x4101, 0x8202, 0xc303, 0x5804, 0x1905, 0xda06, 0x9b07,
          0xac08, 0xed09, 0x2e0a, 0x6f0b, 0x640c, 0xb50d, 0x760e, 0x370f },
       {/*22*/ 1, 0xc441, 0x4882, 0x8cc3, 0xf654, 0x3215, 0xbed6, 0x7a97,
          0x5ba8, 0x9fe9, 0x132a, 0xd76b, 0xadfc, 0x69bd, 0xe57e, 0x213f },
       {/*23*/ 1, 0x7621, 0x9b32, 0xed13, 0xda44, 0xac65, 0x4176, 0x3757,
          0x6f88, 0x19a9, 0xf4ba, 0x829b, 0xb5cc, 0xc3ed, 0x2efe, 0x58df }
};

/*
 * Given the syndrome argument, scan each of the channel tables for a syndrome
 * match. Depending on which table it is found, return the channel number.
 */
static int get_channel_from_ecc_syndrome(unsigned short syndrome)
{
       int row;
       int column;

       /* Determine column to scan */
       column = syndrome & 0xF;

       /* Scan all rows, looking for syndrome, or end of table */
       for (row = 0; row < NUMBER_ECC_ROWS; row++) {
               if (ecc_chipkill_syndromes[row][column] == syndrome)
                       return ecc_chipkill_syndromes[row][0];
       }

       debugf0("syndrome(%x) not found\n", syndrome);
       return -1;
}

/*
 * Check for valid error in the NB Status High register. If so, proceed to read
 * NB Status Low, NB Address Low and NB Address High registers and store data
 * into error structure.
 *
 * Returns:
 *     - 1: if hardware regs contains valid error info
 *     - 0: if no valid error is indicated
 */
static int amd64_get_error_info_regs(struct mem_ctl_info *mci,
                                    struct err_regs *regs)
{
       struct amd64_pvt *pvt;
       struct pci_dev *misc_f3_ctl;
       int err = 0;

       pvt = mci->pvt_info;
       misc_f3_ctl = pvt->misc_f3_ctl;

       err = pci_read_config_dword(misc_f3_ctl, K8_NBSH, &regs->nbsh);
       if (err)
               goto err_reg;

       if (!(regs->nbsh & K8_NBSH_VALID_BIT))
               return 0;

       /* valid error, read remaining error information registers */
       err = pci_read_config_dword(misc_f3_ctl, K8_NBSL, &regs->nbsl);
       if (err)
               goto err_reg;

       err = pci_read_config_dword(misc_f3_ctl, K8_NBEAL, &regs->nbeal);
       if (err)
               goto err_reg;

       err = pci_read_config_dword(misc_f3_ctl, K8_NBEAH, &regs->nbeah);
       if (err)
               goto err_reg;

       err = pci_read_config_dword(misc_f3_ctl, K8_NBCFG, &regs->nbcfg);
       if (err)
               goto err_reg;

       return 1;

err_reg:
       debugf0("Reading error info register failed\n");
       return 0;
}

/*
 * This function is called to retrieve the error data from hardware and store it
 * in the info structure.
 *
 * Returns:
 *     - 1: if a valid error is found
 *     - 0: if no error is found
 */
static int amd64_get_error_info(struct mem_ctl_info *mci,
                               struct err_regs *info)
{
       struct amd64_pvt *pvt;
       struct err_regs regs;

       pvt = mci->pvt_info;

       if (!amd64_get_error_info_regs(mci, info))
               return 0;

       /*
        * Here's the problem with the K8's EDAC reporting: There are four
        * registers which report pieces of error information. They are shared
        * between CEs and UEs. Furthermore, contrary to what is stated in the
        * BKDG, the overflow bit is never used! Every error always updates the
        * reporting registers.
        *
        * Can you see the race condition? All four error reporting registers
        * must be read before a new error updates them! There is no way to read
        * all four registers atomically. The best than can be done is to detect
        * that a race has occured and then report the error without any kind of
        * precision.
        *
        * What is still positive is that errors are still reported and thus
        * problems can still be detected - just not localized because the
        * syndrome and address are spread out across registers.
        *
        * Grrrrr!!!!!  Here's hoping that AMD fixes this in some future K8 rev.
        * UEs and CEs should have separate register sets with proper overflow
        * bits that are used! At very least the problem can be fixed by
        * honoring the ErrValid bit in 'nbsh' and not updating registers - just
        * set the overflow bit - unless the current error is CE and the new
        * error is UE which would be the only situation for overwriting the
        * current values.
        */

       regs = *info;

       /* Use info from the second read - most current */
       if (unlikely(!amd64_get_error_info_regs(mci, info)))
               return 0;

       /* clear the error bits in hardware */
       pci_write_bits32(pvt->misc_f3_ctl, K8_NBSH, 0, K8_NBSH_VALID_BIT);

       /* Check for the possible race condition */
       if ((regs.nbsh != info->nbsh) ||
            (regs.nbsl != info->nbsl) ||
            (regs.nbeah != info->nbeah) ||
            (regs.nbeal != info->nbeal)) {
               amd64_mc_printk(mci, KERN_WARNING,
                               "hardware STATUS read access race condition "
                               "detected!\n");
               return 0;
       }
       return 1;
}

/*
 * Handle any Correctable Errors (CEs) that have occurred. Check for valid ERROR
 * ADDRESS and process.
 */
static void amd64_handle_ce(struct mem_ctl_info *mci,
                           struct err_regs *info)
{
       struct amd64_pvt *pvt = mci->pvt_info;
       u64 SystemAddress;

       /* Ensure that the Error Address is VALID */
       if ((info->nbsh & K8_NBSH_VALID_ERROR_ADDR) == 0) {
               amd64_mc_printk(mci, KERN_ERR,
                       "HW has no ERROR_ADDRESS available\n");
               edac_mc_handle_ce_no_info(mci, EDAC_MOD_STR);
               return;
       }

       SystemAddress = extract_error_address(mci, info);

       amd64_mc_printk(mci, KERN_ERR,
               "CE ERROR_ADDRESS= 0x%llx\n", SystemAddress);

       pvt->ops->map_sysaddr_to_csrow(mci, info, SystemAddress);
}

/* Handle any Un-correctable Errors (UEs) */
static void amd64_handle_ue(struct mem_ctl_info *mci,
                           struct err_regs *info)
{
       int csrow;
       u64 SystemAddress;
       u32 page, offset;
       struct mem_ctl_info *log_mci, *src_mci = NULL;

       log_mci = mci;

       if ((info->nbsh & K8_NBSH_VALID_ERROR_ADDR) == 0) {
               amd64_mc_printk(mci, KERN_CRIT,
                       "HW has no ERROR_ADDRESS available\n");
               edac_mc_handle_ue_no_info(log_mci, EDAC_MOD_STR);
               return;
       }

       SystemAddress = extract_error_address(mci, info);

       /*
        * Find out which node the error address belongs to. This may be
        * different from the node that detected the error.
        */
       src_mci = find_mc_by_sys_addr(mci, SystemAddress);
       if (!src_mci) {
               amd64_mc_printk(mci, KERN_CRIT,
                       "ERROR ADDRESS (0x%lx) value NOT mapped to a MC\n",
                       (unsigned long)SystemAddress);
               edac_mc_handle_ue_no_info(log_mci, EDAC_MOD_STR);
               return;
       }

       log_mci = src_mci;

       csrow = sys_addr_to_csrow(log_mci, SystemAddress);
       if (csrow < 0) {
               amd64_mc_printk(mci, KERN_CRIT,
                       "ERROR_ADDRESS (0x%lx) value NOT mapped to 'csrow'\n",
                       (unsigned long)SystemAddress);
               edac_mc_handle_ue_no_info(log_mci, EDAC_MOD_STR);
       } else {
               error_address_to_page_and_offset(SystemAddress, &page, &offset);
               edac_mc_handle_ue(log_mci, page, offset, csrow, EDAC_MOD_STR);
       }
}

static inline void __amd64_decode_bus_error(struct mem_ctl_info *mci,
                                           struct err_regs *info)
{
       u32 ec  = ERROR_CODE(info->nbsl);
       u32 xec = EXT_ERROR_CODE(info->nbsl);
       int ecc_type = (info->nbsh >> 13) & 0x3;

       /* Bail early out if this was an 'observed' error */
       if (PP(ec) == K8_NBSL_PP_OBS)
               return;

       /* Do only ECC errors */
       if (xec && xec != F10_NBSL_EXT_ERR_ECC)
               return;

       if (ecc_type == 2)
               amd64_handle_ce(mci, info);
       else if (ecc_type == 1)
               amd64_handle_ue(mci, info);

       /*
        * If main error is CE then overflow must be CE.  If main error is UE
        * then overflow is unknown.  We'll call the overflow a CE - if
        * panic_on_ue is set then we're already panic'ed and won't arrive
        * here. Else, then apparently someone doesn't think that UE's are
        * catastrophic.
        */
       if (info->nbsh & K8_NBSH_OVERFLOW)
               edac_mc_handle_ce_no_info(mci, EDAC_MOD_STR "Error Overflow");
}

void amd64_decode_bus_error(int node_id, struct err_regs *regs)
{
       struct mem_ctl_info *mci = mci_lookup[node_id];

       __amd64_decode_bus_error(mci, regs);

       /*
        * Check the UE bit of the NB status high register, if set generate some
        * logs. If NOT a GART error, then process the event as a NO-INFO event.
        * If it was a GART error, skip that process.
        *
        * FIXME: this should go somewhere else, if at all.
        */
       if (regs->nbsh & K8_NBSH_UC_ERR && !report_gart_errors)
               edac_mc_handle_ue_no_info(mci, "UE bit is set");

}

/*
 * The main polling 'check' function, called FROM the edac core to perform the
 * error checking and if an error is encountered, error processing.
 */
static void amd64_check(struct mem_ctl_info *mci)
{
       struct err_regs regs;

       if (amd64_get_error_info(mci, &regs)) {
               struct amd64_pvt *pvt = mci->pvt_info;
               amd_decode_nb_mce(pvt->mc_node_id, &regs, 1);
       }
}

/*
 * Input:
 *     1) struct amd64_pvt which contains pvt->dram_f2_ctl pointer
 *     2) AMD Family index value
 *
 * Ouput:
 *     Upon return of 0, the following filled in:
 *
 *             struct pvt->addr_f1_ctl
 *             struct pvt->misc_f3_ctl
 *
 *     Filled in with related device funcitions of 'dram_f2_ctl'
 *     These devices are "reserved" via the pci_get_device()
 *
 *     Upon return of 1 (error status):
 *
 *             Nothing reserved
 */
static int amd64_reserve_mc_sibling_devices(struct amd64_pvt *pvt, int mc_idx)
{
       const struct amd64_family_type *amd64_dev = &amd64_family_types[mc_idx];

       /* Reserve the ADDRESS MAP Device */
       pvt->addr_f1_ctl = pci_get_related_function(pvt->dram_f2_ctl->vendor,
                                                   amd64_dev->addr_f1_ctl,
                                                   pvt->dram_f2_ctl);

       if (!pvt->addr_f1_ctl) {
               amd64_printk(KERN_ERR, "error address map device not found: "
                            "vendor %x device 0x%x (broken BIOS?)\n",
                            PCI_VENDOR_ID_AMD, amd64_dev->addr_f1_ctl);
               return 1;
       }

       /* Reserve the MISC Device */
       pvt->misc_f3_ctl = pci_get_related_function(pvt->dram_f2_ctl->vendor,
                                                   amd64_dev->misc_f3_ctl,
                                                   pvt->dram_f2_ctl);

       if (!pvt->misc_f3_ctl) {
               pci_dev_put(pvt->addr_f1_ctl);
               pvt->addr_f1_ctl = NULL;

               amd64_printk(KERN_ERR, "error miscellaneous device not found: "
                            "vendor %x device 0x%x (broken BIOS?)\n",
                            PCI_VENDOR_ID_AMD, amd64_dev->misc_f3_ctl);
               return 1;
       }

       debugf1("    Addr Map device PCI Bus ID:\t%s\n",
               pci_name(pvt->addr_f1_ctl));
       debugf1("    DRAM MEM-CTL PCI Bus ID:\t%s\n",
               pci_name(pvt->dram_f2_ctl));
       debugf1("    Misc device PCI Bus ID:\t%s\n",
               pci_name(pvt->misc_f3_ctl));

       return 0;
}

static void amd64_free_mc_sibling_devices(struct amd64_pvt *pvt)
{
       pci_dev_put(pvt->addr_f1_ctl);
       pci_dev_put(pvt->misc_f3_ctl);
}

/*
 * Retrieve the hardware registers of the memory controller (this includes the
 * 'Address Map' and 'Misc' device regs)
 */
static void amd64_read_mc_registers(struct amd64_pvt *pvt)
{
       u64 msr_val;
       int dram, err = 0;

       /*
        * Retrieve TOP_MEM and TOP_MEM2; no masking off of reserved bits since
        * those are Read-As-Zero
        */
       rdmsrl(MSR_K8_TOP_MEM1, msr_val);
       pvt->top_mem = msr_val >> 23;
       debugf0("  TOP_MEM=0x%08llx\n", pvt->top_mem);

       /* check first whether TOP_MEM2 is enabled */
       rdmsrl(MSR_K8_SYSCFG, msr_val);
       if (msr_val & (1U << 21)) {
               rdmsrl(MSR_K8_TOP_MEM2, msr_val);
               pvt->top_mem2 = msr_val >> 23;
               debugf0("  TOP_MEM2=0x%08llx\n", pvt->top_mem2);
       } else
               debugf0("  TOP_MEM2 disabled.\n");

       amd64_cpu_display_info(pvt);

       err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCAP, &pvt->nbcap);
       if (err)
               goto err_reg;

       if (pvt->ops->read_dram_ctl_register)
               pvt->ops->read_dram_ctl_register(pvt);

       for (dram = 0; dram < DRAM_REG_COUNT; dram++) {
               /*
                * Call CPU specific READ function to get the DRAM Base and
                * Limit values from the DCT.
                */
               pvt->ops->read_dram_base_limit(pvt, dram);

               /*
                * Only print out debug info on rows with both R and W Enabled.
                * Normal processing, compiler should optimize this whole 'if'
                * debug output block away.
                */
               if (pvt->dram_rw_en[dram] != 0) {
                       debugf1("  DRAM_BASE[%d]: 0x%8.08x-%8.08x "
                               "DRAM_LIMIT:  0x%8.08x-%8.08x\n",
                               dram,
                               (u32)(pvt->dram_base[dram] >> 32),
                               (u32)(pvt->dram_base[dram] & 0xFFFFFFFF),
                               (u32)(pvt->dram_limit[dram] >> 32),
                               (u32)(pvt->dram_limit[dram] & 0xFFFFFFFF));
                       debugf1("        IntlvEn=%s %s %s "
                               "IntlvSel=%d DstNode=%d\n",
                               pvt->dram_IntlvEn[dram] ?
                                       "Enabled" : "Disabled",
                               (pvt->dram_rw_en[dram] & 0x2) ? "W" : "!W",
                               (pvt->dram_rw_en[dram] & 0x1) ? "R" : "!R",
                               pvt->dram_IntlvSel[dram],
                               pvt->dram_DstNode[dram]);
               }
       }

       amd64_read_dct_base_mask(pvt);

       err = pci_read_config_dword(pvt->addr_f1_ctl, K8_DHAR, &pvt->dhar);
       if (err)
               goto err_reg;

       amd64_read_dbam_reg(pvt);

       err = pci_read_config_dword(pvt->misc_f3_ctl,
                               F10_ONLINE_SPARE, &pvt->online_spare);
       if (err)
               goto err_reg;

       err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCLR_0, &pvt->dclr0);
       if (err)
               goto err_reg;

       err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCHR_0, &pvt->dchr0);
       if (err)
               goto err_reg;

       if (!dct_ganging_enabled(pvt)) {
               err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCLR_1,
                                               &pvt->dclr1);
               if (err)
                       goto err_reg;

               err = pci_read_config_dword(pvt->dram_f2_ctl, F10_DCHR_1,
                                               &pvt->dchr1);
               if (err)
                       goto err_reg;
       }

       amd64_dump_misc_regs(pvt);

       return;

err_reg:
       debugf0("Reading an MC register failed\n");

}

/*
 * NOTE: CPU Revision Dependent code
 *
 * Input:
 *     @csrow_nr ChipSelect Row Number (0..pvt->cs_count-1)
 *     k8 private pointer to -->
 *                     DRAM Bank Address mapping register
 *                     node_id
 *                     DCL register where dual_channel_active is
 *
 * The DBAM register consists of 4 sets of 4 bits each definitions:
 *
 * Bits:       CSROWs
 * 0-3         CSROWs 0 and 1
 * 4-7         CSROWs 2 and 3
 * 8-11                CSROWs 4 and 5
 * 12-15       CSROWs 6 and 7
 *
 * Values range from: 0 to 15
 * The meaning of the values depends on CPU revision and dual-channel state,
 * see relevant BKDG more info.
 *
 * The memory controller provides for total of only 8 CSROWs in its current
 * architecture. Each "pair" of CSROWs normally represents just one DIMM in
 * single channel or two (2) DIMMs in dual channel mode.
 *
 * The following code logic collapses the various tables for CSROW based on CPU
 * revision.
 *
 * Returns:
 *     The number of PAGE_SIZE pages on the specified CSROW number it
 *     encompasses
 *
 */
static u32 amd64_csrow_nr_pages(int csrow_nr, struct amd64_pvt *pvt)
{
	u32 cs_mode, nr_pages;

       /*
        * The math on this doesn't look right on the surface because x/2*4 can
        * be simplified to x*2 but this expression makes use of the fact that
        * it is integral math where 1/2=0. This intermediate value becomes the
        * number of bits to shift the DBAM register to extract the proper CSROW
        * field.
        */
	cs_mode = (pvt->dbam0 >> ((csrow_nr / 2) * 4)) & 0xF;

	nr_pages = pvt->ops->dbam_to_cs(pvt, cs_mode) << (20 - PAGE_SHIFT);

       /*
        * If dual channel then double the memory size of single channel.
        * Channel count is 1 or 2
        */
	nr_pages <<= (pvt->channel_count - 1);

	debugf0("  (csrow=%d) DBAM map index= %d\n", csrow_nr, cs_mode);
	debugf0("    nr_pages= %u  channel-count = %d\n",
               nr_pages, pvt->channel_count);

	return nr_pages;
}

/*
 * Initialize the array of csrow attribute instances, based on the values
 * from pci config hardware registers.
 */
static int amd64_init_csrows(struct mem_ctl_info *mci)
{
       struct csrow_info *csrow;
       struct amd64_pvt *pvt;
       u64 input_addr_min, input_addr_max, sys_addr;
       int i, err = 0, empty = 1;

       pvt = mci->pvt_info;

       err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCFG, &pvt->nbcfg);
       if (err)
               debugf0("Reading K8_NBCFG failed\n");

       debugf0("NBCFG= 0x%x  CHIPKILL= %s DRAM ECC= %s\n", pvt->nbcfg,
               (pvt->nbcfg & K8_NBCFG_CHIPKILL) ? "Enabled" : "Disabled",
               (pvt->nbcfg & K8_NBCFG_ECC_ENABLE) ? "Enabled" : "Disabled"
               );

       for (i = 0; i < pvt->cs_count; i++) {
               csrow = &mci->csrows[i];

               if ((pvt->dcsb0[i] & K8_DCSB_CS_ENABLE) == 0) {
                       debugf1("----CSROW %d EMPTY for node %d\n", i,
                               pvt->mc_node_id);
                       continue;
               }

               debugf1("----CSROW %d VALID for MC node %d\n",
                       i, pvt->mc_node_id);

               empty = 0;
               csrow->nr_pages = amd64_csrow_nr_pages(i, pvt);
               find_csrow_limits(mci, i, &input_addr_min, &input_addr_max);
               sys_addr = input_addr_to_sys_addr(mci, input_addr_min);
               csrow->first_page = (u32) (sys_addr >> PAGE_SHIFT);
               sys_addr = input_addr_to_sys_addr(mci, input_addr_max);
               csrow->last_page = (u32) (sys_addr >> PAGE_SHIFT);
               csrow->page_mask = ~mask_from_dct_mask(pvt, i);
               /* 8 bytes of resolution */

               csrow->mtype = amd64_determine_memory_type(pvt);

               debugf1("  for MC node %d csrow %d:\n", pvt->mc_node_id, i);
               debugf1("    input_addr_min: 0x%lx input_addr_max: 0x%lx\n",
                       (unsigned long)input_addr_min,
                       (unsigned long)input_addr_max);
               debugf1("    sys_addr: 0x%lx  page_mask: 0x%lx\n",
                       (unsigned long)sys_addr, csrow->page_mask);
               debugf1("    nr_pages: %u  first_page: 0x%lx "
                       "last_page: 0x%lx\n",
                       (unsigned)csrow->nr_pages,
                       csrow->first_page, csrow->last_page);

               /*
                * determine whether CHIPKILL or JUST ECC or NO ECC is operating
                */
               if (pvt->nbcfg & K8_NBCFG_ECC_ENABLE)
                       csrow->edac_mode =
                           (pvt->nbcfg & K8_NBCFG_CHIPKILL) ?
                           EDAC_S4ECD4ED : EDAC_SECDED;
               else
                       csrow->edac_mode = EDAC_NONE;
       }

       return empty;
}

static void __rdmsr_on_cpu(void *info)
{
       struct msr_info *rv = info;
       struct msr *reg;
       int this_cpu = raw_smp_processor_id();

       if (rv->msrs)
               reg = &rv->msrs[this_cpu];
       else
               reg = &rv->reg;

       rdmsr(rv->msr_no, reg->l, reg->h);
}

static void __wrmsr_on_cpu(void *info)
{
       struct msr_info *rv = info;
       struct msr *reg;
       int this_cpu = raw_smp_processor_id();

       if (rv->msrs)
               reg = &rv->msrs[this_cpu];
       else
               reg = &rv->reg;

       wrmsr(rv->msr_no, reg->l, reg->h);
}

static void __rwmsr_on_cpus(u32 msr_no, struct msr *msrs,
                           void (*msr_func) (void *info))
{
       struct msr_info rv;

       memset(&rv, 0, sizeof(rv));

       rv.msrs   = msrs;
       rv.msr_no = msr_no;

       on_each_cpu(msr_func, &rv, 0, 1);

}

static void wrmsr_on_cpus(u32 msr_no, struct msr *msrs)
{
       __rwmsr_on_cpus(msr_no, msrs, __wrmsr_on_cpu);
}

static void rdmsr_on_cpus(u32 msr_no, struct msr *msrs)
{
       __rwmsr_on_cpus(msr_no, msrs, __rdmsr_on_cpu);
}

static void check_mcg_ctl(void *ret)
{
       u64 msr_val = 0;
       u8 nbe;

       rdmsrl(MSR_IA32_MCG_CTL, msr_val);
       nbe = msr_val & K8_MSR_MCGCTL_NBE;

       debugf0("core: %u, MCG_CTL: 0x%llx, NB MSR is %s\n",
               raw_smp_processor_id(), msr_val,
               (nbe ? "enabled" : "disabled"));

       if (!nbe)
               *(int *)ret = 0;
}

/* check MCG_CTL on all the cpus on this node */
static bool amd64_nb_mce_bank_enabled_on_node(int nid)
{
       int ret = 1;
       on_each_cpu(check_mcg_ctl, &ret, 0, 1);

       return ret;
}

static int amd64_toggle_ecc_err_reporting(struct amd64_pvt *pvt, bool on)
{
       struct msr *msrs = NULL;
       int cpu, idx = 0;

       msrs = kzalloc(sizeof(struct msr) * num_online_cpus(), GFP_KERNEL);
       if (!msrs) {
               amd64_printk(KERN_WARNING, "%s: error allocating msrs\n",
                            __func__);
               return -ENOMEM;
       }

       rdmsr_on_cpus(MSR_IA32_MCG_CTL, msrs);
       for_each_online_cpu(cpu) {

               if (on) {
                       if (msrs[idx].l & K8_MSR_MCGCTL_NBE)
                               pvt->flags.ecc_report = 1;

                       msrs[idx].l |= K8_MSR_MCGCTL_NBE;
               } else {
                       /*
                        * Turn off ECC reporting only when it was off before
                        */
                       if (!pvt->flags.ecc_report)
                               msrs[idx].l &= ~K8_MSR_MCGCTL_NBE;
               }
               idx++;
       }
       wrmsr_on_cpus(MSR_IA32_MCG_CTL, msrs);

       kfree(msrs);

       return 0;
}

/*
 * Only if 'ecc_enable_override' is set AND BIOS had ECC disabled, do "we"
 * enable it.
 */
static void amd64_enable_ecc_error_reporting(struct mem_ctl_info *mci)
{
       struct amd64_pvt *pvt = mci->pvt_info;
       int err = 0;
       u32 value, mask = K8_NBCTL_CECCEn | K8_NBCTL_UECCEn;

       if (!ecc_enable_override)
               return;

       amd64_printk(KERN_WARNING,
               "'ecc_enable_override' parameter is active, "
               "Enabling AMD ECC hardware now: CAUTION\n");

       err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCTL, &value);
       if (err)
               debugf0("Reading K8_NBCTL failed\n");

       /* turn on UECCn and CECCEn bits */
       pvt->old_nbctl = value & mask;
       pvt->nbctl_mcgctl_saved = 1;

       value |= mask;
       pci_write_config_dword(pvt->misc_f3_ctl, K8_NBCTL, value);

       if (amd64_toggle_ecc_err_reporting(pvt, ON))
               amd64_printk(KERN_WARNING, "Error enabling ECC reporting over "
                                          "MCGCTL!\n");

       err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCFG, &value);
       if (err)
               debugf0("Reading K8_NBCFG failed\n");

       debugf0("NBCFG(1)= 0x%x  CHIPKILL= %s ECC_ENABLE= %s\n", value,
               (value & K8_NBCFG_CHIPKILL) ? "Enabled" : "Disabled",
               (value & K8_NBCFG_ECC_ENABLE) ? "Enabled" : "Disabled");

       if (!(value & K8_NBCFG_ECC_ENABLE)) {
               amd64_printk(KERN_WARNING,
                       "This node reports that DRAM ECC is "
                       "currently Disabled; ENABLING now\n");

               /* Attempt to turn on DRAM ECC Enable */
               value |= K8_NBCFG_ECC_ENABLE;
               pci_write_config_dword(pvt->misc_f3_ctl, K8_NBCFG, value);

               err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCFG, &value);
               if (err)
                       debugf0("Reading K8_NBCFG failed\n");

               if (!(value & K8_NBCFG_ECC_ENABLE)) {
                       amd64_printk(KERN_WARNING,
                               "Hardware rejects Enabling DRAM ECC checking\n"
                               "Check memory DIMM configuration\n");
               } else {
                       amd64_printk(KERN_DEBUG,
                               "Hardware accepted DRAM ECC Enable\n");
               }
       }
       debugf0("NBCFG(2)= 0x%x  CHIPKILL= %s ECC_ENABLE= %s\n", value,
               (value & K8_NBCFG_CHIPKILL) ? "Enabled" : "Disabled",
               (value & K8_NBCFG_ECC_ENABLE) ? "Enabled" : "Disabled");

       pvt->ctl_error_info.nbcfg = value;
}

static void amd64_restore_ecc_error_reporting(struct amd64_pvt *pvt)
{
       int err = 0;
       u32 value, mask = K8_NBCTL_CECCEn | K8_NBCTL_UECCEn;

       if (!pvt->nbctl_mcgctl_saved)
               return;

       err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCTL, &value);
       if (err)
               debugf0("Reading K8_NBCTL failed\n");
       value &= ~mask;
       value |= pvt->old_nbctl;

       /* restore the NB Enable MCGCTL bit */
       pci_write_config_dword(pvt->misc_f3_ctl, K8_NBCTL, value);

       if (amd64_toggle_ecc_err_reporting(pvt, OFF))
               amd64_printk(KERN_WARNING, "Error restoring ECC reporting over "
                                          "MCGCTL!\n");
}

/*
 * EDAC requires that the BIOS have ECC enabled before taking over the
 * processing of ECC errors. This is because the BIOS can properly initialize
 * the memory system completely. A command line option allows to force-enable
 * hardware ECC later in amd64_enable_ecc_error_reporting().
 */
static const char *ecc_warning =
       "WARNING: ECC is disabled by BIOS. Module will NOT be loaded.\n"
       " Either Enable ECC in the BIOS, or set 'ecc_enable_override'.\n"
       " Also, use of the override can cause unknown side effects.\n";

static int amd64_check_ecc_enabled(struct amd64_pvt *pvt)
{
       u32 value;
       int err = 0;
       u8 ecc_enabled = 0;
       bool nb_mce_en = false;

       err = pci_read_config_dword(pvt->misc_f3_ctl, K8_NBCFG, &value);
       if (err)
               debugf0("Reading K8_NBCTL failed\n");

       ecc_enabled = !!(value & K8_NBCFG_ECC_ENABLE);
       if (!ecc_enabled)
               amd64_printk(KERN_WARNING, "This node reports that Memory ECC "
                            "is currently disabled, set F3x%x[22] (%s).\n",
                            K8_NBCFG, pci_name(pvt->misc_f3_ctl));
       else
               amd64_printk(KERN_INFO, "ECC is enabled by BIOS.\n");

       nb_mce_en = amd64_nb_mce_bank_enabled_on_node(pvt->mc_node_id);
       if (!nb_mce_en)
               amd64_printk(KERN_WARNING, "NB MCE bank disabled, set MSR "
                            "0x%08x[4] on node %d to enable.\n",
                            MSR_IA32_MCG_CTL, pvt->mc_node_id);

       if (!ecc_enabled || !nb_mce_en) {
               if (!ecc_enable_override) {
                       amd64_printk(KERN_WARNING, "%s", ecc_warning);
                       return -ENODEV;
               }
       } else
               /* CLEAR the override, since BIOS controlled it */
               ecc_enable_override = 0;

       return 0;
}

static void amd64_setup_mci_misc_attributes(struct mem_ctl_info *mci)
{
       struct amd64_pvt *pvt = mci->pvt_info;

       mci->mtype_cap          = MEM_FLAG_DDR2 | MEM_FLAG_RDDR2;
       mci->edac_ctl_cap       = EDAC_FLAG_NONE;

       if (pvt->nbcap & K8_NBCAP_SECDED)
               mci->edac_ctl_cap |= EDAC_FLAG_SECDED;

       if (pvt->nbcap & K8_NBCAP_CHIPKILL)
               mci->edac_ctl_cap |= EDAC_FLAG_S4ECD4ED;

       mci->edac_cap           = amd64_determine_edac_cap(pvt);
       mci->mod_name           = EDAC_MOD_STR;
       mci->mod_ver            = EDAC_AMD64_VERSION;
       mci->ctl_name           = get_amd_family_name(pvt->mc_type_index);
       mci->dev_name           = pci_name(pvt->dram_f2_ctl);
       mci->ctl_page_to_phys   = NULL;

       /* IMPORTANT: Set the polling 'check' function in this module */
       mci->edac_check         = amd64_check;
}

/*
 * Init stuff for this DRAM Controller device.
 *
 * Due to a hardware feature on Fam10h CPUs, the Enable Extended Configuration
 * Space feature MUST be enabled on ALL Processors prior to actually reading
 * from the ECS registers. Since the loading of the module can occur on any
 * 'core', and cores don't 'see' all the other processors ECS data when the
 * others are NOT enabled. Our solution is to first enable ECS access in this
 * routine on all processors, gather some data in a amd64_pvt structure and
 * later come back in a finish-setup function to perform that final
 * initialization. See also amd64_init_2nd_stage() for that.
 */
static int amd64_probe_one_instance(struct pci_dev *dram_f2_ctl,
                                   int mc_type_index)
{
       struct amd64_pvt *pvt = NULL;
       int err = 0, ret;

       ret = -ENOMEM;
       pvt = kzalloc(sizeof(struct amd64_pvt), GFP_KERNEL);
       if (!pvt)
               goto err_exit;

       pvt->mc_node_id = get_node_id(dram_f2_ctl);

       pvt->dram_f2_ctl        = dram_f2_ctl;
       pvt->ext_model          = boot_cpu_data.x86_model >> 4;
       pvt->mc_type_index      = mc_type_index;
       pvt->ops                = family_ops(mc_type_index);

       /*
        * We have the dram_f2_ctl device as an argument, now go reserve its
        * sibling devices from the PCI system.
        */
       ret = -ENODEV;
       err = amd64_reserve_mc_sibling_devices(pvt, mc_type_index);
       if (err)
               goto err_free;

       ret = -EINVAL;
       err = amd64_check_ecc_enabled(pvt);
       if (err)
               goto err_put;

       /*
        * Key operation here: setup of HW prior to performing ops on it. Some
        * setup is required to access ECS data. After this is performed, the
        * 'teardown' function must be called upon error and normal exit paths.
        */
       if (boot_cpu_data.x86 >= 0x10)
               amd64_setup(pvt);

       /*
        * Save the pointer to the private data for use in 2nd initialization
        * stage
        */
       pvt_lookup[pvt->mc_node_id] = pvt;

       return 0;

err_put:
       amd64_free_mc_sibling_devices(pvt);

err_free:
       kfree(pvt);

err_exit:
       return ret;
}

/*
 * This is the finishing stage of the init code. Needs to be performed after all
 * MCs' hardware have been prepped for accessing extended config space.
 */
static int amd64_init_2nd_stage(struct amd64_pvt *pvt)
{
       int node_id = pvt->mc_node_id;
       struct mem_ctl_info *mci;
	int ret = -ENODEV;

       amd64_read_mc_registers(pvt);

       /*
        * We need to determine how many memory channels there are. Then use
        * that information for calculating the size of the dynamic instance
        * tables in the 'mci' structure
        */
       pvt->channel_count = pvt->ops->early_channel_count(pvt);
       if (pvt->channel_count < 0)
               goto err_exit;

       ret = -ENOMEM;
       mci = edac_mc_alloc(0, pvt->cs_count, pvt->channel_count);
       if (!mci)
               goto err_exit;

       mci->pvt_info = pvt;

       mci->dev = &pvt->dram_f2_ctl->dev;
       amd64_setup_mci_misc_attributes(mci);

       if (amd64_init_csrows(mci))
               mci->edac_cap = EDAC_FLAG_NONE;

       amd64_enable_ecc_error_reporting(mci);

       ret = -ENODEV;
       if (edac_mc_add_mc(mci, node_id)) {
               debugf1("failed edac_mc_add_mc()\n");
               goto err_add_mc;
       }

       mci_lookup[node_id] = mci;
       pvt_lookup[node_id] = NULL;

       /* register stuff with EDAC MCE */
       if (report_gart_errors)
               amd_report_gart_errors(true);

       amd_register_ecc_decoder(amd64_decode_bus_error);

       return 0;

err_add_mc:
       edac_mc_free(mci);

err_exit:
       debugf0("failure to init 2nd stage: ret=%d\n", ret);

       amd64_restore_ecc_error_reporting(pvt);

       if (boot_cpu_data.x86 > 0xf)
               amd64_teardown(pvt);

       amd64_free_mc_sibling_devices(pvt);

       kfree(pvt_lookup[pvt->mc_node_id]);
       pvt_lookup[node_id] = NULL;

       return ret;
}


static int __devinit amd64_init_one_instance(struct pci_dev *pdev,
                                const struct pci_device_id *mc_type)
{
       int ret = 0;

       debugf0("(MC node=%d,mc_type='%s')\n", get_node_id(pdev),
               get_amd_family_name(mc_type->driver_data));

       ret = pci_enable_device(pdev);
       if (ret < 0)
               ret = -EIO;
       else
               ret = amd64_probe_one_instance(pdev, mc_type->driver_data);

       if (ret < 0)
               debugf0("ret=%d\n", ret);

       return ret;
}

static void __devexit amd64_remove_one_instance(struct pci_dev *pdev)
{
       struct mem_ctl_info *mci;
       struct amd64_pvt *pvt;

       /* Remove from EDAC CORE tracking list */
       mci = edac_mc_del_mc(&pdev->dev);
       if (!mci)
               return;

       pvt = mci->pvt_info;

       amd64_restore_ecc_error_reporting(pvt);

       if (boot_cpu_data.x86 > 0xf)
               amd64_teardown(pvt);

       amd64_free_mc_sibling_devices(pvt);

       kfree(pvt);
       mci->pvt_info = NULL;

       mci_lookup[pvt->mc_node_id] = NULL;

       /* unregister from EDAC MCE */
       amd_report_gart_errors(false);
       amd_unregister_ecc_decoder(amd64_decode_bus_error);

       /* Free the EDAC CORE resources */
       edac_mc_free(mci);
}

/*
 * This table is part of the interface for loading drivers for PCI devices. The
 * PCI core identifies what devices are on a system during boot, and then
 * inquiry this table to see if this driver is for a given device found.
 */
static const struct pci_device_id amd64_pci_table[] __devinitdata = {
       {
               .vendor         = PCI_VENDOR_ID_AMD,
               .device         = PCI_DEVICE_ID_AMD_10H_NB_DRAM,
               .subvendor      = PCI_ANY_ID,
               .subdevice      = PCI_ANY_ID,
               .class          = 0,
               .class_mask     = 0,
               .driver_data    = F10_CPUS
       },
       {
               .vendor         = PCI_VENDOR_ID_AMD,
               .device         = PCI_DEVICE_ID_AMD_11H_NB_DRAM,
               .subvendor      = PCI_ANY_ID,
               .subdevice      = PCI_ANY_ID,
               .class          = 0,
               .class_mask     = 0,
               .driver_data    = F11_CPUS
       },
       {0, }
};
MODULE_DEVICE_TABLE(pci, amd64_pci_table);

static struct pci_driver amd64_pci_driver = {
       .name           = EDAC_MOD_STR,
       .probe          = amd64_init_one_instance,
       .remove         = __devexit_p(amd64_remove_one_instance),
       .id_table       = amd64_pci_table,
};

static int __init amd64_edac_init(void)
{
       int nb, err = -ENODEV;

       edac_printk(KERN_INFO, EDAC_MOD_STR, EDAC_AMD64_VERSION "\n");

       if (cache_k8_northbridges() < 0)
               return err;

       err = pci_register_driver(&amd64_pci_driver);
       if (err)
               return err;

       /*
        * At this point, the array 'pvt_lookup[]' contains pointers to alloc'd
        * amd64_pvt structs. These will be used in the 2nd stage init function
        * to finish initialization of the MC instances.
        */
       for (nb = 0; nb < num_k8_northbridges; nb++) {
               if (!pvt_lookup[nb])
                       continue;

               err = amd64_init_2nd_stage(pvt_lookup[nb]);
               if (err)
                       goto err_2nd_stage;
       }

       return 0;

err_2nd_stage:
       debugf0("2nd stage failed\n");
       pci_unregister_driver(&amd64_pci_driver);

       return err;
}

static void __exit amd64_edac_exit(void)
{
       pci_unregister_driver(&amd64_pci_driver);
}

module_init(amd64_edac_init);
module_exit(amd64_edac_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SoftwareBitMaker: Doug Thompson, "
               "Dave Peterson, Thayne Harbaugh");
MODULE_DESCRIPTION("MC support for AMD64 memory controllers - "
               EDAC_AMD64_VERSION);
