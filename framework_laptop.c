// SPDX-License-Identifier: GPL-2.0+
/*
 * Framework Laptop ACPI Driver
 *
 * Copyright (C) 2022 Dustin L. Howett
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/pci_ids.h>
#include <linux/power_supply.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/version.h>

#include <acpi/battery.h>

#define DRV_NAME "framework_laptop"
#define FRAMEWORK_LAPTOP_EC_DEVICE_NAME "cros-ec-dev"

static struct platform_device *fwdevice;
static struct device *ec_device;
struct framework_data {
	struct platform_device *pdev;
	struct led_classdev kb_led;
	struct device *hwmon_dev;
};

#define EC_CMD_CHARGE_LIMIT_CONTROL 0x3E03

enum ec_chg_limit_control_modes {
	/* Disable all setting, charge control by charge_manage */
	CHG_LIMIT_DISABLE	= BIT(0),
	/* Set maximum and minimum percentage */
	CHG_LIMIT_SET_LIMIT	= BIT(1),
	/* Host read current setting */
	CHG_LIMIT_GET_LIMIT	= BIT(3),
	/* Enable override mode, allow charge to full this time */
	CHG_LIMIT_OVERRIDE	= BIT(7),
};

struct ec_params_ec_chg_limit_control {
	/* See enum ec_chg_limit_control_modes */
	uint8_t modes;
	uint8_t max_percentage;
	uint8_t min_percentage;
} __ec_align1;

struct ec_response_chg_limit_control {
	uint8_t max_percentage;
	uint8_t min_percentage;
} __ec_align1;

static int charge_limit_control(enum ec_chg_limit_control_modes modes, uint8_t max_percentage) {
	struct {
		struct cros_ec_command msg;
		union {
			struct ec_params_ec_chg_limit_control params;
			struct ec_response_chg_limit_control resp;
		};
	} __packed buf;
	struct ec_params_ec_chg_limit_control *params = &buf.params;
	struct ec_response_chg_limit_control *resp = &buf.resp;
	struct cros_ec_command *msg = &buf.msg;
	struct cros_ec_device *ec;
	int ret;

	if (!ec_device)
		return -ENODEV;

	ec = dev_get_drvdata(ec_device);

	memset(&buf, 0, sizeof(buf));

	msg->version = 0;
	msg->command = EC_CMD_CHARGE_LIMIT_CONTROL;
	msg->outsize = sizeof(*params);
	msg->insize = sizeof(*resp);

	params->modes = modes;
	params->max_percentage = max_percentage;

	ret = cros_ec_cmd_xfer_status(ec, msg);
	if (ret < 0) {
		return -EIO;
	}

	return resp->max_percentage;
}

// Get the last set keyboard LED brightness
static enum led_brightness kb_led_get(struct led_classdev *led)
{
	struct {
		struct cros_ec_command msg;
		union {
			struct ec_params_pwm_get_duty p;
			struct ec_response_pwm_get_duty resp;
		};
	} __packed buf;

	struct ec_params_pwm_get_duty *p = &buf.p;
	struct ec_response_pwm_get_duty *resp = &buf.resp;
	struct cros_ec_command *msg = &buf.msg;
	struct cros_ec_device *ec;
	int ret;
	if (!ec_device)
		goto out;

	ec = dev_get_drvdata(ec_device);

	memset(&buf, 0, sizeof(buf));

	p->pwm_type = EC_PWM_TYPE_KB_LIGHT;
	
	msg->version = 0;
	msg->command = EC_CMD_PWM_GET_DUTY;
	msg->insize = sizeof(*resp);
	msg->outsize = sizeof(*p);

	ret = cros_ec_cmd_xfer_status(ec, msg);
	if (ret < 0) {
		goto out;
	}

	return resp->duty * 100 / EC_PWM_MAX_DUTY;

out:
	return 0;
}

// Set the keyboard LED brightness
static int kb_led_set(struct led_classdev *led, enum led_brightness value)
{
	struct {
		struct cros_ec_command msg;
		union {
			struct ec_params_pwm_set_keyboard_backlight params;
		};
	} __packed buf;

	struct ec_params_pwm_set_keyboard_backlight *params = &buf.params;
	struct cros_ec_command *msg = &buf.msg;
	struct cros_ec_device *ec;
	int ret;

	if (!ec_device)
		return -EIO;

	ec = dev_get_drvdata(ec_device);

	memset(&buf, 0, sizeof(buf));
	
	msg->version = 0;
	msg->command = EC_CMD_PWM_SET_KEYBOARD_BACKLIGHT;
	msg->insize = 0;
	msg->outsize = sizeof(*params);

	params->percent = value;

	ret = cros_ec_cmd_xfer_status(ec, msg);
	if (ret < 0) {
		return -EIO;
	}

	return 0;
}


static ssize_t battery_get_threshold(char *buf)
{
	int ret;

	ret = charge_limit_control(CHG_LIMIT_GET_LIMIT, 0);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", (int)ret);
}

static ssize_t battery_set_threshold(const char *buf, size_t count)
{
	int ret;
	int value;

	ret = kstrtouint(buf, 10, &value);
	if (ret)
		return ret;

	if (value > 100)
		return -EINVAL;

	ret = charge_limit_control(CHG_LIMIT_SET_LIMIT, (uint8_t)value);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t charge_control_end_threshold_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return battery_get_threshold(buf);
}

static ssize_t charge_control_end_threshold_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	return battery_set_threshold(buf, count);
}

static DEVICE_ATTR_RW(charge_control_end_threshold);

static struct attribute *framework_laptop_battery_attrs[] = {
	&dev_attr_charge_control_end_threshold.attr,
	NULL,
};

ATTRIBUTE_GROUPS(framework_laptop_battery);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
static int framework_laptop_battery_add(struct power_supply *battery, struct acpi_battery_hook *hook)
#else
static int framework_laptop_battery_add(struct power_supply *battery)
#endif
{
	// Framework EC only supports 1 battery
	if (strcmp(battery->desc->name, "BAT1") != 0)
		return -ENODEV;

	if (device_add_groups(&battery->dev, framework_laptop_battery_groups))
		return -ENODEV;

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
static int framework_laptop_battery_remove(struct power_supply *battery, struct acpi_battery_hook *hook)
#else
static int framework_laptop_battery_remove(struct power_supply *battery)
#endif
{
	device_remove_groups(&battery->dev, framework_laptop_battery_groups);
	return 0;
}

// --- fanN_input ---
// Read the current fan speed from the EC's memory
static ssize_t ec_get_fan_speed(u8 idx, u16 *val)
{
	if (!ec_device)
		return -ENODEV;

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);

	const u8 offset = EC_MEMMAP_FAN + 2 * idx;

	return ec->cmd_readmem(ec, offset, sizeof(*val), val);
}

static ssize_t fw_fan_speed_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);

	u16 val;
	if (ec_get_fan_speed(sen_attr->index, &val) < 0) {
		return -EIO;
	}

	if (val == EC_FAN_SPEED_NOT_PRESENT || val == EC_FAN_SPEED_STALLED) {
		return sysfs_emit(buf, "%u\n", 0);
	}

	// Format as string for sysfs
	return sysfs_emit(buf, "%u\n", val);
}

// --- fanN_target ---
static ssize_t ec_set_target_rpm(u8 idx, u32 *val)
{
	int ret;
	if (!ec_device)
		return -ENODEV;

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);

	struct ec_params_pwm_set_fan_target_rpm_v1 params = {
		.rpm = *val,
		.fan_idx = idx,
	};

	ret = cros_ec_cmd(ec, 1, EC_CMD_PWM_SET_FAN_TARGET_RPM, &params,
			  sizeof(params), NULL, 0);
	if (ret < 0)
		return -EIO;

	return 0;
}

static ssize_t ec_get_target_rpm(u8 idx, u32 *val)
{
	int ret;
	if (!ec_device)
		return -ENODEV;

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);

	struct ec_response_pwm_get_fan_rpm resp;

	// index isn't supported, it should only return fan 0's target

	ret = cros_ec_cmd(ec, 0, EC_CMD_PWM_GET_FAN_TARGET_RPM, NULL, 0, &resp,
			  sizeof(resp));
	if (ret < 0)
		return -EIO;

	*val = resp.rpm;

	return 0;
}

static ssize_t fw_fan_target_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);
	u32 val;

	int err;
	err = kstrtou32(buf, 10, &val);
	if (err < 0)
		return err;

	if (ec_set_target_rpm(sen_attr->index, &val) < 0) {
		return -EIO;
	}

	return count;
}

static ssize_t fw_fan_target_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);

	// Only fan 0 is supported
	if (sen_attr->index != 0) {
		return -EINVAL;
	}

	u32 val;
	if (ec_get_target_rpm(sen_attr->index, &val) < 0) {
		return -EIO;
	}

	// Format as string for sysfs
	return sysfs_emit(buf, "%u\n", val);
}

// --- fanN_fault ---
static ssize_t fw_fan_fault_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);

	u16 val;
	if (ec_get_fan_speed(sen_attr->index, &val) < 0) {
		return -EIO;
	}

	// Format as string for sysfs
	return sysfs_emit(buf, "%u\n", val == EC_FAN_SPEED_NOT_PRESENT);
}

// --- fanN_alarm ---
static ssize_t fw_fan_alarm_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);

	u16 val;
	if (ec_get_fan_speed(sen_attr->index, &val) < 0) {
		return -EIO;
	}

	// Format as string for sysfs
	return sysfs_emit(buf, "%u\n", val == EC_FAN_SPEED_STALLED);
}

// --- pwmN_enable ---
static ssize_t ec_set_auto_fan_ctrl(u8 idx)
{
	int ret;
	if (!ec_device)
		return -ENODEV;

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);

	struct ec_params_auto_fan_ctrl_v1 params = {
		.fan_idx = idx,
	};

	ret = cros_ec_cmd(ec, 1, EC_CMD_THERMAL_AUTO_FAN_CTRL, &params,
			  sizeof(params), NULL, 0);
	if (ret < 0)
		return -EIO;

	return 0;
}

static ssize_t fw_pwm_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);

	// The EC doesn't take any arguments for this command,
	// so we don't need to parse the buffer
	// u32 val;

	// int err;
	// err = kstrtou32(buf, 10, &val);
	// if (err < 0)
	// 	return err;

	if (ec_set_auto_fan_ctrl(sen_attr->index) < 0) {
		return -EIO;
	}

	return count;
}

// --- pwmN ---
static ssize_t ec_set_fan_duty(u8 idx, u32 *val)
{
	int ret;
	if (!ec_device)
		return -ENODEV;

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);

	struct ec_params_pwm_set_fan_duty_v1 params = {
		.percent = *val,
		.fan_idx = idx,
	};

	ret = cros_ec_cmd(ec, 1, EC_CMD_PWM_SET_FAN_DUTY, &params,
			  sizeof(params), NULL, 0);
	if (ret < 0)
		return -EIO;

	return 0;
}

static ssize_t fw_pwm_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct sensor_device_attribute *sen_attr = to_sensor_dev_attr(attr);
	u32 val;

	int err;
	err = kstrtou32(buf, 10, &val);
	if (err < 0)
		return err;

	if (ec_set_fan_duty(sen_attr->index, &val) < 0) {
		return -EIO;
	}

	return count;
}

static ssize_t fw_pwm_min_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%i\n", 0);
}

static ssize_t fw_pwm_max_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%i\n", 100);
}

static ssize_t ec_count_fans(size_t *val)
{
	if (!ec_device)
		return -ENODEV;

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);

	u16 fans[EC_FAN_SPEED_ENTRIES];

	int ret = ec->cmd_readmem(ec, EC_MEMMAP_FAN, sizeof(fans), fans);
	if (ret < 0)
		return -EIO;

	for (size_t i = 0; i < EC_FAN_SPEED_ENTRIES; i++) {
		if (fans[i] == EC_FAN_SPEED_NOT_PRESENT) {
			*val = i;
			return 0;
		}
	}

	*val = EC_FAN_SPEED_ENTRIES;
	return 0;
}

#define FW_ATTRS_PER_FAN 8

// --- hwmon sysfs attributes ---
// clang-format off
static SENSOR_DEVICE_ATTR_RO(fan1_input, fw_fan_speed, 0); // Fan Reading
static SENSOR_DEVICE_ATTR_RW(fan1_target, fw_fan_target, 0); // Target RPM (RW on fan 0 only)
static SENSOR_DEVICE_ATTR_RO(fan1_fault, fw_fan_fault, 0); // Fan Not Present
static SENSOR_DEVICE_ATTR_RO(fan1_alarm, fw_fan_alarm, 0); // Fan Stalled
static SENSOR_DEVICE_ATTR_WO(pwm1_enable, fw_pwm_enable, 0); // Set Fan Control Mode
static SENSOR_DEVICE_ATTR_WO(pwm1, fw_pwm, 0); // Set Fan Speed
static SENSOR_DEVICE_ATTR_RO(pwm1_min, fw_pwm_min, 0); // Min Fan Speed
static SENSOR_DEVICE_ATTR_RO(pwm1_max, fw_pwm_max, 0); // Max Fan Speed
// clang-format on

static SENSOR_DEVICE_ATTR_RO(fan2_input, fw_fan_speed, 1);
static SENSOR_DEVICE_ATTR_WO(fan2_target, fw_fan_target, 1);
static SENSOR_DEVICE_ATTR_RO(fan2_fault, fw_fan_fault, 1);
static SENSOR_DEVICE_ATTR_RO(fan2_alarm, fw_fan_alarm, 1);
static SENSOR_DEVICE_ATTR_WO(pwm2_enable, fw_pwm_enable, 1);
static SENSOR_DEVICE_ATTR_WO(pwm2, fw_pwm, 1);
static SENSOR_DEVICE_ATTR_RO(pwm2_min, fw_pwm_min, 1);
static SENSOR_DEVICE_ATTR_RO(pwm2_max, fw_pwm_max, 1);

static SENSOR_DEVICE_ATTR_RO(fan3_input, fw_fan_speed, 2);
static SENSOR_DEVICE_ATTR_WO(fan3_target, fw_fan_target, 2);
static SENSOR_DEVICE_ATTR_RO(fan3_fault, fw_fan_fault, 2);
static SENSOR_DEVICE_ATTR_RO(fan3_alarm, fw_fan_alarm, 2);
static SENSOR_DEVICE_ATTR_WO(pwm3_enable, fw_pwm_enable, 2);
static SENSOR_DEVICE_ATTR_WO(pwm3, fw_pwm, 2);
static SENSOR_DEVICE_ATTR_RO(pwm3_min, fw_pwm_min, 2);
static SENSOR_DEVICE_ATTR_RO(pwm3_max, fw_pwm_max, 2);

static SENSOR_DEVICE_ATTR_RO(fan4_input, fw_fan_speed, 3);
static SENSOR_DEVICE_ATTR_WO(fan4_target, fw_fan_target, 3);
static SENSOR_DEVICE_ATTR_RO(fan4_fault, fw_fan_fault, 3);
static SENSOR_DEVICE_ATTR_RO(fan4_alarm, fw_fan_alarm, 3);
static SENSOR_DEVICE_ATTR_WO(pwm4_enable, fw_pwm_enable, 3);
static SENSOR_DEVICE_ATTR_WO(pwm4, fw_pwm, 3);
static SENSOR_DEVICE_ATTR_RO(pwm4_min, fw_pwm_min, 3);
static SENSOR_DEVICE_ATTR_RO(pwm4_max, fw_pwm_max, 3);

static struct attribute
	*fw_hwmon_attrs[(EC_FAN_SPEED_ENTRIES * FW_ATTRS_PER_FAN) + 1] = {
		&sensor_dev_attr_fan1_input.dev_attr.attr,
		&sensor_dev_attr_fan1_target.dev_attr.attr,
		&sensor_dev_attr_fan1_fault.dev_attr.attr,
		&sensor_dev_attr_fan1_alarm.dev_attr.attr,
		&sensor_dev_attr_pwm1_enable.dev_attr.attr,
		&sensor_dev_attr_pwm1.dev_attr.attr,
		&sensor_dev_attr_pwm1_min.dev_attr.attr,
		&sensor_dev_attr_pwm1_max.dev_attr.attr,

		&sensor_dev_attr_fan2_input.dev_attr.attr,
		&sensor_dev_attr_fan2_target.dev_attr.attr,
		&sensor_dev_attr_fan2_fault.dev_attr.attr,
		&sensor_dev_attr_fan2_alarm.dev_attr.attr,
		&sensor_dev_attr_pwm2_enable.dev_attr.attr,
		&sensor_dev_attr_pwm2.dev_attr.attr,
		&sensor_dev_attr_pwm2_min.dev_attr.attr,
		&sensor_dev_attr_pwm2_max.dev_attr.attr,

		&sensor_dev_attr_fan3_input.dev_attr.attr,
		&sensor_dev_attr_fan3_target.dev_attr.attr,
		&sensor_dev_attr_fan3_fault.dev_attr.attr,
		&sensor_dev_attr_fan3_alarm.dev_attr.attr,
		&sensor_dev_attr_pwm3_enable.dev_attr.attr,
		&sensor_dev_attr_pwm3.dev_attr.attr,
		&sensor_dev_attr_pwm3_min.dev_attr.attr,
		&sensor_dev_attr_pwm3_max.dev_attr.attr,

		&sensor_dev_attr_fan4_input.dev_attr.attr,
		&sensor_dev_attr_fan4_target.dev_attr.attr,
		&sensor_dev_attr_fan4_fault.dev_attr.attr,
		&sensor_dev_attr_fan4_alarm.dev_attr.attr,
		&sensor_dev_attr_pwm4_enable.dev_attr.attr,
		&sensor_dev_attr_pwm4.dev_attr.attr,
		&sensor_dev_attr_pwm4_min.dev_attr.attr,
		&sensor_dev_attr_pwm4_max.dev_attr.attr,

		NULL,
	};

ATTRIBUTE_GROUPS(fw_hwmon);

static struct acpi_battery_hook framework_laptop_battery_hook = {
	.add_battery = framework_laptop_battery_add,
	.remove_battery = framework_laptop_battery_remove,
	.name = "Framework Laptop Battery Extension",
};

static const struct acpi_device_id device_ids[] = {
	{"FRMW0001", 0},
	{"FRMW0004", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, device_ids);

static const struct dmi_system_id framework_laptop_dmi_table[] __initconst = {
	{
		/* the Framework Laptop */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Framework"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Laptop"),
		},
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(dmi, framework_laptop_dmi_table);

static int device_match_cros_ec(struct device *dev, const void* foo) {
	const char* name = dev_name(dev);
	if (strncmp(name, "cros-ec-dev", 11))
		return 0;
	return 1;
}

static int framework_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct framework_data *data;
	int ret = 0;

	dev = &pdev->dev;

	ec_device = bus_find_device(&platform_bus_type, NULL, NULL, device_match_cros_ec);
	if (!ec_device) {
		dev_err(dev, DRV_NAME ": failed to find EC %s.\n", FRAMEWORK_LAPTOP_EC_DEVICE_NAME);
		return -EINVAL;
	}
	ec_device = ec_device->parent;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);
	data->pdev = pdev;

	data->kb_led.name = DRV_NAME "::kbd_backlight";
	data->kb_led.brightness_get = kb_led_get;
	data->kb_led.brightness_set_blocking = kb_led_set;
	data->kb_led.max_brightness = 100;
	ret = devm_led_classdev_register(&pdev->dev, &data->kb_led);
	if (ret)
		return ret;

#if 0
	/* Register the driver */
	ret = platform_driver_register(&cros_ec_lpc_driver);
	if (ret) {
		pr_err(DRV_NAME ": can't register driver: %d\n", ret);
		return ret;
	}

	/* Register the device, and it'll get hooked up automatically */
	ret = platform_device_register(&cros_ec_lpc_device);
	if (ret) {
		pr_err(DRV_NAME ": can't register device: %d\n", ret);
		platform_driver_unregister(&cros_ec_lpc_driver);
	}
#endif

	struct cros_ec_device *ec = dev_get_drvdata(ec_device);
	if (ec->cmd_readmem) {
		// Count the number of fans
		size_t fan_count;
		if (ec_count_fans(&fan_count) < 0) {
			dev_err(dev, DRV_NAME ": failed to count fans.\n");
			return -EINVAL;
		}
		// NULL terminates the list after the last detected fan
		fw_hwmon_attrs[fan_count * FW_ATTRS_PER_FAN] = NULL;

		data->hwmon_dev = hwmon_device_register_with_groups(
			dev, DRV_NAME, NULL, fw_hwmon_groups);
		if (IS_ERR(data->hwmon_dev))
			return PTR_ERR(data->hwmon_dev);

	} else {
		dev_err(dev, DRV_NAME ": fan readings could not be enabled for this EC %s.\n",
		FRAMEWORK_LAPTOP_EC_DEVICE_NAME);
	}

	battery_hook_register(&framework_laptop_battery_hook);

	return ret;
}

static int framework_remove(struct platform_device *pdev)
{
	struct framework_data *data;

	data = (struct framework_data *)platform_get_drvdata(pdev);

	battery_hook_unregister(&framework_laptop_battery_hook);

	// Make sure it's not null before we try to unregister it
	if (data && data->hwmon_dev)
		hwmon_device_unregister(data->hwmon_dev);

	put_device(ec_device);

	return 0;
}

static struct platform_driver framework_driver = {
	.driver = {
		.name = DRV_NAME,
		.acpi_match_table = device_ids,
	},
	.probe = framework_probe,
	.remove = framework_remove,
};

static int __init framework_laptop_init(void)
{
	int ret;

	if (!dmi_check_system(framework_laptop_dmi_table)) {
		pr_err(DRV_NAME ": unsupported system.\n");
		return -ENODEV;
	}

	ret = platform_driver_register(&framework_driver);
	if (ret)
		goto fail;

	fwdevice = platform_device_alloc(DRV_NAME, PLATFORM_DEVID_NONE);
	if (!fwdevice)
	{
		ret = -ENOMEM;
		goto fail_platform_driver;
	}

	ret = platform_device_add(fwdevice);
	if (ret)
		goto fail_device_add;

	return 0;

fail_device_add:
	platform_device_del(fwdevice);
	fwdevice = NULL;

fail_platform_driver:
	platform_driver_unregister(&framework_driver);

fail:
	return ret;
}

static void __exit framework_laptop_exit(void)
{
	if (fwdevice)
	{
		platform_device_unregister(fwdevice);
		platform_driver_unregister(&framework_driver);
	}
}

module_init(framework_laptop_init);
module_exit(framework_laptop_exit);

MODULE_DESCRIPTION("Framework Laptop Platform Driver");
MODULE_AUTHOR("Dustin L. Howett <dustin@howett.net>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_SOFTDEP("pre: cros_ec_lpcs");
