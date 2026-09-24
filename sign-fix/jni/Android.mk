LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := signfix
LOCAL_SRC_FILES := signfix.c
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Wall
include $(BUILD_SHARED_LIBRARY)
