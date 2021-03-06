/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2010  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <string.h>

#include "connman.h"

static GSList *device_list = NULL;
static gchar **device_filter = NULL;
static gchar **nodevice_filter = NULL;

struct connman_device {
	gint refcount;
	enum connman_device_type type;
	connman_bool_t offlinemode;
	connman_bool_t blocked;
	connman_bool_t powered;
	connman_bool_t powered_pending;
	connman_bool_t powered_persistent;
	connman_bool_t scanning;
	connman_bool_t disconnected;
	connman_bool_t reconnect;
	connman_uint16_t scan_interval;
	connman_uint16_t backoff_interval;
	char *name;
	char *node;
	char *address;
	char *interface;
	char *ident;
	char *path;
	char *devname;
	int phyindex;
	int index;
	guint scan_timeout;

	struct connman_device_driver *driver;
	void *driver_data;

	char *last_network;
	struct connman_network *network;
	GHashTable *networks;
};

#define SCAN_INITIAL_DELAY 10

static gboolean device_scan_trigger(gpointer user_data)
{
	struct connman_device *device = user_data;

	DBG("device %p", device);

	if (device->driver == NULL) {
		device->scan_timeout = 0;
		return FALSE;
	}

	if (device->driver->scan)
		device->driver->scan(device);

	return TRUE;
}

static void clear_scan_trigger(struct connman_device *device)
{
	if (device->scan_timeout > 0) {
		g_source_remove(device->scan_timeout);
		device->scan_timeout = 0;
	}
}

static void reset_scan_trigger(struct connman_device *device)
{
	clear_scan_trigger(device);

	if (device->scan_interval > 0) {
		guint interval;

		if (g_hash_table_size(device->networks) == 0) {
			if (device->backoff_interval >= device->scan_interval)
				device->backoff_interval = SCAN_INITIAL_DELAY;
			interval = device->backoff_interval;
		} else
			interval = device->scan_interval;

		DBG("interval %d", interval);

		device->scan_timeout = g_timeout_add_seconds(interval,
					device_scan_trigger, device);

		device->backoff_interval *= 2;
		if (device->backoff_interval > device->scan_interval)
			device->backoff_interval = device->scan_interval;
	}
}

static void force_scan_trigger(struct connman_device *device)
{
	clear_scan_trigger(device);

	device->scan_timeout = g_timeout_add_seconds(5,
					device_scan_trigger, device);
}

void connman_device_schedule_scan(struct connman_device *device)
{
	reset_scan_trigger(device);
}

static const char *type2description(enum connman_device_type type)
{
	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_VENDOR:
		break;
	case CONNMAN_DEVICE_TYPE_ETHERNET:
		return "Ethernet";
	case CONNMAN_DEVICE_TYPE_WIFI:
		return "Wireless";
	case CONNMAN_DEVICE_TYPE_WIMAX:
		return "WiMAX";
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
		return "Bluetooth";
	case CONNMAN_DEVICE_TYPE_GPS:
		return "GPS";
	case CONNMAN_DEVICE_TYPE_CELLULAR:
		return "Cellular";
	case CONNMAN_DEVICE_TYPE_GADGET:
		return "Gadget";

	}

	return NULL;
}

static const char *type2string(enum connman_device_type type)
{
	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_VENDOR:
		break;
	case CONNMAN_DEVICE_TYPE_ETHERNET:
		return "ethernet";
	case CONNMAN_DEVICE_TYPE_WIFI:
		return "wifi";
	case CONNMAN_DEVICE_TYPE_WIMAX:
		return "wimax";
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
		return "bluetooth";
	case CONNMAN_DEVICE_TYPE_GPS:
		return "gps";
	case CONNMAN_DEVICE_TYPE_CELLULAR:
		return "cellular";
	case CONNMAN_DEVICE_TYPE_GADGET:
		return "gadget";

	}

	return NULL;
}

enum connman_service_type __connman_device_get_service_type(struct connman_device *device)
{
	enum connman_device_type type = connman_device_get_type(device);

	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_VENDOR:
	case CONNMAN_DEVICE_TYPE_GPS:
		break;
	case CONNMAN_DEVICE_TYPE_ETHERNET:
		return CONNMAN_SERVICE_TYPE_ETHERNET;
	case CONNMAN_DEVICE_TYPE_WIFI:
		return CONNMAN_SERVICE_TYPE_WIFI;
	case CONNMAN_DEVICE_TYPE_WIMAX:
		return CONNMAN_SERVICE_TYPE_WIMAX;
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
		return CONNMAN_SERVICE_TYPE_BLUETOOTH;
	case CONNMAN_DEVICE_TYPE_CELLULAR:
		return CONNMAN_SERVICE_TYPE_CELLULAR;
	case CONNMAN_DEVICE_TYPE_GADGET:
		return CONNMAN_SERVICE_TYPE_GADGET;

	}

	return CONNMAN_SERVICE_TYPE_UNKNOWN;
}

int __connman_device_enable(struct connman_device *device)
{
	int err;
	enum connman_service_type type;

	DBG("device %p %d", device, device->blocked);

	if (!device->driver || !device->driver->enable)
		return -EOPNOTSUPP;

	if (device->powered_pending == TRUE)
		return -EALREADY;

	if (device->blocked == TRUE)
		return -ENOLINK;

	connman_device_set_disconnected(device, FALSE);
	device->scanning = FALSE;

	err = device->driver->enable(device);
	if (err < 0 && err != -EALREADY) {
		if (err == -EINPROGRESS) {
			device->powered_pending = TRUE;
			device->offlinemode = FALSE;
			if (__connman_profile_get_offlinemode() == TRUE)
				__connman_profile_set_offlinemode(FALSE, FALSE);
		}
		return err;
	}

	device->powered_pending = TRUE;
	device->powered = TRUE;
	device->offlinemode = FALSE;
	if (__connman_profile_get_offlinemode() == TRUE)
		__connman_profile_set_offlinemode(FALSE, FALSE);

	type = __connman_device_get_service_type(device);
	__connman_technology_enable(type);

	return 0;
}

int __connman_device_disable(struct connman_device *device)
{
	int err;
	enum connman_service_type type;

	DBG("device %p", device);

	if (!device->driver || !device->driver->disable)
		return -EOPNOTSUPP;

	if (device->powered == FALSE)
		return -ENOLINK;

	if (device->powered_pending == FALSE)
		return -EALREADY;

	device->reconnect = FALSE;

	clear_scan_trigger(device);

	if (device->network)
		connman_network_set_connected(device->network, FALSE);

	err = device->driver->disable(device);
	if (err < 0 && err != -EALREADY) {
		if (err == -EINPROGRESS)
			device->powered_pending = FALSE;
		return err;
	}

	g_hash_table_remove_all(device->networks);

	device->powered_pending = FALSE;
	device->powered = FALSE;

	type = __connman_device_get_service_type(device);
	__connman_technology_disable(type);

	return 0;
}

static int set_powered(struct connman_device *device, connman_bool_t powered)
{
	DBG("device %p powered %d", device, powered);

	if (powered == TRUE)
		return __connman_device_enable(device);
	else
		return __connman_device_disable(device);
}

static int setup_device(struct connman_device *device)
{
	DBG("device %p", device);

	__connman_technology_add_device(device);

	if (device->offlinemode == FALSE &&
				device->powered_persistent == TRUE)
		__connman_device_enable(device);

	return 0;
}

static void probe_driver(struct connman_device_driver *driver)
{
	GSList *list;

	DBG("driver %p name %s", driver, driver->name);

	for (list = device_list; list != NULL; list = list->next) {
		struct connman_device *device = list->data;

		if (device->driver != NULL)
			continue;

		if (driver->type != device->type)
			continue;

		if (driver->probe(device) < 0)
			continue;

		device->driver = driver;

		setup_device(device);
	}
}

static void remove_device(struct connman_device *device)
{
	DBG("device %p", device);

	__connman_device_disable(device);

	__connman_technology_remove_device(device);

	if (device->driver->remove)
		device->driver->remove(device);

	device->driver = NULL;
}

static void remove_driver(struct connman_device_driver *driver)
{
	GSList *list;

	DBG("driver %p name %s", driver, driver->name);

	for (list = device_list; list != NULL; list = list->next) {
		struct connman_device *device = list->data;

		if (device->driver == driver)
			remove_device(device);
	}
}

connman_bool_t __connman_device_has_driver(struct connman_device *device)
{
	if (device == NULL || device->driver == NULL)
		return FALSE;

	return TRUE;
}

static GSList *driver_list = NULL;

static gint compare_priority(gconstpointer a, gconstpointer b)
{
	const struct connman_device_driver *driver1 = a;
	const struct connman_device_driver *driver2 = b;

	return driver2->priority - driver1->priority;
}

/**
 * connman_device_driver_register:
 * @driver: device driver definition
 *
 * Register a new device driver
 *
 * Returns: %0 on success
 */
int connman_device_driver_register(struct connman_device_driver *driver)
{
	DBG("driver %p name %s", driver, driver->name);

	driver_list = g_slist_insert_sorted(driver_list, driver,
							compare_priority);
	probe_driver(driver);

	return 0;
}

/**
 * connman_device_driver_unregister:
 * @driver: device driver definition
 *
 * Remove a previously registered device driver
 */
void connman_device_driver_unregister(struct connman_device_driver *driver)
{
	DBG("driver %p name %s", driver, driver->name);

	driver_list = g_slist_remove(driver_list, driver);

	remove_driver(driver);
}

static void free_network(gpointer data)
{
	struct connman_network *network = data;

	DBG("network %p", network);

	__connman_network_set_device(network, NULL);

	connman_network_unref(network);
}

static void device_destruct(struct connman_device *device)
{
	DBG("device %p name %s", device, device->name);

	clear_scan_trigger(device);

	g_free(device->ident);
	g_free(device->node);
	g_free(device->name);
	g_free(device->address);
	g_free(device->interface);
	g_free(device->path);
	g_free(device->devname);

	g_free(device->last_network);

	g_hash_table_destroy(device->networks);
	device->networks = NULL;

	g_free(device);
}

/**
 * connman_device_create:
 * @node: device node name (for example an address)
 * @type: device type
 *
 * Allocate a new device of given #type and assign the #node name to it.
 *
 * Returns: a newly-allocated #connman_device structure
 */
struct connman_device *connman_device_create(const char *node,
						enum connman_device_type type)
{
	struct connman_device *device;
	enum connman_service_type service_type;
	connman_bool_t bg_scan;

	DBG("node %s type %d", node, type);

	device = g_try_new0(struct connman_device, 1);
	if (device == NULL)
		return NULL;

	DBG("device %p", device);

	device->refcount = 1;

	bg_scan = connman_setting_get_bool("BackgroundScanning");

	device->type = type;
	device->name = g_strdup(type2description(device->type));

	device->powered_persistent = TRUE;

	device->phyindex = -1;

	service_type = __connman_device_get_service_type(device);
	device->blocked = __connman_technology_get_blocked(service_type);
	device->backoff_interval = SCAN_INITIAL_DELAY;

	switch (type) {
	case CONNMAN_DEVICE_TYPE_UNKNOWN:
	case CONNMAN_DEVICE_TYPE_ETHERNET:
	case CONNMAN_DEVICE_TYPE_WIMAX:
	case CONNMAN_DEVICE_TYPE_BLUETOOTH:
	case CONNMAN_DEVICE_TYPE_CELLULAR:
	case CONNMAN_DEVICE_TYPE_GPS:
	case CONNMAN_DEVICE_TYPE_GADGET:
	case CONNMAN_DEVICE_TYPE_VENDOR:
		device->scan_interval = 0;
		break;
	case CONNMAN_DEVICE_TYPE_WIFI:
		if (bg_scan == TRUE)
			device->scan_interval = 300;
		else
			device->scan_interval = 0;
		break;
	}

	device->networks = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, free_network);

	device_list = g_slist_append(device_list, device);

	return device;
}

/**
 * connman_device_ref:
 * @device: device structure
 *
 * Increase reference counter of device
 */
struct connman_device *connman_device_ref(struct connman_device *device)
{
	DBG("%p", device);

	g_atomic_int_inc(&device->refcount);

	return device;
}

/**
 * connman_device_unref:
 * @device: device structure
 *
 * Decrease reference counter of device
 */
void connman_device_unref(struct connman_device *device)
{
	if (g_atomic_int_dec_and_test(&device->refcount) == FALSE)
		return;

	if (device->driver) {
		device->driver->remove(device);
		device->driver = NULL;
	}

	device_list = g_slist_remove(device_list, device);

	device_destruct(device);
}

const char *__connman_device_get_type(struct connman_device *device)
{
	return type2string(device->type);
}

/**
 * connman_device_get_type:
 * @device: device structure
 *
 * Get type of device
 */
enum connman_device_type connman_device_get_type(struct connman_device *device)
{
	return device->type;
}

/**
 * connman_device_set_index:
 * @device: device structure
 * @index: index number
 *
 * Set index number of device
 */
void connman_device_set_index(struct connman_device *device, int index)
{
	device->index = index;
}

/**
 * connman_device_get_index:
 * @device: device structure
 *
 * Get index number of device
 */
int connman_device_get_index(struct connman_device *device)
{
	return device->index;
}

int __connman_device_get_phyindex(struct connman_device *device)
{
	return device->phyindex;
}

void __connman_device_set_phyindex(struct connman_device *device,
							int phyindex)
{
	device->phyindex = phyindex;
}

/**
 * connman_device_set_interface:
 * @device: device structure
 * @interface: interface name
 *
 * Set interface name of device
 */
void connman_device_set_interface(struct connman_device *device,
						const char *interface)
{
	g_free(device->devname);
	device->devname = g_strdup(interface);

	g_free(device->interface);
	device->interface = g_strdup(interface);

	if (device->name == NULL) {
		const char *str = type2description(device->type);
		if (str != NULL && device->interface != NULL)
			device->name = g_strdup_printf("%s (%s)", str,
							device->interface);
	}
}

/**
 * connman_device_set_ident:
 * @device: device structure
 * @ident: unique identifier
 *
 * Set unique identifier of device
 */
void connman_device_set_ident(struct connman_device *device,
							const char *ident)
{
	g_free(device->ident);
	device->ident = g_strdup(ident);
}

const char *connman_device_get_ident(struct connman_device *device)
{
	return device->ident;
}

/**
 * connman_device_set_powered:
 * @device: device structure
 * @powered: powered state
 *
 * Change power state of device
 */
int connman_device_set_powered(struct connman_device *device,
						connman_bool_t powered)
{
	int err;
	enum connman_service_type type;

	DBG("driver %p powered %d", device, powered);

	if (device->powered == powered) {
		device->powered_pending = powered;
		return -EALREADY;
	}

	if (powered == TRUE)
		err = __connman_device_enable(device);
	else
		err = __connman_device_disable(device);

	if (err < 0 && err != -EINPROGRESS && err != -EALREADY)
		return err;

	device->powered = powered;
	device->powered_pending = powered;

	type = __connman_device_get_service_type(device);

	if (device->powered == TRUE)
		__connman_technology_enable(type);
	else
		__connman_technology_disable(type);

	if (device->offlinemode == TRUE && powered == TRUE)
		return connman_device_set_powered(device, FALSE);

	if (powered == FALSE)
		return 0;

	reset_scan_trigger(device);

	if (device->driver && device->driver->scan)
		device->driver->scan(device);

	return 0;
}

int __connman_device_set_blocked(struct connman_device *device,
						connman_bool_t blocked)
{
	connman_bool_t powered;

	DBG("device %p blocked %d", device, blocked);

	device->blocked = blocked;

	if (device->offlinemode == TRUE)
		return 0;

	connman_info("%s {rfkill} blocked %d", device->interface, blocked);

	if (blocked == FALSE)
		powered = device->powered_persistent;
	else
		powered = FALSE;

	return set_powered(device, powered);
}

connman_bool_t __connman_device_get_blocked(struct connman_device *device)
{
	return device->blocked;
}

int __connman_device_scan(struct connman_device *device)
{
	if (!device->driver || !device->driver->scan)
		return -EOPNOTSUPP;

	if (device->powered == FALSE)
		return -ENOLINK;

	reset_scan_trigger(device);

	return device->driver->scan(device);
}

int __connman_device_enable_persistent(struct connman_device *device)
{
	int err;

	DBG("device %p", device);

	device->powered_persistent = TRUE;

	__connman_storage_save_device(device);

	err = __connman_device_enable(device);
	if (err == 0 || err == -EINPROGRESS) {
		device->offlinemode = FALSE;
		if (__connman_profile_get_offlinemode() == TRUE) {
			__connman_profile_set_offlinemode(FALSE, FALSE);

			__connman_profile_save_default();
		}
	}

	return err;
}

int __connman_device_disable_persistent(struct connman_device *device)
{
	DBG("device %p", device);

	device->powered_persistent = FALSE;

	__connman_storage_save_device(device);

	return __connman_device_disable(device);
}

int __connman_device_disconnect(struct connman_device *device)
{
	GHashTableIter iter;
	gpointer key, value;

	DBG("device %p", device);

	connman_device_set_disconnected(device, TRUE);

	g_hash_table_iter_init(&iter, device->networks);

	while (g_hash_table_iter_next(&iter, &key, &value) == TRUE) {
		struct connman_network *network = value;

		if (connman_network_get_connecting(network) == TRUE) {
			/*
			 * Skip network in the process of connecting.
			 * This is a workaround for WiFi networks serviced
			 * by the supplicant plugin that hold a reference
			 * to the network.  If we disconnect the network
			 * here then the referenced object will not be
			 * registered and usage (like launching DHCP client)
			 * will fail.  There is nothing to be gained by
			 * removing the network here anyway.
			 */
			connman_warn("Skipping disconnect of %s, network is connecting.",
				connman_network_get_identifier(network));
			continue;
		}

		__connman_network_disconnect(network);
	}

	return 0;
}

static void mark_network_available(gpointer key, gpointer value,
							gpointer user_data)
{
	struct connman_network *network = value;

	connman_network_set_available(network, TRUE);
}

static void mark_network_unavailable(gpointer key, gpointer value,
							gpointer user_data)
{
	struct connman_network *network = value;

	if (connman_network_get_connected(network) == TRUE)
		return;

	connman_network_set_available(network, FALSE);
}

static gboolean remove_unavailable_network(gpointer key, gpointer value,
							gpointer user_data)
{
	struct connman_network *network = value;

	if (connman_network_get_connected(network) == TRUE)
		return FALSE;

	if (connman_network_get_available(network) == TRUE)
		return FALSE;

	return TRUE;
}

void __connman_device_cleanup_networks(struct connman_device *device)
{
	g_hash_table_foreach_remove(device->networks,
					remove_unavailable_network, NULL);
}

connman_bool_t __connman_device_scanning(struct connman_device *device)
{
	return device->scanning;
}

void connman_device_reset_scanning(struct connman_device *device)
{
	device->scanning = FALSE;

	g_hash_table_foreach(device->networks,
				mark_network_available, NULL);

}

/**
 * connman_device_set_scanning:
 * @device: device structure
 * @scanning: scanning state
 *
 * Change scanning state of device
 */
int connman_device_set_scanning(struct connman_device *device,
						connman_bool_t scanning)
{
	DBG("device %p scanning %d", device, scanning);

	if (!device->driver || !device->driver->scan)
		return -EINVAL;

	if (device->scanning == scanning)
		return -EALREADY;

	device->scanning = scanning;

	if (scanning == TRUE) {
		reset_scan_trigger(device);

		g_hash_table_foreach(device->networks,
					mark_network_unavailable, NULL);

		return 0;
	}

	__connman_device_cleanup_networks(device);

	__connman_service_auto_connect();

	return 0;
}

/**
 * connman_device_set_disconnected:
 * @device: device structure
 * @disconnected: disconnected state
 *
 * Change disconnected state of device (only for device with networks)
 */
int connman_device_set_disconnected(struct connman_device *device,
						connman_bool_t disconnected)
{
	DBG("device %p disconnected %d", device, disconnected);

	if (device->disconnected == disconnected)
		return -EALREADY;

	device->disconnected = disconnected;

	if (disconnected == TRUE)
	{
		force_scan_trigger(device);
		device->backoff_interval = SCAN_INITIAL_DELAY;
	}

	return 0;
}

/**
 * connman_device_get_disconnected:
 * @device: device structure
 *
 * Get device disconnected state
 */
connman_bool_t connman_device_get_disconnected(struct connman_device *device)
{
	return device->disconnected;
}

/**
 * connman_device_set_string:
 * @device: device structure
 * @key: unique identifier
 * @value: string value
 *
 * Set string value for specific key
 */
int connman_device_set_string(struct connman_device *device,
					const char *key, const char *value)
{
	DBG("device %p key %s value %s", device, key, value);

	if (g_str_equal(key, "Address") == TRUE) {
		g_free(device->address);
		device->address = g_strdup(value);
	} else if (g_str_equal(key, "Name") == TRUE) {
		g_free(device->name);
		device->name = g_strdup(value);
	} else if (g_str_equal(key, "Node") == TRUE) {
		g_free(device->node);
		device->node = g_strdup(value);
	} else if (g_str_equal(key, "Path") == TRUE) {
		g_free(device->path);
		device->path = g_strdup(value);
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * connman_device_get_string:
 * @device: device structure
 * @key: unique identifier
 *
 * Get string value for specific key
 */
const char *connman_device_get_string(struct connman_device *device,
							const char *key)
{
	DBG("device %p key %s", device, key);

	if (g_str_equal(key, "Address") == TRUE)
		return device->address;
	else if (g_str_equal(key, "Name") == TRUE)
		return device->name;
	else if (g_str_equal(key, "Node") == TRUE)
		return device->node;
	else if (g_str_equal(key, "Interface") == TRUE)
		return device->interface;
	else if (g_str_equal(key, "Path") == TRUE)
		return device->path;

	return NULL;
}

static void set_offlinemode(struct connman_device *device,
				connman_bool_t offlinemode)
{
	connman_bool_t powered;

	DBG("device %p name %s", device, device->name);

	if (device == NULL)
		return;

	device->offlinemode = offlinemode;

	if (device->blocked == TRUE)
		return;

	powered = (offlinemode == TRUE) ? FALSE : TRUE;

	if (device->powered == powered)
		return;

	if (device->powered_persistent == FALSE)
		powered = FALSE;

	set_powered(device, powered);
}

int __connman_device_set_offlinemode(connman_bool_t offlinemode)
{
	GSList *list;

	DBG("offlinmode %d", offlinemode);

	for (list = device_list; list != NULL; list = list->next) {
		struct connman_device *device = list->data;

		set_offlinemode(device, offlinemode);
	}

	__connman_notifier_offlinemode(offlinemode);

	return 0;
}

/**
 * connman_device_add_network:
 * @device: device structure
 * @network: network structure
 *
 * Add new network to the device
 */
int connman_device_add_network(struct connman_device *device,
					struct connman_network *network)
{
	const char *identifier = connman_network_get_identifier(network);

	DBG("device %p network %p", device, network);

	if (identifier == NULL)
		return -EINVAL;

	connman_network_ref(network);

	__connman_network_set_device(network, device);

	g_hash_table_insert(device->networks, g_strdup(identifier),
								network);

	return 0;
}

/**
 * connman_device_get_network:
 * @device: device structure
 * @identifier: network identifier
 *
 * Get network for given identifier
 */
struct connman_network *connman_device_get_network(struct connman_device *device,
							const char *identifier)
{
	DBG("device %p identifier %s", device, identifier);

	return g_hash_table_lookup(device->networks, identifier);
}

/**
 * connman_device_remove_network:
 * @device: device structure
 * @identifier: network identifier
 *
 * Remove network for given identifier
 */
int connman_device_remove_network(struct connman_device *device,
						struct connman_network *network)
{
	const char *identifier;

	DBG("device %p network %p", device, network);

	if (network == NULL)
		return 0;

	identifier = connman_network_get_identifier(network);
	g_hash_table_remove(device->networks, identifier);

	return 0;
}

void connman_device_remove_all_networks(struct connman_device *device)
{
	g_hash_table_remove_all(device->networks);
}

void __connman_device_set_network(struct connman_device *device,
					struct connman_network *network)
{
	const char *name;

	if (device == NULL)
		return;

	if (device->network == network)
		return;

	if (network != NULL) {
		name = connman_network_get_string(network, "Name");
		g_free(device->last_network);
		device->last_network = g_strdup(name);

		device->network = network;
	} else {
		g_free(device->last_network);
		device->last_network = NULL;

		device->network = NULL;
	}
}

void __connman_device_set_reconnect(struct connman_device *device,
						connman_bool_t reconnect)
{
	device->reconnect = reconnect;
}

connman_bool_t  __connman_device_get_reconnect(
				struct connman_device *device)
{
	return device->reconnect;
}

static gboolean match_driver(struct connman_device *device,
					struct connman_device_driver *driver)
{
	if (device->type == driver->type ||
			driver->type == CONNMAN_DEVICE_TYPE_UNKNOWN)
		return TRUE;

	return FALSE;
}

static int device_probe(struct connman_device *device)
{
	GSList *list;

	DBG("device %p name %s", device, device->name);

	if (device->driver != NULL)
		return -EALREADY;

	for (list = driver_list; list; list = list->next) {
		struct connman_device_driver *driver = list->data;

		if (match_driver(device, driver) == FALSE)
			continue;

		DBG("driver %p name %s", driver, driver->name);

		if (driver->probe(device) == 0) {
			device->driver = driver;
			break;
		}
	}

	if (device->driver == NULL)
		return 0;

	return setup_device(device);
}

static void device_remove(struct connman_device *device)
{
	DBG("device %p name %s", device, device->name);

	if (device->driver == NULL)
		return;

	remove_device(device);
}

/**
 * connman_device_register:
 * @device: device structure
 *
 * Register device with the system
 */
int connman_device_register(struct connman_device *device)
{
	__connman_storage_load_device(device);

	device->offlinemode = __connman_profile_get_offlinemode();

	return device_probe(device);
}

/**
 * connman_device_unregister:
 * @device: device structure
 *
 * Unregister device with the system
 */
void connman_device_unregister(struct connman_device *device)
{
	__connman_storage_save_device(device);

	device_remove(device);
}

/**
 * connman_device_get_data:
 * @device: device structure
 *
 * Get private device data pointer
 */
void *connman_device_get_data(struct connman_device *device)
{
	return device->driver_data;
}

/**
 * connman_device_set_data:
 * @device: device structure
 * @data: data pointer
 *
 * Set private device data pointer
 */
void connman_device_set_data(struct connman_device *device, void *data)
{
	device->driver_data = data;
}

struct connman_device *__connman_device_find_device(
				enum connman_service_type type)
{
	GSList *list;

	for (list = device_list; list != NULL; list = list->next) {
		struct connman_device *device = list->data;
		enum connman_service_type service_type =
			__connman_device_get_service_type(device);

		if (service_type != type)
			continue;

		return device;
	}

	return NULL;
}

int __connman_device_request_scan(enum connman_service_type type)
{
	GSList *list;
	int err;

	switch (type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return 0;
	case CONNMAN_SERVICE_TYPE_WIFI:
	case CONNMAN_SERVICE_TYPE_WIMAX:
		break;
	}

	for (list = device_list; list != NULL; list = list->next) {
		struct connman_device *device = list->data;
		enum connman_service_type service_type =
			__connman_device_get_service_type(device);

		if (service_type != CONNMAN_SERVICE_TYPE_UNKNOWN &&
				service_type != type) {
			continue;
		}

		err = __connman_device_scan(device);
		if (err < 0 && err != -EINPROGRESS) {
			DBG("err %d", err);
			/* XXX maybe only a continue? */
			return err;
		}
	}

	return 0;
}

static int set_technology(enum connman_service_type type, connman_bool_t enable)
{
	GSList *list;
	int err;

	DBG("type %d enable %d", type, enable);

	switch (type) {
	case CONNMAN_SERVICE_TYPE_UNKNOWN:
	case CONNMAN_SERVICE_TYPE_SYSTEM:
	case CONNMAN_SERVICE_TYPE_GPS:
	case CONNMAN_SERVICE_TYPE_VPN:
	case CONNMAN_SERVICE_TYPE_GADGET:
		return 0;
	case CONNMAN_SERVICE_TYPE_ETHERNET:
	case CONNMAN_SERVICE_TYPE_WIFI:
	case CONNMAN_SERVICE_TYPE_WIMAX:
	case CONNMAN_SERVICE_TYPE_BLUETOOTH:
	case CONNMAN_SERVICE_TYPE_CELLULAR:
		break;
	}

	for (list = device_list; list != NULL; list = list->next) {
		struct connman_device *device = list->data;
		enum connman_service_type service_type =
			__connman_device_get_service_type(device);

		if (service_type != CONNMAN_SERVICE_TYPE_UNKNOWN &&
				service_type != type) {
			continue;
		}

		if (enable == TRUE)
			err = __connman_device_enable_persistent(device);
		else
			err = __connman_device_disable_persistent(device);

		if (err < 0 && err != -EINPROGRESS) {
			DBG("err %d", err);
			/* XXX maybe only a continue? */
			return err;
		}
	}

	return 0;
}

int __connman_device_enable_technology(enum connman_service_type type)
{
	return set_technology(type, TRUE);
}

int __connman_device_disable_technology(enum connman_service_type type)
{
	return set_technology(type, FALSE);
}

connman_bool_t __connman_device_isfiltered(const char *devname)
{
	char **pattern;

	if (device_filter == NULL)
		goto nodevice;

	for (pattern = device_filter; *pattern; pattern++) {
		if (g_pattern_match_simple(*pattern, devname) == FALSE) {
			DBG("ignoring device %s (match)", devname);
			return TRUE;
		}
	}

nodevice:
	if (g_pattern_match_simple("dummy*", devname) == TRUE) {
		DBG("ignoring dummy networking devices");
		return TRUE;
	}

	if (nodevice_filter == NULL)
		return FALSE;

	for (pattern = nodevice_filter; *pattern; pattern++) {
		if (g_pattern_match_simple(*pattern, devname) == TRUE) {
			DBG("ignoring device %s (no match)", devname);
			return TRUE;
		}
	}

	return FALSE;
}

static int device_load(struct connman_device *device)
{
	const char *ident = __connman_profile_active_ident();
	GKeyFile *keyfile;
	GError *error = NULL;
	gchar *identifier;
	connman_bool_t powered;

	DBG("device %p", device);

	keyfile = __connman_storage_open_profile(ident);
	if (keyfile == NULL)
		return 0;

	identifier = g_strdup_printf("device_%s", device->name);
	if (identifier == NULL)
		goto done;

	powered = g_key_file_get_boolean(keyfile, identifier,
						"Powered", &error);
	if (error == NULL)
		device->powered_persistent = powered;
	g_clear_error(&error);

done:
	g_free(identifier);

	__connman_storage_close_profile(ident, keyfile, FALSE);

	return 0;
}

static int device_save(struct connman_device *device)
{
	const char *ident = __connman_profile_active_ident();
	GKeyFile *keyfile;
	gchar *identifier;

	DBG("device %p", device);

	keyfile = __connman_storage_open_profile(ident);
	if (keyfile == NULL)
		return 0;

	identifier = g_strdup_printf("device_%s", device->name);
	if (identifier == NULL)
		goto done;

	g_key_file_set_boolean(keyfile, identifier,
					"Powered", device->powered_persistent);

done:
	g_free(identifier);

	__connman_storage_close_profile(ident, keyfile, TRUE);

	return 0;
}

static struct connman_storage device_storage = {
	.name		= "device",
	.priority	= CONNMAN_STORAGE_PRIORITY_LOW,
	.device_load	= device_load,
	.device_save	= device_save,
};

int __connman_device_init(const char *device, const char *nodevice)
{
	DBG("");

	if (device != NULL)
		device_filter = g_strsplit(device, ",", -1);

	if (nodevice != NULL)
		nodevice_filter = g_strsplit(nodevice, ",", -1);

	return connman_storage_register(&device_storage);
}

void __connman_device_cleanup(void)
{
	DBG("");

	g_strfreev(nodevice_filter);
	g_strfreev(device_filter);

	connman_storage_unregister(&device_storage);
}
