/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NOTE: This file has been modified by Sony Mobile Communications Inc.
 * Modifications are Copyright (c) 2014 Sony Mobile Communications Inc,
 * and licensed under the license of the file.
 */
#ifndef __QCOM_RPM_H__
#define __QCOM_RPM_H__

#include <linux/types.h>

struct qcom_rpm;

#define QCOM_RPM_ACTIVE_STATE	0
#define QCOM_RPM_SLEEP_STATE	1

int qcom_rpm_write(struct qcom_rpm *rpm, int state, int resource, u32 *buf, size_t count);

#endif
