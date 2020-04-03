/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_QOS_TYPES__
#define __INTEL_QOS_TYPES__

#include <linux/atomic.h>
#include <linux/pm_qos.h>
#include <linux/timer.h>

struct intel_qos {
	/** PM QoS request of this device. */
	struct pm_qos_request req;

	/** Timer used for delayed update of the PM QoS request. */
	struct timer_list timer;

	/** Response frequency target to use in GPU-bound conditions. */
	u32 target_hz;

	/**
	 * Maximum delay before the PM QoS request is updated
	 * after we become GPU-bound.
	 */
	u32 delay_max_ns;

	/**
	 * Exponent of delay slope used when the workload
	 * becomes non-GPU-bound, used to provide greater
	 * sensitivity to periods of GPU inactivity which may
	 * indicate that the workload is latency-bound.
	 */
	u32 delay_slope_shift;
	u32 debug;

	/**
	 * Last time intel_gt_pm_active_begin() was called to
	 * indicate that the GPU is a bottleneck.
	 */
	atomic64_t time_set_ns;

	/**
	 * Last time intel_gt_pm_active_end() was called to
	 * indicate that the GPU is no longer a bottleneck.
	 */
	atomic64_t time_clear_ns;

	/**
	 * Number of times intel_gt_pm_active_begin() was
	 * called without a matching intel_gt_pm_active_end().
	 * Will be greater than zero if the GPU is currently
	 * considered to be a bottleneck.
	 */
	atomic_t active_count;
};

#endif /* __INTEL_QOS_TYPES_H__ */
