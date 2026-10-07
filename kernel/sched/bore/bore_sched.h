/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021-2024 Masahito S. <firelzrd@gmail.com>
 * Copyright (C) 2025-2026 AxionOS
 */

#ifndef _BORE_SCHED_H
#define _BORE_SCHED_H

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/types.h>

#define MAX_BURST_PENALTY ((40U << 8) - 1)
#define BORE_BC_TIMESTAMP_SHIFT 16
#define BURST_CACHE_SAMPLE_LIMIT 63
#define BURST_CACHE_SCAN_LIMIT 126

#ifndef task_of
#define task_of(_se) container_of(_se, struct task_struct, se)
#endif

#ifndef DEQUEUE_SLEEP
#define DEQUEUE_SLEEP 0x01
#endif

#define DEFAULT_SCHED_BORE 1
#define DEFAULT_BURST_INHERIT_TYPE 2
#define DEFAULT_BURST_SMOOTHNESS 1
#define DEFAULT_BURST_PENALTY_OFFSET 24
#define DEFAULT_BURST_PENALTY_SCALE 1536
#define DEFAULT_BURST_CACHE_LIFETIME 75000000U

extern uint sched_bore;
extern uint sched_burst_inherit_type;
extern uint sched_burst_smoothness;
extern uint sched_burst_penalty_offset;
extern uint sched_burst_penalty_scale;
extern uint sched_burst_cache_lifetime;

struct bore_bc {
	union {
		struct {
			u64 timestamp: 48;
			u64 penalty:   16;
		};
		u64 value;
	};
};

struct bore_ctx {
	u64 burst_time;
	u16 prev_penalty;
	u16 curr_penalty;
	u16 penalty;
	u8 stop_update;
	u8 futex_waiting;
	struct bore_bc subtree;
	struct bore_bc group;
};

/* bore_ctx lives in the sched_entity's ANDROID_KABI_RESERVE(1..4). */
static_assert(sizeof(struct bore_ctx) <= 4 * sizeof(u64),
	      "bore_ctx does not fit the sched_entity's reserved fields");

static inline struct bore_ctx *bore_get_ctx_se(struct sched_entity *se)
{
	return (struct bore_ctx *)&se->android_kabi_reserved1;
}

static inline struct bore_ctx *bore_get_ctx(struct task_struct *p)
{
	return bore_get_ctx_se(&p->se);
}

static inline bool bore_entity_is_task(struct sched_entity *se)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	return !se->my_q;
#else
	return true;
#endif
}

static inline bool bore_task_is_eligible(struct task_struct *p)
{
	if (p->policy != SCHED_NORMAL && p->policy != SCHED_BATCH && p->policy != SCHED_IDLE)
		return false;
	if (p->flags & PF_IDLE)
		return false;
	return true;
}

static inline u32 log2p1_u64_u32fp(u64 v, u8 fp)
{
	int clz;
	int exponent;
	u32 mantissa;

	if (unlikely(!v))
		return 0;

	clz = __builtin_clzll(v);
	exponent = 64 - clz;
	mantissa = (u32)((v << clz) << 1 >> (64 - fp));
	return (u32)(exponent << fp | mantissa);
}

static inline u32 calc_burst_penalty(u64 burst_time)
{
	u32 greed = log2p1_u64_u32fp(burst_time, 8);
	u32 tolerance = sched_burst_penalty_offset << 8;
	s32 diff = (s32)(greed - tolerance);
	u32 penalty = diff & ~(diff >> 31);
	u32 scaled_penalty = (penalty * sched_burst_penalty_scale) >> 10;
	s32 overflow = scaled_penalty - MAX_BURST_PENALTY;
	return scaled_penalty - (overflow & ~(overflow >> 31));
}

static inline u32 binary_smooth(u32 new_val, u32 old_val)
{
	u32 is_growing = (new_val > old_val);
	u32 increment = (new_val - old_val) * is_growing;
	u32 shift = sched_burst_smoothness;
	u32 smoothed = old_val + ((increment + (1U << shift) - 1) >> shift);
	return (new_val & ~(-is_growing)) | (smoothed & (-is_growing));
}

#endif
