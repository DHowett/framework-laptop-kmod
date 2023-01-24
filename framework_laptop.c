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

#include <acpi/battery.h>

#define DRV_NAME "framework_laptop"
#define FRAMEWORK_LAPTOP_EC_DEVICE_NAME "cros_ec_lpcs.0"

static struct device *ec_device;

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
	msg->insize = sizeof(*resp);
	msg->outsize = sizeof(*params);

	params->modes = modes;
	params->max_percentage = max_percentage;

	ret = cros_ec_cmd_xfer_status(ec, msg);
	if (ret < 0) {
		return -EIO;
	}

	return resp->max_percentage;
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

static int framework_laptop_battery_add(struct power_supply *battery)
{
	// Framework EC only supports 1 battery
	if (strcmp(battery->desc->name, "BAT1") != 0)
		return -ENODEV;

	if (device_add_groups(&battery->dev, framework_laptop_battery_groups))
		return -ENODEV;

	return 0;
}

static int framework_laptop_battery_remove(struct power_supply *battery)
{
	device_remove_groups(&battery->dev, framework_laptop_battery_groups);
	return 0;
}

static struct acpi_battery_hook framework_laptop_battery_hook = {
	.add_battery = framework_laptop_battery_add,
	.remove_battery = framework_laptop_battery_remove,
	.name = "Framework Laptop Battery Extension",
};

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

static int __init framework_laptop_init(void)
{
	int ret = 0;

	if (!dmi_check_system(framework_laptop_dmi_table)) {
		pr_err(DRV_NAME ": unsupported system.\n");
		return -ENODEV;
	}

	ec_device = bus_find_device_by_name(&platform_bus_type, NULL, FRAMEWORK_LAPTOP_EC_DEVICE_NAME);
	if (!ec_device)
		return -EINVAL;

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

	battery_hook_register(&framework_laptop_battery_hook);

	return ret;
}

static void __exit framework_laptop_exit(void)
{
	battery_hook_unregister(&framework_laptop_battery_hook);

	put_device(ec_device);
}

module_init(framework_laptop_init);
module_exit(framework_laptop_exit);

MODULE_DESCRIPTION("Framework Laptop Platform Driver");
MODULE_AUTHOR("Dustin L. Howett <dustin@howett.net>");
MODULE_LICENSE("GPL");
