LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := iovy.c

LOCAL_MODULE    := libiovy_exploit
LOCAL_MODULE_TAGS := optional

include $(BUILD_STATIC_LIBRARY)
