/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __UFCS_CABLE_INFO_H__
#define __UFCS_CABLE_INFO_H__

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>

#define UFCS_CABLE_INFO_SIZE 10
#define UFCS_CABLE_INFO_LEGACY_SIZE 8

static inline void ufcs_cable_info_legacy_to_ext(u64 legacy, u8 *info)
{
	u32 value;

	memset(info, 0, UFCS_CABLE_INFO_SIZE);
	value = (legacy & 0xff) * 100;
	info[0] = value;
	info[1] = value >> 8;
	value = ((legacy >> 8) & 0xff) * 100;
	info[2] = value;
	info[3] = value >> 8;
	value = ((legacy >> 16) & 0xffff) / 10;
	info[4] = value;
	info[5] = value >> 8;
}

static inline int ufcs_cable_info_ext_to_legacy(const u8 *info, u64 *legacy)
{
	u32 cable_current = (info[0] | (info[1] << 8)) / 100;
	u32 voltage = (info[2] | (info[3] << 8)) / 100;
	u32 impedance = (info[4] | (info[5] << 8)) * 10;

	/* Round power limits down to the legacy whole-amp/volt units. */
	if (cable_current > 0xff || voltage > 0xff || impedance > 0xffff)
		return -ERANGE;
	*legacy = ((u64)impedance << 16) | (voltage << 8) | cable_current;
	return 0;
}
#endif
