load(":oplus_modules_define.bzl", "define_oplus_ddk_module")
load(":oplus_modules_dist.bzl", "ddk_copy_to_dist_dir")

def define_oplus_local_modules():

    define_oplus_ddk_module(
        name = "oplus_bsp_dfr_keyevent_handler",
        srcs = native.glob([
            "**/*.h",
            "common/keyevent_handler/keyevent_handler.c",
        ]),
        includes = ["."],
    )

    define_oplus_ddk_module(
        name = "oplus_bsp_dfr_pmic_monitor",
        srcs = native.glob([
            "**/*.h",
        ]),
        conditional_srcs = {
            "CONFIG_OPLUS_DDK_MTK": {
                False: ["qcom/qcom_pmic_monitor/oplus_pmic_info_smem.c",
                "qcom/qcom_pmic_monitor/main.c",
                "qcom/qcom_pmic_monitor/oplus_pmic_machine_state.c",
                "qcom/qcom_pmic_monitor/oplus_ocp_dev.c",
                "qcom/qcom_pmic_monitor/oplus_ocp_state_nvmem.c"],
            }
        },
        includes = ["."],
    )

    ddk_copy_to_dist_dir(
        name = "oplus_bsp_dfr",
        module_list = [
            "oplus_bsp_dfr_keyevent_handler",
            "oplus_bsp_dfr_pmic_monitor",
        ],
    )
