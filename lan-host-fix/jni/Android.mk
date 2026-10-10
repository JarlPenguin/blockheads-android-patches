LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := lanhostfix
LOCAL_SRC_FILES := lanhostfix.c
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Wall
include $(BUILD_SHARED_LIBRARY)
