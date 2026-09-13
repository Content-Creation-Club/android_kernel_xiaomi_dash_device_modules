// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/kthread.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <asm/unaligned.h>
#include <net/sock.h>
#include <net/netlink.h>
#include "xiaomi_touch_common.h"
#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
#include "../../../gpu/drm/mediatek/mediatek_v2/mi_disp/mi_disp_notifier.h"
#endif
//#include "../tp_get_lcm_name/tp_get_lcd_name.h"
#define NETLINK_TEST 24
#define MAX_MSGSIZE 16
int stringlength(char *s);
void sendnlmsg(char message);
static int pid = -1;
struct sock *nl_sk;
static struct xiaomi_touch_pdata *touch_pdata;
static struct xiaomi_touch *xiaomi_touch_device;
static struct xiaomi_touch_panel_data touch_panel_data[XIAOMI_TOUCH_MAX_PANEL];
/* Callback lifetime and mode data use separate locks: NVT callbacks read getters.
 * Lock order is callback -> mode; never drain work while holding either lock.
 */
/* ponytail: serialize both panels; split locks if dual-panel contention matters. */
static DEFINE_MUTEX(touch_callback_lock);
static DEFINE_MUTEX(touch_mode_lock);
/* Registration and teardown serialize here; workers/getters never take it. */
static DEFINE_MUTEX(touch_lifecycle_lock);
static common_data_t thp_ic_cmd_data_common_data;
static DEFINE_MUTEX(thp_ic_read_data_mutex);
static DEFINE_MUTEX(thp_ic_write_data_mutex);
static bool xiaomi_touch_probe_finished = false;
/* An old misc fd must not attach to a later common-device instance. */
static u64 touch_generation;
static void xiaomi_touch_unregister_panel(int touch_id);
#define RAW_SIZE (PAGE_SIZE * 12)

int current_log_level = 3;
static int vendor_log_level = 3;
static u64 ic_buffer_addr;
static int temperature_last = 1000;

int get_bms_temp_common(void)
{
	struct power_supply *bms;
	union power_supply_propval value = { 0 };

	bms = power_supply_get_by_name("bms");
	if (!bms)
		return -1000;
	if (power_supply_get_property(bms, POWER_SUPPLY_PROP_TEMP, &value))
		value.intval = -1000;
	power_supply_put(bms);
	return value.intval;
}
EXPORT_SYMBOL_GPL(get_bms_temp_common);

/* Lockless: raw-enable and recovery can call this while holding NVT's lock. */
int xiaomi_touch_get_charge_state(int touch_id)
{
	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return 0;
	return READ_ONCE(touch_panel_data[touch_id].charge_state);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_get_charge_state);

static int xiaomi_touch_get_charging_status(void)
{
	const char * const names[] = { "wireless", "usb" };
	union power_supply_propval value;
	struct power_supply *psy;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		psy = power_supply_get_by_name(names[i]);
		if (!psy)
			continue;
		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
		power_supply_put(psy);
		if (!ret && value.intval == 1)
			return 1;
	}
	return 0;
}

/* Caller holds touch_callback_lock; cache/notification also advance asleep. */
static void xiaomi_touch_charge_notify(struct xiaomi_touch_panel_data *panel,
		int charging)
{
	int touch_id = panel - touch_panel_data;

	WRITE_ONCE(panel->charge_state, charging);
	panel->charge_initialized = true;
	add_common_data_to_buf(touch_id, SET_CUR_VALUE, THP_HAL_CHARGING_STATUS,
			      1, &charging);
	if (panel->hardware_operation.set_charge_state)
		panel->hardware_operation.set_charge_state(charging);
}

static void xiaomi_touch_charge_work(struct work_struct *work)
{
	struct xiaomi_touch_panel_data *panel = container_of(work,
			struct xiaomi_touch_panel_data, charge_work);
	int charging;

	mutex_lock(&touch_callback_lock);
	if (panel->registered) {
		charging = xiaomi_touch_get_charging_status();
		if (!panel->charge_initialized || charging != panel->charge_state)
			xiaomi_touch_charge_notify(panel, charging);
	}
	mutex_unlock(&touch_callback_lock);
}

static int xiaomi_touch_charge_notifier(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct xiaomi_touch_panel_data *panel = container_of(nb,
			struct xiaomi_touch_panel_data, charge_notifier);

	/* Unregister synchronizes this notifier before the queue is drained. */
	if (READ_ONCE(panel->registered))
		queue_work(panel->pm_wq, &panel->charge_work);
	return NOTIFY_OK;
}

void enable_temperature_detection_func(bool enable)
{
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (!panel->registered || !panel->hardware_operation.set_thermal_temp)
		return;
	WRITE_ONCE(panel->temp_enabled, enable);
	wake_up_interruptible(&panel->temp_wait);
}
EXPORT_SYMBOL_GPL(enable_temperature_detection_func);

void stop_temperature_detection_func(void)
{
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (panel->temp_thread) {
		kthread_stop(panel->temp_thread);
		panel->temp_thread = NULL;
	}
}
EXPORT_SYMBOL_GPL(stop_temperature_detection_func);

static int xiaomi_touch_temp_thread_func(void *data)
{
	struct xiaomi_touch_panel_data *panel = data;
	int raw_temp;
	int temperature;
	unsigned int interval;

	while (!kthread_should_stop()) {
		wait_event_interruptible(panel->temp_wait,
			READ_ONCE(panel->temp_enabled) || kthread_should_stop());
		if (kthread_should_stop())
			break;
		if (!READ_ONCE(panel->temp_enabled))
			continue;
		raw_temp = get_bms_temp_common();
		temperature = (raw_temp + 5) / 10;
		if (abs(raw_temp) < 1000 &&
		    abs(temperature - temperature_last) >=
		    panel->hardware_param.temperature_change_threshold) {
			panel->hardware_operation.set_thermal_temp(temperature, false);
			/* Stock notifies and advances the cache even on IC failure. */
			add_common_data_to_buf(0, SET_CUR_VALUE, 0x446, 1, &temperature);
			temperature_last = temperature;
		}
		interval = xiaomi_touch_get_charge_state(0) ? 5000 : 10000;
		wait_event_interruptible_timeout(panel->temp_wait,
			kthread_should_stop(), msecs_to_jiffies(interval));
	}
	return 0;
}

static_assert(sizeof(hardware_param_t) == 0xd6);
static_assert(offsetof(hardware_param_t, temperature_change_threshold) == 0xd5);

static_assert(sizeof(hardware_operation_t) == 31 * sizeof(void *));
static_assert(offsetof(hardware_operation_t, set_cur_value) ==
		4 * sizeof(void *));
static_assert(offsetof(hardware_operation_t, enable_touch_raw) == 0x60);
static_assert(offsetof(hardware_operation_t, touch_doze_analysis) == 0x90);
static_assert(offsetof(hardware_operation_t, display_suspend_ready) ==
		20 * sizeof(void *));
static_assert(offsetof(hardware_operation_t, resume_suspend) ==
		23 * sizeof(void *));

static_assert(offsetof(hardware_operation_t, set_charge_state) ==
		24 * sizeof(void *));

/* Lock order: callback -> pm -> NVT lock -> mode (getter). No queue flush
 * under these locks. Mode 19 getters report the request, not a hardware ACK.
 */
static void xiaomi_touch_apply_pm(struct xiaomi_touch_panel_data *panel)
{
	int ret;

	if (!panel->registered || panel->suspended == !panel->pm_resume ||
	    !panel->hardware_operation.resume_suspend)
		return;
	/* NVT ignores the public gesture argument; its bit encoding differs. */
	ret = panel->hardware_operation.resume_suspend(panel->pm_resume, 0);
	if (ret) {
		dev_err(panel->dev, "touch PM failed: %d\n", ret);
		return;
	}
	if (panel->pm_resume) {
		xiaomi_touch_set_suspend_state(XIAOMI_TOUCH_RESUME);
		/* Query here too: a supply event may still be queued behind resume. */
		xiaomi_touch_charge_notify(panel, xiaomi_touch_get_charging_status());
	} else {
		if (panel->pm_async_suspend &&
		    panel->hardware_operation.display_suspend_ready)
			panel->hardware_operation.display_suspend_ready();
		xiaomi_touch_set_suspend_state(XIAOMI_TOUCH_SUSPEND);
	}
	panel->suspended = !panel->pm_resume;
}

static void xiaomi_touch_resume_work(struct work_struct *work)
{
	struct xiaomi_touch_panel_data *panel = container_of(work,
			struct xiaomi_touch_panel_data, resume_work);

	mutex_lock(&touch_callback_lock);
	mutex_lock(&panel->pm_lock);
	xiaomi_touch_apply_pm(panel);
	mutex_unlock(&panel->pm_lock);
	mutex_unlock(&touch_callback_lock);
}

/* Caller holds callback_lock. A pending worker always consumes the latest
 * target; a request after a running worker queues another pass. DRM low-power
 * transitions execute synchronously, without the mode19 display-ready call.
 */
static void xiaomi_touch_request_pm(struct xiaomi_touch_panel_data *panel,
		bool resume, bool asynchronous)
{
	if (!panel->registered)
		return;
	mutex_lock(&panel->pm_lock);
	panel->pm_resume = resume;
	panel->pm_async_suspend = asynchronous;
	if (asynchronous || resume)
		queue_work(panel->pm_wq, &panel->resume_work);
	else
		xiaomi_touch_apply_pm(panel);
	mutex_unlock(&panel->pm_lock);
}

#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
static int xiaomi_drm_panel_notifier_callback(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct xiaomi_touch_panel_data *panel = container_of(nb,
			struct xiaomi_touch_panel_data, panel_notifier);
	struct mi_disp_notifier *evdata = data;
	int blank;

	if (event != MI_DISP_DPMS_EVENT && event != MI_DISP_DPMS_EARLY_EVENT)
		return NOTIFY_DONE;
	if (!evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = *(int *)evdata->data;
	mutex_lock(&touch_callback_lock);
	if (!panel->registered || !panel->panel_notifier_registered)
		goto out;
	if (blank == MI_DISP_DPMS_LP1 || blank == MI_DISP_DPMS_LP2 ||
	    blank == MI_DISP_DPMS_POWERDOWN)
		xiaomi_touch_request_pm(panel, false, false);
	else if (event == MI_DISP_DPMS_EVENT && blank == MI_DISP_DPMS_ON)
		xiaomi_touch_request_pm(panel, true, false);
	else if (event == MI_DISP_DPMS_EARLY_EVENT && blank == MI_DISP_DPMS_OFF &&
		 panel->hardware_operation.display_suspend_ready)
		panel->hardware_operation.display_suspend_ready();
out:
	mutex_unlock(&touch_callback_lock);
	return NOTIFY_OK;
}
#endif

static int xiaomi_touch_register_panel_notifier(struct device *dev, int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
	int ret;
#endif

	if (!dev || touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return -EINVAL;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return -ENODEV;
	if (panel->panel_notifier_registered)
		return -EBUSY;

#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
	panel->panel_notifier.notifier_call =
		xiaomi_drm_panel_notifier_callback;
	ret = mi_disp_register_client(&panel->panel_notifier);
	if (ret)
		return ret;
	panel->panel_notifier_registered = true;
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}
int xiaomi_register_panel_notifier_common(struct device *dev, int touch_id)
{
	int ret;

	mutex_lock(&touch_lifecycle_lock);
	ret = xiaomi_touch_probe_finished ?
		xiaomi_touch_register_panel_notifier(dev, touch_id) : -ENODEV;
	mutex_unlock(&touch_lifecycle_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_register_panel_notifier_common);

static void xiaomi_touch_unregister_panel_notifier(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->panel_notifier_registered)
		return;

#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
	if (mi_disp_unregister_client(&panel->panel_notifier))
		dev_err(panel->dev, "failed to unregister display notifier\n");
#endif
	mutex_lock(&touch_callback_lock);
	panel->panel_notifier_registered = false;
	mutex_unlock(&touch_callback_lock);
}
void xiaomi_unregister_panel_notifier_common(int touch_id)
{
	mutex_lock(&touch_lifecycle_lock);
	xiaomi_touch_unregister_panel_notifier(touch_id);
	mutex_unlock(&touch_lifecycle_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_unregister_panel_notifier_common);

void sendnlmsg(char message)//char *message
{
	struct sk_buff *skb_1;
	struct nlmsghdr *nlh;
	int len = NLMSG_SPACE(MAX_MSGSIZE);
	int slen = 0;
	int ret = 0;
	if (!nl_sk || !pid) {
		return;
	}
	skb_1 = alloc_skb(len, GFP_KERNEL);
	if (!skb_1) {
		mi_ts_err("alloc_skb error\n");
		return;
	}
	slen = sizeof(message);
	nlh = nlmsg_put(skb_1, 0, 0, 0, MAX_MSGSIZE, 0);
	NETLINK_CB(skb_1).portid = 0;
	NETLINK_CB(skb_1).dst_group = 0;
//	message[slen] = '\0';
	memcpy(NLMSG_DATA(nlh), &message, slen);
	ret = netlink_unicast(nl_sk, skb_1, pid, MSG_DONTWAIT);
	if (!ret) {
		/*kfree_skb(skb_1); */
		mi_ts_err("send msg from kernel to usespace failed ret 0x%x\n",
		       ret);
	}
}
void nl_data_ready(struct sk_buff *__skb)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	char str[100];
	skb = skb_get(__skb);
	if (skb->len >= NLMSG_SPACE(0)) {
		nlh = nlmsg_hdr(skb);
		memcpy(str, NLMSG_DATA(nlh), sizeof(str));
		pid = nlh->nlmsg_pid;
		kfree_skb(skb);
	}
	mi_ts_info("netlink socket, pid =%d, msg = %d\n", pid, str[0]);
}
int netlink_init(void)
{
	struct netlink_kernel_cfg netlink_cfg;
	memset(&netlink_cfg, 0, sizeof(struct netlink_kernel_cfg));
	netlink_cfg.groups = 0;
	netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;
	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST, &netlink_cfg);
	if (!nl_sk) {
		mi_ts_err("create netlink socket error\n");
		return 1;
	}
	return 0;
}
void netlink_exit(void)
{
	if (nl_sk != NULL) {
		netlink_kernel_release(nl_sk);
		nl_sk = NULL;
	}
	mi_ts_info("self module exited\n");
}

struct xiaomi_touch_info_snapshot {
	char result[320];
	size_t length;
	bool valid;
};

static int xiaomi_touch_info_open(struct inode *inode, struct file *file)
{
	file->private_data = kzalloc(sizeof(struct xiaomi_touch_info_snapshot), GFP_KERNEL);
	return file->private_data ? 0 : -ENOMEM;
}

static int xiaomi_touch_info_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static void xiaomi_touch_format_info(struct xiaomi_touch_panel_data *panel,
				     const char *name,
				     struct xiaomi_touch_info_snapshot *snapshot)
{
	hardware_operation_t *ops = &panel->hardware_operation;
	hardware_param_t *param = &panel->hardware_param;
	char *buf = snapshot->result;
	size_t size = sizeof(snapshot->result);
	int len = 0;
	int i;

	if (!strncmp(name, "tp_fw_version", 13)) {
		if (ops->get_fw_version)
			ops->get_fw_version(param->fw_version);
		len = scnprintf(buf, size, "fw version: %s\ndriver version: %s\nhal version: %s\n"
			   "xiaomi-touch version: %s\n", param->fw_version,
			   param->driver_version, panel->hal_version, "2025.04.30-01");
		if (ops->limit_version_read) {
			ops->limit_version_read(panel->limit_version);
			len += scnprintf(buf + len, size - len, "limit csv version: %s\n",
					panel->limit_version);
		}
	} else if (!strncmp(name, "tp_lockdown_info", 16)) {
		if (ops->lockdown_info_read) {
			ops->lockdown_info_read(param->lockdown_info);
			for (i = 0; i < ARRAY_SIZE(param->lockdown_info); i++)
				len += scnprintf(buf + len, size - len, "0x%02X%s", param->lockdown_info[i],
					   i == 7 ? "\n" : ",");
		} else {
			len = scnprintf(buf, size, "null");
		}
	} else {
		len = scnprintf(buf, size, "%d\n", panel->self_test_result);
	}
	snapshot->length = len;
	snapshot->valid = true;
}

static ssize_t xiaomi_touch_info_read(struct file *file, char __user *buf,
				      size_t count, loff_t *pos)
{
	struct xiaomi_touch_panel_data *panel = pde_data(file_inode(file));
	struct xiaomi_touch_info_snapshot *snapshot = file->private_data;
	ssize_t ret;

	/* proc_remove runs down read/write/release before panel reuse. */
	mutex_lock(&touch_callback_lock);
	if (!panel->registered) {
		ret = -ENODEV;
		goto out;
	}
	if (!snapshot->valid)
		xiaomi_touch_format_info(panel, file->f_path.dentry->d_name.name, snapshot);
	ret = simple_read_from_buffer(buf, count, pos, snapshot->result,
				      snapshot->length);
out:
	mutex_unlock(&touch_callback_lock);
	return ret;
}

static ssize_t xiaomi_touch_info_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *pos)
{
	struct xiaomi_touch_panel_data *panel = pde_data(file_inode(file));
	const char *name = file->f_path.dentry->d_name.name;
	char command[64] = { 0 };
	int retry;
	ssize_t ret = count;

	mutex_lock(&touch_callback_lock);
	if (!panel->registered) {
		ret = -ENODEV;
		goto out;
	}
	if (!strncmp(name, "tp_fw_version", 13)) {
		memset(panel->hal_version, 0, sizeof(panel->hal_version));
		if (copy_from_user(panel->hal_version, buf,
				   min(count, sizeof(panel->hal_version) - 1)))
			ret = -EFAULT;
	} else if (!strncmp(name, "tp_selftest", 11)) {
		if (copy_from_user(command, buf, min(count, sizeof(command) - 1))) {
			ret = -EFAULT;
			goto out;
		}
		if (panel->hardware_operation.self_test) {
			/* Stock calls once, then performs up to five checked attempts. */
			panel->hardware_operation.self_test(command, &panel->self_test_result);
			for (retry = 0; retry < 5; retry++) {
				panel->hardware_operation.self_test(command, &panel->self_test_result);
				if (panel->self_test_result == 2)
					break;
			}
		}
		xiaomi_touch_format_info(panel, name, file->private_data);
		*pos = 0;
		file->f_pos = 0;
	}
out:
	mutex_unlock(&touch_callback_lock);
	return ret;
}

static const struct proc_ops xiaomi_touch_info_ops = {
	.proc_open = xiaomi_touch_info_open,
	.proc_release = xiaomi_touch_info_release,
	.proc_read = xiaomi_touch_info_read,
	.proc_write = xiaomi_touch_info_write,
	.proc_lseek = default_llseek,
};

static int xiaomi_touch_create_info_proc(struct xiaomi_touch_panel_data *panel,
				       int touch_id)
{
	static const char * const names[] = {
		"tp_fw_version", "tp_lockdown_info", "tp_selftest",
	};
	char name[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (touch_id)
			snprintf(name, sizeof(name), "%s_%d", names[i], touch_id);
		else
			strscpy(name, names[i], sizeof(name));
		panel->info_proc[i] = proc_create_data(name, 0644, NULL,
						     &xiaomi_touch_info_ops, panel);
		if (!panel->info_proc[i]) {
			while (i--) {
				proc_remove(panel->info_proc[i]);
				panel->info_proc[i] = NULL;
			}
			return -ENOMEM;
		}
	}
	return 0;
}

static void xiaomi_touch_init_modes(struct xiaomi_touch_panel_data *panel,
				    struct device_node *np)
{
	static const char * const properties[Touch_Report_Rate + 1] = {
		[Touch_Game_Mode] = "mi_tp,game-mode-array",
		[Touch_Active_MODE] = "mi_tp,active-mode-array",
		[Touch_UP_THRESHOLD] = "mi_tp,up-threshold-array",
		[Touch_Tolerance] = "mi_tp,tolerance-array",
		[Touch_Aim_Sensitivity] = "mi_tp,aim-sensitivity-array",
		[Touch_Tap_Stability] = "mi_tp,tap-stability-array",
		[Touch_Edge_Filter] = "mi_tp,edge-filter-array",
		[Touch_Panel_Orientation] = "mi_tp,panel-orien-array",
		[Touch_Report_Rate] = "mi_tp,report-rate-array",
	};
	u32 values[5];
	int i;

	memset(panel->touch_mode, 0, sizeof(panel->touch_mode));
	memset(panel->grid_value, 0, sizeof(panel->grid_value));
	panel->grid_length = 0;
	/* Stock parses expert parameters but never initializes mode 6 from them. */
	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		if (!properties[i] ||
		    of_property_read_u32_array(np, properties[i], values, 5))
			continue;
		panel->touch_mode[i][GET_MAX_VALUE] = values[0];
		panel->touch_mode[i][GET_MIN_VALUE] = values[1];
		panel->touch_mode[i][GET_DEF_VALUE] = values[2];
		panel->touch_mode[i][SET_CUR_VALUE] = values[3];
		panel->touch_mode[i][GET_CUR_VALUE] = values[4];
	}
}

int driver_get_touch_mode_common(int touch_id, int mode)
{
	int value = -1;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL ||
	    mode < 0 || mode >= Touch_Mode_NUM)
		return value;
	mutex_lock(&touch_mode_lock);
	if (touch_panel_data[touch_id].registered)
		value = touch_panel_data[touch_id].touch_mode[mode][GET_CUR_VALUE];
	mutex_unlock(&touch_mode_lock);
	return value;
}
EXPORT_SYMBOL_GPL(driver_get_touch_mode_common);

static void xiaomi_touch_get_mode(struct xiaomi_touch_panel_data *panel,
				  common_data_t *data)
{
	if (data->mode >= Touch_Mode_NUM)
		return;
	mutex_lock(&touch_mode_lock);
	if (data->cmd == GET_MODE_VALUE)
		memcpy(data->data_buf, &panel->touch_mode[data->mode][GET_CUR_VALUE],
		       4 * sizeof(int));
	else
		data->data_buf[0] = panel->touch_mode[data->mode][data->cmd];
	mutex_unlock(&touch_mode_lock);
}

static void xiaomi_touch_grid_work(struct work_struct *work)
{
	struct xiaomi_touch_panel_data *panel = container_of(work,
			struct xiaomi_touch_panel_data, grid_work);

	mutex_lock(&touch_callback_lock);
	/* Stock dash has no operation +0x50. Keep the fixed payload and work
	 * contract, but do not invent a grid writer or DT region conversion.
	 */
	if (panel->registered && panel->hardware_operation.reserved_50[0])
		mi_ts_err("grid provider is not implemented for this device");
	mutex_unlock(&touch_callback_lock);
}

static void xiaomi_touch_cmd_work(struct work_struct *work)
{
	struct xiaomi_touch_panel_data *panel = container_of(work,
			struct xiaomi_touch_panel_data, cmd_work);
	int snapshot[Touch_Mode_NUM];
	u64 changed = 0;
	int i;

	mutex_lock(&touch_callback_lock);
	if (!panel->registered)
		goto out;
	mutex_lock(&touch_mode_lock);
	for (i = 0; i < Touch_Mode_NUM; i++) {
		int *mode = panel->touch_mode[i];

		if (i <= Touch_Panel_Orientation) {
			if (mode[SET_CUR_VALUE] > mode[GET_MAX_VALUE])
				mode[SET_CUR_VALUE] = mode[GET_MAX_VALUE];
			else if (mode[SET_CUR_VALUE] < mode[GET_MIN_VALUE])
				mode[SET_CUR_VALUE] = mode[GET_MIN_VALUE];
		}
		if (mode[GET_CUR_VALUE] != mode[SET_CUR_VALUE]) {
			mode[GET_CUR_VALUE] = mode[SET_CUR_VALUE];
			changed |= BIT_ULL(i);
		}
		snapshot[i] = mode[GET_CUR_VALUE];
	}
	mutex_unlock(&touch_mode_lock);
	if (changed && panel->hardware_operation.cmd_mode_update)
		panel->hardware_operation.cmd_mode_update(changed, snapshot);
	if (changed & (BIT_ULL(Touch_Game_Mode) | BIT_ULL(Touch_Edge_Filter) |
		       BIT_ULL(Touch_Panel_Orientation)))
		queue_work(panel->mode_wq, &panel->grid_work);
out:
	mutex_unlock(&touch_callback_lock);
}

static void xiaomi_touch_switch_work(struct work_struct *work)
{
	struct xiaomi_touch_panel_data *panel = container_of(work,
			struct xiaomi_touch_panel_data, switch_work);

	mutex_lock(&touch_callback_lock);
	if (panel->registered && panel->hardware_operation.switch_mode)
		panel->hardware_operation.switch_mode();
	mutex_unlock(&touch_callback_lock);
}

/* Caller holds touch_callback_lock, including the queue operation. */
static void xiaomi_touch_set_mode(struct xiaomi_touch_panel_data *panel,
				 int touch_id, int mode, int *values)
{
	int notification;

	if (mode < 0 || values[0] < 0 || !panel->registered)
		return;
	if (mode >= Touch_Mode_NUM) {
		if (panel->hardware_operation.set_cur_value)
			panel->hardware_operation.set_cur_value(mode, values);
		return;
	}
	mutex_lock(&touch_mode_lock);
	panel->touch_mode[mode][SET_CUR_VALUE] = values[0];
	switch (mode) {
	case Touch_Fod_Enable:
	case Touch_Aod_Enable:
	case Touch_Doubletap_Mode:
	case Touch_FodIcon_Enable:
	case Touch_Nonui_Mode:
	case Touch_Debug_Level:
	case Touch_Power_Status:
		panel->touch_mode[mode][GET_CUR_VALUE] = values[0];
		break;
	}
	mutex_unlock(&touch_mode_lock);

	switch (mode) {
	case Touch_Game_Mode:
	case Touch_UP_THRESHOLD:
	case Touch_Tolerance:
	case Touch_Aim_Sensitivity:
	case Touch_Tap_Stability:
	case Touch_Edge_Filter:
	case Touch_Panel_Orientation:
		queue_work(panel->mode_wq, &panel->cmd_work);
		break;
	case Touch_Fod_Enable:
	case Touch_Aod_Enable:
	case Touch_Doubletap_Mode:
	case Touch_FodIcon_Enable:
	case Touch_Nonui_Mode:
		queue_work(panel->mode_wq, &panel->switch_work);
		break;
	case Touch_Fod_Setting:
	case Touch_Fingerprint_Auth_State:
		if (panel->hardware_operation.set_cur_value)
			panel->hardware_operation.set_cur_value(mode, values);
		break;
	case 31:
		notification = values[0] ? 100 : 101;
		add_common_data_to_buf(touch_id, SET_CUR_VALUE, Touch_Suspend,
				       1, &notification);
		break;
	case Touch_Power_Status:
		xiaomi_touch_request_pm(panel, !!values[0], true);
		break;
	case Touch_Debug_Level:
		WRITE_ONCE(current_log_level, values[0]);
		break;
	default:
		/* Stock retains requested; a later cmd work may commit it. */
		break;
	}
}

static void xiaomi_touch_reset_mode(struct xiaomi_touch_panel_data *panel,
				   int touch_id, int mode)
{
	int values[CMD_DATA_BUF_SIZE] = { 0 };
	int i;

	if (mode < 0 || mode >= Touch_Mode_NUM)
		return;
	if (mode) {
		mutex_lock(&touch_mode_lock);
		values[0] = panel->touch_mode[mode][GET_DEF_VALUE];
		mutex_unlock(&touch_mode_lock);
		xiaomi_touch_set_mode(panel, touch_id, mode, values);
		return;
	}
	/* Deliberately keep the existing game-only reset, not stock's broken
	 * 42-iteration fixed-address loop. Do not reset FOD or PM requests.
	 */
	mutex_lock(&touch_mode_lock);
	for (i = 0; i <= Touch_Report_Rate; i++)
		panel->touch_mode[i][SET_CUR_VALUE] = panel->touch_mode[i]
			[i == Touch_Panel_Orientation ? GET_CUR_VALUE : GET_DEF_VALUE];
	mutex_unlock(&touch_mode_lock);
	queue_work(panel->mode_wq, &panel->cmd_work);
}

static void xiaomi_touch_set_long_mode(struct xiaomi_touch_panel_data *panel,
				      common_data_t *data)
{
	if (data->mode != Touch_Grip_Mode || !data->data_len ||
	    data->data_len > CMD_DATA_BUF_SIZE)
		return;
	/* The ioctl copied the entire fixed payload, even when data_len is 96. */
	memcpy(panel->grid_value, data->data_buf, sizeof(panel->grid_value));
	panel->grid_length = data->data_len;
	queue_work(panel->mode_wq, &panel->grid_work);
}

static int xiaomi_touch_register_panel(struct device *dev, int touch_id,
		const hardware_param_t *hardware_param,
		const hardware_operation_t *hardware_operation)
{
	struct xiaomi_touch_panel_data *panel;
	size_t frame_total;
	size_t raw_total;
	int ret;

	if (!dev || !dev->of_node ||
	    touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL || !hardware_param)
		return -EINVAL;

	panel = &touch_panel_data[touch_id];
	if (panel->registered || panel->frame_data)
		return -EBUSY;

	if (!hardware_param->frame_data_page_size ||
	    !hardware_param->frame_data_buf_size ||
	    !hardware_param->raw_data_page_size ||
	    !hardware_param->raw_data_buf_size)
		return -EINVAL;

	frame_total = (size_t)hardware_param->frame_data_page_size * PAGE_SIZE *
		      hardware_param->frame_data_buf_size;
	raw_total = (size_t)hardware_param->raw_data_page_size * PAGE_SIZE *
		    hardware_param->raw_data_buf_size;

	panel->frame_data = kzalloc(frame_total, GFP_KERNEL);
	if (!panel->frame_data)
		return -ENOMEM;

	panel->raw_data = kzalloc(raw_total, GFP_KERNEL);
	if (!panel->raw_data) {
		kfree(panel->frame_data);
		panel->frame_data = NULL;
		return -ENOMEM;
	}

	memcpy(&panel->hardware_param, hardware_param, sizeof(*hardware_param));
	memset(&panel->hardware_operation, 0, sizeof(panel->hardware_operation));
	if (hardware_operation)
		memcpy(&panel->hardware_operation, hardware_operation,
		       sizeof(panel->hardware_operation));
	panel->frame_data_size =
		(size_t)hardware_param->frame_data_page_size * PAGE_SIZE;
	panel->frame_data_buf_size = hardware_param->frame_data_buf_size;
	panel->frame_data_phys = virt_to_phys(panel->frame_data);
	panel->raw_data_size =
		(size_t)hardware_param->raw_data_page_size * PAGE_SIZE;
	panel->raw_data_buf_size = hardware_param->raw_data_buf_size;
	panel->raw_data_phys = virt_to_phys(panel->raw_data);
	atomic_set(&panel->frame_data_index, 0);
	atomic_set(&panel->raw_data_index, 0);
	atomic_set(&panel->common_data_index, 0);
	INIT_LIST_HEAD(&panel->client_list);
	spin_lock_init(&panel->client_lock);
	mutex_init(&panel->common_data_lock);
	mutex_init(&panel->pm_lock);
	memset(panel->hal_version, 0, sizeof(panel->hal_version));
	memset(panel->limit_version, 0, sizeof(panel->limit_version));
	panel->self_test_result = 0;
	INIT_WORK(&panel->cmd_work, xiaomi_touch_cmd_work);
	INIT_WORK(&panel->switch_work, xiaomi_touch_switch_work);
	INIT_WORK(&panel->grid_work, xiaomi_touch_grid_work);
	panel->mode_wq = alloc_ordered_workqueue("xiaomi_touch_mode_%d",
			WQ_MEM_RECLAIM, touch_id);
	if (!panel->mode_wq) {
		kfree(panel->raw_data);
		kfree(panel->frame_data);
		panel->raw_data = NULL;
		panel->frame_data = NULL;
		memset(&panel->hardware_operation, 0, sizeof(panel->hardware_operation));
		return -ENOMEM;
	}
	panel->dev = dev;
	panel->suspended = false;
	panel->pm_resume = true;
	panel->pm_async_suspend = false;
	panel->charge_notifier_registered = false;
	panel->charge_initialized = false;
	panel->charge_state = xiaomi_touch_get_charging_status();
	INIT_WORK(&panel->resume_work, xiaomi_touch_resume_work);
	INIT_WORK(&panel->charge_work, xiaomi_touch_charge_work);
	panel->pm_wq = alloc_ordered_workqueue("xiaomi_touch_pm_%d",
			WQ_MEM_RECLAIM, touch_id);
	if (!panel->pm_wq) {
		destroy_workqueue(panel->mode_wq);
		panel->mode_wq = NULL;
		kfree(panel->raw_data);
		kfree(panel->frame_data);
		panel->raw_data = NULL;
		panel->frame_data = NULL;
		memset(&panel->hardware_operation, 0, sizeof(panel->hardware_operation));
		return -ENOMEM;
	}
	init_waitqueue_head(&panel->temp_wait);
	panel->temp_enabled = true;
	mutex_lock(&touch_callback_lock);
	mutex_lock(&touch_mode_lock);
	xiaomi_touch_init_modes(panel, dev->of_node);
	WRITE_ONCE(panel->registered, true);
	mutex_unlock(&touch_mode_lock);
	mutex_unlock(&touch_callback_lock);
	panel->charge_notifier.notifier_call = xiaomi_touch_charge_notifier;
	ret = power_supply_reg_notifier(&panel->charge_notifier);
	if (ret) {
		xiaomi_touch_unregister_panel(touch_id);
		return ret;
	}
	panel->charge_notifier_registered = true;
	queue_work(panel->pm_wq, &panel->charge_work);
	if (xiaomi_touch_create_info_proc(panel, touch_id)) {
		xiaomi_touch_unregister_panel(touch_id);
		return -ENOMEM;
	}
	if (touch_id == 0 && panel->hardware_operation.set_thermal_temp) {
		panel->temp_thread = kthread_run(xiaomi_touch_temp_thread_func,
					       panel, "xiaomi_touch_temp_thread");
		if (IS_ERR(panel->temp_thread)) {
			int ret = PTR_ERR(panel->temp_thread);

			panel->temp_thread = NULL;
			xiaomi_touch_unregister_panel(touch_id);
			return ret;
		}
	}

	return 0;
}
int register_touch_panel_common(struct device *dev, int touch_id,
		const hardware_param_t *hardware_param,
		const hardware_operation_t *hardware_operation)
{
	int ret;

	mutex_lock(&touch_lifecycle_lock);
	if (!xiaomi_touch_probe_finished)
		ret = -EPROBE_DEFER;
	else
		ret = xiaomi_touch_register_panel(dev, touch_id, hardware_param,
						 hardware_operation);
	mutex_unlock(&touch_lifecycle_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(register_touch_panel_common);

static void xiaomi_touch_remove_info_proc(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	int i;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;
	panel = &touch_panel_data[touch_id];
	for (i = 0; i < ARRAY_SIZE(panel->info_proc); i++) {
		proc_remove(panel->info_proc[i]);
		panel->info_proc[i] = NULL;
	}
}
void xiaomi_touch_remove_info_proc_common(int touch_id)
{
	mutex_lock(&touch_lifecycle_lock);
	xiaomi_touch_remove_info_proc(touch_id);
	mutex_unlock(&touch_lifecycle_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_remove_info_proc_common);

static void xiaomi_touch_stop_panel(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_client *client;
	struct xiaomi_touch_client *next;
	unsigned long flags;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;

	panel = &touch_panel_data[touch_id];
	/* Withdraw external producers before draining callbacks and work. */
	if (panel->charge_notifier_registered) {
		power_supply_unreg_notifier(&panel->charge_notifier);
		panel->charge_notifier_registered = false;
	}
	xiaomi_touch_unregister_panel_notifier(touch_id);
	mutex_lock(&touch_callback_lock);
	if (!panel->registered) {
		mutex_unlock(&touch_callback_lock);
		return;
	}
	/* Close requests and wait for in-flight ioctl/work/sysfs callbacks.
	 * The getter remains usable without taking touch_callback_lock.
	 */
	mutex_lock(&touch_mode_lock);
	WRITE_ONCE(panel->registered, false);
	mutex_unlock(&touch_mode_lock);
	panel->hardware_operation.cmd_mode_update = NULL;
	panel->hardware_operation.switch_mode = NULL;
	if (touch_pdata && touch_pdata->touch_data[touch_id]) {
		struct xiaomi_touch_interface *data = touch_pdata->touch_data[touch_id];

		/* Withdraw the contiguous callback section, not its live locks/data. */
		memset(&data->setModeValue, 0,
		       offsetof(struct xiaomi_touch_interface, long_mode_len) -
		       offsetof(struct xiaomi_touch_interface, setModeValue));
#if IS_ENABLED(CONFIG_MIEV)
		data->touch_dfs_test = NULL;
#endif
	}
	/* Revoke selected fds permanently before dropping callback lifetime. */
	spin_lock_irqsave(&panel->client_lock, flags);
	list_for_each_entry_safe(client, next, &panel->client_list, node) {
		list_del_init(&client->node);
		WRITE_ONCE(client->dead, true);
		client->touch_id = -1;
		wake_up_all(&client->poll_wait_queue_head);
	}
	spin_unlock_irqrestore(&panel->client_lock, flags);
	mutex_unlock(&touch_callback_lock);
	xiaomi_touch_remove_info_proc(touch_id);
	cancel_work_sync(&panel->resume_work);
	cancel_work_sync(&panel->charge_work);
	destroy_workqueue(panel->pm_wq);
	panel->pm_wq = NULL;
	cancel_work_sync(&panel->cmd_work);
	cancel_work_sync(&panel->switch_work);
	cancel_work_sync(&panel->grid_work);
	destroy_workqueue(panel->mode_wq);
	panel->mode_wq = NULL;
	if (touch_id == 0)
		stop_temperature_detection_func();

	mutex_lock(&touch_callback_lock);
	memset(&panel->hardware_operation, 0, sizeof(panel->hardware_operation));
	mutex_unlock(&touch_callback_lock);
}

void xiaomi_touch_stop_panel_common(int touch_id)
{
	mutex_lock(&touch_lifecycle_lock);
	xiaomi_touch_stop_panel(touch_id);
	mutex_unlock(&touch_lifecycle_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_stop_panel_common);

static void xiaomi_touch_unregister_panel(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;
	xiaomi_touch_stop_panel(touch_id);
	panel = &touch_panel_data[touch_id];
	/* The provider must stop its raw producer before releasing backing. */
	kfree(panel->raw_data);
	kfree(panel->frame_data);
	panel->raw_data = NULL;
	panel->frame_data = NULL;
}
void unregister_touch_panel_common(int touch_id)
{
	mutex_lock(&touch_lifecycle_lock);
	xiaomi_touch_unregister_panel(touch_id);
	mutex_unlock(&touch_lifecycle_lock);
}
EXPORT_SYMBOL_GPL(unregister_touch_panel_common);

void *get_raw_data_base_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return NULL;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return NULL;

	return (u8 *)panel->frame_data + atomic_read(&panel->frame_data_index) *
		panel->frame_data_size;
}
EXPORT_SYMBOL_GPL(get_raw_data_base_common);

void notify_raw_data_update_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_client *client;
	unsigned long flags;
	int index;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return;

	smp_wmb();
	index = atomic_inc_return(&panel->frame_data_index);
	if (index >= panel->frame_data_buf_size)
		atomic_set(&panel->frame_data_index, 0);

	spin_lock_irqsave(&panel->client_lock, flags);
	list_for_each_entry(client, &panel->client_list, node)
		wake_up_all(&client->poll_wait_queue_head);
	spin_unlock_irqrestore(&panel->client_lock, flags);
}
EXPORT_SYMBOL_GPL(notify_raw_data_update_common);

static int xiaomi_touch_dev_open(struct inode *inode, struct file *file)
{
	struct xiaomi_touch_client *client;
	int ret = 0;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	mutex_lock(&touch_callback_lock);
	if (!xiaomi_touch_probe_finished) {
		kfree(client);
		ret = -ENODEV;
	} else {
		INIT_LIST_HEAD(&client->node);
		client->touch_id = -1;
		client->generation = touch_generation;
		init_waitqueue_head(&client->poll_wait_queue_head);
		file->private_data = client;
	}
	mutex_unlock(&touch_callback_lock);
	return ret;
}

static bool xiaomi_touch_client_live(struct file *file)
{
	struct xiaomi_touch_client *client = file->private_data;

	return xiaomi_touch_probe_finished && client && !READ_ONCE(client->dead) &&
		client->generation == touch_generation;
}

static ssize_t xiaomi_touch_dev_read_locked(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	int index;

	if (!client || client->touch_id < 0 || count != sizeof(common_data_t))
		return 0;

	panel = &touch_panel_data[client->touch_id];
	if (!panel->registered)
		return 0;

	mutex_lock(&panel->common_data_lock);
	index = atomic_read(&client->common_data_index);
	if (index == atomic_read(&panel->common_data_index)) {
		mutex_unlock(&panel->common_data_lock);
		return 0;
	}
	if (copy_to_user(buf, &panel->common_data_buf[index], count)) {
		mutex_unlock(&panel->common_data_lock);
		return -1;
	}
	if (++index >= COMMON_DATA_BUF_SIZE)
		index = 0;
	atomic_set(&client->common_data_index, index);
	mutex_unlock(&panel->common_data_lock);

	return count;
}

static ssize_t xiaomi_touch_dev_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (xiaomi_touch_client_live(file))
		ret = xiaomi_touch_dev_read_locked(file, buf, count, pos);
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t xiaomi_touch_dev_write(struct file *file,
				const char __user *buf, size_t count, loff_t *pos)
{
	return count;
}
static unsigned int xiaomi_touch_dev_poll_locked(struct file *file,
		poll_table *wait)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	unsigned int mask = 0;

	if (!client || client->touch_id < 0)
		return 0;

	panel = &touch_panel_data[client->touch_id];
	if (!panel->registered)
		return 0;

	poll_wait(file, &client->poll_wait_queue_head, wait);
	if (atomic_read(&client->common_data_index) !=
	    atomic_read(&panel->common_data_index))
		mask |= POLLRDNORM;
	if (atomic_read(&client->frame_data_index) !=
	    atomic_read(&panel->frame_data_index))
		mask |= POLLPRI;
	if (atomic_read(&client->raw_data_index) !=
	    atomic_read(&panel->raw_data_index))
		mask |= POLLRDBAND;

	return mask;
}

static unsigned int xiaomi_touch_dev_poll(struct file *file, poll_table *wait)
{
	unsigned int ret = POLLHUP;

	mutex_lock(&touch_callback_lock);
	if (xiaomi_touch_client_live(file))
		ret = xiaomi_touch_dev_poll_locked(file, wait);
	mutex_unlock(&touch_callback_lock);
	return ret;
}
void notify_xiaomi_touch(struct xiaomi_touch_interface *touch_data)
{
	struct xiaomi_touch_pdata *client_private_data = NULL;
	if (!touch_data)
		return;
	spin_lock(&touch_data->private_data_lock);
	list_for_each_entry_rcu(client_private_data, &touch_data->private_data_list, node) {
		mi_ts_info("notify xiaomi-touch data update, client private data is %p", client_private_data);
		wake_up_all(&client_private_data->poll_wait_queue_head);
	}
	spin_unlock(&touch_data->private_data_lock);
}
void add_common_data_to_buf(s8 touch_id, enum common_data_cmd cmd, enum common_data_mode mode, int length, int *data)
{
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_client *client;
	common_data_t *common_data = NULL;
	unsigned long flags;
	int index;

	if (!READ_ONCE(xiaomi_touch_probe_finished)) {
		return;
	}
	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL ||
	    length < 0 || length > CMD_DATA_BUF_SIZE || (length && !data))
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return;

	mutex_lock(&panel->common_data_lock);
	index = atomic_read(&panel->common_data_index);
	common_data = &panel->common_data_buf[index];
	memset(common_data, 0, sizeof(*common_data));
	common_data->touch_id = touch_id;
	common_data->cmd = cmd;
	common_data->mode = mode;
	common_data->data_len = length;
	if (length)
		memcpy(common_data->data_buf, data, length * sizeof(s32));
	if (++index >= COMMON_DATA_BUF_SIZE)
		index = 0;
	atomic_set(&panel->common_data_index, index);
	mutex_unlock(&panel->common_data_lock);

	spin_lock_irqsave(&panel->client_lock, flags);
	list_for_each_entry(client, &panel->client_list, node)
		wake_up_all(&client->poll_wait_queue_head);
	spin_unlock_irqrestore(&panel->client_lock, flags);
}
EXPORT_SYMBOL_GPL(add_common_data_to_buf);
static long xiaomi_touch_dev_ioctl_locked(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	common_data_t common_data;
	void __user *argp = (void __user *)arg;
	unsigned long flags;
	int touch_id;
	int index;
	int ret = 0;

	if (!client || _IOC_TYPE(cmd) != 'T')
		return -EINVAL;

	switch (_IOC_NR(cmd)) {
	case COMMON_DATA_CMD:
		if (_IOC_SIZE(cmd) != sizeof(common_data_t))
			return -EINVAL;
		if (copy_from_user(&common_data, argp, sizeof(common_data)))
			return -1;
		touch_id = common_data.touch_id;
		if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL ||
		    !touch_pdata || !touch_pdata->touch_data[touch_id] ||
		    !touch_panel_data[touch_id].registered ||
		    client->touch_id != touch_id)
			return -1;
		if (common_data.data_len > CMD_DATA_BUF_SIZE)
			return -EINVAL;
		panel = &touch_panel_data[touch_id];

		switch (common_data.cmd) {
		case SET_CUR_VALUE:
			if (common_data.data_buf[0] < 0)
				break;
			if (common_data.mode == Touch_GAMETURBOTOOL_FOLLOWUP &&
			    common_data.data_buf[0] > 100 &&
			    common_data.data_buf[0] < 127) {
				sendnlmsg(common_data.data_buf[0]);
				break;
			}
			/* Stock keeps the request payload, not the callback status. */
			xiaomi_touch_set_mode(panel, touch_id, common_data.mode,
					      common_data.data_buf);
			break;
		case GET_CUR_VALUE:
		case GET_DEF_VALUE:
		case GET_MIN_VALUE:
		case GET_MAX_VALUE:
		case GET_MODE_VALUE:
			xiaomi_touch_get_mode(panel, &common_data);
			break;
		case RESET_MODE:
			xiaomi_touch_reset_mode(panel, touch_id, common_data.mode);
			break;
		case SET_LONG_VALUE:
			xiaomi_touch_set_long_mode(panel, &common_data);
			break;
		case SET_THP_IC_CUR_VALUE:
			if (panel->hardware_operation.htc_ic_set_mode_value)
				panel->hardware_operation.htc_ic_set_mode_value(&common_data);
			break;
		case GET_THP_IC_CUR_VALUE:
			if (!panel->hardware_operation.htc_ic_get_mode_value ||
			    panel->hardware_operation.htc_ic_get_mode_value(&common_data)) {
				ret = -1;
				break;
			}
			memcpy(&thp_ic_cmd_data_common_data, &common_data,
			       sizeof(common_data));
			break;
		case SET_CMD_FOR_THP:
			add_common_data_to_buf(touch_id, SET_CUR_VALUE,
					       common_data.mode, common_data.data_len,
					       common_data.data_buf);
			break;
		case SET_CMD_FOR_DRIVER:
			if (common_data.mode == Touch_Palm_Sensor)
				update_palm_sensor_value(common_data.data_buf[0]);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (!ret && ((common_data.cmd >= GET_CUR_VALUE &&
			      common_data.cmd <= GET_MODE_VALUE) ||
			     common_data.cmd == GET_THP_IC_CUR_VALUE)) {
			if (copy_to_user(argp, &common_data, sizeof(common_data))) {
				mi_ts_err("copyback failed for command %u", common_data.cmd);
				if (common_data.cmd == GET_THP_IC_CUR_VALUE)
					ret = -1;
			}
		}
		if (!ret && common_data.cmd != SET_CMD_FOR_THP &&
		    common_data.cmd != SET_CMD_FOR_DRIVER &&
		    (common_data.mode == THP_HAL_INIT_READY ||
		     common_data.mode == Touch_Suspend))
			add_common_data_to_buf(touch_id, common_data.cmd,
					       common_data.mode, common_data.data_len,
					       common_data.data_buf);
		return ret;

	case HARDWARE_PARAM_CMD:
		if (_IOC_SIZE(cmd) != sizeof(hardware_param_t))
			return -EINVAL;
		touch_id = client->touch_id;
		if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
			return -1;
		panel = &touch_panel_data[touch_id];
		if (!panel->registered)
			return -1;
		return copy_to_user(argp, &panel->hardware_param,
				    sizeof(panel->hardware_param));

	case SELECT_MMAP_CMD:
		client->mmap_area = arg;
		return 0;

	case SELECT_TOUCH_ID:
		if (client->touch_id >= 0)
			return 0;
		if (arg >= XIAOMI_TOUCH_MAX_PANEL)
			return -1;
		touch_id = arg;
		panel = &touch_panel_data[touch_id];
		if (!panel->registered)
			return -1;

		client->touch_id = touch_id;
		atomic_set(&client->frame_data_index,
			   atomic_read(&panel->frame_data_index));
		atomic_set(&client->raw_data_index,
			   atomic_read(&panel->raw_data_index));
		spin_lock_irqsave(&panel->client_lock, flags);
		list_add_tail(&client->node, &panel->client_list);
		spin_unlock_irqrestore(&panel->client_lock, flags);
		return 0;

	case GET_FRAME_DATA_INDEX:
		touch_id = client->touch_id;
		if (touch_id < 0)
			return -1;
		panel = &touch_panel_data[touch_id];
		if (!panel->registered)
			return -1;
		index = atomic_read(&client->frame_data_index);
		if (index == atomic_read(&panel->frame_data_index))
			return -1;
		ret = index;
		if (++index >= panel->frame_data_buf_size)
			index = 0;
		atomic_set(&client->frame_data_index, index);
		return ret;

	case RAW_DATA_INDEX:
		touch_id = client->touch_id;
		if (touch_id < 0)
			return -1;
		panel = &touch_panel_data[touch_id];
		if (!panel->registered)
			return -1;
		if (_IOC_DIR(cmd) == _IOC_READ) {
			index = atomic_read(&client->raw_data_index);
			if (index == atomic_read(&panel->raw_data_index))
				return -1;
			ret = index;
			if (++index >= panel->raw_data_buf_size)
				index = 0;
			atomic_set(&client->raw_data_index, index);
			return ret;
		}
		if (_IOC_DIR(cmd) == _IOC_WRITE) {
			smp_wmb();
			index = atomic_inc_return(&panel->raw_data_index);
			if (index >= panel->raw_data_buf_size)
				atomic_set(&panel->raw_data_index, 0);
			spin_lock_irqsave(&panel->client_lock, flags);
			list_for_each_entry(client, &panel->client_list, node)
				wake_up_all(&client->poll_wait_queue_head);
			spin_unlock_irqrestore(&panel->client_lock, flags);
			return 0;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static long xiaomi_touch_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -ENODEV;

	mutex_lock(&touch_callback_lock);
	if (xiaomi_touch_client_live(file))
		ret = xiaomi_touch_dev_ioctl_locked(file, cmd, arg);
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static int xiaomi_touch_dev_mmap_locked(struct file *file, struct vm_area_struct *vma)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	phys_addr_t phys;
	size_t total;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (!client || client->touch_id < 0)
		return -EINVAL;

	panel = &touch_panel_data[client->touch_id];
	if (!panel->registered)
		return -ENODEV;

	switch (client->mmap_area) {
	case XIAOMI_TOUCH_MMAP_FRAME:
		phys = panel->frame_data_phys;
		total = panel->frame_data_size * panel->frame_data_buf_size;
		break;
	case XIAOMI_TOUCH_MMAP_RAW:
		phys = panel->raw_data_phys;
		total = panel->raw_data_size * panel->raw_data_buf_size;
		break;
	default:
		return -EINVAL;
	}

	if (offset > total || size > total - offset)
		return -EINVAL;

	if (remap_pfn_range(vma, vma->vm_start,
			    (phys + offset) >> PAGE_SHIFT, size,
			    PAGE_SHARED))
		return -EAGAIN;

	return 0;
}

static int xiaomi_touch_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret = -ENODEV;

	mutex_lock(&touch_callback_lock);
	if (xiaomi_touch_client_live(file))
		ret = xiaomi_touch_dev_mmap_locked(file, vma);
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static int xiaomi_touch_dev_release(struct inode *inode, struct file *file)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	unsigned long flags;

	if (!client)
		return 0;

	mutex_lock(&touch_callback_lock);
	if (!READ_ONCE(client->dead) && client->touch_id >= 0 &&
	    client->generation == touch_generation) {
		panel = &touch_panel_data[client->touch_id];
		spin_lock_irqsave(&panel->client_lock, flags);
		if (!list_empty(&client->node))
			list_del_init(&client->node);
		spin_unlock_irqrestore(&panel->client_lock, flags);
	}

	mutex_unlock(&touch_callback_lock);
	kfree(client);
	file->private_data = NULL;
	return 0;
}
static const struct file_operations xiaomitouch_dev_fops = {
	.owner = THIS_MODULE,
	.open = xiaomi_touch_dev_open,
	.read = xiaomi_touch_dev_read,
	.write = xiaomi_touch_dev_write,
	.poll = xiaomi_touch_dev_poll,
	.mmap = xiaomi_touch_dev_mmap,
	.unlocked_ioctl = xiaomi_touch_dev_ioctl,
	.compat_ioctl = xiaomi_touch_dev_ioctl,
	.release = xiaomi_touch_dev_release,
	.llseek	= no_llseek,
};
static struct xiaomi_touch xiaomi_touch_dev = {
	.misc_dev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "xiaomi-touch",
		.fops = &xiaomitouch_dev_fops,
		.parent = NULL,
	},
	.mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.mutex),
	.palm_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.palm_mutex),
	.prox_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.prox_mutex),
	.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(xiaomi_touch_dev.wait_queue),
#ifdef	TOUCH_FOD_SUPPORT
	.fod_press_status_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.fod_press_status_mutex),
#endif
	.gesture_single_tap_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.gesture_single_tap_mutex),
	.abnormal_event_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.abnormal_event_mutex),
};
struct xiaomi_touch *xiaomi_touch_dev_get(int minor)
{
	if (xiaomi_touch_dev.misc_dev.minor == minor)
		return &xiaomi_touch_dev;
	else
		return NULL;
}
struct class *get_xiaomi_touch_class(void)
{
	return xiaomi_touch_dev.class;
}
EXPORT_SYMBOL_GPL(get_xiaomi_touch_class);
struct device *get_xiaomi_touch_dev(void)
{
	return xiaomi_touch_dev.dev;
}
EXPORT_SYMBOL_GPL(get_xiaomi_touch_dev);
int xiaomitouch_register_modedata(int touchId, struct xiaomi_touch_interface *data)
{
	int ret = 0;
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!data || touchId < 0 || touchId >= XIAOMI_TOUCH_MAX_PANEL)
		return -EINVAL;
	mutex_lock(&touch_callback_lock);
	if (!xiaomi_touch_probe_finished || !touch_pdata ||
	    !touch_panel_data[touchId].registered) {
		mutex_unlock(&touch_callback_lock);
		return -ENODEV;
	}
	touch_data = touch_pdata->touch_data[touchId];
	if (data->setModeValue)
		touch_data->setModeValue = data->setModeValue;
	if (data->getModeValue)
		touch_data->getModeValue = data->getModeValue;
	if (data->resetMode)
		touch_data->resetMode = data->resetMode;
	if (data->getModeAll)
		touch_data->getModeAll = data->getModeAll;
	if (data->palm_sensor_read)
		touch_data->palm_sensor_read = data->palm_sensor_read;
	if (data->palm_sensor_write)
		touch_data->palm_sensor_write = data->palm_sensor_write;
	if (data->prox_sensor_read)
		touch_data->prox_sensor_read = data->prox_sensor_read;
	if (data->prox_sensor_write)
		touch_data->prox_sensor_write = data->prox_sensor_write;
	if (data->panel_vendor_read)
		touch_data->panel_vendor_read = data->panel_vendor_read;
	if (data->panel_color_read)
		touch_data->panel_color_read = data->panel_color_read;
	if (data->panel_display_read)
		touch_data->panel_display_read = data->panel_display_read;
	if (data->touch_vendor_read)
		touch_data->touch_vendor_read = data->touch_vendor_read;
	if (data->setModeLongValue)
		touch_data->setModeLongValue = data->setModeLongValue;
	if (data->get_touch_rx_num)
		touch_data->get_touch_rx_num = data->get_touch_rx_num;
	if (data->get_touch_tx_num)
		touch_data->get_touch_tx_num = data->get_touch_tx_num;
	if (data->get_touch_freq_num)
		touch_data->get_touch_freq_num = data->get_touch_freq_num;
	if (data->get_touch_x_resolution)
		touch_data->get_touch_x_resolution = data->get_touch_x_resolution;
	if (data->get_touch_y_resolution)
		touch_data->get_touch_y_resolution = data->get_touch_y_resolution;
	if (data->enable_touch_raw)
		touch_data->enable_touch_raw = data->enable_touch_raw;
	if (data->enable_touch_delta)
		touch_data->enable_touch_delta = data->enable_touch_delta;
	if (data->enable_clicktouch_raw)
		touch_data->enable_clicktouch_raw = data->enable_clicktouch_raw;
	if (data->get_touch_super_resolution_factor)
		touch_data->get_touch_super_resolution_factor = data->get_touch_super_resolution_factor;
	if (data->set_up_interrupt_mode)
		touch_data->set_up_interrupt_mode = data->set_up_interrupt_mode;
	if (data->touch_doze_analysis)
		touch_data->touch_doze_analysis = data->touch_doze_analysis;
	if (data->get_touch_ic_buffer)
		touch_data->get_touch_ic_buffer = data->get_touch_ic_buffer;
	if (data->fod_test_store)
		touch_data->fod_test_store = data->fod_test_store;
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 start*/
	if (data->touch_dfs_test)
		touch_data->touch_dfs_test = data->touch_dfs_test;
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 end*/
	mutex_unlock(&touch_callback_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xiaomitouch_register_modedata);
int update_palm_sensor_value(int value)
{
	struct xiaomi_touch *dev = NULL;
	mutex_lock(&xiaomi_touch_dev.palm_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.palm_mutex);
		return -ENODEV;
	}
	dev = touch_pdata->device;
	if (value != touch_pdata->palm_value) {
		mi_ts_info("value:%d\n", value);
		touch_pdata->palm_value = value;
		touch_pdata->palm_changed = true;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			     "palm_sensor");
	}
	mutex_unlock(&xiaomi_touch_dev.palm_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(update_palm_sensor_value);
static ssize_t palm_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	pdata->palm_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->palm_value);
}
static ssize_t palm_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	pdata->touch_data[0]->palm_sensor_onoff = input;
	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->palm_sensor_write)
		pdata->touch_data[0]->palm_sensor_write(!!input);
	else
		mi_ts_err("has not implement\n");
	mutex_unlock(&touch_callback_lock);
	mi_ts_info("value:%d\n", !!input);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "palm_sensor_data");
	return count;
}
static ssize_t palm_sensor_data_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	pdata->palm_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->palm_sensor_onoff);
}
static ssize_t palm_sensor_data_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	mutex_lock(&xiaomi_touch_dev.palm_mutex);
	if (sscanf(buf, "%d", &input) < 0) {
		mutex_unlock(&xiaomi_touch_dev.palm_mutex);
		return -EINVAL;
	}
	if (input != pdata->palm_value) {
		mi_ts_info("value:%d\n", input);
		pdata->palm_value = input;
		pdata->palm_changed = true;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			"palm_sensor");
	}
	mutex_unlock(&xiaomi_touch_dev.palm_mutex);
	return count;
}
static ssize_t touch_sensor_ctrl_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->touch_sensor_ctrl_value);
}
static ssize_t touch_sensor_ctrl_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		mi_ts_info("get input error\n");
		return -EINVAL;
	}
	pdata->touch_data[0]->touch_sensor_ctrl_value = input;
	mi_ts_info("touch sensor ctrl %d\n", input);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_sensor_ctrl");
	return count;
}
static ssize_t touch_sensor_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}
static ssize_t touch_sensor_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	if (input >= 0)
		thp_send_cmd_to_hal(THP_HAL_TOUCH_SENSOR, input);
	mutex_lock(&touch_callback_lock);
	if (input == 0 && pdata->touch_data[0]->set_up_interrupt_mode)
		pdata->touch_data[0]->set_up_interrupt_mode(0);
	mutex_unlock(&touch_callback_lock);
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_testmode_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->thp_test_mode);
}
static ssize_t touch_testmode_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	pdata->touch_data[0]->thp_test_mode = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_testmode");
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_preset_point_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->thp_preset_point);
}
static ssize_t touch_testresult_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	pdata->touch_data[0]->thp_test_result = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_testresult");
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_testresult_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->thp_test_result);
}
static ssize_t touch_preset_point_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	pdata->touch_data[0]->thp_preset_point = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_preset_point");
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_thp_mem_notify_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}
static ssize_t touch_thp_mem_notify_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (buf[0] == '1')
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_mem_notify");
	return count;
}
int update_prox_sensor_value(int value)
{
	struct xiaomi_touch *dev = NULL;
	mutex_lock(&xiaomi_touch_dev.prox_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.prox_mutex);
		return -ENODEV;
	}
	dev = touch_pdata->device;
	if (value != touch_pdata->prox_value) {
		mi_ts_info("value:%d\n", value);
		touch_pdata->prox_value = value;
		touch_pdata->prox_changed = true;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			     "prox_sensor");
	}
	mutex_unlock(&xiaomi_touch_dev.prox_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(update_prox_sensor_value);
static ssize_t prox_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	pdata->prox_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->prox_changed);
}
static ssize_t prox_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->prox_sensor_write)
		pdata->touch_data[0]->prox_sensor_write(!!input);
	else
		mi_ts_err("has not implement\n");
	mutex_unlock(&touch_callback_lock);
	mi_ts_info("value:%d\n", !!input);
	return count;
}
static ssize_t touch_log_level_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"echo [value0] [value1] > touch_log_level\n"
		"[value0]: control xiaomi_touch log level, 0:Always,1:Error,2:Warning,3:Info,4:Debug,5:Verbose\n"
		"[value1]: control vendor driver log level, 0:Always,1:Error,2:Warning,3:Info,4:Debug,5:Verbose\n"
		"current xiaomi_touch log_level = %d, vendor log_level = %d\n",
		READ_ONCE(current_log_level), READ_ONCE(vendor_log_level));
}

static ssize_t touch_log_level_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int common = 0;

	if (sscanf(buf, "%d %d", &common, &vendor_log_level) < 0)
		return -EINVAL;
	WRITE_ONCE(current_log_level, common);
	return count;
}

static ssize_t touch_ic_buffer_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t count = 0;
	u8 *tmp_buf = NULL;

	mi_ts_info("address: 0x%llx", (unsigned long long)ic_buffer_addr);
	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->get_touch_ic_buffer) {
		tmp_buf = pdata->touch_data[0]->get_touch_ic_buffer();
		if (tmp_buf)
			count = snprintf(buf, PAGE_SIZE, "%s", tmp_buf);
	}
	if (tmp_buf)
		vfree(tmp_buf);
	mutex_unlock(&touch_callback_lock);
	return count;
}

static ssize_t touch_ic_buffer_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long long address = READ_ONCE(ic_buffer_addr);

	if (sscanf(buf, "0x%llX", &address) < 0)
		return -EINVAL;
	WRITE_ONCE(ic_buffer_addr, address);
	return count;
}
static int touch_doze_analysis_result;

static ssize_t touch_doze_analysis_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", touch_doze_analysis_result);
}
static ssize_t touch_doze_analysis_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input = 0, i;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mutex_lock(&touch_callback_lock);
	for (i = 0; i < ARRAY_SIZE(touch_panel_data); i++) {
		if (touch_panel_data[i].registered &&
		    touch_panel_data[i].hardware_operation.touch_doze_analysis)
			touch_doze_analysis_result =
				touch_panel_data[i].hardware_operation.touch_doze_analysis(input);
		else if (pdata->touch_data[i] && pdata->touch_data[i]->touch_doze_analysis)
			touch_doze_analysis_result = pdata->touch_data[i]->touch_doze_analysis(input);
	}
	mutex_unlock(&touch_callback_lock);
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t panel_vendor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->panel_vendor_read)
		ret = snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->panel_vendor_read());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t panel_color_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->panel_color_read)
		ret = snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->panel_color_read());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t panel_display_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->panel_display_read)
		ret = snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->panel_display_read());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t touch_vendor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->touch_vendor_read)
		ret = snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->touch_vendor_read());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t xiaomi_touch_tx_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->get_touch_tx_num)
		ret = snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_tx_num());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t xiaomi_touch_rx_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->get_touch_rx_num)
		ret = snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_rx_num());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t xiaomi_touch_freq_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->get_touch_freq_num)
		ret = snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_freq_num());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t xiaomi_touch_x_resolution_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->get_touch_x_resolution)
		ret = snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_x_resolution());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static ssize_t xiaomi_touch_y_resolution_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t ret = 0;

	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->get_touch_y_resolution)
		ret = snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_y_resolution());
	mutex_unlock(&touch_callback_lock);
	return ret;
}
static int copy_touch_rawdata_locked(char *raw_base,  int len)
{
	struct xiaomi_touch *dev = NULL;
	if (!touch_pdata)
		return -ENODEV;
	dev = touch_pdata->device;
#ifdef TOUCH_THP_SUPPORT
	memcpy((unsigned char *)touch_pdata->raw_buf[touch_pdata->raw_tail],
			(unsigned char *)raw_base, len);
	touch_pdata->raw_len = len;
	spin_lock(&touch_pdata->raw_lock);
	touch_pdata->raw_tail++;
	if (touch_pdata->raw_tail == RAW_BUF_NUM)
		touch_pdata->raw_tail = 0;
	spin_unlock(&touch_pdata->raw_lock);
#else
	// 使用共享内存touch_pdata->raw_data向touch tool apk传递数据
	if (!touch_pdata->raw_data)
		return -ENOMEM;
	memcpy((unsigned char *)touch_pdata->raw_data, (unsigned char *)raw_base, len);
#endif
	return 0;
}

int copy_touch_rawdata(char *raw_base, int len)
{
	int ret = -ENODEV;

	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		ret = copy_touch_rawdata_locked(raw_base, len);
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(copy_touch_rawdata);
void get_current_timestamp(char* timebuf, int len)
{
	struct rtc_time tm;
	struct timespec64 tv = { 0 };
	/* android time */
	struct rtc_time tm_android;
	struct timespec64 tv_android = { 0 };
	ktime_get_real_ts64(&tv);
	tv_android = tv;
	rtc_time64_to_tm(tv.tv_sec, &tm);
	tv_android.tv_sec -= sys_tz.tz_minuteswest * 60;
	rtc_time64_to_tm(tv_android.tv_sec, &tm_android);
	if(len >= 24) {
		snprintf(timebuf, 24, "%04d-%02d-%02d-%02d-%02d-%02d.%03d",
			tm_android.tm_year + 1900,tm_android.tm_mon + 1,
			tm_android.tm_mday, tm_android.tm_hour,
			tm_android.tm_min, tm_android.tm_sec,
			(unsigned int)(tv_android.tv_nsec / 1000));
	} else {
		mi_ts_info("%02d-%02d-%02d %02d:%02d:%02d.%03d(android time)\n",
			tm_android.tm_year + 1900,tm_android.tm_mon + 1,
			tm_android.tm_mday, tm_android.tm_hour,
			tm_android.tm_min, tm_android.tm_sec,
			(unsigned int)(tv_android.tv_nsec / 1000));
	}
}
EXPORT_SYMBOL_GPL(get_current_timestamp);
static int update_touch_rawdata_locked(void)
{
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "update_rawdata");
	return 0;
}

int update_touch_rawdata(void)
{
	int ret = -ENODEV;

	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		ret = update_touch_rawdata_locked();
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(update_touch_rawdata);
static ssize_t enable_touchraw_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input = 0;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	mutex_lock(&touch_callback_lock);
	if (touch_panel_data[0].registered &&
	    touch_panel_data[0].hardware_operation.enable_touch_raw)
		touch_panel_data[0].hardware_operation.enable_touch_raw(input);
	else if (touch_data->enable_touch_raw)
		touch_data->enable_touch_raw(input);
	mutex_unlock(&touch_callback_lock);
	touch_data->is_enable_touchraw = input;
	touch_pdata->raw_tail = 0;
	touch_pdata->raw_head = 0;
	return count;
}
static ssize_t enable_touchraw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->is_enable_touchraw);
}
static ssize_t enable_touchdelta_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	mutex_lock(&touch_callback_lock);
	if (touch_data->enable_touch_delta)
		touch_data->enable_touch_delta(!!input);
	touch_data->is_enable_touchdelta = !!input;
	mutex_unlock(&touch_callback_lock);
	return count;
}
static ssize_t enable_touchdelta_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->is_enable_touchdelta);
}
static ssize_t thp_cmd_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	mutex_lock(&dev->mutex);
	if (!touch_pdata) {
		mutex_unlock(&dev->mutex);
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_buf, touch_data->thp_cmd_size * sizeof(int));
	if (touch_pdata->param_head == touch_pdata->param_tail
		&& touch_pdata->param_flag == 0) {
		mi_ts_err("param buffer is empty!\n");
		mutex_unlock(&dev->mutex);
		return -EINVAL;
	}
	spin_lock(&touch_pdata->param_lock);
	memcpy(buf, touch_pdata->touch_cmd_data[touch_pdata->param_head]->param_buf,
		touch_pdata->touch_cmd_data[touch_pdata->param_head]->thp_cmd_size * sizeof(int));
	touch_pdata->param_head++;
	if (touch_pdata->param_head == PARAM_BUF_NUM)
		touch_pdata->param_head = 0;
	if (touch_pdata->param_head == touch_pdata->param_tail)
		touch_pdata->param_flag = 0;
	spin_unlock(&touch_pdata->param_lock);
	if (touch_pdata->param_head != touch_pdata->param_tail)
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_cmd");
#ifdef TOUCH_THP_SUPPORT
	touch_data->touch_event_status = 0;
	wake_up_all(&touch_data->wait_queue);
#endif
	mutex_unlock(&dev->mutex);
	return touch_data->thp_cmd_size * sizeof(int);
}
static ssize_t thp_cmd_status_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input[MAX_BUF_SIZE];
	const char *p = buf;
	bool new_data = false;
	int para_cnt = 0;
	int i = 0;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	memset(input, 0x00, sizeof(int) * MAX_BUF_SIZE);
	for (p = buf; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			input[i] = input[i] * 10 + (*p - '0');
			if (!new_data) {
				new_data = true;
				para_cnt++;
			}
		} else if (*p == ' ' || *p == ',') {
			if (new_data) {
				i++;
				new_data = false;
			}
		} else {
			break;
		}
	}
	mi_ts_info("size:%d, cmd:%d, %d, %d, %d\n", para_cnt, input[0], input[1], input[2], input[3]);
	if (sizeof(int) * para_cnt < MAX_BUF_SIZE) {
		for (i = 0; i < para_cnt; i++) {
			touch_data->thp_cmd_buf[i] = input[i];
		}
		touch_data->thp_cmd_size = para_cnt;
		touch_data->touch_event_status = 1;
		if (touch_pdata->param_head == touch_pdata->param_tail
			&& touch_pdata->param_flag == 1) {
			mi_ts_err("param buffer is full!\n");
			return -ENOMEM;
		}
		spin_lock(&touch_pdata->param_lock);
		touch_pdata->touch_cmd_data[touch_pdata->param_tail]->thp_cmd_size = touch_data->thp_cmd_size;
		memcpy((unsigned char*)touch_pdata->touch_cmd_data[touch_pdata->param_tail]->param_buf,
			(unsigned char*)touch_data->thp_cmd_buf, sizeof(int) * touch_data->thp_cmd_size);
		touch_pdata->param_tail++;
		if (touch_pdata->param_tail == PARAM_BUF_NUM)
			touch_pdata->param_tail = 0;
		if (touch_pdata->param_head == touch_pdata->param_tail)
			touch_pdata->param_flag = 1;
		spin_unlock(&touch_pdata->param_lock);
		sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd");
#ifdef TOUCH_THP_SUPPORT
		if (wait_event_interruptible_timeout(touch_data->wait_queue,
				!touch_data->touch_event_status, msecs_to_jiffies(100)) <= 0) {
			mi_ts_err("thp read timeout, skip this event, status:%d\n", touch_data->touch_event_status);
		}
#endif
	} else {
		mi_ts_info("memory overlow\n");
	}
	return count;
}
static ssize_t thp_cmd_ready_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	mutex_lock(&dev->mutex);
	if (!touch_pdata) {
		mutex_unlock(&dev->mutex);
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_ready_buf, touch_data->thp_cmd_ready_size * sizeof(int));
#if 0
	touch_data->touch_event_ready_status = 0;
	wake_up_all(&touch_data->wait_queue_ready);
#endif
	mutex_unlock(&dev->mutex);
	return touch_data->thp_cmd_ready_size * sizeof(int);
}
static ssize_t thp_cmd_ready_status_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input[MAX_BUF_SIZE];
	const char *p = buf;
	bool new_data = false;
	int para_cnt = 0;
	int i = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memset(input, 0x00, sizeof(int) * MAX_BUF_SIZE);
	for (p = buf; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			input[i] = input[i] * 10 + (*p - '0');
			if (!new_data) {
				new_data = true;
				para_cnt++;
			}
		} else if (*p == ' ' || *p == ',') {
			if (new_data) {
				i++;
				new_data = false;
			}
		} else {
			break;
		}
	}
	mi_ts_info("size:%d, cmd:%d, %d, %d, %d\n",
				para_cnt, input[0], input[1], input[2], input[3]);
	if (sizeof(int) * para_cnt < MAX_BUF_SIZE) {
		for (i = 0; i < para_cnt; i++) {
			touch_data->thp_cmd_ready_buf[i] = input[i];
		}
		touch_data->thp_cmd_ready_size = para_cnt;
		touch_data->touch_event_ready_status = 1;
		sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd_ready");
#if 0
		if (wait_event_interruptible_timeout(touch_data->wait_queue_ready,
			!touch_data->touch_event_ready_status, msecs_to_jiffies(100)) <= 0) {
			mi_ts_err("%s thp read timeout, skip this event, status:%d\n", __func__,
						touch_data->touch_event_ready_status);
		}
#endif
	} else {
		mi_ts_info("memory overlow\n");
	}
	return count;
}
static ssize_t thp_cmd_data_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_data_buf, touch_data->thp_cmd_data_size);
	return touch_data->thp_cmd_data_size;
}
static ssize_t thp_cmd_data_store(struct device *dev,
				  struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (count > MAX_BUF_SIZE) {
		mi_ts_info("memory out of range:%d\n", (int)count);
		return count;
	}
	mi_ts_info("buf:%s, size:%d\n", buf, (int)count);
	memcpy(touch_data->thp_cmd_data_buf, buf, count);
	touch_data->thp_cmd_data_size = count;
	sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd_data");
	return count;
}
static ssize_t touch_thp_ic_cmd_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input[CMD_DATA_BUF_SIZE] = { 0 };
	unsigned int value = 0;
	bool in_number = false;
	int n = 0;
	int length;
	size_t i;

	for (i = 0; i < count && buf[i]; i++) {
		unsigned int digit;

		if (buf[i] >= '0' && buf[i] <= '9') {
			digit = buf[i] - '0';
			if (!in_number) {
				if (n == ARRAY_SIZE(input))
					return -EINVAL;
				value = 0;
				in_number = true;
			}
			if (value > (INT_MAX - digit) / 10)
				return -EINVAL;
			value = value * 10 + digit;
			input[n] = value;
		} else if (buf[i] == ',' || buf[i] == ' ') {
			if (in_number) {
				n++;
				in_number = false;
			}
		} else {
			break;
		}
	}
	if (in_number)
		n++;

	if ((input[0] != SET_THP_IC_CUR_VALUE &&
	     input[0] != GET_THP_IC_CUR_VALUE) || n < 4 || n > CMD_DATA_BUF_SIZE)
		return -EINVAL;

	if (input[1] == SET_OPEN_TRANSPORT_MODE) {
		if (input[0] != SET_THP_IC_CUR_VALUE)
			return count;
		length = n - 3;
		if (input[n - 1] != length)
			return -EINVAL;
		mutex_lock(&touch_callback_lock);
		add_common_data_to_buf(0, input[0], input[1], length, input + 2);
		mutex_unlock(&touch_callback_lock);
		return count;
	}

	if (input[1] < 0)
		return count;
	length = input[3];
	if (n != 4 || length == 1 ||
	    (input[1] != SET_TOUCH_IC_INFO && length != 2) ||
	    length < 0 || length > CMD_DATA_BUF_SIZE - 2)
		return -EINVAL;
	mutex_lock(&touch_callback_lock);
	add_common_data_to_buf(0, input[0], input[1], length, input + 2);
	mutex_unlock(&touch_callback_lock);
	return count;
}

static ssize_t touch_thp_ic_cmd_data_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	u16 data_len;
	int i;

	mutex_lock(&thp_ic_read_data_mutex);
	if (thp_ic_cmd_data_common_data.cmd != GET_THP_IC_CUR_VALUE)
		goto out;

	data_len = thp_ic_cmd_data_common_data.data_len;
	if (data_len > sizeof(thp_ic_cmd_data_common_data.data_buf))
		goto out;

	if (thp_ic_cmd_data_common_data.mode < SET_IDLE_THD) {
		memcpy(buf, thp_ic_cmd_data_common_data.data_buf, data_len);
		count = data_len;
	} else if (thp_ic_cmd_data_common_data.mode == SET_TOUCH_IC_INFO) {
		u8 *data = (u8 *)thp_ic_cmd_data_common_data.data_buf;

		count += sysfs_emit_at(buf, count, "protocol_version:    %u\n", get_unaligned_le16(data));
		count += sysfs_emit_at(buf, count, "frame_len:    %u\n", get_unaligned_le16(data + 2));
		count += sysfs_emit_at(buf, count, "ic_fw_v:    %u\n", get_unaligned_le16(data + 4));
		count += sysfs_emit_at(buf, count, "ic_name:    %u\n", get_unaligned_le16(data + 6));
		count += sysfs_emit_at(buf, count, "ic_project_name:    %u\n", get_unaligned_le16(data + 8));
		count += sysfs_emit_at(buf, count, "ic_supplier_name:    %u\n", get_unaligned_le16(data + 10));
		count += sysfs_emit_at(buf, count, "pitch_size_y:    %u\n", data[12]);
		count += sysfs_emit_at(buf, count, "pitch_size_x:    %u\n", data[13]);
		count += sysfs_emit_at(buf, count, "numCol:    %u\n", data[14]);
		count += sysfs_emit_at(buf, count, "numRow:    %u\n", data[15]);
		count += sysfs_emit_at(buf, count, "packaging_factory:    %u\n", get_unaligned_le16(data + 16));
		count += sysfs_emit_at(buf, count, "wafe_factory:    %u\n", get_unaligned_le16(data + 18));
		count += sysfs_emit_at(buf, count, "x_resolution:    %u\n", get_unaligned_le16(data + 20));
		count += sysfs_emit_at(buf, count, "y_resolution:    %u\n", get_unaligned_le16(data + 22));
		count += sysfs_emit_at(buf, count, "lockdown_info:    %llu\n",
				       (unsigned long long)get_unaligned_le64(data + 24));
		count += sysfs_emit_at(buf, count, "frame_data_type:    %u\n", data[32]);
		count += sysfs_emit_at(buf, count, "mutual_len:    %u\n", get_unaligned_le16(data + 33));
		count += sysfs_emit_at(buf, count, "slef1_len:    %u\n", get_unaligned_le16(data + 35));
		count += sysfs_emit_at(buf, count, "slef2_len:    %u\n", get_unaligned_le16(data + 37));
	} else {
		for (i = 0; i < data_len && count < PAGE_SIZE; i++)
			count += sysfs_emit_at(buf, count, "%x",
				((u8 *)thp_ic_cmd_data_common_data.data_buf)[i]);
		if (count < PAGE_SIZE)
			count += sysfs_emit_at(buf, count, "\n");
	}
out:
	mutex_unlock(&thp_ic_read_data_mutex);
	return count;
}
static ssize_t touch_thp_ic_cmd_data_store(struct device *dev,
				  struct device_attribute *attr, const char *buf,
				  size_t count)
{
	if (count > sizeof(thp_ic_cmd_data_common_data)) {
		mi_ts_err("memory out of range:%zu\n", count);
		return count;
	}

	mutex_lock(&thp_ic_write_data_mutex);
	memcpy(&thp_ic_cmd_data_common_data, buf, count);
	mutex_unlock(&thp_ic_write_data_mutex);
	return count;
}
void thp_send_cmd_to_hal(int cmd, int value)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	mutex_lock(&xiaomi_touch_dev.mutex);
	if (!xiaomi_touch_probe_finished || !touch_pdata)
		goto out;
	touch_data = touch_pdata->touch_data[0];
	mi_ts_info("cmd: %d, value: %d", cmd, value);
	if (!touch_data)
		goto out;
	touch_data->thp_cmd_buf[0] = SET_CUR_VALUE;
	touch_data->thp_cmd_buf[1] = 0;
	touch_data->thp_cmd_buf[2] = cmd;
	touch_data->thp_cmd_buf[3] = value;
	touch_data->thp_cmd_size = 4;
	touch_data->touch_event_status = 1;
	if (touch_pdata->param_head == touch_pdata->param_tail && touch_pdata->param_flag == 1) {
		mi_ts_err("param buffer is full!\n");
		mutex_unlock(&xiaomi_touch_dev.mutex);
		return;
	}
	spin_lock(&touch_pdata->param_lock);
	touch_pdata->touch_cmd_data[touch_pdata->param_tail]->thp_cmd_size = touch_data->thp_cmd_size;
	memcpy((unsigned char*)touch_pdata->touch_cmd_data[touch_pdata->param_tail]->param_buf,
		(unsigned char*)touch_data->thp_cmd_buf, sizeof(int) * touch_data->thp_cmd_size);
	touch_pdata->param_tail++;
	if (touch_pdata->param_tail == PARAM_BUF_NUM)
		touch_pdata->param_tail = 0;
	if (touch_pdata->param_head == touch_pdata->param_tail)
		touch_pdata->param_flag = 1;
	spin_unlock(&touch_pdata->param_lock);
	sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd");
#ifdef TOUCH_THP_SUPPORT
	if (wait_event_interruptible_timeout(touch_data->wait_queue,
			!touch_data->touch_event_status, msecs_to_jiffies(100)) <= 0) {
		mi_ts_err("thp read timeout, skip this event, status:%d\n", touch_data->touch_event_status);
	}
#endif
out:
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return;
}
EXPORT_SYMBOL_GPL(thp_send_cmd_to_hal);
static ssize_t thp_downthreshold_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_downthreshold);
}
static ssize_t thp_downthreshold_store(struct device *dev,
				       struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_downthreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_downthd");
	return count;
}
static ssize_t thp_upthreshold_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_upthreshold);
}
static ssize_t thp_upthreshold_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_upthreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_upthd");
	return count;
}
static ssize_t thp_movethreshold_store(struct device *dev,
				       struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_movethreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_movethd");
	return count;
}
static ssize_t thp_movethreshold_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_movethreshold);
}
static ssize_t thp_islandthreshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_islandthreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_islandthd");
	return count;
}
static ssize_t thp_islandthreshold_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_islandthreshold);
}
static ssize_t thp_noisefilter_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_noisefilter = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_noisefilter");
	return count;
}
static ssize_t thp_noisefilter_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_noisefilter);
}
static ssize_t thp_smooth_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_smooth = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_smooth");
	return count;
}
static ssize_t thp_smooth_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_smooth);
}
static ssize_t thp_dump_frame_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_dump_raw = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_dump");
	return count;
}
static ssize_t thp_dump_frame_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_dump_raw);
}
static ssize_t thp_disconnect_result_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("value:%d\n", input);
	touch_data->thp_disconnect_result = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_disconnect_result");
	return count;
}
static ssize_t thp_disconnect_result_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_disconnect_result);
}
static ssize_t thp_disconnect_type_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (count > MAX_BUF_SIZE) {
		mi_ts_err("memory out of range:%d\n", (int)count);
		return count;
	}
	mi_ts_info("buf:%s, size:%d\n", buf, (int)count);
	memcpy(touch_data->thp_disconnect_type, buf, count);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_disconnect_type");
	return count;
}
static ssize_t thp_disconnect_type_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int len;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	len = strlen(touch_data->thp_disconnect_type);
	memcpy(buf, touch_data->thp_disconnect_type, len);
	return len;
}
static ssize_t thp_disconnect_detect_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("value:%d\n", input);
	touch_data->thp_disconnect_detect = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_disconnect_detect");
	return count;
}
static ssize_t thp_disconnect_detect_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_disconnect_detect);
}
static ssize_t update_rawdata_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
#ifdef TOUCH_THP_SUPPORT
	int remaining = 0;
	if (!touch_pdata->raw_data)
		return -ENOMEM;
	if (touch_pdata->raw_head == touch_pdata->raw_tail)
		return snprintf(buf, PAGE_SIZE, "%s\n", "0");
	else {
		if (touch_pdata->raw_head < touch_pdata->raw_tail)
			remaining = touch_pdata->raw_tail - touch_pdata->raw_head;
		else
			remaining = RAW_BUF_NUM - touch_pdata->raw_head + touch_pdata->raw_tail;
		memcpy((unsigned char *)touch_pdata->raw_data, (unsigned char *)touch_pdata->raw_buf[touch_pdata->raw_head],
			touch_pdata->raw_len);
		spin_lock(&touch_pdata->raw_lock);
		touch_pdata->raw_head++;
		if (touch_pdata->raw_head == RAW_BUF_NUM)
			touch_pdata->raw_head = 0;
		spin_unlock(&touch_pdata->raw_lock);
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", remaining);
#else
	int len;
	char buf0[10] = "update";
	// mi_ts_info("%s in\n", __func__);
	len = strlen(buf0);
	memcpy(buf, buf0, len);
	return len;
#endif
}
static ssize_t update_rawdata_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	if (!touch_pdata->raw_data)
		return -ENOMEM;
	if (touch_pdata->raw_head != touch_pdata->raw_tail)
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "update_rawdata");
	// mi_ts_info("%s notify buf\n", __func__);
	return count;
}
static ssize_t enable_clicktouch_store(struct device *dev,
				       struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	mutex_lock(&touch_callback_lock);
	if (touch_data->enable_clicktouch_raw)
		touch_data->enable_clicktouch_raw(input);
	mutex_unlock(&touch_callback_lock);
	return count;
}
static ssize_t enable_clicktouch_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", "1");
}
static int update_clicktouch_raw_locked(void)
{
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "clicktouch_raw");
	return 0;
}

int update_clicktouch_raw(void)
{
	int ret = -ENODEV;

	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		ret = update_clicktouch_raw_locked();
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(update_clicktouch_raw);
/* The in-tree PM callers hold touch_callback_lock across this notification. */
int xiaomi_touch_set_suspend_state(int state)
{
	if (!touch_pdata)
		return -ENODEV;
	touch_pdata->suspend_state = state;
#ifndef TOUCH_THP_SUPPORT
	add_common_data_to_buf(0, SET_CUR_VALUE, Touch_Suspend, 1, &(touch_pdata->suspend_state));
#endif
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "suspend_state");
	return 0;
}
EXPORT_SYMBOL_GPL(xiaomi_touch_set_suspend_state);
static ssize_t xiaomi_touch_suspend_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_pdata->suspend_state);
}
#ifdef	TOUCH_FOD_SUPPORT
int update_fod_press_status(int value)
{
	struct xiaomi_touch *dev = NULL;
	mutex_lock(&xiaomi_touch_dev.fod_press_status_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.fod_press_status_mutex);
		return -ENODEV;
	}
	dev = touch_pdata->device;
	if (value != touch_pdata->fod_press_status_value) {
		mi_ts_info("value:%d\n", value);
		touch_pdata->fod_press_status_value = value;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			     "fod_press_status");
	}
	mutex_unlock(&xiaomi_touch_dev.fod_press_status_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(update_fod_press_status);
#endif
int notify_gesture_single_tap(void)
{
	mutex_lock(&xiaomi_touch_dev.gesture_single_tap_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.gesture_single_tap_mutex);
		return -ENODEV;
	}
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
		     "gesture_single_tap_state");
	mutex_unlock(&xiaomi_touch_dev.gesture_single_tap_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(notify_gesture_single_tap);
static ssize_t gesture_single_tap_value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}
#ifdef	TOUCH_FOD_SUPPORT
static ssize_t fod_press_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->fod_press_status_value);
}
#endif
static void update_active_status_locked(bool status)
{
	if (!touch_pdata || status == touch_pdata->touch_data[0]->active_status) {
		return;
	}
	touch_pdata->touch_data[0]->active_status = status;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			"touch_active_status");
	return;
}

void update_active_status(bool status)
{
	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		update_active_status_locked(status);
	mutex_unlock(&xiaomi_touch_dev.mutex);
}
EXPORT_SYMBOL_GPL(update_active_status);
static ssize_t touch_active_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->active_status);
}
static void update_touch_irq_no_locked(int irq_no)
{
	if (!touch_pdata) {
		return;
	}
	touch_pdata->touch_data[0]->irq_no = irq_no;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			"touch_irq_no");
	return;
}

void update_touch_irq_no(int irq_no)
{
	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		update_touch_irq_no_locked(irq_no);
	mutex_unlock(&xiaomi_touch_dev.mutex);
}
EXPORT_SYMBOL_GPL(update_touch_irq_no);
static ssize_t touch_finger_status_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
			return -EINVAL;
	if (touch_data->finger_status == input)
		return count;
	touch_data->finger_status = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_finger_status");
	return count;
}
static ssize_t touch_finger_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->finger_status);
}
static ssize_t touch_irq_no_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->irq_no);
}
static ssize_t resolution_factor_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int factor = 1;
	if (!touch_pdata)
		return -ENODEV;
	mutex_lock(&touch_callback_lock);
	if (touch_panel_data[0].registered) {
		factor = touch_panel_data[0].hardware_param.super_resolution_factor;
		mi_ts_info("enter resolution_factor_show factor %d", factor);
	} else if (touch_pdata->touch_data[0]->get_touch_super_resolution_factor){
		factor = touch_pdata->touch_data[0]->get_touch_super_resolution_factor();
		mi_ts_info("enter resolution_factor_show factor %d", factor);
	}
	mutex_unlock(&touch_callback_lock);
	return snprintf(buf, PAGE_SIZE, "%d", factor);
}
static ssize_t abnormal_event_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	int struct_abnormal_event_size = sizeof(struct abnormal_event);
	if (!pdata)
		return -ENODEV;
	mutex_lock(&pdata->device->abnormal_event_mutex);
	if (pdata->abnormal_event_buf.bottom == pdata->abnormal_event_buf.top &&
			!pdata->abnormal_event_buf.full_flag) {
		mi_ts_err("buf is empty\n");
		mutex_unlock(&pdata->device->abnormal_event_mutex);
		return -1;
	}
	memcpy(buf, &pdata->abnormal_event_buf.abnormal_event[pdata->abnormal_event_buf.bottom], struct_abnormal_event_size);
	pdata->abnormal_event_buf.bottom++;
	if (pdata->abnormal_event_buf.bottom >= SENSITIVE_EVENT_BUF_SIZE) {
		pdata->abnormal_event_buf.full_flag = false;
		pdata->abnormal_event_buf.bottom = 0;
	}
	if (pdata->abnormal_event_buf.top > pdata->abnormal_event_buf.bottom || pdata->abnormal_event_buf.full_flag)
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "abnormal_event");
	mutex_unlock(&pdata->device->abnormal_event_mutex);
	return struct_abnormal_event_size;
}
static ssize_t abnormal_event_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	struct abnormal_event *temp_event = (struct abnormal_event *)buf;
	int struct_abnormal_event_size = sizeof(struct abnormal_event);
	if (!pdata || count != struct_abnormal_event_size) {
		mi_ts_err("fail! pdata = %p, size = %zu, %d\n", (void *)pdata, count, struct_abnormal_event_size);
		return -ENODEV;
	}
	mutex_lock(&pdata->device->abnormal_event_mutex);
	memcpy(&pdata->abnormal_event_buf.abnormal_event[pdata->abnormal_event_buf.top], temp_event, struct_abnormal_event_size);
	pdata->abnormal_event_buf.top++;
	if (pdata->abnormal_event_buf.top >= SENSITIVE_EVENT_BUF_SIZE) {
		pdata->abnormal_event_buf.full_flag = true;
		pdata->abnormal_event_buf.top = 0;
	}
	mutex_unlock(&pdata->device->abnormal_event_mutex);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "abnormal_event");
	return count;
}
static ssize_t fod_test_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int value = 0;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if ((!pdata) || (!pdata->touch_data[0])) {
		pr_err("%s touch_pdata is null!\n", __func__);
		return -ENODEV;
	}
	pr_info("%s,buf:%s,count:%zu\n", __func__, buf, count);
	if (kstrtoint(buf, FOD_TEST_BASE, &value))
		return -ENODEV;
	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->fod_test_store)
		pdata->touch_data[0]->fod_test_store(value);
	else {
		pr_err("%s has not implement\n", __func__);
	}
	mutex_unlock(&touch_callback_lock);
	pr_info("%s value:%d\n", __func__, value);
	return count;
}

static ssize_t fod_attn_test_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int value = 0;
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (sscanf(buf, "%d", &value) < 0)
		return -EINVAL;

	mutex_lock(&touch_callback_lock);
	if (panel->registered && panel->hardware_operation.fod_attn_test)
		panel->hardware_operation.fod_attn_test(value);
	mutex_unlock(&touch_callback_lock);
	return count;
}

static ssize_t fod_low_attn_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int value = 0;
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (sscanf(buf, "%d", &value) < 0)
		return -EINVAL;

	mutex_lock(&touch_callback_lock);
	if (panel->registered && panel->hardware_operation.fod_low_attn)
		panel->hardware_operation.fod_low_attn(value);
	mutex_unlock(&touch_callback_lock);
	return count;
}
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 start*/
static ssize_t touch_dfs_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}
static ssize_t touch_dfs_test_store(struct device *dev,struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mutex_lock(&touch_callback_lock);
	if (pdata->touch_data[0]->touch_dfs_test)
		pdata->touch_data[0]->touch_dfs_test(input);
	else {
		pr_err("%s has not touch_dfs_test\n", __func__);
	}
	mutex_unlock(&touch_callback_lock);
	pr_info("%s value:%d\n", __func__, input);
	return count;
}

#if IS_ENABLED(CONFIG_MIEV)
void xiaomi_touch_mievent_report_int(unsigned int code, int panel_id, const char *fault_name, const char *vendor_name, long error_code)
{
	struct misight_mievent *event;
#ifdef CONFIG_FACTORY_BUILD
	pr_info("factory build,return\n");
	return;
#endif
	pr_info("code:%d,fault_name:%s,panel_id:%d,vendor_name:%s,error_code:%ld\n", code, fault_name, panel_id, vendor_name, error_code);
	event = cdev_tevent_alloc(code);
	if (!event) {
		pr_err("misight event is error\n");
		return;
	}
	cdev_tevent_add_int(event, "panel_id", panel_id);
	cdev_tevent_add_str(event, "fault_name", fault_name);
	cdev_tevent_add_str(event, "vendor_name", vendor_name);
	cdev_tevent_add_int(event, "error_code", error_code);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_mievent_report_int);

void xiaomi_touch_mievent_report_str(unsigned int code, int panel_id, const char *fault_name, const char *vendor_name)
{
	struct misight_mievent *event;
#ifdef CONFIG_FACTORY_BUILD
	pr_info("factory build,return\n");
	return;
#endif
	pr_info("code:%d,fault_name:%s,panel_id:%d,vendor_name:%s\n", code, fault_name, panel_id, vendor_name);
	event = cdev_tevent_alloc(code);
	if (!event) {
		pr_err("misight event is error\n");
		return;
	}
	cdev_tevent_add_int(event, "panel_id", panel_id);
	cdev_tevent_add_str(event, "fault_name", fault_name);
	cdev_tevent_add_str(event, "vendor_name", vendor_name);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_mievent_report_str);
#endif
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 end*/
static DEVICE_ATTR(abnormal_event, (S_IRUGO | S_IWUSR | S_IWGRP),
			abnormal_event_show, abnormal_event_store);
static DEVICE_ATTR(touch_thp_cmd_data, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_cmd_data_show, thp_cmd_data_store);
static DEVICE_ATTR(touch_thp_ic_cmd, (S_IRUGO | S_IWUSR | S_IWGRP),
			NULL, touch_thp_ic_cmd_store);
static DEVICE_ATTR(touch_thp_ic_cmd_data, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_thp_ic_cmd_data_show, touch_thp_ic_cmd_data_store);
static DEVICE_ATTR(touch_thp_cmd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_cmd_status_show, thp_cmd_status_store);
static DEVICE_ATTR(touch_thp_cmd_ready, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_cmd_ready_status_show, thp_cmd_ready_status_store);
static DEVICE_ATTR(touch_thp_islandthd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_islandthreshold_show, thp_islandthreshold_store);
static DEVICE_ATTR(touch_thp_downthd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_downthreshold_show, thp_downthreshold_store);
static DEVICE_ATTR(touch_thp_upthd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_upthreshold_show, thp_upthreshold_store);
static DEVICE_ATTR(touch_thp_movethd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_movethreshold_show, thp_movethreshold_store);
static DEVICE_ATTR(touch_thp_smooth, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_smooth_show, thp_smooth_store);
static DEVICE_ATTR(touch_thp_dump, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_dump_frame_show, thp_dump_frame_store);
static DEVICE_ATTR(touch_thp_disconnect_detect, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_disconnect_detect_show, thp_disconnect_detect_store);
static DEVICE_ATTR(touch_thp_disconnect_result, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_disconnect_result_show, thp_disconnect_result_store);
static DEVICE_ATTR(touch_thp_disconnect_type, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_disconnect_type_show, thp_disconnect_type_store);
static DEVICE_ATTR(touch_thp_noisefilter, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_noisefilter_show, thp_noisefilter_store);
static DEVICE_ATTR(enable_touch_raw, (S_IRUGO | S_IWUSR | S_IWGRP),
			enable_touchraw_show, enable_touchraw_store);
static DEVICE_ATTR(enable_touch_delta, (S_IRUGO | S_IWUSR | S_IWGRP),
			enable_touchdelta_show, enable_touchdelta_store);
static DEVICE_ATTR(palm_sensor, (S_IRUGO | S_IWUSR | S_IWGRP),
			palm_sensor_show, palm_sensor_store);
static DEVICE_ATTR(palm_sensor_data, (S_IRUGO | S_IWUSR | S_IWGRP),
			palm_sensor_data_show, palm_sensor_data_store);
static DEVICE_ATTR(prox_sensor, (S_IRUGO | S_IWUSR | S_IWGRP),
			prox_sensor_show, prox_sensor_store);
static DEVICE_ATTR(touch_doze_analysis, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_doze_analysis_show, touch_doze_analysis_store);
static DEVICE_ATTR(clicktouch_raw, (S_IRUGO | S_IWUSR | S_IWGRP),
			enable_clicktouch_show, enable_clicktouch_store);
static DEVICE_ATTR(panel_vendor, (S_IRUGO), panel_vendor_show, NULL);
static DEVICE_ATTR(panel_color, (S_IRUGO), panel_color_show, NULL);
static DEVICE_ATTR(panel_display, (S_IRUGO), panel_display_show, NULL);
static DEVICE_ATTR(touch_vendor, (S_IRUGO), touch_vendor_show, NULL);
static DEVICE_ATTR(touch_thp_tx_num, (S_IRUGO), xiaomi_touch_tx_num_show, NULL);
static DEVICE_ATTR(touch_thp_rx_num, (S_IRUGO), xiaomi_touch_rx_num_show, NULL);
static DEVICE_ATTR(touch_thp_freq_num, (S_IRUGO), xiaomi_touch_freq_num_show, NULL);
static DEVICE_ATTR(touch_sensor, (S_IRUGO | S_IWUSR | S_IWGRP), touch_sensor_show, touch_sensor_store);
static DEVICE_ATTR(touch_sensor_ctrl, (S_IRUGO | S_IWUSR | S_IWGRP), touch_sensor_ctrl_show, touch_sensor_ctrl_store);
static DEVICE_ATTR(touch_thp_mem_notify, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_thp_mem_notify_show, touch_thp_mem_notify_store);
static DEVICE_ATTR(touch_thp_x_resolution, (S_IRUGO), xiaomi_touch_x_resolution_show, NULL);
static DEVICE_ATTR(touch_thp_y_resolution, (S_IRUGO), xiaomi_touch_y_resolution_show, NULL);
static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);
static DEVICE_ATTR(update_rawdata, (0664), update_rawdata_show,
			update_rawdata_store);
#ifdef	TOUCH_FOD_SUPPORT
static DEVICE_ATTR(fod_press_status, (S_IRUGO | S_IWUSR | S_IWGRP), fod_press_status_show, NULL);
#endif
static DEVICE_ATTR(gesture_single_tap_state, (S_IRUGO | S_IWUSR | S_IWGRP), gesture_single_tap_value_show, NULL);
static DEVICE_ATTR(touch_active_status, (S_IRUGO | S_IWUSR | S_IWGRP), touch_active_status_show, NULL);
static DEVICE_ATTR(touch_irq_no, (S_IRUGO | S_IWUSR | S_IWGRP), touch_irq_no_show, NULL);
static DEVICE_ATTR(touch_finger_status, (S_IRUGO | S_IWUSR | S_IWGRP), touch_finger_status_show, touch_finger_status_store);
static DEVICE_ATTR(touch_thp_testmode, (S_IRUGO | S_IWUSR | S_IWGRP), touch_testmode_show, touch_testmode_store);
static DEVICE_ATTR(touch_thp_testresult, (S_IRUGO | S_IWUSR | S_IWGRP), touch_testresult_show, touch_testresult_store);
static DEVICE_ATTR(touch_thp_preset_point, (S_IRUGO | S_IWUSR | S_IWGRP), touch_preset_point_show, touch_preset_point_store);
static DEVICE_ATTR(resolution_factor, 0644, resolution_factor_show, NULL);
static DEVICE_ATTR(touch_log_level, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_log_level_show, touch_log_level_store);
static DEVICE_ATTR(touch_ic_buffer, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_ic_buffer_show, touch_ic_buffer_store);
static DEVICE_ATTR(fod_test, (S_IRUGO | S_IWUSR | S_IWGRP), NULL, fod_test_store);
static DEVICE_ATTR(fod_attn_test, (S_IRUGO | S_IWUSR | S_IWGRP), NULL,
			fod_attn_test_store);
static DEVICE_ATTR(fod_low_attn, (S_IRUGO | S_IWUSR | S_IWGRP), NULL,
			fod_low_attn_store);
static DEVICE_ATTR(touch_dfs_test, (0664), touch_dfs_test_show, touch_dfs_test_store);
static struct attribute *touch_attr_group[] = {
	&dev_attr_abnormal_event.attr,
	&dev_attr_enable_touch_raw.attr,
	&dev_attr_enable_touch_delta.attr,
	&dev_attr_touch_thp_cmd.attr,
	&dev_attr_touch_thp_cmd_data.attr,
	&dev_attr_touch_thp_ic_cmd.attr,
	&dev_attr_touch_thp_ic_cmd_data.attr,
	&dev_attr_clicktouch_raw.attr,
	&dev_attr_touch_thp_tx_num.attr,
	&dev_attr_touch_thp_rx_num.attr,
	&dev_attr_touch_thp_freq_num.attr,
	&dev_attr_touch_thp_x_resolution.attr,
	&dev_attr_touch_thp_y_resolution.attr,
	&dev_attr_touch_thp_downthd.attr,
	&dev_attr_touch_thp_upthd.attr,
	&dev_attr_touch_thp_movethd.attr,
	&dev_attr_touch_thp_islandthd.attr,
	&dev_attr_touch_thp_smooth.attr,
	&dev_attr_touch_thp_dump.attr,
	&dev_attr_touch_thp_disconnect_detect.attr,
	&dev_attr_touch_thp_disconnect_result.attr,
	&dev_attr_touch_thp_disconnect_type.attr,
	&dev_attr_touch_thp_noisefilter.attr,
	&dev_attr_palm_sensor.attr,
	&dev_attr_palm_sensor_data.attr,
	&dev_attr_prox_sensor.attr,
	&dev_attr_panel_vendor.attr,
	&dev_attr_panel_color.attr,
	&dev_attr_panel_display.attr,
	&dev_attr_touch_vendor.attr,
	&dev_attr_update_rawdata.attr,
	&dev_attr_suspend_state.attr,
#ifdef	TOUCH_FOD_SUPPORT
	&dev_attr_fod_press_status.attr,
#endif
	&dev_attr_gesture_single_tap_state.attr,
	&dev_attr_touch_active_status.attr,
	&dev_attr_touch_irq_no.attr,
	&dev_attr_touch_finger_status.attr,
	&dev_attr_resolution_factor.attr,
	&dev_attr_touch_log_level.attr,
	&dev_attr_fod_attn_test.attr,
	&dev_attr_fod_low_attn.attr,
	&dev_attr_touch_sensor.attr,
	&dev_attr_touch_sensor_ctrl.attr,
	&dev_attr_touch_thp_mem_notify.attr,
	&dev_attr_touch_thp_testmode.attr,
	&dev_attr_touch_thp_testresult.attr,
	&dev_attr_touch_thp_preset_point.attr,
	&dev_attr_touch_doze_analysis.attr,
	&dev_attr_touch_ic_buffer.attr,
	&dev_attr_touch_thp_cmd_ready.attr,
	&dev_attr_fod_test.attr,
	&dev_attr_touch_dfs_test.attr,
	NULL,
};
static void *event_start(struct seq_file *m, loff_t *p)
{
	int pos = 0;
	struct last_touch_event *event;
	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;
	event = touch_pdata->last_touch_events;
	if (*p >= LAST_TOUCH_EVENTS_MAX)
		return NULL;
	pos = (event->head + *p) & (LAST_TOUCH_EVENTS_MAX - 1);
	return event->touch_event_buf + pos;
}
static void *event_next(struct seq_file *m, void *v, loff_t *p)
{
	int pos = 0;
	struct last_touch_event *event;
	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;
	event = touch_pdata->last_touch_events;
	if (++*p >= LAST_TOUCH_EVENTS_MAX)
		return NULL;
	pos = (event->head + *p) & (LAST_TOUCH_EVENTS_MAX - 1);
	return event->touch_event_buf + pos;
}
static int32_t event_show(struct seq_file *m, void *v)
{
	struct touch_event *event_info;
	struct rtc_time tm;
	event_info = (struct touch_event *)v;
	if (event_info->state == EVENT_INIT)
		return 0;
	rtc_time64_to_tm(event_info->touch_time.tv_sec, &tm);
	seq_printf(m, "%d-%02d-%02d %02d:%02d:%02d.%09lu UTC Finger (%2d) %s\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec, event_info->touch_time.tv_nsec,
		   event_info->slot, event_info->state == EVENT_DOWN ? "P" : "R");
	return 0;
}
static void event_stop(struct seq_file *m, void *v)
{
}
const struct seq_operations last_touch_events_seq_ops = {
	.start = event_start,
	.next = event_next,
	.stop = event_stop,
	.show = event_show,
};
static void last_touch_events_collect_locked(int slot, int state)
{
	struct touch_event *event_info;
	struct last_touch_event *event;
	static int event_state[MAX_TOUCH_ID] = {0};
	if (!touch_pdata || !touch_pdata->last_touch_events || slot >= MAX_TOUCH_ID || event_state[slot] == state)
		return;
	event_state[slot] = state;
	event = touch_pdata->last_touch_events;
	event_info = &event->touch_event_buf[event->head];
	event_info->state = !!state ? EVENT_DOWN : EVENT_UP;
	event_info->slot = slot;
	ktime_get_real_ts64(&event_info->touch_time);
	event->head++;
	event->head &= LAST_TOUCH_EVENTS_MAX - 1;
}

void last_touch_events_collect(int slot, int state)
{
	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		last_touch_events_collect_locked(slot, state);
	mutex_unlock(&xiaomi_touch_dev.mutex);
}
EXPORT_SYMBOL_GPL(last_touch_events_collect);
static ssize_t tp_hal_version_read(struct file *file, char __user *buf,
			size_t count, loff_t *pos)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int cnt = 0;
	int ret = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	if (*pos != 0)
		return 0;
	touch_data = touch_pdata->touch_data[0];
	cnt = strlen(touch_data->tp_hal_version);
	if ((ret = copy_to_user(buf, touch_data->tp_hal_version, cnt))) {
		return -EFAULT;
	}
	*pos += cnt;
	if (ret != 0)
		return 0;
	else
		return cnt;
}
static ssize_t tp_hal_version_write(struct file *file, const char __user *buf,
			size_t count, loff_t *pos)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int retval = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memset(touch_data->tp_hal_version, 0x00, TP_VERSION_SIZE);
	if (count < TP_VERSION_SIZE && copy_from_user(touch_data->tp_hal_version, buf, count)) {
		retval = -EFAULT;
		goto out;
	}
out:
	if (retval >= 0)
		retval = count;
	return retval;
}
static const struct proc_ops tp_hal_version_ops = {
	.proc_read = tp_hal_version_read,
	.proc_write = tp_hal_version_write,
};
static const struct of_device_id xiaomi_touch_of_match[] = {
	{ .compatible = "xiaomi-touch", },
	{ },
};
static int xiaomi_touch_parse_dt(struct device *dev, struct xiaomi_touch_pdata *data)
{
	int ret;
	struct device_node *np;
	np = dev->of_node;
	if (!np)
		return -ENODEV;
	ret = of_property_read_string(np, "touch,name", &data->name);
	if (ret)
		return ret;
	mi_ts_info("touch,name:%s\n", data->name);
	return 0;
}
static struct drm_panel *xiaomi_touch_check_panel_locked(void)
{
	int i;
	int count;
	struct device_node *node;
	struct drm_panel *panel = NULL;
	if (!touch_pdata || !touch_pdata->of_node) {
		mi_ts_err("Invalid params");
		return NULL;
	}
	count = of_count_phandle_with_args(touch_pdata->of_node, "panel", NULL);
	if (count <= 0)
		return NULL;
	for (i = 0; i < count; i++) {
		node = of_parse_phandle(touch_pdata->of_node, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel))
			break;
	}
	return panel;
}

struct drm_panel *xiaomi_touch_check_panel(void)
{
	struct drm_panel * ret = NULL;

	mutex_lock(&xiaomi_touch_dev.mutex);
	if (touch_pdata)
		ret = xiaomi_touch_check_panel_locked();
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_touch_check_panel);
/* Sysfs/proc have their own rundown. Misc calls use callback_lock; hardware
 * event reports use the existing sensor locks and never take callback_lock
 * under the NVT lock. Withdraw pdata under those same locks before freeing it.
 */
static void xiaomi_touch_publish_pdata(struct xiaomi_touch_pdata *pdata)
{
	mutex_lock(&xiaomi_touch_dev.mutex);
	mutex_lock(&touch_callback_lock);
	mutex_lock(&xiaomi_touch_dev.palm_mutex);
	mutex_lock(&xiaomi_touch_dev.prox_mutex);
	mutex_lock(&xiaomi_touch_dev.fod_press_status_mutex);
	mutex_lock(&xiaomi_touch_dev.gesture_single_tap_mutex);
	touch_pdata = pdata;
	xiaomi_touch_device = pdata ? &xiaomi_touch_dev : NULL;
	mutex_unlock(&xiaomi_touch_dev.gesture_single_tap_mutex);
	mutex_unlock(&xiaomi_touch_dev.fod_press_status_mutex);
	mutex_unlock(&xiaomi_touch_dev.prox_mutex);
	mutex_unlock(&xiaomi_touch_dev.palm_mutex);
	mutex_unlock(&touch_callback_lock);
	mutex_unlock(&xiaomi_touch_dev.mutex);
}

static void xiaomi_touch_free_pdata(struct xiaomi_touch_pdata *pdata)
{
	int i;

	for (i = 0; i < XIAOMI_TOUCH_MAX_PANEL; i++)
		kfree(pdata->touch_data[i]);
	for (i = 0; i < PARAM_BUF_NUM; i++)
		kfree(pdata->touch_cmd_data[i]);
	for (i = 0; i < RAW_BUF_NUM; i++)
		kfree(pdata->raw_buf[i]);
	kfree(pdata->last_touch_events);
	kfree(pdata->raw_data);
}

static int xiaomi_touch_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xiaomi_touch_pdata *pdata;
	int ret, i;

	if (!dev->of_node)
		return -ENODEV;
	mutex_lock(&touch_lifecycle_lock);
	if (touch_pdata) {
		ret = -EBUSY;
		goto out;
	}
	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		ret = -ENOMEM;
		goto out;
	}
	pdata->of_node = dev->of_node;
	pdata->device = &xiaomi_touch_dev;
	ret = xiaomi_touch_parse_dt(dev, pdata);
	if (ret)
		goto free_data;
	ret = -ENOMEM;
	pdata->raw_data = kzalloc(RAW_SIZE, GFP_KERNEL);
	if (!pdata->raw_data)
		goto free_data;
	pdata->phy_base = virt_to_phys(pdata->raw_data);
	for (i = 0; i < PARAM_BUF_NUM; i++) {
		pdata->touch_cmd_data[i] = kzalloc(sizeof(*pdata->touch_cmd_data[i]), GFP_KERNEL);
		if (!pdata->touch_cmd_data[i])
			goto free_data;
	}
	for (i = 0; i < RAW_BUF_NUM; i++) {
		pdata->raw_buf[i] = kzalloc(RAW_SIZE, GFP_KERNEL);
		if (!pdata->raw_buf[i])
			goto free_data;
	}
	for (i = 0; i < XIAOMI_TOUCH_MAX_PANEL; i++) {
		struct xiaomi_touch_interface *data;

		data = kzalloc(sizeof(*data), GFP_KERNEL);
		if (!data)
			goto free_data;
		pdata->touch_data[i] = data;
		init_waitqueue_head(&data->wait_queue);
		init_waitqueue_head(&data->wait_queue_ready);
		INIT_LIST_HEAD(&data->private_data_list);
		spin_lock_init(&data->private_data_lock);
		mutex_init(&data->common_data_buf_lock);
	}
	pdata->last_touch_events = kzalloc(sizeof(*pdata->last_touch_events), GFP_KERNEL);
	if (!pdata->last_touch_events)
		goto free_data;
	spin_lock_init(&pdata->raw_lock);
	spin_lock_init(&pdata->param_lock);
	init_waitqueue_head(&pdata->poll_wait_queue_head);
	INIT_LIST_HEAD(&pdata->node);
	list_add_tail(&pdata->node, &pdata->touch_data[0]->private_data_list);

	xiaomi_touch_dev.class = class_create("touch");
	if (IS_ERR(xiaomi_touch_dev.class)) {
		ret = PTR_ERR(xiaomi_touch_dev.class);
		xiaomi_touch_dev.class = NULL;
		goto free_data;
	}
	xiaomi_touch_dev.dev = device_create(xiaomi_touch_dev.class, NULL, 'T', pdata, "touch_dev");
	if (IS_ERR(xiaomi_touch_dev.dev)) {
		ret = PTR_ERR(xiaomi_touch_dev.dev);
		xiaomi_touch_dev.dev = NULL;
		goto destroy_class;
	}
	/* Every allocation, lock and list is initialized before global/sysfs access. */
	xiaomi_touch_publish_pdata(pdata);
	xiaomi_touch_dev.attrs.attrs = touch_attr_group;
	ret = sysfs_create_group(&xiaomi_touch_dev.dev->kobj, &xiaomi_touch_dev.attrs);
	if (ret)
		goto withdraw;
	pdata->last_touch_events_proc =
		proc_create_seq("last_touch_events", 0644, NULL, &last_touch_events_seq_ops);
	if (!pdata->last_touch_events_proc) {
		ret = -ENOMEM;
		goto remove_sysfs;
	}
	pdata->tp_hal_version_proc =
		proc_create("tp_hal_version", 0644, NULL, &tp_hal_version_ops);
	if (!pdata->tp_hal_version_proc) {
		ret = -ENOMEM;
		goto remove_proc;
	}
	if (netlink_init()) {
		ret = -ENOMEM;
		goto remove_proc;
	}
	ret = misc_register(&xiaomi_touch_dev.misc_dev);
	if (ret)
		goto remove_netlink;
	/* Knock is deliberately last, not registered ahead of pdata allocation. */
	ret = knock_node_init();
	if (ret)
		goto remove_misc;
	platform_set_drvdata(pdev, pdata);
	mutex_lock(&xiaomi_touch_dev.mutex);
	mutex_lock(&touch_callback_lock);
	touch_generation++;
	WRITE_ONCE(xiaomi_touch_probe_finished, true);
	mutex_unlock(&touch_callback_lock);
	mutex_unlock(&xiaomi_touch_dev.mutex);
	knock_node_publish();
	mutex_unlock(&touch_lifecycle_lock);
	return 0;

remove_misc:
	misc_deregister(&xiaomi_touch_dev.misc_dev);
remove_netlink:
	netlink_exit();
remove_proc:
	proc_remove(pdata->tp_hal_version_proc);
	proc_remove(pdata->last_touch_events_proc);
remove_sysfs:
	sysfs_remove_group(&xiaomi_touch_dev.dev->kobj, &xiaomi_touch_dev.attrs);
withdraw:
	xiaomi_touch_publish_pdata(NULL);
	dev_set_drvdata(xiaomi_touch_dev.dev, NULL);
	device_destroy(xiaomi_touch_dev.class, 'T');
	xiaomi_touch_dev.dev = NULL;
destroy_class:
	class_destroy(xiaomi_touch_dev.class);
	xiaomi_touch_dev.class = NULL;
free_data:
	xiaomi_touch_free_pdata(pdata);
out:
	mutex_unlock(&touch_lifecycle_lock);
	return ret;
}

static int xiaomi_touch_remove(struct platform_device *pdev)
{
	struct xiaomi_touch_pdata *pdata = platform_get_drvdata(pdev);
	int i;

	mutex_lock(&touch_lifecycle_lock);
	mutex_lock(&xiaomi_touch_dev.mutex);
	mutex_lock(&touch_callback_lock);
	WRITE_ONCE(xiaomi_touch_probe_finished, false);
	mutex_unlock(&touch_callback_lock);
	mutex_unlock(&xiaomi_touch_dev.mutex);
	/* No rundown under callback_lock: sysfs/proc callbacks acquire it. */
	knock_node_release();
	misc_deregister(&xiaomi_touch_dev.misc_dev);
	sysfs_remove_group(&xiaomi_touch_dev.dev->kobj, &xiaomi_touch_dev.attrs);
	proc_remove(pdata->tp_hal_version_proc);
	proc_remove(pdata->last_touch_events_proc);
	for (i = 0; i < XIAOMI_TOUCH_MAX_PANEL; i++)
		xiaomi_touch_stop_panel(i);
	/* Frame/delta backing still belongs to the provider until it stops its
	 * producer and unregisters. Existing VMAs are not made safe by this drain.
	 */
	netlink_exit();
	xiaomi_touch_publish_pdata(NULL);
	dev_set_drvdata(xiaomi_touch_dev.dev, NULL);
	device_destroy(xiaomi_touch_dev.class, 'T');
	xiaomi_touch_dev.dev = NULL;
	class_destroy(xiaomi_touch_dev.class);
	xiaomi_touch_dev.class = NULL;
	xiaomi_touch_free_pdata(pdata);
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&touch_lifecycle_lock);
	return 0;
}
static struct platform_driver xiaomi_touch_device_driver = {
	.probe		= xiaomi_touch_probe,
	.remove		= xiaomi_touch_remove,
	.driver		= {
		.name	= "xiaomi-touch",
		/* Independent rebind cannot re-register an already-bound provider. */
		.suppress_bind_attrs = true,
		.of_match_table = of_match_ptr(xiaomi_touch_of_match),
	}
};
static int __init xiaomi_touch_init(void)
{
	return platform_driver_register(&xiaomi_touch_device_driver);
}
static void __exit xiaomi_touch_exit(void)
{
	platform_driver_unregister(&xiaomi_touch_device_driver);
}

MODULE_LICENSE("GPL");
module_init(xiaomi_touch_init);
module_exit(xiaomi_touch_exit);
