# SPDX-License-Identifier: GPL-2.0-or-later
load(":repo_paths.bzl", "soc_label")
load("//build/kernel/kleaf:kernel.bzl", "ddk_headers")
load(":oplus_modules_define.bzl", "define_oplus_ddk_module", "oplus_ddk_get_target", "oplus_ddk_get_variant")

def define_oplus_local_modules():
    variant = "{}_{}".format(oplus_ddk_get_target(), oplus_ddk_get_variant())
    define_oplus_ddk_module(
        name = "wonder",
        srcs = native.glob(["**/*.h"]) + [
            "band_config.c", "mac80211.c", "mac80211_txs.c", "main.c",
            "nl80211_ven_cmd.c", "ssr.c", "wondertap.c",
        ],
        conditional_srcs = {"CONFIG_DEBUG_FS": {True: ["debugfs.c"]}},
        includes = ["include"],
        ko_deps = [
            soc_label("{}/net/wireless/cfg80211").format(variant),
            soc_label("{}/net/mac80211/mac80211").format(variant),
        ],
    )
    ddk_headers(
        name = "wonder_headers",
        hdrs = native.glob(["include/**/*.h"]),
        includes = ["include"],
    )
