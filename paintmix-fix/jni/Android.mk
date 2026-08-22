LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := paintmixfix
LOCAL_SRC_FILES := paintmixfix.c
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
