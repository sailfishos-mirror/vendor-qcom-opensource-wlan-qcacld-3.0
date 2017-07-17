/*
* Copyright (c) 2017 The Linux Foundation. All rights reserved.
*
* Permission to use, copy, modify, and/or distribute this software for
* any purpose with or without fee is hereby granted, provided that the
* above copyright notice and this permission notice appear in all
* copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
* WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
* AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
* DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
* PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
* TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
* PERFORMANCE OF THIS SOFTWARE.
*/
/**
 * DOC: Implements static configuration on vdev attach
 */

#include "wlan_pmo_static_config.h"
#include "wlan_pmo_tgt_api.h"
#include "wlan_pmo_main.h"
#include "wlan_pmo_wow.h"
#include "wlan_pmo_obj_mgmt_public_struct.h"

static const uint8_t arp_ptrn[] = {0x08, 0x06};
static const uint8_t arp_mask[] = {0xff, 0xff};
static const uint8_t ns_ptrn[] = {0x86, 0xDD};
static const uint8_t discvr_ptrn[] = {0xe0, 0x00, 0x00, 0xf8};
static const uint8_t discvr_mask[] = {0xf0, 0x00, 0x00, 0xf8};

void pmo_register_wow_wakeup_events(struct wlan_objmgr_vdev *vdev)
{
	uint32_t event_bitmap[PMO_WOW_MAX_EVENT_BM_LEN] = {0};
	uint8_t vdev_id;
	enum tQDF_ADAPTER_MODE  vdev_opmode;
	const char *iface_type;

	vdev_opmode = pmo_get_vdev_opmode(vdev);
	vdev_id = pmo_vdev_get_id(vdev);
	pmo_info("vdev_opmode %d vdev_id %d", vdev_opmode, vdev_id);

	switch (vdev_opmode) {
	case QDF_P2P_CLIENT_MODE:
	case QDF_P2P_DEVICE_MODE:
	case QDF_OCB_MODE:
	case QDF_STA_MODE:
	case QDF_MONITOR_MODE:
		iface_type = "STA";
		pmo_set_sta_wow_bitmask(event_bitmap, PMO_WOW_MAX_EVENT_BM_LEN);
		break;

	case QDF_IBSS_MODE:
		iface_type = "IBSS";
		pmo_set_sta_wow_bitmask(event_bitmap, PMO_WOW_MAX_EVENT_BM_LEN);
		pmo_set_wow_event_bitmap(WOW_BEACON_EVENT,
					 PMO_WOW_MAX_EVENT_BM_LEN,
					 event_bitmap);
		break;

	case QDF_P2P_GO_MODE:
	case QDF_SAP_MODE:
		iface_type = "SAP";
		pmo_set_sap_wow_bitmask(event_bitmap, PMO_WOW_MAX_EVENT_BM_LEN);
		break;

	case QDF_NDI_MODE:
#ifdef WLAN_FEATURE_NAN_DATAPATH
		iface_type = "NAN";
		pmo_set_wow_event_bitmap(WOW_NAN_DATA_EVENT,
					 PMO_WOW_MAX_EVENT_BM_LEN,
					 event_bitmap);
#endif
		break;

	default:
		pmo_err("Skipping wake event configuration for vdev_opmode %d",
			vdev_opmode);
		return;
	}

	pmo_info("Selected %s wake event mask 0x%x%x%x%x, vdev %d",
		 iface_type, event_bitmap[0], event_bitmap[1],
		 event_bitmap[2], event_bitmap[3], vdev_id);

	pmo_tgt_enable_wow_wakeup_event(vdev, event_bitmap);
}

/**
 * pmo_configure_wow_ap() - set WOW patterns in ap mode
 * @vdev: objmgr vdev handle
 *
 * Configures default WOW pattern for the given vdev_id which is in AP mode.
 *
 * Return: QDF status
 */
static QDF_STATUS pmo_configure_wow_ap(struct wlan_objmgr_vdev *vdev)
{
	QDF_STATUS ret;
	uint8_t arp_offset = 20;
	uint8_t mac_mask[PMO_80211_ADDR_LEN];
	struct pmo_vdev_priv_obj *vdev_ctx;

	vdev_ctx = pmo_vdev_get_priv(vdev);

	/*
	 * Setup unicast pkt pattern
	 * WoW pattern id should be unique for each vdev
	 * WoW pattern id can be same on 2 different VDEVs
	 */
	qdf_mem_set(&mac_mask, PMO_80211_ADDR_LEN, 0xFF);
	ret = pmo_tgt_send_wow_patterns_to_fw(vdev,
			pmo_get_and_increment_wow_default_ptrn(vdev_ctx),
			wlan_vdev_mlme_get_macaddr(vdev),
			PMO_80211_ADDR_LEN, 0, mac_mask,
			PMO_80211_ADDR_LEN, false);
	if (ret != QDF_STATUS_SUCCESS) {
		pmo_err("Failed to add WOW unicast pattern ret %d", ret);
		return ret;
	}

	/*
	 * Setup all ARP pkt pattern. This is dummy pattern hence the length
	 * is zero. Pattern ID should be unique per vdev.
	 */
	ret = pmo_tgt_send_wow_patterns_to_fw(vdev,
			pmo_get_and_increment_wow_default_ptrn(vdev_ctx),
			arp_ptrn, 0, arp_offset, arp_mask, 0, false);
	if (ret != QDF_STATUS_SUCCESS)
		pmo_err("Failed to add WOW ARP pattern ret %d", ret);

	return ret;
}

/**
  * pmo_configure_mc_ssdp() - API to configure SSDP address as MC list
  *@vdev: objmgr vdev handle.
  *
  * SSDP address 239.255.255.250 is converted to Multicast Mac address
  * and configure it to FW. Firmware will apply this pattern on the incoming
  * packets to filter them out during chatter/wow mode.
  *
  * Return: Success/Failure
  */
static QDF_STATUS pmo_configure_mc_ssdp(
	struct wlan_objmgr_vdev *vdev)
{
	struct wlan_objmgr_psoc *psoc;
	const uint8_t ssdp_addr[QDF_MAC_ADDR_SIZE] = {
		0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfa };
	struct qdf_mac_addr multicast_addr;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	psoc = pmo_vdev_get_psoc(vdev);

	qdf_mem_copy(&multicast_addr.bytes, &ssdp_addr, QDF_MAC_ADDR_SIZE);
	status = pmo_tgt_set_mc_filter_req(vdev,
					  multicast_addr);
	if (status != QDF_STATUS_SUCCESS)
		pmo_err("unable to set ssdp as mc addr list filter");

	return status;
}

/**
 * pmo_configure_wow_ssdp() - API to configure WoW SSDP
 *@vdev: objmgr vdev handle
 *
 * API to configure SSDP pattern as WoW pattern
 *
 * Return: Success/Failure
 */
static QDF_STATUS pmo_configure_wow_ssdp(
			struct wlan_objmgr_vdev *vdev)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	uint8_t discvr_offset = 30;
	struct pmo_vdev_priv_obj *vdev_ctx;

	vdev_ctx = pmo_vdev_get_priv(vdev);

	/*
	 * WoW pattern ID should be unique for each vdev
	 * Different WoW patterns can use same pattern ID
	 */
	 status = pmo_tgt_send_wow_patterns_to_fw(vdev,
			pmo_get_and_increment_wow_default_ptrn(vdev_ctx),
			discvr_ptrn, sizeof(discvr_ptrn), discvr_offset,
			discvr_mask, sizeof(discvr_ptrn), false);

	if (status != QDF_STATUS_SUCCESS)
		pmo_err("Failed to add WOW mDNS/SSDP/LLMNR pattern");

	return status;
}

/**
 * pmo_configure_ssdp() - API to Configure SSDP pattern to FW
 *@vdev: objmgr vdev handle
 *
 * Setup multicast pattern for mDNS 224.0.0.251, SSDP 239.255.255.250 and LLMNR
 * 224.0.0.252
 *
 * Return: Success/Failure.
 */
static QDF_STATUS pmo_configure_ssdp(struct wlan_objmgr_vdev *vdev)
{
	struct pmo_vdev_priv_obj *vdev_ctx;

	vdev_ctx = pmo_vdev_get_priv(vdev);

	if (!vdev_ctx->pmo_psoc_ctx->psoc_cfg.ssdp) {
		pmo_err("mDNS, SSDP, LLMNR patterns are disabled from ini");
		return QDF_STATUS_SUCCESS;
	}

	pmo_debug("enable_mc_list:%d",
		vdev_ctx->pmo_psoc_ctx->psoc_cfg.enable_mc_list);

	if (vdev_ctx->pmo_psoc_ctx->psoc_cfg.enable_mc_list)
		return pmo_configure_mc_ssdp(vdev);

	return pmo_configure_wow_ssdp(vdev);
}

/**
 * pmo_configure_wow_sta() - set WOW patterns in sta mode
 * @vdev: objmgr vdev handle
 *
 * Configures default WOW pattern for the given vdev_id which is in sta mode.
 *
 * Return: QDF status
 */
static QDF_STATUS pmo_configure_wow_sta(struct wlan_objmgr_vdev *vdev)
{
	uint8_t arp_offset = 12;
	uint8_t mac_mask[PMO_80211_ADDR_LEN];
	QDF_STATUS ret = QDF_STATUS_SUCCESS;
	struct pmo_vdev_priv_obj *vdev_ctx;

	vdev_ctx = pmo_vdev_get_priv(vdev);

	qdf_mem_set(&mac_mask, PMO_80211_ADDR_LEN, 0xFF);
	/*
	 * Set up unicast wow pattern
	 * WoW pattern ID should be unique for each vdev
	 * Different WoW patterns can use same pattern ID
	 */
	ret = pmo_tgt_send_wow_patterns_to_fw(vdev,
			pmo_get_and_increment_wow_default_ptrn(vdev_ctx),
			wlan_vdev_mlme_get_macaddr(vdev),
			PMO_80211_ADDR_LEN, 0, mac_mask,
			PMO_80211_ADDR_LEN, false);
	if (ret != QDF_STATUS_SUCCESS) {
		pmo_err("Failed to add WOW unicast pattern ret %d", ret);
		return ret;
	}

	ret = pmo_configure_ssdp(vdev);
	if (ret != QDF_STATUS_SUCCESS)
		pmo_err("Failed to configure SSDP patterns to FW");

	/*
	 * when arp offload or ns offloaded is disabled
	 * from ini file, configure broad cast arp pattern
	 * to fw, so that host can wake up
	 */
	if (!vdev_ctx->pmo_psoc_ctx->psoc_cfg.arp_offload_enable) {
		/* Setup all ARP pkt pattern */
		pmo_info("ARP offload is disabled in INI enable WoW for ARP");
		ret = pmo_tgt_send_wow_patterns_to_fw(vdev,
				pmo_get_and_increment_wow_default_ptrn(
					vdev_ctx),
				arp_ptrn, sizeof(arp_ptrn), arp_offset,
				arp_mask, sizeof(arp_mask), false);
		if (ret != QDF_STATUS_SUCCESS) {
			pmo_err("Failed to add WOW ARP pattern");
			return ret;
		}
	}
	/* for NS or NDP offload packets */
	if (!vdev_ctx->pmo_psoc_ctx->psoc_cfg.ns_offload_enable_static) {
		/* Setup all NS pkt pattern */
		pmo_info("NS offload is disabled in INI enable WoW for NS");
		ret = pmo_tgt_send_wow_patterns_to_fw(vdev,
				pmo_get_and_increment_wow_default_ptrn(
					vdev_ctx),
				ns_ptrn, sizeof(arp_ptrn), arp_offset,
				arp_mask, sizeof(arp_mask), false);
		if (ret != QDF_STATUS_SUCCESS) {
			pmo_err("Failed to add WOW NS pattern");
			return ret;
		}
	}

	return ret;
}

void pmo_register_wow_default_patterns(struct wlan_objmgr_vdev *vdev)
{
	enum tQDF_ADAPTER_MODE  vdev_opmode = QDF_MAX_NO_OF_MODE;
	struct pmo_vdev_priv_obj *vdev_ctx;
	uint8_t vdev_id;
	struct pmo_psoc_priv_obj *psoc_ctx;

	vdev_ctx = pmo_vdev_get_priv(vdev);

	vdev_id = pmo_vdev_get_id(vdev);
	if (vdev_id > WLAN_UMAC_PSOC_MAX_VDEVS) {
		pmo_err("Invalid vdev id %d", vdev_id);
		return;
	}

	vdev_opmode = pmo_get_vdev_opmode(vdev);
	if (vdev_opmode == QDF_MAX_NO_OF_MODE) {
		pmo_err("Invalid vdev opmode %d", vdev_id);
		return;
	}

	if (!vdev_ctx->ptrn_match_enable) {
		pmo_err("ptrn_match is disable for vdev %d", vdev_id);
		return;
	}

	if (pmo_is_vdev_in_beaconning_mode(vdev_opmode)) {
		/* Configure SAP/GO/IBSS mode default wow patterns */
		pmo_info("Config SAP default wow patterns vdev_id %d",
			 vdev_id);
		pmo_configure_wow_ap(vdev);
	} else {
		/* Configure STA/P2P CLI mode default wow patterns */
		pmo_info("Config STA default wow patterns vdev_id %d",
			vdev_id);
		pmo_configure_wow_sta(vdev);
		psoc_ctx = vdev_ctx->pmo_psoc_ctx;
		if (psoc_ctx && psoc_ctx->psoc_cfg.ra_ratelimit_enable) {
			pmo_info("Config STA RA wow pattern vdev_id %d",
				vdev_id);
			pmo_tgt_send_ra_filter_req(vdev);
		}
	}

}

void pmo_register_action_frame_patterns(struct wlan_objmgr_vdev *vdev)
{

	struct pmo_action_wakeup_set_params cmd = {0};
	int i = 0;
	QDF_STATUS status;

	cmd.vdev_id = pmo_vdev_get_id(vdev);
	cmd.operation = pmo_action_wakeup_set;

	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP0;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP1;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP2;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP3;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP4;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP5;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP6;
	cmd.action_category_map[i++] = ALLOWED_ACTION_FRAMES_BITMAP7;

	for (i = 0; i < PMO_SUPPORTED_ACTION_CATE_ELE_LIST; i++) {
		if (i < ALLOWED_ACTION_FRAME_MAP_WORDS)
			pmo_debug("%s: %d action Wakeup pattern 0x%x in fw",
				__func__, i, cmd.action_category_map[i]);
		else
			cmd.action_category_map[i] = 0;
	}

	/*  config action frame patterns */
	status = pmo_tgt_send_action_frame_pattern_req(vdev, &cmd);
	if (status != QDF_STATUS_SUCCESS)
		pmo_err("Failed to config wow action frame map, ret %d",
			status);
}

