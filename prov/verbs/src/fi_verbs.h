/*
 * Copyright (c) 2013-2018 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc. All rights reserved.
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

#ifndef FI_VERBS_H
#define FI_VERBS_H

#include "config.h"

#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sys/epoll.h>

#include <infiniband/ib.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_errno.h>

#include "ofi.h"
#include "ofi_atomic.h"
#include "ofi_enosys.h"
#include <uthash.h>
#include "ofi_prov.h"
#include "ofi_list.h"
#include "ofi_signal.h"
#include "ofi_util.h"
#include "ofi_tree.h"
#include "ofi_indexer.h"

#ifdef HAVE_VERBS_EXP_H
#include <infiniband/verbs_exp.h>
#endif /* HAVE_VERBS_EXP_H */

#include "ofi_verbs_priv.h"

#ifndef AF_IB
#define AF_IB 27
#endif

#ifndef RAI_FAMILY
#define RAI_FAMILY              0x00000008
#endif

#define VERBS_PROV_NAME "verbs"
#define VERBS_PROV_VERS FI_VERSION(1,0)

#define VERBS_DBG(subsys, ...) FI_DBG(&fi_ibv_prov, subsys, __VA_ARGS__)
#define VERBS_INFO(subsys, ...) FI_INFO(&fi_ibv_prov, subsys, __VA_ARGS__)
#define VERBS_INFO_ERRNO(subsys, fn, errno) VERBS_INFO(subsys, fn ": %s(%d)\n",	\
		strerror(errno), errno)
#define VERBS_WARN(subsys, ...) FI_WARN(&fi_ibv_prov, subsys, __VA_ARGS__)


#define VERBS_INJECT_FLAGS(ep, len, flags) ((((flags) & FI_INJECT) || \
		len <= (ep)->inject_limit) ? IBV_SEND_INLINE : 0)
#define VERBS_INJECT(ep, len) VERBS_INJECT_FLAGS(ep, len, (ep)->info->tx_attr->op_flags)

#define VERBS_COMP_FLAGS(ep, flags, context)		\
	(((ep)->util_ep.tx_op_flags | (flags)) &		\
	 FI_COMPLETION ? context : VERBS_NO_COMP_FLAG)
#define VERBS_COMP(ep, context)						\
	VERBS_COMP_FLAGS((ep), (ep)->info->tx_attr->op_flags, context)

#define VERBS_WCE_CNT 1024
#define VERBS_WRE_CNT 1024

#define VERBS_DEF_CQ_SIZE 1024
#define VERBS_MR_IOV_LIMIT 1

#define VERBS_NO_COMP_FLAG	((uint64_t)-1)

#define FI_IBV_CM_DATA_SIZE	(56)
#define VERBS_CM_DATA_SIZE	(FI_IBV_CM_DATA_SIZE -		\
				 sizeof(struct fi_ibv_cm_data_hdr))

#define FI_IBV_CM_REJ_CONSUMER_DEFINED	28

#define VERBS_DGRAM_MSG_PREFIX_SIZE	(40)

#define FI_IBV_EP_TYPE(info)						\
	((info && info->ep_attr) ? info->ep_attr->type : FI_EP_MSG)
#define FI_IBV_EP_PROTO(info)						\
	(((info) && (info)->ep_attr) ? (info)->ep_attr->protocol :	\
					FI_PROTO_UNSPEC)

#define FI_IBV_MEM_ALIGNMENT (64)
#define FI_IBV_BUF_ALIGNMENT (4096) /* TODO: Page or MTU size */
#define FI_IBV_POOL_BUF_CNT (100)

#define VERBS_ANY_DOMAIN "verbs_any_domain"
#define VERBS_ANY_FABRIC "verbs_any_fabric"

extern struct fi_provider fi_ibv_prov;
extern struct util_prov fi_ibv_util_prov;

extern struct fi_ibv_gl_data {
	int	def_tx_size;
	int	def_rx_size;
	int	def_tx_iov_limit;
	int	def_rx_iov_limit;
	int	def_inline_size;
	int	min_rnr_timer;
	int	use_odp;
	int	cqread_bunch_size;
	char	*iface;
	int	gid_idx;

	struct {
		int	buffer_num;
		int	buffer_size;
		int	rndv_seg_size;
		int	thread_timeout;
		char	*eager_send_opcode;
		char	*cm_thread_affinity;
	} rdm;

	struct {
		int	use_name_server;
		int	name_server_port;
	} dgram;

	struct {
		int	prefer_xrc;
		char	*xrcd_filename;
	} msg;
} fi_ibv_gl_data;

struct verbs_addr {
	struct dlist_entry entry;
	struct rdma_addrinfo *rai;
};

/*
 * fields of Infiniband packet headers that are used to
 * represent OFI EP address
 * - LRH (Local Route Header) - Link Layer:
 *   - LID - destination Local Identifier
 *   - SL - Service Level
 * - GRH (Global Route Header) - Network Layer:
 *   - GID - destination Global Identifier
 * - BTH (Base Transport Header) - Transport Layer:
 *   - QPN - destination Queue Pair number
 *   - P_key - Partition Key
 *
 * Note: DON'T change the placement of the fields in the structure.
 *       The placement is to keep structure size = 256 bits (32 byte).
 */
struct ofi_ib_ud_ep_name {
	union ibv_gid	gid;		/* 64-bit GUID + 64-bit EUI - GRH */

	uint32_t	qpn;		/* BTH */

	uint16_t	lid; 		/* LRH */
	uint16_t	pkey;		/* BTH */
	uint16_t	service;	/* for NS src addr, 0 means any */

	uint8_t 	sl;		/* LRH */
	uint8_t		padding[5];	/* forced padding to 256 bits (32 byte) */
}; /* 256 bits */

#define VERBS_IB_UD_NS_ANY_SERVICE	0

static inline
int fi_ibv_dgram_ns_is_service_wildcard(void *svc)
{
	return (*(int *)svc == VERBS_IB_UD_NS_ANY_SERVICE);
}

static inline
int fi_ibv_dgram_ns_service_cmp(void *svc1, void *svc2)
{
	int service1 = *(int *)svc1, service2 = *(int *)svc2;

	if (fi_ibv_dgram_ns_is_service_wildcard(svc1) ||
	    fi_ibv_dgram_ns_is_service_wildcard(svc2))
		return 0;
	return (service1 < service2) ? -1 : (service1 > service2);
}

struct verbs_dev_info {
	struct dlist_entry entry;
	char *name;
	struct dlist_entry addrs;
};


struct fi_ibv_fabric {
	struct util_fabric	util_fabric;
	const struct fi_info	*info;
	struct util_ns		name_server;
};

int fi_ibv_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
		  void *context);
int fi_ibv_find_fabric(const struct fi_fabric_attr *attr);

struct fi_ibv_eq_entry {
	struct dlist_entry	item;
	uint32_t		event;
	size_t			len;
	char 			eq_entry[0];
};

typedef int (*fi_ibv_trywait_func)(struct fid *fid);

/* An OFI indexer is used to maintain a unique connection request to
 * endpoint mapping. The key is a 32-bit value (referred to as a
 * connection tag) and is passed to the remote peer by the active side
 * of a connection request. When the reciprocal XRC connection in the
 * reverse direction is made, the key is passed back and used to map
 * back to the original endpoint. A key is defined as a 32-bit value:
 *
 *     SSSSSSSS:SSSSSSII:IIIIIIII:IIIIIIII
 *     |-- sequence -||--- unique key ---|
 */
#define VERBS_CONN_TAG_INDEX_BITS	18
#define VERBS_CONN_TAG_INVALID		0xFFFFFFFF	/* Key is not valid */

struct fi_ibv_eq {
	struct fid_eq		eq_fid;
	struct fi_ibv_fabric	*fab;
	fastlock_t		lock;
	struct dlistfd_head	list_head;
	struct rdma_event_channel *channel;
	uint64_t		flags;
	struct fi_eq_err_entry	err;
	int			epfd;

	struct {
		/* The connection key map is used during the XRC connection
		 * process to map an XRC reciprocal connection request back
		 * to the active endpoint that initiated the original
		 * connection request. It is protected with the eq::lock */
		struct ofi_key_idx	conn_key_idx;
		struct indexer		*conn_key_map;

		/* TODO: This is limiting and restricts applications to using
		 * a single listener per EQ. While sufficient for RXM we should
		 * consider using an internal PEP listener for handling the
		 * internally processed reciprocal connections. */
		uint16_t		pep_port;
	} xrc;
};

int fi_ibv_eq_open(struct fid_fabric *fabric, struct fi_eq_attr *attr,
		   struct fid_eq **eq, void *context);
int fi_ibv_eq_trywait(struct fi_ibv_eq *eq);

int fi_ibv_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
		   struct fid_av **av, void *context);

struct fi_ibv_pep {
	struct fid_pep		pep_fid;
	struct fi_ibv_eq	*eq;
	struct rdma_cm_id	*id;
	int			backlog;
	int			bound;
	size_t			src_addrlen;
	struct fi_info		*info;
};

struct fi_ops_cm *fi_ibv_pep_ops_cm(struct fi_ibv_pep *pep);

struct fi_ibv_mem_desc;
struct fi_ibv_domain;
typedef int(*fi_ibv_mr_reg_cb)(struct fi_ibv_domain *domain, void *buf,
			       size_t len, uint64_t access,
			       struct fi_ibv_mem_desc *md);
typedef int(*fi_ibv_mr_dereg_cb)(struct fi_ibv_mem_desc *md);

struct fi_ibv_domain {
	struct util_domain		util_domain;
	struct ibv_context		*verbs;
	struct ibv_pd			*pd;

	enum fi_ep_type			ep_type;
	struct fi_info			*info;
	/* The EQ is utilized by verbs/MSG */
	struct fi_ibv_eq		*eq;
	uint64_t			eq_flags;

	/* Indicates that MSG endpoints should use the XRC transport.
	 * TODO: Move selection of XRC/RC to endpoint info from domain */
	int				use_xrc;
	struct {
		int			xrcd_fd;
		struct ibv_xrcd		*xrcd;

		/* The domain maintains a RBTree for mapping an endpoint
		 * destination addresses to physical XRC INI QP connected
		 * to that host. */
		fastlock_t		ini_mgmt_lock;
		struct ofi_rbmap	*ini_conn_rbmap;
	} xrc ;

	/* MR stuff */
	int				use_odp;
	struct ofi_mr_cache		cache;
	fi_ibv_mr_reg_cb		internal_mr_reg;
	fi_ibv_mr_dereg_cb		internal_mr_dereg;
	int 				(*post_send)(struct ibv_qp *qp,
						     struct ibv_send_wr *wr,
						     struct ibv_send_wr **bad_wr);
	int				(*poll_cq)(struct ibv_cq *cq,
						   int num_entries,
						   struct ibv_wc *wc);
};

struct fi_ibv_cq;
typedef void (*fi_ibv_cq_read_entry)(struct ibv_wc *wc, void *buf);

struct fi_ibv_wce {
	struct slist_entry	entry;
	struct ibv_wc		wc;
};

struct fi_ibv_srq_ep;
struct fi_ibv_cq {
	struct util_cq		util_cq;
	struct ibv_comp_channel	*channel;
	struct ibv_cq		*cq;
	size_t			entry_size;
	uint64_t		flags;
	enum fi_cq_wait_cond	wait_cond;
	struct ibv_wc		wc;
	int			signal_fd[2];
	fi_ibv_cq_read_entry	read_entry;
	struct slist		wcq;
	ofi_atomic32_t		nevents;
	struct ofi_bufpool	*wce_pool;

	struct {
		/* The list of XRC SRQ contexts associated with this CQ */
		fastlock_t		srq_list_lock;
		struct dlist_entry	srq_list;
	} xrc;
	/* Track tx credits for verbs devices that can free-up send queue
	 * space after processing WRs even if the app hasn't read the CQ.
	 * Without this tracking we might overrun the CQ */
	ofi_atomic32_t		credits;
};

int fi_ibv_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		   struct fid_cq **cq, void *context);
int fi_ibv_cq_trywait(struct fi_ibv_cq *cq);

struct fi_ibv_mem_desc {
	struct fid_mr		mr_fid;
	struct ibv_mr		*mr;
	struct fi_ibv_domain	*domain;
	size_t			len;
	/* this field is used only by MR cache operations */
	struct ofi_mr_entry	*entry;
};

static inline uint64_t
fi_ibv_mr_internal_rkey(struct fi_ibv_mem_desc *md)
{
	return md->mr->rkey;
}

static inline uint64_t
fi_ibv_mr_internal_lkey(struct fi_ibv_mem_desc *md)
{
	return md->mr->lkey;
}

struct fi_ibv_mr_internal_ops {
	struct fi_ops_mr	*fi_ops;
	fi_ibv_mr_reg_cb	internal_mr_reg;
	fi_ibv_mr_dereg_cb	internal_mr_dereg;
};


extern struct fi_ibv_mr_internal_ops fi_ibv_mr_internal_ops;
extern struct fi_ibv_mr_internal_ops fi_ibv_mr_internal_cache_ops;
extern struct fi_ibv_mr_internal_ops fi_ibv_mr_internal_ex_ops;

int fi_ibv_mr_cache_entry_reg(struct ofi_mr_cache *cache,
			      struct ofi_mr_entry *entry);
void fi_ibv_mr_cache_entry_dereg(struct ofi_mr_cache *cache,
				 struct ofi_mr_entry *entry);

/*
 * An XRC SRQ cannot be created until the associated RX CQ is known,
 * maintain a list of validated pre-posted receives to post once
 * the SRQ is created.
 */
struct fi_ibv_xrc_srx_prepost {
	struct slist_entry	prepost_entry;
	void			*buf;
	void			*desc;
	void			*context;
	size_t			len;
	fi_addr_t		src_addr;
};

struct fi_ibv_srq_ep {
	struct fid_ep		ep_fid;
	struct ibv_srq		*srq;
	struct fi_ibv_domain	*domain;

	/* For XRC SRQ only */
	struct {
		/* XRC SRQ is not created until endpoint enable */
		fastlock_t		prepost_lock;
		struct slist		prepost_list;
		uint32_t		max_recv_wr;
		uint32_t		max_sge;
		uint32_t		prepost_count;

		/* The RX CQ associated with this XRC SRQ. This field
		 * and the srq_entry should only be modified while holding
		 * the associted cq::xrc.srq_list_lock. */
		struct fi_ibv_cq	*cq;

		/* The CQ maintains a list of XRC SRQ associated with it */
		struct dlist_entry	srq_entry;
	} xrc;
};

int fi_ibv_srq_context(struct fid_domain *domain, struct fi_rx_attr *attr,
		       struct fid_ep **rx_ep, void *context);

static inline int fi_ibv_is_xrc(struct fi_info *info)
{
	return  (FI_IBV_EP_TYPE(info) == FI_EP_MSG) &&
		(FI_IBV_EP_PROTO(info) == FI_PROTO_RDMA_CM_IB_XRC);
}

static inline int fi_ibv_is_xrc_send_qp(enum ibv_qp_type qp_type)
{
	return qp_type == IBV_QPT_XRC_SEND;
}

int fi_ibv_domain_xrc_init(struct fi_ibv_domain *domain);
int fi_ibv_domain_xrc_cleanup(struct fi_ibv_domain *domain);

enum fi_ibv_ini_qp_state {
	FI_IBV_INI_QP_UNCONNECTED,
	FI_IBV_INI_QP_CONNECTING,
	FI_IBV_INI_QP_CONNECTED
};

#define FI_IBV_NO_INI_TGT_QPNUM 0
#define FI_IBV_RECIP_CONN	1

/*
 * An XRC transport INI QP connection can be shared within a process to
 * communicate with all the ranks on the same remote node. This structure is
 * only accessed during connection setup and tear down and should be
 * done while holding the domain:xrc:ini_mgmt_lock.
 */
struct fi_ibv_ini_shared_conn {
	/* To share, EP must have same remote peer host addr and TX CQ */
	struct sockaddr			*peer_addr;
	struct fi_ibv_cq		*tx_cq;

	/* The physical INI/TGT QPN connection. Virtual connections to the
	 * same remote peer and TGT QPN will share this connection, with
	 * the remote end opening the specified XRC TGT QPN for sharing. */
	enum fi_ibv_ini_qp_state	state;
	struct ibv_qp			*ini_qp;
	uint32_t			tgt_qpn;

	/* EP waiting on or using this INI/TGT physical connection will be in
	 * one of these list and hold a reference to the shared connection. */
	struct dlist_entry		pending_list;
	struct dlist_entry		active_list;
	ofi_atomic32_t			ref_cnt;
};

enum fi_ibv_xrc_ep_conn_state {
	FI_IBV_XRC_UNCONNECTED,
	FI_IBV_XRC_ORIG_CONNECTING,
	FI_IBV_XRC_ORIG_CONNECTED,
	FI_IBV_XRC_RECIP_CONNECTING,
	FI_IBV_XRC_CONNECTED
};

/*
 * The following XRC state is only required during XRC connection
 * establishment and can be freed once bidirectional connectivity
 * is established.
 */
struct fi_ibv_xrc_ep_conn_setup {
	/* The connection tag is used to associate the reciprocal
	 * XRC INI/TGT QP connection request in the reverse direction
	 * with the original request. The tag is created by the
	 * original active side. */
	uint32_t			conn_tag;
	bool				created_conn_tag;

	/* IB CM message stale/duplicate detection processing requires
	 * that shared INI/TGT connections use unique QP numbers during
	 * RDMA CM connection setup. To avoid conflicts with actual HCA
	 * QP number space, we allocate minimal QP that are left in the
	 * reset state and closed once the setup process completes. */
	struct ibv_qp			*rsvd_ini_qpn;
	struct ibv_qp			*rsvd_tgt_qpn;

	/* Temporary flags to indicate if the INI QP setup and the
	 * TGT QP setup have completed. */
	bool				ini_connected;
	bool				tgt_connected;

	/* Delivery of the FI_CONNECTED event is delayed until
	 * bidirectional connectivity is established. */
	size_t				event_len;
	uint8_t				event_data[FI_IBV_CM_DATA_SIZE];

	/* Connection request may have to queue waiting for the
	 * physical XRC INI/TGT QP connection to complete. */
	int				pending_recip;
	size_t				pending_paramlen;
	uint8_t				pending_param[FI_IBV_CM_DATA_SIZE];
};

struct fi_ibv_ep {
	struct util_ep			util_ep;
	struct ibv_qp			*ibv_qp;
	union {
		struct rdma_cm_id		*id;
		struct {
			struct ofi_ib_ud_ep_name	ep_name;
			int				service;
		};
	};

	size_t				inject_limit;

	struct fi_ibv_eq		*eq;
	struct fi_ibv_srq_ep		*srq_ep;
	struct fi_info			*info;

	struct {
		struct ibv_send_wr	rma_wr;
		struct ibv_send_wr	msg_wr;
		struct ibv_sge		sge;
	} *wrs;
	size_t				rx_size;
};

#define VERBS_XRC_EP_MAGIC		0x1F3D5B79
struct fi_ibv_xrc_ep {
	/* Must be first */
	struct fi_ibv_ep		base_ep;

	/* XRC only fields */
	struct rdma_cm_id		*tgt_id;
	struct ibv_qp			*tgt_ibv_qp;
	enum fi_ibv_xrc_ep_conn_state	conn_state;
	uint32_t			magic;
	uint32_t			srqn;
	uint32_t			peer_srqn;

	/* A reference is held to a shared physical XRC INI/TGT QP connecting
	 * to the destination node. */
	struct fi_ibv_ini_shared_conn	*ini_conn;
	struct dlist_entry		ini_conn_entry;

	/* The following state is allocated during XRC bidirectional setup and
	 * freed once the connection is established. */
	struct fi_ibv_xrc_ep_conn_setup	*conn_setup;
};

int fi_ibv_open_ep(struct fid_domain *domain, struct fi_info *info,
		   struct fid_ep **ep, void *context);
int fi_ibv_passive_ep(struct fid_fabric *fabric, struct fi_info *info,
		      struct fid_pep **pep, void *context);
int fi_ibv_create_ep(const char *node, const char *service,
		     uint64_t flags, const struct fi_info *hints,
		     struct rdma_addrinfo **rai, struct rdma_cm_id **id);
void fi_ibv_destroy_ep(struct rdma_addrinfo *rai, struct rdma_cm_id **id);
int fi_ibv_dgram_av_open(struct fid_domain *domain_fid, struct fi_av_attr *attr,
			 struct fid_av **av_fid, void *context);
static inline
struct fi_ibv_domain *fi_ibv_ep_to_domain(struct fi_ibv_ep *ep)
{
	return container_of(ep->util_ep.domain, struct fi_ibv_domain,
			    util_domain);
}

struct fi_ops_atomic fi_ibv_msg_ep_atomic_ops;
struct fi_ops_atomic fi_ibv_msg_xrc_ep_atomic_ops;
struct fi_ops_cm fi_ibv_msg_ep_cm_ops;
struct fi_ops_cm fi_ibv_msg_xrc_ep_cm_ops;
const struct fi_ops_msg fi_ibv_msg_ep_msg_ops_ts;
const struct fi_ops_msg fi_ibv_msg_ep_msg_ops;
const struct fi_ops_msg fi_ibv_dgram_msg_ops_ts;
const struct fi_ops_msg fi_ibv_dgram_msg_ops;
const struct fi_ops_msg fi_ibv_msg_xrc_ep_msg_ops;
const struct fi_ops_msg fi_ibv_msg_xrc_ep_msg_ops_ts;
const struct fi_ops_msg fi_ibv_msg_srq_xrc_ep_msg_ops;
struct fi_ops_rma fi_ibv_msg_ep_rma_ops_ts;
struct fi_ops_rma fi_ibv_msg_ep_rma_ops;
struct fi_ops_rma fi_ibv_msg_xrc_ep_rma_ops_ts;
struct fi_ops_rma fi_ibv_msg_xrc_ep_rma_ops;

#define FI_IBV_XRC_VERSION	1

struct fi_ibv_xrc_cm_data {
	uint8_t		version;
	uint8_t		reciprocal;
	uint16_t	port;
	uint32_t	param;
	uint32_t	conn_tag;
};

struct fi_ibv_xrc_conn_info {
	uint32_t		conn_tag;
	uint32_t		is_reciprocal;
	uint32_t		ini_qpn;
	uint32_t		conn_data;
	uint16_t		port;
	struct rdma_conn_param	conn_param;
};

struct fi_ibv_connreq {
	struct fid			handle;
	struct rdma_cm_id		*id;

	/* Support for XRC bidirectional connections, and
	 * non-RDMA CM managed QP. */
	int				is_xrc;
	struct fi_ibv_xrc_conn_info	xrc;
};

struct fi_ibv_cm_data_hdr {
	uint8_t	size;
	char	data[];
};

void fi_ibv_msg_ep_get_qp_attr(struct fi_ibv_ep *ep,
			       struct ibv_qp_init_attr *attr);
int fi_ibv_process_xrc_connreq(struct fi_ibv_ep *ep,
			       struct fi_ibv_connreq *connreq);

void fi_ibv_next_xrc_conn_state(struct fi_ibv_xrc_ep *ep);
void fi_ibv_prev_xrc_conn_state(struct fi_ibv_xrc_ep *ep);
void fi_ibv_eq_set_xrc_conn_tag(struct fi_ibv_xrc_ep *ep);
void fi_ibv_eq_clear_xrc_conn_tag(struct fi_ibv_xrc_ep *ep);
struct fi_ibv_xrc_ep *fi_ibv_eq_xrc_conn_tag2ep(struct fi_ibv_eq *eq,
						uint32_t conn_tag);
void fi_ibv_set_xrc_cm_data(struct fi_ibv_xrc_cm_data *local, int reciprocal,
			    uint32_t conn_tag, uint16_t port, uint32_t param);
int fi_ibv_verify_xrc_cm_data(struct fi_ibv_xrc_cm_data *remote,
			      int private_data_len);
int fi_ibv_connect_xrc(struct fi_ibv_xrc_ep *ep, struct sockaddr *addr,
		       int reciprocal, void *param, size_t paramlen);
int fi_ibv_accept_xrc(struct fi_ibv_xrc_ep *ep, int reciprocal,
		      void *param, size_t paramlen);
void fi_ibv_free_xrc_conn_setup(struct fi_ibv_xrc_ep *ep, int disconnect);
void fi_ibv_add_pending_ini_conn(struct fi_ibv_xrc_ep *ep, int reciprocal,
				 void *conn_param, size_t conn_paramlen);
void fi_ibv_sched_ini_conn(struct fi_ibv_ini_shared_conn *ini_conn);
int fi_ibv_get_shared_ini_conn(struct fi_ibv_xrc_ep *ep,
			       struct fi_ibv_ini_shared_conn **ini_conn);
void fi_ibv_put_shared_ini_conn(struct fi_ibv_xrc_ep *ep);
int fi_ibv_reserve_qpn(struct fi_ibv_xrc_ep *ep, struct ibv_qp **qp);

void fi_ibv_save_priv_data(struct fi_ibv_xrc_ep *ep, const void *data,
			   size_t len);
int fi_ibv_ep_create_ini_qp(struct fi_ibv_xrc_ep *ep, void *dst_addr,
			    uint32_t *peer_tgt_qpn);
void fi_ibv_ep_ini_conn_done(struct fi_ibv_xrc_ep *ep, uint32_t peer_srqn,
			    uint32_t peer_tgt_qpn);
void fi_ibv_ep_ini_conn_rejected(struct fi_ibv_xrc_ep *ep);
int fi_ibv_ep_create_tgt_qp(struct fi_ibv_xrc_ep *ep, uint32_t tgt_qpn);
void fi_ibv_ep_tgt_conn_done(struct fi_ibv_xrc_ep *qp);
int fi_ibv_ep_destroy_xrc_qp(struct fi_ibv_xrc_ep *ep);

int fi_ibv_xrc_close_srq(struct fi_ibv_srq_ep *srq_ep);
int fi_ibv_sockaddr_len(struct sockaddr *addr);


int fi_ibv_init_info(const struct fi_info **all_infos);
int fi_ibv_getinfo(uint32_t version, const char *node, const char *service,
		   uint64_t flags, const struct fi_info *hints,
		   struct fi_info **info);
const struct fi_info *fi_ibv_get_verbs_info(const struct fi_info *ilist,
					    const char *domain_name);
int fi_ibv_fi_to_rai(const struct fi_info *fi, uint64_t flags,
		     struct rdma_addrinfo *rai);
int fi_ibv_get_rdma_rai(const char *node, const char *service, uint64_t flags,
			const struct fi_info *hints, struct rdma_addrinfo **rai);
struct verbs_ep_domain {
	char			*suffix;
	enum fi_ep_type		type;
	uint32_t		protocol;
	uint64_t		caps;
};

extern const struct verbs_ep_domain verbs_dgram_domain;
extern const struct verbs_ep_domain verbs_msg_xrc_domain;

int fi_ibv_check_ep_attr(const struct fi_info *hints,
			 const struct fi_info *info);
int fi_ibv_check_rx_attr(const struct fi_rx_attr *attr,
			 const struct fi_info *hints,
			 const struct fi_info *info);

static inline int fi_ibv_cmp_xrc_domain_name(const char *domain_name,
					     const char *rdma_name)
{
	size_t domain_len = strlen(domain_name);
	size_t suffix_len = strlen(verbs_msg_xrc_domain.suffix);

	return domain_len > suffix_len ? strncmp(domain_name, rdma_name,
						 domain_len - suffix_len) : -1;
}

int fi_ibv_cq_signal(struct fid_cq *cq);

ssize_t fi_ibv_eq_write_event(struct fi_ibv_eq *eq, uint32_t event,
		const void *buf, size_t len);

int fi_ibv_query_atomic(struct fid_domain *domain_fid, enum fi_datatype datatype,
			enum fi_op op, struct fi_atomic_attr *attr,
			uint64_t flags);
int fi_ibv_set_rnr_timer(struct ibv_qp *qp);
void fi_ibv_cleanup_cq(struct fi_ibv_ep *cur_ep);
int fi_ibv_find_max_inline(struct ibv_pd *pd, struct ibv_context *context,
			   enum ibv_qp_type qp_type);

struct fi_ibv_dgram_av {
	struct util_av util_av;
	struct dlist_entry av_entry_list;
};

struct fi_ibv_dgram_av_entry {
	struct dlist_entry list_entry;
	struct ofi_ib_ud_ep_name addr;
	struct ibv_ah *ah;
};

static inline struct fi_ibv_dgram_av_entry*
fi_ibv_dgram_av_lookup_av_entry(fi_addr_t fi_addr)
{
	return (struct fi_ibv_dgram_av_entry *) (uintptr_t) fi_addr;
}

/* NOTE:
 * When ibv_post_send/recv returns '-1' it means the following:
 * Deal with non-compliant libibverbs drivers which set errno
 * instead of directly returning the error value
 */
static inline ssize_t fi_ibv_handle_post(int ret)
{
	switch (ret) {
		case -ENOMEM:
		case ENOMEM:
			ret = -FI_EAGAIN;
			break;
		case -1:
			ret = (errno == ENOMEM) ? -FI_EAGAIN :
						  -errno;
			break;
		default:
			ret = -abs(ret);
			break;
	}
	return ret;
}

/* Returns 0 if it processes WR entry for which user
 * doesn't request the completion */
static inline int
fi_ibv_process_wc(struct fi_ibv_cq *cq, struct ibv_wc *wc)
{
	return (wc->wr_id == VERBS_NO_COMP_FLAG) ? 0 : 1;
}

/* Returns 0 and tries read new completions if it processes
 * WR entry for which user doesn't request the completion */
static inline int
fi_ibv_process_wc_poll_new(struct fi_ibv_cq *cq, struct ibv_wc *wc)
{
	struct fi_ibv_domain *domain = container_of(cq->util_cq.domain,
						    struct fi_ibv_domain,
						    util_domain);
	if (wc->wr_id == VERBS_NO_COMP_FLAG) {
		int ret;

		while ((ret = domain->poll_cq(cq->cq, 1, wc)) > 0) {
			if (wc->wr_id != VERBS_NO_COMP_FLAG)
				return 1;
		}
		return ret;
	}
	return 1;
}

static inline int fi_ibv_wc_2_wce(struct fi_ibv_cq *cq,
				  struct ibv_wc *wc,
				  struct fi_ibv_wce **wce)

{
	*wce = ofi_buf_alloc(cq->wce_pool);
	if (OFI_UNLIKELY(!*wce))
		return -FI_ENOMEM;
	memset(*wce, 0, sizeof(**wce));
	(*wce)->wc = *wc;

	return FI_SUCCESS;
}

#define fi_ibv_init_sge(buf, len, desc) (struct ibv_sge)		\
	{ .addr = (uintptr_t)buf,					\
	  .length = (uint32_t)len,					\
	  .lkey = (uint32_t)(uintptr_t)desc }

#define fi_ibv_set_sge_iov(sg_list, iov, count, desc)	\
({							\
	size_t i;					\
	sg_list = alloca(sizeof(*sg_list) * count);	\
	for (i = 0; i < count; i++) {			\
		sg_list[i] = fi_ibv_init_sge(		\
				iov[i].iov_base,	\
				iov[i].iov_len,		\
				desc[i]);		\
	}						\
})

#define fi_ibv_set_sge_iov_count_len(sg_list, iov, count, desc, len)	\
({									\
	size_t i;							\
	sg_list = alloca(sizeof(*sg_list) * count);			\
	for (i = 0; i < count; i++) {					\
		sg_list[i] = fi_ibv_init_sge(				\
				iov[i].iov_base,			\
				iov[i].iov_len,				\
				desc[i]);				\
		len += iov[i].iov_len;					\
	}								\
})

#define fi_ibv_init_sge_inline(buf, len) fi_ibv_init_sge(buf, len, NULL)

#define fi_ibv_set_sge_iov_inline(sg_list, iov, count, len)	\
({								\
	size_t i;						\
	sg_list = alloca(sizeof(*sg_list) * count);		\
	for (i = 0; i < count; i++) {				\
		sg_list[i] = fi_ibv_init_sge_inline(		\
					iov[i].iov_base,	\
					iov[i].iov_len);	\
		len += iov[i].iov_len;				\
	}							\
})

#define fi_ibv_send_iov(ep, wr, iov, desc, count)		\
	fi_ibv_send_iov_flags(ep, wr, iov, desc, count,		\
			      (ep)->info->tx_attr->op_flags)

#define fi_ibv_send_msg(ep, wr, msg, flags)				\
	fi_ibv_send_iov_flags(ep, wr, (msg)->msg_iov, (msg)->desc,	\
			      (msg)->iov_count, flags)


static inline int fi_ibv_poll_reap_unsig_cq(struct fi_ibv_ep *ep)
{
	struct fi_ibv_wce *wce;
	struct ibv_wc wc[10];
	int ret, i;
	struct fi_ibv_cq *cq =
		container_of(ep->util_ep.tx_cq, struct fi_ibv_cq, util_cq);
	struct fi_ibv_domain *domain = container_of(cq->util_cq.domain,
						    struct fi_ibv_domain,
						    util_domain);

	cq->util_cq.cq_fastlock_acquire(&cq->util_cq.cq_lock);
	/* TODO: retrieve WCs as much as possible in a single
	 * ibv_poll_cq call */
	while (1) {
		ret = domain->poll_cq(cq->cq, 10, wc);
		if (ret <= 0) {
			cq->util_cq.cq_fastlock_release(&cq->util_cq.cq_lock);
			return ret;
		}
		for (i = 0; i < ret; i++) {
			if (!fi_ibv_process_wc(cq, &wc[i]))
				continue;
			if (OFI_LIKELY(!fi_ibv_wc_2_wce(cq, &wc[i], &wce)))
				slist_insert_tail(&wce->entry, &cq->wcq);
		}
	}

	cq->util_cq.cq_fastlock_release(&cq->util_cq.cq_lock);
	return FI_SUCCESS;
}

/* WR must be filled out by now except for context */
static inline ssize_t
fi_ibv_send_poll_cq_if_needed(struct fi_ibv_ep *ep, struct ibv_send_wr *wr)
{
	struct ibv_send_wr *bad_wr;
	struct fi_ibv_domain *domain =
		container_of(ep->util_ep.domain, struct fi_ibv_domain, util_domain);
	int ret;

	ret = domain->post_send(ep->ibv_qp, wr, &bad_wr);
	if (OFI_UNLIKELY(ret)) {
		ret = fi_ibv_handle_post(ret);
		if (OFI_LIKELY(ret == -FI_EAGAIN)) {
			ret = fi_ibv_poll_reap_unsig_cq(ep);
			if (OFI_UNLIKELY(ret))
				return -FI_EAGAIN;
			/* Try again and return control to a caller */
			ret = fi_ibv_handle_post(
				domain->post_send(ep->ibv_qp, wr, &bad_wr));
		}
	}
	return ret;
}

static inline ssize_t
fi_ibv_send_buf(struct fi_ibv_ep *ep, struct ibv_send_wr *wr,
		const void *buf, size_t len, void *desc)
{
	struct ibv_sge sge = fi_ibv_init_sge(buf, len, desc);

	assert(wr->wr_id != VERBS_NO_COMP_FLAG);

	wr->sg_list = &sge;
	wr->num_sge = 1;

	return fi_ibv_send_poll_cq_if_needed(ep, wr);
}

static inline ssize_t
fi_ibv_send_buf_inline(struct fi_ibv_ep *ep, struct ibv_send_wr *wr,
		       const void *buf, size_t len)
{
	struct ibv_sge sge = fi_ibv_init_sge_inline(buf, len);

	assert(wr->wr_id == VERBS_NO_COMP_FLAG);

	wr->sg_list = &sge;
	wr->num_sge = 1;

	return fi_ibv_send_poll_cq_if_needed(ep, wr);
}

static inline ssize_t
fi_ibv_send_iov_flags(struct fi_ibv_ep *ep, struct ibv_send_wr *wr,
		      const struct iovec *iov, void **desc, int count,
		      uint64_t flags)
{
	size_t len = 0;

	if (!desc)
		fi_ibv_set_sge_iov_inline(wr->sg_list, iov, count, len);
	else
		fi_ibv_set_sge_iov_count_len(wr->sg_list, iov, count, desc, len);

	wr->num_sge = count;
	wr->send_flags = VERBS_INJECT_FLAGS(ep, len, flags);
	wr->wr_id = VERBS_COMP_FLAGS(ep, flags, wr->wr_id);

	if (flags & FI_FENCE)
		wr->send_flags |= IBV_SEND_FENCE;

	return fi_ibv_send_poll_cq_if_needed(ep, wr);
}

int fi_ibv_get_rai_id(const char *node, const char *service, uint64_t flags,
		      const struct fi_info *hints, struct rdma_addrinfo **rai,
		      struct rdma_cm_id **id);

#endif /* FI_VERBS_H */
