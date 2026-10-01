load(":repo_paths.bzl", "modules_label")

load(":oplus_modules_define.bzl", "define_oplus_ddk_module", "oplus_ddk_get_kernel_version", "bazel_support_platform")
load(":oplus_modules_dist.bzl", "ddk_copy_to_dist_dir")
def define_oplus_storage_modules():
    kernel_version = oplus_ddk_get_kernel_version()

    # add for ufs_oplus_dbg
    if bazel_support_platform == "qcom":
        copts = [
                    "-DCONFIG_OPLUS_QCOM_UFS_DRIVER",
                    "-I$(srctree)/drivers/ufs/host/",
                ]
        ko_deps = [modules_label("oplus/kernel/device_info/device_info/bazel:device_info")]
        hdrs = [
            "storage_feature_in_module/common/ufs_oplus_dbg/ufs-oplus-dbg.h",
            "storage_feature_in_module/common/ufs_oplus_dbg/ufs-qcom.h",
            "storage_feature_in_module/common/ufs_oplus_dbg/ufshcd-priv.h",
        ]
        header_deps = []
    else:
        copts = [
                    "-I$(srctree)/drivers/ufs/core/",
                    "-I$(DEVICE_MODULES_PATH)/drivers/ufs/",
                ]
        hdrs = [
            "storage_feature_in_module/common/ufs_oplus_dbg/ufs-oplus-dbg.h",
        ]
        ko_deps = [
                    "//kernel_device_modules-{}/drivers/soc/oplus/device_info:device_info".format(kernel_version),
        ]
        header_deps = ["//kernel_device_modules-{}/drivers/ufs:headers".format(kernel_version),]

    define_oplus_ddk_module(
        name = "ufs-oplus-dbg",
        srcs = native.glob([
            "storage_feature_in_module/common/ufs_oplus_dbg/*.c",
        ]),
        hdrs = hdrs,
        includes = ["."],
        copts = copts,
        ko_deps = ko_deps,
        conditional_defines = {
            "qcom": ["CONFIG_OPLUS_QCOM_UFS_DRIVER"],
        },
        header_deps = header_deps,
        out = "ufs-oplus-dbg.ko",
    )

    ddk_copy_to_dist_dir(
        name = "oplus_storage",
        module_list = [
            "ufs-oplus-dbg",
        ],
    )
