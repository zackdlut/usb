// SPDX-License-Identifier: GPL-2.0
/*
 * 第 13 章 · 步骤 2：在 sysfs 里读 vendor_info（GET_INFO 控制传输）。
 */
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>

#include "vendor_protocol.h"

struct miniusb_dev {
	struct usb_interface *intf;
	struct usb_device *udev;
};

static int vendor_get_info(struct miniusb_dev *m, struct vendor_info *out)
{
	int ret;

	ret = usb_control_msg(m->udev, usb_rcvctrlpipe(m->udev, 0),
			      VENDOR_REQ_GET_INFO,
			      USB_TYPE_VENDOR | USB_DIR_IN | USB_RECIP_DEVICE,
			      0, 0, out, sizeof(*out), USB_CTRL_GET_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret != sizeof(*out))
		return -EIO;
	return 0;
}

static ssize_t uptime_ms_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct miniusb_dev *m = usb_get_intfdata(intf);
	struct vendor_info info;
	int ret;

	ret = vendor_get_info(m, &info);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", info.uptime_ms);
}

static DEVICE_ATTR_RO(uptime_ms);

static struct attribute *miniusb_attrs[] = {
	&dev_attr_uptime_ms.attr,
	NULL,
};

static const struct attribute_group miniusb_attr_group = {
	.attrs = miniusb_attrs,
};

static int miniusb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct miniusb_dev *m;
	int ret;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	m->intf = usb_get_intf(intf);
	m->udev = usb_get_dev(udev);
	usb_set_intfdata(intf, m);

	ret = sysfs_create_group(&intf->dev.kobj, &miniusb_attr_group);
	if (ret) {
		usb_put_intf(m->intf);
		usb_put_dev(m->udev);
		kfree(m);
		return ret;
	}

	dev_info(&intf->dev, "probe 完成，读 uptime：cat .../uptime_ms\n");
	return 0;
}

static void miniusb_disconnect(struct usb_interface *intf)
{
	struct miniusb_dev *m = usb_get_intfdata(intf);

	sysfs_remove_group(&intf->dev.kobj, &miniusb_attr_group);
	usb_set_intfdata(intf, NULL);
	usb_put_intf(m->intf);
	usb_put_dev(m->udev);
	kfree(m);
}

static struct usb_device_id miniusb_table[] = {
	{ USB_DEVICE(VENDOR_VID, VENDOR_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, miniusb_table);

static struct usb_driver miniusb_driver = {
	.name       = "miniusb_step2",
	.probe      = miniusb_probe,
	.disconnect = miniusb_disconnect,
	.id_table   = miniusb_table,
};

module_usb_driver(miniusb_driver);

MODULE_AUTHOR("usb tutorial");
MODULE_DESCRIPTION("第13章步骤2：sysfs + GET_INFO");
MODULE_LICENSE("GPL");
