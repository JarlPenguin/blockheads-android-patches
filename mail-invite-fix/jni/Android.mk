LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := mailinvitefix
LOCAL_SRC_FILES := mailinvitefix.c
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Wall
include $(BUILD_SHARED_LIBRARY)
