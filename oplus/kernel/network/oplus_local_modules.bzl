load(":repo_paths.bzl", "modules_label", "soc_label")
load("//build/kernel/kleaf:kernel.bzl", "ddk_headers")
load(":oplus_modules_define.bzl", "define_oplus_ddk_module", "oplus_ddk_get_target", "oplus_ddk_get_variant")
load(":oplus_modules_dist.bzl", "ddk_copy_to_dist_dir")

def define_oplus_local_modules():
    target = oplus_ddk_get_target()
    variant  = oplus_ddk_get_variant()

    kernel_build_variant = "{}_{}".format(target, variant)

    if target == "canoe" :
        ko_oem_qmi_deps = [
            soc_label("{}/drivers/soc/qcom/qmi_helpers").format(kernel_build_variant)
        ]
    else :
        ko_oem_qmi_deps = []

    define_oplus_ddk_module(
        name = "oplus_network_rf_cable_monitor",
        srcs = native.glob([
            "**/*.h",
            "oplus_rf_cable_monitor/oplus_rf_cable_monitor.c",
        ]),
        conditional_defines = {
            "qcom":  ["QCOM_PLATFORM"],
        },
        includes = ["."],
    )

    define_oplus_ddk_module(
        name = "oplus_network_oem_qmi",
        srcs = native.glob([
            "**/*.h",
            "oplus_network_oem_qmi/oem_qmi_client.c",
        ]),
        ko_deps = ko_oem_qmi_deps,
        conditional_defines = {
            "qcom":  ["QCOM_PLATFORM"],
        },
        includes = ["."],
    )

    define_oplus_ddk_module(
        name = "oplus_network_esim",
        srcs = native.glob([
            "**/*.h",
            "oplus_network_esim/oplus_network_esim.c",
        ]),
        ko_deps = [
            modules_label("oplus/kernel/network:oplus_network_oem_qmi"),
        ],
        conditional_defines = {
            "qcom":  ["QCOM_PLATFORM"],
        },
        includes = ["."],
    )

    define_oplus_ddk_module(
        name = "oplus_network_sim_detect",
        srcs = native.glob([
            "**/*.h",
            "oplus_network_sim_detect/sim_detect.c",
        ]),
        ko_deps = [
            modules_label("oplus/kernel/network:oplus_network_oem_qmi"),
        ],
        conditional_defines = {
            "qcom":  ["QCOM_PLATFORM"],
        },
        includes = ["."],
    )

    ddk_headers(
        name = "config_headers",
        hdrs  = native.glob([
            "**/*.h",
        ]),
        includes = ["."],
    )

    ddk_copy_to_dist_dir(
        name = "oplus_network",
        module_list = [
            "oplus_network_rf_cable_monitor",
            "oplus_network_oem_qmi",
            "oplus_network_esim",
            "oplus_network_sim_detect",
        ],
    )
