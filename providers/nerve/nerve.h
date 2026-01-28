/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2019-2025 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __NERVE_H__
#define __NERVE_H__

#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>

#include <infiniband/driver.h>
#include <util/udma_barrier.h>

#include "nerve-abi.h"
#include "nerve_io_defs.h"
#include "nervedv.h"

#define NERVE_GET(ptr, mask) FIELD_GET(mask##_MASK, *(ptr))

#define NERVE_SET(ptr, mask, value)                                              \
	({                                                                     \
		typeof(ptr) _ptr = ptr;                                        \
		*_ptr = (*_ptr & ~(mask##_MASK)) |                             \
			FIELD_PREP(mask##_MASK, value);                        \
	})

struct nerve_context {
	struct verbs_context ibvctx;
	uint32_t cmds_supp_udata_mask;
	uint16_t sub_cqs_per_cq;
	uint16_t inline_buf_size;
	uint32_t max_llq_size;
	uint32_t device_caps;
	uint32_t max_sq_wr;
	uint32_t max_rq_wr;
	uint16_t max_sq_sge;
	uint16_t max_rq_sge;
	uint32_t max_rdma_size;
	uint16_t max_wr_rdma_sge;
	uint16_t max_tx_batch;
	uint16_t min_sq_wr;
	size_t cqe_size;
	size_t ex_cqe_size;
	struct nerve_qp **qp_table;
	unsigned int qp_table_sz_m1;
	pthread_spinlock_t qp_table_lock;
};

struct nerve_pd {
	struct ibv_pd ibvpd;
	uint16_t pdn;
	/* Pointer to original PD used to create parent domain */
	struct nerve_pd *orig_pd;
	/* Number of parent domains referencing this PD */
	atomic_int refcount;
};

struct nerve_td {
	struct ibv_td ibvtd;
	/* Number of parent domains referencing this TD */
	atomic_int refcount;
};

struct nerve_parent_domain {
	struct nerve_pd pd;
	struct nerve_td *td;
	/* Number of objects referencing this parent domain */
	atomic_int refcount;
	void *pd_context;
};

struct nerve_sub_cq {
	uint16_t consumed_cnt;
	int phase;
	uint8_t *buf;
	int qmask;
	int cqe_size;
	uint32_t ref_cnt;
};

struct nerve_cq {
	struct verbs_cq verbs_cq;
	struct nervedv_cq dv_cq;
	uint32_t cqn;
	size_t cqe_size;
	uint8_t *buf;
	size_t buf_size;
	bool buf_mmaped;
	uint32_t *db;
	uint8_t *db_mmap_addr;
	uint16_t cc; /* Consumer Counter */
	uint8_t cmd_sn;
	uint16_t num_sub_cqs;
	/* Index of next sub cq idx to poll. This is used to guarantee fairness for sub cqs */
	uint16_t next_poll_idx;
	pthread_spinlock_t lock;
	struct nerve_wq *cur_wq;
	struct nerve_io_cdesc_common *cur_cqe;
	struct ibv_device *dev;
	struct nerve_parent_domain *parent_domain;
	struct nerve_sub_cq sub_cq_arr[];
};

struct nerve_wq {
	uint64_t *wrid;
	/* wrid_idx_pool: Pool of free indexes in the wrid array, used to select the
	 * wrid entry to be used to hold the next tx packet's context.
	 * At init time, entry N will hold value N, as OOO tx-completions arrive,
	 * the value stored in a given entry might not equal the entry's index.
	 */
	uint32_t *wrid_idx_pool;
	uint32_t wqe_cnt;
	uint32_t wqe_posted;
	uint32_t wqe_completed;
	uint16_t pc; /* Producer counter */
	uint16_t desc_mask;
	/* wrid_idx_pool_next: Index of the next entry to use in wrid_idx_pool. */
	uint16_t wrid_idx_pool_next;
	int max_sge;
	int phase;
	pthread_spinlock_t wqlock;
	bool need_lock;

	uint32_t *db;
	uint16_t sub_cq_idx;
};

struct nerve_rq {
	struct nerve_wq wq;
	uint8_t *buf;
	size_t buf_size;
};

struct nerve_sq {
	struct nerve_wq wq;
	uint8_t *desc;
	uint32_t desc_offset;
	size_t desc_ring_mmap_size;
	size_t max_inline_data;
	size_t max_wr_rdma_sge;
	uint16_t max_batch_wr;

	/* Buffer for pending WR entries in the current session */
	uint8_t *local_queue;
	/* Number of WR entries posted in the current session */
	uint32_t num_wqe_pending;
	/* Phase before current session */
	int phase_rb;
	/* Current wqe being built */
	struct nerve_io_tx_wqe *curr_tx_wqe;
};

struct nerve_qp {
	struct verbs_qp verbs_qp;
	struct nerve_sq sq;
	struct nerve_rq rq;
	int page_size;
	int sq_sig_all;
	int wr_session_err;
	struct ibv_device *dev;
	struct nerve_parent_domain *parent_domain;
};

struct nerve_mr {
	struct verbs_mr vmr;
};

struct nerve_ah {
	struct ibv_ah ibvah;
	uint16_t nerve_ah;
};

struct nerve_dev {
	struct verbs_device vdev;
	uint32_t pg_sz;
};

static inline struct nerve_dev *to_nerve_dev(struct ibv_device *ibvdev)
{
	return container_of(ibvdev, struct nerve_dev, vdev.device);
}

static inline struct nerve_context *to_nerve_context(struct ibv_context *ibvctx)
{
	return container_of(ibvctx, struct nerve_context, ibvctx.context);
}

static inline struct nerve_pd *to_nerve_pd(struct ibv_pd *ibvpd)
{
	return container_of(ibvpd, struct nerve_pd, ibvpd);
}

static inline struct nerve_cq *to_nerve_cq(struct ibv_cq *ibvcq)
{
	return container_of(ibvcq, struct nerve_cq, verbs_cq.cq);
}

static inline struct nerve_cq *to_nerve_cq_ex(struct ibv_cq_ex *ibvcqx)
{
	return container_of(ibvcqx, struct nerve_cq, verbs_cq.cq_ex);
}

static inline struct nerve_cq *nervedv_cq_to_nerve_cq(struct nervedv_cq *nervedv_cq)
{
	return container_of(nervedv_cq, struct nerve_cq, dv_cq);
}

static inline struct nerve_qp *to_nerve_qp(struct ibv_qp *ibvqp)
{
	return container_of(ibvqp, struct nerve_qp, verbs_qp.qp);
}

static inline struct nerve_qp *to_nerve_qp_ex(struct ibv_qp_ex *ibvqpx)
{
	return container_of(ibvqpx, struct nerve_qp, verbs_qp.qp_ex);
}

static inline struct nerve_ah *to_nerve_ah(struct ibv_ah *ibvah)
{
	return container_of(ibvah, struct nerve_ah, ibvah);
}

static inline struct nerve_td *to_nerve_td(struct ibv_td *ibvtd)
{
	return container_of(ibvtd, struct nerve_td, ibvtd);
}

static inline struct nerve_parent_domain *to_nerve_parent_domain(struct ibv_pd *ibvpd)
{
	return container_of(ibvpd, struct nerve_parent_domain, pd.ibvpd);
}

bool is_nerve_dev(struct ibv_device *device);

#endif /* __NERVE_H__ */
