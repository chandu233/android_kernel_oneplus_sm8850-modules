// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2026 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[STRATEGY_CTD]([%s][%d]): " fmt, __func__, __LINE__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include <oplus_chg.h>
#include <oplus_chg_monitor.h>
#include <oplus_mms.h>
#include <oplus_mms_gauge.h>
#include <oplus_chg_comm.h>
#include <oplus_strategy.h>

#ifndef CYCLE_TIER_DERATING_CC_THR_MAX
#define CYCLE_TIER_DERATING_CC_THR_MAX	6
#endif

#ifndef CTD_IBUS_TIER_BOUNDS_MAX
#define CTD_IBUS_TIER_BOUNDS_MAX	32
#endif

#ifndef CTD_TEMP_REGION_SELECT_MAX
#define CTD_TEMP_REGION_SELECT_MAX	32
#endif

/*
 * Field ordering: grouped by natural alignment to minimize internal padding
 * on 64-bit kernels (pointers and large sub-structs first, then 4-byte
 * arrays/scalars, with all bools tucked together at the end so they share
 * the trailing pad slot rather than each forcing its own 3-byte gap).
 *
 * The mutex `lock` serializes the runtime path: set_process_data("curve_ibus")
 * and the get_data / get_custom_data readers, plus init's reset of cache
 * fields. The strategy is invoked from the voocphy work context, but the
 * framework does not guarantee the set+get pair is atomic w.r.t. other
 * callers, and last_* / last_vbat_delta_* must not be torn. Static config
 * (cycle_thr, tier_*, temp_range, etc.) is established once in
 * *_alloc_by_node and is read-only afterwards, so it does NOT need to be
 * covered by this lock.
 */
struct ctd_track_info {
	int curve_ibus;
	int derated_ibus;
	int derated_ratio;
	int vbat_delta_mv;
};

struct ctd_strategy {
	struct oplus_chg_strategy strategy;
	struct device_node *node;
	struct mutex lock;
	struct work_struct track_work;
	struct ctd_track_info track_info;
	u32 *tier_ibus_min;
	u32 *tier_ibus_max;
	u32 *tier_derated_ratio;
	u32 *tier_vbat_cap_mv;

	s32 temp_range[CTD_TEMP_REGION_SELECT_MAX];
	u32 cycle_thr[CYCLE_TIER_DERATING_CC_THR_MAX];

	u32 cycle_thr_cnt;
	u32 tier_cnt;
	u32 tier_derated_ratio_cnt;
	u32 temp_range_cnt;
	u32 temp_factor_cnt;

	int curve_ibus;
	int shell_temp_cached;

	int last_curve_ibus;
	u32 last_temp_idx;
	int last_cycle_idx;
	u32 last_tier_idx;
	int last_derated;

	int last_vbat_delta_curve_ibus;
	u32 last_vbat_delta_temp_idx;
	int last_vbat_delta_cycle_idx;
	u32 last_vbat_delta_tier_idx;
	int last_vbat_delta;

	bool shell_temp_valid;
	bool last_valid;
	bool last_vbat_delta_valid;
};

/*
 * File-scope MMS topic handles. Populated lazily at the start of each
 * charging session inside ctd_strategy_init() (which runs under the
 * per-instance ctd->lock in voocphy work context). oplus_mms_get_by_name()
 * is an idempotent name lookup that returns a stable pointer and takes no
 * reference, so the worst case across racing CTD instances is two writes
 * of the same value -- which is safe on every supported arch. Consumers
 * (e.g. ctd_resolve_pos) treat NULL as "topic not registered yet, skip
 * CTD this session" instead of doing their own lazy init, so the lookup
 * pattern lives in exactly one place.
 */
static struct oplus_mms *ctd_gauge_topic;
static struct oplus_mms *ctd_comm_topic;

static void ctd_track_workfn(struct work_struct *work)
{
	struct ctd_strategy *ctd = container_of(work, struct ctd_strategy, track_work);
	struct ctd_track_info info;
	struct oplus_mms *err_topic;
	struct mms_msg *msg;
	int rc;

	mutex_lock(&ctd->lock);
	info = ctd->track_info;
	mutex_unlock(&ctd->lock);

	err_topic = oplus_mms_get_by_name("error");
	if (!err_topic)
		return;

	msg = oplus_mms_alloc_str_msg(MSG_TYPE_ITEM, MSG_PRIO_MEDIUM,
		ERR_ITEM_CYCLE_TIER_DERATING,
		"$$cycle_tier_derating_trig$$curve_ibus@@%d$$derated_ibus@@%d"
		"$$derated_ratio@@%d$$vbat_delta_mv@@%d",
		info.curve_ibus, info.derated_ibus,
		info.derated_ratio, info.vbat_delta_mv);
	if (!msg)
		return;

	rc = oplus_mms_publish_msg_sync(err_topic, msg);
	if (rc < 0)
		kfree(msg);
}

static void ctd_track_try_trigger(struct ctd_strategy *ctd, int curve_ibus, int derated, int ratio, int vbat_delta)
{
	if (derated >= curve_ibus && vbat_delta <= 0)
		return;

	ctd->track_info.curve_ibus = curve_ibus;
	ctd->track_info.derated_ibus = derated;
	ctd->track_info.derated_ratio = ratio;
	ctd->track_info.vbat_delta_mv = vbat_delta;
	schedule_work(&ctd->track_work);
}

static void ctd_clear_tier_data(struct ctd_strategy *ctd)
{
	if (!ctd)
		return;
	kfree(ctd->tier_ibus_min);
	ctd->tier_ibus_min = NULL;
	kfree(ctd->tier_ibus_max);
	ctd->tier_ibus_max = NULL;
	kfree(ctd->tier_derated_ratio);
	ctd->tier_derated_ratio = NULL;
	kfree(ctd->tier_vbat_cap_mv);
	ctd->tier_vbat_cap_mv = NULL;
	ctd->tier_cnt = 0;
	ctd->tier_derated_ratio_cnt = 0;
	ctd->temp_range_cnt = 0;
	ctd->temp_factor_cnt = 0;
}

static void ctd_strategy_free(struct ctd_strategy *ctd)
{
	if (!ctd)
		return;
	cancel_work_sync(&ctd->track_work);
	if (ctd->node)
		of_node_put(ctd->node);
	ctd_clear_tier_data(ctd);
	mutex_destroy(&ctd->lock);
	kfree(ctd);
}

static int ctd_parse_cycle_thr(struct ctd_strategy *ctd, struct device_node *node)
{
	int rc;
	int i;

	rc = of_property_count_elems_of_size(node, "oplus_spec,cycle-derating-cc-thr", sizeof(u32));
	if (rc <= 0 || rc > CYCLE_TIER_DERATING_CC_THR_MAX)
		return rc <= 0 ? -ENODEV : -EINVAL;

	ctd->cycle_thr_cnt = rc;
	rc = of_property_read_u32_array(node, "oplus_spec,cycle-derating-cc-thr",
		ctd->cycle_thr, ctd->cycle_thr_cnt);
	if (rc < 0)
		return -EINVAL;
	for (i = 1; i < ctd->cycle_thr_cnt; i++) {
		if (ctd->cycle_thr[i] <= ctd->cycle_thr[i - 1])
			return -EINVAL;
	}
	chg_info("ctd cycle_thr: cnt=%u first=%u last=%u\n",
		ctd->cycle_thr_cnt, ctd->cycle_thr[0],
		ctd->cycle_thr[ctd->cycle_thr_cnt - 1]);
	for (i = 0; i < ctd->cycle_thr_cnt; i++)
		chg_info("ctd cycle_thr[%d]=%u\n", i, ctd->cycle_thr[i]);
	return 0;
}

static int ctd_parse_temp_ranges(struct ctd_strategy *ctd, struct device_node *node)
{
	int cnt;
	int i;
	u32 raw;
	s32 val;

	cnt = of_property_count_elems_of_size(
		node, "oplus_spec,cycle-derating-temp-region-select", sizeof(u32));
	if (cnt <= 0) {
		ctd->temp_factor_cnt = 1;
		chg_info("ctd temp_range: use full-range default\n");
		return 0;
	}
	if (cnt < 2 || cnt > CTD_TEMP_REGION_SELECT_MAX)
		return -EINVAL;

	for (i = 0; i < cnt; i++) {
		if (of_property_read_u32_index(node,
			"oplus_spec,cycle-derating-temp-region-select", i, &raw) < 0)
			return -EINVAL;
		val = (s32)raw;
		if (i > 0 && val <= ctd->temp_range[i - 1])
			return -EINVAL;
		ctd->temp_range[i] = val;
	}
	ctd->temp_range_cnt = cnt;
	ctd->temp_factor_cnt = cnt - 1;
	chg_info("ctd temp_range: cnt=%u segment=%u first=%d last=%d\n",
		ctd->temp_range_cnt, ctd->temp_factor_cnt,
		ctd->temp_range[0], ctd->temp_range[ctd->temp_range_cnt - 1]);
	for (i = 0; i < ctd->temp_range_cnt; i++)
		chg_info("ctd temp_range[%d]=%dC\n", i, ctd->temp_range[i]);
	return 0;
}

static bool ctd_resolve_temp_index(const struct ctd_strategy *ctd, int shell_temp, u32 *temp_idx)
{
	u32 i;

	if (!ctd || !temp_idx || !ctd->temp_factor_cnt)
		return false;
	if (!ctd->temp_range_cnt) {
		*temp_idx = 0;
		return true;
	}
	for (i = 0; i < ctd->temp_factor_cnt; i++) {
		if (shell_temp >= ctd->temp_range[i] * 10 &&
		    shell_temp < ctd->temp_range[i + 1] * 10) {
			*temp_idx = i;
			return true;
		}
	}
	return false;
}

static int ctd_alloc_tier_arrays(struct ctd_strategy *ctd, u32 cnt)
{
	ctd->tier_ibus_min = kcalloc(cnt, sizeof(*ctd->tier_ibus_min), GFP_KERNEL);
	ctd->tier_ibus_max = kcalloc(cnt, sizeof(*ctd->tier_ibus_max), GFP_KERNEL);
	if (!ctd->tier_ibus_min || !ctd->tier_ibus_max)
		return -ENOMEM;
	return 0;
}

static int ctd_load_tier_arrays(struct ctd_strategy *ctd, struct device_node *node, u32 cnt)
{
	int rc;

	rc = of_property_read_u32_array(node,
		"oplus_spec,cycle-derating-ibus-tier-min", ctd->tier_ibus_min, cnt);
	if (rc < 0)
		return -EINVAL;
	rc = of_property_read_u32_array(node,
		"oplus_spec,cycle-derating-ibus-tier-max", ctd->tier_ibus_max, cnt);
	if (rc < 0)
		return -EINVAL;
	return 0;
}

static int ctd_validate_tier_arrays(const struct ctd_strategy *ctd, u32 cnt)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		if (ctd->tier_ibus_min[i] >= ctd->tier_ibus_max[i])
			return -EINVAL;
		if (i > 0 && ctd->tier_ibus_min[i] < ctd->tier_ibus_max[i - 1])
			return -EINVAL;
	}
	return 0;
}

static void ctd_log_tier_ranges(const struct ctd_strategy *ctd)
{
	u32 i;

	chg_info("ctd tier_range: cnt=%u first=[%u,%u) last=[%u,%u)\n",
		ctd->tier_cnt, ctd->tier_ibus_min[0], ctd->tier_ibus_max[0],
		ctd->tier_ibus_min[ctd->tier_cnt - 1],
		ctd->tier_ibus_max[ctd->tier_cnt - 1]);
	for (i = 0; i < ctd->tier_cnt; i++)
		chg_info("ctd tier_range[%d]=[%u,%u)\n", i,
			ctd->tier_ibus_min[i], ctd->tier_ibus_max[i]);
}

static int ctd_parse_tier_interval_props(struct ctd_strategy *ctd, struct device_node *node)
{
	int cnt_min;
	int cnt_max;
	int rc;

	cnt_min = of_property_count_elems_of_size(node,
		"oplus_spec,cycle-derating-ibus-tier-min", sizeof(u32));
	cnt_max = of_property_count_elems_of_size(node,
		"oplus_spec,cycle-derating-ibus-tier-max", sizeof(u32));
	if (cnt_min <= 0 || cnt_max <= 0 || cnt_min != cnt_max)
		return -EINVAL;
	if (cnt_min > CTD_IBUS_TIER_BOUNDS_MAX)
		return -EINVAL;

	rc = ctd_alloc_tier_arrays(ctd, cnt_min);
	if (rc < 0)
		goto err;
	rc = ctd_load_tier_arrays(ctd, node, cnt_min);
	if (rc < 0)
		goto err;
	rc = ctd_validate_tier_arrays(ctd, cnt_min);
	if (rc < 0)
		goto err;

	ctd->tier_cnt = cnt_min;
	ctd_log_tier_ranges(ctd);
	return 0;

err:
	ctd_clear_tier_data(ctd);
	return rc;
}

static int ctd_parse_ratio_array(struct ctd_strategy *ctd, struct device_node *node)
{
	int ratio_num;
	int rc;
	int i;

	ratio_num = of_property_count_elems_of_size(
		node, "oplus_spec,cycle-derating-tier-derated-ratio", sizeof(u32));
	if (ratio_num <= 0 ||
	    ratio_num != ctd->cycle_thr_cnt * ctd->temp_factor_cnt * ctd->tier_cnt)
		return -EINVAL;

	ctd->tier_derated_ratio = kcalloc(ratio_num,
		sizeof(*ctd->tier_derated_ratio), GFP_KERNEL);
	if (!ctd->tier_derated_ratio)
		return -ENOMEM;
	ctd->tier_derated_ratio_cnt = ratio_num;

	rc = of_property_read_u32_array(node,
		"oplus_spec,cycle-derating-tier-derated-ratio",
		ctd->tier_derated_ratio, ctd->tier_derated_ratio_cnt);
	if (rc < 0)
		return -EINVAL;

	for (i = 0; i < ctd->tier_derated_ratio_cnt; i++) {
		if (ctd->tier_derated_ratio[i] > 100)
			return -EINVAL;
	}
	return 0;
}

static int ctd_parse_vbat_cap_array(struct ctd_strategy *ctd, struct device_node *node)
{
	int vbat_cap_num;
	int rc;

	vbat_cap_num = of_property_count_elems_of_size(
		node, "oplus_spec,cycle-derating-tier-vbat-cap-mv", sizeof(u32));
	if (vbat_cap_num <= 0)
		return 0;
	if (vbat_cap_num != ctd->tier_derated_ratio_cnt)
		return -EINVAL;

	ctd->tier_vbat_cap_mv = kcalloc(vbat_cap_num,
		sizeof(*ctd->tier_vbat_cap_mv), GFP_KERNEL);
	if (!ctd->tier_vbat_cap_mv)
		return -ENOMEM;
	rc = of_property_read_u32_array(node,
		"oplus_spec,cycle-derating-tier-vbat-cap-mv",
		ctd->tier_vbat_cap_mv, vbat_cap_num);
	if (rc < 0)
		return -EINVAL;
	return 0;
}

static void ctd_log_tier_factors(const struct ctd_strategy *ctd)
{
	int i;

	chg_info("ctd factors: ratio_cnt=%u ratio_first=%u ratio_last=%u vbat_cap=%d\n",
		ctd->tier_derated_ratio_cnt, ctd->tier_derated_ratio[0],
		ctd->tier_derated_ratio[ctd->tier_derated_ratio_cnt - 1],
		!!ctd->tier_vbat_cap_mv);
	for (i = 0; i < ctd->tier_derated_ratio_cnt; i++)
		chg_info("ctd ratio[%d]=%u%s\n", i, ctd->tier_derated_ratio[i],
			ctd->tier_vbat_cap_mv ? "" : " (no vbat_cap)");
	if (!ctd->tier_vbat_cap_mv)
		return;
	for (i = 0; i < ctd->tier_derated_ratio_cnt; i++)
		chg_info("ctd vbat_cap[%d]=%u\n", i, ctd->tier_vbat_cap_mv[i]);
}

static int ctd_parse_tier_factors(struct ctd_strategy *ctd, struct device_node *node)
{
	int rc;

	rc = ctd_parse_tier_interval_props(ctd, node);
	if (rc < 0)
		return rc;
	rc = ctd_parse_temp_ranges(ctd, node);
	if (rc < 0)
		goto err;
	rc = ctd_parse_ratio_array(ctd, node);
	if (rc < 0)
		goto err;
	rc = ctd_parse_vbat_cap_array(ctd, node);
	if (rc < 0)
		goto err;
	ctd_log_tier_factors(ctd);
	return 0;

err:
	ctd_clear_tier_data(ctd);
	return rc;
}

static int ctd_get_cycle_index(const struct ctd_strategy *ctd, int batt_cc, bool *skip_derate)
{
	int i;
	bool skip;

	skip = !ctd || !ctd->cycle_thr_cnt || batt_cc <= 0 || batt_cc < ctd->cycle_thr[0];
	if (skip_derate)
		*skip_derate = skip;
	if (skip)
		return 0;

	for (i = ctd->cycle_thr_cnt - 1; i >= 0; i--) {
		if (batt_cc > ctd->cycle_thr[i])
			return i;
	}
	return 0;
}

static bool ctd_tier_resolve_index(const struct ctd_strategy *ctd, int curve_ibus, u32 *tier_out)
{
	u32 i;

	if (!ctd || !tier_out || !ctd->tier_cnt || !ctd->tier_ibus_min || !ctd->tier_ibus_max)
		return false;
	for (i = 0; i < ctd->tier_cnt; i++) {
		if (curve_ibus >= (int)ctd->tier_ibus_min[i] &&
		    curve_ibus < (int)ctd->tier_ibus_max[i]) {
			*tier_out = i;
			return true;
		}
	}
	return false;
}

static void ctd_capture_shell_temp(struct ctd_strategy *ctd)
{
	union mms_msg_data data = { 0 };
	int rc;

	if (!ctd)
		return;
	ctd->shell_temp_valid = false;

	if (!ctd_comm_topic) {
		chg_info("ctd snapshot: comm topic not ready, fallback to per-call read\n");
		return;
	}

	rc = oplus_mms_get_item_data(ctd_comm_topic, COMM_ITEM_SHELL_TEMP, &data, false);
	if (rc < 0) {
		chg_info("ctd snapshot: shell_temp read failed rc=%d, fallback\n", rc);
		return;
	}
	ctd->shell_temp_cached = data.intval;
	ctd->shell_temp_valid = true;
	chg_info("ctd snapshot: shell_temp=%dC cached for this session\n",
		ctd->shell_temp_cached / 10);
}

static int ctd_read_shell_temp(const struct ctd_strategy *ctd, int *out)
{
	union mms_msg_data data = { 0 };
	int rc;

	if (ctd->shell_temp_valid) {
		*out = ctd->shell_temp_cached;
		return 0;
	}
	rc = oplus_mms_get_item_data(ctd_comm_topic, COMM_ITEM_SHELL_TEMP, &data, false);
	if (rc < 0)
		return rc;
	*out = data.intval;
	return 0;
}

static void ctd_apply_resolve_output(int batt_cc, int shell_temp, int cycle_idx,
	u32 temp_idx, u32 tier_idx,
	int *batt_cc_out, int *shell_temp_out, int *cycle_idx_out,
	u32 *temp_idx_out, u32 *tier_idx_out)
{
	if (batt_cc_out)
		*batt_cc_out = batt_cc;
	if (shell_temp_out)
		*shell_temp_out = shell_temp;
	if (cycle_idx_out)
		*cycle_idx_out = cycle_idx;
	if (temp_idx_out)
		*temp_idx_out = temp_idx;
	if (tier_idx_out)
		*tier_idx_out = tier_idx;
}

static bool ctd_last_cache_matches(const struct ctd_strategy *ctd, int curve_ibus,
	u32 temp_idx, int cycle_idx, u32 tier_idx)
{
	return ctd->last_valid &&
		ctd->last_curve_ibus == curve_ibus &&
		ctd->last_temp_idx == temp_idx &&
		ctd->last_cycle_idx == cycle_idx &&
		ctd->last_tier_idx == tier_idx;
}

static bool ctd_last_vbat_cache_matches(const struct ctd_strategy *ctd, int curve_ibus,
	u32 temp_idx, int cycle_idx, u32 tier_idx)
{
	return ctd->last_vbat_delta_valid &&
		ctd->last_vbat_delta_curve_ibus == curve_ibus &&
		ctd->last_vbat_delta_temp_idx == temp_idx &&
		ctd->last_vbat_delta_cycle_idx == cycle_idx &&
		ctd->last_vbat_delta_tier_idx == tier_idx;
}

static int ctd_compute_derated(const struct ctd_strategy *ctd, int curve_ibus,
	u32 tier_idx, u32 pos)
{
	int ratio = ctd->tier_derated_ratio[pos];
	int processed;
	int floor;

	if (ratio <= 0 || ratio >= 100)
		return curve_ibus;
	processed = curve_ibus * ratio / 100;
	floor = (int)ctd->tier_ibus_min[tier_idx];
	return processed > floor ? processed : floor;
}

static int ctd_resolve_indices(struct ctd_strategy *ctd, int curve_ibus,
	int *batt_cc_out, int *shell_temp_out, int *cycle_idx_out,
	u32 *temp_idx_out, u32 *tier_idx_out)
{
	int batt_cc;
	int shell_temp = 0;
	int cycle_idx;
	bool skip_derate;
	u32 temp_idx;
	u32 tier_idx;
	int rc;

	if (!ctd_comm_topic || !ctd_gauge_topic)
		return -ENODEV;

	rc = ctd_read_shell_temp(ctd, &shell_temp);
	if (rc < 0) {
		chg_info("ctd resolve_pos: get shell_temp failed, rc=%d\n", rc);
		return rc;
	}
	if (!ctd_resolve_temp_index(ctd, shell_temp, &temp_idx)) {
		chg_info("ctd resolve_pos: shell_temp=%dC no temp_idx match\n",
			shell_temp / 10);
		return -ENOENT;
	}

	batt_cc = oplus_gauge_get_dec_cv_soh(ctd_gauge_topic);
	cycle_idx = ctd_get_cycle_index(ctd, batt_cc, &skip_derate);
	if (skip_derate) {
		chg_info("ctd resolve_pos: batt_cc=%d skip derate\n", batt_cc);
		return -ENOENT;
	}
	if (!ctd_tier_resolve_index(ctd, curve_ibus, &tier_idx)) {
		chg_info("ctd resolve_pos: curve_ibus=%d no tier match\n", curve_ibus);
		return -ENOENT;
	}

	ctd_apply_resolve_output(batt_cc, shell_temp, cycle_idx, temp_idx, tier_idx,
		batt_cc_out, shell_temp_out, cycle_idx_out, temp_idx_out, tier_idx_out);
	return 0;
}

static int ctd_resolve_pos(struct ctd_strategy *ctd, int curve_ibus,
	int *batt_cc_out, int *shell_temp_out, int *cycle_idx_out,
	u32 *temp_idx_out, u32 *tier_idx_out, u32 *pos_out)
{
	int batt_cc = 0;
	int shell_temp = 0;
	int cycle_idx = 0;
	u32 temp_idx = 0;
	u32 tier_idx = 0;
	u32 pos;
	int rc;

	if (!ctd || !pos_out || curve_ibus <= 0)
		return -EINVAL;

	rc = ctd_resolve_indices(ctd, curve_ibus, &batt_cc, &shell_temp,
		&cycle_idx, &temp_idx, &tier_idx);
	if (rc < 0)
		return rc;

	pos = ((u32)cycle_idx * ctd->temp_factor_cnt + temp_idx) * ctd->tier_cnt + tier_idx;
	if (pos >= ctd->tier_derated_ratio_cnt) {
		chg_info("ctd resolve_pos: pos=%u invalid ratio_cnt=%u\n",
			pos, ctd->tier_derated_ratio_cnt);
		return -EINVAL;
	}

	ctd_apply_resolve_output(batt_cc, shell_temp, cycle_idx, temp_idx, tier_idx,
		batt_cc_out, shell_temp_out, cycle_idx_out, temp_idx_out, tier_idx_out);
	*pos_out = pos;
	return 0;
}

static struct oplus_chg_strategy *ctd_strategy_alloc(unsigned char *buf, size_t size)
{
	return ERR_PTR(-ENOTSUPP);
}

static struct oplus_chg_strategy *ctd_strategy_alloc_by_node(struct device_node *node)
{
	struct ctd_strategy *ctd;
	struct device_node *common_node;
	int rc;

	common_node = of_find_compatible_node(NULL, NULL, "oplus,common-charge");
	if (!common_node)
		return ERR_PTR(-ENODEV);

	/*
	 * Ownership / lifetime:
	 *   - On success, this kzalloc()'d ctd is embedded in the returned
	 *     oplus_chg_strategy and handed to the framework. It is later
	 *     freed via ctd_strategy_release() -> ctd_strategy_free() ->
	 *     kfree(ctd).
	 *   - On the err: path below, ctd_strategy_free() runs locally so
	 *     the allocation never leaks even on partial init failure.
	 * Linters that count alloc/free pairs per-function will flag this as
	 * "more new than delete"; the deletion is in the release callback,
	 * not this function, by design.
	 */
	ctd = kzalloc(sizeof(*ctd), GFP_KERNEL);
	if (!ctd) {
		of_node_put(common_node);
		return ERR_PTR(-ENOMEM);
	}
	mutex_init(&ctd->lock);
	INIT_WORK(&ctd->track_work, ctd_track_workfn);
	ctd->node = node ? of_node_get(node) : NULL;

	rc = ctd_parse_cycle_thr(ctd, common_node);
	if (rc < 0)
		goto err;
	rc = ctd_parse_tier_factors(ctd, common_node);
	if (rc < 0)
		goto err;

	chg_info("cycle_thr_cnt=%u temp_factor_cnt=%u tier_cnt=%u temp_range_cnt=%u ratio_cnt=%u vbat_cap=%d\n",
		ctd->cycle_thr_cnt, ctd->temp_factor_cnt, ctd->tier_cnt, ctd->temp_range_cnt,
		ctd->tier_derated_ratio_cnt, !!ctd->tier_vbat_cap_mv);
	of_node_put(common_node);
	return &ctd->strategy;

err:
	of_node_put(common_node);
	ctd_strategy_free(ctd);
	return ERR_PTR(rc);
}

static int ctd_strategy_release(struct oplus_chg_strategy *strategy)
{
	struct ctd_strategy *ctd;

	if (!strategy)
		return -EINVAL;
	ctd = container_of(strategy, struct ctd_strategy, strategy);
	ctd_strategy_free(ctd);
	return 0;
}

static int ctd_strategy_init(struct oplus_chg_strategy *strategy)
{
	struct ctd_strategy *ctd;

	if (!strategy)
		return -EINVAL;
	ctd = container_of(strategy, struct ctd_strategy, strategy);
	mutex_lock(&ctd->lock);
	ctd->curve_ibus = 0;
	ctd->last_valid = false;
	ctd->last_vbat_delta_valid = false;
	/*
	 * Single canonical site for the MMS topic name lookups (see the
	 * file-scope comment on ctd_comm_topic/ctd_gauge_topic). Doing this
	 * once per session here -- under ctd->lock and in voocphy work
	 * context -- replaces the previous per-call lazy init inside
	 * ctd_resolve_pos(), so the consumer path stays read-only on these
	 * globals.
	 */
	if (!ctd_comm_topic)
		ctd_comm_topic = oplus_mms_get_by_name("common");
	if (!ctd_gauge_topic)
		ctd_gauge_topic = oplus_mms_get_by_name("gauge");
	ctd_capture_shell_temp(ctd);
	mutex_unlock(&ctd->lock);
	return 0;
}

static int ctd_strategy_set_process_data(struct oplus_chg_strategy *strategy,
	const char *type, unsigned long arg)
{
	struct ctd_strategy *ctd;

	if (!strategy || !type)
		return -EINVAL;
	ctd = container_of(strategy, struct ctd_strategy, strategy);
	/*
	 * strncmp(..., sizeof(literal)) bounds the compare window to the
	 * literal's length including its NUL, so a non-NUL-terminated `type`
	 * from a misbehaving caller cannot run past either buffer. Matches
	 * "curve_ibus" exactly.
	 */
	if (!strncmp(type, "curve_ibus", sizeof("curve_ibus"))) {
		mutex_lock(&ctd->lock);
		ctd->curve_ibus = (int)(long)arg;
		mutex_unlock(&ctd->lock);
		return 0;
	}
	return -EINVAL;
}

/*
 * CTD's strategy_get_data type contract
 * -------------------------------------
 * The generic ops table forces a void *ret signature. For this strategy,
 * `ret` MUST point to storage of EXACTLY sizeof(int) bytes, aligned for
 * `int` access. The value written back is the cycle/tier-derated ibus in
 * mA (or the un-derated curve_ibus when CTD is disabled / unresolved).
 *
 * Passing a narrower buffer (e.g. u16 *, char *) is a programmer error
 * and would corrupt the caller's stack on a successful path. To make this
 * less of a footgun, validate alignment here at runtime and refuse the
 * call. The compile-time BUILD_BUG_ON pins the write size invariant.
 */
static int ctd_query_derated_locked(struct ctd_strategy *ctd, int *result)
{
	int curve_ibus;
	int batt_cc = 0;
	int shell_temp = 0;
	int cycle_idx = 0;
	u32 temp_idx = 0;
	u32 tier_idx = 0;
	u32 pos = 0;
	int derated;
	int rc;

	curve_ibus = ctd->curve_ibus;
	if (curve_ibus <= 0) {
		*result = 0;
		return 0;
	}

	rc = ctd_resolve_pos(ctd, curve_ibus, &batt_cc, &shell_temp,
		&cycle_idx, &temp_idx, &tier_idx, &pos);
	if (rc < 0) {
		*result = curve_ibus;
		return 0;
	}
	if (ctd_last_cache_matches(ctd, curve_ibus, temp_idx, cycle_idx, tier_idx)) {
		*result = ctd->last_derated;
		return 0;
	}

	derated = ctd_compute_derated(ctd, curve_ibus, tier_idx, pos);
	chg_info("get_data: ibus=%d batt_cc=%d shell_temp=%dC cycle=%d temp_idx=%u tier=%u pos=%u ratio=%u derated=%d\n",
		curve_ibus, batt_cc, shell_temp / 10, cycle_idx, temp_idx, tier_idx, pos,
		ctd->tier_derated_ratio[pos], derated);

	ctd_track_try_trigger(ctd, curve_ibus, derated, (int)ctd->tier_derated_ratio[pos],
		ctd->tier_vbat_cap_mv ? (int)ctd->tier_vbat_cap_mv[pos] : 0);

	ctd->last_valid = true;
	ctd->last_curve_ibus = curve_ibus;
	ctd->last_temp_idx = temp_idx;
	ctd->last_cycle_idx = cycle_idx;
	ctd->last_tier_idx = tier_idx;
	ctd->last_derated = derated;
	*result = derated;
	return 0;
}

static int ctd_strategy_get_data(struct oplus_chg_strategy *strategy, void *ret)
{
	struct ctd_strategy *ctd;
	int *result;

	BUILD_BUG_ON(sizeof(*result) != sizeof(int));

	if (!strategy || !ret)
		return -EINVAL;
	if (unlikely(!IS_ALIGNED((uintptr_t)ret, __alignof__(int)))) {
		WARN_ONCE(1,
			  "ctd: get_data caller passed misaligned ret=%p, need %zu-byte align\n",
			  ret, (size_t)__alignof__(int));
		return -EINVAL;
	}
	result = ret;
	ctd = container_of(strategy, struct ctd_strategy, strategy);

	mutex_lock(&ctd->lock);
	ctd_query_derated_locked(ctd, result);
	mutex_unlock(&ctd->lock);
	return 0;
}

/*
 * CTD's strategy_get_custom_data type contract
 * --------------------------------------------
 * Supported type strings and their required `ret` buffer:
 *   "vbat_delta_mv" -> int *  (writes the per-tier CV voltage delta in mV)
 *
 * As with strategy_get_data above, `ret` must be sizeof(int) bytes wide and
 * aligned for `int`. Any new type added later MUST extend this comment and
 * validate its own buffer requirements before dereferencing.
 */
static void ctd_commit_vbat_delta(struct ctd_strategy *ctd, u32 temp_idx,
	int cycle_idx, u32 tier_idx, u32 pos, int delta)
{
	int derated_ibus;

	derated_ibus = (ctd->last_valid && ctd->last_curve_ibus == ctd->curve_ibus) ?
		ctd->last_derated : ctd->curve_ibus;
	ctd_track_try_trigger(ctd, ctd->curve_ibus, derated_ibus,
		(int)ctd->tier_derated_ratio[pos], delta);

	ctd->last_vbat_delta_valid = true;
	ctd->last_vbat_delta_curve_ibus = ctd->curve_ibus;
	ctd->last_vbat_delta_temp_idx = temp_idx;
	ctd->last_vbat_delta_cycle_idx = cycle_idx;
	ctd->last_vbat_delta_tier_idx = tier_idx;
	ctd->last_vbat_delta = delta;
}

static int ctd_query_vbat_delta_locked(struct ctd_strategy *ctd, int *delta_out)
{
	int batt_cc = 0;
	int shell_temp = 0;
	int cycle_idx = 0;
	u32 temp_idx = 0;
	u32 tier_idx = 0;
	u32 pos = 0;
	int rc;

	*delta_out = 0;
	if (!ctd->tier_vbat_cap_mv || ctd->curve_ibus <= 0)
		return 0;

	rc = ctd_resolve_pos(ctd, ctd->curve_ibus, &batt_cc, &shell_temp,
		&cycle_idx, &temp_idx, &tier_idx, &pos);
	if (rc < 0)
		return 0;
	if (ctd_last_vbat_cache_matches(ctd, ctd->curve_ibus, temp_idx, cycle_idx, tier_idx)) {
		*delta_out = ctd->last_vbat_delta;
		return 0;
	}
	*delta_out = (int)ctd->tier_vbat_cap_mv[pos];
	chg_info("vbat_delta: ibus=%d batt_cc=%d shell_temp=%dC cycle=%d temp_idx=%u tier=%u pos=%u delta=%d\n",
		ctd->curve_ibus, batt_cc, shell_temp / 10, cycle_idx, temp_idx, tier_idx, pos, *delta_out);
	ctd_commit_vbat_delta(ctd, temp_idx, cycle_idx, tier_idx, pos, *delta_out);
	return 0;
}

static int ctd_strategy_get_custom_data(struct oplus_chg_strategy *strategy,
	const char *type, void *ret)
{
	struct ctd_strategy *ctd;
	int *delta_out;

	BUILD_BUG_ON(sizeof(*delta_out) != sizeof(int));

	if (!strategy || !type || !ret)
		return -EINVAL;
	ctd = container_of(strategy, struct ctd_strategy, strategy);

	/* See ctd_strategy_set_process_data for why strncmp(..., sizeof(literal)). */
	if (strncmp(type, "vbat_delta_mv", sizeof("vbat_delta_mv")))
		return -ENOTSUPP;

	if (unlikely(!IS_ALIGNED((uintptr_t)ret, __alignof__(int)))) {
		WARN_ONCE(1,
			  "ctd: get_custom_data(%s) caller passed misaligned ret=%p, need %zu-byte align\n",
			  type, ret, (size_t)__alignof__(int));
		return -EINVAL;
	}
	delta_out = ret;

	mutex_lock(&ctd->lock);
	ctd_query_vbat_delta_locked(ctd, delta_out);
	mutex_unlock(&ctd->lock);
	return 0;
}

static struct oplus_chg_strategy_desc ctd_strategy_desc = {
	.name = "cycle_tier_derating",
	.strategy_alloc = ctd_strategy_alloc,
	.strategy_alloc_by_node = ctd_strategy_alloc_by_node,
	.strategy_release = ctd_strategy_release,
	.strategy_init = ctd_strategy_init,
	.strategy_get_data = ctd_strategy_get_data,
	.strategy_set_process_data = ctd_strategy_set_process_data,
	.strategy_get_custom_data = ctd_strategy_get_custom_data,
};

int ctd_strategy_register(void)
{
	return oplus_chg_strategy_register(&ctd_strategy_desc);
}
