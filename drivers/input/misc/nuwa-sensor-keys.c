// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sensor inputs of Nuwa robots (face touch, PIR). Like the vendor kernel,
 * every edge of the sensor GPIO sends a tap of one key code for the high
 * level and another for the low level, so the stock key layouts and apps
 * keep working.
 */

#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

struct nuwa_sensor {
	struct input_dev *input;
	struct gpio_desc *gpio;
	u32 high_code;
	u32 low_code;
	u32 recovery_code;
	bool suppress;
	bool recovery;
};

static void nuwa_sensor_tap(struct input_dev *input, unsigned int code)
{
	input_report_key(input, code, 1);
	input_sync(input);
	input_report_key(input, code, 0);
	input_sync(input);
}

static irqreturn_t nuwa_sensor_irq(int irq, void *data)
{
	struct nuwa_sensor *sensor = data;
	int level;

	if (READ_ONCE(sensor->suppress))
		return IRQ_HANDLED;

	level = gpiod_get_value_cansleep(sensor->gpio);
	if (level < 0)
		return IRQ_HANDLED;

	if (READ_ONCE(sensor->recovery) && sensor->recovery_code) {
		input_report_key(sensor->input, sensor->recovery_code, !level);
		input_sync(sensor->input);
	} else {
		nuwa_sensor_tap(sensor->input,
				level ? sensor->high_code : sensor->low_code);
	}

	return IRQ_HANDLED;
}

/* The vendor name: writing a non-zero value suppresses the events. */
static ssize_t enable_switch_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct nuwa_sensor *sensor = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(sensor->suppress));
}

static ssize_t enable_switch_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct nuwa_sensor *sensor = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	WRITE_ONCE(sensor->suppress, !!val);
	return count;
}
static DEVICE_ATTR_RW(enable_switch);

static ssize_t recovery_switch_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nuwa_sensor *sensor = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(sensor->recovery));
}

static ssize_t recovery_switch_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct nuwa_sensor *sensor = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	WRITE_ONCE(sensor->recovery, !!val);
	return count;
}
static DEVICE_ATTR_RW(recovery_switch);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct nuwa_sensor *sensor = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gpiod_get_value_cansleep(sensor->gpio));
}
static DEVICE_ATTR_RO(state);

static struct attribute *nuwa_sensor_attrs[] = {
	&dev_attr_enable_switch.attr,
	&dev_attr_recovery_switch.attr,
	&dev_attr_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(nuwa_sensor);

static int nuwa_sensor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nuwa_sensor *sensor;
	const char *label;
	int irq, ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "Failed to enable vdd\n");

	sensor->gpio = devm_gpiod_get(dev, NULL, GPIOD_IN);
	if (IS_ERR(sensor->gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->gpio),
				     "Failed to get the sensor GPIO\n");

	if (device_property_read_u32(dev, "nuwa,high-code", &sensor->high_code) ||
	    device_property_read_u32(dev, "nuwa,low-code", &sensor->low_code))
		return dev_err_probe(dev, -EINVAL, "Missing key codes\n");
	device_property_read_u32(dev, "nuwa,recovery-code",
				 &sensor->recovery_code);

	if (device_property_read_string(dev, "label", &label))
		label = dev_name(dev);

	sensor->input = devm_input_allocate_device(dev);
	if (!sensor->input)
		return -ENOMEM;

	sensor->input->name = label;
	input_set_capability(sensor->input, EV_KEY, sensor->high_code);
	input_set_capability(sensor->input, EV_KEY, sensor->low_code);
	if (sensor->recovery_code)
		input_set_capability(sensor->input, EV_KEY,
				     sensor->recovery_code);

	ret = input_register_device(sensor->input);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, sensor);

	irq = gpiod_to_irq(sensor->gpio);
	if (irq < 0)
		return dev_err_probe(dev, irq, "Failed to get the GPIO IRQ\n");

	ret = devm_request_threaded_irq(dev, irq, NULL, nuwa_sensor_irq,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					label, sensor);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request the IRQ\n");

	if (device_property_read_bool(dev, "wakeup-source")) {
		device_init_wakeup(dev, true);
		ret = devm_pm_set_wake_irq(dev, irq);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to set the wake IRQ\n");
	}

	return 0;
}

static const struct of_device_id nuwa_sensor_of_match[] = {
	{ .compatible = "nuwa,sensor-keys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nuwa_sensor_of_match);

static struct platform_driver nuwa_sensor_driver = {
	.probe = nuwa_sensor_probe,
	.driver = {
		.name = "nuwa-sensor-keys",
		.of_match_table = nuwa_sensor_of_match,
		.dev_groups = nuwa_sensor_groups,
	},
};
module_platform_driver(nuwa_sensor_driver);

MODULE_DESCRIPTION("Nuwa robot sensor inputs");
MODULE_LICENSE("GPL");
