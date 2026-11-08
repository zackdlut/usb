// SPDX-License-Identifier: GPL-2.0
/*
 * 第 13 章 · 步骤 1：只做 probe / disconnect，在 dmesg 里确认驱动被匹配。
 */
#include <linux/module.h>
#include <linux/usb.h>

#include "vendor_protocol.h"

static int miniusb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);

	dev_info(&intf->dev, "probe: %04x:%04x 总线=%d 地址=%d\n",
		 le16_to_cpu(udev->descriptor.idVendor),
		 le16_to_cpu(udev->descriptor.idProduct),
		 udev->bus->busnum, udev->devnum);
	return 0;
}

static void miniusb_disconnect(struct usb_interface *intf)
{
	dev_info(&intf->dev, "disconnect\n");
}

static struct usb_device_id miniusb_table[] = {
	{ USB_DEVICE(VENDOR_VID, VENDOR_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, miniusb_table);

static struct usb_driver miniusb_driver = {
	.name       = "miniusb_step1",
	.probe      = miniusb_probe,
	.disconnect = miniusb_disconnect,
	.id_table   = miniusb_table,
};

module_usb_driver(miniusb_driver);

MODULE_AUTHOR("usb tutorial");
MODULE_DESCRIPTION("第13章步骤1：USB probe 演示");
MODULE_LICENSE("GPL");
