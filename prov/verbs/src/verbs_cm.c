/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include "fi_verbs.h"


static int fi_ibv_copy_addr(void *dst_addr, size_t *dst_addrlen, void *src_addr)
{
	size_t src_addrlen = fi_ibv_sockaddr_len(src_addr);

	if (*dst_addrlen == 0) {
		*dst_addrlen = src_addrlen;
		return -FI_ETOOSMALL;
	}

	if (*dst_addrlen < src_addrlen) {
		memcpy(dst_addr, src_addr, *dst_addrlen);
	} else {
		memcpy(dst_addr, src_addr, src_addrlen);
	}
	*dst_addrlen = src_addrlen;
	return 0;
}

static int fi_ibv_msg_ep_setname(fid_t ep_fid, void *addr, size_t addrlen)
{
	void *save_addr;
	struct rdma_cm_id *id;
	int ret;
	struct fi_ibv_ep *ep =
		container_of(ep_fid, struct fi_ibv_ep, util_ep.ep_fid);

	if (addrlen != ep->info->src_addrlen) {
		VERBS_INFO(FI_LOG_EP_CTRL,"addrlen expected: %zu, got: %zu.\n",
			   ep->info->src_addrlen, addrlen);
		return -FI_EINVAL;
	}

	save_addr = ep->info->src_addr;

	ep->info->src_addr = malloc(ep->info->src_addrlen);
	if (!ep->info->src_addr) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	memcpy(ep->info->src_addr, addr, ep->info->src_addrlen);

	ret = fi_ibv_create_ep(NULL, NULL, 0, ep->info, NULL, &id);
	if (ret)
		goto err2;

	if (ep->id)
		rdma_destroy_ep(ep->id);

	ep->id = id;
	ep->ibv_qp = ep->id->qp;
	free(save_addr);

	return 0;
err2:
	free(ep->info->src_addr);
err1:
	ep->info->src_addr = save_addr;
	return ret;
}

static int fi_ibv_msg_ep_getname(fid_t ep, void *addr, size_t *addrlen)
{
	struct sockaddr *sa;
	struct fi_ibv_ep *_ep =
		container_of(ep, struct fi_ibv_ep, util_ep.ep_fid);
	sa = rdma_get_local_addr(_ep->id);
	return fi_ibv_copy_addr(addr, addrlen, sa);
}

static int fi_ibv_msg_ep_getpeer(struct fid_ep *ep, void *addr, size_t *addrlen)
{
	struct sockaddr *sa;
	struct fi_ibv_ep *_ep =
		container_of(ep, struct fi_ibv_ep, util_ep.ep_fid);
	sa = rdma_get_peer_addr(_ep->id);
	return fi_ibv_copy_addr(addr, addrlen, sa);
}

static inline void
fi_ibv_msg_ep_prepare_cm_data(const void *param, size_t param_size,
			      struct fi_ibv_cm_data_hdr *cm_hdr)
{
	cm_hdr->size = (uint8_t)param_size;
	memcpy(cm_hdr->data, param, cm_hdr->size);
}

static inline void
fi_ibv_ep_prepare_rdma_cm_param(struct rdma_conn_param *conn_param,
				struct fi_ibv_cm_data_hdr *cm_hdr,
				size_t cm_hdr_data_size)
{
	conn_param->private_data = cm_hdr;
	conn_param->private_data_len = (uint8_t)cm_hdr_data_size;
	conn_param->responder_resources = RDMA_MAX_RESP_RES;
	conn_param->initiator_depth = RDMA_MAX_INIT_DEPTH;
	conn_param->flow_control = 1;
	conn_param->rnr_retry_count = 7;
}

static int
fi_ibv_msg_ep_connect(struct fid_ep *ep, const void *addr,
		      const void *param, size_t paramlen)
{
	struct rdma_conn_param conn_param = { 0 };
	struct sockaddr *src_addr, *dst_addr;
	int ret;
	struct fi_ibv_cm_data_hdr *cm_hdr;
	struct fi_ibv_ep *_ep =
		container_of(ep, struct fi_ibv_ep, util_ep.ep_fid);

	if (OFI_UNLIKELY(paramlen > VERBS_CM_DATA_SIZE))
		return -FI_EINVAL;

	if (!_ep->id->qp) {
		ret = fi_control(&ep->fid, FI_ENABLE, NULL);
		if (ret)
			return ret;
	}

	cm_hdr = alloca(sizeof(*cm_hdr) + paramlen);
	fi_ibv_msg_ep_prepare_cm_data(param, paramlen, cm_hdr);
	fi_ibv_ep_prepare_rdma_cm_param(&conn_param, cm_hdr,
					sizeof(*cm_hdr) + paramlen);
	conn_param.retry_count = 15;

	if (_ep->srq_ep)
		conn_param.srq = 1;

	src_addr = rdma_get_local_addr(_ep->id);
	if (src_addr) {
		VERBS_INFO(FI_LOG_CORE, "src_addr: %s:%d\n",
			   inet_ntoa(((struct sockaddr_in *)src_addr)->sin_addr),
			   ntohs(((struct sockaddr_in *)src_addr)->sin_port));
	}

	dst_addr = rdma_get_peer_addr(_ep->id);
	if (dst_addr) {
		VERBS_INFO(FI_LOG_CORE, "dst_addr: %s:%d\n",
			   inet_ntoa(((struct sockaddr_in *)dst_addr)->sin_addr),
			   ntohs(((struct sockaddr_in *)dst_addr)->sin_port));
	}

	return rdma_connect(_ep->id, &conn_param) ? -errno : 0;
}

static int
fi_ibv_msg_ep_accept(struct fid_ep *ep, const void *param, size_t paramlen)
{
	struct rdma_conn_param conn_param;
	struct fi_ibv_connreq *connreq;
	int ret;
	struct fi_ibv_cm_data_hdr *cm_hdr;
	struct fi_ibv_ep *_ep =
		container_of(ep, struct fi_ibv_ep, util_ep.ep_fid);

	if (OFI_UNLIKELY(paramlen > VERBS_CM_DATA_SIZE))
		return -FI_EINVAL;

	if (!_ep->id->qp) {
		ret = fi_control(&ep->fid, FI_ENABLE, NULL);
		if (ret)
			return ret;
	}

	cm_hdr = alloca(sizeof(*cm_hdr) + paramlen);
	fi_ibv_msg_ep_prepare_cm_data(param, paramlen, cm_hdr);
	fi_ibv_ep_prepare_rdma_cm_param(&conn_param, cm_hdr,
					sizeof(*cm_hdr) + paramlen);

	if (_ep->srq_ep)
		conn_param.srq = 1;

	ret = rdma_accept(_ep->id, &conn_param);
	if (ret)
		return -errno;

	connreq = container_of(_ep->info->handle, struct fi_ibv_connreq, handle);
	free(connreq);

	return 0;
}

static int fi_ibv_msg_alloc_xrc_params(void **adjusted_param,
				       const void *param, size_t *paramlen)
{
	struct fi_ibv_xrc_cm_data *cm_data;
	size_t cm_datalen = sizeof(*cm_data) + *paramlen;

	*adjusted_param = NULL;

	if (cm_datalen > FI_IBV_CM_DATA_SIZE) {
		VERBS_WARN(FI_LOG_EP_CTRL, "XRC CM data overflow %zu\n",
			   cm_datalen);
		return -FI_EINVAL;
	}

	cm_data = malloc(cm_datalen);
	if (!cm_data) {
		VERBS_WARN(FI_LOG_EP_CTRL, "Unable to allocate XRC CM data\n");
		return -FI_ENOMEM;
	}

	if (*paramlen)
		memcpy((cm_data + 1), param, *paramlen);

	*paramlen = cm_datalen;
	*adjusted_param = cm_data;
	return FI_SUCCESS;
}

static int
fi_ibv_msg_xrc_ep_reject(struct fi_ibv_connreq *connreq,
			 const void *param, size_t paramlen)
{
	struct fi_ibv_xrc_cm_data *cm_data;
	int ret;

	ret = fi_ibv_msg_alloc_xrc_params((void **)&cm_data, param, &paramlen);
	if (ret)
		return ret;

	fi_ibv_set_xrc_cm_data(cm_data, connreq->xrc.is_reciprocal,
			       connreq->xrc.conn_tag, connreq->xrc.port, 0);
	ret = rdma_reject(connreq->id, cm_data,
			  (uint8_t) paramlen) ? -errno : 0;
	free(cm_data);
	return ret;
}

static int
fi_ibv_msg_ep_reject(struct fid_pep *pep, fid_t handle,
		     const void *param, size_t paramlen)
{
	struct fi_ibv_connreq *connreq =
		container_of(handle, struct fi_ibv_connreq, handle);
	struct fi_ibv_cm_data_hdr *cm_hdr;
	int ret;

	if (OFI_UNLIKELY(paramlen > VERBS_CM_DATA_SIZE))
		return -FI_EINVAL;

	cm_hdr = alloca(sizeof(*cm_hdr) + paramlen);
	fi_ibv_msg_ep_prepare_cm_data(param, paramlen, cm_hdr);

	if (connreq->is_xrc)
		ret = fi_ibv_msg_xrc_ep_reject(connreq, cm_hdr,
				(uint8_t)(sizeof(*cm_hdr) + paramlen));

	else
		ret = rdma_reject(connreq->id, cm_hdr,
			(uint8_t)(sizeof(*cm_hdr) + paramlen)) ? -errno : 0;
	free(connreq);
	return ret;
}

static int fi_ibv_msg_ep_shutdown(struct fid_ep *ep, uint64_t flags)
{
	struct fi_ibv_ep *_ep =
		container_of(ep, struct fi_ibv_ep, util_ep.ep_fid);
	if (_ep->id)
		return rdma_disconnect(_ep->id) ? -errno : 0;
	return 0;
}

struct fi_ops_cm fi_ibv_msg_ep_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = fi_ibv_msg_ep_setname,
	.getname = fi_ibv_msg_ep_getname,
	.getpeer = fi_ibv_msg_ep_getpeer,
	.connect = fi_ibv_msg_ep_connect,
	.listen = fi_no_listen,
	.accept = fi_ibv_msg_ep_accept,
	.reject = fi_no_reject,
	.shutdown = fi_ibv_msg_ep_shutdown,
	.join = fi_no_join,
};

static int
fi_ibv_msg_xrc_cm_common_verify(struct fi_ibv_xrc_ep *ep, size_t paramlen)
{
	int ret;

	if (!fi_ibv_is_xrc(ep->base_ep.info)) {
		VERBS_WARN(FI_LOG_EP_CTRL, "EP is not using XRC\n");
		return -FI_EINVAL;
	}

	if (!ep->srqn) {
		ret = fi_control(&ep->base_ep.util_ep.ep_fid.fid,
				 FI_ENABLE, NULL);
		if (ret)
			return ret;
	}

	if (OFI_UNLIKELY(paramlen > VERBS_CM_DATA_SIZE -
			 sizeof(struct fi_ibv_xrc_cm_data)))
		return -FI_EINVAL;

	return FI_SUCCESS;
}

static int
fi_ibv_msg_xrc_ep_connect(struct fid_ep *ep, const void *addr,
		   const void *param, size_t paramlen)
{
	struct sockaddr *dst_addr;
	void *adjusted_param;
	struct fi_ibv_ep *_ep = container_of(ep, struct fi_ibv_ep,
					     util_ep.ep_fid);
	struct fi_ibv_xrc_ep *xrc_ep = container_of(_ep, struct fi_ibv_xrc_ep,
						    base_ep);
	int ret;
	struct fi_ibv_cm_data_hdr *cm_hdr;

	ret = fi_ibv_msg_xrc_cm_common_verify(xrc_ep, paramlen);
	if (ret)
		return ret;

	cm_hdr = alloca(sizeof(*cm_hdr) + paramlen);
	fi_ibv_msg_ep_prepare_cm_data(param, paramlen, cm_hdr);
	paramlen += sizeof(*cm_hdr);

	ret = fi_ibv_msg_alloc_xrc_params(&adjusted_param, cm_hdr, &paramlen);
	if (ret)
		return ret;

	xrc_ep->conn_setup = calloc(1, sizeof(*xrc_ep->conn_setup));
	if (!xrc_ep->conn_setup) {
		free(adjusted_param);
		return -FI_ENOMEM;
	}

	fastlock_acquire(&xrc_ep->base_ep.eq->lock);
	xrc_ep->conn_setup->conn_tag = VERBS_CONN_TAG_INVALID;
	fi_ibv_eq_set_xrc_conn_tag(xrc_ep);
	fastlock_release(&xrc_ep->base_ep.eq->lock);

	dst_addr = rdma_get_peer_addr(_ep->id);
	ret = fi_ibv_connect_xrc(xrc_ep, dst_addr, 0, adjusted_param, paramlen);
	free(adjusted_param);
	return ret;
}

static int
fi_ibv_msg_xrc_ep_accept(struct fid_ep *ep, const void *param, size_t paramlen)
{
	void *adjusted_param;
	struct fi_ibv_ep *_ep =
		container_of(ep, struct fi_ibv_ep, util_ep.ep_fid);
	struct fi_ibv_xrc_ep *xrc_ep = container_of(_ep, struct fi_ibv_xrc_ep,
						    base_ep);
	int ret;
	struct fi_ibv_cm_data_hdr *cm_hdr;

	ret = fi_ibv_msg_xrc_cm_common_verify(xrc_ep, paramlen);
	if (ret)
		return ret;

	cm_hdr = alloca(sizeof(*cm_hdr) + paramlen);
	fi_ibv_msg_ep_prepare_cm_data(param, paramlen, cm_hdr);
	paramlen += sizeof(*cm_hdr);

	ret = fi_ibv_msg_alloc_xrc_params(&adjusted_param, cm_hdr, &paramlen);
	if (ret)
		return ret;

	ret = fi_ibv_accept_xrc(xrc_ep, 0, adjusted_param, paramlen);
	free(adjusted_param);
	return ret;
}

struct fi_ops_cm fi_ibv_msg_xrc_ep_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = fi_ibv_msg_ep_setname,
	.getname = fi_ibv_msg_ep_getname,
	.getpeer = fi_ibv_msg_ep_getpeer,
	.connect = fi_ibv_msg_xrc_ep_connect,
	.listen = fi_no_listen,
	.accept = fi_ibv_msg_xrc_ep_accept,
	.reject = fi_no_reject,
	.shutdown = fi_ibv_msg_ep_shutdown,
	.join = fi_no_join,
};

static int fi_ibv_pep_setname(fid_t pep_fid, void *addr, size_t addrlen)
{
	struct fi_ibv_pep *pep;
	int ret;

	pep = container_of(pep_fid, struct fi_ibv_pep, pep_fid);

	if (pep->src_addrlen && (addrlen != pep->src_addrlen)) {
		VERBS_INFO(FI_LOG_FABRIC, "addrlen expected: %zu, got: %zu.\n",
			   pep->src_addrlen, addrlen);
		return -FI_EINVAL;
	}

	/* Re-create id if already bound */
	if (pep->bound) {
		ret = rdma_destroy_id(pep->id);
		if (ret) {
			VERBS_INFO(FI_LOG_FABRIC,
				   "Unable to destroy previous rdma_cm_id\n");
			return -errno;
		}
		ret = rdma_create_id(NULL, &pep->id, &pep->pep_fid.fid, RDMA_PS_TCP);
		if (ret) {
			VERBS_INFO(FI_LOG_FABRIC,
				   "Unable to create rdma_cm_id\n");
			return -errno;
		}
	}

	ret = rdma_bind_addr(pep->id, (struct sockaddr *)addr);
	if (ret) {
		VERBS_INFO(FI_LOG_FABRIC,
			   "Unable to bind address to rdma_cm_id\n");
		return -errno;
	}

	return 0;
}

static int fi_ibv_pep_getname(fid_t pep, void *addr, size_t *addrlen)
{
	struct fi_ibv_pep *_pep;
	struct sockaddr *sa;

	_pep = container_of(pep, struct fi_ibv_pep, pep_fid);
	sa = rdma_get_local_addr(_pep->id);
	return fi_ibv_copy_addr(addr, addrlen, sa);
}

static int fi_ibv_pep_listen(struct fid_pep *pep_fid)
{
	struct fi_ibv_pep *pep;
	struct sockaddr *addr;

	pep = container_of(pep_fid, struct fi_ibv_pep, pep_fid);

	addr = rdma_get_local_addr(pep->id);
	if (addr) {
		VERBS_INFO(FI_LOG_CORE, "Listening on %s:%d\n",
			   inet_ntoa(((struct sockaddr_in *)addr)->sin_addr),
			   ntohs(((struct sockaddr_in *)addr)->sin_port));
	}

	return rdma_listen(pep->id, pep->backlog) ? -errno : 0;
}

static struct fi_ops_cm fi_ibv_pep_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = fi_ibv_pep_setname,
	.getname = fi_ibv_pep_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_ibv_pep_listen,
	.accept = fi_no_accept,
	.reject = fi_ibv_msg_ep_reject,
	.shutdown = fi_no_shutdown,
	.join = fi_no_join,
};

struct fi_ops_cm *fi_ibv_pep_ops_cm(struct fi_ibv_pep *pep)
{
	return &fi_ibv_pep_cm_ops;
}
