LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := sfxexitfix
LOCAL_SRC_FILES := sfxexitfix.c
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Wall
include $(BUILD_SHARED_LIBRARY)
