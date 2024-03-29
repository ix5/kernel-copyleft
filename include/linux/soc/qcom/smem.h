/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NOTE: This file has been modified by Sony Mobile Communications Inc.
 * Modifications are Copyright (c) 2015 Sony Mobile Communications Inc,
 * and licensed under the license of the file.
 */
#ifndef __QCOM_SMEM_H__
#define __QCOM_SMEM_H__

#include <linux/types.h>

#define QCOM_SMEM_HOST_ANY -1

int qcom_smem_alloc(unsigned host, unsigned item, size_t size);
void *qcom_smem_get(unsigned host, unsigned item, size_t *size);

int qcom_smem_get_free_space(unsigned host);
phys_addr_t qcom_smem_virt_to_phys(void *addr);

#endif
