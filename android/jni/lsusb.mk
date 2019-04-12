LOCAL_PATH:= $(call my-dir)
LIBUSB_ROOT_REL:= ../..
LIBUSB_ROOT_ABS:= $(LOCAL_PATH)/../..


include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
  $(LIBUSB_ROOT_REL)/usbutils/lsusb.c \
  $(LIBUSB_ROOT_REL)/usbutils/lsusb-t.c \
  $(LIBUSB_ROOT_REL)/usbutils/names.c \
  $(LIBUSB_ROOT_REL)/usbutils/usbmisc.c \
  $(LIBUSB_ROOT_REL)/usbutils/desc-dump.c \
  $(LIBUSB_ROOT_REL)/usbutils/desc-defs.c 

LOCAL_C_INCLUDES += \
  $(LIBUSB_ROOT_ABS)/libusb \
  $(LIBUSB_ROOT_REL)/usbutils

LOCAL_SHARED_LIBRARIES += libusb1.0 libudev

LOCAL_MODULE:= lsusb
include $(BUILD_EXECUTABLE)
