LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := dpifix
LOCAL_SRC_FILES := dpifix.c
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
