/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024-2026 Oplus. All rights reserved.
 *
 * Public API of the AI CV service module. The service owns the
 * AIDL-supplied AI CV curve table and the full-charge CV cutoff delta.
 * Userspace writes are routed through oplus_configfs.c, which exposes
 * /sys/class/oplus_chg/battery/{ai_cv_curve, ai_cv_fc_thresh} and forwards
 * via oplus_ai_cv_curve_parse_set / oplus_ai_cv_set_fc_thresh_mv.
 *
 * Charging consumers (PUC v1/v2, voocphy, oplus_chg_comm) query derating
 * decisions through oplus_ai_cv_query_*; they no longer go through the
 * oplus_chg_strategy framework.
 */

#ifndef _OPLUS_CHG_AI_CV_H_
#define _OPLUS_CHG_AI_CV_H_

#include <linux/types.h>

/*
 * Returns the derated ibus (>= 0) for the given target_ibus, in the same
 * unit as the caller passed in (mA or 100*mA - the curve is unit-agnostic
 * because it stores ratios). Returns target_ibus when no curve is loaded
 * or no point matches.
 */
int oplus_ai_cv_query_derate_ibus(int target_ibus);

/*
 * Returns the vbat threshold reduction (>= 0 mV) requested by the curve
 * for the given target_ibus, or 0 when no curve is loaded / no match.
 */
int oplus_ai_cv_query_vbat_delta_mv(int target_ibus);

/* Returns the full-charge CV cutoff override (>= 0 mV), 0 when unset. */
int oplus_ai_cv_get_fc_thresh_mv(void);

/* Drops the cached curve. Used by comm on session-exit / low-temp transitions. */
void oplus_ai_cv_clear_curve(void);

/* configfs sysfs glue helpers. Buf semantics match struct device_attribute. */
ssize_t oplus_ai_cv_curve_format(char *buf, size_t size);
int     oplus_ai_cv_curve_parse_set(const char *buf, size_t count);
int     oplus_ai_cv_set_fc_thresh_mv(int mv);

#endif /* _OPLUS_CHG_AI_CV_H_ */
