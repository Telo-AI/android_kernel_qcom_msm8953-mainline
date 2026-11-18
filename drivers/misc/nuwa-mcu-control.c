// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power switch of the motion controller in Nuwa robots. The vendor robot
 * service switches it through /proc/nuwa_mcu_en, so it keeps that interface.
 */

#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define NUWA_MCU_PROC_NAME	"nuwa_mcu_en"

static int nuwa_mcu_show(struct seq_file *m, void *v)
{
	struct gpio_desc *enable_gpio = m->private;

	seq_printf(m, "%d\n", gpiod_get_value_cansleep(enable_gpio));
	return 0;
}

static int nuwa_mcu_open(struct inode *inode, struct file *file)
{
	return single_open(file, nuwa_mcu_show, pde_data(inode));
}

static ssize_t nuwa_mcu_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct gpio_desc *enable_gpio = pde_data(file_inode(file));
	char c;

	if (!count)
		return 0;

	if (get_user(c, buf))
		return -EFAULT;

	/* Like the vendor driver, anything but a leading '1' switches it off */
	gpiod_set_value_cansleep(enable_gpio, c == '1');

	return count;
}

static const struct proc_ops nuwa_mcu_proc_ops = {
	.proc_open = nuwa_mcu_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = nuwa_mcu_write,
};

static void nuwa_mcu_remove_proc(void *data)
{
	remove_proc_entry(NUWA_MCU_PROC_NAME, NULL);
}

static int nuwa_mcu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_desc *enable_gpio;

	enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(enable_gpio))
		return dev_err_probe(dev, PTR_ERR(enable_gpio),
				     "Failed to get the enable GPIO\n");

	if (!proc_create_data(NUWA_MCU_PROC_NAME, 0, NULL, &nuwa_mcu_proc_ops,
			      enable_gpio))
		return -ENOMEM;

	return devm_add_action_or_reset(dev, nuwa_mcu_remove_proc, NULL);
}

static const struct of_device_id nuwa_mcu_of_match[] = {
	{ .compatible = "nuwa,mcu-control" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nuwa_mcu_of_match);

static struct platform_driver nuwa_mcu_driver = {
	.probe = nuwa_mcu_probe,
	.driver = {
		.name = "nuwa-mcu-control",
		.of_match_table = nuwa_mcu_of_match,
	},
};
module_platform_driver(nuwa_mcu_driver);

MODULE_DESCRIPTION("Nuwa robot motion controller power switch");
MODULE_LICENSE("GPL");
