// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "hbp_core.h"
#include "hbp_report.h"

#define HBP_REPORT_SKIP_AXES	BIT(0)
#define HBP_REPORT_KEYS_UP	BIT(2)
#define HBP_REPORT_FORCE_RELEASE	BIT(3)
#define HBP_REPORT_MAX_DELAY_US	50000U
#define HBP_REPORT_TRACKING_BASE	16

static_assert(sizeof(struct hbp_report_request) == 16);
static_assert(sizeof(struct hbp_report_slot) == 48);
static_assert(sizeof(struct hbp_report_payload) == 488);
static_assert(offsetof(struct hbp_report_dual, immediate) == 12);
static_assert(offsetof(struct hbp_report_dual, delayed) == 500);
static_assert(sizeof(struct hbp_report_dual) == 992);

struct hbp_report_device {
	struct miscdevice miscdev;
	struct kref refs;
	/* The worker never takes this lock: cancellation may wait for it. */
	struct mutex submit_lock;
	struct hbp_device *hbp_dev;
	struct hrtimer timer;
	struct work_struct work;
	struct hbp_report_payload pending;
	u64 pending_timestamp;
	char name[sizeof("hbp_report") + 10];
};

static void hbp_report_release_slot(struct input_dev *input, unsigned int slot)
{
	input_mt_slot(input, slot);
	input_report_abs(input, ABS_MT_TRACKING_ID, -1);
}

static void hbp_report_release_keys(struct input_dev *input)
{
	input_report_key(input, BTN_TOUCH, 0);
	input_report_key(input, BTN_TOOL_FINGER, 0);
}

/* Called with mreport held, including when the native IRQ path is active. */
static void hbp_report_points(struct hbp_device *hbp_dev,
			      const struct hbp_report_payload *payload)
{
	struct input_dev *input = hbp_dev->i_dev;
	u32 mask = payload->mask & TOUCH_BIT_CHECK;
	u32 flags = payload->flags;
	unsigned int i;

	if (!mask) {
		if (!(flags & HBP_REPORT_FORCE_RELEASE) && hbp_dev->up_status) {
			hbp_report_release_keys(input);
			return;
		}

		if ((flags & HBP_REPORT_FORCE_RELEASE) ||
		    !(flags & HBP_REPORT_KEYS_UP)) {
			for (i = 0; i < TOUCH_MAX_FINGERS; i++)
				hbp_report_release_slot(input, i);
			hbp_dev->irq_slot = 0;
			hbp_dev->up_status = true;
		}
		hbp_report_release_keys(input);
		input_sync(input);
		return;
	}

	hbp_dev->up_status = false;
	for (i = 0; i < TOUCH_MAX_FINGERS; i++) {
		const struct hbp_report_slot *point = &payload->slots[i];
		u8 status = point->status;

		if (point->id >= TOUCH_MAX_FINGERS)
			continue;

		/* Releases are meaningful even when their mask bit is clear. */
		if (status == 3)
			hbp_report_release_slot(input, i);
		if (!(mask & BIT(i)))
			continue;

		switch (status) {
		case 1:
		case 2:
			input_mt_slot(input, i);
			/* Userspace owns contact IDs; do not allocate new ones here. */
			input_report_abs(input, ABS_MT_TRACKING_ID,
					 point->id + HBP_REPORT_TRACKING_BASE);
			input_report_abs(input, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
			input_report_key(input, BTN_TOUCH, 1);
			input_report_key(input, BTN_TOOL_FINGER, 1);
			input_report_abs(input, ABS_MT_POSITION_X, (u16)point->x);
			input_report_abs(input, ABS_MT_POSITION_Y, (u16)point->y);
			if (!(flags & HBP_REPORT_SKIP_AXES)) {
				input_report_abs(input, ABS_MT_WIDTH_MAJOR,
						 (u8)point->width_major);
				if ((u8)point->touch_major)
					input_report_abs(input, ABS_MT_TOUCH_MAJOR,
							 (u8)point->touch_major);
				input_report_abs(input, ABS_MT_PRESSURE,
						 (u16)point->pressure);
				input_report_abs(input, ABS_MT_TOUCH_MINOR,
						 (u8)point->touch_minor);
			}
			break;
		case 3:
		case 255:
			break;
		default:
			hbp_report_release_slot(input, i);
			break;
		}
		hbp_dev->irq_slot |= BIT(i);
	}

	if (flags & HBP_REPORT_KEYS_UP)
		hbp_report_release_keys(input);
	input_sync(input);
}

static int hbp_report_apply(struct hbp_device *hbp_dev,
			    const struct hbp_report_payload *payload,
			    u64 timestamp_ns)
{
	if (!hbp_dev || !hbp_dev->i_dev)
		return -ENODEV;

	mutex_lock(&hbp_dev->mreport);
	if (timestamp_ns)
		input_set_timestamp(hbp_dev->i_dev, ns_to_ktime(timestamp_ns));
	hbp_report_points(hbp_dev, payload);
	mutex_unlock(&hbp_dev->mreport);
	return 0;
}

static void hbp_report_work(struct work_struct *work)
{
	struct hbp_report_device *report =
		container_of(work, struct hbp_report_device, work);
	struct hbp_device *hbp_dev = report->hbp_dev;
	struct hbp_report_payload payload;
	u64 timestamp_ns;

	mutex_lock(&hbp_dev->mreport);
	payload = report->pending;
	timestamp_ns = report->pending_timestamp;
	mutex_unlock(&hbp_dev->mreport);
	hbp_report_apply(hbp_dev, &payload, timestamp_ns);
}

static enum hrtimer_restart hbp_report_timer(struct hrtimer *timer)
{
	struct hbp_report_device *report =
		container_of(timer, struct hbp_report_device, timer);

	schedule_work(&report->work);
	return HRTIMER_NORESTART;
}

static long hbp_report_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct hbp_report_device *report = file->private_data;
	struct hbp_report_request request;
	struct hbp_report_dual dual;
	struct hbp_device *hbp_dev;
	u32 delay_us;
	int ret;

	mutex_lock(&report->submit_lock);
	hbp_dev = report->hbp_dev;
	if (!hbp_dev) {
		ret = -ENODEV;
		goto unlock;
	}
	if (cmd != HBP_REPORT_IOCTL) {
		ret = -EINVAL;
		goto unlock;
	}
	if (copy_from_user(&request, (void __user *)arg, sizeof(request))) {
		ret = -EFAULT;
		goto unlock;
	}
	if (!request.payload || request.size != sizeof(dual)) {
		ret = -EINVAL;
		goto unlock;
	}
	if (copy_from_user(&dual, u64_to_user_ptr(request.payload), sizeof(dual))) {
		ret = -EFAULT;
		goto unlock;
	}

	/* Do not hold mreport while waiting for the previous worker. */
	hrtimer_cancel(&report->timer);
	cancel_work_sync(&report->work);
	ret = hbp_report_apply(hbp_dev, &dual.immediate, dual.timestamp_ns);
	if (ret || !dual.delay_us)
		goto unlock;

	delay_us = min(dual.delay_us, HBP_REPORT_MAX_DELAY_US);
	mutex_lock(&hbp_dev->mreport);
	report->pending = dual.delayed;
	report->pending_timestamp = dual.timestamp_ns + (u64)delay_us * NSEC_PER_USEC;
	mutex_unlock(&hbp_dev->mreport);
	hrtimer_start(&report->timer, ns_to_ktime((u64)delay_us * NSEC_PER_USEC),
		      HRTIMER_MODE_REL);
unlock:
	mutex_unlock(&report->submit_lock);
	return ret;
}

static void hbp_report_free(struct kref *refs)
{
	struct hbp_report_device *report =
		container_of(refs, struct hbp_report_device, refs);

	kfree(report);
}

static int hbp_report_open(struct inode *inode, struct file *file)
{
	struct hbp_report_device *report =
		container_of(file->private_data, struct hbp_report_device, miscdev);

	/* misc_open serializes this reference acquisition with deregistration. */
	kref_get(&report->refs);
	file->private_data = report;
	return nonseekable_open(inode, file);
}

static int hbp_report_release(struct inode *inode, struct file *file)
{
	struct hbp_report_device *report = file->private_data;

	kref_put(&report->refs, hbp_report_free);
	return 0;
}

static const struct file_operations hbp_report_fops = {
	.owner = THIS_MODULE,
	.open = hbp_report_open,
	.release = hbp_report_release,
	.unlocked_ioctl = hbp_report_ioctl,
};

static void hbp_report_unregister(void *data)
{
	struct hbp_report_device *report = data;

	misc_deregister(&report->miscdev);
	mutex_lock(&report->submit_lock);
	hrtimer_cancel(&report->timer);
	cancel_work_sync(&report->work);
	/* Existing file descriptors stay valid, but can no longer report. */
	report->hbp_dev = NULL;
	mutex_unlock(&report->submit_lock);
	kref_put(&report->refs, hbp_report_free);
}

int hbp_report_register(struct hbp_device *hbp_dev)
{
	struct hbp_report_device *report;
	int ret;

	report = kzalloc(sizeof(*report), GFP_KERNEL);
	if (!report)
		return -ENOMEM;

	kref_init(&report->refs);
	mutex_init(&report->submit_lock);
	report->hbp_dev = hbp_dev;
	INIT_WORK(&report->work, hbp_report_work);
	hrtimer_init(&report->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	report->timer.function = hbp_report_timer;
	snprintf(report->name, sizeof(report->name), "hbp_report%d", hbp_dev->id);
	report->miscdev.minor = MISC_DYNAMIC_MINOR;
	report->miscdev.name = report->name;
	report->miscdev.fops = &hbp_report_fops;
	report->miscdev.parent = hbp_dev->dev;
	report->miscdev.mode = 0600;

	ret = misc_register(&report->miscdev);
	if (ret) {
		kref_put(&report->refs, hbp_report_free);
		return ret;
	}

	return devm_add_action_or_reset(hbp_dev->dev, hbp_report_unregister, report);
}
