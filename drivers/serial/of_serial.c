/*
 *  Serial Port driver for Open Firmware platform devices
 *
 *    Copyright (C) 2006 Arnd Bergmann <arnd@arndb.de>, IBM Corp.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/serial_core.h>
#include <linux/serial_8250.h>

#include <asm/of_device.h>
#include <asm/prom.h>

/*
 * Fill a struct uart_port for a given device node
 */
static int __devinit of_platform_serial_setup(struct of_device *ofdev,
					int type, struct uart_port *port)
{
	struct resource resource;
	struct device_node *np = ofdev->node;
	const unsigned int *clk, *spd;
	int ret;

	memset(port, 0, sizeof *port);
	spd = get_property(np, "current-speed", NULL);
	clk = get_property(np, "clock-frequency", NULL);
	if (!clk) {
		dev_warn(&ofdev->dev, "no clock-frequency property set\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(np, 0, &resource);
	if (ret) {
		dev_warn(&ofdev->dev, "invalid address\n");
		return ret;
	}

	spin_lock_init(&port->lock);
	port->mapbase = resource.start;
	port->irq = irq_of_parse_and_map(np, 0);
	port->iotype = UPIO_MEM;
	port->type = type;
	port->uartclk = *clk;
	port->flags = UPF_SHARE_IRQ | UPF_BOOT_AUTOCONF | UPF_IOREMAP
		| UPF_FIXED_PORT;
	port->dev = &ofdev->dev;
	port->custom_divisor = *clk / (16 * (*spd));

	return 0;
}

/*
 * Try to register a serial port
 */
static int __devinit of_platform_serial_probe(struct of_device *ofdev,
						const struct of_device_id *id)
{
	struct uart_port port;
	int port_type;
	int ret;

	if (of_find_property(ofdev->node, "used-by-rtas", NULL))
		return -EBUSY;

	port_type = (unsigned long)id->data;
	ret = of_platform_serial_setup(ofdev, port_type, &port);
	if (ret)
		goto out;

	switch (port_type) {
	case PORT_UNKNOWN:
		dev_info(&ofdev->dev, "Unknown serial port found, "
			"attempting to use 8250 driver\n");
		/* fallthrough */
	case PORT_8250 ... PORT_MAX_8250:
		ret = serial8250_register_port(&port);
		break;
	default:
		/* need to add code for these */
		ret = -ENODEV;
		break;
	}
	if (ret < 0)
		goto out;

	ofdev->dev.driver_data = (void *)(unsigned long)ret;
	return 0;
out:
	irq_dispose_mapping(port.irq);
	return ret;
}

/*
 * Release a line
 */
static int of_platform_serial_remove(struct of_device *ofdev)
{
	int line = (unsigned long)ofdev->dev.driver_data;
	serial8250_unregister_port(line);
	return 0;
}

/*
 * A few common types, add more as needed.
 */
static struct of_device_id __devinitdata of_platform_serial_table[] = {
	{ .type = "serial", .compatible = "ns8250",   .data = (void *)PORT_8250, },
	{ .type = "serial", .compatible = "ns16450",  .data = (void *)PORT_16450, },
	{ .type = "serial", .compatible = "ns16550",  .data = (void *)PORT_16550, },
	{ .type = "serial", .compatible = "ns16750",  .data = (void *)PORT_16750, },
	{ .type = "serial",			      .data = (void *)PORT_UNKNOWN, },
	{ /* end of list */ },
};

static struct of_platform_driver __devinitdata of_platform_serial_driver = {
	.owner = THIS_MODULE,
	.name = "of_serial",
	.probe = of_platform_serial_probe,
	.remove = of_platform_serial_remove,
	.match_table = of_platform_serial_table,
};

static int __init of_platform_serial_init(void)
{
	return of_register_platform_driver(&of_platform_serial_driver);
}
module_init(of_platform_serial_init);

static void __exit of_platform_serial_exit(void)
{
	return of_unregister_platform_driver(&of_platform_serial_driver);
};
module_exit(of_platform_serial_exit);

MODULE_AUTHOR("Arnd Bergmann <arnd@arndb.de>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Serial Port driver for Open Firmware platform devices");
