/*
 *  ctr_gpiointc.c
 *
 *  Copyright (C) 2021 Wolfvak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DRIVER_NAME "3ds-gpiointc"
#define pr_fmt(fmt)	DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/irqchip/chained_irq.h>

#define to_gpiointc(map)	\
	container_of(map, struct ctr_gpiointc, map[(map)->lid])

#define REGOFF_TYPE(intc, i)	(i)
#define REGOFF_ENABLE(intc, i)	((intc)->nreg + (i))

struct ctr_gpiointc_map {
	int lid;
	int hwid;

	int virq;
	struct gpio_desc *gpio;
};

struct ctr_gpiointc {
	struct device *dev;

	int nreg;
	void __iomem *regs;

	int irq_base;
	struct irq_domain *irqdom;
	struct irq_chip_generic *irqgc;

	int nr_irqs;
	struct ctr_gpiointc_map map[];
};

static void ctr_gpiointc_irq_toggle(struct irq_data *d, int enable)
{
	void __iomem *en_reg;
	unsigned irqn, mask, ie;
	struct irq_chip_generic *irqgc = irq_data_get_irq_chip_data(d);
	struct ctr_gpiointc *intc = irqgc->private;

	irqn = d->hwirq;
	irqn = intc->map[irqn].hwid;
	mask = BIT(irqn % 8);
	en_reg = intc->regs + REGOFF_ENABLE(intc, irqn / 8);

	irq_gc_lock(irqgc);
	ie = ioread8(en_reg);
	ie = enable ? (ie | mask) : (ie & ~mask);
	iowrite8(ie, en_reg);
	irq_gc_unlock(irqgc);
}

static void ctr_gpiointc_irq_mask(struct irq_data *d)
{
	ctr_gpiointc_irq_toggle(d, 0);
}

static void ctr_gpiointc_irq_unmask(struct irq_data *d)
{
	ctr_gpiointc_irq_toggle(d, 1);
}

static int ctr_gpiointc_irq_set_type(struct irq_data *d, unsigned int type)
{
	void __iomem *type_reg;
	unsigned irqn, mask, itype;
	struct irq_chip_generic *irqgc = irq_data_get_irq_chip_data(d);
	struct ctr_gpiointc *intc = irqgc->private;

	irqn = d->hwirq;
	irqn = intc->map[irqn].hwid;
	mask = BIT(irqn % 8);
	type_reg = intc->regs + REGOFF_TYPE(intc, irqn / 8);

	if ((type != IRQ_TYPE_EDGE_RISING) && (type != IRQ_TYPE_EDGE_FALLING))
		return -EINVAL;

	irq_gc_lock(irqgc);
	itype = ioread8(type_reg);
	itype = (type == IRQ_TYPE_EDGE_RISING) ? (itype | mask) : (itype & ~mask);
	iowrite8(itype, type_reg);
	irq_gc_unlock(irqgc);
	return 0;
}

static irqreturn_t ctr_gpiointc_irq(int irq, void *data)
{
	struct ctr_gpiointc_map *map = data;
	struct ctr_gpiointc *intc = to_gpiointc(map);
	pr_err("got irq %d %d\n", map->lid, map->hwid);
	generic_handle_irq(irq_find_mapping(intc->irqdom, map->lid));
	return IRQ_HANDLED;
}

static int ctr_gpiointc_xlate(struct irq_domain *h, struct device_node *node,
				const u32 *intspec, u32 intsize, irq_hw_number_t *out_hwirq,
				u32 *out_type)
{
	if (node != irq_domain_get_of_node(h))
		return -ENODEV;

	if (intsize < 2)
		return -EINVAL;

	switch(intspec[1]) {
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		*out_type = intspec[1];
		break;

	default:
		return -EINVAL;
	}

	*out_hwirq = intspec[0];
	return 0;
}

static const struct irq_domain_ops ctr_gpiointc_irq_domain_ops = {
	.xlate = ctr_gpiointc_xlate,
};

static int ctr_gpiointc_domap(struct platform_device *pdev, struct ctr_gpiointc *intc, int n)
{
	int irq;
	struct gpio_desc *gpio;
	struct device *dev = &pdev->dev;
	struct ctr_gpiointc_map *map = &intc->map[n];

	irq = platform_get_irq(pdev, n);
	if (irq < 0)
		return irq;

	gpio = devm_gpiod_get_index(dev, NULL, n, GPIOD_IN);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);

	map->lid = n;
	map->hwid = n ? 1 : 9;
	map->virq = irq;
	map->gpio = gpio;

	return devm_request_irq(dev, irq,
		ctr_gpiointc_irq, 0, dev_name(dev), map);
}

static int ctr_gpiointc_initirq(struct device *dev, struct ctr_gpiointc *intc)
{
	int i, err;
	struct irq_chip_type *ct;

	/* mask all irqs initially */
	for (i = 0; i < intc->nreg; i++)
		iowrite8(0, intc->regs + REGOFF_ENABLE(intc, i));

	intc->irq_base = devm_irq_alloc_descs(dev, -1, 0, intc->nr_irqs, -1);
	if (intc->irq_base < 0)
		return intc->irq_base;

	intc->irqgc = devm_irq_alloc_generic_chip(dev, dev_name(dev), 1,
		intc->irq_base, intc->regs, handle_simple_irq);
	if (!intc->irqgc)
		return -ENOMEM;

	intc->irqgc->private = intc;
	ct = intc->irqgc->chip_types;
	ct->type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	ct->chip.irq_mask = ctr_gpiointc_irq_mask;
	ct->chip.irq_unmask = ctr_gpiointc_irq_unmask;
	ct->chip.irq_set_type = ctr_gpiointc_irq_set_type;

	err = devm_irq_setup_generic_chip(dev, intc->irqgc, IRQ_MSK(intc->nr_irqs),
			0, IRQ_NOREQUEST, IRQ_NOPROBE);
	if (err < 0)
		return err;

	intc->irqdom = irq_domain_add_simple(dev->of_node, intc->nr_irqs,
		intc->irq_base, &ctr_gpiointc_irq_domain_ops, intc);
	if (!intc->irqdom)
		return -ENODEV;

	return 0;
}

static int ctr_gpiointc_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct resource *mem;
	int i, err, nirq, ngpio;
	struct ctr_gpiointc *intc;

	dev = &pdev->dev;

	ngpio = gpiod_count(dev, NULL);
	if (ngpio < 0)
		return ngpio;

	nirq = platform_irq_count(pdev);
	if (nirq < 0)
		return nirq;

	if (nirq != ngpio)
		return -EINVAL;

	intc = devm_kzalloc(dev,
		struct_size(intc, map, nirq), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	intc->regs = devm_ioremap_resource(dev, mem);
	if (IS_ERR(intc->regs))
		return PTR_ERR(intc->regs);

	intc->dev = dev;
	intc->nr_irqs = nirq;
	intc->nreg = resource_size(mem) / 2;

	err = ctr_gpiointc_initirq(dev, intc);
	if (err < 0)
		return err;

	for (i = 0; i < ngpio; i++) {
		err = ctr_gpiointc_domap(pdev, intc, i);
		if (err < 0)
			return err;
	}

	return 0;
}

static const struct of_device_id ctr_gpiointc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{ },
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

MODULE_DESCRIPTION("Nintendo 3DS GPIO IRQ controller");
MODULE_AUTHOR("Wolfvak");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
