#ifndef __HBP_REPORT_H__
#define __HBP_REPORT_H__

#include <linux/ioctl.h>
#include <linux/types.h>

#define TOUCH_MAX_FINGERS 10

/* HBP5 uses a separate node and fixed-width ABI for processed contacts. */
#define HBP_REPORT_IOCTL _IO(0xC5, 0x24)

struct hbp_report_request {
	__aligned_u64 payload;
	__aligned_u64 size;
};

struct hbp_report_slot {
	__u32 index;
	__u32 id;
	__u32 x;
	__u32 y;
	__u32 pressure;
	__u32 raw_width_x;
	__u32 raw_width_y;
	__u32 width_major;
	__u32 touch_major;
	__u32 touch_minor;
	__u32 status;
	__u32 previous_status;
};

struct hbp_report_payload {
	__u32 mask;
	__u32 flags;
	struct hbp_report_slot slots[TOUCH_MAX_FINGERS];
};

struct hbp_report_dual {
	__aligned_u64 timestamp_ns;
	__u32 delay_us;
	struct hbp_report_payload immediate;
	struct hbp_report_payload delayed;
};

struct hbp_device;

int hbp_report_register(struct hbp_device *hbp_dev);

enum hbp_data_type {
	HBP_REQUEST_SOURCE,
	HBP_REPORT_DATA,
};

enum hbp_source_type {
	HBP_POWER_AVDD,
	HBP_POWER_VDDI,
	HBP_POWER_RESET
};

enum hbp_report_type {
	HBP_REPORT_OBJS,
	HBP_REPORT_PEN,
	HBP_REPORT_DIFF,
	HBP_REPORT_RAW
};

enum touch_area {
	AREA_NOTOUCH,
	AREA_EDGE,
	AREA_CRITICAL,
	AREA_NORMAL,
	AREA_CORNER,
} ;

struct object_slot {
	uint16_t x;
	uint16_t y;
	uint16_t z;
	uint8_t  width_major;
	uint8_t  touch_major;
	uint8_t  tx_press;
	uint8_t  rx_press;
	uint8_t  tx_er;
	uint8_t  rx_er;
	uint8_t  status;
	enum touch_area type;
};

struct object_info {
	/*fingers on touch*/
	uint8_t slots;
	struct object_slot obj[TOUCH_MAX_FINGERS];
};

#endif
