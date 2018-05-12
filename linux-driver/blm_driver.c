#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>

/*---------------------------------------------------------------------------*/
/*                               DEVICE DECLARATIONS                         */
/*---------------------------------------------------------------------------*/

#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/hidraw.h>
#include "usbhid.h"

#define VENDOR_ID 0x03EB
#define PRODUCT_ID 0x204F
#define DRIVER_NAME "blm_driver"

static struct hid_device *blm;

static int blm_probe(struct hid_device *hdev,
	const struct hid_device_id *id);
static void blm_remove(struct hid_device *dev);

static const struct hid_device_id blm_id_table[] =
{
    { HID_USB_DEVICE(VENDOR_ID, PRODUCT_ID) }, {}
};

static struct hid_driver blm_driver =
{
	.name = DRIVER_NAME,
	.id_table = blm_id_table,
	.probe = blm_probe,
	.remove = blm_remove
};

#define BLM_REPORT_SIZE 2

static __u8 *create_blm_report(void);
static void remove_blm_report(__u8 *blm_report);

static int usbhid_set_raw_report(struct hid_device *hid, unsigned int reportnum,
				 __u8 *buf, size_t count, unsigned char rtype)
{
	struct usbhid_device *usbhid = hid->driver_data;
	struct usb_device *dev = hid_to_usb_dev(hid);
	struct usb_interface *intf = usbhid->intf;
	struct usb_host_interface *interface = intf->cur_altsetting;
	int ret, skipped_report_id = 0;

	if ((rtype == HID_OUTPUT_REPORT) &&
	    (hid->quirks & HID_QUIRK_SKIP_OUTPUT_REPORT_ID))
		buf[0] = 0;
	else
		buf[0] = reportnum;

	if (buf[0] == 0x0) {
		buf++;
		count--;
		skipped_report_id = 1;
	}

	ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			HID_REQ_SET_REPORT,
			USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			((rtype + 1) << 8) | reportnum,
			interface->desc.bInterfaceNumber, buf, count,
			USB_CTRL_SET_TIMEOUT);

	if (ret > 0 && skipped_report_id)
		ret++;

	return ret;
}

/*---------------------------------------------------------------------------*/
/*                               THREAD DECLARATIONS                         */
/*---------------------------------------------------------------------------*/

#include <linux/kthread.h>
#include <linux/delay.h>

#define IO_PERIOD 1000

static volatile char device_io_flag = 0;

static int io_thread_function(void *data);

/*---------------------------------------------------------------------------*/
/*                               PROC DECLARATIONS                           */
/*---------------------------------------------------------------------------*/

#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>

static volatile char should_continue_io_flag = 0;

static struct proc_dir_entry* proc_entry = NULL;
static volatile int timer_active_flag = 0;

static ssize_t module_dev_write(struct file *sp_file,
	const char __user *buf, size_t size, loff_t *offset);

struct file_operations fops =
{
	.write = module_dev_write
};

/*---------------------------------------------------------------------------*/
/*                             BATTERY DECLARATIONS                          */
/*---------------------------------------------------------------------------*/

#include <linux/power_supply.h>

static int get_power_ac_status(void);

/*---------------------------------------------------------------------------*/
/*                               DEVICE OPERATIONS                           */
/*---------------------------------------------------------------------------*/

static int blm_probe(struct hid_device *dev,
                            const struct hid_device_id *id)
{
	printk(KERN_INFO "blm_driver: Device connected.\n");

	int err = hid_parse(dev);
	if (err)
	{
		printk(KERN_ERR "blm_driver: Parse failed.\n");
		return err;
	}

	err = hid_hw_start(dev, HID_CONNECT_HIDRAW);
	if (err)
	{
		printk(KERN_ERR "blm_driver: Start failed.\n");
		return err;
	}

	err = hid_hw_open(dev);
	if (err)
	{
		printk(KERN_ERR "blm_driver: Cannot open hidraw.\n");
		return err;
	}

	blm = dev;

	proc_entry = proc_create("blm", 0666, NULL, &fops);
	if (!proc_entry)
	{
		printk(KERN_ERR "blm_driver: Failed to create vfs entry.\n");
	}

	device_io_flag = 0;

	return 0;
}

static void blm_remove(struct hid_device *dev)
{
	device_io_flag = 0;

	if (proc_entry)
	{
		proc_remove(proc_entry);
	}

	hid_hw_close(dev);
	hid_hw_stop(dev);

	printk(KERN_INFO "blm_driver: Device disconnected.\n");
}

static __u8 *create_blm_report(void)
{
	__u8 *io_data = kmalloc(BLM_REPORT_SIZE, GFP_KERNEL);
	if (!io_data)
	{
		printk(KERN_ERR "blm_driver: Out of memory.\n");
		return NULL;
	}

	io_data[0] = 0;
	io_data[1] = (get_power_ac_status() ==
		POWER_SUPPLY_STATUS_DISCHARGING) ? 0b10000000 : 0b00000000;

	return io_data;
}

static void remove_blm_report(__u8 *blm_report)
{
	kfree(blm_report);
}

void usb_io_function(void)
{
	__u8 *blm_report = create_blm_report();
	
	if (blm_report)
	{
		usbhid_set_raw_report(blm, 0, blm_report,
			BLM_REPORT_SIZE, HID_OUTPUT_REPORT);
		remove_blm_report(blm_report);
	}
}

/*---------------------------------------------------------------------------*/
/*                               THREAD OPERATIONS                           */
/*---------------------------------------------------------------------------*/

static int io_thread_function(void *data)
{
	while (device_io_flag)
	{
		usb_io_function();
		msleep(IO_PERIOD);
	}
}

/*---------------------------------------------------------------------------*/
/*                               PROC OPERATIONS                             */
/*---------------------------------------------------------------------------*/

#define COMMAND_BUF_LEN 5
static char command_buf[COMMAND_BUF_LEN];

static ssize_t module_dev_write(struct file *sp_file,
	const char __user *buf, size_t size, loff_t *offset)
{
	*command_buf = 0;
	strncpy_from_user(command_buf, buf, COMMAND_BUF_LEN);

	switch (*command_buf)
	{
		case '0':
			printk(KERN_INFO "blm_driver: User stopped io.\n");
			device_io_flag = 0;
			break;

		case '1':
			printk(KERN_INFO "blm_driver: User started io.\n");
			if (!device_io_flag)
			{
				device_io_flag = 1;
				kthread_run(io_thread_function, NULL, "blm_io_thread");
			}
			break;

		default:
			break;
	}
}

/*---------------------------------------------------------------------------*/
/*                              BATTERY OPERATIONS                           */
/*---------------------------------------------------------------------------*/


static int get_power_ac_status(void)
{
	union power_supply_propval val;
	struct power_supply *psy = power_supply_get_by_name("BAT0");
	power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS , &val);
	return val.intval;
}

/*---------------------------------------------------------------------------*/
/*                               MODULE OPERATIONS                           */
/*---------------------------------------------------------------------------*/

module_driver(blm_driver, hid_register_driver, hid_unregister_driver);

MODULE_LICENSE("GPL");