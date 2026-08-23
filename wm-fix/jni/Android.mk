LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := wmfix
LOCAL_SRC_FILES := wmfix.c
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Wall -O2
include $(BUILD_SHARED_LIBRARY)
