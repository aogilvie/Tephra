LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := js_shared_prebuilt
LOCAL_MODULE_FILENAME := libJavaScriptCore
LOCAL_SRC_FILES := libs/$(TARGET_ARCH_ABI)/libJavaScriptCore.so
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include 
                           
include $(PREBUILT_SHARED_LIBRARY)
