/*
 *  thermal_helpers.c - helper functions to handle thermal devices
 *
 *  Copyright (C) 2016 Eduardo Valentin <edubezval@gmail.com>
 *
 *  Highly based on original thermal_core.c
 *  Copyright (C) 2008 Intel Corp
 *  Copyright (C) 2008 Zhang Rui <rui.zhang@intel.com>
 *  Copyright (C) 2008 Sujith Thomas <sujith.thomas@intel.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <trace/events/thermal.h>

#include "thermal_core.h"

int get_tz_trend(struct thermal_zone_device *tz, int trip)
{
	enum thermal_trend trend;

	if (tz->emul_temperature || !tz->ops->get_trend ||
	    tz->ops->get_trend(tz, trip, &trend)) {
		if (tz->temperature > tz->last_temperature)
			trend = THERMAL_TREND_RAISING;
		else if (tz->temperature < tz->last_temperature)
			trend = THERMAL_TREND_DROPPING;
		else
			trend = THERMAL_TREND_STABLE;
	}

	return trend;
}
EXPORT_SYMBOL(get_tz_trend);

struct thermal_instance *
get_thermal_instance(struct thermal_zone_device *tz,
		     struct thermal_cooling_device *cdev, int trip)
{
	struct thermal_instance *pos = NULL;
	struct thermal_instance *target_instance = NULL;

	mutex_lock(&tz->lock);
	mutex_lock(&cdev->lock);

	list_for_each_entry(pos, &tz->thermal_instances, tz_node) {
		if (pos->tz == tz && pos->trip == trip && pos->cdev == cdev) {
			target_instance = pos;
			break;
		}
	}

	mutex_unlock(&cdev->lock);
	mutex_unlock(&tz->lock);

	return target_instance;
}
EXPORT_SYMBOL(get_thermal_instance);

/**
 * thermal_zone_get_temp() - returns the temperature of a thermal zone
 * @tz: a valid pointer to a struct thermal_zone_device
 * @temp: a valid pointer to where to store the resulting temperature.
 *
 * When a valid thermal zone reference is passed, it will fetch its
 * temperature and fill @temp.
 *
 * Return: On success returns 0, an error code otherwise
 */
int thermal_zone_get_temp(struct thermal_zone_device *tz, int *temp)
{
	int ret = -EINVAL;
	int count;
	int crit_temp = INT_MAX;
	enum thermal_trip_type type;

	if (!tz || IS_ERR(tz) || !tz->ops->get_temp)
		goto exit;

	mutex_lock(&tz->lock);

	ret = tz->ops->get_temp(tz, temp);

	if (IS_ENABLED(CONFIG_THERMAL_EMULATION) && tz->emul_temperature) {
		for (count = 0; count < tz->trips; count++) {
			ret = tz->ops->get_trip_type(tz, count, &type);
			if (!ret && type == THERMAL_TRIP_CRITICAL) {
				ret = tz->ops->get_trip_temp(tz, count,
						&crit_temp);
				break;
			}
		}

		/*
		 * Only allow emulating a temperature when the real temperature
		 * is below the critical temperature so that the emulation code
		 * cannot hide critical conditions.
		 */
		if (!ret && *temp < crit_temp)
			*temp = tz->emul_temperature;
	}
	trace_thermal_query_temp(tz, *temp);
	mutex_unlock(&tz->lock);
exit:
	return ret;
}
EXPORT_SYMBOL_GPL(thermal_zone_get_temp);

void thermal_zone_set_trips(struct thermal_zone_device *tz)
{
	int low = -INT_MAX;
	int high = INT_MAX;
	int trip_temp, hysteresis;
	int i, ret;

	mutex_lock(&tz->lock);

	if (!tz->ops->set_trips || !tz->ops->get_trip_hyst)
		goto exit;

	for (i = 0; i < tz->trips; i++) {
		int trip_low;

		tz->ops->get_trip_temp(tz, i, &trip_temp);
		tz->ops->get_trip_hyst(tz, i, &hysteresis);

		trip_low = trip_temp - hysteresis;

		if (trip_low < tz->temperature && trip_low > low)
			low = trip_low;

		if (trip_temp > tz->temperature && trip_temp < high)
			high = trip_temp;
	}

	tz->prev_low_trip = low;
	tz->prev_high_trip = high;

	dev_dbg(&tz->device,
		"thermal name:%s,  new temperature boundaries: %d < x < %d\n", tz->type, low, high);

	/*
	 * Set a temperature window. When this window is left the driver
	 * must inform the thermal core via thermal_zone_device_update.
	 */
	 
	/* ASUS BSP Clay: remove ambigous log when thermal is stable +++ */
	 /* Skip lmh-dcvs-00 & 01 since these nodes are not monitor on-die sensor */
	if(!strcmp(tz->type, "lmh-dcvs-00")){
		dev_dbg(&tz->device, "Thermal: Skip set_trip!!!");
	}else if(!strcmp(tz->type, "lmh-dcvs-01")){
		dev_dbg(&tz->device, "Thermal: Skip set_trip!!!");
	}else{
		ret = tz->ops->set_trips(tz, low, high);
		if (ret)
			dev_err(&tz->device, "Failed to set trips: %d\n", ret);
	}
	/* ASUS BSP Clay: remove ambigous log when thermal is stable --- */
	trace_thermal_set_trip(tz);

exit:
	mutex_unlock(&tz->lock);
}
EXPORT_SYMBOL_GPL(thermal_zone_set_trips);

void thermal_cdev_update(struct thermal_cooling_device *cdev)
{
	struct thermal_instance *instance;
	unsigned long current_target = 0, min_target = ULONG_MAX;

	mutex_lock(&cdev->lock);
	/* cooling device is updated*/
	if (cdev->updated) {
		mutex_unlock(&cdev->lock);
		return;
	}

	/* Make sure cdev enters the deepest cooling state */
	current_target = cdev->sysfs_cur_state_req;
	min_target = cdev->sysfs_min_state_req;
	list_for_each_entry(instance, &cdev->thermal_instances, cdev_node) {
		dev_dbg(&cdev->device, "zone%d->target=%lu\n",
				instance->tz->id, instance->target);
		if (instance->target == THERMAL_NO_TARGET)
			continue;
		if (instance->tz->governor->min_state_throttle) {
			if (instance->target < min_target)
				min_target = instance->target;
		} else {
			if (instance->target > current_target)
				current_target = instance->target;
		}
	}
	trace_cdev_update_start(cdev);
	cdev->ops->set_cur_state(cdev, current_target);
	if (cdev->ops->set_min_state)
		cdev->ops->set_min_state(cdev, min_target);
	cdev->updated = true;
	mutex_unlock(&cdev->lock);
	trace_cdev_update(cdev, current_target, min_target);
	dev_dbg(&cdev->device, "set to state %lu min state %lu\n",
				current_target, min_target);
}
EXPORT_SYMBOL(thermal_cdev_update);

/**
 * thermal_zone_get_slope - return the slope attribute of the thermal zone
 * @tz: thermal zone device with the slope attribute
 *
 * Return: If the thermal zone device has a slope attribute, return it, else
 * return 1.
 */
int thermal_zone_get_slope(struct thermal_zone_device *tz)
{
	if (tz && tz->tzp)
		return tz->tzp->slope;
	return 1;
}
EXPORT_SYMBOL_GPL(thermal_zone_get_slope);

/**
 * thermal_zone_get_offset - return the offset attribute of the thermal zone
 * @tz: thermal zone device with the offset attribute
 *
 * Return: If the thermal zone device has a offset attribute, return it, else
 * return 0.
 */
int thermal_zone_get_offset(struct thermal_zone_device *tz)
{
	if (tz && tz->tzp)
		return tz->tzp->offset;
	return 0;
}
EXPORT_SYMBOL_GPL(thermal_zone_get_offset);
