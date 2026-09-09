/* sane - Scanner Access Now Easy.

   Copyright (C) 2019 Touboul Nathane
   Copyright (C) 2019 Thierry HUCHARD <thierry@ordissimo.com>

   This file is part of the SANE package.

   SANE is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3 of the License, or (at your
   option) any later version.

   SANE is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with sane; see the file COPYING.
   If not, see <https://www.gnu.org/licenses/>.

   This file implements a SANE backend for eSCL scanners.  */

#define DEBUG_DECLARE_ONLY
#include "../include/sane/config.h"

#include "escl.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>

#include "../include/sane/sanei.h"

static AvahiSimplePoll *simple_poll = NULL;

#define ESCL_DISCOVERY_PASSES 3
#define ESCL_RESOLVER_RETRIES 3

static unsigned int resolver_retries = 0;

/**
 * \fn static void resolve_callback(AvahiServiceResolver *r, AVAHI_GCC_UNUSED
 * AvahiIfIndex interface, AVAHI_GCC_UNUSED AvahiProtocol protocol,
 * AvahiResolverEvent event, const char *name,
 *                            const char *type, const char *domain, const char *host_name,
 *                            const AvahiAddress *address, uint16_t port,
 *                            AvahiStringList *txt, AvahiLookupResultFlags flags,
 *                            void *userdata)
 * \brief Callback function that will check if the selected scanner follows the escl
 *  protocol or not.
 */
static void
resolve_callback(AvahiServiceResolver *r, AvahiIfIndex interface,
                            AvahiProtocol protocol,
                            AvahiResolverEvent event,
                            const char *name,
                            const char *type,
                            const char *domain,
                            const char *host_name,
                            const AvahiAddress *address,
                            uint16_t port,
                            AvahiStringList *txt,
                            AvahiLookupResultFlags flags,
                            void *userdata)
{
    char *t = NULL;
    const char *is;
    const char *uuid;
    AvahiStringList   *s;
    AvahiClient *client = userdata;
    (void)host_name;
    (void)flags;
    assert(r);
    switch (event) {
        case AVAHI_RESOLVER_FAILURE:
            if (client && resolver_retries < ESCL_RESOLVER_RETRIES)
            {
                resolver_retries++;
                DBG (10, "eSCL resolver failed for %s; retrying (%u/%u).\n",
                     name, resolver_retries, ESCL_RESOLVER_RETRIES);
                if (avahi_service_resolver_new(client, interface, protocol,
                                               name, type, domain,
                                               AVAHI_PROTO_INET, 0,
                                               resolve_callback, client))
                {
                    avahi_service_resolver_free(r);
                    return;
                }
            }
            DBG (10, "eSCL resolver failed for %s: %s\n", name,
                 avahi_strerror(avahi_client_errno(client)));
           break;
        case AVAHI_RESOLVER_FOUND:
        {
	    char *psz_addr = ((void*)0);
            char b[128] = { 0 };
	    avahi_address_snprint(b, (sizeof(b)/sizeof(b[0]))-1, address);
#ifdef ENABLE_IPV6
            if (protocol == AVAHI_PROTO_INET6 && strchr(b, ':'))
            {
		if ( asprintf( &psz_addr, "[%s]", b ) == -1 )
		   break;
	    }
            else
#endif
	    {
		if ( asprintf( &psz_addr, "%s", b ) == -1 )
		   break;
	    }
            t = avahi_string_list_to_string(txt);
            if (t && (strstr(t, "\"rs=eSCL\"") ||
                      strstr(t, "\"rs=/eSCL\""))) {
	        s = avahi_string_list_find(txt, "is");
	        if (s && s->size > 3)
	            is = (const char*)s->text + 3;
	        else
	            is = (const char*)NULL;
	        s = avahi_string_list_find(txt, "uuid");
	        if (s && s->size > 5)
	            uuid = (const char*)s->text + 5;
	        else
	            uuid = (const char*)NULL;
                if (strstr(psz_addr, "127.0.0.1") != NULL) {
                    escl_device_add(port, name, "localhost", is, uuid, (char*)type);
                    DBG (10,"resolve_callback fix redirect [localhost]\n");
                }
                else
                    escl_device_add(port, name, psz_addr, is, uuid, (char*)type);
            }
	    avahi_free(t);
	    free(psz_addr);
	}
    }
    avahi_service_resolver_free(r);
}

/**
 * \fn static void browse_callback(AvahiServiceBrowser *b, AvahiIfIndex interface,
 * AvahiProtocol protocol, AvahiBrowserEvent event, const char *name,
 * const char *type, const char *domain,
 *                           AVAHI_GCC_UNUSED AvahiLookupResultFlags flags, void* userdata)
 * \brief Callback function that will browse tanks to 'avahi' the scanners
 * connected in network.
 */
static void
browse_callback(AvahiServiceBrowser *b, AvahiIfIndex interface,
                            AvahiProtocol protocol, AvahiBrowserEvent event,
                            const char *name, const char *type,
                            const char *domain,
                            AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
                            void* userdata)
{
    AvahiClient *c = userdata;
    assert(b);
    switch (event) {
    case AVAHI_BROWSER_FAILURE:
        avahi_simple_poll_quit(simple_poll);
        return;
    case AVAHI_BROWSER_NEW:
        if (!(avahi_service_resolver_new(c, interface, protocol, name,
                                                               type, domain,
                                                               AVAHI_PROTO_UNSPEC, 0,
                                                               resolve_callback, c)))
            break;
    case AVAHI_BROWSER_REMOVE:
        break;
    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        break;
    }
}

/**
 * \fn static void client_callback(AvahiClient *c, AvahiClientState state,
 * AVAHI_GCC_UNUSED void *userdata)
 * \brief Callback Function that quit if it doesn't find a connected scanner,
 * possible thanks the "Hello Protocol".
 *        --> Waiting for a answer by the scanner to continue the avahi process.
 */
static void
client_callback(AvahiClient *c, AvahiClientState state,
                         AVAHI_GCC_UNUSED void *userdata)
{
    assert(c);
    if (state == AVAHI_CLIENT_FAILURE)
        avahi_simple_poll_quit(simple_poll);
}

/**
 * \fn ESCL_Device *escl_devices(SANE_Status *status)
 * \brief Function that calls all the avahi functions and then, recovers the
 * connected eSCL devices.
 *        This function is called in the 'sane_get_devices' function.
 *
 * \return NULL (the eSCL devices found)
 */
ESCL_Device *
escl_devices(SANE_Status *status, SANE_Bool disable_https)
{
    AvahiClient *client = NULL;
    AvahiServiceBrowser *sb_http = NULL;
    AvahiServiceBrowser *sb_https = NULL;
    int error;

    *status = SANE_STATUS_GOOD;
    resolver_retries = 0;
    if (!(simple_poll = avahi_simple_poll_new())) {
        DBG( 10, "Failed to create simple poll object.\n");
        *status = SANE_STATUS_INVAL;
        goto fail;
    }
    client = avahi_client_new(avahi_simple_poll_get(simple_poll), 0,
                                               client_callback, NULL, &error);
    if (!client) {
        DBG( 10, "Failed to create client: %s\n", avahi_strerror(error));
        *status = SANE_STATUS_INVAL;
        goto fail;
    }
    for (int pass = 0; pass < ESCL_DISCOVERY_PASSES; pass++) {
    if (!(sb_http = avahi_service_browser_new(client, AVAHI_IF_UNSPEC,
                                                                   AVAHI_PROTO_UNSPEC, "_uscan._tcp",
                                                                   NULL, 0, browse_callback, client))) {
        DBG( 10, "Failed to create service browser: %s\n",
                              avahi_strerror(avahi_client_errno(client)));
        *status = SANE_STATUS_INVAL;
        goto fail;
    }
    if (!disable_https) {
        if (!(sb_https = avahi_service_browser_new(client, AVAHI_IF_UNSPEC,
                                                                       AVAHI_PROTO_UNSPEC,
                                                                       "_uscans._tcp", NULL, 0,
                                                                       browse_callback, client))) {
            DBG( 10, "Failed to create service browser: %s\n",
                                    avahi_strerror(avahi_client_errno(client)));
            *status = SANE_STATUS_INVAL;
            goto fail;
        }
    } else {
        DBG(10, "HTTPS device discovery disabled by configuration.\n");
    }
    for (int iteration = 0; iteration < 10; iteration++) {
        if (avahi_simple_poll_iterate(simple_poll, 100) != 0) {
            *status = SANE_STATUS_INVAL;
            goto fail;
        }
    }
    if (sb_https) {
        avahi_service_browser_free(sb_https);
        sb_https = NULL;
    }
    if (sb_http) {
        avahi_service_browser_free(sb_http);
        sb_http = NULL;
    }
    }
fail:
    if (sb_https)
        avahi_service_browser_free(sb_https);
    if (sb_http)
        avahi_service_browser_free(sb_http);
    if (client)
        avahi_client_free(client);
    if (simple_poll)
        avahi_simple_poll_free(simple_poll);
    return (NULL);
}
