#undef TRACE_SYSTEM
#define TRACE_SYSTEM bore

#define TRACE_INCLUDE_PATH trace/hooks

#if !defined(_TRACE_HOOK_BORE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_BORE_H

#include <linux/tracepoint.h>
#include <trace/hooks/vendor_hooks.h>

struct task_struct;
struct sched_entity;

DECLARE_HOOK(android_vh_bore_update_curr,
	TP_PROTO(struct task_struct *p, u64 delta_exec, int *new_prio),
	TP_ARGS(p, delta_exec, new_prio));

DECLARE_HOOK(android_vh_bore_dequeue_task_fair,
	TP_PROTO(struct task_struct *p, int flags),
	TP_ARGS(p, flags));

DECLARE_HOOK(android_vh_bore_set_next_entity,
	TP_PROTO(struct sched_entity *se),
	TP_ARGS(se));

DECLARE_HOOK(android_vh_bore_yield_task_fair,
	TP_PROTO(struct task_struct *p),
	TP_ARGS(p));

DECLARE_HOOK(android_vh_bore_task_fork,
	TP_PROTO(struct task_struct *p),
	TP_ARGS(p));

DECLARE_HOOK(android_vh_bore_init_entity,
	TP_PROTO(struct sched_entity *se),
	TP_ARGS(se));

DECLARE_HOOK(android_vh_bore_set_load_weight,
	TP_PROTO(struct task_struct *p, int *prio),
	TP_ARGS(p, prio));

#endif

#include <trace/define_trace.h>

