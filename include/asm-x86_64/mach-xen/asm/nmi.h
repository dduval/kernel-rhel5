/*
 *  linux/include/asm-i386/nmi.h
 */
#ifndef ASM_NMI_H
#define ASM_NMI_H

#include <linux/pm.h>
#include <asm/io.h>

#include <xen/interface/nmi.h>

struct pt_regs;

typedef int (*nmi_callback_t)(struct pt_regs * regs, int cpu);

/**
 * set_nmi_callback
 *
 * Set a handler for an NMI. Only one handler may be
 * set. Return 1 if the NMI was handled.
 */
void set_nmi_callback(nmi_callback_t callback);

/**
 * unset_nmi_callback
 *
 * Remove the handler previously set.
 */
void unset_nmi_callback(void);

#ifdef CONFIG_PM
 
/** Replace the PM callback routine for NMI. */
struct pm_dev * set_nmi_pm_callback(pm_callback callback);

/** Unset the PM callback routine back to the default. */
void unset_nmi_pm_callback(struct pm_dev * dev);

#else

static inline struct pm_dev * set_nmi_pm_callback(pm_callback callback)
{
	return 0;
} 
 
static inline void unset_nmi_pm_callback(struct pm_dev * dev)
{
}

#endif /* CONFIG_PM */
 
extern void default_do_nmi(struct pt_regs *);
extern void die_nmi(char *str, struct pt_regs *regs);

static inline unsigned char get_nmi_reason(void)
{
        shared_info_t *s = HYPERVISOR_shared_info;
        unsigned char reason = 0;

        /* construct a value which looks like it came from
         * port 0x61.
         */
        if (test_bit(_XEN_NMIREASON_io_error, &s->arch.nmi_reason))
                reason |= 0x40;
        if (test_bit(_XEN_NMIREASON_parity_error, &s->arch.nmi_reason))
                reason |= 0x80;

        return reason;
}

extern int panic_on_timeout;
extern int unknown_nmi_panic;

void lapic_watchdog_stop(void);
int lapic_watchdog_probe(void);
int lapic_watchdog_init(unsigned nmi_hz);
int lapic_wd_event(unsigned nmi_hz);
unsigned lapic_adjust_nmi_hz(unsigned hz);
void disable_lapic_nmi_watchdog(void);
void enable_lapic_nmi_watchdog(void);

extern int check_nmi_watchdog(void);
 
extern void setup_apic_nmi_watchdog (void);
extern int reserve_lapic_nmi(void);
extern void release_lapic_nmi(void);
extern void disable_timer_nmi_watchdog(void);
extern void enable_timer_nmi_watchdog(void);
extern int nmi_watchdog_tick (struct pt_regs * regs, unsigned reason);
extern int do_nmi_callback2(struct pt_regs *regs, int cpu);

extern void nmi_watchdog_default(void);
extern int setup_nmi_watchdog(char *);

extern unsigned int nmi_watchdog;
#define NMI_DEFAULT	-1
#define NMI_NONE	0
#define NMI_IO_APIC	1
#define NMI_LOCAL_APIC	2
#define NMI_INVALID	3

#endif /* ASM_NMI_H */
