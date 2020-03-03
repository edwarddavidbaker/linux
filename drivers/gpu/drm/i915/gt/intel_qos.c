// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */

#include "intel_qos.h"

/**
 * Time increment until the most immediate PM QoS scaling response
 * frequency update.
 *
 * May be in the future (return value > 0) if the GPU is currently
 * active but we haven't updated the PM QoS request to reflect a
 * bottleneck yet.  May be in the past (return value < 0) if the GPU
 * isn't fully utilized and we've already reset the PM QoS request to
 * the default value.  May be zero if a PM QoS request update is due.
 *
 * The time increment returned by this function decreases linearly
 * with time until it reaches either zero or a configurable limit.
 */
static s32 time_to_sf_qos_update_ns(struct intel_qos *qos)
{
	const u64 t1 = ktime_get_ns();
	const u64 dt1 = qos->delay_max_ns;

	if (atomic_read_acquire(&qos->active_count)) {
		const u64 t0 = atomic64_read(&qos->time_set_ns);

		return min(dt1, t0 <= t1 ? 0 : t0 - t1);
	} else {
		const u64 t0 = atomic64_read(&qos->time_clear_ns);
		const unsigned int shift = qos->delay_slope_shift;

		return -(s32)(t1 <= t0 ? 1 : min(dt1, (t1 - t0) << shift));
	}
}

/**
 * Perform a delayed PM QoS scaling response frequency update.
 */
static void intel_qos_update(struct intel_qos *qos)
{
	const u32 dt = max(0, time_to_sf_qos_update_ns(qos));

	timer_reduce(&qos->timer, jiffies + nsecs_to_jiffies(dt));
}

/**
 * Timer that fires once the delay used to switch the PM QoS scaling
 * response frequency request has elapsed.
 */
static void intel_qos_timeout(struct timer_list *timer)
{
	struct intel_qos *qos = from_timer(qos, timer, timer);
	const s32 dt = time_to_sf_qos_update_ns(qos);
	s32 value;

	value = PM_QOS_DEFAULT_VALUE;
	if (dt == 0)
		value = qos->target_hz;
	cpu_scaling_response_qos_update_request(&qos->req, value);

	if (dt > 0)
		intel_qos_update(qos);
}

/**
 * Report the beginning of a period of GPU utilization to PM.
 *
 * May trigger a more energy-efficient response mode in CPU PM, but
 * only after a certain delay has elapsed so we don't have a negative
 * impact on the CPU ramp-up latency except after the GPU has been
 * continuously utilized for a long enough period of time.
 */
void intel_qos_overload_begin(struct intel_qos *qos)
{
	const u32 dt = abs(time_to_sf_qos_update_ns(qos));

	atomic64_set(&qos->time_set_ns, ktime_get_ns() + dt);

	if (!atomic_fetch_inc_release(&qos->active_count))
		intel_qos_update(qos);
}

/**
 * Report the end of a period of GPU utilization to PM.
 *
 * Must be called once after each call to intel_gt_pm_active_begin().
 */
void intel_qos_overload_end(struct intel_qos *qos)
{
	const u32 dt = abs(time_to_sf_qos_update_ns(qos));
	const unsigned int shift = qos->delay_slope_shift;

	atomic64_set(&qos->time_clear_ns, ktime_get_ns() - (dt >> shift));

	if (!atomic_dec_return_release(&qos->active_count))
		intel_qos_update(qos);
}

void intel_qos_init(struct intel_qos *qos)
{
	cpu_scaling_response_qos_add_request(&qos->req, PM_QOS_DEFAULT_VALUE);

	qos->delay_max_ns = 10000000;
	qos->delay_slope_shift = 1;
	qos->target_hz = 2;
	timer_setup(&qos->timer, intel_qos_timeout, 0);
}

void intel_qos_fini(struct intel_qos *qos)
{
	del_timer_sync(&qos->timer);
	cpu_scaling_response_qos_remove_request(&qos->req);
}
