/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_params.h"
#include "intel_engine_pm.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_pm.h"
#include "intel_wakeref.h"

static void pm_notify(struct drm_i915_private *i915, int state)
{
	blocking_notifier_call_chain(&i915->gt.pm_notifications, state, i915);
}

static int __gt_unpark(struct intel_wakeref *wf)
{
	struct intel_gt *gt = container_of(wf, typeof(*gt), wakeref);
	struct drm_i915_private *i915 = gt->i915;

	GEM_TRACE("\n");

	/*
	 * It seems that the DMC likes to transition between the DC states a lot
	 * when there are no connected displays (no active power domains) during
	 * command submission.
	 *
	 * This activity has negative impact on the performance of the chip with
	 * huge latencies observed in the interrupt handler and elsewhere.
	 *
	 * Work around it by grabbing a GT IRQ power domain whilst there is any
	 * GT activity, preventing any DC state transitions.
	 */
	gt->awake = intel_display_power_get(i915, POWER_DOMAIN_GT_IRQ);
	GEM_BUG_ON(!gt->awake);

	if (NEEDS_RC6_CTX_CORRUPTION_WA(i915))
		intel_uncore_forcewake_get(&i915->uncore, FORCEWAKE_ALL);

	intel_enable_gt_powersave(i915);

	i915_update_gfx_val(i915);
	if (INTEL_GEN(i915) >= 6)
		gen6_rps_busy(i915);

	i915_pmu_gt_unparked(i915);

	intel_gt_queue_hangcheck(gt);

	pm_notify(i915, INTEL_GT_UNPARK);

	return 0;
}

static int __gt_park(struct intel_wakeref *wf)
{
	struct drm_i915_private *i915 =
		container_of(wf, typeof(*i915), gt.wakeref);
	intel_wakeref_t wakeref = fetch_and_zero(&i915->gt.awake);

	GEM_TRACE("\n");

	pm_notify(i915, INTEL_GT_PARK);

	i915_pmu_gt_parked(i915);
	if (INTEL_GEN(i915) >= 6)
		gen6_rps_idle(i915);

	if (NEEDS_RC6_CTX_CORRUPTION_WA(i915)) {
		i915_rc6_ctx_wa_check(i915);
		intel_uncore_forcewake_put(&i915->uncore, FORCEWAKE_ALL);
	}

	/* Everything switched off, flush any residual interrupt just in case */
	intel_synchronize_irq(i915);

	GEM_BUG_ON(!wakeref);
	intel_display_power_put(i915, POWER_DOMAIN_GT_IRQ, wakeref);

	return 0;
}

static const struct intel_wakeref_ops wf_ops = {
	.get = __gt_unpark,
	.put = __gt_park,
	.flags = INTEL_WAKEREF_PUT_ASYNC,
};

/**
 * Time increment until the most immediate PM QoS response frequency
 * update.
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
static int32_t time_to_rf_qos_update_ns(struct intel_gt *gt)
{
	const uint64_t t1 = ktime_get_ns();
	const uint64_t dt1 = gt->rf_qos.delay_max_ns;

	if (atomic_read_acquire(&gt->rf_qos.active_count)) {
		const uint64_t t0 = atomic64_read(&gt->rf_qos.time_set_ns);

		return min(dt1, t0 <= t1 ? 0 : t0 - t1);
	} else {
		const uint64_t t0 = atomic64_read(&gt->rf_qos.time_clear_ns);
		const unsigned int shift = gt->rf_qos.delay_slope_shift;

		return -(int32_t)(t1 <= t0 ? 1 :
				  min(dt1, (t1 - t0) << shift));
	}
}

/**
 * Perform a delayed PM QoS response frequency update.
 */
static void intel_gt_rf_qos_update(struct intel_gt *gt)
{
	const uint32_t dt = max(0, time_to_rf_qos_update_ns(gt));

	timer_reduce(&gt->rf_qos.timer, jiffies + nsecs_to_jiffies(dt));
}

/**
 * Timer that fires once the delay used to switch the PM QoS response
 * frequency request has elapsed.
 */
static void intel_gt_rf_qos_timeout(struct timer_list *timer)
{
	struct intel_gt *gt = container_of(timer, struct intel_gt,
					   rf_qos.timer);
	const int32_t dt = time_to_rf_qos_update_ns(gt);

	if (dt == 0)
		pm_qos_update_request(&gt->rf_qos.req, gt->rf_qos.target_hz);
	else
		pm_qos_update_request(&gt->rf_qos.req, PM_QOS_DEFAULT_VALUE);

	if (dt > 0)
		intel_gt_rf_qos_update(gt);
}

/**
 * Report the beginning of a period of GPU utilization to PM.
 *
 * May trigger a more energy-efficient response mode in CPU PM, but
 * only after a certain delay has elapsed so we don't have a negative
 * impact on the CPU ramp-up latency except after the GPU has been
 * continuously utilized for a long enough period of time.
 */
void intel_gt_pm_active_begin(struct intel_gt *gt)
{
	const uint32_t dt = abs(time_to_rf_qos_update_ns(gt));

	atomic64_set(&gt->rf_qos.time_set_ns, ktime_get_ns() + dt);

	if (!atomic_fetch_inc_release(&gt->rf_qos.active_count))
		intel_gt_rf_qos_update(gt);
}

/**
 * Report the end of a period of GPU utilization to PM.
 *
 * Must be called once after each call to intel_gt_pm_active_begin().
 */
void intel_gt_pm_active_end(struct intel_gt *gt)
{
	const uint32_t dt = abs(time_to_rf_qos_update_ns(gt));
	const unsigned int shift = gt->rf_qos.delay_slope_shift;

	atomic64_set(&gt->rf_qos.time_clear_ns, ktime_get_ns() - (dt >> shift));

	if (!atomic_dec_return_release(&gt->rf_qos.active_count))
		intel_gt_rf_qos_update(gt);
}

void intel_gt_pm_init_early(struct intel_gt *gt)
{
	intel_wakeref_init(&gt->wakeref, &gt->i915->runtime_pm, &wf_ops);

	BLOCKING_INIT_NOTIFIER_HEAD(&gt->pm_notifications);

	pm_qos_add_request(&gt->rf_qos.req, PM_QOS_CPU_RESPONSE_FREQUENCY,
			   PM_QOS_DEFAULT_VALUE);

	gt->rf_qos.delay_max_ns = 250000;
	gt->rf_qos.delay_slope_shift = 0;
	gt->rf_qos.target_hz = 2;
	timer_setup(&gt->rf_qos.timer, intel_gt_rf_qos_timeout, 0);
}

static bool reset_engines(struct intel_gt *gt)
{
	if (INTEL_INFO(gt->i915)->gpu_reset_clobbers_display)
		return false;

	return __intel_gt_reset(gt, ALL_ENGINES) == 0;
}

/**
 * intel_gt_sanitize: called after the GPU has lost power
 * @gt: the i915 GT container
 * @force: ignore a failed reset and sanitize engine state anyway
 *
 * Anytime we reset the GPU, either with an explicit GPU reset or through a
 * PCI power cycle, the GPU loses state and we must reset our state tracking
 * to match. Note that calling intel_gt_sanitize() if the GPU has not
 * been reset results in much confusion!
 */
void intel_gt_sanitize(struct intel_gt *gt, bool force)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	GEM_TRACE("\n");

	intel_uc_sanitize(&gt->uc);

	if (!reset_engines(gt) && !force)
		return;

	for_each_engine(engine, gt->i915, id)
		__intel_engine_reset(engine, false);
}

int intel_gt_resume(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = 0;

	/*
	 * After resume, we may need to poke into the pinned kernel
	 * contexts to paper over any damage caused by the sudden suspend.
	 * Only the kernel contexts should remain pinned over suspend,
	 * allowing us to fixup the user contexts on their first pin.
	 */
	intel_gt_pm_get(gt);
	for_each_engine(engine, gt->i915, id) {
		struct intel_context *ce;

		intel_engine_pm_get(engine);

		ce = engine->kernel_context;
		if (ce)
			ce->ops->reset(ce);

		engine->serial++; /* kernel context lost */
		err = engine->resume(engine);

		intel_engine_pm_put(engine);
		if (err) {
			dev_err(gt->i915->drm.dev,
				"Failed to restart %s (%d)\n",
				engine->name, err);
			break;
		}
	}
	intel_gt_pm_put(gt);

	return err;
}

void intel_gt_runtime_suspend(struct intel_gt *gt)
{
	intel_uc_runtime_suspend(&gt->uc);
}

int intel_gt_runtime_resume(struct intel_gt *gt)
{
	intel_gt_init_swizzling(gt);

	return intel_uc_runtime_resume(&gt->uc);
}
