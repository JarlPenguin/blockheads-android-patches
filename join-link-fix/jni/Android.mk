LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := joinlinkfix
LOCAL_SRC_FILES := joinlinkfix.c
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
