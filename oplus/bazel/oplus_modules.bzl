load(":repo_paths.bzl", "modules_label")
load("@rules_pkg//pkg:install.bzl", "pkg_install")
load("@rules_pkg//pkg:mappings.bzl", "pkg_files", "strip_prefix")
load(":oplus_modules_define.bzl", "oplus_ddk_get_oplus_features")

def define_oplus_ddk_modules(target, msm_target, variant):
    oplus_ddk_targets = [
        modules_label("oplus/kernel/audio/bazel:snd-soc-tfa98xx"),
        modules_label("oplus/hardware/radio/kernel:oplus_mdmfeature"),
        modules_label("oplus/hardware/radio/mdmrst/bazel:oplus_mdmrst"),
        modules_label("oplus/kernel/boot:buildvariant"),
        modules_label("oplus/kernel/boot:cdt_integrity"),
        modules_label("oplus/kernel/boot:oplus_bsp_boot_projectinfo"),
        modules_label("oplus/kernel/boot:oplus_bsp_bootloader_log"),
        modules_label("oplus/kernel/boot:oplus_bsp_bootmode"),
        modules_label("oplus/kernel/boot:oplus_charger_present"),
        modules_label("oplus/kernel/boot:oplus_ftm_mode"),
        modules_label("oplus/kernel/boot:oplusboot"),
        modules_label("oplus/kernel/boot:saupwk"),
        modules_label("oplus/kernel/camera:{}_camera_extension").format(target),
        modules_label("oplus/kernel/charger/bazel:{}_oplus_cfg").format(target),
        modules_label("oplus/kernel/charger/bazel:{}_oplus_chg_v2").format(target),
        modules_label("oplus/kernel/charger/bazel:{}_test-kit").format(target),
        modules_label("oplus/kernel/charger/bazel:{}_ufcs_class").format(target),
        modules_label("oplus/kernel/charger/bazel:{}_oplus_wireless_pen_mt5806").format(target),
        modules_label("oplus/kernel/cpu:horae_shell_temp"),
        modules_label("oplus/kernel/device_info/cs_press:oplus_bsp_cs_press_f71"),
        modules_label("oplus/kernel/device_info/device_info/bazel:device_info"),
        modules_label("oplus/kernel/device_info/magnetic_cover:oplus_magcvr_ak09973"),
        modules_label("oplus/kernel/device_info/magnetic_cover:oplus_magcvr_mkh100a"),
        modules_label("oplus/kernel/device_info/magnetic_cover:oplus_magcvr_mxm1120"),
        modules_label("oplus/kernel/device_info/magnetic_cover:oplus_magnetic_cover"),
        modules_label("oplus/kernel/device_info/magtransfer:oplus_magcvr_notify"),
        modules_label("oplus/kernel/dfr:oplus_bsp_dfr_keyevent_handler"),
        modules_label("oplus/kernel/dfr:oplus_bsp_dfr_pmic_monitor"),
        modules_label("oplus/kernel/dft/bazel:oplus_bsp_dft_kernel_fb"),
        modules_label("oplus/kernel/dft/bazel:oplus_bsp_dft_olc"),
        modules_label("oplus/kernel/multimedia/feedback/bazel:oplus_mm_kevent"),
        modules_label("oplus/kernel/multimedia/feedback/bazel:oplus_mm_kevent_fb"),
        modules_label("oplus/kernel/network:oplus_network_esim"),
        modules_label("oplus/kernel/network:oplus_network_oem_qmi"),
        modules_label("oplus/kernel/network:oplus_network_rf_cable_monitor"),
        modules_label("oplus/kernel/network:oplus_network_sim_detect"),
        modules_label("oplus/kernel/storage:ufs-oplus-dbg"),
        modules_label("oplus/kernel/touchpanel/kernelFwUpdate/bazel:oplus_bsp_fw_update"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_common"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_custom"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_focal_common"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ft3518"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ft3658u_spi"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ft3681"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ft3683g"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ft8057p"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_goodix_comnon"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_gt9916"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_gt9966"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ilitek7807s"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_ilitek_common"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_novatek_common"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_nt36528_noflash"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_nt36532_noflash"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_nt36536_noflash"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_nt36672c_noflash"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_syna_common"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_tcm_S3908"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_tcm_S3910"),
        modules_label("oplus/kernel/touchpanel/oplus_touchscreen_v2:oplus_bsp_tp_td4377_noflash"),
        modules_label("oplus/kernel/touchpanel/synaptics_hbp:oplus_bsp_synaptics_tcm2"),
        modules_label("oplus/kernel/touchpanel/touchpanel_notify/bazel:oplus_bsp_tp_notify"),
        modules_label("oplus/kernel/tp/hbp/hbp:oplus_bsp_tp_hbp_goodix_gt99x6"),
        modules_label("oplus/kernel/tp/hbp/hbp:oplus_bsp_tp_hbp_syna_s3910"),
        modules_label("oplus/kernel/tp/hbp/hbp:oplus_ft3683g"),
        modules_label("oplus/kernel/tp/hbp/hbp:oplus_hbp_core"),
        modules_label("oplus/kernel/vibrator/bazel:oplus_bsp_haptic"),
        modules_label("oplus/kernel/vibrator/bazel:oplus_bsp_haptic_feedback"),
        modules_label("oplus/secure/biometrics/fingerprints/bsp/uff/driver:oplus_bsp_uff_fp_driver"),
        modules_label("oplus/secure/common/bsp/drivers/oplus_secure_common:oplus_secure_common"),
        modules_label("oplus/sensor/kernel:oplus_trace_sensor_err"),
        modules_label("oplus/sensor/kernel/qcom:oplus_sensor_deviceinfo"),
        modules_label("oplus/sensor/kernel/qcom:oplus_sensor_feedback"),
        modules_label("oplus/sensor/kernel/qcom:oplus_sensor_interact"),
        modules_label("oplus/sensor/kernel/qcom:oplus_sensor_ir_core"),
        modules_label("oplus/sensor/kernel/qcom:oplus_sensor_kookong_ir_spi"),
        modules_label("oplus/kernel/nfc:oplus_nfc"),
        modules_label("oplus/kernel/nfc:oplus_network_nfc_thn31"),
        modules_label("oplus/kernel/tp/hbp/hbp:oplus_ft3685g"),
    ]

    #conditional_build modules
    oplus_feature_list = oplus_ddk_get_oplus_features()
    if str(oplus_feature_list.get("OPLUS_FEATURE_BSP_DRV_INJECT_TEST", 'foo')).upper() == "1":
        oplus_ddk_targets += [
            modules_label("oplus/sensor/kernel/qcom:pseudo_sensor"),
        ]

    pkg_files(
        name = "{}_all_oplus_ddk_modules_files".format(target),
        srcs = oplus_ddk_targets,
        strip_prefix = strip_prefix.files_only(),
        visibility = ["//visibility:private"],
    )

    pkg_install(
        name = "{}_all_oplus_ddk_modules_dist".format(target),
        srcs = [":{}_all_oplus_ddk_modules_files".format(target)],
        destdir = "out/msm-kernel-{}/techpack",
    )

    return oplus_ddk_targets
