# nvidia-prolapse prebuilt vendor modules. Make, not Soong: libdrm.so.2 needs an installed name that is not
# <stem>.so, and the ANGLE/gralloc replacements override modules by name (LOCAL_OVERRIDES_MODULES).
# The ELFs are release builds from pinned sources (CI); their NEEDED closure is LL-NDK + VNDK-SP + each other, so
# Make's ELF dependency check (which only knows declared modules) is off for them.
LOCAL_PATH := $(call my-dir)
NVP_NATIVE := prebuilts/native/payload
NVP_ANGLE := prebuilts/angle

ifeq ($(wildcard $(LOCAL_PATH)/$(NVP_NATIVE)/vendor_extra/lib64/libnv_bionic_native.so),)
$(error vendor/nvidia-prolapse: no native bundle under $(LOCAL_PATH)/$(NVP_NATIVE); run vendor-image/build-vendor.sh product)
endif
ifeq ($(wildcard $(LOCAL_PATH)/$(NVP_ANGLE)/vendor/lib64/egl/libGLESv2_angle.so),)
$(error vendor/nvidia-prolapse: no ANGLE build under $(LOCAL_PATH)/$(NVP_ANGLE); run vendor-image/build-vendor.sh product)
endif

# $(1) module name, $(2) installed file name, $(3) relative path under /vendor/<lib>, $(4) source path under
# prebuilts with %LIB% for lib/lib64, $(5) modules it replaces
define nvp-prebuilt-lib
include $$(CLEAR_VARS)
LOCAL_MODULE := $(1)
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_VENDOR_MODULE := true
LOCAL_MULTILIB := both
LOCAL_MODULE_RELATIVE_PATH := $(3)
LOCAL_SRC_FILES_64 := $(subst %LIB%,lib64,$(4))
LOCAL_SRC_FILES_32 := $(subst %LIB%,lib,$(4))
LOCAL_INSTALLED_MODULE_STEM := $(2)
LOCAL_CHECK_ELF_FILES := false
LOCAL_STRIP_MODULE := false
LOCAL_OVERRIDES_MODULES := $(5)
LOCAL_LICENSE_KINDS := legacy_notice
LOCAL_LICENSE_CONDITIONS := notice
include $$(BUILD_PREBUILT)
endef

$(eval $(call nvp-prebuilt-lib,nvprolapse_vulkan.nvidia,vulkan.nvidia.so,hw,$(NVP_NATIVE)/vendor_extra/%LIB%/hw/vulkan.nvidia.so,))
$(eval $(call nvp-prebuilt-lib,nvprolapse_libvulkan_nouveau,libvulkan_nouveau.so,,$(NVP_NATIVE)/vendor_extra/%LIB%/libvulkan_nouveau.so,))
$(eval $(call nvp-prebuilt-lib,nvprolapse_libnv_bionic_native,libnv_bionic_native.so,,$(NVP_NATIVE)/vendor_extra/%LIB%/libnv_bionic_native.so,))
$(eval $(call nvp-prebuilt-lib,nvprolapse_libdrm.so.2,libdrm.so.2,,$(NVP_NATIVE)/vendor_extra/%LIB%/libdrm.so.2,))
$(eval $(call nvp-prebuilt-lib,nvprolapse_libgallium_dri_nvrm,libgallium_dri_nvrm.so,,$(NVP_NATIVE)/vendor_extra/%LIB%/libgallium_dri_nvrm.so,))
$(foreach l,EGL GLESv1_CM GLESv2,\
  $(eval $(call nvp-prebuilt-lib,nvprolapse_lib$(l)_nvrm,lib$(l)_nvrm.so,egl,$(NVP_NATIVE)/vendor/%LIB%/egl/lib$(l)_nvrm.so,))\
  $(eval $(call nvp-prebuilt-lib,nvprolapse_lib$(l)_angle,lib$(l)_angle.so,egl,$(NVP_ANGLE)/vendor/%LIB%/egl/lib$(l)_angle.so,lib$(l)_angle)))
$(eval $(call nvp-prebuilt-lib,nvprolapse_libgbm_mesa_wrapper,libgbm_mesa_wrapper.so,,$(NVP_NATIVE)/vendor_extra/%LIB%/libgbm_mesa_wrapper.so,libgbm_mesa_wrapper))

# Codec2 NVDEC service (binary from the bundle; rc/VINTF/seccomp from the bundle's driver/media/odm/etc, copied to
# prebuilts/media by the product stage). Its HIDL/Codec2 runtime libraries are the tree's vendor variants (installed to
# /vendor/lib64 via LOCAL_REQUIRED_MODULES), not the bundle's standalone builds.
include $(CLEAR_VARS)
LOCAL_MODULE := android.hardware.media.c2@1.2-service-nvprolapse
LOCAL_MODULE_CLASS := EXECUTABLES
LOCAL_ODM_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MULTILIB := 64
LOCAL_SRC_FILES := $(NVP_NATIVE)/odm/bin/hw/android.hardware.media.c2@1.2-service-nvprolapse
LOCAL_INIT_RC := prebuilts/media/odm/etc/init/android.hardware.media.c2@1.2-service-nvprolapse.rc
LOCAL_VINTF_FRAGMENTS := prebuilts/media/odm/etc/vintf/manifest/android.hardware.media.c2@1.2-nvprolapse.xml
LOCAL_CHECK_ELF_FILES := false
LOCAL_STRIP_MODULE := false
LOCAL_REQUIRED_MODULES := \
    android.hardware.media.c2@1.0.vendor \
    android.hardware.media.c2@1.1.vendor \
    android.hardware.media.c2@1.2.vendor \
    libcodec2_hidl@1.0.vendor \
    libcodec2_hidl@1.1.vendor \
    libcodec2_hidl@1.2.vendor \
    libcodec2_hidl_plugin \
    libcodec2_vndk.vendor \
    libstagefright_bufferpool@2.0.1.vendor
LOCAL_LICENSE_KINDS := legacy_notice
LOCAL_LICENSE_CONDITIONS := notice
include $(BUILD_PREBUILT)
