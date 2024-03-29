/* arch/arm/mach-msm/htc_battery_8x60.c
 *
 * Copyright (C) 2011 HTC Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>
#include <mach/board.h>
#include <asm/mach-types.h>
#include <mach/board_htc.h>
#include <mach/htc_battery_core.h>
#include <mach/htc_battery_8x60.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/tps65200.h>
#include <linux/reboot.h>
#include <linux/miscdevice.h>
#include <linux/m_adc.h>
#include <linux/pmic8058-xoadc.h>
#include <linux/pmic8058-batt-alarm.h>
#include <mach/mpp.h>
#include <linux/android_alarm.h>
#include <linux/suspend.h>

#define BATT_SUSPEND_CHECK_TIME			3600
#define BATT_TIMER_CHECK_TIME			360

static void mbat_in_func(struct work_struct *work);
static DECLARE_DELAYED_WORK(mbat_in_struct, mbat_in_func);
static struct kset *htc_batt_kset;

static int htc_batt_phone_call;
module_param_named(phone_call, htc_batt_phone_call,
			int, S_IRUGO | S_IWUSR | S_IWGRP);

struct htc_battery_info {
	int device_id;

	/* lock to protect the battery info */
	struct mutex info_lock;

	spinlock_t batt_lock;
	int is_open;

	struct kobject batt_timer_kobj;
	struct kobject batt_cable_kobj;

	struct wake_lock vbus_wake_lock;
	char debug_log[DEBUG_LOG_LENGTH];

	struct battery_info_reply rep;
	struct mpp_config_data *mpp_config;
	struct battery_adc_reply adc_data;
	int adc_vref[ADC_REPLY_ARRAY_SIZE];

	int guage_driver;
	int charger;
};
static struct htc_battery_info htc_batt_info;

struct htc_battery_timer {
	unsigned long batt_system_jiffies;
	unsigned long batt_suspend_ms;
	unsigned long total_time_ms;
	unsigned int batt_alarm_status;
	unsigned int batt_alarm_enabled;
	unsigned int alarm_timer_flag;
	unsigned int time_out;
	struct work_struct batt_work;
	struct alarm batt_check_wakeup_alarm;
	struct timer_list batt_timer;
	struct workqueue_struct *batt_wq;
	struct wake_lock battery_lock;
};
static struct htc_battery_timer htc_batt_timer;

static void cable_status_notifier_func(int online);
static struct t_cable_status_notifier cable_status_notifier = {
	.name = "htc_battery_8x60",
	.func = cable_status_notifier_func,
};

static int htc_battery_initial;
static int htc_full_level_flag;

static void tps_int_notifier_func(int int_reg, int value);
static struct tps65200_chg_int_notifier tps_int_notifier = {
	.name = "htc_battery_8x60",
	.func = tps_int_notifier_func,
};

static void tps_int_notifier_func(int int_reg, int value)
{
	if (int_reg == CHECK_INT1) {
		htc_batt_info.rep.over_vchg = (unsigned int)value;
		htc_battery_core_update_changed();
	}
}

static int batt_alarm_config(unsigned long lower_threshold,
			unsigned long upper_threshold)
{
	int rc = 0;

	rc = pm8058_batt_alarm_state_set(0, 0);
	if (rc) {
		BATT_ERR("state_set disabled failed, rc=%d", rc);
		goto done;
	}

	rc = pm8058_batt_alarm_threshold_set(lower_threshold, upper_threshold);
	if (rc) {
		BATT_ERR("threshold_set failed, rc=%d!", rc);
		goto done;
	}

done:
	return rc;
}

static int battery_alarm_notifier_func(struct notifier_block *nfb,
					unsigned long value, void *data);
static struct notifier_block battery_alarm_notifier = {
	.notifier_call = battery_alarm_notifier_func,
};

static int battery_alarm_notifier_func(struct notifier_block *nfb,
					unsigned long status, void *data)
{
	htc_batt_timer.batt_alarm_status++;
	BATT_LOG("%s: batt alarm status %u", __func__, htc_batt_timer.batt_alarm_status);
	return 0;
}

static void update_wake_lock(int status)
{
	if (status == CHARGER_USB)
		wake_lock(&htc_batt_info.vbus_wake_lock);
	else
		/* give userspace some time to see the uevent and update
		   LED state or whatnot...*/
		wake_lock_timeout(&htc_batt_info.vbus_wake_lock, HZ * 5);
}

static void cable_status_notifier_func(enum usb_connect_type online)
{
	char message[16];
	char *envp[] = { message, NULL };

	if (online == htc_batt_info.rep.charging_source) {
		BATT_LOG("%s: charger type (%u) same return.",
			__func__, online);
		return;
	}

	mutex_lock(&htc_batt_info.info_lock);

	switch (online) {
	case CONNECT_TYPE_USB:
		BATT_LOG("cable USB");
		htc_batt_info.rep.charging_source = CHARGER_USB;
		break;
	case CONNECT_TYPE_AC:
		BATT_LOG("cable AC");
		htc_batt_info.rep.charging_source = CHARGER_AC;
		break;
	case CONNECT_TYPE_WIRELESS:
		BATT_LOG("cable wireless");
		htc_batt_info.rep.charging_source = CHARGER_WIRELESS;
		break;
	case CONNECT_TYPE_UNKNOWN:
		BATT_ERR("unknown cable");
		htc_batt_info.rep.charging_source = CHARGER_USB;
		break;
	case CONNECT_TYPE_NONE:
	default:
		BATT_LOG("No cable exists");
		htc_batt_info.rep.charging_source = CHARGER_BATTERY;
		break;
	}
	htc_batt_timer.alarm_timer_flag =
			(unsigned int)htc_batt_info.rep.charging_source;
	mutex_unlock(&htc_batt_info.info_lock);

	scnprintf(message, 16, "CHG_SOURCE=%d",
					htc_batt_info.rep.charging_source);

	update_wake_lock(htc_batt_info.rep.charging_source);

	kobject_uevent_env(&htc_batt_info.batt_cable_kobj, KOBJ_CHANGE, envp);
}

static int htc_battery_set_charging(int ctl)
{
	int rc = 0;

	if (htc_batt_info.charger == SWITCH_CHARGER_TPS65200)
		rc = tps_set_charger_ctrl(ctl);

	return rc;
}

static int htc_batt_charger_control(enum charger_control_flag control)
{
	char message[16] = "CHARGERSWITCH=";
	char *envp[] = { message, NULL };

	BATT_LOG("%s: switch charger to mode: %u", __func__, control);

	if (control == STOP_CHARGER)
		strncat(message, "0", 1);
	else if (control == ENABLE_CHARGER)
		strncat(message, "1", 1);
	else
		return -1;

	kobject_uevent_env(&htc_batt_info.batt_cable_kobj, KOBJ_CHANGE, envp);

	return 0;
}

static void htc_batt_set_full_level(int percent)
{
	char message[16];
	char *envp[] = { message, NULL };

	BATT_LOG("%s: set full level as %d", __func__, percent);

	scnprintf(message, sizeof(message), "FULL_LEVEL=%d", percent);

	kobject_uevent_env(&htc_batt_info.batt_cable_kobj, KOBJ_CHANGE, envp);

	return;
}

static ssize_t htc_battery_show_batt_attr(struct device_attribute *attr,
					char *buf)
{
	int len = 0;

	len += scnprintf(buf + len, PAGE_SIZE - len,
			"%s", htc_batt_info.debug_log);
	return len;
}

static int htc_batt_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	BATT_LOG("%s: open misc device driver.", __func__);
	spin_lock(&htc_batt_info.batt_lock);

	if (!htc_batt_info.is_open)
		htc_batt_info.is_open = 1;
	else
		ret = -EBUSY;

	spin_unlock(&htc_batt_info.batt_lock);
	return ret;
}

static int htc_batt_release(struct inode *inode, struct file *filp)
{
	BATT_LOG("%s: release misc device driver.", __func__);
	spin_lock(&htc_batt_info.batt_lock);
	htc_batt_info.is_open = 0;
	spin_unlock(&htc_batt_info.batt_lock);

	return 0;
}

static int htc_batt_get_battery_info(struct battery_info_reply *htc_batt_update)
{
	htc_batt_update->batt_vol = htc_batt_info.rep.batt_vol;
	htc_batt_update->batt_id = htc_batt_info.rep.batt_id;
	htc_batt_update->batt_temp = htc_batt_info.rep.batt_temp;
	htc_batt_update->batt_current = htc_batt_info.rep.batt_current;
	htc_batt_update->batt_discharg_current =
				htc_batt_info.rep.batt_discharg_current;
	htc_batt_update->level = htc_batt_info.rep.level;
	htc_batt_update->charging_source =
				htc_batt_info.rep.charging_source;
	htc_batt_update->charging_enabled =
				htc_batt_info.rep.charging_enabled;
	htc_batt_update->full_bat = htc_batt_info.rep.full_bat;
	htc_batt_update->full_level = htc_batt_info.rep.full_level;
	htc_batt_update->over_vchg = htc_batt_info.rep.over_vchg;
	htc_batt_update->temp_fault = htc_batt_info.rep.temp_fault;
	htc_batt_update->batt_state = htc_batt_info.rep.batt_state;

	return 0;
}

static void batt_set_check_timer(u32 seconds)
{
	mod_timer(&htc_batt_timer.batt_timer,
			jiffies + msecs_to_jiffies(seconds * 1000));
}

static int32_t htc_batt_get_battery_adc(void)
{
	int ret = 0;
	struct battery_adc_reply adc;

	/* Read battery voltage adc data. */
	ret = pm8058_htc_config_mpp_and_adc_read(
			adc.adc_voltage,
			ADC_REPLY_ARRAY_SIZE,
			CHANNEL_ADC_BATT_AMON,
			htc_batt_info.mpp_config->vol[XOADC_MPP],
			htc_batt_info.mpp_config->vol[PM_MPP_AIN_AMUX]);
	if (ret)
		goto get_adc_failed;
	/* Read battery current adc data. */
	ret = pm8058_htc_config_mpp_and_adc_read(
			adc.adc_current,
			ADC_REPLY_ARRAY_SIZE,
			CHANNEL_ADC_BATT_AMON,
			htc_batt_info.mpp_config->curr[XOADC_MPP],
			htc_batt_info.mpp_config->curr[PM_MPP_AIN_AMUX]);
	if (ret)
		goto get_adc_failed;
	/* Read battery temperature adc data. */
	ret = pm8058_htc_config_mpp_and_adc_read(
			adc.adc_temperature,
			ADC_REPLY_ARRAY_SIZE,
			CHANNEL_ADC_BATT_AMON,
			htc_batt_info.mpp_config->temp[XOADC_MPP],
			htc_batt_info.mpp_config->temp[PM_MPP_AIN_AMUX]);
	if (ret)
		goto get_adc_failed;
	/* Read battery id adc data. */
	ret = pm8058_htc_config_mpp_and_adc_read(
			adc.adc_battid,
			ADC_REPLY_ARRAY_SIZE,
			CHANNEL_ADC_BATT_AMON,
			htc_batt_info.mpp_config->battid[XOADC_MPP],
			htc_batt_info.mpp_config->battid[PM_MPP_AIN_AMUX]);
	if (ret)
		goto get_adc_failed;

	memcpy(&htc_batt_info.adc_data, &adc,
		sizeof(struct battery_adc_reply));

get_adc_failed:
	return ret;
}

static void batt_regular_timer_handler(unsigned long data)
{
	wake_lock(&htc_batt_timer.battery_lock);
	queue_work(htc_batt_timer.batt_wq, &htc_batt_timer.batt_work);
}

static void batt_check_alarm_handler(struct alarm *alarm)
{
	BATT_LOG("alarm handler, but do nothing.");
	return;
}

static void batt_work_func(struct work_struct *work)
{
	int rc = 0;
	char total_time[32];
	char battery_alarm[16];
	char *envp[] = { total_time, battery_alarm, NULL };

	rc = htc_batt_get_battery_adc();
	if (rc)
		BATT_ERR("Read ADC failed!");

	scnprintf(total_time, sizeof(total_time), "TOTAL_TIME=%lu",
					htc_batt_timer.total_time_ms);

	scnprintf(battery_alarm, sizeof(battery_alarm), "BATT_ALARM=%u",
					htc_batt_timer.batt_alarm_status);

	kobject_uevent_env(&htc_batt_info.batt_timer_kobj, KOBJ_CHANGE, envp);
	htc_batt_timer.total_time_ms = 0;
	htc_batt_timer.batt_system_jiffies = jiffies;
	htc_batt_timer.batt_alarm_status = 0;
	batt_set_check_timer(htc_batt_timer.time_out);
	wake_unlock(&htc_batt_timer.battery_lock);
	return;
}

static long htc_batt_ioctl(struct file *filp,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	wake_lock(&htc_batt_timer.battery_lock);

	switch (cmd) {
	case HTC_BATT_IOCTL_READ_SOURCE: {
		if (copy_to_user((void __user *)arg,
			&htc_batt_info.rep.charging_source, sizeof(u32)))
			ret = -EFAULT;
		break;
	}
	case HTC_BATT_IOCTL_SET_BATT_ALARM: {
		u32 time_out = 0;
		if (copy_from_user(&time_out, (void *)arg, sizeof(u32))) {
			ret = -EFAULT;
			break;
		}

		htc_batt_timer.time_out = time_out;
		if (!htc_battery_initial) {
			htc_battery_initial = 1;
			batt_set_check_timer(htc_batt_timer.time_out);
		}
		break;
	}
	case HTC_BATT_IOCTL_GET_ADC_VREF: {
		if (copy_to_user((void __user *)arg, &htc_batt_info.adc_vref,
				sizeof(htc_batt_info.adc_vref))) {
			BATT_ERR("copy_to_user failed!");
			ret = -EFAULT;
		}
		break;
	}
	case HTC_BATT_IOCTL_GET_ADC_ALL: {
		if (copy_to_user((void __user *)arg, &htc_batt_info.adc_data,
					sizeof(struct battery_adc_reply))) {
			BATT_ERR("copy_to_user failed!");
			ret = -EFAULT;
		}
		break;
	}
	case HTC_BATT_IOCTL_CHARGER_CONTROL: {
		u32 charger_mode = 0;
		if (copy_from_user(&charger_mode, (void *)arg, sizeof(u32))) {
			ret = -EFAULT;
			break;
		}
		BATT_LOG("do charger control = %u", charger_mode);
		htc_battery_set_charging(charger_mode);
		break;
	}
	case HTC_BATT_IOCTL_UPDATE_BATT_INFO: {
		mutex_lock(&htc_batt_info.info_lock);
		if (copy_from_user(&htc_batt_info.rep, (void *)arg,
					sizeof(struct battery_info_reply))) {
			BATT_ERR("copy_from_user failed!");
			ret = -EFAULT;
			mutex_unlock(&htc_batt_info.info_lock);
			break;
		}
		mutex_unlock(&htc_batt_info.info_lock);

		BATT_LOG("ioctl: battery level update: %u",
			htc_batt_info.rep.level);

		htc_battery_core_update_changed();
		break;
	}
	case HTC_BATT_IOCTL_BATT_DEBUG_LOG:
		if (copy_from_user(htc_batt_info.debug_log, (void *)arg,
					DEBUG_LOG_LENGTH)) {
			BATT_ERR("copy debug log from user failed!");
			ret = -EFAULT;
		}
		break;
	case HTC_BATT_IOCTL_SET_VOLTAGE_ALARM: {
		struct battery_vol_alarm alarm_data;

		if (copy_from_user(&alarm_data, (void *)arg,
					sizeof(struct battery_vol_alarm))) {
			BATT_ERR("user set batt alarm failed!");
			ret = -EFAULT;
			break;
		}

		htc_batt_timer.batt_alarm_status = 0;
		htc_batt_timer.batt_alarm_enabled = alarm_data.enable;
		ret = batt_alarm_config(alarm_data.lower_threshold,
				alarm_data.upper_threshold);
		if (ret)
			BATT_ERR("batt alarm config failed!");

		BATT_LOG("Set lower threshold: %d, upper threshold: %d, "
			"Enabled:%u.", alarm_data.lower_threshold,
			alarm_data.upper_threshold, alarm_data.enable);
		break;
	}
	case HTC_BATT_IOCTL_SET_ALARM_TIMER_FLAG: {
		/* alarm flag could be reset by cable. */
		unsigned int flag;
		if (copy_from_user(&flag, (void *)arg, sizeof(unsigned int))) {
			BATT_ERR("Set timer type into alarm failed!");
			ret = -EFAULT;
			break;
		}
		htc_batt_timer.alarm_timer_flag = flag;
		BATT_LOG("Set alarm timer flag:%u", flag);
		break;
	}
	default:
		BATT_ERR("%s: no matched ioctl cmd", __func__);
		break;
	}

	wake_unlock(&htc_batt_timer.battery_lock);

	return ret;
}

/*  MBAT_IN interrupt handler	*/
static void mbat_in_func(struct work_struct *work)
{
	BATT_LOG("shut down device due to MBAT_IN interrupt");
	htc_battery_set_charging(0);
	machine_power_off();
}

static irqreturn_t mbat_int_handler(int irq, void *data)
{
	struct htc_battery_platform_data *pdata = data;

	disable_irq_nosync(pdata->gpio_mbat_in);

	schedule_delayed_work(&mbat_in_struct, msecs_to_jiffies(50));

	return IRQ_HANDLED;
}
/*  MBAT_IN interrupt handler end   */

const struct file_operations htc_batt_fops = {
	.owner = THIS_MODULE,
	.open = htc_batt_open,
	.release = htc_batt_release,
	.unlocked_ioctl = htc_batt_ioctl,
};

static struct miscdevice htc_batt_device_node = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "htc_batt",
	.fops = &htc_batt_fops,
};

static void htc_batt_kobject_release(struct kobject *kobj)
{
	printk(KERN_ERR "htc_batt_kobject_release.\n");
	return;
}

static struct kobj_type htc_batt_ktype = {
	.release = htc_batt_kobject_release,
};

static int htc_battery_prepare(struct device *dev)
{
	int time_diff, rc = 0;
	ktime_t interval;
	ktime_t slack = ktime_set(0, 0);
	ktime_t next_alarm;

	htc_batt_timer.total_time_ms += (jiffies -
			htc_batt_timer.batt_system_jiffies) * MSEC_PER_SEC / HZ;
	htc_batt_timer.batt_system_jiffies = jiffies;
	htc_batt_timer.batt_suspend_ms = xtime.tv_sec * MSEC_PER_SEC +
					xtime.tv_nsec / NSEC_PER_MSEC;

	if (htc_batt_phone_call || htc_batt_timer.alarm_timer_flag) {
		time_diff = htc_batt_timer.time_out * MSEC_PER_SEC -
						htc_batt_timer.total_time_ms;
		if (time_diff > 0)
			interval = ktime_set(time_diff / MSEC_PER_SEC,
				(time_diff % MSEC_PER_SEC) * NSEC_PER_MSEC);
		else {
			del_timer_sync(&htc_batt_timer.batt_timer);
			cancel_work_sync(&htc_batt_timer.batt_work);
			wake_lock(&htc_batt_timer.battery_lock);
			queue_work(htc_batt_timer.batt_wq, &htc_batt_timer.batt_work);
			return -EBUSY;
		}
	} else {
		interval = ktime_set(BATT_SUSPEND_CHECK_TIME, 0);
		if (htc_batt_timer.batt_alarm_enabled) {
			rc = pm8058_batt_alarm_state_set(1, 1);
			if (rc) {
				BATT_ERR("state_set enabled failed, rc=%d!", rc);
				return -EBUSY;
			}
		}
	}

	BATT_LOG("%s: passing time:%lu, status:%u%u, "
		"alarm will be triggered after %d.%d seconds",
		__func__, htc_batt_timer.total_time_ms,
		htc_batt_phone_call,
		htc_batt_timer.alarm_timer_flag,
		interval.tv.sec, interval.tv.nsec);
	next_alarm = ktime_add(alarm_get_elapsed_realtime(), interval);
	alarm_start_range(&htc_batt_timer.batt_check_wakeup_alarm,
				next_alarm, ktime_add(next_alarm, slack));

	return 0;
}

static void htc_battery_complete(struct device *dev)
{
	unsigned long resume_ms;
	unsigned long check_time;

	resume_ms = xtime.tv_sec * MSEC_PER_SEC + xtime.tv_nsec / NSEC_PER_MSEC;
	htc_batt_timer.total_time_ms += resume_ms -
					htc_batt_timer.batt_suspend_ms;
	htc_batt_timer.batt_system_jiffies = jiffies;

	BATT_LOG("%s: total suspend time:%lu, the total passing time:%lu",
			__func__, (resume_ms - htc_batt_timer.batt_suspend_ms),
			htc_batt_timer.total_time_ms);
	if (htc_batt_phone_call || htc_batt_timer.alarm_timer_flag)
		/* 500 msecs check buffer time */
		check_time = (htc_batt_timer.time_out * MSEC_PER_SEC)
				- (MSEC_PER_SEC / 2);
	else
		check_time = BATT_TIMER_CHECK_TIME * MSEC_PER_SEC;

	/*  - When kernel resumes, battery driver should check total time to decide if do battery algorithm or just ignore.
	    - If kernel resumes due to battery voltage alarm, do battery algorithm forcibly. */
	if ((htc_batt_timer.total_time_ms >= check_time) ||
	    (htc_batt_timer.batt_alarm_status > 2)) {
		del_timer_sync(&htc_batt_timer.batt_timer);
		cancel_work_sync(&htc_batt_timer.batt_work);
		wake_lock(&htc_batt_timer.battery_lock);
		queue_work(htc_batt_timer.batt_wq, &htc_batt_timer.batt_work);
	}

	pm8058_batt_alarm_state_set(0, 0);

	return;
}

static struct dev_pm_ops htc_battery_8x60_pm_ops = {
	.prepare = htc_battery_prepare,
	.complete = htc_battery_complete,
};

static int htc_battery_probe(struct platform_device *pdev)
{
	int i, rc = 0;
	struct htc_battery_platform_data *pdata = pdev->dev.platform_data;
	struct htc_battery_core *htc_battery_core_ptr;

	htc_battery_core_ptr = kmalloc(sizeof(struct htc_battery_core),
					GFP_KERNEL);
	if (!htc_battery_core_ptr) {
		BATT_ERR("%s: kmalloc failed for htc_battery_core_ptr.",
				__func__);
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&mbat_in_struct, mbat_in_func);
	if (pdata->gpio_mbat_in_trigger_level == MBAT_IN_HIGH_TRIGGER)
		rc = request_irq(pdata->gpio_mbat_in,
				mbat_int_handler, IRQF_TRIGGER_HIGH,
				"mbat_in", pdata);
	else if (pdata->gpio_mbat_in_trigger_level == MBAT_IN_LOW_TRIGGER)
		rc = request_irq(pdata->gpio_mbat_in,
				mbat_int_handler, IRQF_TRIGGER_LOW,
				"mbat_in", pdata);
	if (rc)
		BATT_ERR("request mbat_in irq failed!");
	else
		set_irq_wake(pdata->gpio_mbat_in, 1);

	htc_battery_core_ptr->func_show_batt_attr = htc_battery_show_batt_attr;
	htc_battery_core_ptr->func_get_battery_info = htc_batt_get_battery_info;
	htc_battery_core_ptr->func_charger_control = htc_batt_charger_control;
	htc_battery_core_ptr->func_set_full_level = htc_batt_set_full_level;
	htc_battery_core_register(&pdev->dev, htc_battery_core_ptr);

	htc_batt_info.device_id = pdev->id;
	htc_batt_info.guage_driver = pdata->guage_driver;
	htc_batt_info.charger = pdata->charger;
	htc_batt_info.rep.full_level = 100;

	htc_batt_info.is_open = 0;

	for (i = 0; i < ADC_REPLY_ARRAY_SIZE; i++)
		htc_batt_info.adc_vref[i] = 66;

	htc_batt_info.mpp_config = &pdata->mpp_data;

	INIT_WORK(&htc_batt_timer.batt_work, batt_work_func);
	init_timer(&htc_batt_timer.batt_timer);
	htc_batt_timer.batt_timer.function = batt_regular_timer_handler;
	alarm_init(&htc_batt_timer.batt_check_wakeup_alarm,
			ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
			batt_check_alarm_handler);
	htc_batt_timer.batt_wq = create_singlethread_workqueue("batt_timer");

	rc = misc_register(&htc_batt_device_node);
	if (rc) {
		BATT_ERR("Unable to register misc device %d",
			MISC_DYNAMIC_MINOR);
		goto fail;
	}

	htc_batt_kset = kset_create_and_add("event_to_daemon", NULL,
			kobject_get(&htc_batt_device_node.this_device->kobj));
	if (!htc_batt_kset) {
		rc = -ENOMEM;
		goto fail;
	}

	htc_batt_info.batt_timer_kobj.kset = htc_batt_kset;
	rc = kobject_init_and_add(&htc_batt_info.batt_timer_kobj,
				&htc_batt_ktype, NULL, "htc_batt_timer");
	if (rc) {
		BATT_ERR("init kobject htc_batt_timer failed.");
		kobject_put(&htc_batt_info.batt_timer_kobj);
		goto fail;
	}

	htc_batt_info.batt_cable_kobj.kset = htc_batt_kset;
	rc = kobject_init_and_add(&htc_batt_info.batt_cable_kobj,
				&htc_batt_ktype, NULL, "htc_cable_detect");
	if (rc) {
		BATT_ERR("init kobject htc_cable_timer failed.");
		kobject_put(&htc_batt_info.batt_timer_kobj);
		goto fail;
	}

	if (pdata->charger == SWITCH_CHARGER_TPS65200)
		tps_register_notifier(&tps_int_notifier);

	pm8058_batt_alarm_register_notifier(&battery_alarm_notifier);

	rc = pm8058_htc_config_mpp_and_adc_read(
				htc_batt_info.adc_vref,
				ADC_REPLY_ARRAY_SIZE,
				CHANNEL_ADC_VCHG,
				0, 0);
	if (rc) {
		BATT_ERR("Get Vref ADC value failed!");
		goto fail;
	}

	BATT_LOG("vref ADC: %d %d %d %d %d", htc_batt_info.adc_vref[0],
					htc_batt_info.adc_vref[1],
					htc_batt_info.adc_vref[2],
					htc_batt_info.adc_vref[3],
					htc_batt_info.adc_vref[4]);

	rc = htc_batt_get_battery_adc();
	if (rc) {
		BATT_ERR("Get first battery ADC value failed!");
		goto fail;
	}

	BATT_LOG("htc_battery_probe(): finish");

fail:
	kfree(htc_battery_core_ptr);
	return rc;
}

static struct platform_driver htc_battery_driver = {
	.probe	= htc_battery_probe,
	.driver	= {
		.name	= "htc_battery",
		.owner	= THIS_MODULE,
		.pm = &htc_battery_8x60_pm_ops,
	},
};

static int __init htc_battery_init(void)
{
	htc_batt_phone_call = 0;
	htc_battery_initial = 0;
	htc_full_level_flag = 0;
	spin_lock_init(&htc_batt_info.batt_lock);
	wake_lock_init(&htc_batt_info.vbus_wake_lock, WAKE_LOCK_SUSPEND,
			"vbus_present");
	wake_lock_init(&htc_batt_timer.battery_lock, WAKE_LOCK_SUSPEND,
			"htc_battery_8x60");
	mutex_init(&htc_batt_info.info_lock);
	cable_detect_register_notifier(&cable_status_notifier);
	platform_driver_register(&htc_battery_driver);

	/* init battery parameters. */
	htc_batt_info.rep.batt_vol = 3300;
	htc_batt_info.rep.batt_id = 1;
	htc_batt_info.rep.batt_temp = 300;
	htc_batt_info.rep.level = 10;
	htc_batt_info.rep.full_bat = 1580000;
	htc_batt_info.rep.full_level = 100;
	htc_batt_info.rep.batt_state = 0;
	htc_batt_info.rep.temp_fault = -1;
	htc_batt_timer.total_time_ms = 0;
	htc_batt_timer.batt_system_jiffies = jiffies;
	htc_batt_timer.batt_alarm_status = 0;
	htc_batt_timer.alarm_timer_flag = 0;

	return 0;
}

module_init(htc_battery_init);
MODULE_DESCRIPTION("HTC Battery Driver");
MODULE_LICENSE("GPL");

