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

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "connman.h"

static int ipv4_probe(struct connman_element *element)
{
	struct connman_service *service;
	struct connman_ipconfig *ipconfig;
	struct connman_element *connection;
	const char *address = NULL, *netmask = NULL, *broadcast = NULL;
	const char *peer = NULL, *nameserver = NULL, *pac = NULL;
	char *timeserver = NULL, *subnet = NULL;
	unsigned char prefixlen;

	DBG("element %p name %s", element, element->name);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_ADDRESS, &address);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_NETMASK, &netmask);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_BROADCAST, &broadcast);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_PEER, &peer);

	connman_element_get_value(element,
			CONNMAN_PROPERTY_ID_IPV4_NAMESERVER, &nameserver);
	connman_element_get_value(element,
			CONNMAN_PROPERTY_ID_IPV4_TIMESERVER, &timeserver);
	connman_element_get_value(element,
			CONNMAN_PROPERTY_ID_IPV4_PAC, &pac);

	DBG("address %s", address);
	DBG("peer %s", peer);
	DBG("netmask %s", netmask);
	DBG("broadcast %s", broadcast);

	if (address == NULL)
		return -EINVAL;

	prefixlen = __connman_ipconfig_netmask_prefix_len(netmask);

	subnet =  __connman_ipconfig_address_subnet(address, netmask);
	if (subnet != NULL) {
		connman_inet_add_network_route_with_table(element->index,
							subnet, NULL, prefixlen);
		free(subnet);
		subnet = NULL;
	}

	if ((__connman_inet_modify_address(RTM_NEWADDR,
			NLM_F_REPLACE | NLM_F_ACK, element->index,
			AF_INET, address, peer, prefixlen, broadcast)) < 0)
		DBG("address setting failed");

	service = __connman_element_get_service(element);

	if (pac != NULL)
		__connman_service_set_proxy_autoconfig(service, pac);

	if (nameserver != NULL)
		__connman_service_append_nameserver(service, nameserver);

	connman_timeserver_append(timeserver);

	connection = connman_element_create(NULL);

	connection->type    = CONNMAN_ELEMENT_TYPE_CONNECTION;
	connection->index   = element->index;
	connection->devname = connman_inet_ifname(element->index);

	ipconfig = __connman_service_get_ip6config(service);
	if (ipconfig != NULL)
		__connman_ipconfig_set_element_ipv6_gateway(
						ipconfig, connection);

	if (connman_element_register(connection, element) < 0)
		connman_element_unref(connection);

	return 0;
}

static void ipv4_remove(struct connman_element *element)
{
	const char *address = NULL, *netmask = NULL, *broadcast = NULL;
	const char *peer = NULL, *nameserver = NULL;
	char *timeserver = NULL, *subnet = NULL;
	unsigned char prefixlen;

	DBG("element %p name %s", element, element->name);

	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_ADDRESS, &address);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_NETMASK, &netmask);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_BROADCAST, &broadcast);
	connman_element_get_value(element,
				CONNMAN_PROPERTY_ID_IPV4_PEER, &peer);

	connman_element_get_value(element,
			CONNMAN_PROPERTY_ID_IPV4_NAMESERVER, &nameserver);
	connman_element_get_value(element,
			CONNMAN_PROPERTY_ID_IPV4_TIMESERVER, &timeserver);

	connman_timeserver_remove(timeserver);

	DBG("address %s", address);
	DBG("peer %s", peer);
	DBG("netmask %s", netmask);
	DBG("broadcast %s", broadcast);

	if (nameserver != NULL) {
		struct connman_service *service;

		service = __connman_element_get_service(element);
		__connman_service_remove_nameserver(service, nameserver);
	}

	prefixlen = __connman_ipconfig_netmask_prefix_len(netmask);

	subnet = __connman_ipconfig_address_subnet(address, netmask);
	if (subnet) {
		connman_inet_del_network_route_with_table(element->index,
							subnet, NULL);
		free(subnet);
		subnet = NULL;
	}

	if ((__connman_inet_modify_address(RTM_DELADDR, 0, element->index,
			AF_INET, address, peer, prefixlen, broadcast) < 0))
		DBG("address removal failed");

	connman_element_unref(element);
}

static struct connman_driver ipv4_driver = {
	.name		= "ipv4",
	.type		= CONNMAN_ELEMENT_TYPE_IPV4,
	.priority	= CONNMAN_DRIVER_PRIORITY_LOW,
	.probe		= ipv4_probe,
	.remove		= ipv4_remove,
};

int __connman_ipv4_init(void)
{
	DBG("");

	return connman_driver_register(&ipv4_driver);
}

void __connman_ipv4_cleanup(void)
{
	connman_driver_unregister(&ipv4_driver);
}
