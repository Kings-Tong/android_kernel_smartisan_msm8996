/*
 * FPC1020 Fingerprint sensor device driver
 *
 * This driver will control the platform resources that the FPC fingerprint
 * sensor needs to operate. The major things are probing the sensor to check
 * that it is actually connected and let the Kernel know this and with that also
 * enabling and disabling of regulators, enabling and disabling of platform
 * clocks, controlling GPIOs such as SPI chip select, sensor reset line, sensor
 * IRQ line, MISO and MOSI lines.
 *
 * The driver will expose most of its available functionality in sysfs which
 * enables dynamic control of these features from eg. a user space process.
 *
 * The sensor's IRQ events will be pushed to Kernel's event handling system and
 * are exposed in the drivers event node. This makes it possible for a user
 * space process to poll the input node and receive IRQ events easily. Usually
 * this node is available under /dev/input/eventX where 'X' is a number given by
 * the event system. A user space process will need to traverse all the event
 * nodes and ask for its parent's name (through EVIOCGNAME) which should match
 * the value in device tree named input-device-name.
 *
 * This driver will NOT send any SPI commands to the sensor it only controls the
 * electrical parts.
 *
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <soc/qcom/scm.h>
#include <linux/platform_device.h>
#include <linux/wakelock.h>

#include <linux/input/keypad.h>

#define FPC1020_NAME "fpc1020"

#define FPC1020_RESET_LOW_US 1000
#define FPC1020_RESET_HIGH1_US 100
#define FPC1020_RESET_HIGH2_US 1250
#define PWR_ON_STEP_SLEEP 100
#define PWR_ON_STEP_RANGE1 100
#define PWR_ON_STEP_RANGE2 900
#define FPC_TTW_HOLD_TIME 1000
#define NUM_PARAMS_REG_ENABLE_SET 2

#define FPC_TEE_HANDLE_SPI_CLOCK

static const char * const pctl_names[] = {
	"fpc1020_spi_active",
	"fpc1020_reset_reset",
	"fpc1020_reset_active",
	"fpc1020_irq_active",
};

struct vreg_config {
	char *name;
	unsigned long vmin;
	unsigned long vmax;
	int ua_load;
};

static const struct vreg_config const vreg_conf[] = {
	{ "vdd_ana", 1800000UL, 1800000UL, 6000, },
	{ "vcc_spi", 1800000UL, 1800000UL, 10, },
	{ "vdd_io", 1800000UL, 1800000UL, 6000, },
};

struct fpc1020_data {
	struct device *dev;
	struct pinctrl *fingerprint_pinctrl;
	struct pinctrl_state *pinctrl_state[ARRAY_SIZE(pctl_names)];
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
	u32 max_speed_hz;
	struct clk *iface_clk;
	struct clk *core_clk;
#endif
	struct regulator *vreg[ARRAY_SIZE(vreg_conf)];
	struct wake_lock ttw_wl;
	int irq_gpio;
	int rst_gpio;
	struct mutex lock;
	bool prepared;
	bool wakeup_enabled;
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
	bool clocks_enabled;
	bool clocks_suspended;
#endif

	struct input_handler input_handler;
	int key_code;
};

static int vreg_setup(struct fpc1020_data *fpc1020, const char *name,
		bool enable)
{
	size_t i;
	int rc;
	struct regulator *vreg;
	struct device *dev = fpc1020->dev;

	for (i = 0; i < ARRAY_SIZE(fpc1020->vreg); i++) {
		const char *n = vreg_conf[i].name;
		if (!strncmp(n, name, strlen(n)))
			goto found;
	}
	dev_err(dev, "Regulator %s not found\n", name);
	return -EINVAL;
found:
	vreg = fpc1020->vreg[i];
	if (enable) {
		if (!vreg) {
			vreg = regulator_get(dev, name);
			if (IS_ERR(vreg)) {
				dev_err(dev, "Unable to get  %s\n", name);
				return PTR_ERR(vreg);
			}
		}
		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg, vreg_conf[i].vmin,
					vreg_conf[i].vmax);
			if (rc)
				dev_err(dev,
						"Unable to set voltage on %s, %d\n",
						name, rc);
		}
		rc = regulator_set_optimum_mode(vreg, vreg_conf[i].ua_load);
		if (rc < 0)
			dev_err(dev, "Unable to set current on %s, %d\n",
					name, rc);
		rc = regulator_enable(vreg);
		if (rc) {
			dev_err(dev, "error enabling %s: %d\n", name, rc);
			regulator_put(vreg);
			vreg = NULL;
		}
		fpc1020->vreg[i] = vreg;
	} else {
		if (vreg) {
			if (regulator_is_enabled(vreg)) {
				regulator_disable(vreg);
				dev_dbg(dev, "disabled %s\n", name);
			}
			regulator_put(vreg);
			fpc1020->vreg[i] = NULL;
		}
		rc = 0;
	}
	return rc;
}

#ifndef FPC_TEE_HANDLE_SPI_CLOCK
static int set_clks(struct fpc1020_data *fpc1020, bool enable)
{
	int rc = 0;
	mutex_lock(&fpc1020->lock);

	if (enable == fpc1020->clocks_enabled)
		goto out;

	if (enable) {
		rc = clk_set_rate(fpc1020->core_clk, fpc1020->max_speed_hz);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error setting clk_rate: %u, %d\n",
					__func__, fpc1020->max_speed_hz,
					rc);
			goto out;
		}
		rc = clk_prepare_enable(fpc1020->core_clk);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error enabling core clk: %d\n",
					__func__, rc);
			goto out;
		}

		rc = clk_prepare_enable(fpc1020->iface_clk);
		if (rc) {
			dev_err(fpc1020->dev,
					"%s: Error enabling iface clk: %d\n",
					__func__, rc);
			clk_disable_unprepare(fpc1020->core_clk);
			goto out;
		}
		dev_dbg(fpc1020->dev, "%s ok. clk rate %u hz\n", __func__,
				fpc1020->max_speed_hz);

		fpc1020->clocks_enabled = true;
	} else {
		clk_disable_unprepare(fpc1020->iface_clk);
		clk_disable_unprepare(fpc1020->core_clk);
		fpc1020->clocks_enabled = false;
	}

out:
	mutex_unlock(&fpc1020->lock);
	return rc;
}

static ssize_t clk_enable_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	return set_clks(fpc1020, (*buf == '1')) ? : count;
}

static DEVICE_ATTR(clk_enable, S_IWUSR, NULL, clk_enable_set);
#endif
/**
 * Will try to select the set of pins (GPIOS) defined in a pin control node of
 * the device tree named @p name.
 *
 * The node can contain several eg. GPIOs that is controlled when selecting it.
 * The node may activate or deactivate the pins it contains, the action is
 * defined in the device tree node itself and not here. The states used
 * internally is fetched at probe time.
 *
 * @see pctl_names
 * @see fpc1020_probe
 */
static int select_pin_ctl(struct fpc1020_data *fpc1020, const char *name)
{
	size_t i;
	int rc;
	struct device *dev = fpc1020->dev;
	for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		if (!strncmp(n, name, strlen(n))) {
			rc = pinctrl_select_state(fpc1020->fingerprint_pinctrl,
					fpc1020->pinctrl_state[i]);
			if (rc)
				dev_err(dev, "cannot select '%s'\n", name);
			else
				dev_dbg(dev, "Selected '%s'\n", name);
			goto exit;
		}
	}
	rc = -EINVAL;
	dev_err(dev, "%s:'%s' not found\n", __func__, name);
exit:
	return rc;
}

/**
 * Will setup clocks, GPIOs, and regulators to correctly initialize the touch
 * sensor to be ready for work.
 *
 * In the correct order according to the sensor spec this function will
 * enable/disable regulators, SPI platform clocks, and reset line, all to set
 * the sensor in a correct power on or off state "electrical" wise.
 *
 * @see  spi_prepare_set
 * @note This function will not send any commands to the sensor it will only
 *       control it "electrically".
 */
static int device_prepare(struct fpc1020_data *fpc1020, bool enable)
{
	int rc;

	mutex_lock(&fpc1020->lock);
	if (enable && !fpc1020->prepared) {
		fpc1020->prepared = true;
		select_pin_ctl(fpc1020, "fpc1020_reset_reset");

		rc = vreg_setup(fpc1020, "vcc_spi", true);
		if (rc)
			goto exit;

		rc = vreg_setup(fpc1020, "vdd_io", true);
		if (rc)
			goto exit_1;

		rc = vreg_setup(fpc1020, "vdd_ana", true);
		if (rc)
			goto exit_2;

		usleep_range(PWR_ON_STEP_SLEEP, PWR_ON_STEP_RANGE2);

		(void)select_pin_ctl(fpc1020, "fpc1020_reset_active");
		usleep_range(PWR_ON_STEP_SLEEP, PWR_ON_STEP_RANGE1);
	} else if (!enable && fpc1020->prepared) {
		rc = 0;

		(void)select_pin_ctl(fpc1020, "fpc1020_reset_reset");
		usleep_range(PWR_ON_STEP_SLEEP, PWR_ON_STEP_RANGE2);

		(void)vreg_setup(fpc1020, "vdd_ana", false);
exit_2:
		(void)vreg_setup(fpc1020, "vdd_io", false);
exit_1:
		(void)vreg_setup(fpc1020, "vcc_spi", false);
exit:
		fpc1020->prepared = false;
	} else {
		rc = 0;
	}
	mutex_unlock(&fpc1020->lock);
	return rc;
}

/**
 * sysfs node for controlling whether the driver is allowed
 * to wake up the platform on interrupt.
 */
static ssize_t wakeup_enable_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable"))) {
		fpc1020->wakeup_enabled = true;
		smp_wmb();
	} else if (!strncmp(buf, "disable", strlen("disable"))) {
		fpc1020->wakeup_enabled = false;
		smp_wmb();
	} else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR(wakeup_enable, S_IWUSR, NULL, wakeup_enable_set);


/**
 * sysf node to check the interrupt status of the sensor, the interrupt
 * handler should perform sysf_notify to allow userland to poll the node.
 */
static ssize_t irq_get(struct device *device,
		struct device_attribute *attribute,
		char* buffer)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(device);
	int irq = gpio_get_value(fpc1020->irq_gpio);
	return scnprintf(buffer, PAGE_SIZE, "%i\n", irq);
}


/**
 * writing to the irq node will just drop a printk message
 * and return success, used for latency measurement.
 */
static ssize_t irq_ack(struct device *device,
		struct device_attribute *attribute,
		const char *buffer, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(device);
	dev_dbg(fpc1020->dev, "%s\n", __func__);
	return count;
}

static DEVICE_ATTR(irq, S_IRUSR | S_IWUSR, irq_get, irq_ack);

static struct attribute *attributes[] = {
	&dev_attr_wakeup_enable.attr,
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
	&dev_attr_clk_enable.attr,
#endif
	&dev_attr_irq.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static int keypad_read(u32 *code, void *data)
{
	struct fpc1020_data *fpc1020 = (struct fpc1020_data *) data;

	if (!fpc1020) {
		return -ENODEV;
	}

	*code = fpc1020->key_code;

	return 0;
}

static int keypad_write(u32 code, void *data)
{
	struct fpc1020_data *fpc1020 = (struct fpc1020_data *) data;

	if (!fpc1020) {
		return -ENODEV;
	}

	fpc1020->key_code = code;

	return 0;
}

static int input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id) {
	int rc;
	struct input_handle *handle;
	struct fpc1020_data *fpc1020 =
		container_of(handler, struct fpc1020_data, input_handler);

	if (!strstr(dev->name, "uinput-fpc"))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = FPC1020_NAME;
	handle->private = fpc1020;

	rc = input_register_handle(handle);
	if (rc)
		goto err_input_register_handle;

	rc = input_open_device(handle);
	if (rc)
		goto err_input_open_device;

	return 0;

err_input_open_device:
	input_unregister_handle(handle);
err_input_register_handle:
	kfree(handle);
	return rc;
}

static bool input_filter(struct input_handle *handle, unsigned int type,
		unsigned int code, int value)
{
	struct fpc1020_data *fpc1020 = handle->private;
	return fpc1020->key_code == 0;
}

static void input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;
	//dev_err(fpc1020->dev, "%s\n", __func__);

	/* Make sure 'wakeup_enabled' is updated before using it
	 ** since this is interrupt context (other thread...) */
	smp_rmb();

	if (fpc1020->wakeup_enabled) {
		wake_lock_timeout(&fpc1020->ttw_wl,
				msecs_to_jiffies(FPC_TTW_HOLD_TIME));
	}

	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
		const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc = of_get_named_gpio(np, label, 0);
	if (rc < 0) {
		dev_err(dev, "failed to get '%s'\n", label);
		return rc;
	}
	*gpio = rc;
	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return rc;
	}
	dev_dbg(dev, "%s %d\n", label, *gpio);
	return 0;
}

static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
	size_t i;
	int irqf;
	struct device_node *np = dev->of_node;

	struct fpc1020_data *fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020),
			GFP_KERNEL);
	if (!fpc1020) {
		dev_err(dev,
				"failed to allocate memory for struct fpc1020_data\n");
		rc = -ENOMEM;
		goto exit;
	}

	fpc1020->dev = dev;
	dev_set_drvdata(dev, fpc1020);

	if (!np) {
		dev_err(dev, "no of node found\n");
		rc = -EINVAL;
		goto exit;
	}

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_irq",
			&fpc1020->irq_gpio);
	if (rc)
		goto exit;

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_rst",
			&fpc1020->rst_gpio);
	if (rc)
		goto exit;

#ifndef FPC_TEE_HANDLE_SPI_CLOCK
	if (of_property_read_u32(dev->of_node, "fpc,spi-max-frequency", &fpc1020->max_speed_hz)) {
		fpc1020->max_speed_hz = 4800000;
		dev_err(dev, "%s: Failed to get spi-max-frequency\n", __func__);
	}

	fpc1020->iface_clk = clk_get(dev, "iface_clk");
	if (IS_ERR(fpc1020->iface_clk)) {
		dev_err(dev, "%s: Failed to get iface_clk\n", __func__);
		rc = -EINVAL;
		goto exit;
	}

	fpc1020->core_clk = clk_get(dev, "core_clk");
	if (IS_ERR(fpc1020->core_clk)) {
		dev_err(dev, "%s: Failed to get core_clk\n", __func__);
		rc = -EINVAL;
		goto exit;
	}
#endif

	fpc1020->fingerprint_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc1020->fingerprint_pinctrl)) {
		if (PTR_ERR(fpc1020->fingerprint_pinctrl) == -EPROBE_DEFER) {
			dev_info(dev, "pinctrl not ready\n");
			rc = -EPROBE_DEFER;
			goto exit;
		}
		dev_err(dev, "Target does not use pinctrl\n");
		fpc1020->fingerprint_pinctrl = NULL;
		rc = -EINVAL;
		goto exit;
	}

	for (i = 0; i < ARRAY_SIZE(fpc1020->pinctrl_state); i++) {
		const char *n = pctl_names[i];
		struct pinctrl_state *state =
			pinctrl_lookup_state(fpc1020->fingerprint_pinctrl, n);
		if (IS_ERR(state)) {
			dev_err(dev, "cannot find '%s'\n", n);
			rc = -EINVAL;
			goto exit;
		}
		dev_info(dev, "found pin control %s\n", n);
		fpc1020->pinctrl_state[i] = state;
	}

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	if (rc)
		goto exit;
	rc = select_pin_ctl(fpc1020, "fpc1020_irq_active");
	if (rc)
		goto exit;
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
	rc = select_pin_ctl(fpc1020, "fpc1020_spi_active");
	if (rc)
		goto exit;
#endif

	fpc1020->wakeup_enabled = false;
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
	fpc1020->clocks_enabled = false;
	fpc1020->clocks_suspended = false;
#endif

	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		irqf |= IRQF_NO_SUSPEND;
		device_init_wakeup(dev, 1);
	}
	mutex_init(&fpc1020->lock);

	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
			NULL, fpc1020_irq_handler, irqf,
			dev_name(dev), fpc1020);
	if (rc) {
		dev_err(dev, "could not request irq %d\n",
				gpio_to_irq(fpc1020->irq_gpio));
		goto exit;
	}
	dev_err(dev, "requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));

	/* Request that the interrupt should be wakeable */
	enable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));

	wake_lock_init(&fpc1020->ttw_wl, WAKE_LOCK_SUSPEND, "fpc_ttw_wl");

	fpc1020->input_handler.filter = input_filter;
	fpc1020->input_handler.connect = input_connect;
	fpc1020->input_handler.disconnect = input_disconnect;
	fpc1020->input_handler.name = FPC1020_NAME;
	fpc1020->input_handler.id_table = ids;

	rc = input_register_handler(&fpc1020->input_handler);
	if (rc) {
		dev_err(dev, "failed to register key handler\n");
		goto exit;
	}

	fpc1020->key_code = KEY_BACK;

	keypad_register("home_touch", fpc1020,
		keypad_read,
		keypad_write);

	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc) {
		dev_err(dev, "could not create sysfs\n");
		goto exit;
	}

	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot")) {
		dev_info(dev, "Enabling hardware\n");
		(void)device_prepare(fpc1020, true);
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
		(void)set_clks(fpc1020, false);
#endif
	}

	dev_info(dev, "%s: ok\n", __func__);
exit:
	return rc;
}

static int fpc1020_remove(struct platform_device *pdev)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(&pdev->dev);

	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
	mutex_destroy(&fpc1020->lock);
	wake_lock_destroy(&fpc1020->ttw_wl);
	(void)vreg_setup(fpc1020, "vdd_io", false);
	(void)vreg_setup(fpc1020, "vcc_spi", false);
	(void)vreg_setup(fpc1020, "vdd_ana", false);
	dev_info(&pdev->dev, "%s\n", __func__);
	return 0;
}

#ifndef FPC_TEE_HANDLE_SPI_CLOCK
static int fpc1020_suspend(struct device *dev)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	fpc1020->clocks_suspended = fpc1020->clocks_enabled;
	set_clks(fpc1020, false);
	return 0;
}

static int fpc1020_resume(struct device *dev)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (fpc1020->clocks_suspended)
		set_clks(fpc1020, true);

	return 0;
}

static const struct dev_pm_ops fpc1020_pm_ops = {
	.suspend = fpc1020_suspend,
	.resume = fpc1020_resume,
};
#endif

static struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{}
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name	= FPC1020_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
#ifndef FPC_TEE_HANDLE_SPI_CLOCK
		.pm = &fpc1020_pm_ops,
#endif
	},
	.probe		= fpc1020_probe,
	.remove		= fpc1020_remove,
};

char *fingerprint_id = "unknown fp";
EXPORT_SYMBOL(fingerprint_id);

static int get_fingerprint_id(char *src)
{
	if (src == NULL)
		return 0;

	if (!strcmp(src, "fpc"))
		fingerprint_id = "fpc";
	else if (!strcmp(src, "goodix"))
		fingerprint_id = "goodix";

	pr_info("fingerprint_id = *%s*\n", fingerprint_id);

	return 1;
}
__setup("androidboot.fingerprint.id=", get_fingerprint_id);

static int __init fpc1020_init(void)
{
	int rc = -1;

	if (!strcmp(fingerprint_id, "fpc")) {
		rc = platform_driver_register(&fpc1020_driver);
		if (!rc)
			pr_info("%s OK\n", __func__);
		else
			pr_err("%s %d\n", __func__, rc);
	} else {
		pr_err(" %s error, fingerprint_id = %s \n", __func__, fingerprint_id);
	}

		return rc;
}

static void __exit fpc1020_exit(void)
{
	pr_info("%s\n", __func__);
	platform_driver_unregister(&fpc1020_driver);
}

module_init(fpc1020_init);
module_exit(fpc1020_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
