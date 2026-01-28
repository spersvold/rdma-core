/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2019-2020 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __NERVE_ABI_H__
#define __NERVE_ABI_H__

#include <infiniband/kern-abi.h>
#include <kernel-abi/nerve-abi.h>
#include <rdma/nerve-abi.h>

#define NERVE_ABI_VERSION 1

DECLARE_DRV_CMD(nerve_alloc_ucontext, IB_USER_VERBS_CMD_GET_CONTEXT,
		nerve_ibv_alloc_ucontext_cmd, nerve_ibv_alloc_ucontext_resp);
DECLARE_DRV_CMD(nerve_alloc_pd, IB_USER_VERBS_CMD_ALLOC_PD, empty,
		nerve_ibv_alloc_pd_resp);
DECLARE_DRV_CMD(nerve_create_cq, IB_USER_VERBS_EX_CMD_CREATE_CQ,
		nerve_ibv_create_cq, nerve_ibv_create_cq_resp);
DECLARE_DRV_CMD(nerve_create_qp, IB_USER_VERBS_CMD_CREATE_QP, nerve_ibv_create_qp,
		nerve_ibv_create_qp_resp);
DECLARE_DRV_CMD(nerve_create_ah, IB_USER_VERBS_CMD_CREATE_AH, empty,
		nerve_ibv_create_ah_resp);
DECLARE_DRV_CMD(nerve_query_device_ex, IB_USER_VERBS_EX_CMD_QUERY_DEVICE, empty,
		nerve_ibv_ex_query_device_resp);

#endif /* __NERVE_ABI_H__ */
