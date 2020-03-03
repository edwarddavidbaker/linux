/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef INTEL_QOS_H
#define INTEL_QOS_H

#include "intel_qos_types.h"

void intel_qos_init(struct intel_qos *qos);
void intel_qos_fini(struct intel_qos *qos);

void intel_qos_overload_begin(struct intel_qos *qos);
void intel_qos_overload_end(struct intel_qos *qos);

#endif /* INTEL_QOS_H */
