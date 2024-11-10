/* HouseDvr - a web server to store videos files from video sources.
 *
 * Copyright 2020, Pascal Martin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *
 * housedvr_feed.c - Maintain the list of registered video feeds.
 *
 * SYNOPSYS:
 *
 * This module handles recording video feed registrations. The servers
 * attached to the feeds are responsible for the periodic registration.
 *
 * This module is not configured by the user: it learns about video feeds
 * on its own.
 *
 * A video feed is eventually removed if its registration is not renewed.
 *
 * TBD: this module will eventually actively discover feed servers, and
 * individual feeds, using the HousePortal mechanism for service discovery.
 * The intent is to make the system more flexible, with less static
 * configuration (no need to hard code one storge server address).
 * Some portions of the code are meant to support this new mechanism,
 * while other portions support the legacy mode (see motionCenter project).
 *
 * void housedvr_feed_initialize (int argc, const char **argv);
 *
 *    Initialize this module.
 *
 * int housedvr_feed_status (char *buffer, int size);
 *
 *    Return a JSON string that represents the status of the known feeds.
 *    Note that this JSON string is designed to be part of a more global
 *    HouseLight status, not a status standing on its own.
 *
 * void housedvr_feed_background (time_t now);
 *
 *    The periodic function that runs the feed registration.
 *
 * int housedvr_feed_list (char *buffer, int size);
 *
 *    A function that populates a complete list of feeds in JSON.
 *
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <echttp.h>
#include <echttp_json.h>

#include "houselog.h"
#include "housediscover.h"

#include "housedvr_feed.h"

#define DEBUG if (echttp_isdebug()) printf

typedef struct {
    char   name[128];
    char   url[256];
    char   space[16];
    time_t timestamp;
} ServerRegistration;

static ServerRegistration *Servers = 0;
static int                 ServersCount = 0;
static int                 ServersSize = 0;

typedef struct {
    char  *name;
    char   url[256];
    time_t timestamp;
} FeedRegistration;

static FeedRegistration *Feeds = 0;
static int               FeedsCount = 0;
static int               FeedsSize = 0;

static const char *HouseFeedService = "cctv"; // Default is security DVR.


static int housedvr_feed_server (const char *name,
                                 const char *url, const char *space) {

    int i;
    int new = -1;

    for (i = ServersCount-1; i >= 0; --i) {
        if (Servers[i].name[0]) {
            if (!strcmp (name, Servers[i].name)) break;
        } else {
            new = i;
        }
    }
    if (i >= 0) {
        new = 0;
    } else {
        if (new < 0) {
            if (ServersCount >= ServersSize) {
                ServersSize += 16;
                Servers = realloc (Servers, ServersSize * sizeof(Servers[0]));
            }
            i = ServersCount++;
        } else {
            i = new;
        }
        snprintf (Servers[i].name, sizeof(Servers[i].name), "%s", name);
        new = 1;
    }
    if (strcmp (Servers[i].url, url)) {
        snprintf (Servers[i].url, sizeof(Servers[i].url), "%s", url);
    }
    if (strcmp (Servers[i].space, space)) {
        snprintf (Servers[i].space, sizeof(Servers[i].space), "%s", space);
    }
    Servers[i].timestamp = time(0);
    return new;
}

static int housedvr_feed_register (const char *name, const char *url) {

    int i;
    int new = -1;

    for (i = FeedsCount-1; i >= 0; --i) {
        if (Feeds[i].url[0]) {
            if (!strcmp (url, Feeds[i].url)) break;
        } else {
            new = i;
        }
    }
    if (i < 0) {
        if (new < 0) {
            if (FeedsCount >= FeedsSize) {
                FeedsSize += 16;
                Feeds = realloc (Feeds, FeedsSize * sizeof(Feeds[0]));
            }
            i = FeedsCount++;
        } else {
            i = new;
        }
        Feeds[i].name = strdup(name);
        new = 1;
    } else {
        new = 0;
    }
    if (strcmp (Feeds[i].url, url)) {
        snprintf (Feeds[i].url, sizeof(Feeds[i].url), "%s", url);
    }
    Feeds[i].timestamp = time(0);
    return new;
}

static void housedvr_feed_prune (time_t now) {

    int i;
    time_t deadline = now - 180;

    for (i = FeedsCount-1; i >= 0; --i) {
        if (Feeds[i].timestamp > deadline) continue;
        Feeds[i].timestamp = 0;
        if (Feeds[i].name) {
            DEBUG ("Feed %s at %s pruned\n", Feeds[i].name, Feeds[i].url);
            houselog_event
                ("FEED", Feeds[i].name, "PRUNE", "AT %s", Feeds[i].url);
            free (Feeds[i].name);
            Feeds[i].name = 0;
            Feeds[i].url[0] = 0;
        }
    }
    for (i = ServersCount-1; i >= 0; --i) {
        if (Servers[i].timestamp > deadline) continue;
        if (Servers[i].name) {
            Servers[i].timestamp = 0;
            Servers[i].name[0] = 0;
            Servers[i].url[0] = 0;
        }
    }
}

// HousePortal based feed discovery: retrieve the video feed services,
// then query each one. This function is the video feed service's response.
//
static void housedvr_feed_discovered
               (void *origin, int status, char *data, int length) {

   const char *server = (const char *) origin;
   ParserToken tokens[128];
   int  innerlist[128];
   char path[256];
   int  count = 128;
   int  i;
   const char *space = "0";

   status = echttp_redirected("GET");
   if (!status) {
       echttp_submit (0, 0, housedvr_feed_discovered, origin);
       return;
   }

   if (status != 200) {
       houselog_trace (HOUSE_FAILURE, server, "HTTP error %d", status);
       return;
   }

   // Analyze the answer and retrieve the listed feeds.
   //
   const char *error = echttp_json_parse (data, tokens, &count);
   if (error) {
       houselog_trace
           (HOUSE_FAILURE, server, "JSON syntax error, %s", error);
       return;
   }
   if (count <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no data");
       return;
   }

   int host = echttp_json_search (tokens, ".host");
   if (host <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no hostname");
       return;
   }
   char *hostname = tokens[host].value.string;

   int feeds = echttp_json_search (tokens, ".cctv.feeds");
   if (feeds <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no feed data");
       return;
   }

   int console = echttp_json_search (tokens, ".cctv.console");
   if (console <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no console URL");
       return;
   }
   const char *adminweb = tokens[console].value.string;

   int available = echttp_json_search (tokens, ".cctv.available");
   if (available >= 0 && tokens[feeds+available].type == PARSER_STRING) {
       space = tokens[feeds+available].value.string;
   }

   if (housedvr_feed_server (hostname, adminweb, space)) {
       houselog_event ("SERVER", hostname, "ADDED", "MOTION URL %s", adminweb);
   }

   int n = tokens[feeds].length;
   if (n <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "empty feed data");
       return;
   }

   error = echttp_json_enumerate (tokens+feeds, innerlist);
   if (error) {
       houselog_trace (HOUSE_FAILURE, path, "%s", error);
       return;
   }

   for (i = 0; i < n; ++i) {
       ParserToken *inner = tokens + feeds + innerlist[i];
       if (inner->type != PARSER_STRING) continue;

       if (housedvr_feed_register (inner->key, inner->value.string)) {

           DEBUG ("Feed %s discovered at %s\n",
                  inner->key, inner->value.string);
           houselog_event ("FEED", inner->key,
                           "ADDED", "URL %s", inner->value.string);
       }
   }
}

static void housedvr_feed_scan
                (const char *service, void *context, const char *server) {

    char url[256];

    snprintf (url, sizeof(url), "%s/status", server);

    DEBUG ("Attempting discovery at %s\n", url);
    const char *error = echttp_client ("GET", url);
    if (error) {
        houselog_trace (HOUSE_FAILURE, server, "%s", error);
        return;
    }
    echttp_submit (0, 0, housedvr_feed_discovered, (void *)server);
}

// LEGACY feed discovery: the video feed servers periodically call the DVR
// server to re-register themselves.
//
static const char *dvr_feed_declare (const char *method,
                                             const char *uri,
                                             const char *data, int length) {

    const char *name = echttp_parameter_get("name");
    const char *admin = echttp_parameter_get("admin");
    const char *url = echttp_parameter_get("url");
    const char *space = echttp_parameter_get("available");
    const char *devices = echttp_parameter_get("devices");

    if (!admin) admin = url;

    if (name && url && space) {
        char device[128];
        char feed[128];
        char camurl[128];
        int i;
        int j = 0;

        if (housedvr_feed_server (name, admin, space))
            houselog_event ("SERVER", name, "ADDED", "MOTION URL %s", admin);

        for (i = 0; devices[i] > 0; ++i) {
            if (devices[i] == '+') {
                device[j] = 0;
                snprintf (feed, sizeof(feed), "%s:%s", name, device);
                snprintf (camurl, sizeof(camurl), "%s/%s/stream", url, device);
                if (housedvr_feed_register (feed, camurl))
                    houselog_event ("FEED", feed, "ADDED", "URL %s", camurl);
                j = 0; // Switch to the next device.
            } else {
                device[j++] = devices[i];
            }
        }
        if (j > 0) {
            device[j] = 0;
            snprintf (feed, sizeof(feed), "%s:%s", name, device);
            snprintf (camurl, sizeof(camurl), "%s/%s/stream", url, device);
            if (housedvr_feed_register (feed, camurl))
                houselog_event ("FEED", feed, "ADDED", "URL %s", camurl);
        }
    }

    return "";
}

int housedvr_feed_status (char *buffer, int size) {

    int i;
    int cursor = 0;
    const char *prefix = "";

    cursor = snprintf (buffer, size, "\"servers\":[");
    if (cursor >= size) goto overflow;

    for (i = 0; i < ServersCount; ++i) {
 
        if (!Servers[i].timestamp) continue;

        cursor += snprintf (buffer+cursor, size-cursor,
                            "%s{\"name\":\"%s\",\"url\":\"%s\""
                                ",\"space\":\"%s\",\"timestamp\":%ld}",
                            prefix, Servers[i].name, Servers[i].url,
                                Servers[i].space, (long)(Servers[i].timestamp));
        if (cursor >= size) goto overflow;
        prefix = ",";
    }
    cursor += snprintf (buffer+cursor, size-cursor, "]");
    if (cursor >= size) goto overflow;

    cursor += snprintf (buffer+cursor, size-cursor, ",\"feed\":[");
    if (cursor >= size) goto overflow;
    prefix = "";

    for (i = 0; i < FeedsCount; ++i) {

        if (!Feeds[i].timestamp) continue;

        cursor += snprintf (buffer+cursor, size-cursor,
                            "%s{\"name\":\"%s\",\"url\":\"%s\""
                                ",\"timestamp\":%d}",
                            prefix, Feeds[i].name, Feeds[i].url,
                                Feeds[i].timestamp);
        if (cursor >= size) goto overflow;
        prefix = ",";
    }

    cursor += snprintf (buffer+cursor, size-cursor, "]");
    if (cursor >= size) goto overflow;

    return cursor;

overflow:
    houselog_trace (HOUSE_FAILURE, "BUFFER", "overflow");
    echttp_error (413, "Payload too large");
    buffer[0] = 0;
    return 0;
}

void housedvr_feed_initialize (int argc, const char **argv) {

    int i;
    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-dvr-feed=", argv[i], &HouseFeedService);
    }
    // Support the legacy mode (each server declares its video feeds):
    echttp_route_uri ("/dvr/source/declare", dvr_feed_declare);
}

void housedvr_feed_background (time_t now) {

    static time_t starting = 0;
    static time_t latestdiscovery = 0;

    if (!now) { // This is a manual reset (force a discovery refresh)
        starting = 0;
        latestdiscovery = 0;
        return;
    }
    if (starting == 0) starting = now;

    // Scan every 15s for the first 2 minutes, then slow down to every minute.
    // The fast start is to make the whole network recover fast from
    // an outage, when we do not know in which order the systems start.
    // Later on, there is no need to create more traffic.
    // The timing of the pruning mechanism is not impacted.
    //
    if (now <= latestdiscovery + 15) return;
    housedvr_feed_prune (now);
    if (now <= latestdiscovery + 1800 && now >= starting + 60) return;
    latestdiscovery = now;

    DEBUG ("Proceeding with discovery of service %s\n", HouseFeedService);
    housediscovered (HouseFeedService, 0, housedvr_feed_scan);
}

