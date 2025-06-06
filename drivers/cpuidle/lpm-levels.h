/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * Copyright (c) 2014-2019, The Linux Foundation. All rights reserved.
 */

#include <soc/qcom/pm.h>

#define NR_LPM_LEVELS 8

struct power_params {
	uint32_t entry_latency;		/* Entry latency */
	uint32_t exit_latency;		/* Exit latency */
	uint32_t min_residency;
	uint32_t max_residency;
	bool local_timer_stop;
};

struct lpm_cpu_level {
	const char *name;
	bool use_bc_timer;
	struct power_params pwr;
	unsigned int psci_id;
	bool is_reset;
	int reset_level;
};

struct lpm_cpu {
	struct list_head list;
	struct cpumask related_cpus;
	struct lpm_cpu_level levels[NR_LPM_LEVELS];
	int nlevels;
	unsigned int psci_mode_shift;
	unsigned int psci_mode_mask;
	struct cpuidle_driver *drv;
	struct lpm_cluster *parent;
};

struct lpm_level_avail {
	bool idle_enabled;
	bool suspend_enabled;
	uint32_t exit_latency;
	struct kobject *kobj;
	struct kobj_attribute idle_enabled_attr;
	struct kobj_attribute suspend_enabled_attr;
	struct kobj_attribute latency_attr;
	void *data;
	int idx;
	bool cpu_node;
};

struct lpm_cluster_level {
	const char *level_name;
	int min_child_level;
	struct cpumask num_cpu_votes;
	struct power_params pwr;
	bool notify_rpm;
	bool sync_level;
	struct lpm_level_avail available;
	unsigned int psci_id;
	bool is_reset;
	int reset_level;
};

struct lpm_cluster {
	struct list_head list;
	struct list_head child;
	const char *cluster_name;
	unsigned long aff_level; /* Affinity level of the node */
	struct lpm_cluster_level levels[NR_LPM_LEVELS];
	int nlevels;
	int min_child_level;
	int default_level;
	int last_level;
	struct list_head cpu;
	raw_spinlock_t sync_lock;
	struct cpumask child_cpus;
	struct cpumask num_children_in_sync;
	struct lpm_cluster *parent;
	struct lpm_stats *stats;
	unsigned int psci_mode_shift;
	unsigned int psci_mode_mask;
};

struct lpm_cluster *lpm_of_parse_cluster(struct platform_device *pdev);
void free_cluster_node(struct lpm_cluster *cluster);
void cluster_dt_walkthrough(struct lpm_cluster *cluster);

int create_cluster_lvl_nodes(struct lpm_cluster *p, struct kobject *kobj);
int lpm_cpu_mode_allow(unsigned int cpu,
		unsigned int mode, bool from_idle);
bool lpm_cluster_mode_allow(struct lpm_cluster *cluster,
		unsigned int mode, bool from_idle);
uint32_t *get_per_cpu_max_residency(int cpu);
extern struct lpm_cluster *lpm_root_node;

#if defined(CONFIG_SMP)
DECLARE_PER_CPU(bool, pending_ipi);
static inline bool is_IPI_pending(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		if per_cpu(pending_ipi, cpu)
			return true;
	}
	return false;
}
#else
static inline bool is_IPI_pending(const struct cpumask *mask)
{
	return false;
}
#endif