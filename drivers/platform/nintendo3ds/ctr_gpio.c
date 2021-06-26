// SPDX-License-Identifier: GPL-2.0
/*
 *  ctr_gpio.c
 *
 *  Copyright (C) 2021 Santiago Herrera
 */

#define DRIVER_NAME "3ds-gpio"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/irqchip/chained_irq.h>

struct ctr_gpio {
	struct device *dev;

	unsigned ngpios;
	void __iomem *dat;
	void __iomem *dir;
	void __iomem *irqedge;
	void __iomem *irqenable;

	struct gpio_chip gpioc;
	struct irq_chip irqc;
};

static void ctr_gpio_irqhandler(struct irq_desc *desc)
{
	int i, irq;
	unsigned long flags, pending;
	struct ctr_gpio *gpio =
		gpiochip_get_data(irq_desc_get_handler_data(desc));
	struct irq_chip *chip = irq_desc_get_chip(desc);

	pending = 0;

	spin_lock_irqsave(&gpio->gpioc.bgpio_lock, flags);
	for (i = 0; i < (gpio->ngpios / 8); i++) {
		u8 data, edge, enabled;
		data = ioread8(gpio->dat + i);
		edge = ioread8(gpio->irqedge + i);
		enabled = ioread8(gpio->irqenable + i);
		pending |= (~(data ^ edge) & enabled) << (8 * i);
	}
	spin_unlock_irqrestore(&gpio->gpioc.bgpio_lock, flags);

	chained_irq_enter(chip, desc);
	for_each_set_bit(irq, &pending, gpio->ngpios) {
		generic_handle_irq(
			irq_find_mapping(gpio->gpioc.irq.domain, irq));
	}
	chained_irq_exit(chip, desc);
}

static void ctr_gpio_irq_toggle(struct ctr_gpio *gpio, unsigned irq,
				unsigned enable)
{
	u8 mask;
	unsigned offset;
	unsigned long flags;

	offset = irq / 8;

	spin_lock_irqsave(&gpio->gpioc.bgpio_lock, flags);
	mask = ioread8(gpio->irqenable + offset);
	mask = enable ? (mask | BIT(irq % 8)) : (mask & ~BIT(irq % 8));
	iowrite8(mask, gpio->irqenable + offset);
	spin_unlock_irqrestore(&gpio->gpioc.bgpio_lock, flags);
}

static void ctr_gpio_irq_mask(struct irq_data *data)
{
	struct ctr_gpio *gpio =
		gpiochip_get_data(irq_data_get_irq_chip_data(data));
	ctr_gpio_irq_toggle(gpio, data->hwirq, 0);
}

static void ctr_gpio_irq_unmask(struct irq_data *data)
{
	struct ctr_gpio *gpio =
		gpiochip_get_data(irq_data_get_irq_chip_data(data));
	ctr_gpio_irq_toggle(gpio, data->hwirq, 1);
}

static int ctr_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	u8 mask;
	unsigned long flags;
	unsigned irq, offset;
	struct ctr_gpio *gpio =
		gpiochip_get_data(irq_data_get_irq_chip_data(data));

	if (type != IRQ_TYPE_EDGE_RISING && type != IRQ_TYPE_EDGE_FALLING)
		return -EINVAL;

	irq = data->hwirq;
	offset = irq / 8;

	spin_lock_irqsave(&gpio->gpioc.bgpio_lock, flags);
	mask = ioread8(gpio->irqedge + offset);

	if (type == IRQ_TYPE_EDGE_RISING) {
		mask |= BIT(irq % 8);
	} else {
		mask &= ~BIT(irq % 8);
	}

	iowrite8(mask, gpio->irqedge + offset);
	spin_unlock_irqrestore(&gpio->gpioc.bgpio_lock, flags);
	return 0;
}

static int ctr_gpiointc_probe(struct platform_device *pdev)
{
	u32 ngpios;
	struct device *dev;
	int err, i, irq_count;
	struct ctr_gpio *gpio;
	unsigned bgpio_flags, nregs;

	dev = &pdev->dev;

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->dev = dev;
	gpio->dat = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->dat))
		return PTR_ERR(gpio->dat);

	err = of_property_read_u32(dev->of_node, "ngpios", &ngpios);
	if (err)
		return err;
	if (ngpios < 1 || ngpios > 32)
		return -EINVAL;

	gpio->ngpios = ngpios;
	nregs = ngpios / 8;

	if (of_property_read_bool(dev->of_node, "interrupt-controller")) {
		/* consider only IRQ capable chips have extra registers */
		gpio->dir = gpio->dat + nregs;
		gpio->irqedge = gpio->dir + nregs;
		gpio->irqenable = gpio->irqedge + nregs;
	} else {
		gpio->dir = NULL;
		gpio->irqedge = NULL;
		gpio->irqenable = NULL;
	}

	if (of_property_read_bool(dev->of_node, "no-output")) {
		bgpio_flags = BGPIOF_NO_OUTPUT;
	} else {
		bgpio_flags = 0;
	}

	err = bgpio_init(&gpio->gpioc, dev, nregs, gpio->dat, gpio->dat, NULL,
			 gpio->dir, NULL, bgpio_flags);
	if (err)
		return err;

	/* get all interrupt parents */
	irq_count = platform_irq_count(pdev);
	if (irq_count < 0)
		return irq_count;

	if (irq_count != 0) {
		struct gpio_irq_chip *girq;

		gpio->irqc.name = dev_name(dev);
		gpio->irqc.irq_mask = ctr_gpio_irq_mask;
		gpio->irqc.irq_unmask = ctr_gpio_irq_unmask;
		gpio->irqc.irq_set_type = ctr_gpio_irq_set_type;

		girq = &gpio->gpioc.irq;
		girq->chip = &gpio->irqc;
		girq->parent_handler = ctr_gpio_irqhandler;
		girq->num_parents = irq_count;
		girq->parents = devm_kcalloc(
			dev, irq_count, sizeof(*girq->parents), GFP_KERNEL);
		if (!girq->parents)
			return -ENOMEM;

		for (i = 0; i < irq_count; i++)
			girq->parents[i] = platform_get_irq(pdev, i);

		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_simple_irq;
	}

	return devm_gpiochip_add_data(dev, &gpio->gpioc, gpio);
}

static const struct of_device_id ctr_gpiointc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{},
};
MODULE_DEVICE_TABLE(of, ctr_gpiointc_of_match);

static struct platform_driver ctr_gpiointc_driver = {
	.probe		= ctr_gpiointc_probe,

	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_gpiointc_of_match),
	},
};

module_platform_driver(ctr_gpiointc_driver);

MODULE_DESCRIPTION("Nintendo 3DS GPIO driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
