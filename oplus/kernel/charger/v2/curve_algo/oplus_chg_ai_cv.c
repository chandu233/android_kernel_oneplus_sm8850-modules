
// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2026 Oplus. All rights reserved.
 *
 * AI CV service module.
 *
 * Owns the AIDL-supplied AI CV curve table and the full-charge CV cutoff
 * delta, and exposes a small set of plain C APIs (see oplus_chg_ai_cv.h)
 * so charging consumers (PUC v1/v2, voocphy, oplus_chg_comm) can query
 * derating decisions without going through the oplus_chg_strategy
 * framework. Userspace writes are routed through oplus_configfs.c, which
 * provides the /sys/class/oplus_chg/battery/{ai_cv_curve, ai_cv_fc_thresh}
 * device_attributes and forwards via oplus_ai_cv_curve_format /
 * oplus_ai_cv_curve_parse_set / oplus_ai_cv_set_fc_thresh_mv.
 */

#define pr_fmt(fmt) "[AI_CV]([%s][%d]): " fmt, __func__, __LINE__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/err.h>

#include <oplus_chg.h>
#include <oplus_chg_comm.h>
#include <oplus_chg_cpa.h>
#include <oplus_mms.h>
#include <oplus_chg_ai_cv.h>

#define AI_CV_CURVE_MAX_POINTS		10
#define AI_CV_CURVE_VBUS_MV		11000
#define AI_CV_CURVE_IBAT_UNBOUNDED	(-1)
/*
 * Hard upper bounds for userspace-supplied values. These are intentionally
 * tighter than the protocol theoretical range so that a compromised or
 * misbehaving userspace writer cannot push the charger into a regime that
 * confuses downstream derating logic.
 *
 * AI_CV_THRESH_MV_MAX caps the per-point CV cutoff/derating delta.
 *
 * AI_CV_PARSE_BUF_MAX caps the input string length. Realistic well-formed
 * payload from HAL (KernelNodeExecutor::applyCvDerate) is "N <p0> <p1> ...\n"
 * where each point is 4 ints serialized via std::to_string. For N=10 and
 * values within range that is ~173 bytes, but a buggy HAL serializing
 * unexpectedly large ints (e.g. INT_MIN = 11 chars) can balloon a 10-point
 * payload to ~480 bytes. 512 keeps headroom for that case so the downstream
 * range / semantic errors surface instead of a generic "too large" reject,
 * while still being 1/8 of PAGE_SIZE.
 */
#define AI_CV_THRESH_MV_MAX		200
#define AI_CV_PARSE_BUF_MAX		512

/*
 * Four ints, no padding -- already optimally packed. Fields are signed int
 * so the UNBOUNDED sentinel (-1) carries through arithmetic without
 * sign-extension surprises. The struct is internal and never memcpy'd from
 * an on-wire byte stream: oplus_ai_cv_curve_format / _parse_set drive the
 * (un)serialization by named-field access, so field order here is purely a
 * readability/convention choice and can be changed safely.
 */
struct ai_cv_curve_point {
	int ibat_min_ma;
	int ibat_max_ma;
	int ratio;
	int thresh_mv;
};

/*
 * File-scope anonymous container. Pointers grouped first to keep them at
 * natural 8-byte alignment; the two int counters then pack into a single
 * 8-byte slot at the tail with no padding. Total: sizeof(curve) + 16 + 8.
 */
static struct {
	struct oplus_mms *cpa_topic;
	struct oplus_mms *comm_topic;
	struct ai_cv_curve_point curve[AI_CV_CURVE_MAX_POINTS];
	int curve_num;
	int fc_thresh_mv;
} ai_cv_data;

static DEFINE_MUTEX(ai_cv_data_lock);

static struct oplus_mms *ai_cv_get_cpa_topic(void)
{
	struct oplus_mms *t = READ_ONCE(ai_cv_data.cpa_topic);

	if (!t) {
		t = oplus_mms_get_by_name("cpa");
		if (t)
			WRITE_ONCE(ai_cv_data.cpa_topic, t);
	}
	return t;
}

static struct oplus_mms *ai_cv_get_comm_topic(void)
{
	struct oplus_mms *t = READ_ONCE(ai_cv_data.comm_topic);

	if (!t) {
		t = oplus_mms_get_by_name("common");
		if (t)
			WRITE_ONCE(ai_cv_data.comm_topic, t);
	}
	return t;
}

static int ai_cv_pct_to_ibat_ma(int pct)
{
	struct oplus_mms *cpa_topic;
	int project_power_mw;
	long long ibat_ma;

	if (pct == AI_CV_CURVE_IBAT_UNBOUNDED)
		return AI_CV_CURVE_IBAT_UNBOUNDED;
	if (pct < 0 || pct > 100)
		return -EINVAL;
	if (pct == 0)
		return 0;

	cpa_topic = ai_cv_get_cpa_topic();
	if (!cpa_topic) {
		chg_err("cpa not ready, can't resolve ai_cv_curve ibat pct=%d\n", pct);
		return -EAGAIN;
	}
	project_power_mw = oplus_cpa_protocol_get_max_power(cpa_topic);
	if (project_power_mw <= 0) {
		chg_err("invalid project power %d mW from cpa\n", project_power_mw);
		return -EAGAIN;
	}

	ibat_ma = (long long)project_power_mw * pct / 100
		  / (AI_CV_CURVE_VBUS_MV / 1000);
	return (int)ibat_ma;
}

static bool ai_cv_point_matches(const struct ai_cv_curve_point *p, int target_ibus)
{
	if (p->ibat_min_ma >= 0 && target_ibus < p->ibat_min_ma)
		return false;
	if (p->ibat_max_ma >= 0 && target_ibus > p->ibat_max_ma)
		return false;
	return true;
}

static void ai_cv_set_lookup_defaults(int *ratio_out, int *thresh_mv_out)
{
	if (ratio_out)
		*ratio_out = 100;
	if (thresh_mv_out)
		*thresh_mv_out = 0;
}

static void ai_cv_emit_point(const struct ai_cv_curve_point *p,
	int *ratio_out, int *thresh_mv_out)
{
	if (ratio_out)
		*ratio_out = p->ratio;
	if (thresh_mv_out)
		*thresh_mv_out = p->thresh_mv;
}

static int ai_cv_lookup(int target_ibus, int *ratio_out, int *thresh_mv_out)
{
	int i;
	int rc = -ENOENT;

	ai_cv_set_lookup_defaults(ratio_out, thresh_mv_out);
	if (target_ibus <= 0)
		return -EINVAL;

	mutex_lock(&ai_cv_data_lock);
	if (ai_cv_data.curve_num <= 0)
		goto out;

	for (i = 0; i < ai_cv_data.curve_num; i++) {
		const struct ai_cv_curve_point *p = &ai_cv_data.curve[i];

		if (!ai_cv_point_matches(p, target_ibus))
			continue;
		ai_cv_emit_point(p, ratio_out, thresh_mv_out);
		rc = 0;
		break;
	}
out:
	mutex_unlock(&ai_cv_data_lock);
	return rc;
}

int oplus_ai_cv_query_derate_ibus(int target_ibus)
{
	int ratio = 100;

	if (target_ibus <= 0)
		return target_ibus;
	ai_cv_lookup(target_ibus, &ratio, NULL);
	if (ratio > 0 && ratio < 100)
		return target_ibus * ratio / 100;
	return target_ibus;
}

int oplus_ai_cv_query_vbat_delta_mv(int target_ibus)
{
	int thresh_mv = 0;

	if (target_ibus <= 0)
		return 0;
	ai_cv_lookup(target_ibus, NULL, &thresh_mv);
	return thresh_mv > 0 ? thresh_mv : 0;
}

int oplus_ai_cv_get_fc_thresh_mv(void)
{
	int val;

	mutex_lock(&ai_cv_data_lock);
	val = ai_cv_data.fc_thresh_mv;
	mutex_unlock(&ai_cv_data_lock);
	return val;
}

void oplus_ai_cv_clear_curve(void)
{
	mutex_lock(&ai_cv_data_lock);
	memset(ai_cv_data.curve, 0, sizeof(ai_cv_data.curve));
	ai_cv_data.curve_num = 0;
	mutex_unlock(&ai_cv_data_lock);
	chg_info("ai_cv curve cleared on session exit\n");
}

ssize_t oplus_ai_cv_curve_format(char *buf, size_t size)
{
	int i;
	ssize_t len = 0;

	if (!buf || size == 0)
		return 0;

	mutex_lock(&ai_cv_data_lock);
	len += scnprintf(buf + len, size - len, "%d", ai_cv_data.curve_num);
	for (i = 0; i < ai_cv_data.curve_num; i++) {
		len += scnprintf(buf + len, size - len, "  %d %d %d %d",
				 ai_cv_data.curve[i].ibat_min_ma,
				 ai_cv_data.curve[i].ibat_max_ma,
				 ai_cv_data.curve[i].ratio,
				 ai_cv_data.curve[i].thresh_mv);
	}
	len += scnprintf(buf + len, size - len, "\n");
	mutex_unlock(&ai_cv_data_lock);

	return len;
}

static int ai_cv_parse_header(const char *buf, size_t count,
	int *n_out, int *offset_out)
{
	int n;
	int offset;

	if (sscanf(buf, "%d%n", &n, &offset) != 1 ||
	    offset <= 0 || (size_t)offset > count) {
		chg_err("failed to parse point count\n");
		return -EINVAL;
	}
	if (n <= 0 || n > AI_CV_CURVE_MAX_POINTS) {
		chg_err("invalid point count %d (max %d)\n",
			n, AI_CV_CURVE_MAX_POINTS);
		return -EINVAL;
	}
	*n_out = n;
	*offset_out = offset;
	return 0;
}

static int ai_cv_resolve_ibat_pair(int idx, int min_pct, int max_pct,
	struct ai_cv_curve_point *out)
{
	int rc;

	rc = ai_cv_pct_to_ibat_ma(min_pct);
	if (rc < 0 && rc != AI_CV_CURVE_IBAT_UNBOUNDED) {
		chg_err("point[%d] ibat_min pct=%d convert failed, rc=%d\n",
			idx, min_pct, rc);
		return rc;
	}
	out->ibat_min_ma = rc;

	rc = ai_cv_pct_to_ibat_ma(max_pct);
	if (rc < 0 && rc != AI_CV_CURVE_IBAT_UNBOUNDED) {
		chg_err("point[%d] ibat_max pct=%d convert failed, rc=%d\n",
			idx, max_pct, rc);
		return rc;
	}
	out->ibat_max_ma = rc;

	/* Semantic check: min must not exceed max when both are bounded. */
	if (out->ibat_min_ma != AI_CV_CURVE_IBAT_UNBOUNDED &&
	    out->ibat_max_ma != AI_CV_CURVE_IBAT_UNBOUNDED &&
	    out->ibat_min_ma > out->ibat_max_ma) {
		chg_err("point[%d] ibat range invalid: min=%d > max=%d\n",
			idx, out->ibat_min_ma, out->ibat_max_ma);
		return -EINVAL;
	}
	return 0;
}

static int ai_cv_parse_one_point(const char *buf, size_t count,
	int *total_offset, int idx, struct ai_cv_curve_point *out)
{
	int ibat_min_pct, ibat_max_pct, ratio, thresh;
	int offset;
	int rc;

	if ((size_t)*total_offset >= count) {
		chg_err("buf truncated before point[%d]\n", idx);
		return -EINVAL;
	}
	if (sscanf(buf + *total_offset, " %d %d %d %d%n",
		   &ibat_min_pct, &ibat_max_pct, &ratio, &thresh, &offset) != 4 ||
	    offset <= 0 ||
	    (size_t)*total_offset + (size_t)offset > count) {
		chg_err("failed to parse point[%d]\n", idx);
		return -EINVAL;
	}
	*total_offset += offset;

	if (ratio < 0 || ratio > 100) {
		chg_err("point[%d] ratio %d out of range [0, 100]\n", idx, ratio);
		return -EINVAL;
	}
	if (thresh < 0 || thresh > AI_CV_THRESH_MV_MAX) {
		chg_err("point[%d] thresh %d out of range [0, %d]\n",
			idx, thresh, AI_CV_THRESH_MV_MAX);
		return -EINVAL;
	}

	rc = ai_cv_resolve_ibat_pair(idx, ibat_min_pct, ibat_max_pct, out);
	if (rc < 0)
		return rc;

	out->ratio = ratio;
	out->thresh_mv = thresh;
	return 0;
}

int oplus_ai_cv_curve_parse_set(const char *buf, size_t count)
{
	struct ai_cv_curve_point pts[AI_CV_CURVE_MAX_POINTS];
	int n = 0;
	int i;
	int offset = 0;
	int total_offset;
	int rc;

	if (!buf) {
		chg_err("buf is NULL\n");
		return -EINVAL;
	}
	/*
	 * Defensive bounds on count: refuse zero-length and absurdly large
	 * inputs so sscanf never scans past the caller's intended buffer.
	 * Callers are still expected to provide a NUL-terminated buf within
	 * [0, count]; this just prevents a misuse from turning into an OOB
	 * read by limiting the scan window we are willing to honor.
	 */
	if (count == 0 || count > AI_CV_PARSE_BUF_MAX) {
		chg_err("invalid count: %zu (max %d)\n",
			count, AI_CV_PARSE_BUF_MAX);
		return -EINVAL;
	}

	rc = ai_cv_parse_header(buf, count, &n, &offset);
	if (rc < 0)
		return rc;
	total_offset = offset;

	for (i = 0; i < n; i++) {
		rc = ai_cv_parse_one_point(buf, count, &total_offset, i, &pts[i]);
		if (rc < 0)
			return rc;
	}

	/*
	 * Bounds for memcpy:
	 *   - BUILD_BUG_ON nails the destination stride at compile time so any
	 *     future change to either array width is caught instantly.
	 *   - n is already capped by ai_cv_parse_header() to [1, AI_CV_CURVE_MAX_POINTS]
	 *     and pts[] is sized exactly to that max; the runtime guard below
	 *     is a defense-in-depth assertion so the memcpy length is provably
	 *     bounded for any reachable n.
	 */
	BUILD_BUG_ON(sizeof(pts) != sizeof(ai_cv_data.curve));
	if (WARN_ON((unsigned int)n > ARRAY_SIZE(ai_cv_data.curve)))
		return -EINVAL;

	mutex_lock(&ai_cv_data_lock);
	memcpy(ai_cv_data.curve, pts, n * sizeof(pts[0]));
	ai_cv_data.curve_num = n;
	mutex_unlock(&ai_cv_data_lock);

	chg_info("ai_cv_curve updated: %d points (vbus=%dmV)\n",
		 n, AI_CV_CURVE_VBUS_MV);
	return (int)count;
}

int oplus_ai_cv_set_fc_thresh_mv(int mv)
{
	struct oplus_mms *comm_topic;
	bool changed;

	if (mv < 0 || mv > AI_CV_THRESH_MV_MAX) {
		chg_err("thresh_mv %d out of range [0, %d]\n",
			mv, AI_CV_THRESH_MV_MAX);
		return -EINVAL;
	}

	mutex_lock(&ai_cv_data_lock);
	changed = (mv != ai_cv_data.fc_thresh_mv);
	if (changed)
		ai_cv_data.fc_thresh_mv = mv;
	mutex_unlock(&ai_cv_data_lock);

	if (!changed) {
		chg_info("ai_cv_fc_thresh unchanged: %d mV\n", mv);
		return 0;
	}

	comm_topic = ai_cv_get_comm_topic();
	if (comm_topic)
		oplus_comm_notify_ai_cv_changed(comm_topic);
	chg_info("ai_cv_fc_thresh updated: %d mV\n", mv);
	return 0;
}
