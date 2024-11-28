/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @brief File containing RX data path specific function definitions for the
 * FMAC IF Layer of the Wi-Fi driver.
 */

#include "hal_api.h"
#include "fmac_rx.h"
#include "fmac_util.h"
#include "fmac_promisc.h"

static enum nrf_wifi_status
nrf_wifi_fmac_map_desc_to_pool(struct nrf_wifi_fmac_dev_ctx *fmac_dev_ctx,
			       unsigned int desc_id,
			       struct nrf_wifi_fmac_rx_pool_map_info *pool_info)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct nrf_wifi_fmac_dev_ctx_def *def_dev_ctx = NULL;
	struct nrf_wifi_fmac_priv_def *def_priv = NULL;
	unsigned int pool_id = 0;

	def_dev_ctx = wifi_dev_priv(fmac_dev_ctx);
	def_priv = wifi_fmac_priv(fmac_dev_ctx->fpriv);

	for (pool_id = 0; pool_id < MAX_NUM_OF_RX_QUEUES; pool_id++) {
		if ((desc_id >= def_priv->rx_desc[pool_id]) &&
		    (desc_id < (def_priv->rx_desc[pool_id] +
				def_priv->rx_buf_pools[pool_id].num_bufs))) {
			pool_info->pool_id = pool_id;
			pool_info->buf_id = (desc_id - def_priv->rx_desc[pool_id]);
			status = NRF_WIFI_STATUS_SUCCESS;
			goto out;
		}
	}
out:
	return status;
}


enum nrf_wifi_status nrf_wifi_fmac_rx_cmd_send(struct nrf_wifi_fmac_dev_ctx *fmac_dev_ctx,
					       enum nrf_wifi_fmac_rx_cmd_type cmd_type,
					       unsigned int desc_id)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct nrf_wifi_fmac_buf_map_info *rx_buf_info = NULL;
	struct host_rpu_rx_buf_info rx_cmd;
	struct nrf_wifi_fmac_rx_pool_map_info pool_info;
	struct nrf_wifi_fmac_dev_ctx_def *def_dev_ctx = NULL;
	struct nrf_wifi_fmac_priv_def *def_priv = NULL;
	unsigned long nwb = 0;
	unsigned long nwb_data = 0;
	unsigned long phy_addr = 0;
	unsigned int buf_len = 0;

	def_dev_ctx = wifi_dev_priv(fmac_dev_ctx);
	def_priv = wifi_fmac_priv(fmac_dev_ctx->fpriv);

	status = nrf_wifi_fmac_map_desc_to_pool(fmac_dev_ctx,
						desc_id,
						&pool_info);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: nrf_wifi_fmac_map_desc_to_pool failed",
				      __func__);
		goto out;
	}

	rx_buf_info = &def_dev_ctx->rx_buf_info[desc_id];

	buf_len = def_priv->rx_buf_pools[pool_info.pool_id].buf_sz + RX_BUF_HEADROOM;

	if (cmd_type == NRF_WIFI_FMAC_RX_CMD_TYPE_INIT) {
		if (rx_buf_info->mapped) {
			nrf_wifi_osal_log_err("%s: RX init called for mapped RX buffer(%d)",
					      __func__,
					      desc_id);
			status = NRF_WIFI_STATUS_FAIL;
			goto out;
		}

		nwb = (unsigned long)nrf_wifi_osal_nbuf_alloc(buf_len);

		if (!nwb) {
			nrf_wifi_osal_log_err("%s: No space for allocating RX buffer",
					      __func__);
			status = NRF_WIFI_STATUS_FAIL;
			goto out;
		}

		nwb_data = (unsigned long)nrf_wifi_osal_nbuf_data_get((void *)nwb);

		*(unsigned int *)(nwb_data) = desc_id;

		phy_addr = nrf_wifi_hal_buf_map_rx(fmac_dev_ctx->hal_dev_ctx,
						   nwb_data,
						   buf_len,
						   pool_info.pool_id,
						   pool_info.buf_id);

		if (!phy_addr) {
			nrf_wifi_osal_log_err("%s: nrf_wifi_hal_buf_map_rx failed",
					      __func__);
			status = NRF_WIFI_STATUS_FAIL;
			goto out;
		}

		rx_buf_info->nwb = nwb;
		rx_buf_info->mapped = true;

		nrf_wifi_osal_mem_set(&rx_cmd,
				      0x0,
				      sizeof(rx_cmd));

		rx_cmd.addr = (unsigned int)phy_addr;

		status = nrf_wifi_hal_data_cmd_send(fmac_dev_ctx->hal_dev_ctx,
						    NRF_WIFI_HAL_MSG_TYPE_CMD_DATA_RX,
						    &rx_cmd,
						    sizeof(rx_cmd),
						    desc_id,
						    pool_info.pool_id);
	} else if (cmd_type == NRF_WIFI_FMAC_RX_CMD_TYPE_DEINIT) {
		/* TODO: Need to initialize a command and send it to LMAC
		 * when LMAC is capable of handling deinit command
		 */
		if (!rx_buf_info->mapped) {
			nrf_wifi_osal_log_err("%s: RX deinit called for unmapped RX buffer(%d)",
					      __func__,
					      desc_id);
			status = NRF_WIFI_STATUS_FAIL;
			goto out;
		}

		nwb_data = nrf_wifi_hal_buf_unmap_rx(fmac_dev_ctx->hal_dev_ctx,
						     0,
						     pool_info.pool_id,
						     pool_info.buf_id);

		if (!nwb_data) {
			nrf_wifi_osal_log_err("%s: nrf_wifi_hal_buf_unmap_rx failed",
					      __func__);
			goto out;
		}

		nrf_wifi_osal_nbuf_free((void *)rx_buf_info->nwb);
		rx_buf_info->nwb = 0;
		rx_buf_info->mapped = false;
		status = NRF_WIFI_STATUS_SUCCESS;
	} else {
		nrf_wifi_osal_log_err("%s: Unknown cmd_type (%d)",
				      __func__,
				      cmd_type);
		goto out;
	}
out:
	return status;
}


#ifdef NRF70_RX_WQ_ENABLED
void nrf_wifi_fmac_rx_tasklet(void *data)
{
	struct nrf_wifi_fmac_dev_ctx *fmac_dev_ctx = (struct nrf_wifi_fmac_dev_ctx *)data;
	struct nrf_wifi_rx_buff *config = NULL;
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct nrf_wifi_fmac_dev_ctx_def *def_dev_ctx = NULL;
	enum NRF_WIFI_HAL_STATUS hal_status;

	nrf_wifi_hal_lock_rx(fmac_dev_ctx->hal_dev_ctx);
	hal_status = nrf_wifi_hal_status_unlocked(fmac_dev_ctx->hal_dev_ctx);
	if (hal_status != NRF_WIFI_HAL_STATUS_ENABLED) {
		goto out;
	}

	def_dev_ctx = wifi_dev_priv(fmac_dev_ctx);

	config = (struct nrf_wifi_rx_buff *)nrf_wifi_utils_q_dequeue(
		def_dev_ctx->rx_tasklet_event_q);

	if (!config) {
		nrf_wifi_osal_log_err("%s: No RX config available",
				      __func__);
		goto out;
	}

	status = nrf_wifi_fmac_rx_event_process(fmac_dev_ctx,
						config);

	if (status != NRF_WIFI_STATUS_SUCCESS) {
		nrf_wifi_osal_log_err("%s: nrf_wifi_fmac_rx_event_process failed",
				      __func__);
		goto out;
	}
out:
	nrf_wifi_osal_mem_free(config);
	nrf_wifi_hal_unlock_rx(fmac_dev_ctx->hal_dev_ctx);
}
#endif /* NRF70_RX_WQ_ENABLED */

enum nrf_wifi_status nrf_wifi_fmac_rx_event_process(struct nrf_wifi_fmac_dev_ctx *fmac_dev_ctx,
						    struct nrf_wifi_rx_buff *config)
{
	enum nrf_wifi_status status = NRF_WIFI_STATUS_FAIL;
	struct nrf_wifi_fmac_vif_ctx *vif_ctx = NULL;
	struct nrf_wifi_fmac_buf_map_info *rx_buf_info = NULL;
	struct nrf_wifi_fmac_rx_pool_map_info pool_info;
#if defined(NRF70_RAW_DATA_RX) || defined(NRF70_PROMISC_DATA_RX)
	struct raw_rx_pkt_header raw_rx_hdr;
#if defined(NRF70_PROMISC_DATA_RX)
	unsigned short frame_control;
#endif
#endif /* NRF70_RAW_DATA_RX || NRF70_PROMISC_DATA_RX */
	void *nwb = NULL;
	void *nwb_data = NULL;
	unsigned int num_pkts = 0;
	unsigned int desc_id = 0;
	unsigned int i = 0;
	unsigned int pkt_len = 0;
#ifdef NRF70_STA_MODE
	struct nrf_wifi_fmac_ieee80211_hdr hdr;
	unsigned short eth_type = 0;
	unsigned int size = 0;
#endif /* NRF70_STA_MODE */
	struct nrf_wifi_fmac_dev_ctx_def *def_dev_ctx = NULL;
	struct nrf_wifi_fmac_priv_def *def_priv = NULL;

	def_dev_ctx = wifi_dev_priv(fmac_dev_ctx);
	def_priv = wifi_fmac_priv(fmac_dev_ctx->fpriv);

	vif_ctx = def_dev_ctx->vif_ctx[config->wdev_id];

#ifdef NRF70_STA_MODE
	if (config->rx_pkt_type != NRF_WIFI_RAW_RX_PKT) {
		def_priv->callbk_fns.process_rssi_from_rx(vif_ctx->os_vif_ctx,
							  config->signal);
	}
#endif /* NRF70_STA_MODE */
	num_pkts = config->rx_pkt_cnt;

	for (i = 0; i < num_pkts; i++) {
		desc_id = config->rx_buff_info[i].descriptor_id;
		pkt_len = config->rx_buff_info[i].rx_pkt_len;

		if (desc_id >= def_priv->num_rx_bufs) {
			nrf_wifi_osal_log_err("%s: Invalid desc_id %d",
					      __func__,
					      desc_id);
			status = NRF_WIFI_STATUS_FAIL;
			continue;
		}

		status = nrf_wifi_fmac_map_desc_to_pool(fmac_dev_ctx,
							desc_id,
							&pool_info);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: nrf_wifi_fmac_map_desc_to_pool failed",
					      __func__);
			status = NRF_WIFI_STATUS_FAIL;
			continue;
		}

		nwb_data = (void *)nrf_wifi_hal_buf_unmap_rx(fmac_dev_ctx->hal_dev_ctx,
							     pkt_len,
							     pool_info.pool_id,
							     pool_info.buf_id);

		if (!nwb_data) {
			nrf_wifi_osal_log_err("%s: nrf_wifi_hal_buf_unmap_rx failed",
					      __func__);
			status = NRF_WIFI_STATUS_FAIL;
			continue;
		}

		rx_buf_info = &def_dev_ctx->rx_buf_info[desc_id];
		nwb = (void *)rx_buf_info->nwb;

		nrf_wifi_osal_nbuf_data_put(nwb,
					    pkt_len + RX_BUF_HEADROOM);
		nrf_wifi_osal_nbuf_data_pull(nwb,
					     RX_BUF_HEADROOM);
		nwb_data = nrf_wifi_osal_nbuf_data_get(nwb);

		rx_buf_info->nwb = 0;
		rx_buf_info->mapped = false;

#ifdef NRF70_PROMISC_DATA_RX
		nrf_wifi_osal_mem_cpy(&frame_control,
				      nwb_data,
				      sizeof(unsigned short));
#endif

		if (config->rx_pkt_type == NRF_WIFI_RX_PKT_DATA) {
#ifdef NRF70_PROMISC_DATA_RX
			if (vif_ctx->promisc_mode) {
				raw_rx_hdr.frequency = config->frequency;
				raw_rx_hdr.signal = config->signal;
				raw_rx_hdr.rate_flags = config->rate_flags;
				raw_rx_hdr.rate = config->rate;
				if (nrf_wifi_util_check_filt_setting(vif_ctx, &frame_control)) {
					def_priv->callbk_fns.sniffer_callbk_fn(vif_ctx->os_vif_ctx,
									       nwb,
									       &raw_rx_hdr,
									       false);
				}
			}
#endif
#ifdef NRF70_STA_MODE
			switch (config->rx_buff_info[i].pkt_type) {
			case PKT_TYPE_MPDU:
				nrf_wifi_osal_mem_cpy(&hdr,
						      nwb_data,
						      sizeof(struct nrf_wifi_fmac_ieee80211_hdr));

				eth_type = nrf_wifi_util_rx_get_eth_type(fmac_dev_ctx,
									 ((char *)nwb_data +
									  config->mac_header_len));

				size = config->mac_header_len +
					nrf_wifi_util_get_skip_header_bytes(eth_type);

				/* Remove hdr len and llc header/length */
				nrf_wifi_osal_nbuf_data_pull(nwb,
							     size);

				nrf_wifi_util_convert_to_eth(fmac_dev_ctx,
							     nwb,
							     &hdr,
							     eth_type);
				break;
			case PKT_TYPE_MSDU_WITH_MAC:
				nrf_wifi_osal_nbuf_data_pull(nwb,
							     config->mac_header_len);

				nrf_wifi_util_rx_convert_amsdu_to_eth(fmac_dev_ctx,
								      nwb);
				break;
			case PKT_TYPE_MSDU:
				nrf_wifi_util_rx_convert_amsdu_to_eth(fmac_dev_ctx,
								      nwb);
				break;
			default:
				nrf_wifi_osal_log_err("%s: Invalid pkt_type=%d",
						      __func__,
						      (config->rx_buff_info[i].pkt_type));
				status = NRF_WIFI_STATUS_FAIL;
				continue;
			}
			def_priv->callbk_fns.rx_frm_callbk_fn(vif_ctx->os_vif_ctx,
									 nwb);
#endif /* NRF70_STA_MODE */
		} else if (config->rx_pkt_type == NRF_WIFI_RX_PKT_BCN_PRB_RSP) {
#ifdef WIFI_MGMT_RAW_SCAN_RESULTS
			def_priv->callbk_fns.rx_bcn_prb_resp_callbk_fn(
							vif_ctx->os_vif_ctx,
							nwb,
							config->frequency,
							config->signal);
#endif /* WIFI_MGMT_RAW_SCAN_RESULTS */
			nrf_wifi_osal_nbuf_free(nwb);
#ifdef NRF_WIFI_MGMT_BUFF_OFFLOAD
			continue;
#endif /* NRF_WIFI_MGMT_BUFF_OFFLOAD */
		}
#if defined(NRF70_RAW_DATA_RX) || defined(NRF70_PROMISC_DATA_RX)
		else if (config->rx_pkt_type == NRF_WIFI_RAW_RX_PKT) {
			raw_rx_hdr.frequency = config->frequency;
			raw_rx_hdr.signal = config->signal;
			raw_rx_hdr.rate_flags = config->rate_flags;
			raw_rx_hdr.rate = config->rate;
#if defined(NRF70_PROMISC_DATA_RX)
			if (nrf_wifi_util_check_filt_setting(vif_ctx, &frame_control))
#endif
			{
				def_priv->callbk_fns.sniffer_callbk_fn(vif_ctx->os_vif_ctx,
								       nwb,
								       &raw_rx_hdr,
								       true);
			}
#if defined(NRF70_PROMISC_DATA_RX)
			/**
			 * In the case of Monitor mode, the sniffer callback function
			 * will free the packet. For promiscuous mode, if the packet
			 * is not meant to be sent up the stack, the packet needs
			 * to be freed here.
			 */
			else {
				nrf_wifi_osal_nbuf_free(nwb);
			}
#endif
		}
#endif /* NRF70_RAW_DATA_RX || NRF70_PROMISC_DATA_RX */
		else {
			nrf_wifi_osal_log_err("%s: Invalid frame type received %d",
					      __func__,
					      config->rx_pkt_type);
			status = NRF_WIFI_STATUS_FAIL;
			nrf_wifi_osal_nbuf_free(nwb);
			continue;
		}

		status = nrf_wifi_fmac_rx_cmd_send(fmac_dev_ctx,
						   NRF_WIFI_FMAC_RX_CMD_TYPE_INIT,
						   desc_id);

		if (status != NRF_WIFI_STATUS_SUCCESS) {
			nrf_wifi_osal_log_err("%s: nrf_wifi_fmac_rx_cmd_send failed",
					      __func__);
			continue;
		}
	}

	/* A single failure returns failure for the entire event */
	return status;
}
