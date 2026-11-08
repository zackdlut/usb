// SPDX-License-Identifier: GPL-2.0
/*
 * 第 13 章 · 步骤 3：完整驱动 — misc 字符设备 + 控制传输 + 批量回环。
 * 协议与第 10 章 vendor_client、第 11 章固件一致。
 */
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include "vendor_protocol.h"

#define MINIUSB_NAME "miniusb_vendor"
#define MINIUSB_MAX_XFER 4096

/*
 * 教学模块：假定总线上只有一个 cafe:4011。
 * 产品级驱动应把 miniusb_dev 挂在链表上，用 iminor(inode) 查找。
 */
static struct miniusb_dev *miniusb_singleton;

struct miniusb_dev {
	struct usb_interface *intf;
	struct usb_device *udev;
	struct mutex lock;
	struct miscdevice misc;

	unsigned char *bulk_in_buf;
	unsigned char *bulk_out_buf;
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

static int vendor_set_led(struct miniusb_dev *m, int on)
{
	return usb_control_msg(m->udev, usb_sndctrlpipe(m->udev, 0),
			       VENDOR_REQ_SET_LED,
			       USB_TYPE_VENDOR | USB_DIR_OUT | USB_RECIP_DEVICE,
			       on ? 1 : 0, 0, NULL, 0, USB_CTRL_GET_TIMEOUT);
}

static int miniusb_loopback(struct miniusb_dev *m, const unsigned char *tx,
			    int tx_len, unsigned char *rx)
{
	int ret, got, chunk;

	if (tx_len <= 0 || tx_len > MINIUSB_MAX_XFER)
		return -EINVAL;

	memcpy(m->bulk_out_buf, tx, tx_len);

	ret = usb_bulk_msg(m->udev, usb_sndbulkpipe(m->udev, VENDOR_EP_OUT),
			    m->bulk_out_buf, tx_len, &chunk, 5000);
	if (ret)
		return ret;

	got = 0;
	while (got < tx_len) {
		ret = usb_bulk_msg(m->udev, usb_rcvbulkpipe(m->udev, VENDOR_EP_IN),
				   m->bulk_in_buf + got, tx_len - got, &chunk, 5000);
		if (ret)
			return ret;
		got += chunk;
	}

	memcpy(rx, m->bulk_in_buf, tx_len);
	return tx_len;
}

static long miniusb_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct miniusb_dev *m = filp->private_data;
	struct vendor_info info;
	int ret, led;

	switch (cmd) {
	case 0xCAFE01: /* 读 vendor_info */
		mutex_lock(&m->lock);
		ret = vendor_get_info(m, &info);
		mutex_unlock(&m->lock);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case 0xCAFE02: /* SET_LED：arg 0/1 */
		led = (int)arg;
		mutex_lock(&m->lock);
		ret = vendor_set_led(m, led);
		mutex_unlock(&m->lock);
		return ret;
	default:
		return -ENOTTY;
	}
}

static ssize_t miniusb_read(struct file *filp, char __user *buf, size_t count,
			    loff_t *ppos)
{
	struct miniusb_dev *m = filp->private_data;
	struct vendor_info info;
	int ret;

	if (count < sizeof(info))
		return -EINVAL;

	mutex_lock(&m->lock);
	ret = vendor_get_info(m, &info);
	mutex_unlock(&m->lock);
	if (ret)
		return ret;

	if (copy_to_user(buf, &info, sizeof(info)))
		return -EFAULT;
	return sizeof(info);
}

static ssize_t miniusb_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct miniusb_dev *m = filp->private_data;
	unsigned char *kbuf;
	int ret;

	if (count == 0 || count > MINIUSB_MAX_XFER)
		return -EINVAL;

	kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, buf, count)) {
		kfree(kbuf);
		return -EFAULT;
	}

	mutex_lock(&m->lock);
	ret = miniusb_loopback(m, kbuf, (int)count, kbuf);
	mutex_unlock(&m->lock);

	if (ret < 0) {
		kfree(kbuf);
		return ret;
	}

	if (copy_to_user((void __user *)buf, kbuf, ret)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kfree(kbuf);
	return ret;
}

static int miniusb_open(struct inode *inode, struct file *filp)
{
	if (!miniusb_singleton)
		return -ENODEV;
	filp->private_data = miniusb_singleton;
	return 0;
}

static const struct file_operations miniusb_fops = {
	.owner          = THIS_MODULE,
	.open           = miniusb_open,
	.read           = miniusb_read,
	.write          = miniusb_write,
	.unlocked_ioctl = miniusb_ioctl,
	.llseek         = noop_llseek,
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

	mutex_init(&m->lock);
	m->intf = usb_get_intf(intf);
	m->udev = usb_get_dev(udev);

	m->bulk_in_buf = kmalloc(MINIUSB_MAX_XFER, GFP_KERNEL);
	m->bulk_out_buf = kmalloc(MINIUSB_MAX_XFER, GFP_KERNEL);
	if (!m->bulk_in_buf || !m->bulk_out_buf) {
		ret = -ENOMEM;
		goto err_buf;
	}

	usb_set_intfdata(intf, m);

	m->misc.minor = MISC_DYNAMIC_MINOR;
	m->misc.name  = MINIUSB_NAME;
	m->misc.fops  = &miniusb_fops;
	m->misc.parent = &intf->dev;

	ret = misc_register(&m->misc);
	if (ret)
		goto err_misc;

	miniusb_singleton = m;

	dev_info(&intf->dev, "就绪：/dev/%s（与 vendor_client 同一协议）\n",
		 MINIUSB_NAME);
	return 0;

err_misc:
	usb_set_intfdata(intf, NULL);
err_buf:
	kfree(m->bulk_in_buf);
	kfree(m->bulk_out_buf);
	usb_put_intf(m->intf);
	usb_put_dev(m->udev);
	kfree(m);
	return ret;
}

static void miniusb_disconnect(struct usb_interface *intf)
{
	struct miniusb_dev *m = usb_get_intfdata(intf);

	miniusb_singleton = NULL;
	misc_deregister(&m->misc);
	usb_set_intfdata(intf, NULL);
	kfree(m->bulk_in_buf);
	kfree(m->bulk_out_buf);
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
	.name       = "miniusb_vendor",
	.probe      = miniusb_probe,
	.disconnect = miniusb_disconnect,
	.id_table   = miniusb_table,
};

module_usb_driver(miniusb_driver);

MODULE_AUTHOR("usb tutorial");
MODULE_DESCRIPTION("第13章步骤3：厂商设备字符驱动");
MODULE_LICENSE("GPL");
