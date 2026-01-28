// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright 2019-2025 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <util/util.h>

#include "nerve.h"
#include "verbs.h"

static void nerve_free_context(struct ibv_context *ibvctx);

#define PCI_VENDOR_ID_AMAZON 0x1d0f

static const struct verbs_match_ent nerve_table[] = {
	VERBS_DRIVER_ID(RDMA_DRIVER_NERVE),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_AMAZON, 0xefa0, NULL),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_AMAZON, 0xefa1, NULL),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_AMAZON, 0xefa2, NULL),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_AMAZON, 0xefa3, NULL),
	{}
};

static const struct verbs_context_ops nerve_ctx_ops = {
	.alloc_pd = nerve_alloc_pd,
	.alloc_parent_domain = nerve_alloc_parent_domain,
	.alloc_td = nerve_alloc_td,
	.create_ah = nerve_create_ah,
	.create_cq = nerve_create_cq,
	.create_cq_ex = nerve_create_cq_ex,
	.create_qp = nerve_create_qp,
	.create_qp_ex = nerve_create_qp_ex,
	.cq_event = nerve_cq_event,
	.dealloc_pd = nerve_dealloc_pd,
	.dealloc_td = nerve_dealloc_td,
	.dereg_mr = nerve_dereg_mr,
	.destroy_ah = nerve_destroy_ah,
	.destroy_cq = nerve_destroy_cq,
	.destroy_qp = nerve_destroy_qp,
	.modify_qp = nerve_modify_qp,
	.poll_cq = nerve_poll_cq,
	.post_recv = nerve_post_recv,
	.post_send = nerve_post_send,
	.query_device_ex = nerve_query_device_ex,
	.query_port = nerve_query_port,
	.query_qp = nerve_query_qp,
	.query_qp_data_in_order = nerve_query_qp_data_in_order,
	.reg_dmabuf_mr = nerve_reg_dmabuf_mr,
	.reg_mr = nerve_reg_mr,
	.req_notify_cq = nerve_arm_cq,
	.free_context = nerve_free_context,
};

static struct verbs_context *nerve_alloc_context(struct ibv_device *vdev,
					       int cmd_fd,
					       void *private_data)
{
	struct nerve_alloc_ucontext_resp resp = {};
	struct nerve_alloc_ucontext cmd = {};
	struct nerve_context *ctx;

	cmd.comp_mask |= NERVE_ALLOC_UCONTEXT_CMD_COMP_TX_BATCH;
	cmd.comp_mask |= NERVE_ALLOC_UCONTEXT_CMD_COMP_MIN_SQ_WR;

	ctx = verbs_init_and_alloc_context(vdev, cmd_fd, ctx, ibvctx,
					   RDMA_DRIVER_NERVE);
	if (!ctx)
		return NULL;

	if (ibv_cmd_get_context(&ctx->ibvctx, &cmd.ibv_cmd, sizeof(cmd),
				NULL, &resp.ibv_resp, sizeof(resp))) {
		verbs_err(&ctx->ibvctx, "ibv_cmd_get_context failed\n");
		goto err_free_ctx;
	}

	ctx->sub_cqs_per_cq = resp.sub_cqs_per_cq;
	ctx->cmds_supp_udata_mask = resp.cmds_supp_udata_mask;
	ctx->cqe_size = sizeof(struct nerve_io_rx_cdesc);
	ctx->ex_cqe_size = sizeof(struct nerve_io_rx_cdesc_ex);
	ctx->inline_buf_size = resp.inline_buf_size;
	ctx->max_llq_size = resp.max_llq_size;
	ctx->max_tx_batch = resp.max_tx_batch;
	ctx->min_sq_wr = resp.min_sq_wr;
	pthread_spin_init(&ctx->qp_table_lock, PTHREAD_PROCESS_PRIVATE);

	/* ah udata is mandatory for ah number retrieval */
	if (!(ctx->cmds_supp_udata_mask & NERVE_USER_CMDS_SUPP_UDATA_CREATE_AH)) {
		verbs_err(&ctx->ibvctx, "Kernel does not support AH udata\n");
		goto err_free_spinlock;
	}

	verbs_set_ops(&ctx->ibvctx, &nerve_ctx_ops);

	if (nerve_query_device_ctx(ctx))
		goto err_free_spinlock;
	return &ctx->ibvctx;

err_free_spinlock:
	pthread_spin_destroy(&ctx->qp_table_lock);
err_free_ctx:
	verbs_uninit_context(&ctx->ibvctx);
	free(ctx);
	return NULL;
}

static void nerve_free_context(struct ibv_context *ibvctx)
{
	struct nerve_context *ctx = to_nerve_context(ibvctx);

	free(ctx->qp_table);
	pthread_spin_destroy(&ctx->qp_table_lock);
	verbs_uninit_context(&ctx->ibvctx);
	free(ctx);
}

static struct verbs_device *nerve_device_alloc(struct verbs_sysfs_dev *sysfs_dev)
{
	struct nerve_dev *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	dev->pg_sz = sysconf(_SC_PAGESIZE);

	return &dev->vdev;
}

static void nerve_uninit_device(struct verbs_device *verbs_device)
{
	struct nerve_dev *dev = to_nerve_dev(&verbs_device->device);

	free(dev);
}

static const struct verbs_device_ops nerve_dev_ops = {
	.name = "nerve",
	.match_min_abi_version = NERVE_ABI_VERSION,
	.match_max_abi_version = NERVE_ABI_VERSION,
	.match_table = nerve_table,
	.alloc_device = nerve_device_alloc,
	.uninit_device = nerve_uninit_device,
	.alloc_context = nerve_alloc_context,
};

bool is_nerve_dev(struct ibv_device *device)
{
	struct verbs_device *verbs_device = verbs_get_device(device);

	return verbs_device->ops == &nerve_dev_ops;
}
PROVIDER_DRIVER(nerve, nerve_dev_ops);
