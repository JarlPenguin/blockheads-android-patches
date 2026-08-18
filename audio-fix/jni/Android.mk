LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := audiofix
LOCAL_SRC_FILES := audiofix.c
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
