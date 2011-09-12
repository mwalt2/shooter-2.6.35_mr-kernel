/*
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
#include <linux/power_supply.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>
#include <linux/rtc.h>
#include <linux/workqueue.h>
#include <mach/htc_battery_core.h>
#include <linux/android_alarm.h>

static ssize_t htc_battery_show_property(struct device *dev,
					struct device_attribute *attr,
					char *buf);

static int htc_power_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val);

static int htc_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val);

static ssize_t htc_battery_charger_ctrl_timer(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

#define HTC_BATTERY_ATTR(_name)                                             \
{                                                                           \
	.attr = { .name = #_name, .mode = S_IRUGO, .owner = THIS_MODULE },  \
	.show = htc_battery_show_property,                                  \
	.store = NULL,                                                      \
}

struct htc_battery_core_info {
	int present;
	int htc_charge_full;
	unsigned long update_time;
	struct mutex info_lock;
	struct battery_info_reply rep;
	struct htc_battery_core func;
};

static struct htc_battery_core_info battery_core_info;
static int battery_register = 1;

static struct alarm batt_charger_ctrl_alarm;
static struct work_struct batt_charger_ctrl_work;
struct workqueue_struct *batt_charger_ctrl_wq;
static unsigned int charger_ctrl_stat;

static enum power_supply_property htc_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
};

static enum power_supply_property htc_power_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static char *supply_list[] = {
	"battery",
};

static struct power_supply htc_power_supplies[] = {
	{
		.name = "battery",
		.type = POWER_SUPPLY_TYPE_BATTERY,
		.properties = htc_battery_properties,
		.num_properties = ARRAY_SIZE(htc_battery_properties),
		.get_property = htc_battery_get_property,
	},
	{
		.name = "usb",
		.type = POWER_SUPPLY_TYPE_USB,
		.supplied_to = supply_list,
		.num_supplicants = ARRAY_SIZE(supply_list),
		.properties = htc_power_properties,
		.num_properties = ARRAY_SIZE(htc_power_properties),
		.get_property = htc_power_get_property,
	},
	{
		.name = "ac",
		.type = POWER_SUPPLY_TYPE_MAINS,
		.supplied_to = supply_list,
		.num_supplicants = ARRAY_SIZE(supply_list),
		.properties = htc_power_properties,
		.num_properties = ARRAY_SIZE(htc_power_properties),
		.get_property = htc_power_get_property,
	},
	{
		.name = "wireless",
		.type = POWER_SUPPLY_TYPE_WIRELESS,
		.supplied_to = supply_list,
		.num_supplicants = ARRAY_SIZE(supply_list),
		.properties = htc_power_properties,
		.num_properties = ARRAY_SIZE(htc_power_properties),
		.get_property = htc_power_get_property,
	},
};

static int htc_battery_get_charging_status(void)
{
	u32 level;
	enum charger_type_t charger;
	int ret;

	mutex_lock(&battery_core_info.info_lock);
	charger = battery_core_info.rep.charging_source;
	level = battery_core_info.rep.level;
	mutex_unlock(&battery_core_info.info_lock);

	if (battery_core_info.rep.batt_id == 255)
		charger = CHARGER_UNKNOWN;

	switch (charger) {
	case CHARGER_BATTERY:
		battery_core_info.htc_charge_full = 0;
		ret = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case CHARGER_USB:
	case CHARGER_AC:
	case CHARGER_9V_AC:
	case CHARGER_WIRELESS:
		if (battery_core_info.htc_charge_full &&
			(battery_core_info.rep.full_level == 100))
			level = 100;

		if (level == 100)
			battery_core_info.htc_charge_full = 1;
		else
			battery_core_info.htc_charge_full = 0;

		if (battery_core_info.htc_charge_full)
			ret = POWER_SUPPLY_STATUS_FULL;
		else {
			if (battery_core_info.rep.charging_enabled != 0)
				ret = POWER_SUPPLY_STATUS_CHARGING;
			else
				ret = POWER_SUPPLY_STATUS_DISCHARGING;
		}
		break;
	default:
		ret = POWER_SUPPLY_STATUS_UNKNOWN;
	}

	return ret;
}

static ssize_t htc_battery_show_batt_attr(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return battery_core_info.func.func_show_batt_attr(attr, buf);
}

static ssize_t htc_battery_set_delta(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long delta = 0;

	delta = simple_strtoul(buf, NULL, 10);

	if (delta > 100)
		return -EINVAL;

	return count;
}

static ssize_t htc_battery_debug_flag(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long debug_flag;
	debug_flag = simple_strtoul(buf, NULL, 10);

	if (debug_flag > 100 || debug_flag == 0)
		return -EINVAL;

	return 0;
}

static ssize_t htc_battery_set_full_level(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int rc = 0;
	unsigned long percent = 100;

	rc = strict_strtoul(buf, 10, &percent);
	if (rc)
		return rc;

	if (percent > 100 || percent == 0)
		return -EINVAL;

	if (!battery_core_info.func.func_set_full_level) {
		BATT_ERR("No set full level function!");
		return -ENOENT;
	}

	battery_core_info.func.func_set_full_level(percent);

	return count;
}

int htc_battery_charger_disable()
{
	int rc = 0;

	if (!battery_core_info.func.func_charger_control) {
		BATT_ERR("No charger control function!");
		return -ENOENT;
	}
	rc = battery_core_info.func.func_charger_control(STOP_CHARGER);
	if (rc < 0)
		BATT_ERR("charger control failed!");

	return rc;
}

static ssize_t htc_battery_charger_stat(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	int i = 0;

	i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n", charger_ctrl_stat);

	return i;
}

static ssize_t htc_battery_charger_switch(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long enable = 0;
	int rc = 0;

	rc = strict_strtoul(buf, 10, &enable);
	if (rc)
		return rc;

	if (enable >= END_CHARGER)
		return -EINVAL;

	if (!battery_core_info.func.func_charger_control) {
		BATT_ERR("No charger control function!");
		return -ENOENT;
	}

	if (enable == STOP_CHARGER) {
		rc = battery_core_info.func.func_charger_control(STOP_CHARGER);
		if (rc < 0) {
			BATT_ERR("charger control failed!");
			return rc;
		}
		charger_ctrl_stat = STOP_CHARGER;
	} else if (enable == ENABLE_CHARGER) {
		rc = battery_core_info.func.func_charger_control(
							    ENABLE_CHARGER);
		if (rc < 0) {
			BATT_ERR("charger control failed!");
			return rc;
		}
		charger_ctrl_stat = ENABLE_CHARGER;
	}

	alarm_cancel(&batt_charger_ctrl_alarm);

	return count;
}

static struct device_attribute htc_battery_attrs[] = {
	HTC_BATTERY_ATTR(batt_id),
	HTC_BATTERY_ATTR(batt_vol),
	HTC_BATTERY_ATTR(batt_temp),
	HTC_BATTERY_ATTR(batt_current),
	HTC_BATTERY_ATTR(charging_source),
	HTC_BATTERY_ATTR(charging_enabled),
	HTC_BATTERY_ATTR(full_bat),
	HTC_BATTERY_ATTR(over_vchg),

	__ATTR(batt_attr_text, S_IRUGO, htc_battery_show_batt_attr, NULL),
};

static struct device_attribute htc_set_delta_attrs[] = {
	__ATTR(delta, S_IWUSR | S_IWGRP, NULL, htc_battery_set_delta),
	__ATTR(full_level, S_IWUSR | S_IWGRP, NULL,
		htc_battery_set_full_level),
	__ATTR(batt_debug_flag, S_IWUSR | S_IWGRP, NULL,
		htc_battery_debug_flag),
	__ATTR(charger_control, S_IWUSR | S_IWGRP, htc_battery_charger_stat,
		htc_battery_charger_switch),
	__ATTR(charger_timer, S_IWUSR | S_IWGRP, NULL,
		htc_battery_charger_ctrl_timer),
};

static int htc_battery_create_attrs(struct device *dev)
{
	int i = 0, j = 0, rc = 0;

	for (i = 0; i < ARRAY_SIZE(htc_battery_attrs); i++) {
		rc = device_create_file(dev, &htc_battery_attrs[i]);
		if (rc)
			goto htc_attrs_failed;
	}

	for (j = 0; j < ARRAY_SIZE(htc_set_delta_attrs); j++) {
		rc = device_create_file(dev, &htc_set_delta_attrs[j]);
		if (rc)
			goto htc_delta_attrs_failed;
	}

	goto succeed;

htc_attrs_failed:
	while (i--)
		device_remove_file(dev, &htc_battery_attrs[i]);
htc_delta_attrs_failed:
	while (j--)
		device_remove_file(dev, &htc_set_delta_attrs[i]);
succeed:
	return rc;
}

static int htc_battery_update_battery_info(uint32_t cache_msec)
{
	if (battery_core_info.update_time &&
		time_before(jiffies, battery_core_info.update_time +
				msecs_to_jiffies(cache_msec))) {
		return -1;
	}

	if (battery_core_info.func.func_get_battery_info) {
		battery_core_info.func.func_get_battery_info(&battery_core_info.rep);
		if (time_after(jiffies, battery_core_info.update_time +
				msecs_to_jiffies(300))) {
			BATT_LOG("ID=%d, level=%d, vol=%d, temp=%d, curr=%d, "
			"dis_curr=%d, chg_src=%d, chg_en=%d, over_vchg=%d",
				battery_core_info.rep.batt_id,
				battery_core_info.rep.level,
				battery_core_info.rep.batt_vol,
				battery_core_info.rep.batt_temp,
				battery_core_info.rep.batt_current,
				battery_core_info.rep.batt_discharg_current,
				battery_core_info.rep.charging_source,
				battery_core_info.rep.charging_enabled,
				battery_core_info.rep.over_vchg);
		}
		battery_core_info.update_time = jiffies;
	} else {
		BATT_ERR("no show property handler function.");
		return -EINVAL;
	}

	return 0;
}

static int htc_battery_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int ret = 0;
	mutex_lock(&battery_core_info.info_lock);
	ret = htc_battery_update_battery_info(300);
	mutex_unlock(&battery_core_info.info_lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = htc_battery_get_charging_status();
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		/* Fix me: temperature criteria should depend on projects,
			   but not hard code. */
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		if (battery_core_info.func.func_is_temperature_fault) {
			if (battery_core_info.func.func_is_temperature_fault(
				battery_core_info.rep.batt_temp))
				val->intval =  POWER_SUPPLY_HEALTH_OVERHEAT;
		}
		else if (battery_core_info.rep.batt_temp >= 480 ||
			battery_core_info.rep.batt_temp <= 0)
			val->intval =  POWER_SUPPLY_HEALTH_OVERHEAT;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = battery_core_info.present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		mutex_lock(&battery_core_info.info_lock);
		val->intval = battery_core_info.rep.level;
		mutex_unlock(&battery_core_info.info_lock);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int htc_power_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	enum charger_type_t charger;

	mutex_lock(&battery_core_info.info_lock);

	charger = battery_core_info.rep.charging_source;

#if 0
	if (battery_core_info.rep.batt_id == 255)
		charger = CHARGER_BATTERY;
#endif

	mutex_unlock(&battery_core_info.info_lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		if (psy->type == POWER_SUPPLY_TYPE_MAINS) {
			if (charger == CHARGER_AC ||
			    charger == CHARGER_9V_AC)
				val->intval = 1;
			else
				val->intval = 0;
		} else if (psy->type == POWER_SUPPLY_TYPE_USB)
			val->intval = (charger ==  CHARGER_USB ? 1 : 0);
		else if (psy->type == POWER_SUPPLY_TYPE_WIRELESS)
			val->intval = (charger ==  CHARGER_WIRELESS ? 1 : 0);
		else
			val->intval = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static ssize_t htc_battery_show_property(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int i = 0;
	int ret = 0;
	const ptrdiff_t off = attr - htc_battery_attrs;

	mutex_lock(&battery_core_info.info_lock);
	ret = htc_battery_update_battery_info(300);

	switch (off) {
	case BATT_ID:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.batt_id);
		break;
	case BATT_VOL:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.batt_vol);
		break;
	case BATT_TEMP:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.batt_temp);
		break;
	case BATT_CURRENT:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.batt_current);
		break;
	case CHARGING_SOURCE:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.charging_source);
		break;
	case CHARGING_ENABLED:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.charging_enabled);
		break;
	case FULL_BAT:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.full_bat);
		break;
	case OVER_VCHG:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				battery_core_info.rep.over_vchg);
		break;
	default:
		i = -EINVAL;
	}
	mutex_unlock(&battery_core_info.info_lock);

	if (i < 0)
		BATT_ERR("%s: battery: attribute is not supported: %d",
			__func__, off);

	return i;
}

static ssize_t htc_battery_charger_ctrl_timer(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int rc;
	unsigned long time_out = 0;
	ktime_t interval;
	ktime_t next_alarm;

	rc = strict_strtoul(buf, 10, &time_out);
	if (rc)
		return rc;

	if (time_out > 65536)
		return -EINVAL;

	if (time_out > 0) {
		rc = battery_core_info.func.func_charger_control(STOP_CHARGER);
		if (rc < 0) {
			BATT_ERR("charger control failed!");
			return rc;
		}
		interval = ktime_set(time_out, 0);
		next_alarm = ktime_add(alarm_get_elapsed_realtime(), interval);
		alarm_start_range(&batt_charger_ctrl_alarm,
					next_alarm, next_alarm);
		charger_ctrl_stat = STOP_CHARGER;
	} else if (time_out == 0) {
		rc = battery_core_info.func.func_charger_control(
							ENABLE_CHARGER);
		if (rc < 0) {
			BATT_ERR("charger control failed!");
			return rc;
		}
		alarm_cancel(&batt_charger_ctrl_alarm);
		charger_ctrl_stat = ENABLE_CHARGER;
	}

	return count;
}

static void batt_charger_ctrl_func(struct work_struct *work)
{
	int rc;

	rc = battery_core_info.func.func_charger_control(ENABLE_CHARGER);
	if (rc) {
		BATT_ERR("charger control failed!");
		return;
	}

	charger_ctrl_stat = (unsigned int)ENABLE_CHARGER;
}

static void batt_charger_ctrl_alarm_handler(struct alarm *alarm)
{
	BATT_LOG("charger control alarm is timeout.");

	queue_work(batt_charger_ctrl_wq, &batt_charger_ctrl_work);
}

int htc_battery_core_update(enum power_supplies_type supply)
{
	if (battery_register) {
		BATT_ERR("No battery driver exists.");
		return -1;
	}

	mutex_lock(&battery_core_info.info_lock);
	htc_battery_update_battery_info(0);
	mutex_unlock(&battery_core_info.info_lock);

	power_supply_changed(&htc_power_supplies[supply]);

	return 0;
}
EXPORT_SYMBOL_GPL(htc_battery_core_update);

int htc_battery_core_register(struct device *dev,
				struct htc_battery_core *htc_battery)
{
	int i, rc = 0;

	if (!battery_register) {
		BATT_ERR("Only one battery driver could exist.");
		return -1;
	}
	battery_register = 0;

	mutex_init(&battery_core_info.info_lock);

	if (htc_battery->func_show_batt_attr)
		battery_core_info.func.func_show_batt_attr =
					htc_battery->func_show_batt_attr;
	if (htc_battery->func_get_battery_info)
		battery_core_info.func.func_get_battery_info =
					htc_battery->func_get_battery_info;
	if (htc_battery->func_charger_control)
		battery_core_info.func.func_charger_control =
					htc_battery->func_charger_control;

	if (htc_battery->func_set_full_level)
		battery_core_info.func.func_set_full_level =
					htc_battery->func_set_full_level;
	if (htc_battery->func_is_temperature_fault)
		battery_core_info.func.func_is_temperature_fault =
					htc_battery->func_is_temperature_fault;

	/* init power supplier framework */
	for (i = 0; i < ARRAY_SIZE(htc_power_supplies); i++) {
		rc = power_supply_register(dev, &htc_power_supplies[i]);
		if (rc)
			BATT_ERR("Failed to register power supply"
				" (%d)\n", rc);
	}

	/* create htc detail attributes */
	htc_battery_create_attrs(htc_power_supplies[CHARGER_BATTERY].dev);

	/* init charger_ctrl_timer */
	charger_ctrl_stat = ENABLE_CHARGER;
	INIT_WORK(&batt_charger_ctrl_work, batt_charger_ctrl_func);
	alarm_init(&batt_charger_ctrl_alarm,
			ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
			batt_charger_ctrl_alarm_handler);
	batt_charger_ctrl_wq =
			create_singlethread_workqueue("charger_ctrl_timer");

	/* init battery parameters. */
	battery_core_info.update_time = jiffies;
	battery_core_info.present = 1;
	battery_core_info.htc_charge_full = 0;
	battery_core_info.rep.charging_source = CHARGER_BATTERY;
	battery_core_info.rep.batt_id = 1;
	battery_core_info.rep.batt_vol = 4000;
	battery_core_info.rep.batt_temp = 285;
	battery_core_info.rep.batt_current = 162;
	battery_core_info.rep.level = 66;
	battery_core_info.rep.full_bat = 1580000;
	battery_core_info.rep.full_level = 100;
	/* initial state = -1, valid values: 0 or 1 */
	battery_core_info.rep.temp_fault = -1;
	/* zero means battey info is not ready */
	battery_core_info.rep.batt_state = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(htc_battery_core_register);
