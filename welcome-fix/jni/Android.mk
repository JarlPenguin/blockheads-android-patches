LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := welcomefix
LOCAL_SRC_FILES := welcomefix.c
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Wall -O2
include $(BUILD_SHARED_LIBRARY)
