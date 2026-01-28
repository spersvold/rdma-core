/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2019-2025 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#ifndef __NERVE_VERBS_H__
#define __NERVE_VERBS_H__

#include <infiniband/driver.h>
#include <infiniband/verbs.h>

int nerve_query_device_ctx(struct nerve_context *ctx);
int nerve_query_port(struct ibv_context *uctx, uint8_t port,
		   struct ibv_port_attr *attr);
int nerve_query_device_ex(struct ibv_context *context,
			const struct ibv_query_device_ex_input *input,
			struct ibv_device_attr_ex *attr, size_t attr_size);
struct ibv_pd *nerve_alloc_pd(struct ibv_context *uctx);
struct ibv_pd *nerve_alloc_parent_domain(struct ibv_context *ibvctx,
				       struct ibv_parent_domain_init_attr *attr);
int nerve_dealloc_pd(struct ibv_pd *ibvpd);
struct ibv_mr *nerve_reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset,
				 size_t length, uint64_t iova, int fd, int acc);
struct ibv_mr *nerve_reg_mr(struct ibv_pd *ibvpd, void *buf, size_t len,
			  uint64_t hca_va, int ibv_access_flags);
int nerve_dereg_mr(struct verbs_mr *vmr);

struct ibv_cq *nerve_create_cq(struct ibv_context *uctx, int ncqe,
			     struct ibv_comp_channel *ch, int vec);
struct ibv_cq_ex *nerve_create_cq_ex(struct ibv_context *uctx,
				   struct ibv_cq_init_attr_ex *attr_ex);
int nerve_destroy_cq(struct ibv_cq *ibvcq);
int nerve_poll_cq(struct ibv_cq *ibvcq, int nwc, struct ibv_wc *wc);
int nerve_arm_cq(struct ibv_cq *ibvcq, int solicited_only);
void nerve_cq_event(struct ibv_cq *ibvcq);

struct ibv_qp *nerve_create_qp(struct ibv_pd *ibvpd,
			     struct ibv_qp_init_attr *attr);
struct ibv_qp *nerve_create_qp_ex(struct ibv_context *ibvctx,
				struct ibv_qp_init_attr_ex *attr_ex);
int nerve_modify_qp(struct ibv_qp *ibvqp, struct ibv_qp_attr *attr,
		  int ibv_qp_attr_mask);
int nerve_query_qp(struct ibv_qp *ibvqp, struct ibv_qp_attr *attr, int attr_mask,
		 struct ibv_qp_init_attr *init_attr);
int nerve_query_qp_data_in_order(struct ibv_qp *ibvqp, enum ibv_wr_opcode op,
			       uint32_t flags);
int nerve_destroy_qp(struct ibv_qp *ibvqp);
int nerve_post_send(struct ibv_qp *ibvqp, struct ibv_send_wr *wr,
		  struct ibv_send_wr **bad);
int nerve_post_recv(struct ibv_qp *ibvqp, struct ibv_recv_wr *wr,
		  struct ibv_recv_wr **bad);

struct ibv_ah *nerve_create_ah(struct ibv_pd *ibvpd, struct ibv_ah_attr *attr);
int nerve_destroy_ah(struct ibv_ah *ibvah);
struct ibv_td *nerve_alloc_td(struct ibv_context *ibvctx, struct ibv_td_init_attr *init_attr);
int nerve_dealloc_td(struct ibv_td *ibvtd);

#endif /* __NERVE_VERBS_H__ */
