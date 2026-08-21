LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := privacypopupfix
LOCAL_SRC_FILES := privacypopupfix.c
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
