/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021-2024 Masahito S. <firelzrd@gmail.com>
 * Copyright (C) 2025-2026 AxionOS
 */

#include "bore_sched.h"
#include <linux/init.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/rculist.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/version.h>
#include <trace/hooks/bore.h>

uint sched_bore = DEFAULT_SCHED_BORE;
uint sched_burst_inherit_type = DEFAULT_BURST_INHERIT_TYPE;
uint sched_burst_smoothness = DEFAULT_BURST_SMOOTHNESS;
uint sched_burst_penalty_offset = DEFAULT_BURST_PENALTY_OFFSET;
uint sched_burst_penalty_scale = DEFAULT_BURST_PENALTY_SCALE;
uint sched_burst_cache_lifetime = DEFAULT_BURST_CACHE_LIFETIME;

module_param_named(sched_bore, sched_bore, uint, 0644);
MODULE_PARM_DESC(sched_bore, "Enable BORE scheduler");

module_param_named(sched_burst_inherit_type, sched_burst_inherit_type, uint, 0644);
MODULE_PARM_DESC(sched_burst_inherit_type, "BORE burst inherit type (0: none, 1: parent, 2: ancestor hub)");

module_param_named(sched_burst_smoothness, sched_burst_smoothness, uint, 0644);
MODULE_PARM_DESC(sched_burst_smoothness, "BORE burst smoothness");

module_param_named(sched_burst_penalty_offset, sched_burst_penalty_offset, uint, 0644);
MODULE_PARM_DESC(sched_burst_penalty_offset, "BORE burst penalty offset");

module_param_named(sched_burst_penalty_scale, sched_burst_penalty_scale, uint, 0644);
MODULE_PARM_DESC(sched_burst_penalty_scale, "BORE burst penalty scale");

module_param_named(sched_burst_cache_lifetime, sched_burst_cache_lifetime, uint, 0644);
MODULE_PARM_DESC(sched_burst_cache_lifetime, "BORE burst cache lifetime in ns");

static bool bore_hooks_active;
static u32 bore_reciprocal_lut[BURST_CACHE_SAMPLE_LIMIT + 1];
static struct ctl_table_header *bore_sysctl_header;
static struct proc_dir_entry *bore_proc_entry;

static void init_bore_reciprocal_lut(void)
{
	int i;

	for (i = 1; i <= BURST_CACHE_SAMPLE_LIMIT; i++)
		bore_reciprocal_lut[i] = (u32)div64_u64(0xffffffffULL + i, i);
}

static inline u32 count_children_upto2(struct task_struct *p)
{
	struct list_head *head = &p->children;
	struct list_head *first = READ_ONCE(head->next);
	struct list_head *second = READ_ONCE(first->next);

	return (first != head) + (second != head);
}

static inline bool burst_cache_expired(struct bore_bc *bc, u64 now)
{
	struct bore_bc bc_val = { .value = READ_ONCE(bc->value) };
	u64 timestamp = (u64)bc_val.timestamp << BORE_BC_TIMESTAMP_SHIFT;

	return (now - timestamp > (u64)sched_burst_cache_lifetime);
}

static void update_burst_cache(struct bore_bc *bc, struct task_struct *p, struct bore_ctx *ctx, u32 count, u32 total, u64 now)
{
	u32 average = (count == 1) ? total :
		(u32)(((u64)total * bore_reciprocal_lut[count]) >> 32);
	struct bore_bc new_bc = {
		.penalty = max(average, (u32)ctx->penalty),
		.timestamp = now >> BORE_BC_TIMESTAMP_SHIFT
	};

	WRITE_ONCE(bc->value, new_bc.value);
}

static u32 inherit_from_parent(struct task_struct *parent, u64 clone_flags, u64 now)
{
	struct bore_bc bc_val;
	struct bore_ctx *parent_ctx;
	struct bore_bc *bc;

	if (clone_flags & CLONE_PARENT)
		parent = rcu_dereference(parent->real_parent);

	if (!parent)
		return 0;

	parent_ctx = bore_get_ctx(parent);
	bc = &parent_ctx->subtree;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *child;
		u32 count = 0, total = 0, scan_count = 0;

		list_for_each_entry_rcu(child, &parent->children, sibling) {
			struct bore_ctx *child_ctx;

			if (count >= BURST_CACHE_SAMPLE_LIMIT)
				break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT)
				break;
			if (!bore_task_is_eligible(child))
				continue;
			child_ctx = bore_get_ctx(child);
			count++;
			total += child_ctx->penalty;
		}

		update_burst_cache(bc, parent, parent_ctx, count, total, now);
	}

	bc_val.value = READ_ONCE(bc->value);
	return (u32)bc_val.penalty;
}

static u32 inherit_from_ancestor_hub(struct task_struct *parent, u64 clone_flags, u64 now)
{
	struct bore_bc bc_val;
	struct task_struct *ancestor = parent;
	struct task_struct *next;
	struct bore_ctx *ancestor_ctx;
	struct bore_bc *bc;
	u32 sole_child_count = 0;

	if (clone_flags & CLONE_PARENT) {
		ancestor = rcu_dereference(ancestor->real_parent);
		sole_child_count = 1;
	}

	if (!ancestor)
		return 0;

	while ((next = rcu_dereference(ancestor->real_parent)) != ancestor &&
	       next != NULL &&
	       count_children_upto2(ancestor) <= sole_child_count) {
		ancestor = next;
		sole_child_count = 1;
	}

	ancestor_ctx = bore_get_ctx(ancestor);
	bc = &ancestor_ctx->subtree;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *direct_child;
		u32 count = 0, total = 0, scan_count = 0;

		list_for_each_entry_rcu(direct_child, &ancestor->children, sibling) {
			struct task_struct *descendant = direct_child;
			struct bore_ctx *desc_ctx;

			if (count >= BURST_CACHE_SAMPLE_LIMIT)
				break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT)
				break;

			while (count_children_upto2(descendant) == 1) {
				struct task_struct *next_descendant =
					list_first_or_null_rcu(&descendant->children, struct task_struct, sibling);
				if (!next_descendant)
					break;
				descendant = next_descendant;
			}

			if (!bore_task_is_eligible(descendant))
				continue;

			desc_ctx = bore_get_ctx(descendant);
			count++;
			total += desc_ctx->penalty;
		}

		update_burst_cache(bc, ancestor, ancestor_ctx, count, total, now);
	}

	bc_val.value = READ_ONCE(bc->value);
	return (u32)bc_val.penalty;
}

static u32 inherit_from_thread_group(struct task_struct *p, u64 now)
{
	struct bore_bc bc_val;
	struct task_struct *leader = p->group_leader;
	struct bore_ctx *leader_ctx;
	struct bore_bc *bc;

	if (!leader)
		return 0;

	leader_ctx = bore_get_ctx(leader);
	bc = &leader_ctx->group;

	if (burst_cache_expired(bc, now)) {
		struct task_struct *sibling;
		u32 count = 0, total = 0, scan_count = 0;

		for_each_thread(leader, sibling) {
			struct bore_ctx *sibling_ctx;

			if (count >= BURST_CACHE_SAMPLE_LIMIT)
				break;
			if (scan_count++ >= BURST_CACHE_SCAN_LIMIT)
				break;
			if (!bore_task_is_eligible(sibling))
				continue;
			sibling_ctx = bore_get_ctx(sibling);
			count++;
			total += sibling_ctx->penalty;
		}

		update_burst_cache(bc, leader, leader_ctx, count, total, now);
	}

	bc_val.value = READ_ONCE(bc->value);
	return (u32)bc_val.penalty;
}

static inline u8 bore_score(struct task_struct *p, struct bore_ctx *ctx)
{
	return (u8)(ctx->penalty >> 8);
}

static inline u8 effective_prio_bore(struct task_struct *p, struct bore_ctx *ctx)
{
	int prio = p->static_prio - MAX_RT_PRIO;
	s32 diff;

	if (sched_bore)
		prio += (int)bore_score(p, ctx);

	prio &= ~(prio >> 31);
	diff = prio - 39;
	prio -= (diff & ~(diff >> 31));

	return (u8)prio;
}

static void update_penalty(struct task_struct *p, struct bore_ctx *ctx, int *new_prio)
{
	u8 prev_prio = effective_prio_bore(p, ctx);
	s32 diff = (s32)ctx->curr_penalty - (s32)ctx->prev_penalty;
	u16 max_val = ctx->curr_penalty - (diff & (diff >> 31));
	u32 is_protected = (p->flags & PF_KTHREAD) || (p->static_prio < 120);
	u8 calculated_new_prio;

	ctx->penalty = max_val & -(s32)(!is_protected);

	calculated_new_prio = effective_prio_bore(p, ctx);
	if (calculated_new_prio != prev_prio) {
		if (new_prio)
			*new_prio = calculated_new_prio;
	}
}

static void restart_burst_bore(struct task_struct *p, struct bore_ctx *ctx, int *new_prio)
{
	u32 new_penalty = binary_smooth(ctx->curr_penalty, ctx->prev_penalty);

	ctx->prev_penalty = (u16)new_penalty;
	ctx->curr_penalty = 0;
	ctx->burst_time = 0;
	update_penalty(p, ctx, new_prio);
}

static void probe_bore_update_curr(void *data, struct task_struct *p, u64 delta_exec, int *new_prio)
{
	struct bore_ctx *ctx;
	u32 curr_penalty;

	if (!sched_bore || !p)
		return;

	ctx = bore_get_ctx(p);
	if (ctx->stop_update)
		return;

	ctx->burst_time += delta_exec;
	curr_penalty = ctx->curr_penalty = calc_burst_penalty(ctx->burst_time);

	if (curr_penalty <= ctx->prev_penalty)
		return;

	update_penalty(p, ctx, new_prio);
}

static void probe_bore_dequeue_task_fair(void *data, struct task_struct *p, int flags)
{
	struct bore_ctx *ctx;

	if (!sched_bore || !p || !(flags & DEQUEUE_SLEEP))
		return;

	ctx = bore_get_ctx(p);
	restart_burst_bore(p, ctx, NULL);
}

static void probe_bore_set_next_entity(void *data, struct sched_entity *se)
{
}

static void probe_bore_yield_task_fair(void *data, struct task_struct *p)
{
	struct bore_ctx *ctx;

	if (!sched_bore || !p)
		return;

	ctx = bore_get_ctx(p);
	restart_burst_bore(p, ctx, NULL);
}

static void probe_bore_task_fork(void *data, struct task_struct *p)
{
	struct bore_ctx *ctx;
	struct task_struct *parent;
	u32 inherited_penalty = 0;
	u64 now;

	if (!sched_bore || !p || !bore_task_is_eligible(p))
		return;

	ctx = bore_get_ctx(p);
	parent = current;
	now = ktime_get_ns();

	rcu_read_lock();
	if (parent && p->group_leader == parent->group_leader && p != parent)
		inherited_penalty = inherit_from_thread_group(parent, now);
	else if (sched_burst_inherit_type == 2)
		inherited_penalty = inherit_from_ancestor_hub(parent, 0, now);
	else if (sched_burst_inherit_type == 1)
		inherited_penalty = inherit_from_parent(parent, 0, now);

	if (ctx->prev_penalty < inherited_penalty)
		ctx->prev_penalty = (u16)inherited_penalty;
	ctx->curr_penalty = 0;
	ctx->burst_time = 0;
	ctx->stop_update = false;
	ctx->futex_waiting = false;

	update_penalty(p, ctx, NULL);
	rcu_read_unlock();
}

static void probe_bore_init_entity(void *data, struct sched_entity *se)
{
	struct bore_ctx *ctx;

	if (!se)
		return;

	ctx = bore_get_ctx_se(se);
	memset(ctx, 0, sizeof(*ctx));
}

static void probe_bore_set_load_weight(void *data, struct task_struct *p, int *prio)
{
	struct bore_ctx *ctx;

	if (!sched_bore || !p || !prio || !bore_task_is_eligible(p))
		return;

	ctx = bore_get_ctx(p);
	*prio = (int)effective_prio_bore(p, ctx);
}

static int bore_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "BORE (Burst-Oriented Response Enhancer) CPU Scheduler 6.8.0\n");
	if (!bore_hooks_active) {
		seq_printf(m, "status:                      disabled (kernel hooks not present / no-op mode)\n");
		return 0;
	}
	seq_printf(m, "sched_bore:                  %u\n", sched_bore);
	seq_printf(m, "sched_burst_inherit_type:    %u\n", sched_burst_inherit_type);
	seq_printf(m, "sched_burst_smoothness:      %u\n", sched_burst_smoothness);
	seq_printf(m, "sched_burst_penalty_offset:  %u\n", sched_burst_penalty_offset);
	seq_printf(m, "sched_burst_penalty_scale:   %u\n", sched_burst_penalty_scale);
	seq_printf(m, "sched_burst_cache_lifetime:  %u\n", sched_burst_cache_lifetime);
	return 0;
}

static int bore_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, bore_proc_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops bore_proc_fops = {
	.proc_open = bore_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations bore_proc_fops = {
	.open = bore_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static struct ctl_table bore_table[] = {
	{
		.procname	= "sched_bore",
		.data		= &sched_bore,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{
		.procname	= "sched_burst_inherit_type",
		.data		= &sched_burst_inherit_type,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{
		.procname	= "sched_burst_smoothness",
		.data		= &sched_burst_smoothness,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{
		.procname	= "sched_burst_penalty_offset",
		.data		= &sched_burst_penalty_offset,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{
		.procname	= "sched_burst_penalty_scale",
		.data		= &sched_burst_penalty_scale,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{
		.procname	= "sched_burst_cache_lifetime",
		.data		= &sched_burst_cache_lifetime,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
	},
	{ }
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
static struct ctl_table bore_root_table[] = {
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= bore_table,
	},
	{ }
};
#endif

static void init_bore_tasks(void)
{
	struct task_struct *p;
	struct task_struct *t;

	rcu_read_lock();
	for_each_process_thread(p, t) {
		struct bore_ctx *ctx = bore_get_ctx(t);
		memset(ctx, 0, sizeof(*ctx));
	}
	rcu_read_unlock();
}

static int __init bore_sched_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct bore_ctx) != 32);

	init_bore_reciprocal_lut();
	init_bore_tasks();

	ret = register_trace_android_vh_bore_update_curr(probe_bore_update_curr, NULL);
	if (ret)
		goto fail_hooks;

	ret = register_trace_android_vh_bore_dequeue_task_fair(probe_bore_dequeue_task_fair, NULL);
	if (ret)
		goto fail_dequeue;

	ret = register_trace_android_vh_bore_set_next_entity(probe_bore_set_next_entity, NULL);
	if (ret)
		goto fail_set_next;

	ret = register_trace_android_vh_bore_yield_task_fair(probe_bore_yield_task_fair, NULL);
	if (ret)
		goto fail_yield;

	ret = register_trace_android_vh_bore_task_fork(probe_bore_task_fork, NULL);
	if (ret)
		goto fail_task_fork;

	ret = register_trace_android_vh_bore_init_entity(probe_bore_init_entity, NULL);
	if (ret)
		goto fail_init_entity;

	ret = register_trace_android_vh_bore_set_load_weight(probe_bore_set_load_weight, NULL);
	if (ret)
		goto fail_set_load_weight;

	bore_hooks_active = true;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	bore_sysctl_header = register_sysctl("kernel", bore_table);
#else
	bore_sysctl_header = register_sysctl_table(bore_root_table);
#endif
	bore_proc_entry = proc_create("bore", 0444, NULL, &bore_proc_fops);

	pr_info("bore_sched: BORE 6.8.0 scheduler module initialized\n");
	return 0;

fail_set_load_weight:
	unregister_trace_android_vh_bore_init_entity(probe_bore_init_entity, NULL);
fail_init_entity:
	unregister_trace_android_vh_bore_task_fork(probe_bore_task_fork, NULL);
fail_task_fork:
	unregister_trace_android_vh_bore_yield_task_fair(probe_bore_yield_task_fair, NULL);
fail_yield:
	unregister_trace_android_vh_bore_set_next_entity(probe_bore_set_next_entity, NULL);
fail_set_next:
	unregister_trace_android_vh_bore_dequeue_task_fair(probe_bore_dequeue_task_fair, NULL);
fail_dequeue:
	unregister_trace_android_vh_bore_update_curr(probe_bore_update_curr, NULL);
fail_hooks:
	pr_info("bore_sched: kernel hooks not present (ret=%d), running in no-op mode\n", ret);
	sched_bore = 0;
	bore_hooks_active = false;
	bore_proc_entry = proc_create("bore", 0444, NULL, &bore_proc_fops);
	return 0;
}

static void __exit bore_sched_exit(void)
{
	if (bore_proc_entry)
		proc_remove(bore_proc_entry);

	if (!bore_hooks_active)
		return;

	if (bore_sysctl_header)
		unregister_sysctl_table(bore_sysctl_header);

	unregister_trace_android_vh_bore_set_load_weight(probe_bore_set_load_weight, NULL);
	unregister_trace_android_vh_bore_init_entity(probe_bore_init_entity, NULL);
	unregister_trace_android_vh_bore_task_fork(probe_bore_task_fork, NULL);
	unregister_trace_android_vh_bore_yield_task_fair(probe_bore_yield_task_fair, NULL);
	unregister_trace_android_vh_bore_set_next_entity(probe_bore_set_next_entity, NULL);
	unregister_trace_android_vh_bore_dequeue_task_fair(probe_bore_dequeue_task_fair, NULL);
	unregister_trace_android_vh_bore_update_curr(probe_bore_update_curr, NULL);

	pr_info("bore_sched: BORE 6.8.0 scheduler module unloaded\n");
}

module_init(bore_sched_init);
module_exit(bore_sched_exit);

MODULE_AUTHOR("Masahito S. <firelzrd@gmail.com>");
MODULE_AUTHOR("AxionOS");
MODULE_DESCRIPTION("BORE (Burst-Oriented Response Enhancer) CPU Scheduler Module");
MODULE_LICENSE("GPL");
