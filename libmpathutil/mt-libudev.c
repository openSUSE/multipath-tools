#include "mt-libudev.h"
#include <stddef.h>
#include <libudev.h>
#include "util.h"

static pthread_mutex_t libudev_mutex = PTHREAD_MUTEX_INITIALIZER;

#define LU_WRAP_0(rtype, func)						\
	rtype mt_##func(void) {						\
		int __oldstate;						\
		rtype __r;						\
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &__oldstate); \
		pthread_mutex_lock(&libudev_mutex);			\
		__r = func();						\
		pthread_mutex_unlock(&libudev_mutex);			\
		pthread_setcancelstate(__oldstate, NULL);		\
		pthread_testcancel();					\
		return __r;						\
	}

#define LU_WRAP_1(rtype, func, type1)					\
	rtype mt_##func(type1 __arg1) {					\
		int __oldstate;						\
		rtype __r;						\
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &__oldstate); \
		pthread_mutex_lock(&libudev_mutex);			\
		__r = func(__arg1);						\
		pthread_mutex_unlock(&libudev_mutex);			\
		pthread_setcancelstate(__oldstate, NULL);		\
		pthread_testcancel();					\
		return __r;						\
	}

#define LU_WRAP_2(rtype, func, type1, type2)				\
	rtype mt_##func(type1 __arg1, type2 __arg2) {			\
		int __oldstate;						\
		rtype __r;						\
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &__oldstate); \
		pthread_mutex_lock(&libudev_mutex);			\
		__r = func(__arg1, __arg2);				\
		pthread_mutex_unlock(&libudev_mutex);			\
		pthread_setcancelstate(__oldstate, NULL);		\
		pthread_testcancel();					\
		return __r;						\
	}

#define LU_WRAP_3(rtype, func, type1, type2, type3)			\
	rtype mt_##func(type1 __arg1, type2 __arg2, type3 __arg3) {	\
		int __oldstate;						\
		rtype __r;						\
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &__oldstate); \
		pthread_mutex_lock(&libudev_mutex);			\
		__r = func(__arg1, __arg2, __arg3);			\
		pthread_mutex_unlock(&libudev_mutex);			\
		pthread_setcancelstate(__oldstate, NULL);		\
		pthread_testcancel();					\
		return __r;						\
	}

LU_WRAP_1(struct udev *, udev_ref, struct udev *);
LU_WRAP_1(struct udev *, udev_unref, struct udev *);
LU_WRAP_0(struct udev *, udev_new);
LU_WRAP_1(struct udev_list_entry *, udev_list_entry_get_next,
          struct udev_list_entry *);
LU_WRAP_2(struct udev_list_entry *, udev_list_entry_get_by_name,
          struct udev_list_entry *, const char *);
LU_WRAP_1(const char *, udev_list_entry_get_name, struct udev_list_entry *);
LU_WRAP_1(const char *, udev_list_entry_get_value, struct udev_list_entry *);

LU_WRAP_2(struct udev_device *, udev_device_new_from_syspath, struct udev *,
          const char *);
LU_WRAP_3(struct udev_device *, udev_device_new_from_devnum, struct udev *,
          char, dev_t);
LU_WRAP_3(struct udev_device *, udev_device_new_from_subsystem_sysname,
          struct udev *, const char *, const char *);
LU_WRAP_2(struct udev_device *, udev_device_new_from_device_id, struct udev *,
          char *);
LU_WRAP_1(struct udev_device *, udev_device_new_from_environment, struct udev *);
LU_WRAP_1(struct udev_device *, udev_device_ref, struct udev_device *);
LU_WRAP_1(struct udev_device *, udev_device_unref, struct udev_device *);
LU_WRAP_1(struct udev *, udev_device_get_udev, struct udev_device *);
LU_WRAP_1(struct udev_device *, udev_device_get_parent, struct udev_device *);
LU_WRAP_3(struct udev_device *, udev_device_get_parent_with_subsystem_devtype,
          struct udev_device *, const char *, const char *);
LU_WRAP_1(const char *, udev_device_get_devpath, struct udev_device *);
LU_WRAP_1(const char *, udev_device_get_subsystem, struct udev_device *);
LU_WRAP_1(const char *, udev_device_get_devtype, struct udev_device *);
LU_WRAP_1(const char *, udev_device_get_syspath, struct udev_device *);
LU_WRAP_1(const char *, udev_device_get_sysname, struct udev_device *);
LU_WRAP_1(dev_t, udev_device_get_devnum, struct udev_device *);
LU_WRAP_1(unsigned long long, udev_device_get_seqnum, struct udev_device *);
LU_WRAP_1(const char *, udev_device_get_driver, struct udev_device *);
LU_WRAP_1(const char *, udev_device_get_devnode, struct udev_device *);
LU_WRAP_1(int, udev_device_get_is_initialized, struct udev_device *);
LU_WRAP_2(const char *, udev_device_get_property_value, struct udev_device *,
          const char *);
LU_WRAP_2(const char *, udev_device_get_sysattr_value, struct udev_device *,
          const char *);
LU_WRAP_3(int, udev_device_set_sysattr_value, struct udev_device *,
          const char *, char *);
LU_WRAP_1(struct udev_list_entry *, udev_device_get_properties_list_entry,
          struct udev_device *);

LU_WRAP_2(struct udev_monitor *, udev_monitor_new_from_netlink, struct udev *,
          const char *);
LU_WRAP_1(struct udev_monitor *, udev_monitor_ref, struct udev_monitor *);
LU_WRAP_1(struct udev_monitor *, udev_monitor_unref, struct udev_monitor *);
LU_WRAP_1(int, udev_monitor_enable_receiving, struct udev_monitor *);
LU_WRAP_1(int, udev_monitor_get_fd, struct udev_monitor *);
LU_WRAP_1(struct udev_device *, udev_monitor_receive_device, struct udev_monitor *);
LU_WRAP_3(int, udev_monitor_filter_add_match_subsystem_devtype,
          struct udev_monitor *, const char *, const char *);
LU_WRAP_2(int, udev_monitor_set_receive_buffer_size, struct udev_monitor *, int);

LU_WRAP_1(struct udev_enumerate *, udev_enumerate_new, struct udev *);
LU_WRAP_1(struct udev_enumerate *, udev_enumerate_ref, struct udev_enumerate *);
LU_WRAP_1(struct udev_enumerate *, udev_enumerate_unref, struct udev_enumerate *);
LU_WRAP_2(int, udev_enumerate_add_match_subsystem, struct udev_enumerate *,
          const char *);
LU_WRAP_2(int, udev_enumerate_add_nomatch_subsystem, struct udev_enumerate *,
          const char *);
LU_WRAP_3(int, udev_enumerate_add_match_sysattr, struct udev_enumerate *,
          const char *, const char *);
LU_WRAP_3(int, udev_enumerate_add_nomatch_sysattr, struct udev_enumerate *,
          const char *, const char *);
LU_WRAP_3(int, udev_enumerate_add_match_property, struct udev_enumerate *,
          const char *, const char *);
LU_WRAP_2(int, udev_enumerate_add_match_tag, struct udev_enumerate *, const char *);
LU_WRAP_2(int, udev_enumerate_add_match_parent, struct udev_enumerate *,
          struct udev_device *);
LU_WRAP_1(int, udev_enumerate_add_match_is_initialized, struct udev_enumerate *);
LU_WRAP_2(int, udev_enumerate_add_syspath, struct udev_enumerate *, const char *);
LU_WRAP_1(int, udev_enumerate_scan_devices, struct udev_enumerate *);
LU_WRAP_1(struct udev_list_entry *, udev_enumerate_get_list_entry,
          struct udev_enumerate *);
