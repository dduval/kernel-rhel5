#ifndef _ASM_X8664_PROTO_H
#define _ASM_X8664_PROTO_H 1

#include <asm/ldt.h>

/* misc architecture specific prototypes */

struct cpuinfo_x86; 
struct pt_regs;

extern void start_kernel(void);
extern void pda_init(int); 

extern void early_idt_handler(void);

extern void mcheck_init(struct cpuinfo_x86 *c);
#ifdef CONFIG_MTRR
extern void mtrr_ap_init(void);
extern void mtrr_bp_init(void);
#else
#define mtrr_ap_init() do {} while (0)
#define mtrr_bp_init() do {} while (0)
#endif
extern void init_memory_mapping(unsigned long start, unsigned long end);
extern void size_zones(unsigned long *z, unsigned long *h,
			unsigned long start_pfn, unsigned long end_pfn);

extern void system_call(void); 
extern int kernel_syscall(void);
extern void syscall_init(void);

extern void ia32_syscall(void);
extern void ia32_cstar_target(void); 
extern void ia32_sysenter_target(void); 

extern void config_acpi_tables(void);
extern void ia32_syscall(void);

extern int pmtimer_mark_offset(void);
extern void pmtimer_resume(void);
extern void pmtimer_wait(unsigned);
extern int pmtimer_calibrate_apic(unsigned, int *tries);
extern long do_gettimeoffset_pm(void);
#ifdef CONFIG_X86_PM_TIMER
extern u32 pmtmr_ioport;
#else
#define pmtmr_ioport 0
#endif
extern unsigned long long monotonic_base;
extern int sysctl_vsyscall;
extern int nohpet;
extern unsigned long vxtime_hz;
extern void time_init_gtod(void);

extern int numa_setup(char *opt);

extern int setup_early_printk(char *); 
extern void early_printk(const char *fmt, ...) __attribute__((format(printf,1,2)));

extern void early_identify_cpu(struct cpuinfo_x86 *c);

extern int k8_scan_nodes(unsigned long start, unsigned long end);

extern void numa_initmem_init(unsigned long start_pfn, unsigned long end_pfn);
extern unsigned long numa_free_all_bootmem(void);

extern int reserve_bootmem_generic(unsigned long phys, unsigned len,
				    unsigned long flags);
extern void free_bootmem_generic(unsigned long phys, unsigned len);

extern void load_gs_index(unsigned gs);

extern void stop_timer_interrupt(void);
extern void main_timer_handler(struct pt_regs *regs);

extern unsigned long end_pfn_map; 

extern void show_trace(struct task_struct *, struct pt_regs *, unsigned long * rsp);
extern void show_registers(struct pt_regs *regs);

extern void exception_table_check(void);

extern void acpi_reserve_bootmem(void);

extern void swap_low_mappings(void);

extern void __show_regs(struct pt_regs * regs);
extern void show_regs(struct pt_regs * regs);

extern void syscall32_cpu_init(void);

extern void setup_node_bootmem(int nodeid, unsigned long start, unsigned long end);

extern void check_ioapic(void);
extern void check_efer(void);

extern int unhandled_signal(struct task_struct *tsk, int sig);

extern int unsynchronized_tsc(void);

extern void select_idle_routine(const struct cpuinfo_x86 *c);

extern unsigned long table_start, table_end;

extern int exception_trace;
extern int using_apic_timer;
extern int disable_apic;
extern unsigned cpu_khz;
extern unsigned tsc_khz;
extern int ioapic_force;
extern int skip_ioapic_setup;
extern int acpi_ht;
extern int acpi_disabled;

extern void pci_iommu_shutdown(void);
extern void no_iommu_init(void);
extern int force_iommu, no_iommu;
extern int iommu_detected;
extern int iommu_pass_through;

/* 10 seconds */
#define DMAR_OPERATION_TIMEOUT ((cycles_t) tsc_khz*10*1000)

#ifdef CONFIG_IOMMU
extern void gart_iommu_init(void);
extern void gart_iommu_shutdown(void);
extern void gart_parse_options(char *);
extern void iommu_hole_init(void);
extern int fallback_aper_order;
extern int fallback_aper_force;
extern int iommu_aperture;
extern int iommu_aperture_allowed;
extern int iommu_aperture_disabled;
extern int fix_aperture;
#else
#define iommu_aperture 0
#define iommu_aperture_allowed 0

static inline void gart_iommu_shutdown(void)
{
}

#endif

extern int reboot_force;
extern int notsc_setup(char *);
extern int setup_additional_cpus(char *);

extern void smp_local_timer_interrupt(struct pt_regs * regs);

extern int force_mwait;

long do_arch_prctl(struct task_struct *task, int code, unsigned long addr);

#define round_up(x,y) (((x) + (y) - 1) & ~((y)-1))
#define round_down(x,y) ((x) & ~((y)-1))

#endif
