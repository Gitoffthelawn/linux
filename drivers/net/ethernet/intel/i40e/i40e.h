/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2013 - 2021 Intel Corporation. */

#ifndef _I40E_H_
#define _I40E_H_

#include <linux/linkmode.h>
#include <linux/pci.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/types.h>
#include <linux/avf/virtchnl.h>
#include <linux/net/intel/i40e_client.h>
#include <net/devlink.h>
#include <net/pkt_cls.h>
#include <net/udp_tunnel.h>
#include "i40e_dcb.h"
#include "i40e_debug.h"
#include "i40e_devlink.h"
#include "i40e_io.h"
#include "i40e_prototype.h"
#include "i40e_register.h"
#include "i40e_txrx.h"

/* Useful i40e defaults */
#define I40E_MAX_VEB			16

#define I40E_MAX_NUM_DESCRIPTORS	4096
#define I40E_MAX_NUM_DESCRIPTORS_XL710	8160
#define I40E_MAX_CSR_SPACE		(4 * 1024 * 1024 - 64 * 1024)
#define I40E_DEFAULT_NUM_DESCRIPTORS	512
#define I40E_REQ_DESCRIPTOR_MULTIPLE	32
#define I40E_MIN_NUM_DESCRIPTORS	64
#define I40E_MIN_MSIX			2
#define I40E_DEFAULT_NUM_VMDQ_VSI	8 /* max 256 VSIs */
#define I40E_MIN_VSI_ALLOC		83 /* LAN, ATR, FCOE, 64 VF */
/* max 16 qps */
#define i40e_default_queues_per_vmdq(pf) \
	(test_bit(I40E_HW_CAP_RSS_AQ, (pf)->hw.caps) ? 4 : 1)
#define I40E_DEFAULT_QUEUES_PER_VF	4
#define I40E_MAX_VF_QUEUES		16
#define i40e_pf_get_max_q_per_tc(pf) \
	(test_bit(I40E_HW_CAP_128_QP_RSS, (pf)->hw.caps) ? 128 : 64)
#define I40E_FDIR_RING_COUNT		32
#define I40E_MAX_AQ_BUF_SIZE		4096
#define I40E_AQ_LEN			256
#define I40E_MIN_ARQ_LEN		1
#define I40E_MIN_ASQ_LEN		2
#define I40E_AQ_WORK_LIMIT		66 /* max number of VFs + a little */
#define I40E_MAX_USER_PRIORITY		8
#define I40E_DEFAULT_TRAFFIC_CLASS	BIT(0)
#define I40E_QUEUE_WAIT_RETRY_LIMIT	10
#define I40E_INT_NAME_STR_LEN		(IFNAMSIZ + 16)

#define I40E_PHY_DEBUG_ALL \
	(I40E_AQ_PHY_DEBUG_DISABLE_LINK_FW | \
	I40E_AQ_PHY_DEBUG_DISABLE_ALL_LINK_FW)

#define I40E_OEM_EETRACK_ID		0xffffffff
#define I40E_NVM_VERSION_LO_MASK	GENMASK(7, 0)
#define I40E_NVM_VERSION_HI_MASK	GENMASK(15, 12)
#define I40E_OEM_VER_BUILD_MASK		GENMASK(23, 8)
#define I40E_OEM_VER_PATCH_MASK		GENMASK(7, 0)
#define I40E_OEM_VER_MASK		GENMASK(31, 24)
#define I40E_OEM_GEN_MASK		GENMASK(31, 24)
#define I40E_OEM_SNAP_MASK		GENMASK(23, 16)
#define I40E_OEM_RELEASE_MASK		GENMASK(15, 0)

#define I40E_RX_DESC(R, i)	\
	(&(((union i40e_rx_desc *)((R)->desc))[i]))
#define I40E_TX_DESC(R, i)	\
	(&(((struct i40e_tx_desc *)((R)->desc))[i]))
#define I40E_TX_CTXTDESC(R, i)	\
	(&(((struct i40e_tx_context_desc *)((R)->desc))[i]))
#define I40E_TX_FDIRDESC(R, i)	\
	(&(((struct i40e_filter_program_desc *)((R)->desc))[i]))

/* BW rate limiting */
#define I40E_BW_CREDIT_DIVISOR		50 /* 50Mbps per BW credit */
#define I40E_BW_MBPS_DIVISOR		125000 /* rate / (1000000 / 8) Mbps */
#define I40E_MAX_BW_INACTIVE_ACCUM	4 /* accumulate 4 credits max */

/* driver state flags */
enum i40e_state {
	__I40E_TESTING,
	__I40E_CONFIG_BUSY,
	__I40E_CONFIG_DONE,
	__I40E_DOWN,
	__I40E_SERVICE_SCHED,
	__I40E_ADMINQ_EVENT_PENDING,
	__I40E_MDD_EVENT_PENDING,
	__I40E_MDD_VF_PRINT_PENDING,
	__I40E_VFLR_EVENT_PENDING,
	__I40E_RESET_RECOVERY_PENDING,
	__I40E_TIMEOUT_RECOVERY_PENDING,
	__I40E_MISC_IRQ_REQUESTED,
	__I40E_RESET_INTR_RECEIVED,
	__I40E_REINIT_REQUESTED,
	__I40E_PF_RESET_REQUESTED,
	__I40E_PF_RESET_AND_REBUILD_REQUESTED,
	__I40E_CORE_RESET_REQUESTED,
	__I40E_GLOBAL_RESET_REQUESTED,
	__I40E_EMP_RESET_INTR_RECEIVED,
	__I40E_SUSPENDED,
	__I40E_PTP_TX_IN_PROGRESS,
	__I40E_BAD_EEPROM,
	__I40E_DOWN_REQUESTED,
	__I40E_FD_FLUSH_REQUESTED,
	__I40E_FD_ATR_AUTO_DISABLED,
	__I40E_FD_SB_AUTO_DISABLED,
	__I40E_RESET_FAILED,
	__I40E_PORT_SUSPENDED,
	__I40E_VF_DISABLE,
	__I40E_MACVLAN_SYNC_PENDING,
	__I40E_TEMP_LINK_POLLING,
	__I40E_CLIENT_SERVICE_REQUESTED,
	__I40E_CLIENT_L2_CHANGE,
	__I40E_CLIENT_RESET,
	__I40E_VIRTCHNL_OP_PENDING,
	__I40E_RECOVERY_MODE,
	__I40E_VF_RESETS_DISABLED,	/* disable resets during i40e_remove */
	__I40E_IN_REMOVE,
	__I40E_VFS_RELEASING,
	/* This must be last as it determines the size of the BITMAP */
	__I40E_STATE_SIZE__,
};

#define I40E_PF_RESET_FLAG	BIT_ULL(__I40E_PF_RESET_REQUESTED)
#define I40E_PF_RESET_AND_REBUILD_FLAG	\
	BIT_ULL(__I40E_PF_RESET_AND_REBUILD_REQUESTED)

/* VSI state flags */
enum i40e_vsi_state {
	__I40E_VSI_DOWN,
	__I40E_VSI_NEEDS_RESTART,
	__I40E_VSI_SYNCING_FILTERS,
	__I40E_VSI_OVERFLOW_PROMISC,
	__I40E_VSI_REINIT_REQUESTED,
	__I40E_VSI_DOWN_REQUESTED,
	__I40E_VSI_RELEASING,
	/* This must be last as it determines the size of the BITMAP */
	__I40E_VSI_STATE_SIZE__,
};

enum i40e_pf_flags {
	I40E_FLAG_MSI_ENA,
	I40E_FLAG_MSIX_ENA,
	I40E_FLAG_RSS_ENA,
	I40E_FLAG_VMDQ_ENA,
	I40E_FLAG_SRIOV_ENA,
	I40E_FLAG_DCB_CAPABLE,
	I40E_FLAG_DCB_ENA,
	I40E_FLAG_FD_SB_ENA,
	I40E_FLAG_FD_ATR_ENA,
	I40E_FLAG_MFP_ENA,
	I40E_FLAG_HW_ATR_EVICT_ENA,
	I40E_FLAG_VEB_MODE_ENA,
	I40E_FLAG_VEB_STATS_ENA,
	I40E_FLAG_LINK_POLLING_ENA,
	I40E_FLAG_TRUE_PROMISC_ENA,
	I40E_FLAG_LEGACY_RX_ENA,
	I40E_FLAG_PTP_ENA,
	I40E_FLAG_IWARP_ENA,
	I40E_FLAG_LINK_DOWN_ON_CLOSE_ENA,
	I40E_FLAG_SOURCE_PRUNING_DIS,
	I40E_FLAG_TC_MQPRIO_ENA,
	I40E_FLAG_FD_SB_INACTIVE,
	I40E_FLAG_FD_SB_TO_CLOUD_FILTER,
	I40E_FLAG_FW_LLDP_DIS,
	I40E_FLAG_RS_FEC,
	I40E_FLAG_BASE_R_FEC,
	/* TOTAL_PORT_SHUTDOWN_ENA
	 * Allows to physically disable the link on the NIC's port.
	 * If enabled, (after link down request from the OS)
	 * no link, traffic or led activity is possible on that port.
	 *
	 * If I40E_FLAG_TOTAL_PORT_SHUTDOWN_ENA is set, the
	 * I40E_FLAG_LINK_DOWN_ON_CLOSE_ENA must be explicitly forced
	 * to true and cannot be disabled by system admin at that time.
	 * The functionalities are exclusive in terms of configuration, but
	 * they also have similar behavior (allowing to disable physical
	 * link of the port), with following differences:
	 * - LINK_DOWN_ON_CLOSE_ENA is configurable at host OS run-time and
	 *   is supported by whole family of 7xx Intel Ethernet Controllers
	 * - TOTAL_PORT_SHUTDOWN_ENA may be enabled only before OS loads
	 *   (in BIOS) only if motherboard's BIOS and NIC's FW has support of it
	 * - when LINK_DOWN_ON_CLOSE_ENABLED is used, the link is being brought
	 *   down by sending phy_type=0 to NIC's FW
	 * - when TOTAL_PORT_SHUTDOWN_ENA is used, phy_type is not altered,
	 *   instead the link is being brought down by clearing
	 *   bit (I40E_AQ_PHY_ENABLE_LINK) in abilities field of
	 *   i40e_aq_set_phy_config structure
	 */
	I40E_FLAG_TOTAL_PORT_SHUTDOWN_ENA,
	I40E_FLAG_VF_VLAN_PRUNING_ENA,
	I40E_FLAG_MDD_AUTO_RESET_VF,
	I40E_PF_FLAGS_NBITS,		/* must be last */
};

enum i40e_interrupt_policy {
	I40E_INTERRUPT_BEST_CASE,
	I40E_INTERRUPT_MEDIUM,
	I40E_INTERRUPT_LOWEST
};

struct i40e_lump_tracking {
	u16 num_entries;
	u16 list[];
#define I40E_PILE_VALID_BIT  0x8000
#define I40E_IWARP_IRQ_PILE_ID  (I40E_PILE_VALID_BIT - 2)
};

#define I40E_DEFAULT_ATR_SAMPLE_RATE	20
#define I40E_FDIR_MAX_RAW_PACKET_SIZE	512
#define I40E_FDIR_BUFFER_FULL_MARGIN	10
#define I40E_FDIR_BUFFER_HEAD_ROOM	32
#define I40E_FDIR_BUFFER_HEAD_ROOM_FOR_ATR (I40E_FDIR_BUFFER_HEAD_ROOM * 4)

#define I40E_HKEY_ARRAY_SIZE	((I40E_PFQF_HKEY_MAX_INDEX + 1) * 4)
#define I40E_HLUT_ARRAY_SIZE	((I40E_PFQF_HLUT_MAX_INDEX + 1) * 4)
#define I40E_VF_HLUT_ARRAY_SIZE	((I40E_VFQF_HLUT1_MAX_INDEX + 1) * 4)

enum i40e_fd_stat_idx {
	I40E_FD_STAT_ATR,
	I40E_FD_STAT_SB,
	I40E_FD_STAT_ATR_TUNNEL,
	I40E_FD_STAT_PF_COUNT
};
#define I40E_FD_STAT_PF_IDX(pf_id) ((pf_id) * I40E_FD_STAT_PF_COUNT)
#define I40E_FD_ATR_STAT_IDX(pf_id) \
			(I40E_FD_STAT_PF_IDX(pf_id) + I40E_FD_STAT_ATR)
#define I40E_FD_SB_STAT_IDX(pf_id)  \
			(I40E_FD_STAT_PF_IDX(pf_id) + I40E_FD_STAT_SB)
#define I40E_FD_ATR_TUNNEL_STAT_IDX(pf_id) \
			(I40E_FD_STAT_PF_IDX(pf_id) + I40E_FD_STAT_ATR_TUNNEL)

/* The following structure contains the data parsed from the user-defined
 * field of the ethtool_rx_flow_spec structure.
 */
struct i40e_rx_flow_userdef {
	bool flex_filter;
	u16 flex_word;
	u16 flex_offset;
};

struct i40e_fdir_filter {
	struct hlist_node fdir_node;
	/* filter ipnut set */
	u8 flow_type;
	u8 ipl4_proto;
	/* TX packet view of src and dst */
	__be32 dst_ip;
	__be32 src_ip;
	__be32 dst_ip6[4];
	__be32 src_ip6[4];
	__be16 src_port;
	__be16 dst_port;
	__be32 sctp_v_tag;

	__be16 vlan_etype;
	__be16 vlan_tag;
	/* Flexible data to match within the packet payload */
	__be16 flex_word;
	u16 flex_offset;
	bool flex_filter;

	/* filter control */
	u16 q_index;
	u8  flex_off;
	u8  pctype;
	u16 dest_vsi;
	u8  dest_ctl;
	u8  fd_status;
	u16 cnt_index;
	u32 fd_id;
};

#define I40E_CLOUD_FIELD_OMAC		BIT(0)
#define I40E_CLOUD_FIELD_IMAC		BIT(1)
#define I40E_CLOUD_FIELD_IVLAN		BIT(2)
#define I40E_CLOUD_FIELD_TEN_ID		BIT(3)
#define I40E_CLOUD_FIELD_IIP		BIT(4)

#define I40E_CLOUD_FILTER_FLAGS_OMAC	I40E_CLOUD_FIELD_OMAC
#define I40E_CLOUD_FILTER_FLAGS_IMAC	I40E_CLOUD_FIELD_IMAC
#define I40E_CLOUD_FILTER_FLAGS_IMAC_IVLAN	(I40E_CLOUD_FIELD_IMAC | \
						 I40E_CLOUD_FIELD_IVLAN)
#define I40E_CLOUD_FILTER_FLAGS_IMAC_TEN_ID	(I40E_CLOUD_FIELD_IMAC | \
						 I40E_CLOUD_FIELD_TEN_ID)
#define I40E_CLOUD_FILTER_FLAGS_OMAC_TEN_ID_IMAC (I40E_CLOUD_FIELD_OMAC | \
						  I40E_CLOUD_FIELD_IMAC | \
						  I40E_CLOUD_FIELD_TEN_ID)
#define I40E_CLOUD_FILTER_FLAGS_IMAC_IVLAN_TEN_ID (I40E_CLOUD_FIELD_IMAC | \
						   I40E_CLOUD_FIELD_IVLAN | \
						   I40E_CLOUD_FIELD_TEN_ID)
#define I40E_CLOUD_FILTER_FLAGS_IIP	I40E_CLOUD_FIELD_IIP

struct i40e_cloud_filter {
	struct hlist_node cloud_node;
	unsigned long cookie;
	/* cloud filter input set follows */
	u8 dst_mac[ETH_ALEN];
	u8 src_mac[ETH_ALEN];
	__be16 vlan_id;
	u16 seid;       /* filter control */
	__be16 dst_port;
	__be16 src_port;
	u32 tenant_id;
	union {
		struct {
			struct in_addr dst_ip;
			struct in_addr src_ip;
		} v4;
		struct {
			struct in6_addr dst_ip6;
			struct in6_addr src_ip6;
		} v6;
	} ip;
#define dst_ipv6	ip.v6.dst_ip6.s6_addr32
#define src_ipv6	ip.v6.src_ip6.s6_addr32
#define dst_ipv4	ip.v4.dst_ip.s_addr
#define src_ipv4	ip.v4.src_ip.s_addr
	u16 n_proto;    /* Ethernet Protocol */
	u8 ip_proto;    /* IPPROTO value */
	u8 flags;
#define I40E_CLOUD_TNL_TYPE_NONE        0xff
	u8 tunnel_type;
};

#define I40E_DCB_PRIO_TYPE_STRICT	0
#define I40E_DCB_PRIO_TYPE_ETS		1
#define I40E_DCB_STRICT_PRIO_CREDITS	127
/* DCB per TC information data structure */
struct i40e_tc_info {
	u16	qoffset;	/* Queue offset from base queue */
	u16	qcount;		/* Total Queues */
	u8	netdev_tc;	/* Netdev TC index if netdev associated */
};

/* TC configuration data structure */
struct i40e_tc_configuration {
	u8	numtc;		/* Total number of enabled TCs */
	u8	enabled_tc;	/* TC map */
	struct i40e_tc_info tc_info[I40E_MAX_TRAFFIC_CLASS];
};

#define I40E_UDP_PORT_INDEX_UNUSED	255
struct i40e_udp_port_config {
	/* AdminQ command interface expects port number in Host byte order */
	u16 port;
	u8 type;
	u8 filter_index;
};

/* macros related to FLX_PIT */
#define I40E_FLEX_SET_FSIZE(fsize) (((fsize) << \
				    I40E_PRTQF_FLX_PIT_FSIZE_SHIFT) & \
				    I40E_PRTQF_FLX_PIT_FSIZE_MASK)
#define I40E_FLEX_SET_DST_WORD(dst) (((dst) << \
				     I40E_PRTQF_FLX_PIT_DEST_OFF_SHIFT) & \
				     I40E_PRTQF_FLX_PIT_DEST_OFF_MASK)
#define I40E_FLEX_SET_SRC_WORD(src) (((src) << \
				     I40E_PRTQF_FLX_PIT_SOURCE_OFF_SHIFT) & \
				     I40E_PRTQF_FLX_PIT_SOURCE_OFF_MASK)
#define I40E_FLEX_PREP_VAL(dst, fsize, src) (I40E_FLEX_SET_DST_WORD(dst) | \
					     I40E_FLEX_SET_FSIZE(fsize) | \
					     I40E_FLEX_SET_SRC_WORD(src))


#define I40E_MAX_FLEX_SRC_OFFSET 0x1F

/* macros related to GLQF_ORT */
#define I40E_ORT_SET_IDX(idx)		(((idx) << \
					  I40E_GLQF_ORT_PIT_INDX_SHIFT) & \
					 I40E_GLQF_ORT_PIT_INDX_MASK)

#define I40E_ORT_SET_COUNT(count)	(((count) << \
					  I40E_GLQF_ORT_FIELD_CNT_SHIFT) & \
					 I40E_GLQF_ORT_FIELD_CNT_MASK)

#define I40E_ORT_SET_PAYLOAD(payload)	(((payload) << \
					  I40E_GLQF_ORT_FLX_PAYLOAD_SHIFT) & \
					 I40E_GLQF_ORT_FLX_PAYLOAD_MASK)

#define I40E_ORT_PREP_VAL(idx, count, payload) (I40E_ORT_SET_IDX(idx) | \
						I40E_ORT_SET_COUNT(count) | \
						I40E_ORT_SET_PAYLOAD(payload))

#define I40E_L3_GLQF_ORT_IDX		34
#define I40E_L4_GLQF_ORT_IDX		35

/* Flex PIT register index */
#define I40E_FLEX_PIT_IDX_START_L3	3
#define I40E_FLEX_PIT_IDX_START_L4	6

#define I40E_FLEX_PIT_TABLE_SIZE	3

#define I40E_FLEX_DEST_UNUSED		63

#define I40E_FLEX_INDEX_ENTRIES		8

/* Flex MASK to disable all flexible entries */
#define I40E_FLEX_INPUT_MASK	(I40E_FLEX_50_MASK | I40E_FLEX_51_MASK | \
				 I40E_FLEX_52_MASK | I40E_FLEX_53_MASK | \
				 I40E_FLEX_54_MASK | I40E_FLEX_55_MASK | \
				 I40E_FLEX_56_MASK | I40E_FLEX_57_MASK)

#define I40E_QINT_TQCTL_VAL(qp, vector, nextq_type) \
	(I40E_QINT_TQCTL_CAUSE_ENA_MASK | \
	(I40E_TX_ITR << I40E_QINT_TQCTL_ITR_INDX_SHIFT) | \
	((vector) << I40E_QINT_TQCTL_MSIX_INDX_SHIFT) | \
	((qp) << I40E_QINT_TQCTL_NEXTQ_INDX_SHIFT) | \
	(I40E_QUEUE_TYPE_##nextq_type << I40E_QINT_TQCTL_NEXTQ_TYPE_SHIFT))

#define I40E_QINT_RQCTL_VAL(qp, vector, nextq_type) \
	(I40E_QINT_RQCTL_CAUSE_ENA_MASK | \
	(I40E_RX_ITR << I40E_QINT_RQCTL_ITR_INDX_SHIFT) | \
	((vector) << I40E_QINT_RQCTL_MSIX_INDX_SHIFT) | \
	((qp) << I40E_QINT_RQCTL_NEXTQ_INDX_SHIFT) | \
	(I40E_QUEUE_TYPE_##nextq_type << I40E_QINT_RQCTL_NEXTQ_TYPE_SHIFT))

struct i40e_flex_pit {
	struct list_head list;
	u16 src_offset;
	u8 pit_index;
};

struct i40e_fwd_adapter {
	struct net_device *netdev;
	int bit_no;
};

struct i40e_channel {
	struct list_head list;
	bool initialized;
	u8 type;
	u16 vsi_number; /* Assigned VSI number from AQ 'Add VSI' response */
	u16 stat_counter_idx;
	u16 base_queue;
	u16 num_queue_pairs; /* Requested by user */
	u16 seid;

	u8 enabled_tc;
	struct i40e_aqc_vsi_properties_data info;

	u64 max_tx_rate;
	struct i40e_fwd_adapter *fwd;

	/* track this channel belongs to which VSI */
	struct i40e_vsi *parent_vsi;
};

struct i40e_ptp_pins_settings;

static inline bool i40e_is_channel_macvlan(struct i40e_channel *ch)
{
	return !!ch->fwd;
}

static inline const u8 *i40e_channel_mac(struct i40e_channel *ch)
{
	if (i40e_is_channel_macvlan(ch))
		return ch->fwd->netdev->dev_addr;
	else
		return NULL;
}

/* struct that defines the Ethernet device */
struct i40e_pf {
	struct pci_dev *pdev;
	struct devlink_port devlink_port;
	struct i40e_hw hw;
	DECLARE_BITMAP(state, __I40E_STATE_SIZE__);
	struct msix_entry *msix_entries;

	u16 num_vmdq_vsis;         /* num vmdq vsis this PF has set up */
	u16 num_vmdq_qps;          /* num queue pairs per vmdq pool */
	u16 num_vmdq_msix;         /* num queue vectors per vmdq pool */
	u16 num_req_vfs;           /* num VFs requested for this PF */
	u16 num_vf_qps;            /* num queue pairs per VF */
	u16 num_lan_qps;           /* num lan queues this PF has set up */
	u16 num_lan_msix;          /* num queue vectors for the base PF vsi */
	u16 num_fdsb_msix;         /* num queue vectors for sideband Fdir */
	u16 num_iwarp_msix;        /* num of iwarp vectors for this PF */
	int iwarp_base_vector;
	int queues_left;           /* queues left unclaimed */
	u16 alloc_rss_size;        /* allocated RSS queues */
	u16 rss_size_max;          /* HW defined max RSS queues */
	u16 fdir_pf_filter_count;  /* num of guaranteed filters for this PF */
	u16 num_alloc_vsi;         /* num VSIs this driver supports */
	bool wol_en;

	struct hlist_head fdir_filter_list;
	u16 fdir_pf_active_filters;
	unsigned long fd_flush_timestamp;
	u32 fd_flush_cnt;
	u32 fd_add_err;
	u32 fd_atr_cnt;

	/* Book-keeping of side-band filter count per flow-type.
	 * This is used to detect and handle input set changes for
	 * respective flow-type.
	 */
	u16 fd_tcp4_filter_cnt;
	u16 fd_udp4_filter_cnt;
	u16 fd_sctp4_filter_cnt;
	u16 fd_ip4_filter_cnt;

	u16 fd_tcp6_filter_cnt;
	u16 fd_udp6_filter_cnt;
	u16 fd_sctp6_filter_cnt;
	u16 fd_ip6_filter_cnt;

	/* Flexible filter table values that need to be programmed into
	 * hardware, which expects L3 and L4 to be programmed separately. We
	 * need to ensure that the values are in ascended order and don't have
	 * duplicates, so we track each L3 and L4 values in separate lists.
	 */
	struct list_head l3_flex_pit_list;
	struct list_head l4_flex_pit_list;

	struct udp_tunnel_nic_shared udp_tunnel_shared;
	struct udp_tunnel_nic_info udp_tunnel_nic;

	struct hlist_head cloud_filter_list;
	u16 num_cloud_filters;

	u16 rx_itr_default;
	u16 tx_itr_default;
	u32 msg_enable;
	char int_name[I40E_INT_NAME_STR_LEN];
	unsigned long service_timer_period;
	unsigned long service_timer_previous;
	struct timer_list service_timer;
	struct work_struct service_task;

	DECLARE_BITMAP(flags, I40E_PF_FLAGS_NBITS);
	struct i40e_client_instance *cinst;
	bool stat_offsets_loaded;
	struct i40e_hw_port_stats stats;
	struct i40e_hw_port_stats stats_offsets;
	u32 tx_timeout_count;
	u32 tx_timeout_recovery_level;
	unsigned long tx_timeout_last_recovery;
	u32 hw_csum_rx_error;
	u32 led_status;
	u16 corer_count; /* Core reset count */
	u16 globr_count; /* Global reset count */
	u16 empr_count; /* EMP reset count */
	u16 pfr_count; /* PF reset count */
	u16 sw_int_count; /* SW interrupt count */

	struct mutex switch_mutex;
	u16 lan_vsi;       /* our default LAN VSI */
	u16 lan_veb;       /* initial relay, if exists */
#define I40E_NO_VEB	0xffff
#define I40E_NO_VSI	0xffff
	u16 next_vsi;      /* Next unallocated VSI - 0-based! */
	struct i40e_vsi **vsi;
	struct i40e_veb *veb[I40E_MAX_VEB];

	struct i40e_lump_tracking *qp_pile;
	struct i40e_lump_tracking *irq_pile;

	/* switch config info */
	u16 main_vsi_seid;
	u16 mac_seid;
#ifdef CONFIG_DEBUG_FS
	struct dentry *i40e_dbg_pf;
#endif /* CONFIG_DEBUG_FS */
	bool cur_promisc;

	/* sr-iov config info */
	struct i40e_vf *vf;
	int num_alloc_vfs;	/* actual number of VFs allocated */
	u32 vf_aq_requests;
	u32 arq_overflows;	/* Not fatal, possibly indicative of problems */
	struct ratelimit_state mdd_message_rate_limit;
	/* DCBx/DCBNL capability for PF that indicates
	 * whether DCBx is managed by firmware or host
	 * based agent (LLDPAD). Also, indicates what
	 * flavor of DCBx protocol (IEEE/CEE) is supported
	 * by the device. For now we're supporting IEEE
	 * mode only.
	 */
	u16 dcbx_cap;

	struct i40e_filter_control_settings filter_settings;
	struct i40e_rx_pb_config pb_cfg; /* Current Rx packet buffer config */
	struct i40e_dcbx_config tmp_cfg;

/* GPIO defines used by PTP */
#define I40E_SDP3_2			18
#define I40E_SDP3_3			19
#define I40E_GPIO_4			20
#define I40E_LED2_0			26
#define I40E_LED2_1			27
#define I40E_LED3_0			28
#define I40E_LED3_1			29
#define I40E_GLGEN_GPIO_SET_SDP_DATA_HI \
	(1 << I40E_GLGEN_GPIO_SET_SDP_DATA_SHIFT)
#define I40E_GLGEN_GPIO_SET_DRV_SDP_DATA \
	(1 << I40E_GLGEN_GPIO_SET_DRIVE_SDP_SHIFT)
#define I40E_GLGEN_GPIO_CTL_PRT_NUM_0 \
	(0 << I40E_GLGEN_GPIO_CTL_PRT_NUM_SHIFT)
#define I40E_GLGEN_GPIO_CTL_PRT_NUM_1 \
	(1 << I40E_GLGEN_GPIO_CTL_PRT_NUM_SHIFT)
#define I40E_GLGEN_GPIO_CTL_RESERVED	BIT(2)
#define I40E_GLGEN_GPIO_CTL_PRT_NUM_NA_Z \
	(1 << I40E_GLGEN_GPIO_CTL_PRT_NUM_NA_SHIFT)
#define I40E_GLGEN_GPIO_CTL_DIR_OUT \
	(1 << I40E_GLGEN_GPIO_CTL_PIN_DIR_SHIFT)
#define I40E_GLGEN_GPIO_CTL_TRI_DRV_HI \
	(1 << I40E_GLGEN_GPIO_CTL_TRI_CTL_SHIFT)
#define I40E_GLGEN_GPIO_CTL_OUT_HI_RST \
	(1 << I40E_GLGEN_GPIO_CTL_OUT_CTL_SHIFT)
#define I40E_GLGEN_GPIO_CTL_TIMESYNC_0 \
	(3 << I40E_GLGEN_GPIO_CTL_PIN_FUNC_SHIFT)
#define I40E_GLGEN_GPIO_CTL_TIMESYNC_1 \
	(4 << I40E_GLGEN_GPIO_CTL_PIN_FUNC_SHIFT)
#define I40E_GLGEN_GPIO_CTL_NOT_FOR_PHY_CONN \
	(0x3F << I40E_GLGEN_GPIO_CTL_PHY_PIN_NAME_SHIFT)
#define I40E_GLGEN_GPIO_CTL_OUT_DEFAULT \
	(1 << I40E_GLGEN_GPIO_CTL_OUT_DEFAULT_SHIFT)
#define I40E_GLGEN_GPIO_CTL_PORT_0_IN_TIMESYNC_0 \
	(I40E_GLGEN_GPIO_CTL_NOT_FOR_PHY_CONN | \
	 I40E_GLGEN_GPIO_CTL_TIMESYNC_0 | \
	 I40E_GLGEN_GPIO_CTL_RESERVED | I40E_GLGEN_GPIO_CTL_PRT_NUM_0)
#define I40E_GLGEN_GPIO_CTL_PORT_1_IN_TIMESYNC_0 \
	(I40E_GLGEN_GPIO_CTL_NOT_FOR_PHY_CONN | \
	 I40E_GLGEN_GPIO_CTL_TIMESYNC_0 | \
	 I40E_GLGEN_GPIO_CTL_RESERVED | I40E_GLGEN_GPIO_CTL_PRT_NUM_1)
#define I40E_GLGEN_GPIO_CTL_PORT_0_OUT_TIMESYNC_1 \
	(I40E_GLGEN_GPIO_CTL_NOT_FOR_PHY_CONN | \
	 I40E_GLGEN_GPIO_CTL_TIMESYNC_1 | I40E_GLGEN_GPIO_CTL_OUT_HI_RST | \
	 I40E_GLGEN_GPIO_CTL_TRI_DRV_HI | I40E_GLGEN_GPIO_CTL_DIR_OUT | \
	 I40E_GLGEN_GPIO_CTL_RESERVED | I40E_GLGEN_GPIO_CTL_PRT_NUM_0)
#define I40E_GLGEN_GPIO_CTL_PORT_1_OUT_TIMESYNC_1 \
	(I40E_GLGEN_GPIO_CTL_NOT_FOR_PHY_CONN | \
	 I40E_GLGEN_GPIO_CTL_TIMESYNC_1 | I40E_GLGEN_GPIO_CTL_OUT_HI_RST | \
	 I40E_GLGEN_GPIO_CTL_TRI_DRV_HI | I40E_GLGEN_GPIO_CTL_DIR_OUT | \
	 I40E_GLGEN_GPIO_CTL_RESERVED | I40E_GLGEN_GPIO_CTL_PRT_NUM_1)
#define I40E_GLGEN_GPIO_CTL_LED_INIT \
	(I40E_GLGEN_GPIO_CTL_PRT_NUM_NA_Z | \
	 I40E_GLGEN_GPIO_CTL_DIR_OUT | \
	 I40E_GLGEN_GPIO_CTL_TRI_DRV_HI | \
	 I40E_GLGEN_GPIO_CTL_OUT_HI_RST | \
	 I40E_GLGEN_GPIO_CTL_OUT_DEFAULT | \
	 I40E_GLGEN_GPIO_CTL_NOT_FOR_PHY_CONN)
#define I40E_PRTTSYN_AUX_1_INSTNT \
	(1 << I40E_PRTTSYN_AUX_1_INSTNT_SHIFT)
#define I40E_PRTTSYN_AUX_0_OUT_ENABLE \
	(1 << I40E_PRTTSYN_AUX_0_OUT_ENA_SHIFT)
#define I40E_PRTTSYN_AUX_0_OUT_CLK_MOD	(3 << I40E_PRTTSYN_AUX_0_OUTMOD_SHIFT)
#define I40E_PRTTSYN_AUX_0_OUT_ENABLE_CLK_MOD \
	(I40E_PRTTSYN_AUX_0_OUT_ENABLE | I40E_PRTTSYN_AUX_0_OUT_CLK_MOD)
#define I40E_PTP_HALF_SECOND		500000000LL /* nano seconds */
#define I40E_PTP_2_SEC_DELAY		2

	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_caps;
	struct sk_buff *ptp_tx_skb;
	unsigned long ptp_tx_start;
	struct hwtstamp_config tstamp_config;
	struct timespec64 ptp_prev_hw_time;
	struct work_struct ptp_extts0_work;
	ktime_t ptp_reset_start;
	struct mutex tmreg_lock; /* Used to protect the SYSTIME registers. */
	u32 ptp_adj_mult;
	u32 tx_hwtstamp_timeouts;
	u32 tx_hwtstamp_skipped;
	u32 rx_hwtstamp_cleared;
	u32 latch_event_flags;
	spinlock_t ptp_rx_lock; /* Used to protect Rx timestamp registers. */
	unsigned long latch_events[4];
	bool ptp_tx;
	bool ptp_rx;
	struct i40e_ptp_pins_settings *ptp_pins;
	u16 rss_table_size; /* HW RSS table size */
	u32 max_bw;
	u32 min_bw;

	u32 ioremap_len;
	u32 fd_inv;
	u16 phy_led_val;

	u16 last_sw_conf_flags;
	u16 last_sw_conf_valid_flags;
	/* List to keep previous DDP profiles to be rolled back in the future */
	struct list_head ddp_old_prof;
};

/**
 * __i40e_pf_next_vsi - get next valid VSI
 * @pf: pointer to the PF struct
 * @idx: pointer to start position number
 *
 * Find and return next non-NULL VSI pointer in pf->vsi array and
 * updates idx position. Returns NULL if no VSI is found.
 **/
static __always_inline struct i40e_vsi *
__i40e_pf_next_vsi(struct i40e_pf *pf, int *idx)
{
	while (*idx < pf->num_alloc_vsi) {
		if (pf->vsi[*idx])
			return pf->vsi[*idx];
		(*idx)++;
	}
	return NULL;
}

#define i40e_pf_for_each_vsi(_pf, _i, _vsi)			\
	for (_i = 0, _vsi = __i40e_pf_next_vsi(_pf, &_i);	\
	     _vsi;						\
	     _i++, _vsi = __i40e_pf_next_vsi(_pf, &_i))

/**
 * __i40e_pf_next_veb - get next valid VEB
 * @pf: pointer to the PF struct
 * @idx: pointer to start position number
 *
 * Find and return next non-NULL VEB pointer in pf->veb array and
 * updates idx position. Returns NULL if no VEB is found.
 **/
static __always_inline struct i40e_veb *
__i40e_pf_next_veb(struct i40e_pf *pf, int *idx)
{
	while (*idx < I40E_MAX_VEB) {
		if (pf->veb[*idx])
			return pf->veb[*idx];
		(*idx)++;
	}
	return NULL;
}

#define i40e_pf_for_each_veb(_pf, _i, _veb)			\
	for (_i = 0, _veb = __i40e_pf_next_veb(_pf, &_i);	\
	     _veb;						\
	     _i++, _veb = __i40e_pf_next_veb(_pf, &_i))

/**
 * i40e_addr_to_hkey - Convert a 6-byte MAC Address to a u64 hash key
 * @macaddr: the MAC Address as the base key
 *
 * Simply copies the address and returns it as a u64 for hashing
 **/
static inline u64 i40e_addr_to_hkey(const u8 *macaddr)
{
	u64 key = 0;

	ether_addr_copy((u8 *)&key, macaddr);
	return key;
}

enum i40e_filter_state {
	I40E_FILTER_INVALID = 0,	/* Invalid state */
	I40E_FILTER_NEW,		/* New, not sent to FW yet */
	I40E_FILTER_ACTIVE,		/* Added to switch by FW */
	I40E_FILTER_FAILED,		/* Rejected by FW */
	I40E_FILTER_REMOVE,		/* To be removed */
	I40E_FILTER_NEW_SYNC,		/* New, not sent yet, is in i40e_sync_vsi_filters() */
/* There is no 'removed' state; the filter struct is freed */
};
struct i40e_mac_filter {
	struct hlist_node hlist;
	u8 macaddr[ETH_ALEN];
#define I40E_VLAN_ANY -1
	s16 vlan;
	enum i40e_filter_state state;
};

/* Wrapper structure to keep track of filters while we are preparing to send
 * firmware commands. We cannot send firmware commands while holding a
 * spinlock, since it might sleep. To avoid this, we wrap the added filters in
 * a separate structure, which will track the state change and update the real
 * filter while under lock. We can't simply hold the filters in a separate
 * list, as this opens a window for a race condition when adding new MAC
 * addresses to all VLANs, or when adding new VLANs to all MAC addresses.
 */
struct i40e_new_mac_filter {
	struct hlist_node hlist;
	struct i40e_mac_filter *f;

	/* Track future changes to state separately */
	enum i40e_filter_state state;
};

struct i40e_veb {
	struct i40e_pf *pf;
	u16 idx;
	u16 seid;
	u16 uplink_seid;
	u16 stats_idx;		/* index of VEB parent */
	u8  enabled_tc;
	u16 bridge_mode;	/* Bridge Mode (VEB/VEPA) */
	u16 bw_limit;
	u8  bw_max_quanta;
	bool is_abs_credits;
	u8  bw_tc_share_credits[I40E_MAX_TRAFFIC_CLASS];
	u16 bw_tc_limit_credits[I40E_MAX_TRAFFIC_CLASS];
	u8  bw_tc_max_quanta[I40E_MAX_TRAFFIC_CLASS];
	struct kobject *kobj;
	bool stat_offsets_loaded;
	struct i40e_eth_stats stats;
	struct i40e_eth_stats stats_offsets;
	struct i40e_veb_tc_stats tc_stats;
	struct i40e_veb_tc_stats tc_stats_offsets;
};

/* struct that defines a VSI, associated with a dev */
struct i40e_vsi {
	struct net_device *netdev;
	unsigned long active_vlans[BITS_TO_LONGS(VLAN_N_VID)];
	bool netdev_registered;
	bool stat_offsets_loaded;

	u32 current_netdev_flags;
	DECLARE_BITMAP(state, __I40E_VSI_STATE_SIZE__);
#define I40E_VSI_FLAG_FILTER_CHANGED	BIT(0)
#define I40E_VSI_FLAG_VEB_OWNER		BIT(1)
	unsigned long flags;

	/* Per VSI lock to protect elements/hash (MAC filter) */
	spinlock_t mac_filter_hash_lock;
	/* Fixed size hash table with 2^8 buckets for MAC filters */
	DECLARE_HASHTABLE(mac_filter_hash, 8);
	bool has_vlan_filter;

	/* VSI stats */
	struct rtnl_link_stats64 net_stats;
	struct rtnl_link_stats64 net_stats_offsets;
	struct i40e_eth_stats eth_stats;
	struct i40e_eth_stats eth_stats_offsets;
	u64 tx_restart;
	u64 tx_busy;
	u64 tx_linearize;
	u64 tx_force_wb;
	u64 tx_stopped;
	u64 rx_buf_failed;
	u64 rx_page_failed;
	u64 rx_page_reuse;
	u64 rx_page_alloc;
	u64 rx_page_waive;
	u64 rx_page_busy;

	/* These are containers of ring pointers, allocated at run-time */
	struct i40e_ring **rx_rings;
	struct i40e_ring **tx_rings;
	struct i40e_ring **xdp_rings; /* XDP Tx rings */

	u32  active_filters;
	u32  promisc_threshold;

	u16 work_limit;
	u16 int_rate_limit;	/* value in usecs */

	u16 rss_table_size;	/* HW RSS table size */
	u16 rss_size;		/* Allocated RSS queues */
	u8  *rss_hkey_user;	/* User configured hash keys */
	u8  *rss_lut_user;	/* User configured lookup table entries */


	u16 max_frame;
	u16 rx_buf_len;

	struct bpf_prog *xdp_prog;

	/* List of q_vectors allocated to this VSI */
	struct i40e_q_vector **q_vectors;
	int num_q_vectors;
	int base_vector;
	bool irqs_ready;

	u16 seid;		/* HW index of this VSI (absolute index) */
	u16 id;			/* VSI number */
	u16 uplink_seid;

	u16 base_queue;		/* vsi's first queue in hw array */
	u16 alloc_queue_pairs;	/* Allocated Tx/Rx queues */
	u16 req_queue_pairs;	/* User requested queue pairs */
	u16 num_queue_pairs;	/* Used tx and rx pairs */
	u16 num_tx_desc;
	u16 num_rx_desc;
	enum i40e_vsi_type type;  /* VSI type, e.g., LAN, FCoE, etc */
	s16 vf_id;		/* Virtual function ID for SRIOV VSIs */

	struct tc_mqprio_qopt_offload mqprio_qopt; /* queue parameters */
	struct i40e_tc_configuration tc_config;
	struct i40e_aqc_vsi_properties_data info;

	/* VSI BW limit (absolute across all TCs) */
	u16 bw_limit;		/* VSI BW Limit (0 = disabled) */
	u8  bw_max_quanta;	/* Max Quanta when BW limit is enabled */

	/* Relative TC credits across VSIs */
	u8  bw_ets_share_credits[I40E_MAX_TRAFFIC_CLASS];
	/* TC BW limit credits within VSI */
	u16  bw_ets_limit_credits[I40E_MAX_TRAFFIC_CLASS];
	/* TC BW limit max quanta within VSI */
	u8  bw_ets_max_quanta[I40E_MAX_TRAFFIC_CLASS];

	struct i40e_pf *back;	/* Backreference to associated PF */
	u16 idx;		/* index in pf->vsi[] */
	u16 veb_idx;		/* index of VEB parent */
	struct kobject *kobj;	/* sysfs object */
	bool current_isup;	/* Sync 'link up' logging */
	enum i40e_aq_link_speed current_speed;	/* Sync link speed logging */

	/* channel specific fields */
	u16 cnt_q_avail;	/* num of queues available for channel usage */
	u16 orig_rss_size;
	u16 current_rss_size;
	bool reconfig_rss;

	u16 next_base_queue;	/* next queue to be used for channel setup */

	struct list_head ch_list;
	u16 tc_seid_map[I40E_MAX_TRAFFIC_CLASS];

	/* macvlan fields */
#define I40E_MAX_MACVLANS		128 /* Max HW vectors - 1 on FVL */
#define I40E_MIN_MACVLAN_VECTORS	2   /* Min vectors to enable macvlans */
	DECLARE_BITMAP(fwd_bitmask, I40E_MAX_MACVLANS);
	struct list_head macvlan_list;
	int macvlan_cnt;

	void *priv;	/* client driver data reference. */

	/* VSI specific handlers */
	irqreturn_t (*irq_handler)(int irq, void *data);

	unsigned long *af_xdp_zc_qps; /* tracks AF_XDP ZC enabled qps */
} ____cacheline_internodealigned_in_smp;

struct i40e_netdev_priv {
	struct i40e_vsi *vsi;
};

extern struct ida i40e_client_ida;

/* struct that defines an interrupt vector */
struct i40e_q_vector {
	struct i40e_vsi *vsi;

	u16 v_idx;		/* index in the vsi->q_vector array. */
	u16 reg_idx;		/* register index of the interrupt */

	struct napi_struct napi;
	struct rcu_head rcu;	/* to avoid race with update stats on free */

	struct i40e_ring_container rx;
	struct i40e_ring_container tx;

	u8 itr_countdown;	/* when 0 should adjust adaptive ITR */
	u8 num_ringpairs;	/* total number of ring pairs in vector */

	cpumask_t affinity_mask;
	struct irq_affinity_notify affinity_notify;

	char name[I40E_INT_NAME_STR_LEN];
	bool arm_wb_state;
	bool in_busy_poll;
	int irq_num;		/* IRQ assigned to this q_vector */
} ____cacheline_internodealigned_in_smp;

/* lan device */
struct i40e_device {
	struct list_head list;
	struct i40e_pf *pf;
};

/**
 * i40e_info_nvm_ver - format the NVM version string
 * @hw: ptr to the hardware info
 * @buf: string buffer to store
 * @len: buffer size
 *
 * Formats NVM version string as:
 * <gen>.<snap>.<release> when eetrackid == I40E_OEM_EETRACK_ID
 * <nvm_major>.<nvm_minor> otherwise
 **/
static inline void i40e_info_nvm_ver(struct i40e_hw *hw, char *buf, size_t len)
{
	struct i40e_nvm_info *nvm = &hw->nvm;

	if (nvm->eetrack == I40E_OEM_EETRACK_ID) {
		u32 full_ver = nvm->oem_ver;
		u8 gen, snap;
		u16 release;

		gen = FIELD_GET(I40E_OEM_GEN_MASK, full_ver);
		snap = FIELD_GET(I40E_OEM_SNAP_MASK, full_ver);
		release = FIELD_GET(I40E_OEM_RELEASE_MASK, full_ver);
		snprintf(buf, len, "%x.%x.%x", gen, snap, release);
	} else {
		u8 major, minor;

		major = FIELD_GET(I40E_NVM_VERSION_HI_MASK, nvm->version);
		minor = FIELD_GET(I40E_NVM_VERSION_LO_MASK, nvm->version);
		snprintf(buf, len, "%x.%02x", major, minor);
	}
}

/**
 * i40e_info_eetrack - format the EETrackID string
 * @hw: ptr to the hardware info
 * @buf: string buffer to store
 * @len: buffer size
 *
 * Returns hexadecimally formated EETrackID if it is
 * different from I40E_OEM_EETRACK_ID or empty string.
 **/
static inline void i40e_info_eetrack(struct i40e_hw *hw, char *buf, size_t len)
{
	struct i40e_nvm_info *nvm = &hw->nvm;

	buf[0] = '\0';
	if (nvm->eetrack != I40E_OEM_EETRACK_ID)
		snprintf(buf, len, "0x%08x", nvm->eetrack);
}

/**
 * i40e_info_civd_ver - format the NVM version strings
 * @hw: ptr to the hardware info
 * @buf: string buffer to store
 * @len: buffer size
 *
 * Returns formated combo image version if adapter's EETrackID is
 * different from I40E_OEM_EETRACK_ID or empty string.
 **/
static inline void i40e_info_civd_ver(struct i40e_hw *hw, char *buf, size_t len)
{
	struct i40e_nvm_info *nvm = &hw->nvm;

	buf[0] = '\0';
	if (nvm->eetrack != I40E_OEM_EETRACK_ID) {
		u32 full_ver = nvm->oem_ver;
		u8 major, minor;
		u16 build;

		major = FIELD_GET(I40E_OEM_VER_MASK, full_ver);
		build = FIELD_GET(I40E_OEM_VER_BUILD_MASK, full_ver);
		minor = FIELD_GET(I40E_OEM_VER_PATCH_MASK, full_ver);
		snprintf(buf, len, "%d.%d.%d", major, build, minor);
	}
}

/**
 * i40e_nvm_version_str - format the NVM version strings
 * @hw: ptr to the hardware info
 * @buf: string buffer to store
 * @len: buffer size
 **/
static inline char *i40e_nvm_version_str(struct i40e_hw *hw, char *buf,
					 size_t len)
{
	char ver[16] = " ";

	/* Get NVM version */
	i40e_info_nvm_ver(hw, buf, len);

	/* Append EETrackID if provided */
	i40e_info_eetrack(hw, &ver[1], sizeof(ver) - 1);
	if (strlen(ver) > 1)
		strlcat(buf, ver, len);

	/* Append combo image version if provided */
	i40e_info_civd_ver(hw, &ver[1], sizeof(ver) - 1);
	if (strlen(ver) > 1)
		strlcat(buf, ver, len);

	return buf;
}

/**
 * i40e_netdev_to_pf: Retrieve the PF struct for given netdev
 * @netdev: the corresponding netdev
 *
 * Return the PF struct for the given netdev
 **/
static inline struct i40e_pf *i40e_netdev_to_pf(struct net_device *netdev)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;

	return vsi->back;
}

static inline void i40e_vsi_setup_irqhandler(struct i40e_vsi *vsi,
				irqreturn_t (*irq_handler)(int, void *))
{
	vsi->irq_handler = irq_handler;
}

/**
 * i40e_get_fd_cnt_all - get the total FD filter space available
 * @pf: pointer to the PF struct
 **/
static inline int i40e_get_fd_cnt_all(struct i40e_pf *pf)
{
	return pf->hw.fdir_shared_filter_count + pf->fdir_pf_filter_count;
}

/**
 * i40e_read_fd_input_set - reads value of flow director input set register
 * @pf: pointer to the PF struct
 * @addr: register addr
 *
 * This function reads value of flow director input set register
 * specified by 'addr' (which is specific to flow-type)
 **/
static inline u64 i40e_read_fd_input_set(struct i40e_pf *pf, u16 addr)
{
	u64 val;

	val = i40e_read_rx_ctl(&pf->hw, I40E_PRTQF_FD_INSET(addr, 1));
	val <<= 32;
	val += i40e_read_rx_ctl(&pf->hw, I40E_PRTQF_FD_INSET(addr, 0));

	return val;
}

/**
 * i40e_write_fd_input_set - writes value into flow director input set register
 * @pf: pointer to the PF struct
 * @addr: register addr
 * @val: value to be written
 *
 * This function writes specified value to the register specified by 'addr'.
 * This register is input set register based on flow-type.
 **/
static inline void i40e_write_fd_input_set(struct i40e_pf *pf,
					   u16 addr, u64 val)
{
	i40e_write_rx_ctl(&pf->hw, I40E_PRTQF_FD_INSET(addr, 1),
			  (u32)(val >> 32));
	i40e_write_rx_ctl(&pf->hw, I40E_PRTQF_FD_INSET(addr, 0),
			  (u32)(val & 0xFFFFFFFFULL));
}

/**
 * i40e_get_pf_count - get PCI PF count.
 * @hw: pointer to a hw.
 *
 * Reports the function number of the highest PCI physical
 * function plus 1 as it is loaded from the NVM.
 *
 * Return: PCI PF count.
 **/
static inline u32 i40e_get_pf_count(struct i40e_hw *hw)
{
	return FIELD_GET(I40E_GLGEN_PCIFCNCNT_PCIPFCNT_MASK,
			 rd32(hw, I40E_GLGEN_PCIFCNCNT));
}

/* needed by i40e_ethtool.c */
int i40e_up(struct i40e_vsi *vsi);
void i40e_down(struct i40e_vsi *vsi);
extern const char i40e_driver_name[];
void i40e_do_reset_safe(struct i40e_pf *pf, u32 reset_flags);
void i40e_do_reset(struct i40e_pf *pf, u32 reset_flags, bool lock_acquired);
int i40e_config_rss(struct i40e_vsi *vsi, u8 *seed, u8 *lut, u16 lut_size);
int i40e_get_rss(struct i40e_vsi *vsi, u8 *seed, u8 *lut, u16 lut_size);
void i40e_fill_rss_lut(struct i40e_pf *pf, u8 *lut,
		       u16 rss_table_size, u16 rss_size);
struct i40e_vsi *i40e_find_vsi_from_id(struct i40e_pf *pf, u16 id);
/**
 * i40e_find_vsi_by_type - Find and return Flow Director VSI
 * @pf: PF to search for VSI
 * @type: Value indicating type of VSI we are looking for
 **/
static inline struct i40e_vsi *
i40e_find_vsi_by_type(struct i40e_pf *pf, u16 type)
{
	struct i40e_vsi *vsi;
	int i;

	i40e_pf_for_each_vsi(pf, i, vsi)
		if (vsi->type == type)
			return vsi;

	return NULL;
}
void i40e_update_stats(struct i40e_vsi *vsi);
void i40e_update_veb_stats(struct i40e_veb *veb);
void i40e_update_eth_stats(struct i40e_vsi *vsi);
struct rtnl_link_stats64 *i40e_get_vsi_stats_struct(struct i40e_vsi *vsi);
int i40e_fetch_switch_configuration(struct i40e_pf *pf,
				    bool printconfig);

int i40e_add_del_fdir(struct i40e_vsi *vsi,
		      struct i40e_fdir_filter *input, bool add);
void i40e_fdir_check_and_reenable(struct i40e_pf *pf);
u32 i40e_get_current_fd_count(struct i40e_pf *pf);
u32 i40e_get_current_atr_cnt(struct i40e_pf *pf);
u32 i40e_get_global_fd_count(struct i40e_pf *pf);
bool i40e_set_ntuple(struct i40e_pf *pf, netdev_features_t features);
void i40e_set_ethtool_ops(struct net_device *netdev);
struct i40e_mac_filter *i40e_add_filter(struct i40e_vsi *vsi,
					const u8 *macaddr, s16 vlan);
void __i40e_del_filter(struct i40e_vsi *vsi, struct i40e_mac_filter *f);
int i40e_sync_vsi_filters(struct i40e_vsi *vsi);
struct i40e_vsi *i40e_vsi_setup(struct i40e_pf *pf, u8 type,
				u16 uplink, u32 param1);
int i40e_vsi_release(struct i40e_vsi *vsi);
void i40e_service_event_schedule(struct i40e_pf *pf);
void i40e_notify_client_of_vf_msg(struct i40e_vsi *vsi, u32 vf_id,
				  u8 *msg, u16 len);

int i40e_control_wait_tx_q(int seid, struct i40e_pf *pf, int pf_q, bool is_xdp,
			   bool enable);
int i40e_control_wait_rx_q(struct i40e_pf *pf, int pf_q, bool enable);
int i40e_vsi_start_rings(struct i40e_vsi *vsi);
void i40e_vsi_stop_rings(struct i40e_vsi *vsi);
void i40e_vsi_stop_rings_no_wait(struct  i40e_vsi *vsi);
int i40e_vsi_wait_queues_disabled(struct i40e_vsi *vsi);
int i40e_reconfig_rss_queues(struct i40e_pf *pf, int queue_count);
struct i40e_veb *i40e_veb_setup(struct i40e_pf *pf, u16 uplink_seid,
				u16 downlink_seid, u8 enabled_tc);
void i40e_veb_release(struct i40e_veb *veb);

int i40e_veb_config_tc(struct i40e_veb *veb, u8 enabled_tc);
int i40e_vsi_add_pvid(struct i40e_vsi *vsi, u16 vid);
void i40e_vsi_remove_pvid(struct i40e_vsi *vsi);
void i40e_vsi_reset_stats(struct i40e_vsi *vsi);
void i40e_pf_reset_stats(struct i40e_pf *pf);
#ifdef CONFIG_DEBUG_FS
void i40e_dbg_pf_init(struct i40e_pf *pf);
void i40e_dbg_pf_exit(struct i40e_pf *pf);
void i40e_dbg_init(void);
void i40e_dbg_exit(void);
#else
static inline void i40e_dbg_pf_init(struct i40e_pf *pf) {}
static inline void i40e_dbg_pf_exit(struct i40e_pf *pf) {}
static inline void i40e_dbg_init(void) {}
static inline void i40e_dbg_exit(void) {}
#endif /* CONFIG_DEBUG_FS*/
/* needed by client drivers */
int i40e_lan_add_device(struct i40e_pf *pf);
int i40e_lan_del_device(struct i40e_pf *pf);
void i40e_client_subtask(struct i40e_pf *pf);
void i40e_notify_client_of_l2_param_changes(struct i40e_pf *pf);
void i40e_notify_client_of_netdev_close(struct i40e_pf *pf, bool reset);
void i40e_notify_client_of_vf_enable(struct i40e_pf *pf, u32 num_vfs);
void i40e_notify_client_of_vf_reset(struct i40e_pf *pf, u32 vf_id);
void i40e_client_update_msix_info(struct i40e_pf *pf);
int i40e_vf_client_capable(struct i40e_pf *pf, u32 vf_id);
/**
 * i40e_irq_dynamic_enable - Enable default interrupt generation settings
 * @vsi: pointer to a vsi
 * @vector: enable a particular Hw Interrupt vector, without base_vector
 **/
static inline void i40e_irq_dynamic_enable(struct i40e_vsi *vsi, int vector)
{
	struct i40e_pf *pf = vsi->back;
	struct i40e_hw *hw = &pf->hw;
	u32 val;

	val = I40E_PFINT_DYN_CTLN_INTENA_MASK |
	      I40E_PFINT_DYN_CTLN_CLEARPBA_MASK |
	      (I40E_ITR_NONE << I40E_PFINT_DYN_CTLN_ITR_INDX_SHIFT);
	wr32(hw, I40E_PFINT_DYN_CTLN(vector + vsi->base_vector - 1), val);
	/* skip the flush */
}

void i40e_irq_dynamic_disable_icr0(struct i40e_pf *pf);
void i40e_irq_dynamic_enable_icr0(struct i40e_pf *pf);
int i40e_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd);
int i40e_open(struct net_device *netdev);
int i40e_close(struct net_device *netdev);
int i40e_vsi_open(struct i40e_vsi *vsi);
void i40e_vlan_stripping_disable(struct i40e_vsi *vsi);
int i40e_add_vlan_all_mac(struct i40e_vsi *vsi, s16 vid);
int i40e_vsi_add_vlan(struct i40e_vsi *vsi, u16 vid);
void i40e_rm_vlan_all_mac(struct i40e_vsi *vsi, s16 vid);
void i40e_vsi_kill_vlan(struct i40e_vsi *vsi, u16 vid);
struct i40e_mac_filter *i40e_add_mac_filter(struct i40e_vsi *vsi,
					    const u8 *macaddr);
int i40e_del_mac_filter(struct i40e_vsi *vsi, const u8 *macaddr);
bool i40e_is_vsi_in_vlan(struct i40e_vsi *vsi);
int i40e_count_filters(struct i40e_vsi *vsi);
struct i40e_mac_filter *i40e_find_mac(struct i40e_vsi *vsi, const u8 *macaddr);
void i40e_vlan_stripping_enable(struct i40e_vsi *vsi);
static inline bool i40e_is_sw_dcb(struct i40e_pf *pf)
{
	return test_bit(I40E_FLAG_FW_LLDP_DIS, pf->flags);
}

#ifdef CONFIG_I40E_DCB
void i40e_dcbnl_flush_apps(struct i40e_pf *pf,
			   struct i40e_dcbx_config *old_cfg,
			   struct i40e_dcbx_config *new_cfg);
void i40e_dcbnl_set_all(struct i40e_vsi *vsi);
void i40e_dcbnl_setup(struct i40e_vsi *vsi);
bool i40e_dcb_need_reconfig(struct i40e_pf *pf,
			    struct i40e_dcbx_config *old_cfg,
			    struct i40e_dcbx_config *new_cfg);
int i40e_hw_dcb_config(struct i40e_pf *pf, struct i40e_dcbx_config *new_cfg);
int i40e_dcb_sw_default_config(struct i40e_pf *pf);
#endif /* CONFIG_I40E_DCB */
void i40e_ptp_rx_hang(struct i40e_pf *pf);
void i40e_ptp_tx_hang(struct i40e_pf *pf);
void i40e_ptp_tx_hwtstamp(struct i40e_pf *pf);
void i40e_ptp_rx_hwtstamp(struct i40e_pf *pf, struct sk_buff *skb, u8 index);
void i40e_ptp_set_increment(struct i40e_pf *pf);
int i40e_ptp_set_ts_config(struct i40e_pf *pf, struct ifreq *ifr);
int i40e_ptp_get_ts_config(struct i40e_pf *pf, struct ifreq *ifr);
void i40e_ptp_save_hw_time(struct i40e_pf *pf);
void i40e_ptp_restore_hw_time(struct i40e_pf *pf);
void i40e_ptp_init(struct i40e_pf *pf);
void i40e_ptp_stop(struct i40e_pf *pf);
int i40e_ptp_alloc_pins(struct i40e_pf *pf);
int i40e_update_adq_vsi_queues(struct i40e_vsi *vsi, int vsi_offset);
int i40e_is_vsi_uplink_mode_veb(struct i40e_vsi *vsi);
int i40e_get_partition_bw_setting(struct i40e_pf *pf);
int i40e_set_partition_bw_setting(struct i40e_pf *pf);
void i40e_print_link_message(struct i40e_vsi *vsi, bool isup);

void i40e_set_fec_in_flags(u8 fec_cfg, unsigned long *flags);

static inline bool i40e_enabled_xdp_vsi(struct i40e_vsi *vsi)
{
	return !!READ_ONCE(vsi->xdp_prog);
}

int i40e_create_queue_channel(struct i40e_vsi *vsi, struct i40e_channel *ch);
int i40e_set_bw_limit(struct i40e_vsi *vsi, u16 seid, u64 max_tx_rate);
int i40e_add_del_cloud_filter(struct i40e_vsi *vsi,
			      struct i40e_cloud_filter *filter,
			      bool add);
int i40e_add_del_cloud_filter_big_buf(struct i40e_vsi *vsi,
				      struct i40e_cloud_filter *filter,
				      bool add);

/**
 * i40e_is_tc_mqprio_enabled - check if TC MQPRIO is enabled on PF
 * @pf: pointer to a pf.
 *
 * Check and return state of flag I40E_FLAG_TC_MQPRIO.
 *
 * Return: true/false if I40E_FLAG_TC_MQPRIO is set or not
 **/
static inline bool i40e_is_tc_mqprio_enabled(struct i40e_pf *pf)
{
	return test_bit(I40E_FLAG_TC_MQPRIO_ENA, pf->flags);
}

/**
 * i40e_hw_to_pf - get pf pointer from the hardware structure
 * @hw: pointer to the device HW structure
 **/
static inline struct i40e_pf *i40e_hw_to_pf(struct i40e_hw *hw)
{
	return container_of(hw, struct i40e_pf, hw);
}

struct device *i40e_hw_to_dev(struct i40e_hw *hw);

/**
 * i40e_pf_get_vsi_by_seid - find VSI by SEID
 * @pf: pointer to a PF
 * @seid: SEID of the VSI
 **/
static inline struct i40e_vsi *
i40e_pf_get_vsi_by_seid(struct i40e_pf *pf, u16 seid)
{
	struct i40e_vsi *vsi;
	int i;

	i40e_pf_for_each_vsi(pf, i, vsi)
		if (vsi->seid == seid)
			return vsi;

	return NULL;
}

/**
 * i40e_pf_get_main_vsi - get pointer to main VSI
 * @pf: pointer to a PF
 *
 * Return: pointer to main VSI or NULL if it does not exist
 **/
static inline struct i40e_vsi *i40e_pf_get_main_vsi(struct i40e_pf *pf)
{
	return (pf->lan_vsi != I40E_NO_VSI) ? pf->vsi[pf->lan_vsi] : NULL;
}

/**
 * i40e_pf_get_veb_by_seid - find VEB by SEID
 * @pf: pointer to a PF
 * @seid: SEID of the VSI
 **/
static inline struct i40e_veb *
i40e_pf_get_veb_by_seid(struct i40e_pf *pf, u16 seid)
{
	struct i40e_veb *veb;
	int i;

	i40e_pf_for_each_veb(pf, i, veb)
		if (veb->seid == seid)
			return veb;

	return NULL;
}

/**
 * i40e_pf_get_main_veb - get pointer to main VEB
 * @pf: pointer to a PF
 *
 * Return: pointer to main VEB or NULL if it does not exist
 **/
static inline struct i40e_veb *i40e_pf_get_main_veb(struct i40e_pf *pf)
{
	return (pf->lan_veb != I40E_NO_VEB) ? pf->veb[pf->lan_veb] : NULL;
}

#endif /* _I40E_H_ */
