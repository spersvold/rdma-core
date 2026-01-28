// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright 2019-2025 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ccan/minmax.h>

#include <util/compiler.h>
#include <util/mmio.h>
#include <util/util.h>

#include "nerve.h"
#include "nerve_io_regs_defs.h"
#include "nervedv.h"
#include "verbs.h"

#define NERVE_DEV_CAP(ctx, cap) \
	((ctx)->device_caps & NERVE_QUERY_DEVICE_CAPS_##cap)

static bool is_buf_cleared(void *buf, size_t len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (((uint8_t *)buf)[i])
			return false;
	}

	return true;
}

#define min3(a, b, c) \
	({ \
		typeof(a) _tmpmin = min(a, b); \
		min(_tmpmin, c); \
	})

#define is_ext_cleared(ptr, inlen) \
	is_buf_cleared((uint8_t *)ptr + sizeof(*ptr), inlen - sizeof(*ptr))

#define is_reserved_cleared(reserved) is_buf_cleared(reserved, sizeof(reserved))

struct nerve_wq_init_attr {
	uint64_t db_mmap_key;
	uint32_t db_off;
	int cmd_fd;
	int pgsz;
	uint16_t sub_cq_idx;
	bool need_lock;
};

int nerve_query_port(struct ibv_context *ibvctx, uint8_t port,
		   struct ibv_port_attr *port_attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(ibvctx, port, port_attr, &cmd, sizeof(cmd));
}

int nerve_query_device_ex(struct ibv_context *context,
			const struct ibv_query_device_ex_input *input,
			struct ibv_device_attr_ex *attr,
			size_t attr_size)
{
	struct nerve_context *ctx = to_nerve_context(context);
	struct ibv_device_attr *a = &attr->orig_attr;
	struct nerve_query_device_ex_resp resp = {};
	size_t resp_size = (ctx->cmds_supp_udata_mask &
			    NERVE_USER_CMDS_SUPP_UDATA_QUERY_DEVICE) ?
				   sizeof(resp) :
				   sizeof(resp.ibv_resp);
	uint8_t fw_ver[8];
	int err;

	err = ibv_cmd_query_device_any(context, input, attr, attr_size,
				       &resp.ibv_resp, &resp_size);
	if (err) {
		verbs_err(verbs_get_ctx(context), "ibv_cmd_query_device_any failed\n");
		return err;
	}

	a->max_qp_wr = min_t(int, a->max_qp_wr,
			     ctx->max_llq_size / sizeof(struct nerve_io_tx_wqe));
	memcpy(fw_ver, &resp.ibv_resp.base.fw_ver,
	       sizeof(resp.ibv_resp.base.fw_ver));
	snprintf(a->fw_ver, sizeof(a->fw_ver), "%u.%u.%u.%u",
		 fw_ver[0], fw_ver[1], fw_ver[2], fw_ver[3]);

	return 0;
}

int nerve_query_device_ctx(struct nerve_context *ctx)
{
	struct nerve_query_device_ex_resp resp = {};
	struct ibv_device_attr_ex attr;
	size_t resp_size = sizeof(resp);
	unsigned int qp_table_sz;
	int err;

	if (ctx->cmds_supp_udata_mask & NERVE_USER_CMDS_SUPP_UDATA_QUERY_DEVICE) {
		err = ibv_cmd_query_device_any(&ctx->ibvctx.context, NULL,
					       &attr, sizeof(attr),
					       &resp.ibv_resp, &resp_size);
		if (err) {
			verbs_err(&ctx->ibvctx,
				  "ibv_cmd_query_device_any failed\n");
			return err;
		}

		ctx->device_caps = resp.device_caps;
		ctx->max_sq_wr = resp.max_sq_wr;
		ctx->max_rq_wr = resp.max_rq_wr;
		ctx->max_sq_sge = resp.max_sq_sge;
		ctx->max_rq_sge = resp.max_rq_sge;
		ctx->max_rdma_size = resp.max_rdma_size;
	} else {
		err = ibv_cmd_query_device_any(&ctx->ibvctx.context, NULL,
					       &attr, sizeof(attr.orig_attr),
					       NULL, NULL);
		if (err) {
			verbs_err(&ctx->ibvctx,
				  "ibv_cmd_query_device_any failed\n");
			return err;
		}
	}

	ctx->max_wr_rdma_sge = attr.orig_attr.max_sge_rd;
	qp_table_sz = roundup_pow_of_two(attr.orig_attr.max_qp);
	ctx->qp_table_sz_m1 = qp_table_sz - 1;
	ctx->qp_table = calloc(qp_table_sz, sizeof(*ctx->qp_table));
	if (!ctx->qp_table)
		return ENOMEM;
	return 0;
}

int nervedv_query_device(struct ibv_context *ibvctx,
		       struct nervedv_device_attr *attr,
		       uint32_t inlen)
{
	struct nerve_context *ctx = to_nerve_context(ibvctx);
	uint64_t comp_mask_out = 0;

	if (!is_nerve_dev(ibvctx->device)) {
		verbs_err(verbs_get_ctx(ibvctx), "Not an NERVE device\n");
		return EOPNOTSUPP;
	}

	if (!vext_field_avail(typeof(*attr), inline_buf_size, inlen)) {
		verbs_err(verbs_get_ctx(ibvctx), "Compatibility issues\n");
		return EINVAL;
	}

	memset(attr, 0, inlen);
	attr->max_sq_wr = ctx->max_sq_wr;
	attr->max_rq_wr = ctx->max_rq_wr;
	attr->max_sq_sge = ctx->max_sq_sge;
	attr->max_rq_sge = ctx->max_rq_sge;
	attr->inline_buf_size = ctx->inline_buf_size;

	if (vext_field_avail(typeof(*attr), device_caps, inlen)) {
		if (NERVE_DEV_CAP(ctx, RNR_RETRY))
			attr->device_caps |= NERVEDV_DEVICE_ATTR_CAPS_RNR_RETRY;

		if (NERVE_DEV_CAP(ctx, CQ_WITH_SGID))
			attr->device_caps |= NERVEDV_DEVICE_ATTR_CAPS_CQ_WITH_SGID;

		if (NERVE_DEV_CAP(ctx, UNSOLICITED_WRITE_RECV))
			attr->device_caps |= NERVEDV_DEVICE_ATTR_CAPS_UNSOLICITED_WRITE_RECV;

		if (NERVE_DEV_CAP(ctx, CQ_WITH_EXT_MEM))
			attr->device_caps |= NERVEDV_DEVICE_ATTR_CAPS_CQ_WITH_EXT_MEM_DMABUF;
	}

	if (vext_field_avail(typeof(*attr), max_rdma_size, inlen)) {
		attr->max_rdma_size = ctx->max_rdma_size;

		if (NERVE_DEV_CAP(ctx, RDMA_READ))
			attr->device_caps |= NERVEDV_DEVICE_ATTR_CAPS_RDMA_READ;

		if (NERVE_DEV_CAP(ctx, RDMA_WRITE))
			attr->device_caps |= NERVEDV_DEVICE_ATTR_CAPS_RDMA_WRITE;
	}

	attr->comp_mask = comp_mask_out;

	return 0;
}

struct ibv_pd *nerve_alloc_pd(struct ibv_context *ibvctx)
{
	struct nerve_alloc_pd_resp resp = {};
	struct ibv_alloc_pd cmd;
	struct nerve_pd *pd;
	int err;

	pd = calloc(1, sizeof(*pd));
	if (!pd)
		return NULL;

	err = ibv_cmd_alloc_pd(ibvctx, &pd->ibvpd, &cmd, sizeof(cmd),
			       &resp.ibv_resp, sizeof(resp));
	if (err) {
		verbs_err(verbs_get_ctx(ibvctx), "Failed to allocate PD\n");
		goto out;
	}

	atomic_init(&pd->refcount, 0);
	pd->pdn = resp.pdn;

	return &pd->ibvpd;

out:
	free(pd);
	errno = err;
	return NULL;
}

struct ibv_pd *nerve_alloc_parent_domain(struct ibv_context *ibvctx,
				       struct ibv_parent_domain_init_attr *attr)
{
	struct nerve_parent_domain *parent_domain;
	struct nerve_pd *pd;

	if (ibv_check_alloc_parent_domain(attr)) {
		errno = EINVAL;
		return NULL;
	}

	if (!check_comp_mask(attr->comp_mask,
			     IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT)) {
		verbs_err(verbs_get_ctx(ibvctx), "Invalid comp_mask\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	pd = to_nerve_pd(attr->pd);

	/* We don't allow nested parent domains */
	if (pd->orig_pd) {
		errno = EINVAL;
		return NULL;
	}

	parent_domain = calloc(1, sizeof(*parent_domain));
	if (!parent_domain) {
		errno = ENOMEM;
		return NULL;
	}

	atomic_init(&parent_domain->refcount, 0);

	if (attr->td) {
		parent_domain->td = to_nerve_td(attr->td);
		atomic_fetch_add(&parent_domain->td->refcount, 1);
	}

	parent_domain->pd.orig_pd = pd;
	atomic_fetch_add(&pd->refcount, 1);

	ibv_initialize_parent_domain(&parent_domain->pd.ibvpd, attr->pd);

	if (attr->comp_mask & IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT)
		parent_domain->pd_context = attr->pd_context;

	return &parent_domain->pd.ibvpd;
}

static int nerve_dealloc_parent_domain(struct nerve_parent_domain *parent_domain)
{
	if (atomic_load(&parent_domain->refcount) > 0)
		return EBUSY;

	atomic_fetch_sub(&parent_domain->pd.orig_pd->refcount, 1);

	if (parent_domain->td)
		atomic_fetch_sub(&parent_domain->td->refcount, 1);

	free(parent_domain);
	return 0;
}

int nerve_dealloc_pd(struct ibv_pd *ibvpd)
{
	struct nerve_parent_domain *parent_domain;
	struct nerve_pd *pd = to_nerve_pd(ibvpd);
	int err;

	if (pd->orig_pd) {
		parent_domain = to_nerve_parent_domain(ibvpd);
		return nerve_dealloc_parent_domain(parent_domain);
	}

	if (atomic_load(&pd->refcount) > 0)
		return EBUSY;

	err = ibv_cmd_dealloc_pd(ibvpd);
	if (err) {
		verbs_err(verbs_get_ctx(ibvpd->context),
			  "Failed to deallocate PD\n");
		return err;
	}
	free(pd);

	return 0;
}

struct ibv_mr *nerve_reg_dmabuf_mr(struct ibv_pd *ibvpd, uint64_t offset,
				 size_t length, uint64_t iova, int fd, int acc)
{
	struct nerve_mr *mr;
	int err;

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return NULL;

	err = ibv_cmd_reg_dmabuf_mr(ibvpd, offset, length, iova, fd, acc,
				    &mr->vmr, NULL);
	if (err) {
		free(mr);
		errno = err;
		return NULL;
	}

	return &mr->vmr.ibv_mr;
}

struct ibv_mr *nerve_reg_mr(struct ibv_pd *ibvpd, void *sva, size_t len,
			  uint64_t hca_va, int access)
{
	struct ib_uverbs_reg_mr_resp resp;
	struct ibv_reg_mr cmd;
	struct nerve_mr *mr;
	int err;

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return NULL;

	err = ibv_cmd_reg_mr(ibvpd, sva, len, hca_va, access, &mr->vmr,
			     &cmd, sizeof(cmd), &resp, sizeof(resp));
	if (err) {
		verbs_err(verbs_get_ctx(ibvpd->context),
			  "Failed to register MR\n");
		free(mr);
		errno = err;
		return NULL;
	}

	return &mr->vmr.ibv_mr;
}

int nervedv_query_mr(struct ibv_mr *ibvmr, struct nervedv_mr_attr *attr, uint32_t inlen)
{
	uint16_t rdma_read_ic_id = 0;
	uint16_t rdma_recv_ic_id = 0;
	uint16_t ic_id_validity = 0;
	uint16_t recv_ic_id = 0;
	int err;

	DECLARE_COMMAND_BUFFER(cmd,
			       UVERBS_OBJECT_MR,
			       NERVE_IB_METHOD_MR_QUERY,
			       5);

	if (!is_nerve_dev(ibvmr->context->device)) {
		verbs_err(verbs_get_ctx(ibvmr->context), "Not an NERVE device\n");
		return EOPNOTSUPP;
	}

	if (!vext_field_avail(typeof(*attr), rdma_recv_ic_id, inlen)) {
		verbs_err(verbs_get_ctx(ibvmr->context), "Compatibility issues\n");
		return EINVAL;
	}

	memset(attr, 0, inlen);
	fill_attr_in_obj(cmd, NERVE_IB_ATTR_QUERY_MR_HANDLE, ibvmr->handle);
	fill_attr_out(cmd, NERVE_IB_ATTR_QUERY_MR_RESP_IC_ID_VALIDITY,
		      &ic_id_validity, sizeof(ic_id_validity));
	fill_attr_out(cmd, NERVE_IB_ATTR_QUERY_MR_RESP_RECV_IC_ID,
		      &recv_ic_id, sizeof(recv_ic_id));
	fill_attr_out(cmd, NERVE_IB_ATTR_QUERY_MR_RESP_RDMA_READ_IC_ID,
		      &rdma_read_ic_id, sizeof(rdma_read_ic_id));
	fill_attr_out(cmd, NERVE_IB_ATTR_QUERY_MR_RESP_RDMA_RECV_IC_ID,
		      &rdma_recv_ic_id, sizeof(rdma_recv_ic_id));

	err = execute_ioctl(ibvmr->context, cmd);
	if (err) {
		verbs_err(verbs_get_ctx(ibvmr->context), "Failed to query MR\n");
		return err;
	}

	if (ic_id_validity & NERVE_QUERY_MR_VALIDITY_RECV_IC_ID) {
		attr->recv_ic_id = recv_ic_id;
		attr->ic_id_validity |= NERVEDV_MR_ATTR_VALIDITY_RECV_IC_ID;
	}
	if (ic_id_validity & NERVE_QUERY_MR_VALIDITY_RDMA_READ_IC_ID) {
		attr->rdma_read_ic_id = rdma_read_ic_id;
		attr->ic_id_validity |= NERVEDV_MR_ATTR_VALIDITY_RDMA_READ_IC_ID;
	}
	if (ic_id_validity & NERVE_QUERY_MR_VALIDITY_RDMA_RECV_IC_ID) {
		attr->rdma_recv_ic_id = rdma_recv_ic_id;
		attr->ic_id_validity |= NERVEDV_MR_ATTR_VALIDITY_RDMA_RECV_IC_ID;
	}

	return 0;
}

int nerve_dereg_mr(struct verbs_mr *vmr)
{
	struct nerve_mr *mr = container_of(vmr, struct nerve_mr, vmr);
	int err;

	err = ibv_cmd_dereg_mr(vmr);
	if (err) {
		verbs_err(verbs_get_ctx(vmr->ibv_mr.context),
			  "Failed to deregister MR\n");
		return err;
	}
	free(mr);

	return 0;
}

static uint32_t nerve_wq_get_next_wrid_idx_locked(struct nerve_wq *wq,
						uint64_t wr_id)
{
	uint32_t wrid_idx;

	/* Get the next wrid to be used from the index pool */
	wrid_idx = wq->wrid_idx_pool[wq->wrid_idx_pool_next];
	wq->wrid[wrid_idx] = wr_id;

	/* Will never overlap, as validate function succeeded */
	wq->wrid_idx_pool_next++;
	assert(wq->wrid_idx_pool_next <= wq->wqe_cnt);

	return wrid_idx;
}

static void nerve_wq_put_wrid_idx_unlocked(struct nerve_wq *wq, uint32_t wrid_idx)
{
	if (wq->need_lock)
		pthread_spin_lock(&wq->wqlock);

	wq->wrid_idx_pool_next--;
	wq->wrid_idx_pool[wq->wrid_idx_pool_next] = wrid_idx;
	wq->wqe_completed++;

	if (wq->need_lock)
		pthread_spin_unlock(&wq->wqlock);
}

static uint32_t nerve_sub_cq_get_current_index(struct nerve_sub_cq *sub_cq)
{
	return sub_cq->consumed_cnt & sub_cq->qmask;
}

static int nerve_cqe_is_pending(struct nerve_io_cdesc_common *cqe_common,
			      int phase)
{
	return NERVE_GET(&cqe_common->flags, NERVE_IO_CDESC_COMMON_PHASE) == phase;
}

static struct nerve_io_cdesc_common *
nerve_sub_cq_get_cqe(struct nerve_sub_cq *sub_cq, int entry)
{
	return (struct nerve_io_cdesc_common *)(sub_cq->buf +
					      (entry * sub_cq->cqe_size));
}

static void nerve_update_cq_doorbell(struct nerve_cq *cq, bool arm)
{
	uint32_t db = 0;

	NERVE_SET(&db, NERVE_IO_REGS_CQ_DB_CONSUMER_INDEX, cq->cc);
	NERVE_SET(&db, NERVE_IO_REGS_CQ_DB_CMD_SN, cq->cmd_sn & 0x3);
	NERVE_SET(&db, NERVE_IO_REGS_CQ_DB_ARM, arm);

	mmio_write32(cq->db, db);
}

void nerve_cq_event(struct ibv_cq *ibvcq)
{
	to_nerve_cq(ibvcq)->cmd_sn++;
}

int nerve_arm_cq(struct ibv_cq *ibvcq, int solicited_only)
{
	if (unlikely(solicited_only))
		return EOPNOTSUPP;

	nerve_update_cq_doorbell(to_nerve_cq(ibvcq), true);
	return 0;
}

static struct nerve_io_cdesc_common *
cq_next_sub_cqe_get(struct nerve_sub_cq *sub_cq)
{
	struct nerve_io_cdesc_common *cqe;
	uint32_t current_index;

	current_index = nerve_sub_cq_get_current_index(sub_cq);
	cqe = nerve_sub_cq_get_cqe(sub_cq, current_index);
	if (nerve_cqe_is_pending(cqe, sub_cq->phase)) {
		/* Do not read the rest of the completion entry before the
		 * phase bit has been validated.
		 */
		udma_from_device_barrier();
		sub_cq->consumed_cnt++;
		if (!nerve_sub_cq_get_current_index(sub_cq))
			sub_cq->phase = 1 - sub_cq->phase;
		return cqe;
	}

	return NULL;
}

static enum ibv_wc_status to_ibv_status(enum nerve_io_comp_status status)
{
	switch (status) {
	case NERVE_IO_COMP_STATUS_OK:
		return IBV_WC_SUCCESS;
	case NERVE_IO_COMP_STATUS_FLUSHED:
		return IBV_WC_WR_FLUSH_ERR;
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_QP_INTERNAL_ERROR:
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_UNSUPPORTED_OP:
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_INVALID_AH:
		return IBV_WC_LOC_QP_OP_ERR;
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_INVALID_LKEY:
		return IBV_WC_LOC_PROT_ERR;
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_BAD_LENGTH:
		return IBV_WC_LOC_LEN_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_ABORT:
		return IBV_WC_REM_ABORT_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_RNR:
		return IBV_WC_RNR_RETRY_EXC_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_BAD_DEST_QPN:
		return IBV_WC_REM_INV_RD_REQ_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_BAD_STATUS:
		return IBV_WC_BAD_RESP_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_BAD_LENGTH:
		return IBV_WC_REM_INV_REQ_ERR;
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_UNRESP_REMOTE:
	case NERVE_IO_COMP_STATUS_LOCAL_ERROR_UNREACH_REMOTE:
		return IBV_WC_RESP_TIMEOUT_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_BAD_ADDRESS:
		return IBV_WC_REM_ACCESS_ERR;
	case NERVE_IO_COMP_STATUS_REMOTE_ERROR_UNKNOWN_PEER:
		return IBV_WC_REM_OP_ERR;
	default:
		return IBV_WC_GENERAL_ERR;
	}
}

static enum ibv_wc_opcode nerve_wc_read_opcode(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	enum nerve_io_send_op_type op_type;
	struct nerve_io_cdesc_common *cqe;

	cqe = cq->cur_cqe;
	op_type = NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_OP_TYPE);

	if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_Q_TYPE) ==
		    NERVE_IO_SEND_QUEUE) {
		if (op_type == NERVE_IO_RDMA_WRITE)
			return IBV_WC_RDMA_WRITE;

		return IBV_WC_SEND;
	}

	if (op_type == NERVE_IO_RDMA_WRITE)
		return IBV_WC_RECV_RDMA_WITH_IMM;

	return IBV_WC_RECV;
}

static uint32_t nerve_wc_read_vendor_err(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);

	return cq->cur_cqe->status;
}

static unsigned int nerve_wc_read_wc_flags(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	unsigned int wc_flags = 0;

	if (NERVE_GET(&cq->cur_cqe->flags, NERVE_IO_CDESC_COMMON_HAS_IMM))
		wc_flags |= IBV_WC_WITH_IMM;

	return wc_flags;
}

static uint32_t nerve_wc_read_byte_len(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_io_cdesc_common *cqe;
	struct nerve_io_rx_cdesc_ex *rcqe;
	uint32_t length;

	cqe = cq->cur_cqe;

	if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_Q_TYPE) != NERVE_IO_RECV_QUEUE)
		return 0;

	rcqe = container_of(cqe, struct nerve_io_rx_cdesc_ex, base.common);

	length = rcqe->base.length;
	if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_OP_TYPE) == NERVE_IO_RDMA_WRITE)
		length |= ((uint32_t)rcqe->u.rdma_write.length_hi << 16);

	return length;
}

static __be32 nerve_wc_read_imm_data(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_io_rx_cdesc *rcqe;

	rcqe = container_of(cq->cur_cqe, struct nerve_io_rx_cdesc, common);

	return htobe32(rcqe->imm);
}

static uint32_t nerve_wc_read_qp_num(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);

	return cq->cur_cqe->qp_num;
}

static uint32_t nerve_wc_read_src_qp(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_io_rx_cdesc *rcqe;

	rcqe = container_of(cq->cur_cqe, struct nerve_io_rx_cdesc, common);

	return rcqe->src_qp_num;
}

static uint32_t nerve_wc_read_slid(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_io_rx_cdesc *rcqe;

	rcqe = container_of(cq->cur_cqe, struct nerve_io_rx_cdesc, common);

	return rcqe->ah;
}

static uint8_t nerve_wc_read_sl(struct ibv_cq_ex *ibvcqx)
{
	return 0;
}

static uint8_t nerve_wc_read_dlid_path_bits(struct ibv_cq_ex *ibvcqx)
{
	return 0;
}

static int nerve_wc_read_sgid(struct nervedv_cq *nervedv_cq, union ibv_gid *sgid)
{
	struct nerve_cq *cq = nervedv_cq_to_nerve_cq(nervedv_cq);
	struct nerve_io_rx_cdesc_ex *rcqex;

	rcqex = container_of(cq->cur_cqe, struct nerve_io_rx_cdesc_ex,
			     base.common);
	if (rcqex->base.ah != 0xFFFF) {
		/* SGID is only available if AH is unknown. */
		return -ENOENT;
	}
	memcpy(sgid->raw, rcqex->u.src_addr, sizeof(sgid->raw));

	return 0;
}

static bool nerve_wc_is_unsolicited(struct nervedv_cq *nervedv_cq)
{
	struct nerve_cq *cq = nervedv_cq_to_nerve_cq(nervedv_cq);

	return NERVE_GET(&cq->cur_cqe->flags, NERVE_IO_CDESC_COMMON_UNSOLICITED);
}

static void nerve_process_cqe(struct nerve_cq *cq, struct ibv_wc *wc,
			    struct nerve_qp *qp)
{
	struct nerve_io_cdesc_common *cqe = cq->cur_cqe;
	enum nerve_io_send_op_type op_type;
	uint32_t wrid_idx;

	wc->status = to_ibv_status(cqe->status);
	wc->vendor_err = cqe->status;
	wc->wc_flags = 0;
	wc->qp_num = cqe->qp_num;

	wrid_idx = cqe->req_id;
	op_type = NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_OP_TYPE);
	if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_Q_TYPE) == NERVE_IO_SEND_QUEUE) {
		cq->cur_wq = &qp->sq.wq;
		if (op_type == NERVE_IO_RDMA_WRITE)
			wc->opcode = IBV_WC_RDMA_WRITE;
		else
			wc->opcode = IBV_WC_SEND;

		/* We do not have to take the WQ lock here,
		 * because this wrid index has not been freed yet,
		 * so there is no contention on this index.
		 */
		wc->wr_id = cq->cur_wq->wrid[wrid_idx];
	} else {
		struct nerve_io_rx_cdesc_ex *rcqe =
			container_of(cqe, struct nerve_io_rx_cdesc_ex, base.common);

		if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_UNSOLICITED)) {
			cq->cur_wq = NULL;
			wc->wr_id = 0;
		} else {
			cq->cur_wq = &qp->rq.wq;
			wc->wr_id = cq->cur_wq->wrid[wrid_idx];
		}

		wc->byte_len = rcqe->base.length;

		if (op_type == NERVE_IO_RDMA_WRITE) {
			wc->byte_len |= ((uint32_t)rcqe->u.rdma_write.length_hi << 16);
			wc->opcode = IBV_WC_RECV_RDMA_WITH_IMM;
		} else {
			wc->opcode = IBV_WC_RECV;
		}

		wc->src_qp = rcqe->base.src_qp_num;
		wc->sl = 0;
		wc->slid = rcqe->base.ah;

		if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_HAS_IMM)) {
			wc->imm_data = htobe32(rcqe->base.imm);
			wc->wc_flags |= IBV_WC_WITH_IMM;
		}
	}
}

static void nerve_process_ex_cqe(struct nerve_cq *cq, struct nerve_qp *qp)
{
	struct ibv_cq_ex *ibvcqx = &cq->verbs_cq.cq_ex;
	struct nerve_io_cdesc_common *cqe = cq->cur_cqe;
	uint32_t wrid_idx;

	wrid_idx = cqe->req_id;

	if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_Q_TYPE) == NERVE_IO_SEND_QUEUE) {
		cq->cur_wq = &qp->sq.wq;
		ibvcqx->wr_id = cq->cur_wq->wrid[wrid_idx];
		ibvcqx->status = to_ibv_status(cqe->status);
	} else {
		if (NERVE_GET(&cqe->flags, NERVE_IO_CDESC_COMMON_UNSOLICITED)) {
			cq->cur_wq = NULL;
			ibvcqx->wr_id = 0;
		} else {
			cq->cur_wq = &qp->rq.wq;
			ibvcqx->wr_id = cq->cur_wq->wrid[wrid_idx];
		}

		ibvcqx->status = to_ibv_status(cqe->status);
	}
}

static inline int nerve_poll_sub_cq(struct nerve_cq *cq, struct nerve_sub_cq *sub_cq,
				  struct nerve_qp **cur_qp, struct ibv_wc *wc,
				  bool extended) ALWAYS_INLINE;
static inline int nerve_poll_sub_cq(struct nerve_cq *cq, struct nerve_sub_cq *sub_cq,
				  struct nerve_qp **cur_qp, struct ibv_wc *wc,
				  bool extended)
{
	struct nerve_context *ctx = to_nerve_context(cq->verbs_cq.cq.context);
	uint32_t qpn;

	cq->cur_cqe = cq_next_sub_cqe_get(sub_cq);
	if (!cq->cur_cqe)
		return ENOENT;

	qpn = cq->cur_cqe->qp_num;
	if (!*cur_qp || qpn != (*cur_qp)->verbs_qp.qp.qp_num) {
		/* We do not have to take the QP table lock here,
		 * because CQs will be locked while QPs are removed
		 * from the table.
		 */
		*cur_qp = ctx->qp_table[qpn & ctx->qp_table_sz_m1];
		if (!*cur_qp || qpn != (*cur_qp)->verbs_qp.qp.qp_num) {
			cq->cur_wq = NULL;
			verbs_err(&ctx->ibvctx,
				  "QP[%u] does not exist in QP table\n",
				  qpn);
			return EINVAL;
		}
	}

	if (extended) {
		nerve_process_ex_cqe(cq, *cur_qp);
	} else {
		nerve_process_cqe(cq, wc, *cur_qp);
		if (cq->cur_wq)
			nerve_wq_put_wrid_idx_unlocked(cq->cur_wq, cq->cur_cqe->req_id);
	}

	return 0;
}

static inline int nerve_poll_sub_cqs(struct nerve_cq *cq, struct ibv_wc *wc,
				   bool extended) ALWAYS_INLINE;
static inline int nerve_poll_sub_cqs(struct nerve_cq *cq, struct ibv_wc *wc,
				   bool extended)
{
	uint16_t num_sub_cqs = cq->num_sub_cqs;
	struct nerve_sub_cq *sub_cq;
	struct nerve_qp *qp = NULL;
	uint16_t sub_cq_idx;
	int err = ENOENT;

	for (sub_cq_idx = 0; sub_cq_idx < num_sub_cqs; sub_cq_idx++) {
		sub_cq = &cq->sub_cq_arr[cq->next_poll_idx++];
		cq->next_poll_idx %= num_sub_cqs;

		err = nerve_poll_sub_cq(cq, sub_cq, &qp, wc, extended);
		if (err != ENOENT) {
			cq->cc++;
			break;
		}
	}

	return err;
}

int nerve_poll_cq(struct ibv_cq *ibvcq, int nwc, struct ibv_wc *wc)
{
	struct nerve_cq *cq = to_nerve_cq(ibvcq);
	int ret = 0;
	int i;

	pthread_spin_lock(&cq->lock);
	for (i = 0; i < nwc; i++) {
		ret = nerve_poll_sub_cqs(cq, &wc[i], false);
		if (ret) {
			if (ret == ENOENT)
				ret = 0;
			break;
		}
	}

	if (i && cq->db)
		nerve_update_cq_doorbell(cq, false);
	pthread_spin_unlock(&cq->lock);

	return i ?: -ret;
}

static inline int nerve_start_poll_comp_check(struct ibv_cq_ex *ibvcqx,
					    struct ibv_poll_cq_attr *attr) ALWAYS_INLINE;
static inline int nerve_start_poll_comp_check(struct ibv_cq_ex *ibvcqx,
					    struct ibv_poll_cq_attr *attr)
{
	if (unlikely(attr->comp_mask)) {
		verbs_err(verbs_get_ctx(ibvcqx->context),
			  "Invalid comp_mask %u\n",
			  attr->comp_mask);
		return EINVAL;
	}

	return 0;
}

static inline void nerve_end_poll_common(struct nerve_cq *cq) ALWAYS_INLINE;
static inline void nerve_end_poll_common(struct nerve_cq *cq)
{
	if (cq->cur_cqe) {
		if (cq->cur_wq)
			nerve_wq_put_wrid_idx_unlocked(cq->cur_wq, cq->cur_cqe->req_id);
		if (cq->db)
			nerve_update_cq_doorbell(cq, false);
	}
}

static int nerve_start_poll(struct ibv_cq_ex *ibvcqx,
			  struct ibv_poll_cq_attr *attr)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	int ret;

	if (nerve_start_poll_comp_check(ibvcqx, attr))
		return EINVAL;

	pthread_spin_lock(&cq->lock);

	ret = nerve_poll_sub_cqs(cq, NULL, true);
	if (ret)
		pthread_spin_unlock(&cq->lock);

	return ret;
}

static int nerve_next_poll(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	int ret;

	if (cq->cur_wq)
		nerve_wq_put_wrid_idx_unlocked(cq->cur_wq, cq->cur_cqe->req_id);
	ret = nerve_poll_sub_cqs(cq, NULL, true);

	return ret;
}

static void nerve_end_poll(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);

	nerve_end_poll_common(cq);
	pthread_spin_unlock(&cq->lock);
}

static int nerve_start_poll_single_sub_cq(struct ibv_cq_ex *ibvcqx,
					struct ibv_poll_cq_attr *attr)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_qp *qp = NULL;
	int ret;

	if (nerve_start_poll_comp_check(ibvcqx, attr))
		return EINVAL;

	pthread_spin_lock(&cq->lock);
	ret = nerve_poll_sub_cq(cq, cq->sub_cq_arr, &qp, NULL, true);
	if (ret != ENOENT)
		cq->cc++;

	if (ret)
		pthread_spin_unlock(&cq->lock);

	return ret;
}

static int nerve_next_poll_single_sub_cq(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_qp *qp = NULL;
	int ret;

	if (cq->cur_wq)
		nerve_wq_put_wrid_idx_unlocked(cq->cur_wq, cq->cur_cqe->req_id);

	ret = nerve_poll_sub_cq(cq, cq->sub_cq_arr, &qp, NULL, true);
	if (ret != ENOENT)
		cq->cc++;

	return ret;
}

static int nerve_start_poll_single_thread(struct ibv_cq_ex *ibvcqx,
					struct ibv_poll_cq_attr *attr)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);

	if (nerve_start_poll_comp_check(ibvcqx, attr))
		return EINVAL;

	return nerve_poll_sub_cqs(cq, NULL, true);
}

static void nerve_end_poll_single_thread(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);

	nerve_end_poll_common(cq);
}

static int nerve_start_poll_single_sub_cq_single_thread(struct ibv_cq_ex *ibvcqx,
						      struct ibv_poll_cq_attr *attr)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);
	struct nerve_qp *qp = NULL;
	int ret;

	if (nerve_start_poll_comp_check(ibvcqx, attr))
		return EINVAL;

	ret = nerve_poll_sub_cq(cq, cq->sub_cq_arr, &qp, NULL, true);
	if (ret != ENOENT)
		cq->cc++;

	return ret;
}

enum cq_pfns_attr {
	SINGLE_SUB_CQ_PFNS = BIT(0),
	SINGLE_THREAD_PFNS = BIT(1),
};

#define nerve_start_poll_name(single_sub_cq, single_thread) nerve_start_poll##single_sub_cq##single_thread
#define nerve_next_poll_name(single_sub_cq) nerve_next_poll##single_sub_cq
#define nerve_end_poll_name(single_thread) nerve_end_poll##single_thread

#define POLL_FN_ENTRY(single_sub_cq, single_thread) { \
		.start_poll = nerve_start_poll_name(single_sub_cq, single_thread), \
		.next_poll = nerve_next_poll_name(single_sub_cq), \
		.end_poll = nerve_end_poll_name(single_thread), \
	}

static struct cq_base_ops {
	int (*start_poll)(struct ibv_cq_ex *ibcq, struct ibv_poll_cq_attr *attr);
	int (*next_poll)(struct ibv_cq_ex *ibcq);
	void (*end_poll)(struct ibv_cq_ex *ibcq);
} base_ops[] = {
	[0] = POLL_FN_ENTRY(,),
	[SINGLE_SUB_CQ_PFNS] = POLL_FN_ENTRY(_single_sub_cq,),
	[SINGLE_THREAD_PFNS] = POLL_FN_ENTRY(, _single_thread),
	[SINGLE_SUB_CQ_PFNS | SINGLE_THREAD_PFNS] = POLL_FN_ENTRY(_single_sub_cq, _single_thread)
};

static void nerve_cq_fill_pfns(struct nerve_cq *cq,
			     struct ibv_cq_init_attr_ex *attr,
			     struct nervedv_cq_init_attr *nerve_attr)
{
	struct ibv_cq_ex *ibvcqx = &cq->verbs_cq.cq_ex;
	const struct cq_base_ops *cq_ops;
	uint32_t cq_pfns_mask = 0;

	if (cq->num_sub_cqs == 1)
		cq_pfns_mask |= SINGLE_SUB_CQ_PFNS;

	if ((cq->parent_domain && cq->parent_domain->td) ||
	    attr->flags & IBV_CREATE_CQ_ATTR_SINGLE_THREADED)
		cq_pfns_mask |= SINGLE_THREAD_PFNS;

	cq_ops = &base_ops[cq_pfns_mask];
	ibvcqx->start_poll = cq_ops->start_poll;
	ibvcqx->next_poll = cq_ops->next_poll;
	ibvcqx->end_poll = cq_ops->end_poll;

	ibvcqx->read_opcode = nerve_wc_read_opcode;
	ibvcqx->read_vendor_err = nerve_wc_read_vendor_err;
	ibvcqx->read_wc_flags = nerve_wc_read_wc_flags;

	if (attr->wc_flags & IBV_WC_EX_WITH_BYTE_LEN)
		ibvcqx->read_byte_len = nerve_wc_read_byte_len;
	if (attr->wc_flags & IBV_WC_EX_WITH_IMM)
		ibvcqx->read_imm_data = nerve_wc_read_imm_data;
	if (attr->wc_flags & IBV_WC_EX_WITH_QP_NUM)
		ibvcqx->read_qp_num = nerve_wc_read_qp_num;
	if (attr->wc_flags & IBV_WC_EX_WITH_SRC_QP)
		ibvcqx->read_src_qp = nerve_wc_read_src_qp;
	if (attr->wc_flags & IBV_WC_EX_WITH_SLID)
		ibvcqx->read_slid = nerve_wc_read_slid;
	if (attr->wc_flags & IBV_WC_EX_WITH_SL)
		ibvcqx->read_sl = nerve_wc_read_sl;
	if (attr->wc_flags & IBV_WC_EX_WITH_DLID_PATH_BITS)
		ibvcqx->read_dlid_path_bits = nerve_wc_read_dlid_path_bits;

	if (nerve_attr->wc_flags & NERVEDV_WC_EX_WITH_SGID)
		cq->dv_cq.wc_read_sgid = nerve_wc_read_sgid;
	if (nerve_attr->wc_flags & NERVEDV_WC_EX_WITH_IS_UNSOLICITED)
		cq->dv_cq.wc_is_unsolicited = nerve_wc_is_unsolicited;
}

static void nerve_sub_cq_initialize(struct nerve_sub_cq *sub_cq, uint8_t *buf,
				  int sub_cq_size, int cqe_size)
{
	sub_cq->consumed_cnt = 0;
	sub_cq->phase = 1;
	sub_cq->buf = buf;
	sub_cq->qmask = sub_cq_size - 1;
	sub_cq->cqe_size = cqe_size;
}

static struct ibv_cq_ex *create_cq(struct ibv_context *ibvctx,
				   struct ibv_cq_init_attr_ex *attr,
				   struct nervedv_cq_init_attr *nerve_attr)
{
	struct nerve_context *ctx = to_nerve_context(ibvctx);
	struct verbs_create_cq_prov_attr prov_attr = {};
	struct nerve_parent_domain *parent_domain = NULL;
	uint16_t cqe_size = ctx->ex_cqe_size;
	struct nerve_create_cq_resp resp = {};
	struct nerve_create_cq cmd = {};
	uint32_t cmd_flags = 0;
	uint16_t num_sub_cqs;
	struct nerve_cq *cq;
	struct nerve_pd *pd;
	int sub_buf_size;
	int sub_cq_size;
	uint8_t *buf;
	int err;
	int i;

#define NERVE_CREATE_CQ_SUPP_ATTR_MASK \
	(IBV_CQ_INIT_ATTR_MASK_PD | IBV_CQ_INIT_ATTR_MASK_FLAGS)

	if (!check_comp_mask(attr->comp_mask, NERVE_CREATE_CQ_SUPP_ATTR_MASK) ||
	    !check_comp_mask(attr->wc_flags, IBV_WC_STANDARD_FLAGS)) {
		verbs_err(verbs_get_ctx(ibvctx),
			  "Invalid comp_mask or wc_flags\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	if (attr->comp_mask & IBV_CQ_INIT_ATTR_MASK_FLAGS &&
	    !check_comp_mask(attr->flags, IBV_CREATE_CQ_ATTR_SINGLE_THREADED)) {
		verbs_err(verbs_get_ctx(ibvctx),
			  "Invalid flags\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	if (attr->channel &&
	    !NERVE_DEV_CAP(ctx, CQ_NOTIFICATIONS)) {
		errno = EOPNOTSUPP;
		return NULL;
	}

	if (attr->comp_mask & IBV_CQ_INIT_ATTR_MASK_PD) {
		pd = to_nerve_pd(attr->parent_domain);
		if (!pd->orig_pd) {
			verbs_err(verbs_get_ctx(ibvctx), "Parent domain set but not provided\n");
			errno = EINVAL;
			return NULL;
		}

		parent_domain = to_nerve_parent_domain(attr->parent_domain);
	}

	cq = calloc(1, sizeof(*cq) +
		       sizeof(*cq->sub_cq_arr) * ctx->sub_cqs_per_cq);
	if (!cq)
		return NULL;

	if (nerve_attr->wc_flags & NERVEDV_WC_EX_WITH_SGID)
		cmd.flags |= NERVE_CREATE_CQ_WITH_SGID;

	num_sub_cqs = ctx->sub_cqs_per_cq;
	cmd.num_sub_cqs = num_sub_cqs;
	cmd.cq_entry_size = cqe_size;

	if (nerve_attr->flags & NERVEDV_CQ_INIT_FLAGS_EXT_MEM_DMABUF) {
		prov_attr.buffer.length = nerve_attr->ext_mem_dmabuf.length;
		prov_attr.buffer.dmabuf.offset = nerve_attr->ext_mem_dmabuf.offset;
		prov_attr.buffer.dmabuf.fd = nerve_attr->ext_mem_dmabuf.fd;
		cmd_flags = CREATE_CQ_CMD_FLAGS_WITH_MEM_DMABUF;
	}

	if (attr->channel)
		cmd.flags |= NERVE_CREATE_CQ_WITH_COMPLETION_CHANNEL;

	attr->cqe = roundup_pow_of_two(attr->cqe);
	err = ibv_cmd_create_cq_ex(ibvctx, attr, &prov_attr, &cq->verbs_cq,
				   &cmd.ibv_cmd, sizeof(cmd),
				   &resp.ibv_resp, sizeof(resp), cmd_flags);
	if (err) {
		errno = err;
		goto err_free_cq;
	}

	sub_cq_size = cq->verbs_cq.cq.cqe;
	cq->cqn = resp.cq_idx;
	cq->num_sub_cqs = num_sub_cqs;
	cq->cqe_size = cqe_size;
	cq->dev = ibvctx->device;
	cq->parent_domain = parent_domain;

	if (nerve_attr->flags & NERVEDV_CQ_INIT_FLAGS_EXT_MEM_DMABUF) {
		cq->buf_size = nerve_attr->ext_mem_dmabuf.length;
		cq->buf = nerve_attr->ext_mem_dmabuf.buffer;
	} else {
		cq->buf_size = resp.q_mmap_size;
		cq->buf = mmap(NULL, cq->buf_size, PROT_READ, MAP_SHARED, ibvctx->cmd_fd,
			       resp.q_mmap_key);
		if (cq->buf == MAP_FAILED)
			goto err_destroy_cq;

		cq->buf_mmaped = true;
	}

	if (cq->buf) {
		buf = cq->buf;
		sub_buf_size = cq->cqe_size * sub_cq_size;
		for (i = 0; i < num_sub_cqs; i++) {
			nerve_sub_cq_initialize(&cq->sub_cq_arr[i], buf, sub_cq_size, cq->cqe_size);
			buf += sub_buf_size;
		}
	}

	if (resp.comp_mask & NERVE_CREATE_CQ_RESP_DB_OFF) {
		cq->db_mmap_addr = mmap(NULL,
					to_nerve_dev(ibvctx->device)->pg_sz, PROT_WRITE,
					MAP_SHARED, ibvctx->cmd_fd, resp.db_mmap_key);
		if (cq->db_mmap_addr == MAP_FAILED)
			goto err_unmap_cq;

		cq->db = (uint32_t *)(cq->db_mmap_addr + resp.db_off);
	}

	nerve_cq_fill_pfns(cq, attr, nerve_attr);
	pthread_spin_init(&cq->lock, PTHREAD_PROCESS_PRIVATE);
	if (cq->parent_domain)
		atomic_fetch_add(&cq->parent_domain->refcount, 1);

	return &cq->verbs_cq.cq_ex;

err_unmap_cq:
	if (cq->buf_mmaped)
		munmap(cq->buf, cq->buf_size);
err_destroy_cq:
	ibv_cmd_destroy_cq(&cq->verbs_cq.cq);
err_free_cq:
	free(cq);
	verbs_err(verbs_get_ctx(ibvctx), "Failed to create CQ\n");
	return NULL;
}

struct ibv_cq *nerve_create_cq(struct ibv_context *ibvctx, int ncqe,
			     struct ibv_comp_channel *channel, int vec)
{
	struct nervedv_cq_init_attr nerve_attr = {};
	struct ibv_cq_init_attr_ex attr_ex = {
		.cqe = ncqe,
		.channel = channel,
		.comp_vector = vec
	};
	struct ibv_cq_ex *ibvcqx;

	ibvcqx = create_cq(ibvctx, &attr_ex, &nerve_attr);

	return ibvcqx ? ibv_cq_ex_to_cq(ibvcqx) : NULL;
}

struct ibv_cq_ex *nerve_create_cq_ex(struct ibv_context *ibvctx,
				   struct ibv_cq_init_attr_ex *attr_ex)
{
	struct nervedv_cq_init_attr nerve_attr = {};

	return create_cq(ibvctx, attr_ex, &nerve_attr);
}

struct ibv_cq_ex *nervedv_create_cq(struct ibv_context *ibvctx,
				  struct ibv_cq_init_attr_ex *attr_ex,
				  struct nervedv_cq_init_attr *nerve_attr,
				  uint32_t inlen)
{
	struct nervedv_cq_init_attr local_nerve_attr = {};
	uint64_t supp_wc_flags = 0;
	struct nerve_context *ctx;

	if (!is_nerve_dev(ibvctx->device)) {
		verbs_err(verbs_get_ctx(ibvctx), "Not an NERVE device\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	if (!vext_field_avail(struct nervedv_cq_init_attr, wc_flags, inlen) ||
	    nerve_attr->comp_mask ||
	    (inlen > sizeof(*nerve_attr) && !is_ext_cleared(nerve_attr, inlen))) {
		verbs_err(verbs_get_ctx(ibvctx), "Compatibility issues\n");
		errno = EINVAL;
		return NULL;
	}

	ctx = to_nerve_context(ibvctx);
	if (NERVE_DEV_CAP(ctx, CQ_WITH_SGID))
		supp_wc_flags |= NERVEDV_WC_EX_WITH_SGID;
	if (NERVE_DEV_CAP(ctx, UNSOLICITED_WRITE_RECV))
		supp_wc_flags |= NERVEDV_WC_EX_WITH_IS_UNSOLICITED;
	if (!check_comp_mask(nerve_attr->wc_flags, supp_wc_flags)) {
		verbs_err(verbs_get_ctx(ibvctx),
			  "Invalid NERVE wc_flags[%#lx]\n", nerve_attr->wc_flags);
		errno = EOPNOTSUPP;
		return NULL;
	}

	memcpy(&local_nerve_attr, nerve_attr, min_t(uint32_t, inlen, sizeof(local_nerve_attr)));
	return create_cq(ibvctx, attr_ex, &local_nerve_attr);
}

int nervedv_query_cq(struct ibv_cq *ibvcq, struct nervedv_cq_attr *attr, uint32_t inlen)
{
	struct nerve_cq *cq = to_nerve_cq(ibvcq);

	if (!is_nerve_dev(ibvcq->context->device)) {
		verbs_err(verbs_get_ctx(ibvcq->context), "Not an NERVE device\n");
		return EOPNOTSUPP;
	}

	if (!vext_field_avail(typeof(*attr), num_entries, inlen)) {
		verbs_err(verbs_get_ctx(ibvcq->context), "Compatibility issues\n");
		return EINVAL;
	}

	attr->comp_mask = 0;
	attr->buffer = cq->buf;
	attr->entry_size = cq->cqe_size;
	attr->num_entries = ibvcq->cqe;

	if (vext_field_avail(typeof(*attr), doorbell, inlen))
		attr->doorbell = cq->db;

	return 0;
}

struct nervedv_cq *nervedv_cq_from_ibv_cq_ex(struct ibv_cq_ex *ibvcqx)
{
	struct nerve_cq *cq = to_nerve_cq_ex(ibvcqx);

	return &cq->dv_cq;
}

int nerve_destroy_cq(struct ibv_cq *ibvcq)
{
	struct nerve_cq *cq = to_nerve_cq(ibvcq);
	int err;

	err = ibv_cmd_destroy_cq(ibvcq);
	if (err) {
		verbs_err(verbs_get_ctx(ibvcq->context),
			  "Failed to destroy CQ[%u]\n", cq->cqn);
		return err;
	}

	munmap(cq->db_mmap_addr, to_nerve_dev(cq->dev)->pg_sz);
	if (cq->buf_mmaped)
		munmap(cq->buf, cq->buf_size);

	pthread_spin_destroy(&cq->lock);
	if (cq->parent_domain)
		atomic_fetch_sub(&cq->parent_domain->refcount, 1);

	free(cq);

	return 0;
}

static void nerve_wq_terminate(struct nerve_wq *wq, int pgsz)
{
	void *db_aligned;

	if (wq->need_lock)
		pthread_spin_destroy(&wq->wqlock);

	db_aligned = (void *)((uintptr_t)wq->db & ~(pgsz - 1));
	munmap(db_aligned, pgsz);

	free(wq->wrid_idx_pool);
	free(wq->wrid);
}

static int nerve_wq_initialize(struct nerve_wq *wq, struct nerve_wq_init_attr *attr)
{
	uint8_t *db_base;
	int err;
	int i;

	wq->wrid = malloc(wq->wqe_cnt * sizeof(*wq->wrid));
	if (!wq->wrid)
		return ENOMEM;

	wq->wrid_idx_pool = malloc(wq->wqe_cnt * sizeof(uint32_t));
	if (!wq->wrid_idx_pool) {
		err = ENOMEM;
		goto err_free_wrid;
	}

	db_base = mmap(NULL, attr->pgsz, PROT_WRITE, MAP_SHARED, attr->cmd_fd,
		       attr->db_mmap_key);
	if (db_base == MAP_FAILED) {
		err = errno;
		goto err_free_wrid_idx_pool;
	}

	wq->db = (uint32_t *)(db_base + attr->db_off);

	/* Initialize the wrid free indexes pool. */
	for (i = 0; i < wq->wqe_cnt; i++)
		wq->wrid_idx_pool[i] = i;

	wq->need_lock = attr->need_lock;
	if (wq->need_lock)
		pthread_spin_init(&wq->wqlock, PTHREAD_PROCESS_PRIVATE);

	wq->sub_cq_idx = attr->sub_cq_idx;

	return 0;

err_free_wrid_idx_pool:
	free(wq->wrid_idx_pool);
err_free_wrid:
	free(wq->wrid);
	return err;
}

static bool nerve_check_cq_on_same_pd_td(struct ibv_pd *ibvpd, struct ibv_cq *ibvcq)
{
	struct nerve_parent_domain *parent_domain;
	struct nerve_pd *pd;
	struct nerve_cq *cq;

	pd = to_nerve_pd(ibvpd);
	cq = to_nerve_cq(ibvcq);

	if (pd->orig_pd) {
		parent_domain = to_nerve_parent_domain(ibvpd);
		if (parent_domain == cq->parent_domain && parent_domain->td)
			return true;
	}

	return false;
}

static void nerve_sq_terminate(struct nerve_qp *qp)
{
	struct nerve_sq *sq = &qp->sq;

	if (!sq->wq.wqe_cnt)
		return;

	munmap(sq->desc - sq->desc_offset, sq->desc_ring_mmap_size);
	free(sq->local_queue);

	nerve_wq_terminate(&sq->wq, qp->page_size);
}

static int nerve_sq_initialize(struct nerve_qp *qp,
			     const struct ibv_qp_init_attr_ex *attr,
			     struct nerve_create_qp_resp *resp)
{
	struct nerve_context *ctx = to_nerve_context(qp->verbs_qp.qp.context);
	struct nerve_wq_init_attr wq_attr;
	struct nerve_sq *sq = &qp->sq;
	size_t desc_ring_size;
	bool need_lock;
	int err;

	if (!sq->wq.wqe_cnt)
		return 0;

	need_lock = !nerve_check_cq_on_same_pd_td(attr->pd, attr->send_cq);

	wq_attr = (struct nerve_wq_init_attr) {
		.db_mmap_key = resp->sq_db_mmap_key,
		.db_off = resp->sq_db_offset,
		.cmd_fd = qp->verbs_qp.qp.context->cmd_fd,
		.pgsz = qp->page_size,
		.sub_cq_idx = resp->send_sub_cq_idx,
		.need_lock = need_lock,
	};

	err = nerve_wq_initialize(&qp->sq.wq, &wq_attr);
	if (err) {
		verbs_err(&ctx->ibvctx, "SQ[%u] nerve_wq_initialize failed\n",
			  qp->verbs_qp.qp.qp_num);
		return err;
	}

	sq->desc_offset = resp->llq_desc_offset;
	desc_ring_size = sq->wq.wqe_cnt * sizeof(struct nerve_io_tx_wqe);
	sq->desc_ring_mmap_size = align(desc_ring_size + sq->desc_offset,
					qp->page_size);
	sq->max_inline_data = attr->cap.max_inline_data;

	sq->local_queue = malloc(desc_ring_size);
	if (!sq->local_queue) {
		err = ENOMEM;
		goto err_terminate_wq;
	}

	sq->desc = mmap(NULL, sq->desc_ring_mmap_size, PROT_WRITE,
			MAP_SHARED, qp->verbs_qp.qp.context->cmd_fd,
			resp->llq_desc_mmap_key);
	if (sq->desc == MAP_FAILED) {
		verbs_err(&ctx->ibvctx, "SQ buffer mmap failed\n");
		err = errno;
		goto err_free_local_queue;
	}

	sq->desc += sq->desc_offset;
	sq->max_wr_rdma_sge = min_t(uint16_t, ctx->max_wr_rdma_sge,
				    NERVE_IO_TX_DESC_NUM_RDMA_BUFS);
	sq->max_batch_wr = ctx->max_tx_batch ?
		(ctx->max_tx_batch * 64) / sizeof(struct nerve_io_tx_wqe) :
		UINT16_MAX;
	if (ctx->min_sq_wr) {
		/* The device can't accept a doorbell for the whole SQ at once,
		 * set the max batch to at least (SQ size - 1).
		 */
		sq->max_batch_wr = min_t(uint32_t, sq->max_batch_wr,
					 sq->wq.wqe_cnt - 1);
	}

	return 0;

err_free_local_queue:
	free(sq->local_queue);
err_terminate_wq:
	nerve_wq_terminate(&sq->wq, qp->page_size);
	return err;
}

static void nerve_rq_terminate(struct nerve_qp *qp)
{
	struct nerve_rq *rq = &qp->rq;

	if (!rq->wq.wqe_cnt)
		return;

	munmap(rq->buf, rq->buf_size);

	nerve_wq_terminate(&rq->wq, qp->page_size);
}

static int nerve_rq_initialize(struct nerve_qp *qp,
			     const struct ibv_qp_init_attr_ex *attr,
			     struct nerve_create_qp_resp *resp)
{
	struct nerve_wq_init_attr wq_attr;
	struct nerve_rq *rq = &qp->rq;
	bool need_lock;
	int err;

	if (!rq->wq.wqe_cnt)
		return 0;

	need_lock = !nerve_check_cq_on_same_pd_td(attr->pd, attr->recv_cq);

	wq_attr = (struct nerve_wq_init_attr) {
		.db_mmap_key = resp->rq_db_mmap_key,
		.db_off = resp->rq_db_offset,
		.cmd_fd = qp->verbs_qp.qp.context->cmd_fd,
		.pgsz = qp->page_size,
		.sub_cq_idx = resp->recv_sub_cq_idx,
		.need_lock = need_lock,
	};

	err = nerve_wq_initialize(&qp->rq.wq, &wq_attr);
	if (err) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "RQ nerve_wq_initialize failed\n");
		return err;
	}

	rq->buf_size = resp->rq_mmap_size;
	rq->buf = mmap(NULL, rq->buf_size, PROT_WRITE, MAP_SHARED,
		       qp->verbs_qp.qp.context->cmd_fd, resp->rq_mmap_key);
	if (rq->buf == MAP_FAILED) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "RQ buffer mmap failed\n");
		err = errno;
		goto err_terminate_wq;
	}

	return 0;

err_terminate_wq:
	nerve_wq_terminate(&rq->wq, qp->page_size);
	return err;
}

static void nerve_qp_init_indices(struct nerve_qp *qp)
{
	qp->sq.wq.wqe_posted = 0;
	qp->sq.wq.wqe_completed = 0;
	qp->sq.wq.pc = 0;
	qp->sq.wq.wrid_idx_pool_next = 0;

	qp->rq.wq.wqe_posted = 0;
	qp->rq.wq.wqe_completed = 0;
	qp->rq.wq.pc = 0;
	qp->rq.wq.wrid_idx_pool_next = 0;
}

static void nerve_setup_qp(struct nerve_context *ctx,
			 struct nerve_qp *qp,
			 struct ibv_qp_cap *cap,
			 size_t page_size)
{
	uint16_t rq_desc_cnt;

	nerve_qp_init_indices(qp);

	qp->sq.wq.wqe_cnt = roundup_pow_of_two(max_t(uint32_t, cap->max_send_wr,
						     ctx->min_sq_wr));
	qp->sq.wq.max_sge = cap->max_send_sge;
	qp->sq.wq.desc_mask = qp->sq.wq.wqe_cnt - 1;

	qp->rq.wq.max_sge = cap->max_recv_sge;
	rq_desc_cnt = roundup_pow_of_two(cap->max_recv_sge * cap->max_recv_wr);
	qp->rq.wq.desc_mask = rq_desc_cnt - 1;
	qp->rq.wq.wqe_cnt = rq_desc_cnt / qp->rq.wq.max_sge;

	qp->page_size = page_size;
}

static void nerve_lock_cqs(struct ibv_qp *ibvqp)
{
	struct nerve_cq *send_cq = to_nerve_cq(ibvqp->send_cq);
	struct nerve_cq *recv_cq = to_nerve_cq(ibvqp->recv_cq);

	if (recv_cq == send_cq) {
		pthread_spin_lock(&recv_cq->lock);
	} else {
		pthread_spin_lock(&recv_cq->lock);
		pthread_spin_lock(&send_cq->lock);
	}
}

static void nerve_unlock_cqs(struct ibv_qp *ibvqp)
{
	struct nerve_cq *send_cq = to_nerve_cq(ibvqp->send_cq);
	struct nerve_cq *recv_cq = to_nerve_cq(ibvqp->recv_cq);

	if (recv_cq == send_cq) {
		pthread_spin_unlock(&recv_cq->lock);
	} else {
		pthread_spin_unlock(&recv_cq->lock);
		pthread_spin_unlock(&send_cq->lock);
	}
}

static void nerve_qp_fill_wr_pfns(struct ibv_qp_ex *ibvqpx,
				struct ibv_qp_init_attr_ex *attr_ex);

static int nerve_check_qp_attr(struct nerve_context *ctx,
			     struct ibv_qp_init_attr_ex *attr,
			     struct nervedv_qp_init_attr *nerve_attr)
{
	uint64_t supp_ud_send_ops_mask = IBV_QP_EX_WITH_SEND |
					 IBV_QP_EX_WITH_SEND_WITH_IMM;
	uint64_t supp_srd_send_ops_mask = IBV_QP_EX_WITH_SEND |
					  IBV_QP_EX_WITH_SEND_WITH_IMM;
	uint64_t supp_send_ops_mask;
	uint16_t supp_nerve_flags = 0;

	if (NERVE_DEV_CAP(ctx, RDMA_READ))
		supp_srd_send_ops_mask |= IBV_QP_EX_WITH_RDMA_READ;
	if (NERVE_DEV_CAP(ctx, RDMA_WRITE))
		supp_srd_send_ops_mask |= IBV_QP_EX_WITH_RDMA_WRITE |
					  IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM;
	if (NERVE_DEV_CAP(ctx, UNSOLICITED_WRITE_RECV))
		supp_nerve_flags |= NERVEDV_QP_FLAGS_UNSOLICITED_WRITE_RECV;

#define NERVE_CREATE_QP_SUPP_ATTR_MASK \
	(IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_SEND_OPS_FLAGS)

	if (attr->qp_type == IBV_QPT_DRIVER &&
	    nerve_attr->driver_qp_type != NERVEDV_QP_DRIVER_TYPE_SRD) {
		verbs_err(&ctx->ibvctx, "Driver QP type must be SRD\n");
		return EOPNOTSUPP;
	}

	if (!check_comp_mask(nerve_attr->flags, supp_nerve_flags)) {
		verbs_err(&ctx->ibvctx,
			  "Unsupported NERVE flags[%#x] supported[%#x]\n",
			  nerve_attr->flags, supp_nerve_flags);
		return EOPNOTSUPP;
	}

	if (!check_comp_mask(attr->comp_mask, NERVE_CREATE_QP_SUPP_ATTR_MASK)) {
		verbs_err(&ctx->ibvctx,
			  "Unsupported comp_mask[%#x] supported[%#x]\n",
			  attr->comp_mask, NERVE_CREATE_QP_SUPP_ATTR_MASK);
		return EOPNOTSUPP;
	}

	if (!(attr->comp_mask & IBV_QP_INIT_ATTR_PD)) {
		verbs_err(&ctx->ibvctx, "Does not support PD in init attr\n");
		return EINVAL;
	}

	if (attr->comp_mask & IBV_QP_INIT_ATTR_SEND_OPS_FLAGS) {
		switch (attr->qp_type) {
		case IBV_QPT_UD:
			supp_send_ops_mask = supp_ud_send_ops_mask;
			break;
		case IBV_QPT_DRIVER:
			supp_send_ops_mask = supp_srd_send_ops_mask;
			break;
		default:
			verbs_err(&ctx->ibvctx, "Invalid QP type %u\n",
				  attr->qp_type);
			return EOPNOTSUPP;
		}

		if (!check_comp_mask(attr->send_ops_flags,
				     supp_send_ops_mask)) {
			verbs_err(&ctx->ibvctx,
				  "Unsupported send_ops_flags[%" PRIx64 "] supported [%" PRIx64 "]\n",
				  attr->send_ops_flags, supp_send_ops_mask);
			return EOPNOTSUPP;
		}
	}

	if (!attr->recv_cq || !attr->send_cq) {
		verbs_err(&ctx->ibvctx, "Send/Receive CQ not provided\n");
		return EINVAL;
	}

	if (attr->srq) {
		verbs_err(&ctx->ibvctx, "SRQ is not supported\n");
		return EINVAL;
	}

	return 0;
}

static int nerve_check_qp_limits(struct nerve_context *ctx,
			       struct ibv_qp_init_attr_ex *attr)
{
	if (attr->cap.max_send_sge > ctx->max_sq_sge) {
		verbs_err(&ctx->ibvctx,
			  "Max send SGE %u > %u\n", attr->cap.max_send_sge,
			  ctx->max_sq_sge);
		return EINVAL;
	}

	if (attr->cap.max_recv_sge > ctx->max_rq_sge) {
		verbs_err(&ctx->ibvctx,
			  "Max receive SGE %u > %u\n", attr->cap.max_recv_sge,
			  ctx->max_rq_sge);
		return EINVAL;
	}

	if (attr->cap.max_send_wr > ctx->max_sq_wr) {
		verbs_err(&ctx->ibvctx,
			  "Max send WR %u > %u\n", attr->cap.max_send_wr,
			  ctx->max_sq_wr);
		return EINVAL;
	}

	if (attr->cap.max_recv_wr > ctx->max_rq_wr) {
		verbs_err(&ctx->ibvctx,
			  "Max receive WR %u > %u\n", attr->cap.max_recv_wr,
			  ctx->max_rq_wr);
		return EINVAL;
	}

	return 0;
}

static struct ibv_qp *create_qp(struct ibv_context *ibvctx,
				struct ibv_qp_init_attr_ex *attr,
				struct nervedv_qp_init_attr *nerve_attr)
{
	struct nerve_context *ctx = to_nerve_context(ibvctx);
	struct nerve_dev *dev = to_nerve_dev(ibvctx->device);
	struct nerve_parent_domain *parent_domain;
	struct nerve_create_qp_resp resp = {};
	struct nerve_create_qp req = {};
	struct ibv_qp *ibvqp;
	struct nerve_qp *qp;
	struct nerve_pd *pd;
	int err;

	err = nerve_check_qp_attr(ctx, attr, nerve_attr);
	if (err)
		goto err_out;

	err = nerve_check_qp_limits(ctx, attr);
	if (err)
		goto err_out;

	qp = calloc(1, sizeof(*qp));
	if (!qp) {
		err = ENOMEM;
		goto err_out;
	}

	nerve_setup_qp(ctx, qp, &attr->cap, dev->pg_sz);

	attr->cap.max_send_wr = qp->sq.wq.wqe_cnt;
	attr->cap.max_recv_wr = qp->rq.wq.wqe_cnt;

	req.rq_ring_size = (qp->rq.wq.desc_mask + 1) *
		sizeof(struct nerve_io_rx_desc);
	req.sq_ring_size = (attr->cap.max_send_wr) *
		sizeof(struct nerve_io_tx_wqe);
	if (attr->qp_type == IBV_QPT_DRIVER)
		req.driver_qp_type = nerve_attr->driver_qp_type;
	if (nerve_attr->flags & NERVEDV_QP_FLAGS_UNSOLICITED_WRITE_RECV)
		req.flags |= NERVE_CREATE_QP_WITH_UNSOLICITED_WRITE_RECV;

	req.sl = nerve_attr->sl;

	err = ibv_cmd_create_qp_ex(ibvctx, &qp->verbs_qp,
				   attr, &req.ibv_cmd, sizeof(req),
				   &resp.ibv_resp, sizeof(resp));
	if (err)
		goto err_free_qp;

	ibvqp = &qp->verbs_qp.qp;
	ibvqp->state = IBV_QPS_RESET;
	qp->sq_sig_all = attr->sq_sig_all;
	qp->dev = ibvctx->device;

	err = nerve_rq_initialize(qp, attr, &resp);
	if (err)
		goto err_destroy_qp;

	err = nerve_sq_initialize(qp, attr, &resp);
	if (err)
		goto err_terminate_rq;

	pthread_spin_lock(&ctx->qp_table_lock);
	ctx->qp_table[ibvqp->qp_num & ctx->qp_table_sz_m1] = qp;
	pthread_spin_unlock(&ctx->qp_table_lock);

	if (attr->comp_mask & IBV_QP_INIT_ATTR_SEND_OPS_FLAGS) {
		nerve_qp_fill_wr_pfns(&qp->verbs_qp.qp_ex, attr);
		qp->verbs_qp.comp_mask |= VERBS_QP_EX;
	}

	pd = to_nerve_pd(attr->pd);
	if (pd->orig_pd) {
		parent_domain = to_nerve_parent_domain(attr->pd);
		qp->parent_domain = parent_domain;
		atomic_fetch_add(&parent_domain->refcount, 1);
	}

	return ibvqp;

err_terminate_rq:
	nerve_rq_terminate(qp);
err_destroy_qp:
	ibv_cmd_destroy_qp(ibvqp);
err_free_qp:
	free(qp);
err_out:
	errno = err;
	verbs_err(verbs_get_ctx(ibvctx), "Failed to create QP\n");
	return NULL;
}

struct ibv_qp *nerve_create_qp(struct ibv_pd *ibvpd,
			     struct ibv_qp_init_attr *attr)
{
	struct ibv_qp_init_attr_ex attr_ex = {};
	struct nervedv_qp_init_attr nerve_attr = {};
	struct ibv_qp *ibvqp;

	if (attr->qp_type != IBV_QPT_UD) {
		verbs_err(verbs_get_ctx(ibvpd->context),
			  "Unsupported QP type %d\n", attr->qp_type);
		errno = EOPNOTSUPP;
		return NULL;
	}

	memcpy(&attr_ex, attr, sizeof(*attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;

	ibvqp = create_qp(ibvpd->context, &attr_ex, &nerve_attr);
	if (ibvqp)
		memcpy(attr, &attr_ex, sizeof(*attr));

	return ibvqp;
}

struct ibv_qp *nerve_create_qp_ex(struct ibv_context *ibvctx,
				struct ibv_qp_init_attr_ex *attr_ex)
{
	struct nervedv_qp_init_attr nerve_attr = {};

	if (attr_ex->qp_type != IBV_QPT_UD) {
		verbs_err(verbs_get_ctx(ibvctx), "Unsupported QP type\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	return create_qp(ibvctx, attr_ex, &nerve_attr);
}

struct ibv_qp *nervedv_create_driver_qp(struct ibv_pd *ibvpd,
				      struct ibv_qp_init_attr *attr,
				      uint32_t driver_qp_type)
{
	struct ibv_qp_init_attr_ex attr_ex = {};
	struct nervedv_qp_init_attr nerve_attr = {};
	struct ibv_qp *ibvqp;

	if (!is_nerve_dev(ibvpd->context->device)) {
		verbs_err(verbs_get_ctx(ibvpd->context), "Not an NERVE device\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	if (attr->qp_type != IBV_QPT_DRIVER) {
		verbs_err(verbs_get_ctx(ibvpd->context),
			  "QP type not IBV_QPT_DRIVER\n");
		errno = EINVAL;
		return NULL;
	}

	memcpy(&attr_ex, attr, sizeof(*attr));
	attr_ex.comp_mask = IBV_QP_INIT_ATTR_PD;
	attr_ex.pd = ibvpd;
	nerve_attr.driver_qp_type = driver_qp_type;

	ibvqp = create_qp(ibvpd->context, &attr_ex, &nerve_attr);
	if (ibvqp)
		memcpy(attr, &attr_ex, sizeof(*attr));

	return ibvqp;
}

struct ibv_qp *nervedv_create_qp_ex(struct ibv_context *ibvctx,
				  struct ibv_qp_init_attr_ex *attr_ex,
				  struct nervedv_qp_init_attr *nerve_attr,
				  uint32_t inlen)
{
	struct nervedv_qp_init_attr local_nerve_attr = {};

	if (!is_nerve_dev(ibvctx->device)) {
		verbs_err(verbs_get_ctx(ibvctx), "Not an NERVE device\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	if (attr_ex->qp_type != IBV_QPT_DRIVER ||
	    !vext_field_avail(struct nervedv_qp_init_attr,
			      driver_qp_type, inlen) ||
	    nerve_attr->comp_mask ||
	    nerve_attr->reserved ||
	    (inlen > sizeof(*nerve_attr) && !is_ext_cleared(nerve_attr, inlen))) {
		verbs_err(verbs_get_ctx(ibvctx), "Compatibility issues\n");
		errno = EINVAL;
		return NULL;
	}

	memcpy(&local_nerve_attr, nerve_attr, min_t(uint32_t, inlen, sizeof(local_nerve_attr)));
	return create_qp(ibvctx, attr_ex, &local_nerve_attr);
}

int nerve_modify_qp(struct ibv_qp *ibvqp, struct ibv_qp_attr *attr,
		  int attr_mask)
{
	struct nerve_qp *qp = to_nerve_qp(ibvqp);
	struct ibv_modify_qp cmd = {};
	int err;

	err = ibv_cmd_modify_qp(ibvqp, attr, attr_mask, &cmd, sizeof(cmd));
	if (err) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "Failed to modify QP[%u]\n", qp->verbs_qp.qp.qp_num);
		return err;
	}

	if (attr_mask & IBV_QP_STATE) {
		qp->verbs_qp.qp.state = attr->qp_state;
		/* transition to reset */
		if (qp->verbs_qp.qp.state == IBV_QPS_RESET)
			nerve_qp_init_indices(qp);
	}

	return 0;
}

int nerve_query_qp(struct ibv_qp *ibvqp, struct ibv_qp_attr *attr,
		 int attr_mask, struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd;

	return ibv_cmd_query_qp(ibvqp, attr, attr_mask, init_attr,
				&cmd, sizeof(cmd));
}

int nervedv_query_qp_wqs(struct ibv_qp *ibvqp, struct nervedv_wq_attr *sq_attr,
		       struct nervedv_wq_attr *rq_attr, uint32_t inlen)
{
	struct nerve_qp *qp = to_nerve_qp(ibvqp);

	if (!is_nerve_dev(ibvqp->context->device)) {
		verbs_err(verbs_get_ctx(ibvqp->context), "Not an NERVE device\n");
		return EOPNOTSUPP;
	}

	if (!vext_field_avail(typeof(*sq_attr), max_batch, inlen)) {
		verbs_err(verbs_get_ctx(ibvqp->context), "Compatibility issues\n");
		return EINVAL;
	}

	sq_attr->comp_mask = 0;
	sq_attr->buffer = qp->sq.desc;
	sq_attr->entry_size = sizeof(struct nerve_io_tx_wqe);
	sq_attr->num_entries = qp->sq.wq.wqe_cnt;
	sq_attr->doorbell = qp->sq.wq.db;
	sq_attr->max_batch = qp->sq.max_batch_wr;

	rq_attr->comp_mask = 0;
	rq_attr->buffer = qp->rq.buf;
	rq_attr->entry_size = sizeof(struct nerve_io_rx_desc);
	rq_attr->num_entries = qp->rq.wq.desc_mask + 1;
	rq_attr->doorbell = qp->rq.wq.db;
	rq_attr->max_batch = rq_attr->num_entries;

	return 0;
}

int nerve_query_qp_data_in_order(struct ibv_qp *ibvqp, enum ibv_wr_opcode op,
			       uint32_t flags)
{
	struct nerve_context *ctx = to_nerve_context(ibvqp->context);
	int caps = 0;

	if (NERVE_DEV_CAP(ctx, DATA_POLLING_128))
		caps |= IBV_QUERY_QP_DATA_IN_ORDER_ALIGNED_128_BYTES;

	return caps;
}

int nerve_destroy_qp(struct ibv_qp *ibvqp)
{
	struct nerve_context *ctx = to_nerve_context(ibvqp->context);
	struct nerve_qp *qp = to_nerve_qp(ibvqp);
	int err;

	err = ibv_cmd_destroy_qp(ibvqp);
	if (err) {
		verbs_err(&ctx->ibvctx, "Failed to destroy QP[%u]\n",
			  ibvqp->qp_num);
		return err;
	}

	if (qp->parent_domain)
		atomic_fetch_sub(&qp->parent_domain->refcount, 1);

	pthread_spin_lock(&ctx->qp_table_lock);
	nerve_lock_cqs(ibvqp);

	ctx->qp_table[ibvqp->qp_num & ctx->qp_table_sz_m1] = NULL;

	nerve_unlock_cqs(ibvqp);
	pthread_spin_unlock(&ctx->qp_table_lock);

	nerve_sq_terminate(qp);
	nerve_rq_terminate(qp);

	free(qp);
	return 0;
}

static void nerve_set_tx_buf(struct nerve_io_tx_buf_desc *tx_buf,
			   uint64_t addr, uint32_t lkey,
			   uint32_t length)
{
	tx_buf->length = length;
	NERVE_SET(&tx_buf->lkey, NERVE_IO_TX_BUF_DESC_LKEY, lkey);
	tx_buf->buf_addr_lo = addr & 0xffffffff;
	tx_buf->buf_addr_hi = addr >> 32;
}

static void nerve_post_send_sgl(struct nerve_io_tx_buf_desc *tx_bufs,
			      const struct ibv_sge *sg_list,
			      int num_sge)
{
	const struct ibv_sge *sge;
	size_t i;

	for (i = 0; i < num_sge; i++) {
		sge = &sg_list[i];
		nerve_set_tx_buf(&tx_bufs[i], sge->addr, sge->lkey, sge->length);
	}
}

static void nerve_post_send_inline_data(const struct ibv_send_wr *wr,
				      struct nerve_io_tx_wqe *tx_wqe)
{
	const struct ibv_sge *sgl = wr->sg_list;
	uint32_t total_length = 0;
	uint32_t length;
	size_t i;

	for (i = 0; i < wr->num_sge; i++) {
		length = sgl[i].length;

		memcpy(tx_wqe->data.inline_data + total_length,
		       (void *)(uintptr_t)sgl[i].addr, length);
		total_length += length;
	}

	NERVE_SET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_INLINE_MSG, 1);
	tx_wqe->meta.length = total_length;
}

static size_t nerve_sge_total_bytes(const struct ibv_sge *sg_list, int num_sge)
{
	size_t bytes = 0;
	size_t i;

	for (i = 0; i < num_sge; i++)
		bytes += sg_list[i].length;

	return bytes;
}

static size_t nerve_buf_list_total_bytes(const struct ibv_data_buf *buf_list,
				       size_t num_buf)
{
	size_t bytes = 0;
	size_t i;

	for (i = 0; i < num_buf; i++)
		bytes += buf_list[i].length;

	return bytes;
}

static void nerve_sq_advance_post_idx(struct nerve_sq *sq)
{
	struct nerve_wq *wq = &sq->wq;

	wq->wqe_posted++;
	wq->pc++;

	if (!(wq->pc & wq->desc_mask))
		wq->phase++;
}

static inline void nerve_rq_ring_doorbell(struct nerve_rq *rq, uint16_t pc)
{
	udma_to_device_barrier();
	mmio_write32(rq->wq.db, pc);
}

static inline void nerve_sq_ring_doorbell(struct nerve_sq *sq, uint16_t pc)
{
	mmio_write32(sq->wq.db, pc);
}

static void nerve_set_common_ctrl_flags(struct nerve_io_tx_meta_desc *desc,
				      struct nerve_sq *sq,
				      enum nerve_io_send_op_type op_type)
{
	NERVE_SET(&desc->ctrl1, NERVE_IO_TX_META_DESC_META_DESC, 1);
	NERVE_SET(&desc->ctrl1, NERVE_IO_TX_META_DESC_OP_TYPE, op_type);
	NERVE_SET(&desc->ctrl2, NERVE_IO_TX_META_DESC_PHASE, sq->wq.phase);
	NERVE_SET(&desc->ctrl2, NERVE_IO_TX_META_DESC_FIRST, 1);
	NERVE_SET(&desc->ctrl2, NERVE_IO_TX_META_DESC_LAST, 1);
	NERVE_SET(&desc->ctrl2, NERVE_IO_TX_META_DESC_COMP_REQ, 1);
}

#ifdef LTTNG_ENABLED
static uint32_t nerve_get_wqe_length(struct nerve_io_tx_wqe *tx_wqe)
{
	enum nerve_io_send_op_type op_type;
	uint32_t length = 0;
	size_t i;

	op_type = NERVE_GET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_OP_TYPE);
	switch (op_type) {
	case NERVE_IO_SEND:
		if (NERVE_GET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_INLINE_MSG))
			return tx_wqe->meta.length;

		for (i = 0; i < tx_wqe->meta.length; i++)
			length += tx_wqe->data.sgl[i].length;

		return length;
	case NERVE_IO_RDMA_READ:
	case NERVE_IO_RDMA_WRITE:
		return tx_wqe->data.rdma_req.remote_mem.length;
	}

	return 0;
}
#endif

static int nerve_post_send_validate(struct nerve_qp *qp,
				  unsigned int wr_flags)
{
	if (unlikely(qp->verbs_qp.qp.state != IBV_QPS_RTS &&
		     qp->verbs_qp.qp.state != IBV_QPS_SQD)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] is in invalid state\n",
			  qp->verbs_qp.qp.qp_num);
		return EINVAL;
	}

	if (unlikely(!(wr_flags & IBV_SEND_SIGNALED) && !qp->sq_sig_all)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] Non signaled WRs not supported\n",
			  qp->verbs_qp.qp.qp_num);
		return EINVAL;
	}

	if (unlikely(wr_flags & ~(IBV_SEND_SIGNALED | IBV_SEND_INLINE))) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] Unsupported wr_flags[%#x] supported[%#x]\n",
			  qp->verbs_qp.qp.qp_num, wr_flags,
			  ~(IBV_SEND_SIGNALED | IBV_SEND_INLINE));
		return EINVAL;
	}

	if (unlikely(qp->sq.wq.wqe_posted - qp->sq.wq.wqe_completed ==
		     qp->sq.wq.wqe_cnt)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] is full wqe_posted[%u] wqe_completed[%u] wqe_cnt[%u]\n",
			  qp->verbs_qp.qp.qp_num, qp->sq.wq.wqe_posted,
			  qp->sq.wq.wqe_completed, qp->sq.wq.wqe_cnt);
		return ENOMEM;
	}

	return 0;
}

static int nerve_post_send_validate_wr(struct nerve_qp *qp,
				     const struct ibv_send_wr *wr)
{
	int err;

	err = nerve_post_send_validate(qp, wr->send_flags);
	if (unlikely(err))
		return err;

	if (unlikely(wr->opcode != IBV_WR_SEND &&
		     wr->opcode != IBV_WR_SEND_WITH_IMM)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] unsupported opcode %d\n",
			  qp->verbs_qp.qp.qp_num, wr->opcode);
		return EINVAL;
	}

	if (wr->send_flags & IBV_SEND_INLINE) {
		if (unlikely(nerve_sge_total_bytes(wr->sg_list, wr->num_sge) >
			     qp->sq.max_inline_data)) {
			verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
				  "SQ[%u] WR total bytes %zu > %zu\n",
				  qp->verbs_qp.qp.qp_num,
				  nerve_sge_total_bytes(wr->sg_list,
						      wr->num_sge),
				  qp->sq.max_inline_data);
			return EINVAL;
		}
	} else {
		if (unlikely(wr->num_sge > qp->sq.wq.max_sge)) {
			verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
				  "SQ[%u] WR num_sge %d > %d\n",
				  qp->verbs_qp.qp.qp_num, wr->num_sge,
				  qp->sq.wq.max_sge);
			return EINVAL;
	}
	}

	return 0;
}

int nerve_post_send(struct ibv_qp *ibvqp, struct ibv_send_wr *wr,
		  struct ibv_send_wr **bad)
{
	struct nerve_io_tx_meta_desc *meta_desc;
	struct nerve_qp *qp = to_nerve_qp(ibvqp);
	struct nerve_io_tx_wqe tx_wqe;
	struct nerve_sq *sq = &qp->sq;
	struct nerve_wq *wq = &sq->wq;
	uint32_t sq_desc_offset;
	uint32_t curbatch = 0;
	struct nerve_ah *ah;
	int err = 0;

	if (wq->need_lock)
		mmio_wc_spinlock(&wq->wqlock);
	else
		mmio_wc_start();

	while (wr) {
		err = nerve_post_send_validate_wr(qp, wr);
		if (err) {
			*bad = wr;
			goto ring_db;
		}

		memset(&tx_wqe, 0, sizeof(tx_wqe));
		meta_desc = &tx_wqe.meta;
		ah = to_nerve_ah(wr->wr.ud.ah);

		if (wr->send_flags & IBV_SEND_INLINE) {
			nerve_post_send_inline_data(wr, &tx_wqe);
		} else {
			meta_desc->length = wr->num_sge;
			nerve_post_send_sgl(tx_wqe.data.sgl, wr->sg_list,
					  wr->num_sge);
		}

		if (wr->opcode == IBV_WR_SEND_WITH_IMM) {
			meta_desc->immediate_data = be32toh(wr->imm_data);
			NERVE_SET(&meta_desc->ctrl1, NERVE_IO_TX_META_DESC_HAS_IMM,
				1);
		}

		/* Set rest of the descriptor fields */
		nerve_set_common_ctrl_flags(meta_desc, sq, NERVE_IO_SEND);
		meta_desc->req_id = nerve_wq_get_next_wrid_idx_locked(wq,
								    wr->wr_id);
		meta_desc->dest_qp_num = wr->wr.ud.remote_qpn;
		meta_desc->ah = ah->nerve_ah;
		meta_desc->qkey = wr->wr.ud.remote_qkey;

		/* Copy descriptor */
		sq_desc_offset = (wq->pc & wq->desc_mask) *
				 sizeof(tx_wqe);
		mmio_memcpy_x64(sq->desc + sq_desc_offset, &tx_wqe,
				sizeof(tx_wqe));

		/* advance index and change phase */
		nerve_sq_advance_post_idx(sq);
		curbatch++;

		if (curbatch == sq->max_batch_wr) {
			curbatch = 0;
			mmio_flush_writes();
			nerve_sq_ring_doorbell(sq, wq->pc);
			mmio_wc_start();
		}
		wr = wr->next;
	}

ring_db:
	if (curbatch) {
		mmio_flush_writes();
		nerve_sq_ring_doorbell(sq, wq->pc);
	}

	/*
	 * Not using mmio_wc_spinunlock as the doorbell write should be done
	 * inside the lock.
	 */
	if (wq->need_lock)
		pthread_spin_unlock(&wq->wqlock);

	return err;
}

static struct nerve_io_tx_wqe *nerve_send_wr_common(struct ibv_qp_ex *ibvqpx,
						enum nerve_io_send_op_type op_type)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_sq *sq = &qp->sq;
	struct nerve_io_tx_meta_desc *meta_desc;
	int err;

	if (unlikely(qp->wr_session_err))
		return NULL;

	err = nerve_post_send_validate(qp, ibvqpx->wr_flags);
	if (unlikely(err)) {
		qp->wr_session_err = err;
		return NULL;
	}

	sq->curr_tx_wqe = (struct nerve_io_tx_wqe *)sq->local_queue +
			  sq->num_wqe_pending;
	memset(sq->curr_tx_wqe, 0, sizeof(*sq->curr_tx_wqe));

	meta_desc = &sq->curr_tx_wqe->meta;
	nerve_set_common_ctrl_flags(meta_desc, sq, op_type);
	meta_desc->req_id = nerve_wq_get_next_wrid_idx_locked(&sq->wq,
							    ibvqpx->wr_id);

	/* advance index and change phase */
	nerve_sq_advance_post_idx(sq);
	sq->num_wqe_pending++;

	return sq->curr_tx_wqe;
}

static void nerve_send_wr_set_imm_data(struct nerve_io_tx_wqe *tx_wqe, __be32 imm_data)
{
	struct nerve_io_tx_meta_desc *meta_desc;

	meta_desc = &tx_wqe->meta;
	meta_desc->immediate_data = be32toh(imm_data);
	NERVE_SET(&meta_desc->ctrl1, NERVE_IO_TX_META_DESC_HAS_IMM, 1);
}

static void nerve_send_wr_set_rdma_addr(struct nerve_io_tx_wqe *tx_wqe, uint32_t rkey,
				      uint64_t remote_addr)
{
	struct nerve_io_remote_mem_addr *remote_mem;

	remote_mem = &tx_wqe->data.rdma_req.remote_mem;
	remote_mem->rkey = rkey;
	remote_mem->buf_addr_lo = remote_addr & 0xFFFFFFFF;
	remote_mem->buf_addr_hi = remote_addr >> 32;
}

static void nerve_send_wr_send(struct ibv_qp_ex *ibvqpx)
{
	nerve_send_wr_common(ibvqpx, NERVE_IO_SEND);
}

static void nerve_send_wr_send_imm(struct ibv_qp_ex *ibvqpx, __be32 imm_data)
{
	struct nerve_io_tx_wqe *tx_wqe;

	tx_wqe = nerve_send_wr_common(ibvqpx, NERVE_IO_SEND);
	if (unlikely(!tx_wqe))
		return;

	nerve_send_wr_set_imm_data(tx_wqe, imm_data);
}

static void nerve_send_wr_rdma_read(struct ibv_qp_ex *ibvqpx, uint32_t rkey,
				  uint64_t remote_addr)
{
	struct nerve_io_tx_wqe *tx_wqe;

	tx_wqe = nerve_send_wr_common(ibvqpx, NERVE_IO_RDMA_READ);
	if (unlikely(!tx_wqe))
		return;

	nerve_send_wr_set_rdma_addr(tx_wqe, rkey, remote_addr);
}

static void nerve_send_wr_rdma_write(struct ibv_qp_ex *ibvqpx, uint32_t rkey,
				   uint64_t remote_addr)
{
	struct nerve_io_tx_wqe *tx_wqe;

	tx_wqe = nerve_send_wr_common(ibvqpx, NERVE_IO_RDMA_WRITE);
	if (unlikely(!tx_wqe))
		return;

	nerve_send_wr_set_rdma_addr(tx_wqe, rkey, remote_addr);
}

static void nerve_send_wr_rdma_write_imm(struct ibv_qp_ex *ibvqpx, uint32_t rkey,
				       uint64_t remote_addr, __be32 imm_data)
{
	struct nerve_io_tx_wqe *tx_wqe;

	tx_wqe = nerve_send_wr_common(ibvqpx, NERVE_IO_RDMA_WRITE);
	if (unlikely(!tx_wqe))
		return;

	nerve_send_wr_set_rdma_addr(tx_wqe, rkey, remote_addr);
	nerve_send_wr_set_imm_data(tx_wqe, imm_data);
}

static void nerve_send_wr_set_sge(struct ibv_qp_ex *ibvqpx, uint32_t lkey,
				uint64_t addr, uint32_t length)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_io_tx_buf_desc *buf;
	struct nerve_io_tx_wqe *tx_wqe;
	uint8_t op_type;

	if (unlikely(qp->wr_session_err))
		return;

	tx_wqe = qp->sq.curr_tx_wqe;
	tx_wqe->meta.length = 1;

	op_type = NERVE_GET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_OP_TYPE);
	switch (op_type) {
	case NERVE_IO_SEND:
		buf = &tx_wqe->data.sgl[0];
		break;
	case NERVE_IO_RDMA_READ:
	case NERVE_IO_RDMA_WRITE:
		tx_wqe->data.rdma_req.remote_mem.length = length;
		buf = &tx_wqe->data.rdma_req.local_mem[0];
		break;
	default:
		return;
	}

	nerve_set_tx_buf(buf, addr, lkey, length);
}

static void nerve_send_wr_set_sge_list(struct ibv_qp_ex *ibvqpx, size_t num_sge,
				     const struct ibv_sge *sg_list)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_io_rdma_req *rdma_req;
	struct nerve_io_tx_wqe *tx_wqe;
	struct nerve_sq *sq = &qp->sq;
	uint8_t op_type;

	if (unlikely(qp->wr_session_err))
		return;

	tx_wqe = sq->curr_tx_wqe;
	op_type = NERVE_GET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_OP_TYPE);
	switch (op_type) {
	case NERVE_IO_SEND:
		if (unlikely(num_sge > sq->wq.max_sge)) {
			verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
				  "SQ[%u] num_sge[%zu] > max_sge[%u]\n",
				  ibvqpx->qp_base.qp_num, num_sge,
				  sq->wq.max_sge);
			qp->wr_session_err = EINVAL;
			return;
		}
		nerve_post_send_sgl(tx_wqe->data.sgl, sg_list, num_sge);
		break;
	case NERVE_IO_RDMA_READ:
	case NERVE_IO_RDMA_WRITE:
		if (unlikely(num_sge > sq->max_wr_rdma_sge)) {
			verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
				  "SQ[%u] num_sge[%zu] > max_rdma_sge[%zu]\n",
				  ibvqpx->qp_base.qp_num, num_sge,
				  sq->max_wr_rdma_sge);
			qp->wr_session_err = EINVAL;
			return;
		}
		rdma_req = &tx_wqe->data.rdma_req;
		rdma_req->remote_mem.length = nerve_sge_total_bytes(sg_list,
								  num_sge);
		nerve_post_send_sgl(rdma_req->local_mem, sg_list, num_sge);
		break;
	default:
		return;
	}

	tx_wqe->meta.length = num_sge;
}

static void nerve_send_wr_set_inline_data(struct ibv_qp_ex *ibvqpx, void *addr,
					size_t length)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_io_tx_wqe *tx_wqe = qp->sq.curr_tx_wqe;

	if (unlikely(qp->wr_session_err))
		return;

	if (unlikely(length > qp->sq.max_inline_data)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] WR inline length %zu > %zu\n",
			  ibvqpx->qp_base.qp_num, length,
			  qp->sq.max_inline_data);
		qp->wr_session_err = EINVAL;
		return;
	}

	NERVE_SET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_INLINE_MSG, 1);
	memcpy(tx_wqe->data.inline_data, addr, length);
	tx_wqe->meta.length = length;
}

static void
nerve_send_wr_set_inline_data_list(struct ibv_qp_ex *ibvqpx,
				 size_t num_buf,
				 const struct ibv_data_buf *buf_list)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_io_tx_wqe *tx_wqe = qp->sq.curr_tx_wqe;
	uint32_t total_length = 0;
	uint32_t length;
	size_t i;

	if (unlikely(qp->wr_session_err))
		return;

	if (unlikely(nerve_buf_list_total_bytes(buf_list, num_buf) >
		     qp->sq.max_inline_data)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "SQ[%u] WR inline length %zu > %zu\n",
			  ibvqpx->qp_base.qp_num,
			  nerve_buf_list_total_bytes(buf_list, num_buf),
			  qp->sq.max_inline_data);
		qp->wr_session_err = EINVAL;
		return;
	}

	for (i = 0; i < num_buf; i++) {
		length = buf_list[i].length;

		memcpy(tx_wqe->data.inline_data + total_length,
		       buf_list[i].addr, length);
		total_length += length;
	}

	NERVE_SET(&tx_wqe->meta.ctrl1, NERVE_IO_TX_META_DESC_INLINE_MSG, 1);
	tx_wqe->meta.length = total_length;
}

static void nerve_send_wr_set_addr(struct ibv_qp_ex *ibvqpx,
				 struct ibv_ah *ibvah,
				 uint32_t remote_qpn, uint32_t remote_qkey)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_ah *ah = to_nerve_ah(ibvah);
	struct nerve_io_tx_wqe *tx_wqe;

	if (unlikely(qp->wr_session_err))
		return;

	tx_wqe = qp->sq.curr_tx_wqe;

	tx_wqe->meta.dest_qp_num = remote_qpn;
	tx_wqe->meta.ah = ah->nerve_ah;
	tx_wqe->meta.qkey = remote_qkey;
}

static void nerve_send_wr_start(struct ibv_qp_ex *ibvqpx)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_sq *sq = &qp->sq;

	if (qp->sq.wq.need_lock)
		mmio_wc_spinlock(&qp->sq.wq.wqlock);
	else
		mmio_wc_start();

	qp->wr_session_err = 0;
	sq->num_wqe_pending = 0;
	sq->phase_rb = qp->sq.wq.phase;
}

static inline void nerve_sq_roll_back(struct nerve_sq *sq)
{
	struct nerve_qp *qp = container_of(sq, struct nerve_qp, sq);
	struct nerve_wq *wq = &sq->wq;

	verbs_debug(verbs_get_ctx(qp->verbs_qp.qp.context),
		    "SQ[%u] Rollback num_wqe_pending = %u\n",
		    qp->verbs_qp.qp.qp_num, sq->num_wqe_pending);
	wq->wqe_posted -= sq->num_wqe_pending;
	wq->pc -= sq->num_wqe_pending;
	wq->wrid_idx_pool_next -= sq->num_wqe_pending;
	wq->phase = sq->phase_rb;
}

static int nerve_send_wr_complete(struct ibv_qp_ex *ibvqpx)
{
	struct nerve_qp *qp = to_nerve_qp_ex(ibvqpx);
	struct nerve_sq *sq = &qp->sq;
	uint32_t max_txbatch = sq->max_batch_wr;
	uint32_t num_wqe_to_copy;
	uint16_t local_idx = 0;
	uint16_t curbatch = 0;
	uint16_t sq_desc_idx;
	uint16_t pc;

	if (unlikely(qp->wr_session_err)) {
		nerve_sq_roll_back(sq);
		goto out;
	}

	/*
	 * Copy local queue to device in chunks, handling wraparound and max
	 * doorbell batch.
	 */
	pc = sq->wq.pc - sq->num_wqe_pending;
	sq_desc_idx = pc & sq->wq.desc_mask;

	/* mmio_wc_start() comes from nerve_send_wr_start() */
	while (sq->num_wqe_pending) {
		num_wqe_to_copy = min3(sq->num_wqe_pending,
				       sq->wq.wqe_cnt - sq_desc_idx,
				       max_txbatch - curbatch);
		mmio_memcpy_x64((struct nerve_io_tx_wqe *)sq->desc +
							sq_desc_idx,
				(struct nerve_io_tx_wqe *)sq->local_queue +
							local_idx,
				num_wqe_to_copy * sizeof(struct nerve_io_tx_wqe));

		sq->num_wqe_pending -= num_wqe_to_copy;
		local_idx += num_wqe_to_copy;
		curbatch += num_wqe_to_copy;
		pc += num_wqe_to_copy;
		sq_desc_idx = (sq_desc_idx + num_wqe_to_copy) &
			      sq->wq.desc_mask;

		if (curbatch == max_txbatch) {
			mmio_flush_writes();
			nerve_sq_ring_doorbell(sq, pc);
			curbatch = 0;
			mmio_wc_start();
		}
	}

	if (curbatch) {
		mmio_flush_writes();
		nerve_sq_ring_doorbell(sq, sq->wq.pc);
	}
out:
	/*
	 * Not using mmio_wc_spinunlock as the doorbell write should be done
	 * inside the lock.
	 */
	if (sq->wq.need_lock)
		pthread_spin_unlock(&sq->wq.wqlock);

	return qp->wr_session_err;
}

static void nerve_send_wr_abort(struct ibv_qp_ex *ibvqpx)
{
	struct nerve_sq *sq = &to_nerve_qp_ex(ibvqpx)->sq;

	nerve_sq_roll_back(sq);
	if (sq->wq.need_lock)
		pthread_spin_unlock(&sq->wq.wqlock);
}

static void nerve_qp_fill_wr_pfns(struct ibv_qp_ex *ibvqpx,
				struct ibv_qp_init_attr_ex *attr_ex)
{
	ibvqpx->wr_start = nerve_send_wr_start;
	ibvqpx->wr_complete = nerve_send_wr_complete;
	ibvqpx->wr_abort = nerve_send_wr_abort;

	if (attr_ex->send_ops_flags & IBV_QP_EX_WITH_SEND)
		ibvqpx->wr_send = nerve_send_wr_send;

	if (attr_ex->send_ops_flags & IBV_QP_EX_WITH_SEND_WITH_IMM)
		ibvqpx->wr_send_imm = nerve_send_wr_send_imm;

	if (attr_ex->send_ops_flags & IBV_QP_EX_WITH_RDMA_READ)
		ibvqpx->wr_rdma_read = nerve_send_wr_rdma_read;

	if (attr_ex->send_ops_flags & IBV_QP_EX_WITH_RDMA_WRITE)
		ibvqpx->wr_rdma_write = nerve_send_wr_rdma_write;

	if (attr_ex->send_ops_flags & IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM)
		ibvqpx->wr_rdma_write_imm = nerve_send_wr_rdma_write_imm;

	ibvqpx->wr_set_inline_data = nerve_send_wr_set_inline_data;
	ibvqpx->wr_set_inline_data_list = nerve_send_wr_set_inline_data_list;
	ibvqpx->wr_set_sge = nerve_send_wr_set_sge;
	ibvqpx->wr_set_sge_list = nerve_send_wr_set_sge_list;
	ibvqpx->wr_set_ud_addr = nerve_send_wr_set_addr;
}

static int nerve_post_recv_validate(struct nerve_qp *qp, struct ibv_recv_wr *wr)
{
	if (unlikely(qp->verbs_qp.qp.state == IBV_QPS_RESET ||
		     qp->verbs_qp.qp.state == IBV_QPS_ERR)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "RQ[%u] Invalid QP state\n",
			  qp->verbs_qp.qp.qp_num);
		return EINVAL;
	}

	if (unlikely(wr->num_sge > qp->rq.wq.max_sge)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "RQ[%u] WR num_sge %d > %d\n",
			  qp->verbs_qp.qp.qp_num, wr->num_sge,
			  qp->rq.wq.max_sge);
		return EINVAL;
	}

	if (unlikely(qp->rq.wq.wqe_posted - qp->rq.wq.wqe_completed ==
		     qp->rq.wq.wqe_cnt)) {
		verbs_err(verbs_get_ctx(qp->verbs_qp.qp.context),
			  "RQ[%u] is full wqe_posted[%u] wqe_completed[%u] wqe_cnt[%u]\n",
			  qp->verbs_qp.qp.qp_num, qp->rq.wq.wqe_posted,
			  qp->rq.wq.wqe_completed, qp->rq.wq.wqe_cnt);
		return ENOMEM;
	}

	return 0;
}

int nerve_post_recv(struct ibv_qp *ibvqp, struct ibv_recv_wr *wr,
		  struct ibv_recv_wr **bad)
{
	struct nerve_qp *qp = to_nerve_qp(ibvqp);
	struct nerve_wq *wq = &qp->rq.wq;
	struct nerve_io_rx_desc rx_buf;
	uint32_t rq_desc_offset;
	uintptr_t addr;
	int err = 0;
	size_t i;

	if (wq->need_lock)
		pthread_spin_lock(&wq->wqlock);

	while (wr) {
		err = nerve_post_recv_validate(qp, wr);
		if (err) {
			*bad = wr;
			goto ring_db;
		}

		memset(&rx_buf, 0, sizeof(rx_buf));

		rx_buf.req_id = nerve_wq_get_next_wrid_idx_locked(wq, wr->wr_id);
		wq->wqe_posted++;

		/* Default init of the rx buffer */
		NERVE_SET(&rx_buf.lkey_ctrl, NERVE_IO_RX_DESC_FIRST, 1);
		NERVE_SET(&rx_buf.lkey_ctrl, NERVE_IO_RX_DESC_LAST, 0);

		for (i = 0; i < wr->num_sge; i++) {
			/* Set last indication if need) */
			if (i == wr->num_sge - 1)
				NERVE_SET(&rx_buf.lkey_ctrl, NERVE_IO_RX_DESC_LAST,
					1);

			addr = wr->sg_list[i].addr;

			/* Set RX buffer desc from SGE */
			rx_buf.length = min_t(uint32_t, wr->sg_list[i].length, UINT16_MAX);
			NERVE_SET(&rx_buf.lkey_ctrl, NERVE_IO_RX_DESC_LKEY,
				wr->sg_list[i].lkey);
			rx_buf.buf_addr_lo = addr;
			rx_buf.buf_addr_hi = (uint64_t)addr >> 32;

			/* Copy descriptor to RX ring */
			rq_desc_offset = (wq->pc & wq->desc_mask) *
					 sizeof(rx_buf);
			memcpy(qp->rq.buf + rq_desc_offset, &rx_buf, sizeof(rx_buf));

			/* Wrap rx descriptor index */
			wq->pc++;
			if (!(wq->pc & wq->desc_mask))
				wq->phase++;

			/* reset descriptor for next iov */
			memset(&rx_buf, 0, sizeof(rx_buf));
		}
		wr = wr->next;
	}

ring_db:
	nerve_rq_ring_doorbell(&qp->rq, wq->pc);

	if (wq->need_lock)
		pthread_spin_unlock(&wq->wqlock);

	return err;
}

int nervedv_query_ah(struct ibv_ah *ibvah, struct nervedv_ah_attr *attr,
		   uint32_t inlen)
{
	uint64_t comp_mask_out = 0;

	if (!is_nerve_dev(ibvah->context->device)) {
		verbs_err(verbs_get_ctx(ibvah->context), "Not an NERVE device\n");
		return EOPNOTSUPP;
	}

	if (!vext_field_avail(typeof(*attr), ahn, inlen)) {
		verbs_err(verbs_get_ctx(ibvah->context),
			  "Compatibility issues\n");
		return EINVAL;
	}

	memset(attr, 0, inlen);
	attr->ahn = to_nerve_ah(ibvah)->nerve_ah;

	attr->comp_mask = comp_mask_out;

	return 0;
}

struct ibv_ah *nerve_create_ah(struct ibv_pd *ibvpd, struct ibv_ah_attr *attr)
{
	struct nerve_create_ah_resp resp = {};
	struct nerve_ah *ah;
	int err;

	ah = calloc(1, sizeof(*ah));
	if (!ah)
		return NULL;

	err = ibv_cmd_create_ah(ibvpd, &ah->ibvah, attr,
				&resp.ibv_resp, sizeof(resp));
	if (err) {
		verbs_err(verbs_get_ctx(ibvpd->context),
			  "Failed to create AH\n");
		free(ah);
		errno = err;
		return NULL;
	}

	ah->nerve_ah = resp.nerve_address_handle;

	return &ah->ibvah;
}

int nerve_destroy_ah(struct ibv_ah *ibvah)
{
	struct nerve_ah *ah;
	int err;

	ah = to_nerve_ah(ibvah);
	err = ibv_cmd_destroy_ah(ibvah);
	if (err) {
		verbs_err(verbs_get_ctx(ibvah->context),
			  "Failed to destroy AH\n");
		return err;
	}
	free(ah);

	return 0;
}

struct ibv_td *nerve_alloc_td(struct ibv_context *ibvctx, struct ibv_td_init_attr *init_attr)
{
	struct nerve_td *td;

	if (!check_comp_mask(init_attr->comp_mask, 0)) {
		verbs_err(verbs_get_ctx(ibvctx), "Invalid comp_mask\n");
		errno = EOPNOTSUPP;
		return NULL;
	}

	td = calloc(1, sizeof(*td));
	if (!td) {
		errno = ENOMEM;
		return NULL;
	}

	td->ibvtd.context = ibvctx;
	atomic_init(&td->refcount, 0);

	return &td->ibvtd;
}

int nerve_dealloc_td(struct ibv_td *ibvtd)
{
	struct nerve_td *td;

	td = to_nerve_td(ibvtd);
	if (atomic_load(&td->refcount) > 0)
		return EBUSY;

	free(td);

	return 0;
}
