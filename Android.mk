QCOM_MEDIA_ROOT := $(call my-dir)
ifneq ($(filter msm8610 msm8226 msm8960 msm8974 msm8660 msm8974 msm7627a msm7630_surf,$(TARGET_BOARD_PLATFORM)),)
include $(QCOM_MEDIA_ROOT)/mm-core/Android.mk
include $(QCOM_MEDIA_ROOT)/libstagefrighthw/Android.mk
endif

ifneq ($(filter msm8610 msm8226 msm8974 msm8960 msm8660 msm7630_surf,$(TARGET_BOARD_PLATFORM)),)
include $(QCOM_MEDIA_ROOT)/mm-video/Android.mk
endif

ifneq ($(filter msm8960,$(TARGET_BOARD_PLATFORM)),)
include $(QCOM_MEDIA_ROOT)/libc2dcolorconvert/Android.mk
endif
