// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <asm/dma-iommu.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/slimbus/slimbus.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_slimbus.h>
#include <linux/timer.h>
#include <linux/msm-sps.h>
#include <soc/qcom/service-locator.h>
#include <soc/qcom/service-notifier.h>
#include <soc/qcom/subsystem_notif.h>
#include "slim-msm.h"

#define NGD_SLIM_NAME	"ngd_msm_ctrl"
#define SLIM_LA_MGR	0xFF
#define SLIM_ROOT_FREQ	24576000
#define LADDR_RETRY	5

#define NGD_BASE_V1(r)	(((r) % 2) ? 0x800 : 0xA00)
#define NGD_BASE_V2(r)	(((r) % 2) ? 0x1000 : 0x2000)
#define NGD_BASE(r, v) ((v) ? NGD_BASE_V2(r) : NGD_BASE_V1(r))
/* NGD (Non-ported Generic Device) registers */
enum ngd_reg {
	NGD_CFG		= 0x0,
	NGD_STATUS	= 0x4,
	NGD_RX_MSGQ_CFG	= 0x8,
	NGD_INT_EN	= 0x10,
	NGD_INT_STAT	= 0x14,
	NGD_INT_CLR	= 0x18,
	NGD_TX_MSG	= 0x30,
	NGD_RX_MSG	= 0x70,
	NGD_IE_STAT	= 0xF0,
	NGD_VE_STAT	= 0x100,
};

enum ngd_msg_cfg {
	NGD_CFG_ENABLE		= 1,
	NGD_CFG_RX_MSGQ_EN	= 1 << 1,
	NGD_CFG_TX_MSGQ_EN	= 1 << 2,
};

enum ngd_intr {
	NGD_INT_RECFG_DONE	= 1 << 24,
	NGD_INT_TX_NACKED_2	= 1 << 25,
	NGD_INT_MSG_BUF_CONTE	= 1 << 26,
	NGD_INT_MSG_TX_INVAL	= 1 << 27,
	NGD_INT_IE_VE_CHG	= 1 << 28,
	NGD_INT_DEV_ERR		= 1 << 29,
	NGD_INT_RX_MSG_RCVD	= 1 << 30,
	NGD_INT_TX_MSG_SENT	= 1 << 31,
};

enum ngd_offsets {
	NGD_NACKED_MC		= 0x7F00000,
	NGD_ACKED_MC		= 0xFE000,
	NGD_ERROR		= 0x1800,
	NGD_MSGQ_SUPPORT	= 0x400,
	NGD_RX_MSGQ_TIME_OUT	= 0x16,
	NGD_ENUMERATED		= 0x1,
	NGD_TX_BUSY		= 0x0,
};

enum ngd_status {
	NGD_LADDR		= 1 << 1,
};

static void ngd_slim_rx(struct msm_slim_ctrl *dev, u8 *buf);
static int ngd_slim_runtime_resume(struct device *device);
static int ngd_slim_power_up(struct msm_slim_ctrl *dev, bool mdm_restart);
static void ngd_dom_down(struct msm_slim_ctrl *dev);
static int dsp_domr_notify_cb(struct notifier_block *n, unsigned long code,
				void *_cmd);
static int ngd_slim_qmi_svc_event_init(struct msm_slim_qmi *qmi);
static void ngd_slim_qmi_svc_event_deinit(struct msm_slim_qmi *qmi);

static irqreturn_t ngd_slim_interrupt(int irq, void *d)
{
	struct msm_slim_ctrl *dev = (struct msm_slim_ctrl *)d;
	void __iomem *ngd = dev->base + NGD_BASE(dev->ctrl.nr, dev->ver);
	u32 stat = readl_relaxed(ngd + NGD_INT_STAT);
	u32 pstat;

	if ((stat & NGD_INT_MSG_BUF_CONTE) ||
		(stat & NGD_INT_MSG_TX_INVAL) || (stat & NGD_INT_DEV_ERR) ||
		(stat & NGD_INT_TX_NACKED_2)) {
		writel_relaxed(stat, ngd + NGD_INT_CLR);
		if (stat & NGD_INT_MSG_TX_INVAL)
			dev->err = -EINVAL;
		else
			dev->err = -EIO;

		SLIM_WARN(dev, "NGD interrupt error:0x%x, err:%d\n", stat,
								dev->err);
		/* Guarantee that error interrupts are cleared */
		mb();
		msm_slim_manage_tx_msgq(dev, false, NULL, dev->err);

	} else if (stat & NGD_INT_TX_MSG_SENT) {
		writel_relaxed(NGD_INT_TX_MSG_SENT, ngd + NGD_INT_CLR);
		/* Make sure interrupt is cleared */
		mb();
		msm_slim_manage_tx_msgq(dev, false, NULL, 0);
	}
	if (stat & NGD_INT_RX_MSG_RCVD) {
		u32 rx_buf[10];
		u8 len, i;

		rx_buf[0] = readl_relaxed(ngd + NGD_RX_MSG);
		len = rx_buf[0] & 0x1F;
		for (i = 1; i < ((len + 3) >> 2); i++) {
			rx_buf[i] = readl_relaxed(ngd + NGD_RX_MSG +
						(4 * i));
			SLIM_DBG(dev, "REG-RX data: %x\n", rx_buf[i]);
		}
		writel_relaxed(NGD_INT_RX_MSG_RCVD,
				ngd + NGD_INT_CLR);
		/*
		 * Guarantee that CLR bit write goes through before
		 * queuing work
		 */
		mb();
		ngd_slim_rx(dev, (u8 *)rx_buf);
	}
	if (stat & NGD_INT_RECFG_DONE) {
		writel_relaxed(NGD_INT_RECFG_DONE, ngd + NGD_INT_CLR);
		/* Guarantee RECONFIG DONE interrupt is cleared */
		mb();
		/* In satellite mode, just log the reconfig done IRQ */
		SLIM_DBG(dev, "reconfig done IRQ for NGD\n");
	}
	if (stat & NGD_INT_IE_VE_CHG) {
		writel_relaxed(NGD_INT_IE_VE_CHG, ngd + NGD_INT_CLR);
		/* Guarantee IE VE change interrupt is cleared */
		mb();
		SLIM_DBG(dev, "NGD IE VE change\n");
	}

	pstat = readl_relaxed(PGD_THIS_EE(PGD_PORT_INT_ST_EEn, dev->ver));
	if (pstat != 0)
		return msm_slim_port_irq_handler(dev, pstat);
	return IRQ_HANDLED;
}

static int ngd_slim_qmi_new_server(struct qmi_handle *hdl,
				   struct qmi_service *service)
{
	struct msm_slim_qmi *qmi =
		container_of(hdl, struct msm_slim_qmi, svc_event_hdl);
	struct msm_slim_ctrl *dev =
		container_of(qmi, struct msm_slim_ctrl, qmi);

	SLIM_INFO(dev, "Slimbus QMI new server event received\n");
	qmi->svc_info.sq_family = AF_QIPCRTR;
	qmi->svc_info.sq_node = service->node;
	qmi->svc_info.sq_port = service->port;
	complete(&dev->qmi_up);

	return 0;
}

static void ngd_slim_qmi_del_server(struct qmi_handle *hdl,
				   struct qmi_service *service)
{
	struct msm_slim_qmi *qmi =
		container_of(hdl, struct msm_slim_qmi, svc_event_hdl);
	struct msm_slim_ctrl *dev =
		container_of(qmi, struct msm_slim_ctrl, qmi);

	reinit_completion(&dev->qmi_up);
	qmi->svc_info.sq_node = 0;
	qmi->svc_info.sq_port = 0;
}

static struct qmi_ops ngd_slim_qmi_svc_event_ops = {
	.new_server = ngd_slim_qmi_new_server,
	.del_server = ngd_slim_qmi_del_server,
};

static int ngd_slim_qmi_svc_event_init(struct msm_slim_qmi *qmi)
{
	int ret = 0;

	ret = qmi_handle_init(&qmi->svc_event_hdl, 0,
				&ngd_slim_qmi_svc_event_ops, NULL);
	if (ret < 0) {
		pr_err("%s: qmi_handle_init failed: %d\n", __func__, ret);
		return ret;
	}

	ret = qmi_add_lookup(&qmi->svc_event_hdl, SLIMBUS_QMI_SVC_ID,
				SLIMBUS_QMI_SVC_V1, SLIMBUS_QMI_INS_ID);
	if (ret < 0) {
		pr_err("%s: qmi_add_lookup failed: %d\n", __func__, ret);
		qmi_handle_release(&qmi->svc_event_hdl);
	}
	return ret;
}

static void ngd_slim_qmi_svc_event_deinit(struct msm_slim_qmi *qmi)
{
	qmi_handle_release(&qmi->svc_event_hdl);
}

static void ngd_reg_ssr(struct msm_slim_ctrl *dev)
{
	int ret;
	const char *subsys_name = NULL;

	dev->dsp.dom_t = MSM_SLIM_DOM_NONE;
	ret = of_property_read_string(dev->dev->of_node,
				"qcom,subsys-name", &subsys_name);
	if (ret)
		subsys_name = "adsp";

	dev->dsp.nb.notifier_call = dsp_domr_notify_cb;
	dev->dsp.domr = subsys_notif_register_notifier(subsys_name,
							&dev->dsp.nb);
	if (IS_ERR_OR_NULL(dev->dsp.domr)) {
		dev_err(dev->dev,
			"subsys_notif_register_notifier failed %ld\n",
			PTR_ERR(dev->dsp.domr));
		return;
	}
	dev->dsp.dom_t = MSM_SLIM_DOM_SS;
	SLIM_INFO(dev, "reg-SSR with:%s, PDR not available\n",
			subsys_name);
}

static int dsp_domr_notify_cb(struct notifier_block *n, unsigned long code,
				void *_cmd)
{
	int cur = -1;
	struct msm_slim_ss *dsp = container_of(n, struct msm_slim_ss, nb);
	struct msm_slim_ctrl *dev = container_of(dsp, struct msm_slim_ctrl,
						dsp);
	struct pd_qmi_client_data *reg;

	/* Resetting the log level */
	SLIM_RST_LOGLVL(dev);
	SLIM_INFO(dev, "SLIM DSP SSR/PDR notify cb:0x%lx, type:%d\n",
			code, dsp->dom_t);
	switch (code) {
	case SUBSYS_BEFORE_SHUTDOWN:
	case SERVREG_NOTIF_SERVICE_STATE_DOWN_V01:
		SLIM_INFO(dev, "SLIM DSP SSR notify cb:%lu\n", code);
		atomic_set(&dev->ssr_in_progress, 1);
		/* wait for current transaction */
		mutex_lock(&dev->tx_lock);
		/* make sure autosuspend is not called until ADSP comes up*/
		pm_runtime_get_noresume(dev->dev);
		dev->state = MSM_CTRL_DOWN;
		dev->qmi.deferred_resp = false;
		msm_slim_sps_exit(dev, false);
		ngd_dom_down(dev);
		mutex_unlock(&dev->tx_lock);
		break;
	case SUBSYS_AFTER_POWERUP:
	case SERVREG_NOTIF_SERVICE_STATE_UP_V01:
		SLIM_INFO(dev, "SLIM DSP SSR notify cb:0x%x\n", code);
		/* Hold wake lock until notify slaves thread is done */
		pm_stay_awake(dev->dev);
		atomic_set(&dev->init_in_progress, 1);
		if (dev->lpass_mem_usage) {
			dev->lpass_mem->start = dev->lpass_phy_base;
			dev->lpass.base = dev->lpass_virt_base;
		}
		atomic_set(&dev->ssr_in_progress, 0);
		schedule_work(&dev->dsp.dom_up);
		break;
	case LOCATOR_UP:
		reg = _cmd;
		if (!reg || reg->total_domains != 1) {
			SLIM_WARN(dev, "error locating audio-PD\n");
			if (reg)
				SLIM_WARN(dev, "audio-PDs matched:%d\n",
						reg->total_domains);

			/* Fall back to SSR */
			ngd_reg_ssr(dev);
			return NOTIFY_DONE;
		}
		dev->dsp.domr = service_notif_register_notifier(
				reg->domain_list->name,
				reg->domain_list->instance_id,
				&dev->dsp.nb,
				&cur);
		SLIM_INFO(dev, "reg-PD client:%s with service:%s\n",
				reg->client_name, reg->service_name);
		SLIM_INFO(dev, "reg-PD dom:%s instance:%d, cur:0x%x\n",
				reg->domain_list->name,
				reg->domain_list->instance_id, cur);

		if (cur == SERVREG_NOTIF_SERVICE_STATE_UP_V01) {
			pm_stay_awake(dev->dev);
			atomic_set(&dev->init_in_progress, 1);
			if (dev->lpass_mem_usage) {
				dev->lpass_mem->start = dev->lpass_phy_base;
				dev->lpass.base = dev->lpass_virt_base;
			}
			atomic_set(&dev->ssr_in_progress, 0);
			schedule_work(&dev->dsp.dom_up);
		}

		if (IS_ERR_OR_NULL(dev->dsp.domr))
			ngd_reg_ssr(dev);
		else
			dev->dsp.dom_t = MSM_SLIM_DOM_PD;
		break;
	case LOCATOR_DOWN:
		ngd_reg_ssr(dev);
	default:
		break;
	}
	return NOTIFY_DONE;
}

static void ngd_dom_init(struct msm_slim_ctrl *dev)
{
	struct pd_qmi_client_data reg;
	int ret;

	memset(&reg, 0, sizeof(struct pd_qmi_client_data));
	dev->dsp.nb.priority = 4;
	dev->dsp.nb.notifier_call = dsp_domr_notify_cb;
	scnprintf(reg.client_name, QMI_SERVREG_LOC_NAME_LENGTH_V01, "appsngd%d",
		 dev->ctrl.nr);
	scnprintf(reg.service_name, QMI_SERVREG_LOC_NAME_LENGTH_V01,
		 "avs/audio");
	ret = get_service_location(reg.client_name, reg.service_name,
				   &dev->dsp.nb);
	if (ret)
		ngd_reg_ssr(dev);
}

static int mdm_ssr_notify_cb(struct notifier_block *n, unsigned long code,
				void *_cmd)
{
	void __iomem *ngd;
	struct msm_slim_ss *ext_mdm = container_of(n, struct msm_slim_ss, nb);
	struct msm_slim_ctrl *dev = container_of(ext_mdm, struct msm_slim_ctrl,
						ext_mdm);
	struct slim_controller *ctrl = &dev->ctrl;
	u32 laddr;
	struct slim_device *sbdev;

	switch (code) {
	case SUBSYS_BEFORE_SHUTDOWN:
		SLIM_INFO(dev, "SLIM %lu external_modem SSR notify cb\n", code);
		/* vote for runtime-pm so that ADSP doesn't go down */
		msm_slim_get_ctrl(dev);
		/*
		 * checking framer here will wake-up ADSP and may avoid framer
		 * handover later
		 */
		msm_slim_qmi_check_framer_request(dev);
		dev->ext_mdm.state = MSM_CTRL_DOWN;
		msm_slim_put_ctrl(dev);
		break;
	case SUBSYS_AFTER_POWERUP:
		if (dev->ext_mdm.state != MSM_CTRL_DOWN)
			return NOTIFY_DONE;
		SLIM_INFO(dev,
			"SLIM %lu external_modem SSR notify cb\n", code);
		/* vote for runtime-pm so that ADSP doesn't go down */
		msm_slim_get_ctrl(dev);
		msm_slim_qmi_check_framer_request(dev);
		/* If NGD enumeration is lost, we will need to power us up */
		ngd = dev->base + NGD_BASE(dev->ctrl.nr, dev->ver);
		laddr = readl_relaxed(ngd + NGD_STATUS);
		if (!(laddr & NGD_LADDR)) {
			mutex_lock(&dev->tx_lock);
			/* runtime-pm state should be consistent with HW */
			pm_runtime_disable(dev->dev);
			pm_runtime_set_suspended(dev->dev);
			dev->state = MSM_CTRL_DOWN;
			mutex_unlock(&dev->tx_lock);
			SLIM_INFO(dev,
				"SLIM MDM SSR (active framer on MDM) dev-down\n");
			list_for_each_entry(sbdev, &ctrl->devs, dev_list)
				slim_report_absent(sbdev);
			ngd_slim_runtime_resume(dev->dev);
			pm_runtime_set_active(dev->dev);
			pm_runtime_enable(dev->dev);
		}
		dev->ext_mdm.state = MSM_CTRL_AWAKE;
		msm_slim_put_ctrl(dev);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static int ngd_get_tid(struct slim_controller *ctrl, struct slim_msg_txn *txn,
				u8 *tid, struct completion *done)
{
	struct msm_slim_ctrl *dev = slim_get_ctrldata(ctrl);
	unsigned long flags;

	spin_lock_irqsave(&ctrl->txn_lock, flags);
	if (ctrl->last_tid < SLIM_MAX_TXNS) {
		dev->msg_cnt = ctrl->last_tid;
		ctrl->last_tid++;
	} else {
		int i;

		for (i = 0; i < SLIM_MAX_TXNS; i++) {
			dev->msg_cnt =
				((dev->msg_cnt + 1) & (SLIM_MAX_TXNS - 1));
			if (ctrl->txnt[dev->msg_cnt] == NULL)
				break;
		}
		if (i >= SLIM_MAX_TXNS) {
			dev_err(&ctrl->dev, "out of TID\n");
			spin_unlock_irqrestore(&ctrl->txn_lock, flags);
			return -ENOMEM;
		}
	}
	ctrl->txnt[dev->msg_cnt] = txn;
	txn->tid = dev->msg_cnt;
	txn->comp = done;
	*tid = dev->msg_cnt;
	spin_unlock_irqrestore(&ctrl->txn_lock, flags);
	return 0;
}

static void slim_reinit_tx_msgq(struct msm_slim_ctrl *dev)
{
	/*
	 * disconnect/recoonect pipe so that subsequent
	 * transactions don't timeout due to unavailable
	 * descriptors
	 */
	if (dev->state != MSM_CTRL_DOWN) {
		msm_slim_disconnect_endp(dev, &dev->tx_msgq,
					&dev->use_tx_msgqs);
		msm_slim_connect_endp(dev, &dev->tx_msgq);
	}
}

static int ngd_check_hw_status(struct msm_slim_ctrl *dev)
{
	void __iomem *ngd = dev->base + NGD_BASE(dev->ctrl.nr, dev->ver);
	u32 laddr = readl_relaxed(ngd + NGD_STATUS);
	int ret = 0;

	/* Lost logical addr due to noise */
	if (!(laddr & NGD_LADDR)) {
		SLIM_WARN(dev, "NGD lost LADDR: status:0x%x\n", laddr);
		ret = ngd_slim_power_up(dev, false);

		if (ret) {
			SLIM_WARN(dev, "slim resume ret:%d, state:%d\n",
					ret, dev->state);
			ret = -EREMOTEIO;
		}
	}
	return ret;
}

static int ngd_xfer_msg(struct slim_controller *ctrl, struct slim_msg_txn *txn)
{
	DECLARE_COMPLETION_ONSTACK(tx_sent);

	struct msm_slim_ctrl *dev = slim_get_ctrldata(ctrl);
	u32 *pbuf;
	u8 *puc;
	int ret = 0;
	u8 la = txn->la;
	u8 txn_mt;
	u16 txn_mc = txn->mc;
	u8 wbuf[SLIM_MSGQ_BUF_LEN] = {0};
	bool report_sat = false;
	bool sync_wr = true;

	reinit_completion(&dev->xfer_done);

	if (txn->mc & SLIM_MSG_CLK_PAUSE_SEQ_FLG)
		return -EPROTONOSUPPORT;

	if (txn->mt == SLIM_MSG_MT_CORE &&
		(txn->mc >= SLIM_MSG_MC_BEGIN_RECONFIGURATION &&
		 txn->mc <= SLIM_MSG_MC_RECONFIGURE_NOW))
		return 0;

	if (txn->mc == SLIM_USR_MC_REPORT_SATELLITE &&
		txn->mt == SLIM_MSG_MT_SRC_REFERRED_USER)
		report_sat = true;
	else
		mutex_lock(&dev->tx_lock);

	if (!report_sat && !pm_runtime_enabled(dev->dev) &&
			dev->state == MSM_CTRL_ASLEEP) {
		/*
		 * Counter-part of system-suspend when runtime-pm is not enabled
		 * This way, resume can be left empty and device will be put in
		 * active mode only if client requests anything on the bus
		 * If the state was DOWN, SSR UP notification will take
		 * care of putting the device in active state.
		 */
		mutex_unlock(&dev->tx_lock);
		ret = ngd_slim_runtime_resume(dev->dev);

		if (ret) {
			SLIM_ERR(dev, "slim resume failed ret:%d, state:%d",
					ret, dev->state);
			return -EREMOTEIO;
		}
		mutex_lock(&dev->tx_lock);
	}

	/* If txn is tried when controller is down, wait for ADSP to boot */
	if (!report_sat) {
		if (dev->state == MSM_CTRL_DOWN) {
			u8 mc = (u8)txn->mc;
			int timeout;

			mutex_unlock(&dev->tx_lock);
			SLIM_INFO(dev, "ADSP slimbus not up yet\n");
			/*
			 * Messages related to data channel management can't
			 * wait since they are holding reconfiguration lock.
			 * clk_pause in resume (which can change state back to
			 * MSM_CTRL_AWAKE), will need that lock.
			 * Port disconnection, channel removal calls should pass
			 * through since there is no activity on the bus and
			 * those calls are triggered by clients due to
			 * device_down callback in that situation.
			 * Returning 0 on the disconnections and
			 * removals will ensure consistent state of channels,
			 * ports with the HW
			 * Remote requests to remove channel/port will be
			 * returned from the path where they wait on
			 * acknowledgment from ADSP
			 */
			if ((txn->mt == SLIM_MSG_MT_DEST_REFERRED_USER) &&
				((mc == SLIM_USR_MC_CHAN_CTRL ||
				mc == SLIM_USR_MC_DISCONNECT_PORT ||
				mc == SLIM_USR_MC_RECONFIG_NOW)))
				return -EREMOTEIO;
			if ((txn->mt == SLIM_MSG_MT_CORE) &&
				((mc == SLIM_MSG_MC_DISCONNECT_PORT ||
				mc == SLIM_MSG_MC_NEXT_REMOVE_CHANNEL ||
				mc == SLIM_USR_MC_RECONFIG_NOW)))
				return 0;
			if ((txn->mt == SLIM_MSG_MT_CORE) &&
				((mc >= SLIM_MSG_MC_CONNECT_SOURCE &&
				mc <= SLIM_MSG_MC_CHANGE_CONTENT) ||
				(mc >= SLIM_MSG_MC_BEGIN_RECONFIGURATION &&
				mc <= SLIM_MSG_MC_RECONFIGURE_NOW)))
				return -EREMOTEIO;
			if ((txn->mt == SLIM_MSG_MT_DEST_REFERRED_USER) &&
				((mc >= SLIM_USR_MC_DEFINE_CHAN &&
				mc < SLIM_USR_MC_DISCONNECT_PORT)))
				return -EREMOTEIO;
			timeout = wait_for_completion_timeout(&dev->ctrl_up,
							HZ);
			if (!timeout)
				return -ETIMEDOUT;
			mutex_lock(&dev->tx_lock);
		}

		mutex_unlock(&dev->tx_lock);
		ret = msm_slim_get_ctrl(dev);
		mutex_lock(&dev->tx_lock);
		/*
		 * Runtime-pm's callbacks are not called until runtime-pm's
		 * error status is cleared
		 * Setting runtime status to suspended clears the error
		 * It also makes HW status cosistent with what SW has it here
		 */
		if ((pm_runtime_enabled(dev->dev) && ret < 0) ||
				dev->state >= MSM_CTRL_ASLEEP) {
			SLIM_ERR(dev, "slim ctrl vote failed ret:%d, state:%d",
					ret, dev->state);
			pm_runtime_set_suspended(dev->dev);
			mutex_unlock(&dev->tx_lock);
			msm_slim_put_ctrl(dev);
			return -EREMOTEIO;
		}
		ret = ngd_check_hw_status(dev);
		if (ret) {
			mutex_unlock(&dev->tx_lock);
			msm_slim_put_ctrl(dev);
			return ret;
		}
	}

	if (txn->mt == SLIM_MSG_MT_CORE &&
		(txn->mc == SLIM_MSG_MC_CONNECT_SOURCE ||
		txn->mc == SLIM_MSG_MC_CONNECT_SINK ||
		txn->mc == SLIM_MSG_MC_DISCONNECT_PORT)) {
		int i = 0;

		if (txn->mc != SLIM_MSG_MC_DISCONNECT_PORT)
			SLIM_INFO(dev,
				"Connect port: laddr 0x%x  port_num %d chan_num %d\n",
					txn->la, txn->wbuf[0], txn->wbuf[1]);
		else
			SLIM_INFO(dev,
				"Disconnect port: laddr 0x%x  port_num %d\n",
					txn->la, txn->wbuf[0]);
		txn->mt = SLIM_MSG_MT_DEST_REFERRED_USER;
		if (txn->mc == SLIM_MSG_MC_CONNECT_SOURCE)
			txn->mc = SLIM_USR_MC_CONNECT_SRC;
		else if (txn->mc == SLIM_MSG_MC_CONNECT_SINK)
			txn->mc = SLIM_USR_MC_CONNECT_SINK;
		else if (txn->mc == SLIM_MSG_MC_DISCONNECT_PORT)
			txn->mc = SLIM_USR_MC_DISCONNECT_PORT;
		if (txn->la == SLIM_LA_MGR) {
			if (dev->pgdla == SLIM_LA_MGR) {
				u8 ea[] = {0, QC_DEVID_PGD, 0, 0, QC_MFGID_MSB,
						QC_MFGID_LSB};
				ea[2] = (u8)(dev->pdata.eapc & 0xFF);
				ea[3] = (u8)((dev->pdata.eapc & 0xFF00) >> 8);
				mutex_unlock(&dev->tx_lock);
				ret = dev->ctrl.get_laddr(&dev->ctrl, ea, 6,
						&dev->pgdla);
				SLIM_DBG(dev, "SLIM PGD LA:0x%x, ret:%d\n",
					dev->pgdla, ret);
				if (ret) {
					SLIM_ERR(dev,
						"Incorrect SLIM-PGD EAPC:0x%x\n",
							dev->pdata.eapc);
					return ret;
				}
				mutex_lock(&dev->tx_lock);
			}
			txn->la = dev->pgdla;
		}
		wbuf[i++] = txn->la;
		la = SLIM_LA_MGR;
		wbuf[i++] = txn->wbuf[0];
		if (txn->mc != SLIM_USR_MC_DISCONNECT_PORT)
			wbuf[i++] = txn->wbuf[1];

		txn->comp = &dev->xfer_done;
		ret = ngd_get_tid(ctrl, txn, &wbuf[i++], &dev->xfer_done);
		if (ret) {
			SLIM_ERR(dev, "TID for connect/disconnect fail:%d\n",
					ret);
			goto ngd_xfer_err;
		}
		txn->len = i;
		txn->wbuf = wbuf;
		txn->rl = txn->len + 4;
	}
	txn->rl--;

	if (txn->len > SLIM_MSGQ_BUF_LEN || txn->rl > SLIM_MSGQ_BUF_LEN) {
		SLIM_WARN(dev, "msg exeeds HW lim:%d, rl:%d, mc:0x%x, mt:0x%x",
					txn->len, txn->rl, txn->mc, txn->mt);
		ret = -EDQUOT;
		goto ngd_xfer_err;
	}

	if (txn->mt == SLIM_MSG_MT_CORE && txn->comp &&
		dev->use_tx_msgqs == MSM_MSGQ_ENABLED &&
		(txn_mc != SLIM_MSG_MC_REQUEST_INFORMATION &&
		 txn_mc != SLIM_MSG_MC_REQUEST_VALUE &&
		 txn_mc != SLIM_MSG_MC_REQUEST_CHANGE_VALUE &&
		 txn_mc != SLIM_MSG_MC_REQUEST_CLEAR_INFORMATION)) {
		sync_wr = false;
		pbuf = msm_get_msg_buf(dev, txn->rl, txn->comp);
	} else if (txn->mt == SLIM_MSG_MT_DEST_REFERRED_USER &&
			dev->use_tx_msgqs == MSM_MSGQ_ENABLED &&
			txn->mc == SLIM_USR_MC_REPEAT_CHANGE_VALUE &&
			txn->comp) {
		sync_wr = false;
		pbuf = msm_get_msg_buf(dev, txn->rl, txn->comp);
	} else {
		pbuf = msm_get_msg_buf(dev, txn->rl, &tx_sent);
	}

	if (!pbuf) {
		SLIM_ERR(dev, "Message buffer unavailable\n");
		ret = -ENOMEM;
		goto ngd_xfer_err;
	}
	dev->err = 0;

	if (txn->dt == SLIM_MSG_DEST_ENUMADDR) {
		ret = -EPROTONOSUPPORT;
		goto ngd_xfer_err;
	}
	if (txn->dt == SLIM_MSG_DEST_LOGICALADDR)
		*pbuf = SLIM_MSG_ASM_FIRST_WORD(txn->rl, txn->mt, txn->mc, 0,
				la);
	else
		*pbuf = SLIM_MSG_ASM_FIRST_WORD(txn->rl, txn->mt, txn->mc, 1,
				la);
	if (txn->dt == SLIM_MSG_DEST_LOGICALADDR)
		puc = ((u8 *)pbuf) + 3;
	else
		puc = ((u8 *)pbuf) + 2;
	if (txn->rbuf)
		*(puc++) = txn->tid;
	if (((txn->mt == SLIM_MSG_MT_CORE) &&
		((txn->mc >= SLIM_MSG_MC_REQUEST_INFORMATION &&
		txn->mc <= SLIM_MSG_MC_REPORT_INFORMATION) ||
		(txn->mc >= SLIM_MSG_MC_REQUEST_VALUE &&
		 txn->mc <= SLIM_MSG_MC_CHANGE_VALUE))) ||
		(txn->mc == SLIM_USR_MC_REPEAT_CHANGE_VALUE &&
		txn->mt == SLIM_MSG_MT_DEST_REFERRED_USER)) {
		*(puc++) = (txn->ec & 0xFF);
		*(puc++) = (txn->ec >> 8)&0xFF;
	}
	if (txn->wbuf) {
		if (dev->lpass_mem_usage)
			memcpy_toio(puc, txn->wbuf, txn->len);
		else
			memcpy(puc, txn->wbuf, txn->len);
	}
	if (txn->mt == SLIM_MSG_MT_DEST_REFERRED_USER &&
		(txn->mc == SLIM_USR_MC_CONNECT_SRC ||
		 txn->mc == SLIM_USR_MC_CONNECT_SINK ||
		 txn->mc == SLIM_USR_MC_DISCONNECT_PORT) && txn->wbuf &&
		wbuf[0] == dev->pgdla) {
		if (txn->mc != SLIM_USR_MC_DISCONNECT_PORT)
			dev->err = msm_slim_connect_pipe_port(dev, wbuf[1]);
		else
			writel_relaxed(0, PGD_PORT(PGD_PORT_CFGn,
					(dev->pipes[wbuf[1]].port_b),
						dev->ver));
		if (dev->err) {
			SLIM_ERR(dev, "pipe-port connect err:%d\n", dev->err);
			goto ngd_xfer_err;
		}
		/* Add port-base to port number if this is manager side port */
		puc[1] = (u8)dev->pipes[wbuf[1]].port_b;
	}
	dev->err = 0;
	/*
	 * If it's a read txn, it may be freed if a response is received by
	 * received thread before reaching end of this function.
	 * mc, mt may have changed to convert standard slimbus code/type to
	 * satellite user-defined message. Reinitialize again
	 */
	txn_mc = txn->mc;
	txn_mt = txn->mt;
	ret = msm_send_msg_buf(dev, pbuf, txn->rl,
			NGD_BASE(dev->ctrl.nr, dev->ver) + NGD_TX_MSG);
	if (!ret && sync_wr) {
		int i;
		int timeout = wait_for_completion_timeout(&tx_sent, HZ);

		if (!timeout && dev->use_tx_msgqs == MSM_MSGQ_ENABLED) {
			struct msm_slim_endp *endpoint = &dev->tx_msgq;
			struct sps_mem_buffer *mem = &endpoint->buf;
			u32 idx = (u32) (((u8 *)pbuf - (u8 *)mem->base) /
						SLIM_MSGQ_BUF_LEN);
			phys_addr_t addr = mem->phys_base +
						(idx * SLIM_MSGQ_BUF_LEN);
			ret = -ETIMEDOUT;
			SLIM_WARN(dev, "timeout, BAM desc_idx:%d, phys:%llx",
					idx, (u64)addr);
			for (i = 0; i < (SLIM_MSGQ_BUF_LEN >> 2) ; i++)
				SLIM_WARN(dev, "timeout:bam-desc[%d]:0x%x",
							i, *(pbuf + i));
			if (idx < MSM_TX_BUFS)
				dev->wr_comp[idx] = NULL;
			slim_reinit_tx_msgq(dev);
		} else if (!timeout) {
			ret = -ETIMEDOUT;
			SLIM_WARN(dev, "timeout non-BAM TX,len:%d", txn->rl);
			for (i = 0; i < (SLIM_MSGQ_BUF_LEN >> 2) ; i++)
				SLIM_WARN(dev, "timeout:txbuf[%d]:0x%x", i,
						dev->tx_buf[i]);
		} else {
			ret = dev->err;
		}
	}
	if (ret) {
		u32 conf, stat, rx_msgq, int_stat, int_en, int_clr;
		void __iomem *ngd = dev->base + NGD_BASE(dev->ctrl.nr,
							dev->ver);
		SLIM_WARN(dev, "TX failed :MC:0x%x,mt:0x%x, ret:%d, ver:%d\n",
				txn_mc, txn_mt, ret, dev->ver);
		conf = readl_relaxed(ngd);
		stat = readl_relaxed(ngd + NGD_STATUS);
		rx_msgq = readl_relaxed(ngd + NGD_RX_MSGQ_CFG);
		int_stat = readl_relaxed(ngd + NGD_INT_STAT);
		int_en = readl_relaxed(ngd + NGD_INT_EN);
		int_clr = readl_relaxed(ngd + NGD_INT_CLR);

		SLIM_WARN(dev, "conf:0x%x,stat:0x%x,rxmsgq:0x%x\n",
				conf, stat, rx_msgq);
		SLIM_ERR(dev, "int_stat:0x%x,int_en:0x%x,int_cll:0x%x\n",
				int_stat, int_en, int_clr);
	}

	if (txn_mt == SLIM_MSG_MT_DEST_REFERRED_USER &&
		(txn_mc == SLIM_USR_MC_CONNECT_SRC ||
		 txn_mc == SLIM_USR_MC_CONNECT_SINK ||
		 txn_mc == SLIM_USR_MC_DISCONNECT_PORT)) {
		int timeout;
		unsigned long flags;

		mutex_unlock(&dev->tx_lock);
		msm_slim_put_ctrl(dev);
		if (!ret) {
			timeout = wait_for_completion_timeout(txn->comp, HZ);
			/* remote side did not acknowledge */
			if (!timeout)
				ret = -EREMOTEIO;
			else
				ret = txn->ec;
		}
		if (ret) {
			SLIM_ERR(dev,
				"connect/disconnect:0x%x,tid:%d err:%d\n",
					txn->mc, txn->tid, ret);
			spin_lock_irqsave(&ctrl->txn_lock, flags);
			ctrl->txnt[txn->tid] = NULL;
			spin_unlock_irqrestore(&ctrl->txn_lock, flags);
		}
		return ret ? ret : dev->err;
	}
ngd_xfer_err:
	if (!report_sat) {
		mutex_unlock(&dev->tx_lock);
		msm_slim_put_ctrl(dev);
	}
	return ret ? ret : dev->err;
}

static int ngd_get_ec(u16 start_offset, u8 len, u16 *ec)
{
	if (len > SLIM_MAX_VE_SLC_BYTES ||
		start_offset > MSM_SLIM_VE_MAX_MAP_ADDR)
		return -EINVAL;
	if (len <= 4) {
		*ec = len - 1;
	} else if (len <= 8) {
		if (len & 0x1)
			return -EINVAL;
		*ec = ((len >> 1) + 1);
	} else {
		if (len & 0x3)
			return -EINVAL;
		*ec = ((len >> 2) + 3);
	}
	*ec |= (0x8 | ((start_offset & 0xF) << 4));
	*ec |= ((start_offset & 0xFF0) << 4);
	return 0;
}

static int ngd_user_msg(struct slim_controller *ctrl, u8 la, u8 mt, u8 mc,
				struct slim_ele_access *msg, u8 *buf, u8 len)
{
	int ret;
	struct slim_msg_txn txn;

	if (mt != SLIM_MSG_MT_DEST_REFERRED_USER ||
		mc != SLIM_USR_MC_REPEAT_CHANGE_VALUE) {
		return -EPROTONOSUPPORT;
	}

	ret = ngd_get_ec(msg->start_offset, len, &txn.ec);
	if (ret)
		return ret;
	txn.la = la;
	txn.mt = mt;
	txn.mc = mc;
	txn.dt = SLIM_MSG_DEST_LOGICALADDR;
	txn.len = len;
	txn.rl = len + 6;
	txn.wbuf = buf;
	txn.rbuf = NULL;
	txn.comp = msg->comp;
	return ngd_xfer_msg(ctrl, &txn);
}

static int ngd_bulk_cb(void *ctx, int err)
{
	if (ctx)
		complete(ctx);
	return err;
}

static int ngd_bulk_wr(struct slim_controller *ctrl, u8 la, u8 mt, u8 mc,
			struct slim_val_inf msgs[], int n,
			int (*comp_cb)(void *ctx, int err), void *ctx)
{
	struct msm_slim_ctrl *dev = slim_get_ctrldata(ctrl);
	int i, ret;
	struct msm_slim_endp *endpoint = &dev->tx_msgq;
	u32 *header;
	DECLARE_COMPLETION_ONSTACK(done);

	ret = msm_slim_get_ctrl(dev);
	mutex_lock(&dev->tx_lock);

	if ((pm_runtime_enabled(dev->dev) && ret < 0) ||
			dev->state >= MSM_CTRL_ASLEEP) {
		SLIM_WARN(dev, "vote failed/SSR in-progress ret:%d, state:%d",
				ret, dev->state);
		pm_runtime_set_suspended(dev->dev);
		mutex_unlock(&dev->tx_lock);
		msm_slim_put_ctrl(dev);
		return -EREMOTEIO;
	}
	if (!pm_runtime_enabled(dev->dev) && dev->state == MSM_CTRL_ASLEEP) {
		mutex_unlock(&dev->tx_lock);
		ret = ngd_slim_runtime_resume(dev->dev);

		if (ret) {
			SLIM_ERR(dev, "slim resume failed ret:%d, state:%d",
					ret, dev->state);
			return -EREMOTEIO;
		}
		mutex_lock(&dev->tx_lock);
	}

	ret = ngd_check_hw_status(dev);
	if (ret) {
		mutex_unlock(&dev->tx_lock);
		msm_slim_put_ctrl(dev);
		return ret;
	}

	if (dev->use_tx_msgqs != MSM_MSGQ_ENABLED) {
		SLIM_WARN(dev, "bulk wr not supported");
		ret = -EPROTONOSUPPORT;
		goto retpath;
	}
	if (dev->bulk.in_progress) {
		SLIM_WARN(dev, "bulk wr in progress:");
		ret = -EAGAIN;
		goto retpath;
	}
	dev->bulk.in_progress = true;
	/* every txn has 5 bytes of overhead: la, mc, mt, ec, len */
	dev->bulk.size = n * 5;
	for (i = 0; i < n; i++) {
		dev->bulk.size += msgs[i].num_bytes;
		dev->bulk.size += (4 - ((msgs[i].num_bytes + 1) & 0x3));
	}

	if (dev->bulk.size > 0xffff) {
		SLIM_WARN(dev, "len exceeds limit, split bulk and retry");
		ret = -EDQUOT;
		goto retpath;
	}
	if (dev->bulk.size > dev->bulk.buf_sz) {
		void *temp = krealloc(dev->bulk.base, dev->bulk.size,
				      GFP_KERNEL | GFP_DMA);
		if (!temp) {
			ret = -ENOMEM;
			goto retpath;
		}
		dev->bulk.base = temp;
		dev->bulk.buf_sz = dev->bulk.size;
	}

	header = dev->bulk.base;
	for (i = 0; i < n; i++) {
		u8 *buf = (u8 *)header;
		int rl = msgs[i].num_bytes + 5;
		u16 ec;

		*header = SLIM_MSG_ASM_FIRST_WORD(rl, mt, mc, 0, la);
		buf += 3;
		ret = ngd_get_ec(msgs[i].start_offset, msgs[i].num_bytes, &ec);
		if (ret)
			goto retpath;
		*(buf++) = (ec & 0xFF);
		*(buf++) = (ec >> 8) & 0xFF;
		memcpy(buf, msgs[i].wbuf, msgs[i].num_bytes);
		buf += msgs[i].num_bytes;
		header += (rl >> 2);
		if (rl & 3) {
			header++;
			memset(buf, 0, ((u8 *)header - buf));
		}
	}
	header = dev->bulk.base;
	if (comp_cb) {
		dev->bulk.cb = comp_cb;
		dev->bulk.ctx = ctx;
	} else {
		dev->bulk.cb = ngd_bulk_cb;
		dev->bulk.ctx = &done;
	}
	dev->bulk.wr_dma = dma_map_single(dev->dev, dev->bulk.base,
					  dev->bulk.size, DMA_TO_DEVICE);
	if (dma_mapping_error(dev->dev, dev->bulk.wr_dma)) {
		ret = -ENOMEM;
		goto retpath;
	}

	ret = sps_transfer_one(endpoint->sps, dev->bulk.wr_dma, dev->bulk.size,
						NULL, SPS_IOVEC_FLAG_EOT);
	if (ret) {
		SLIM_WARN(dev, "sps transfer one returned error:%d", ret);
		goto retpath;
	}
	if (dev->bulk.cb == ngd_bulk_cb) {
		int timeout = wait_for_completion_timeout(&done, HZ);

		if (!timeout) {
			SLIM_WARN(dev, "timeout for bulk wr");
			dma_unmap_single(dev->dev, dev->bulk.wr_dma,
					 dev->bulk.size, DMA_TO_DEVICE);
			ret = -ETIMEDOUT;
		}
	}
retpath:
	if (ret) {
		dev->bulk.in_progress = false;
		dev->bulk.ctx = NULL;
		dev->bulk.wr_dma = 0;
		slim_reinit_tx_msgq(dev);
	}
	mutex_unlock(&dev->tx_lock);
	msm_slim_put_ctrl(dev);
	return ret;
}

static int ngd_xferandwait_ack(struct slim_controller *ctrl,
				struct slim_msg_txn *txn)
{
	struct msm_slim_ctrl *dev = slim_get_ctrldata(ctrl);
	unsigned long flags;
	int ret;

	if (dev->state == MSM_CTRL_DOWN) {
		/*
		 * no need to send anything to the bus due to SSR
		 * transactions related to channel removal marked as success
		 * since HW is down
		 */
		if ((txn->mt == SLIM_MSG_MT_DEST_REFERRED_USER) &&
			((txn->mc >= SLIM_USR_MC_CHAN_CTRL &&
			  txn->mc <= SLIM_USR_MC_REQ_BW) ||
			txn->mc == SLIM_USR_MC_DISCONNECT_PORT)) {
			spin_lock_irqsave(&ctrl->txn_lock, flags);
			ctrl->txnt[txn->tid] = NULL;
			spin_unlock_irqrestore(&ctrl->txn_lock, flags);
			return 0;
		}
	}

	ret = ngd_xfer_msg(ctrl, txn);
	if (!ret) {
		int timeout;

		timeout = wait_for_completion_timeout(txn->comp, HZ);
		if (!timeout)
			ret = -ETIMEDOUT;
		else
			ret = txn->ec;
	}

	if (ret) {
		if (ret != -EREMOTEIO || txn->mc != SLIM_USR_MC_CHAN_CTRL)
			SLIM_ERR(dev, "master msg:0x%x,tid:%d ret:%d\n",
				txn->mc, txn->tid, ret);
		spin_lock_irqsave(&ctrl->txn_lock, flags);
		ctrl->txnt[txn->tid] = NULL;
		spin_unlock_irqrestore(&ctrl->txn_lock, flags);
	}

	return ret;
}

static int ngd_allocbw(struct slim_device *sb, int *subfrmc, int *clkgear)
{
	int ret = 0, num_chan = 0;
	struct slim_pending_ch *pch;
	struct slim_msg_txn txn;
	struct slim_controller *ctrl = sb->ctrl;
	DECLARE_COMPLETION_ONSTACK(done);
	u8 wbuf[SLIM_MSGQ_BUF_LEN];
	struct msm_slim_ctrl *dev = slim_get_ctrldata(ctrl);

	*clkgear = ctrl->clkgear;
	*subfrmc = 0;
	txn.mt = SLIM_MSG_MT_DEST_REFERRED_USER;
	txn.dt = SLIM_MSG_DEST_LOGICALADDR;
	txn.la = SLIM_LA_MGR;
	txn.len = 0;
	txn.ec = 0;
	txn.wbuf = wbuf;
	txn.rbuf = NULL;

	if (ctrl->sched.msgsl != ctrl->sched.pending_msgsl) {
		SLIM_DBG(dev, "slim reserve BW for messaging: req: %d\n",
				ctrl->sched.pending_msgsl);
		txn.mc = SLIM_USR_MC_REQ_BW;
		wbuf[txn.len++] = ((sb->laddr & 0x1f) |
				((u8)(ctrl->sched.pending_msgsl & 0x7) << 5));
		wbuf[txn.len++] = (u8)(ctrl->sched.pending_msgsl >> 3);
		ret = ngd_get_tid(ctrl, &txn, &wbuf[txn.len++], &done);
		if (ret)
			return ret;
		txn.rl = txn.len + 4;
		ret = ngd_xferandwait_ack(ctrl, &txn);
		if (ret)
			return ret;

		txn.mc = SLIM_USR_MC_RECONFIG_NOW;
		txn.len = 2;
		wbuf[1] = sb->laddr;
		txn.rl = txn.len + 4;
		ret = ngd_get_tid(ctrl, &txn, &wbuf[0], &done);
		if (ret)
			return ret;
		ret = ngd_xferandwait_ack(ctrl, &txn);
		if (ret)
			return ret;

		txn.len = 0;
	}
	list_for_each_entry(pch, &sb->mark_define, pending) {
		struct slim_ich *slc;

		slc = &ctrl->chans[pch->chan];
		if (!slc) {
			SLIM_WARN(dev, "no channel in define?\n");
			return -ENXIO;
		}
		if (txn.len == 0) {
			/* Per protocol, only last 5 bits for client no. */
			wbuf[txn.len++] = (u8) (slc->prop.dataf << 5) |
					(sb->laddr & 0x1f);
			wbuf[txn.len] = slc->prop.sampleszbits >> 2;
			if (slc->srch && slc->prop.prot == SLIM_PUSH)
				slc->prop.prot = SLIM_PULL;
			if (slc->coeff == SLIM_COEFF_3)
				wbuf[txn.len] |= 1 << 5;
			wbuf[txn.len++] |= slc->prop.auxf << 6;
			wbuf[txn.len++] = slc->rootexp << 4 | slc->prop.prot;
			wbuf[txn.len++] = slc->prrate;
			ret = ngd_get_tid(ctrl, &txn, &wbuf[txn.len++], &done);
			if (ret) {
				SLIM_WARN(dev, "no tid for channel define?\n");
				return -ENXIO;
			}
		}
		num_chan++;
		wbuf[txn.len++] = slc->chan;
		SLIM_INFO(dev, "slim activate chan:%d, laddr: 0x%x\n",
				slc->chan, sb->laddr);
	}
	if (txn.len) {
		txn.mc = SLIM_USR_MC_DEF_ACT_CHAN;
		txn.rl = txn.len + 4;
		ret = ngd_xferandwait_ack(ctrl, &txn);
		if (ret)
			return ret;

		txn.mc = SLIM_USR_MC_RECONFIG_NOW;
		txn.len = 2;
		wbuf[1] = sb->laddr;
		txn.rl = txn.len + 4;
		ret = ngd_get_tid(ctrl, &txn, &wbuf[0], &done);
		if (ret)
			return ret;
		ret = ngd_xferandwait_ack(ctrl, &txn);
		if (ret)
			return ret;
	}
	txn.len = 0;
	list_for_each_entry(pch, &sb->mark_removal, pending) {
		struct slim_ich *slc;

		slc = &ctrl->chans[pch->chan];
		if (!slc) {
			SLIM_WARN(dev, "no channel in removal?\n");
			return -ENXIO;
		}
		if (txn.len == 0) {
			/* Per protocol, only last 5 bits for client no. */
			wbuf[txn.len++] = (u8) (SLIM_CH_REMOVE << 6) |
					(sb->laddr & 0x1f);
			ret = ngd_get_tid(ctrl, &txn, &wbuf[txn.len++], &done);
			if (ret) {
				SLIM_WARN(dev, "no tid for channel define?\n");
				return -ENXIO;
			}
		}
		wbuf[txn.len++] = slc->chan;
		SLIM_INFO(dev, "slim remove chan:%d, laddr: 0x%x\n",
			   slc->chan, sb->laddr);
	}
	if (txn.len) {
		txn.mc = SLIM_USR_MC_CHAN_CTRL;
		txn.rl = txn.len + 4;
		ret = ngd_xferandwait_ack(ctrl, &txn);
		/* HW restarting, channel removal should succeed */
		if (ret == -EREMOTEIO)
			return 0;
		else if (ret)
			return ret;

		txn.mc = SLIM_USR_MC_RECONFIG_NOW;
		txn.len = 2;
		wbuf[1] = sb->laddr;
		txn.rl = txn.len + 4;
		ret = ngd_get_tid(ctrl, &txn, &wbuf[0], &done);
		if (ret)
			return ret;
		ret = ngd_xferandwait_ack(ctrl, &txn);
		if (ret)
			return ret;
		txn.len = 0;
	}
	return 0;
}

static int ngd_set_laddr(struct slim_controller *ctrl, const u8 *ea,
				u8 elen, u8 laddr)
{
	return 0;
}

static int ngd_get_laddr(struct slim_controller *ctrl, const u8 *ea,
				u8 elen, u8 *laddr)
{
	int ret;
	u8 wbuf[10];
	struct slim_msg_txn txn;
	DECLARE_COMPLETION_ONSTACK(done);

	txn.mt = SLIM_MSG_MT_DEST_REFERRED_USER;
	txn.dt = SLIM_MSG_DEST_LOGICALADDR;
	txn.la = SLIM_LA_MGR;
	txn.ec = 0;
	ret = ngd_get_tid(ctrl, &txn, &wbuf[0], &done);
	if (ret)
		return ret;
	memcpy(&wbuf[1], ea, elen);
	txn.mc = SLIM_USR_MC_ADDR_QUERY;
	txn.rl = 11;
	txn.len = 7;
	txn.wbuf = wbuf;
	txn.rbuf = NULL;
	ret = ngd_xferandwait_ack(ctrl, &txn);
	if (!ret && txn.la == 0xFF)
		ret = -ENXIO;
	else if (!ret)
		*laddr = txn.la;
	return ret;
}

static void ngd_slim_setup(struct msm_slim_ctrl *dev)
{
	u32 new_cfg = NGD_CFG_ENABLE;
	u32 cfg = readl_relaxed(dev->base +
				 NGD_BASE(dev->ctrl.nr, dev->ver));
	if (dev->state == MSM_CTRL_DOWN) {
		/* if called after SSR, cleanup and re-assign */
		if (dev->use_tx_msgqs != MSM_MSGQ_RESET)
			msm_slim_deinit_ep(dev, &dev->tx_msgq,
					   &dev->use_tx_msgqs);

		if (dev->use_rx_msgqs != MSM_MSGQ_RESET)
			msm_slim_deinit_ep(dev, &dev->rx_msgq,
					   &dev->use_rx_msgqs);

		msm_slim_sps_init(dev, dev->bam_mem,
			NGD_BASE(dev->ctrl.nr,
			dev->ver) + NGD_STATUS, true);
	} else {
		if (dev->use_rx_msgqs == MSM_MSGQ_DISABLED)
			goto setup_tx_msg_path;

		if ((dev->use_rx_msgqs == MSM_MSGQ_ENABLED) &&
			(cfg & NGD_CFG_RX_MSGQ_EN))
			goto setup_tx_msg_path;

		if (dev->use_rx_msgqs == MSM_MSGQ_ENABLED)
			msm_slim_disconnect_endp(dev, &dev->rx_msgq,
						 &dev->use_rx_msgqs);
		msm_slim_connect_endp(dev, &dev->rx_msgq);

setup_tx_msg_path:
		if (dev->use_tx_msgqs == MSM_MSGQ_DISABLED)
			goto ngd_enable;
		if (dev->use_tx_msgqs == MSM_MSGQ_ENABLED &&
			cfg & NGD_CFG_TX_MSGQ_EN)
			goto ngd_enable;

		if (dev->use_tx_msgqs == MSM_MSGQ_ENABLED)
			msm_slim_disconnect_endp(dev, &dev->tx_msgq,
						 &dev->use_tx_msgqs);
		msm_slim_connect_endp(dev, &dev->tx_msgq);
	}
ngd_enable:

	if (dev->use_rx_msgqs == MSM_MSGQ_ENABLED)
		new_cfg |= NGD_CFG_RX_MSGQ_EN;
	if (dev->use_tx_msgqs == MSM_MSGQ_ENABLED)
		new_cfg |= NGD_CFG_TX_MSGQ_EN;

	/* Enable NGD, and program MSGQs if not already */
	if (cfg == new_cfg)
		return;

	writel_relaxed(new_cfg, dev->base + NGD_BASE(dev->ctrl.nr, dev->ver));
	/* make sure NGD MSG-Q config goes through */
	mb();
}

static void ngd_slim_rx(struct msm_slim_ctrl *dev, u8 *buf)
{
	unsigned long flags;
	u8 mc, mt, len;

	len = buf[0] & 0x1F;
	mt = (buf[0] >> 5) & 0x7;
	mc = buf[1];
	if (mc == SLIM_USR_MC_MASTER_CAPABILITY &&
		mt == SLIM_MSG_MT_SRC_REFERRED_USER)
		complete(&dev->rx_msgq_notify);

	if (mc == SLIM_MSG_MC_REPLY_INFORMATION ||
			mc == SLIM_MSG_MC_REPLY_VALUE) {
		u8 tid = buf[3];

		dev_dbg(dev->dev, "tid:%d, len:%d\n", tid, len);
		slim_msg_response(&dev->ctrl, &buf[4], tid,
					len - 4);
		pm_runtime_mark_last_busy(dev->dev);
	}
	if (mc == SLIM_USR_MC_ADDR_REPLY &&
		mt == SLIM_MSG_MT_SRC_REFERRED_USER) {
		struct slim_msg_txn *txn;
		u8 failed_ea[6] = {0, 0, 0, 0, 0, 0};

		spin_lock_irqsave(&dev->ctrl.txn_lock, flags);
		txn = dev->ctrl.txnt[buf[3]];
		if (!txn) {
			spin_unlock_irqrestore(&dev->ctrl.txn_lock, flags);
			SLIM_WARN(dev,
				"LADDR response after timeout, tid:0x%x\n",
					buf[3]);
			return;
		}
		if (memcmp(&buf[4], failed_ea, 6))
			txn->la = buf[10];
		dev->ctrl.txnt[buf[3]] = NULL;
		complete(txn->comp);
		spin_unlock_irqrestore(&dev->ctrl.txn_lock, flags);
	}
	if (mc == SLIM_USR_MC_GENERIC_ACK &&
		mt == SLIM_MSG_MT_SRC_REFERRED_USER) {
		struct slim_msg_txn *txn;

		spin_lock_irqsave(&dev->ctrl.txn_lock, flags);
		txn = dev->ctrl.txnt[buf[3]];
		if (!txn) {
			spin_unlock_irqrestore(&dev->ctrl.txn_lock, flags);
			SLIM_WARN(dev, "ACK received after timeout, tid:0x%x\n",
				buf[3]);
			return;
		}
		dev_dbg(dev->dev, "got response:tid:%d, response:0x%x",
				(int)buf[3], buf[4]);
		if (!(buf[4] & MSM_SAT_SUCCSS)) {
			SLIM_WARN(dev, "TID:%d, NACK code:0x%x\n", (int)buf[3],
						buf[4]);
			txn->ec = -EIO;
		}
		dev->ctrl.txnt[buf[3]] = NULL;
		complete(txn->comp);
		spin_unlock_irqrestore(&dev->ctrl.txn_lock, flags);
	}
}

static int ngd_slim_power_up(struct msm_slim_ctrl *dev, bool mdm_restart)
{
	void __iomem *ngd;
	int timeout, retries = 0, ret = 0;
	enum msm_ctrl_state cur_state = dev->state;
	u32 laddr;
	u32 rx_msgq;
	u32 ngd_int = (NGD_INT_TX_NACKED_2 |
			NGD_INT_MSG_BUF_CONTE | NGD_INT_MSG_TX_INVAL |
			NGD_INT_IE_VE_CHG | NGD_INT_DEV_ERR |
			NGD_INT_TX_MSG_SENT);

	if (!mdm_restart && cur_state == MSM_CTRL_DOWN) {
		int timeout = wait_for_completion_timeout(&dev->qmi.qmi_comp,
						HZ);
		if (!timeout) {
			SLIM_ERR(dev, "slimbus QMI init timed out\n");
			return -EREMOTEIO;
		}
	}

hw_init_retry:
	/* No need to vote if contorller is not in low power mode */
	if (!mdm_restart &&
		(cur_state == MSM_CTRL_DOWN || cur_state == MSM_CTRL_ASLEEP)) {
		ret = msm_slim_qmi_power_request(dev, true);
		if (ret) {
			SLIM_WARN(dev, "SLIM power req failed:%d, retry:%d\n",
					ret, retries);
			if (!atomic_read(&dev->ssr_in_progress))
				msm_slim_qmi_power_request(dev, false);
			if (retries < INIT_MX_RETRIES &&
				!atomic_read(&dev->ssr_in_progress)) {
				retries++;
				goto hw_init_retry;
			}
			return ret;
		}
	}
	retries = 0;

	if (!dev->ver) {
		dev->ver = readl_relaxed(dev->base);
		/* Version info in 16 MSbits */
		dev->ver >>= 16;
	}
	ngd = dev->base + NGD_BASE(dev->ctrl.nr, dev->ver);
	laddr = readl_relaxed(ngd + NGD_STATUS);
	if (laddr & NGD_LADDR) {
		u32 int_en = readl_relaxed(ngd + NGD_INT_EN);

		/*
		 * external MDM restart case where ADSP itself was active framer
		 * For example, modem restarted when playback was active
		 */
		if (cur_state == MSM_CTRL_AWAKE) {
			SLIM_INFO(dev, "Subsys restart: ADSP active framer\n");
			return 0;
		}
		/*
		 * ADSP power collapse case, where HW wasn't reset.
		 */
		if (int_en != 0)
			return 0;

		/* Retention */
		if (dev->use_rx_msgqs == MSM_MSGQ_ENABLED)
			msm_slim_disconnect_endp(dev, &dev->rx_msgq,
						 &dev->use_rx_msgqs);
		if (dev->use_tx_msgqs == MSM_MSGQ_ENABLED)
			msm_slim_disconnect_endp(dev, &dev->tx_msgq,
						 &dev->use_tx_msgqs);

		writel_relaxed(ngd_int, (dev->base + NGD_INT_EN +
					NGD_BASE(dev->ctrl.nr, dev->ver)));

		rx_msgq = readl_relaxed(ngd + NGD_RX_MSGQ_CFG);
		/**
		 * Program with minimum value so that signal get
		 * triggered immediately after receiving the message
		 */
		writel_relaxed((rx_msgq | SLIM_RX_MSGQ_TIMEOUT_VAL),
						(ngd + NGD_RX_MSGQ_CFG));
		/* reconnect BAM pipes if needed and enable NGD */
		ngd_slim_setup(dev);
		return 0;
	}

	if (mdm_restart) {
		/*
		 * external MDM SSR when MDM is active framer
		 * ADSP will reset slimbus HW. disconnect BAM pipes so that
		 * they can be connected after capability message is received.
		 * Set device state to ASLEEP to be synchronous with the HW
		 */
		/* make current state as DOWN */
		cur_state = MSM_CTRL_DOWN;
		SLIM_INFO(dev,
			"SLIM MDM restart: MDM active framer: reinit HW\n");
		/* disconnect BAM pipes */
		msm_slim_sps_exit(dev, false);
		dev->state = MSM_CTRL_DOWN;
	}

capability_retry:
	/*
	 * ADSP power collapse case (OR SSR), where HW was reset
	 * BAM programming will happen when capability message is received
	 */
	writel_relaxed(ngd_int, dev->base + NGD_INT_EN +
				NGD_BASE(dev->ctrl.nr, dev->ver));

	rx_msgq = readl_relaxed(ngd + NGD_RX_MSGQ_CFG);
	/*
	 * Program with minimum value so that signal get
	 * triggered immediately after receiving the message
	 */
	writel_relaxed(rx_msgq|SLIM_RX_MSGQ_TIMEOUT_VAL,
					ngd + NGD_RX_MSGQ_CFG);
	/* make sure register got updated */
	mb();

	/* reconnect BAM pipes if needed and enable NGD */
	ngd_slim_setup(dev);

	timeout = wait_for_completion_timeout(&dev->reconf, HZ);
	if (!timeout) {
		u32 cfg = readl_relaxed(dev->base +
					 NGD_BASE(dev->ctrl.nr, dev->ver));
		laddr = readl_relaxed(ngd + NGD_STATUS);
		SLIM_WARN(dev,
			  "slim capability time-out:%d, stat:0x%x,cfg:0x%x\n",
				retries, laddr, cfg);
		if ((retries < INIT_MX_RETRIES) &&
				!atomic_read(&dev->ssr_in_progress)) {
			retries++;
			goto capability_retry;
		}
		return -ETIMEDOUT;
	}
	/* mutliple transactions waiting on slimbus to power up? */
	if (cur_state == MSM_CTRL_DOWN)
		complete_all(&dev->ctrl_up);
	/* Resetting the log level */
	SLIM_RST_LOGLVL(dev);
	return 0;
}

static int ngd_slim_enable(struct msm_slim_ctrl *dev, bool enable)
{
	int ret = 0;

	if (enable) {
		ret = msm_slim_qmi_init(dev, false);
		/* controller state should be in sync with framework state */
		if (!ret) {
			complete(&dev->qmi.qmi_comp);
			if (!pm_runtime_enabled(dev->dev) ||
					!pm_runtime_suspended(dev->dev))
				ngd_slim_runtime_resume(dev->dev);
			else
				pm_runtime_resume(dev->dev);
			pm_runtime_mark_last_busy(dev->dev);
			pm_runtime_put(dev->dev);
		} else
			SLIM_ERR(dev, "qmi init fail, ret:%d, state:%d\n",
					ret, dev->state);
	} else {
		msm_slim_qmi_exit(dev);
	}

	return ret;
}

#ifdef CONFIG_PM
static int ngd_slim_power_down(struct msm_slim_ctrl *dev)
{
	unsigned long flags;
	int i;
	struct slim_controller *ctrl = &dev->ctrl;

	spin_lock_irqsave(&ctrl->txn_lock, flags);
	/* Pending response for a message */
	for (i = 0; i < ctrl->last_tid; i++) {
		if (ctrl->txnt[i]) {
			spin_unlock_irqrestore(&ctrl->txn_lock, flags);
			SLIM_INFO(dev, "NGD down:txn-rsp for %d pending", i);
			return -EBUSY;
		}
	}
	spin_unlock_irqrestore(&ctrl->txn_lock, flags);
	return msm_slim_qmi_power_request(dev, false);
}
#endif

static int ngd_slim_rx_msgq_thread(void *data)
{
	struct msm_slim_ctrl *dev = (struct msm_slim_ctrl *)data;
	struct completion *notify = &dev->rx_msgq_notify;
	int ret = 0;
	bool release_wake_lock = false;

	while (!kthread_should_stop()) {
		struct slim_msg_txn txn;
		int retries = 0;
		u8 wbuf[8];

		wait_for_completion_interruptible(notify);

		txn.dt = SLIM_MSG_DEST_LOGICALADDR;
		txn.ec = 0;
		txn.rbuf = NULL;
		txn.mc = SLIM_USR_MC_REPORT_SATELLITE;
		txn.mt = SLIM_MSG_MT_SRC_REFERRED_USER;
		txn.la = SLIM_LA_MGR;
		wbuf[0] = SAT_MAGIC_LSB;
		wbuf[1] = SAT_MAGIC_MSB;
		wbuf[2] = SAT_MSG_VER;
		wbuf[3] = SAT_MSG_PROT;
		txn.wbuf = wbuf;
		txn.len = 4;
		SLIM_INFO(dev, "SLIM SAT: Rcvd master capability\n");
capability_retry:
		txn.rl = 8;
		ret = ngd_xfer_msg(&dev->ctrl, &txn);
		if (!ret) {
			enum msm_ctrl_state prev_state = dev->state;

			SLIM_INFO(dev,
				"SLIM SAT: capability exchange successful\n");
			if (prev_state < MSM_CTRL_ASLEEP)
				SLIM_WARN(dev,
					"capability due to noise, state:%d\n",
						prev_state);
			complete(&dev->reconf);
			/* ADSP SSR, send device_up notifications */
			if (prev_state == MSM_CTRL_DOWN)
				complete(&dev->qmi.slave_notify);
			else
				release_wake_lock = true;
		} else if (ret == -EIO) {
			SLIM_WARN(dev, "capability message NACKed, retrying\n");
			if (retries < INIT_MX_RETRIES) {
				msleep(DEF_RETRY_MS);
				retries++;
				goto capability_retry;
			} else {
				release_wake_lock = true;
			}
		} else {
			SLIM_WARN(dev, "SLIM: capability TX failed:%d\n", ret);
			release_wake_lock = true;
		}

		if (release_wake_lock) {
			/*
			 * As we are not going to reset the
			 * init_in_progress flag and release wake
			 * lock from notify slave thread, we are
			 * doing it here.
			 */
			atomic_set(&dev->init_in_progress, 0);
			pm_relax(dev->dev);
			release_wake_lock = false;
		}
	}
	atomic_set(&dev->init_in_progress, 0);
	pm_relax(dev->dev);
	return 0;
}

static int ngd_notify_slaves(void *data)
{
	struct msm_slim_ctrl *dev = (struct msm_slim_ctrl *)data;
	struct slim_controller *ctrl = &dev->ctrl;
	struct slim_device *sbdev;
	struct list_head *pos, *next;
	int ret, i = 0;

	ret = ngd_slim_qmi_svc_event_init(&dev->qmi);
	if (ret) {
		pr_err("Slimbus QMI service registration failed:%d\n", ret);
		pm_relax(dev->dev);
		return ret;
	}
	ngd_dom_init(dev);

	while (!kthread_should_stop()) {
		wait_for_completion_interruptible(&dev->qmi.slave_notify);
		/* Probe devices for first notification */
		if (!i) {
			i++;
			dev->err = 0;
			if (dev->dev->of_node)
				of_register_slim_devices(&dev->ctrl);

			/*
			 * Add devices registered with board-info now that
			 * controller is up
			 */
			slim_ctrl_add_boarddevs(&dev->ctrl);
		} else {
			slim_framer_booted(ctrl);
		}
		mutex_lock(&ctrl->m_ctrl);
		list_for_each_safe(pos, next, &ctrl->devs) {
			int j;

			sbdev = list_entry(pos, struct slim_device, dev_list);
			mutex_unlock(&ctrl->m_ctrl);
			for (j = 0; j < LADDR_RETRY; j++) {
				ret = slim_get_logical_addr(sbdev,
						sbdev->e_addr,
						6, &sbdev->laddr);
				if (!ret)
					break;
				/* time for ADSP to assign LA */
				msleep(20);
			}
			mutex_lock(&ctrl->m_ctrl);
		}
		mutex_unlock(&ctrl->m_ctrl);
		atomic_set(&dev->init_in_progress, 0);
		pm_relax(dev->dev);
	}
	atomic_set(&dev->init_in_progress, 0);
	pm_relax(dev->dev);
	return 0;
}

static void ngd_dom_down(struct msm_slim_ctrl *dev)
{
	struct slim_controller *ctrl = &dev->ctrl;
	struct slim_device *sbdev;

	mutex_lock(&dev->ssr_lock);
	ngd_slim_enable(dev, false);
	/* device up should be called again after SSR */
	list_for_each_entry(sbdev, &ctrl->devs, dev_list)
		slim_report_absent(sbdev);
	SLIM_INFO(dev, "SLIM ADSP SSR (DOWN) done\n");
	mutex_unlock(&dev->ssr_lock);
}

static void ngd_dom_up(struct work_struct *work)
{
	struct msm_slim_ss *dsp =
		container_of(work, struct msm_slim_ss, dom_up);
	struct msm_slim_ctrl *dev =
		container_of(dsp, struct msm_slim_ctrl, dsp);

	/* Make sure qmi service is up before continuing */
	wait_for_completion_interruptible(&dev->qmi_up);

	mutex_lock(&dev->ssr_lock);
	if (ngd_slim_enable(dev, true)) {
		atomic_set(&dev->init_in_progress, 0);
		pm_relax(dev->dev);
	}
	mutex_unlock(&dev->ssr_lock);
}

static ssize_t debug_mask_show(struct device *device,
				struct device_attribute *attr,
				char *buf)
{
	struct platform_device *pdev = to_platform_device(device);
	struct msm_slim_ctrl *dev = platform_get_drvdata(pdev);

	return snprintf(buf, sizeof(int), "%u\n", dev->ipc_log_mask);
}

static ssize_t debug_mask_store(struct device *device,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(device);
	struct msm_slim_ctrl *dev = platform_get_drvdata(pdev);

	dev->ipc_log_mask = buf[0] - '0';
	if (dev->ipc_log_mask > DBG_LEV)
		dev->ipc_log_mask = DBG_LEV;
	return count;
}

static DEVICE_ATTR_RW(debug_mask);

static const struct of_device_id ngd_slim_dt_match[] = {
	{
		.compatible = "qcom,slim-ngd",
	},
	{
		.compatible = "qcom,iommu-slim-ctrl-cb",
	},
	{}
};

static int ngd_slim_iommu_probe(struct device *dev)
{
	struct platform_device *pdev;
	struct msm_slim_ctrl *ctrl_dev;

	if (unlikely(!dev->parent)) {
		dev_err(dev, "%s no parent for this device\n", __func__);
		return -EINVAL;
	}

	pdev = to_platform_device(dev->parent);
	if (!pdev) {
		dev_err(dev, "%s Parent platform device not found\n", __func__);
		return -EINVAL;
	}

	ctrl_dev = platform_get_drvdata(pdev);
	if (!ctrl_dev) {
		dev_err(dev, "%s NULL controller device\n", __func__);
		return -EINVAL;

	}
	ctrl_dev->iommu_desc.cb_dev = dev;
	SLIM_INFO(ctrl_dev, "NGD IOMMU initialization complete\n");
	return 0;
}

static int ngd_slim_probe(struct platform_device *pdev)
{
	struct msm_slim_ctrl *dev;
	int ret;
	struct resource		*bam_mem;
	struct resource		*slim_mem;
	struct resource		*lpass_mem;
	struct resource		*irq, *bam_irq;
	bool			rxreg_access = false;
	bool			slim_mdm = false;
	const char		*ext_modem_id = NULL;
#ifdef CONFIG_IPC_LOGGING
	char			ipc_err_log_name[30];
#endif

	if (of_device_is_compatible(pdev->dev.of_node,
				    "qcom,iommu-slim-ctrl-cb"))
		return ngd_slim_iommu_probe(&pdev->dev);

	slim_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"slimbus_physical");
	if (!slim_mem) {
		dev_err(&pdev->dev, "no slimbus physical memory resource\n");
		return -ENODEV;
	}
	bam_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"slimbus_bam_physical");
	if (!bam_mem) {
		dev_err(&pdev->dev, "no slimbus BAM memory resource\n");
		return -ENODEV;
	}
	irq = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						"slimbus_irq");
	if (!irq) {
		dev_err(&pdev->dev, "no slimbus IRQ resource\n");
		return -ENODEV;
	}
	bam_irq = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
						"slimbus_bam_irq");
	if (!bam_irq) {
		dev_err(&pdev->dev, "no slimbus BAM IRQ resource\n");
		return -ENODEV;
	}

	dev = kzalloc(sizeof(struct msm_slim_ctrl), GFP_KERNEL);
	if (IS_ERR_OR_NULL(dev)) {
		dev_err(&pdev->dev, "no memory for MSM slimbus controller\n");
		return PTR_ERR(dev);
	}

	dev->lpass_mem_usage = false;
	lpass_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"slimbus_lpass_mem");
	if (lpass_mem) {
		dev_dbg(&pdev->dev, "Slimbus lpass memory is used\n");
		dev->lpass_mem_usage = true;
		dev->lpass_phy_base = (unsigned long long)lpass_mem->start;
	}

	dev->wr_comp = kzalloc(sizeof(struct completion *) * MSM_TX_BUFS,
				GFP_KERNEL);
	if (!dev->wr_comp) {
		ret = -ENOMEM;
		goto err_nobulk;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&pdev->dev, "could not set 64 bit DMA mask,trying 32\n");
		if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
			dev_err(&pdev->dev, "could not set 32 bit DMA mask\n");
			goto err_nobulk;
		}
	}

	/* typical txn numbers and size used in bulk operation */
	dev->bulk.buf_sz = SLIM_MAX_TXNS * 8;
	dev->bulk.base = kzalloc(dev->bulk.buf_sz, GFP_KERNEL | GFP_DMA);
	if (!dev->bulk.base) {
		ret = -ENOMEM;
		goto err_nobulk;
	}

	dev->dev = &pdev->dev;
	platform_set_drvdata(pdev, dev);
	slim_set_ctrldata(&dev->ctrl, dev);

#ifdef CONFIG_IPC_LOGGING
	/* Create IPC log context */
	dev->ipc_slimbus_log = ipc_log_context_create(IPC_SLIMBUS_LOG_PAGES,
						dev_name(dev->dev), 0);
	if (!dev->ipc_slimbus_log)
		dev_err(&pdev->dev, "error creating ipc_logging context\n");
	else {
		/* Initialize the log mask */
		dev->ipc_log_mask = INFO_LEV;
		dev->default_ipc_log_mask = INFO_LEV;
		SLIM_INFO(dev, "start logging for slim dev %s\n",
				dev_name(dev->dev));
	}

	/* Create Error IPC log context */
	memset(ipc_err_log_name, 0, sizeof(ipc_err_log_name));
	scnprintf(ipc_err_log_name, sizeof(ipc_err_log_name), "%s%s",
						dev_name(dev->dev), "_err");
	dev->ipc_slimbus_log_err =
		ipc_log_context_create(IPC_SLIMBUS_LOG_PAGES,
						ipc_err_log_name, 0);
	if (!dev->ipc_slimbus_log_err)
		dev_err(&pdev->dev,
			"error creating ipc_error_logging context\n");
	else
		SLIM_INFO(dev, "start error logging for slim dev %s\n",
							ipc_err_log_name);
#endif

	ret = sysfs_create_file(&dev->dev->kobj, &dev_attr_debug_mask.attr);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create dev. attr\n");
		dev->sysfs_created = false;
	} else
		dev->sysfs_created = true;

	dev->base = devm_ioremap(&pdev->dev, slim_mem->start,
					resource_size(slim_mem));
	if (!dev->base) {
		dev_err(&pdev->dev, "IOremap failed\n");
		ret = -ENOMEM;
		goto err_ioremap_failed;
	}
	dev->bam.base = devm_ioremap(&pdev->dev, bam_mem->start,
					resource_size(bam_mem));
	if (!dev->bam.base) {
		dev_err(&pdev->dev, "BAM IOremap failed\n");
		ret = -ENOMEM;
		goto err_ioremap_failed;
	}

	if (lpass_mem) {
		dev->lpass.base = devm_ioremap(&pdev->dev, lpass_mem->start,
					resource_size(lpass_mem));
		if (!dev->lpass.base) {
			dev_err(&pdev->dev, "LPASS IOremap failed\n");
			ret = -ENOMEM;
			goto err_ioremap_failed;
		}
		dev->lpass_virt_base = dev->lpass.base;
	}

	if (pdev->dev.of_node) {

		ret = of_property_read_u32(pdev->dev.of_node, "cell-index",
					&dev->ctrl.nr);
		if (ret) {
			dev_err(&pdev->dev,
					"Cell index not specified:%d\n", ret);
			goto err_ioremap_failed;
		}
		rxreg_access = of_property_read_bool(pdev->dev.of_node,
					"qcom,rxreg-access");
		of_property_read_u32(pdev->dev.of_node, "qcom,apps-ch-pipes",
					&dev->pdata.apps_pipes);
		of_property_read_u32(pdev->dev.of_node, "qcom,ea-pc",
					&dev->pdata.eapc);
		ret = of_property_read_string(pdev->dev.of_node,
					"qcom,slim-mdm", &ext_modem_id);
		if (!ret)
			slim_mdm = true;

		dev->iommu_desc.s1_bypass = of_property_read_bool(
							pdev->dev.of_node,
							"qcom,iommu-s1-bypass");
		ret = of_platform_populate(pdev->dev.of_node, ngd_slim_dt_match,
					   NULL, &pdev->dev);
		if (ret) {
			dev_err(dev->dev, "%s: Failed to of_platform_populate %d\n",
				__func__, ret);
			goto err_ioremap_failed;
		}
	} else {
		dev->ctrl.nr = pdev->id;
	}
	/*
	 * Keep PGD's logical address as manager's. Query it when first data
	 * channel request comes in
	 */
	dev->pgdla = SLIM_LA_MGR;
	dev->ctrl.nchans = MSM_SLIM_NCHANS;
	dev->ctrl.nports = MSM_SLIM_NPORTS;
	dev->framer.rootfreq = SLIM_ROOT_FREQ >> 3;
	dev->framer.superfreq =
		dev->framer.rootfreq / SLIM_CL_PER_SUPERFRAME_DIV8;
	dev->ctrl.a_framer = &dev->framer;
	dev->ctrl.clkgear = SLIM_MAX_CLK_GEAR;
	dev->ctrl.set_laddr = ngd_set_laddr;
	dev->ctrl.get_laddr = ngd_get_laddr;
	dev->ctrl.allocbw = ngd_allocbw;
	dev->ctrl.xfer_msg = ngd_xfer_msg;
	dev->ctrl.xfer_user_msg = ngd_user_msg;
	dev->ctrl.xfer_bulk_wr = ngd_bulk_wr;
	dev->ctrl.wakeup = NULL;
	dev->ctrl.alloc_port = msm_alloc_port;
	dev->ctrl.dealloc_port = msm_dealloc_port;
	dev->ctrl.port_xfer = msm_slim_port_xfer;
	dev->ctrl.port_xfer_status = msm_slim_port_xfer_status;
	dev->bam_mem = bam_mem;
	dev->lpass_mem = lpass_mem;
	dev->rx_slim = ngd_slim_rx;

	init_completion(&dev->reconf);
	init_completion(&dev->ctrl_up);
	init_completion(&dev->qmi_up);
	init_completion(&dev->xfer_done);
	mutex_init(&dev->tx_lock);
	mutex_init(&dev->ssr_lock);
	spin_lock_init(&dev->tx_buf_lock);
	spin_lock_init(&dev->rx_lock);
	dev->ee = 1;
	dev->irq = irq->start;
	dev->bam.irq = bam_irq->start;
	atomic_set(&dev->ssr_in_progress, 0);

	if (rxreg_access)
		dev->use_rx_msgqs = MSM_MSGQ_DISABLED;
	else
		dev->use_rx_msgqs = MSM_MSGQ_RESET;

	/* Enable TX message queues by default as recommended by HW */
	dev->use_tx_msgqs = MSM_MSGQ_RESET;

	init_completion(&dev->rx_msgq_notify);
	init_completion(&dev->qmi.slave_notify);

	/* Register with framework */
	ret = slim_add_numbered_controller(&dev->ctrl);
	if (ret) {
		dev_err(dev->dev, "error adding controller\n");
		goto err_ioremap_failed;
	}

	dev->ctrl.dev.parent = &pdev->dev;
	dev->ctrl.dev.of_node = pdev->dev.of_node;
	dev->state = MSM_CTRL_DOWN;

	/*
	 * As this does not perform expensive
	 * operations, it can execute in an
	 * interrupt context. This avoids
	 * context switches, provides
	 * extensive benifits and performance
	 * improvements.
	 */
	ret = request_irq(dev->irq,
			ngd_slim_interrupt,
			IRQF_TRIGGER_HIGH,
			"ngd_slim_irq", dev);

	if (ret) {
		dev_err(&pdev->dev, "request IRQ failed\n");
		goto err_ioremap_failed;
	}

	init_completion(&dev->qmi.qmi_comp);
	dev->err = -EPROBE_DEFER;
	pm_runtime_use_autosuspend(dev->dev);
	pm_runtime_set_autosuspend_delay(dev->dev, MSM_SLIM_AUTOSUSPEND);
	pm_runtime_set_suspended(dev->dev);
	pm_runtime_enable(dev->dev);

	if (slim_mdm) {
		dev->ext_mdm.nb.notifier_call = mdm_ssr_notify_cb;
		dev->ext_mdm.domr = subsys_notif_register_notifier(ext_modem_id,
							&dev->ext_mdm.nb);
		if (IS_ERR_OR_NULL(dev->ext_mdm.domr))
			dev_err(dev->dev,
				"subsys_notif_register_notifier failed %p\n",
				dev->ext_mdm.domr);
	}

	INIT_WORK(&dev->dsp.dom_up, ngd_dom_up);
	pm_runtime_get_noresume(dev->dev);

	/* Fire up the Rx message queue thread */
	dev->rx_msgq_thread = kthread_run(ngd_slim_rx_msgq_thread, dev,
					"ngd_rx_thread%d", dev->ctrl.nr);
	if (IS_ERR(dev->rx_msgq_thread)) {
		ret = PTR_ERR(dev->rx_msgq_thread);
		dev_err(dev->dev, "Failed to start Rx thread:%d\n", ret);
		goto err_rx_thread_create_failed;
	}

	/* Start thread to probe, and notify slaves */
	dev->qmi.slave_thread = kthread_run(ngd_notify_slaves, dev,
					"ngd_notify_sl%d", dev->ctrl.nr);
	if (IS_ERR(dev->qmi.slave_thread)) {
		ret = PTR_ERR(dev->qmi.slave_thread);
		dev_err(dev->dev, "Failed to start notifier thread:%d\n", ret);
		goto err_notify_thread_create_failed;
	}
	SLIM_INFO(dev, "NGD SB controller is up!\n");
	return 0;

err_notify_thread_create_failed:
	kthread_stop(dev->rx_msgq_thread);
err_rx_thread_create_failed:
	free_irq(dev->irq, dev);
err_ioremap_failed:
	if (dev->sysfs_created)
		sysfs_remove_file(&dev->dev->kobj,
				&dev_attr_debug_mask.attr);
	kfree(dev->bulk.base);
err_nobulk:
	kfree(dev->wr_comp);
	kfree(dev);
	return ret;
}

static int ngd_slim_remove(struct platform_device *pdev)
{
	struct msm_slim_ctrl *dev = platform_get_drvdata(pdev);

	ngd_slim_enable(dev, false);
	if (dev->sysfs_created)
		sysfs_remove_file(&dev->dev->kobj,
				&dev_attr_debug_mask.attr);
	ngd_slim_qmi_svc_event_deinit(&dev->qmi);
	pm_runtime_disable(&pdev->dev);
	if (dev->dsp.dom_t == MSM_SLIM_DOM_SS)
		subsys_notif_unregister_notifier(dev->dsp.domr,
						&dev->dsp.nb);
	if (dev->dsp.dom_t == MSM_SLIM_DOM_PD)
		service_notif_unregister_notifier(dev->dsp.domr,
						&dev->dsp.nb);
	if (!IS_ERR_OR_NULL(dev->ext_mdm.domr))
		subsys_notif_unregister_notifier(dev->ext_mdm.domr,
						&dev->ext_mdm.nb);
	kfree(dev->bulk.base);
	free_irq(dev->irq, dev);
	slim_del_controller(&dev->ctrl);
	kthread_stop(dev->rx_msgq_thread);
	iounmap(dev->bam.base);
	iounmap(dev->base);
	kfree(dev->wr_comp);
	kfree(dev);
	return 0;
}

#ifdef CONFIG_PM
static int ngd_slim_runtime_idle(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct msm_slim_ctrl *dev = platform_get_drvdata(pdev);

	mutex_lock(&dev->tx_lock);
	if (dev->state == MSM_CTRL_AWAKE)
		dev->state = MSM_CTRL_IDLE;
	mutex_unlock(&dev->tx_lock);
	dev_dbg(device, "pm_runtime: idle...\n");
	pm_request_autosuspend(device);
	return -EAGAIN;
}
#endif

/*
 * If PM_RUNTIME is not defined, these 2 functions become helper
 * functions to be called from system suspend/resume. So they are not
 * inside ifdef CONFIG_PM_RUNTIME
 */
static int ngd_slim_runtime_resume(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct msm_slim_ctrl *dev = platform_get_drvdata(pdev);
	int ret = 0;

	mutex_lock(&dev->tx_lock);

	if (dev->qmi.deferred_resp) {
		SLIM_WARN(dev, "%s: RT resume called ahead of sys resume\n",
								__func__);
		ret = msm_slim_qmi_deferred_status_req(dev);
		if (ret)
			SLIM_WARN(dev, "%s: deferred resp failure\n",
								__func__);
		dev->qmi.deferred_resp = false;
	}

	if ((dev->state >= MSM_CTRL_ASLEEP) && (dev->qmi.handle != NULL))
		ret = ngd_slim_power_up(dev, false);
	if (ret || dev->qmi.handle == NULL) {
		/* Did SSR cause this power up failure */
		if (dev->state != MSM_CTRL_DOWN)
			dev->state = MSM_CTRL_ASLEEP;
		else
			SLIM_WARN(dev, "HW wakeup attempt during SSR\n");
	} else {
		dev->state = MSM_CTRL_AWAKE;
	}
	mutex_unlock(&dev->tx_lock);
	SLIM_INFO(dev, "Slim runtime resume: ret %d\n", ret);
	return ret;
}

#ifdef CONFIG_PM
static int ngd_slim_runtime_suspend(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);
	struct msm_slim_ctrl *dev = platform_get_drvdata(pdev);
	int ret = 0;

	mutex_lock(&dev->tx_lock);
	if (dev->qmi.handle != NULL) {
		ret = ngd_slim_power_down(dev);
	} else {
		if (dev->state == MSM_CTRL_DOWN)
			SLIM_INFO(dev, "SB rt suspend in SSR: %d\n",
								dev->state);
		else
			SLIM_INFO(dev, "SB rt suspend bad state: %d\n",
								dev->state);
		mutex_unlock(&dev->tx_lock);
		return ret;
	}
	if (ret && ret != -EBUSY)
		SLIM_INFO(dev, "slim resource not idle:%d\n", ret);
	if (!ret || ret == -ETIMEDOUT)
		dev->state = MSM_CTRL_ASLEEP;
	mutex_unlock(&dev->tx_lock);
	SLIM_INFO(dev, "Slim runtime suspend: ret %d\n", ret);
	return ret;
}
#endif

#ifdef CONFIG_PM_SLEEP
static int ngd_slim_suspend(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_slim_ctrl *cdev;

	if (of_device_is_compatible(pdev->dev.of_node,
				    "qcom,iommu-slim-ctrl-cb"))
		return 0;

	cdev = platform_get_drvdata(pdev);

	if (atomic_read(&cdev->init_in_progress)) {
		ret = -EBUSY;
		SLIM_INFO(cdev, "system suspend due to ssr: %d\n", ret);
		return ret;
	}

	if (cdev->state == MSM_CTRL_AWAKE) {
		ret = -EBUSY;
		SLIM_INFO(cdev, "system suspend: %d\n", ret);
		return ret;

	}
	if (!pm_runtime_enabled(dev) ||
		(!pm_runtime_suspended(dev) &&
			cdev->state == MSM_CTRL_IDLE)) {
		cdev->qmi.deferred_resp = true;
		ret = ngd_slim_runtime_suspend(dev);
		/*
		 * If runtime-PM still thinks it's active, then make sure its
		 * status is in sync with HW status.
		 */
		if (!ret) {
			pm_runtime_disable(dev);
			pm_runtime_set_suspended(dev);
			pm_runtime_enable(dev);
		} else {
			cdev->qmi.deferred_resp = false;
		}
	}
	SLIM_INFO(cdev, "system suspend state: %d\n", cdev->state);
	return ret;
}

static int ngd_slim_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_slim_ctrl *cdev;
	int ret = 0;

	if (of_device_is_compatible(pdev->dev.of_node,
				    "qcom,iommu-slim-ctrl-cb"))
		return 0;

	cdev = platform_get_drvdata(pdev);
	/*
	 * If deferred response was requested for power-off and it failed,
	 * mark runtime-pm status as active to be consistent
	 * with HW status
	 */
	mutex_lock(&cdev->tx_lock);
	if (cdev->qmi.deferred_resp) {
		ret = msm_slim_qmi_deferred_status_req(cdev);
		if (ret) {
			pm_runtime_disable(dev);
			pm_runtime_set_active(dev);
			pm_runtime_enable(dev);
		}
		cdev->qmi.deferred_resp = false;
	}
	/*
	 * Rely on runtime-PM to call resume in case it is enabled.
	 * Even if it's not enabled, rely on 1st client transaction to do
	 * clock/power on
	 */
	SLIM_INFO(cdev, "system resume state: %d\n", cdev->state);
	mutex_unlock(&cdev->tx_lock);
	return ret;
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops ngd_slim_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(
		ngd_slim_suspend,
		ngd_slim_resume
	)
	SET_RUNTIME_PM_OPS(
		ngd_slim_runtime_suspend,
		ngd_slim_runtime_resume,
		ngd_slim_runtime_idle
	)
};

static struct platform_driver ngd_slim_driver = {
	.probe = ngd_slim_probe,
	.remove = ngd_slim_remove,
	.driver	= {
		.name = NGD_SLIM_NAME,
		.pm = &ngd_slim_dev_pm_ops,
		.of_match_table = ngd_slim_dt_match,
	},
};

static int ngd_slim_init(void)
{
	return platform_driver_register(&ngd_slim_driver);
}
late_initcall(ngd_slim_init);

static void ngd_slim_exit(void)
{
	platform_driver_unregister(&ngd_slim_driver);
}
module_exit(ngd_slim_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM Slimbus controller");
MODULE_ALIAS("platform:msm-slim-ngd");
