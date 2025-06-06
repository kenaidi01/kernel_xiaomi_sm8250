/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_SYSCTL_H
#define _LINUX_SCHED_SYSCTL_H

#include <linux/types.h>

struct ctl_table;

#ifdef CONFIG_DETECT_HUNG_TASK
extern int	     sysctl_hung_task_check_count;
extern unsigned int  sysctl_hung_task_panic;
extern unsigned long sysctl_hung_task_timeout_secs;
extern unsigned long sysctl_hung_task_check_interval_secs;
extern int sysctl_hung_task_warnings;
extern int sysctl_hung_task_selective_monitoring;
extern int proc_dohung_task_timeout_secs(struct ctl_table *table, int write,
					 void __user *buffer,
					 size_t *lenp, loff_t *ppos);
#else
/* Avoid need for ifdefs elsewhere in the code */
enum { sysctl_hung_task_timeout_secs = 0 };
#endif

#define MAX_CLUSTERS 3
/* MAX_MARGIN_LEVELS should be one less than MAX_CLUSTERS */
#define MAX_MARGIN_LEVELS (MAX_CLUSTERS - 1)

extern unsigned int sysctl_sched_min_granularity;
extern unsigned int sysctl_sched_sync_hint_enable;
extern unsigned int sysctl_sched_cstate_aware;
extern unsigned int sysctl_boost_stask_to_big;

#if defined(CONFIG_PREEMPTIRQ_EVENTS) || defined(CONFIG_PREEMPT_TRACER)
extern unsigned int sysctl_preemptoff_tracing_threshold_ns;
#endif
#if defined(CONFIG_PREEMPTIRQ_EVENTS) && defined(CONFIG_IRQSOFF_TRACER)
extern unsigned int sysctl_irqsoff_tracing_threshold_ns;
#endif

#ifdef CONFIG_PELT_COMPATIBILITY_LAYER
static unsigned int sysctl_sched_boost;
static unsigned int sysctl_sched_prefer_spread;
static unsigned int sysctl_sched_busy_hyst_enable_cpus;
static unsigned int sysctl_sched_busy_hyst;
static unsigned int sysctl_sched_group_upmigrate_pct;
static unsigned int sysctl_sched_group_downmigrate_pct;
static unsigned int sysctl_sched_ravg_window_nr_ticks;
#endif /* CONFIG_PELT_COMPATIBILITY_LAYER */

enum sched_tunable_scaling {
	SCHED_TUNABLESCALING_NONE,
	SCHED_TUNABLESCALING_LOG,
	SCHED_TUNABLESCALING_LINEAR,
	SCHED_TUNABLESCALING_END,
};
extern enum sched_tunable_scaling sysctl_sched_tunable_scaling;

extern unsigned int sysctl_numa_balancing_scan_delay;
extern unsigned int sysctl_numa_balancing_scan_period_min;
extern unsigned int sysctl_numa_balancing_scan_period_max;
extern unsigned int sysctl_numa_balancing_scan_size;

#ifdef CONFIG_SCHED_DEBUG
extern __read_mostly unsigned int sysctl_sched_migration_cost;
extern __read_mostly unsigned int sysctl_sched_nr_migrate;

int sched_proc_update_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *length,
		loff_t *ppos);
#endif

extern int sched_boost_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *lenp, loff_t *ppos);

extern int sched_boost_top_app_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *lenp, loff_t *ppos);

/*
 *  control realtime throttling:
 *
 *  /proc/sys/kernel/sched_rt_period_us
 *  /proc/sys/kernel/sched_rt_runtime_us
 */
extern unsigned int sysctl_sched_rt_period;
extern int sysctl_sched_rt_runtime;

#ifdef CONFIG_UCLAMP_TASK
extern unsigned int sysctl_sched_uclamp_util_min;
extern unsigned int sysctl_sched_uclamp_util_max;
extern unsigned int sysctl_sched_uclamp_util_min_rt_default;
#endif

#ifdef CONFIG_CFS_BANDWIDTH
extern unsigned int sysctl_sched_cfs_bandwidth_slice;
#endif

#ifdef CONFIG_SCHED_AUTOGROUP
extern unsigned int sysctl_sched_autogroup_enabled;
#endif

extern int sysctl_sched_rr_timeslice;
extern int sched_rr_timeslice;

extern int sched_rr_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);

extern int sched_rt_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos);

#ifdef CONFIG_UCLAMP_TASK
extern int sysctl_sched_uclamp_handler(struct ctl_table *table, int write,
				       void __user *buffer, size_t *lenp,
				       loff_t *ppos);
#endif

extern int sysctl_numa_balancing(struct ctl_table *table, int write,
				 void __user *buffer, size_t *lenp,
				 loff_t *ppos);

extern int sysctl_schedstats(struct ctl_table *table, int write,
				 void __user *buffer, size_t *lenp,
				 loff_t *ppos);

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
extern unsigned int sysctl_sched_energy_aware;
extern int sched_energy_aware_handler(struct ctl_table *table, int write,
				 void __user *buffer, size_t *lenp,
				 loff_t *ppos);
#endif

#define LIB_PATH_LENGTH 512
static char sched_lib_name[LIB_PATH_LENGTH];
static unsigned int sched_lib_mask_force;
#endif /* _LINUX_SCHED_SYSCTL_H */
