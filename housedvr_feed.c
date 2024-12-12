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

#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/time.h>

#include <echttp.h>
#include <echttp_json.h>

#include "housediscover.h"
#include "houselog.h"
#include "houselog_sensor.h"

#include "housedvr_feed.h"

#define DEBUG if (echttp_isdebug()) printf

typedef struct {
    char   name[128];
    long long updated;
    char   url[256];
    int    latest_available;
    int    latest_index;
    int    available[60];
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


static void housedvr_feed_reset_metrics (int server) {
    int i;
    for (i = 59; i >= 0; --i) Servers[server].available[i] = -1; // No data.
    Servers[server].latest_index = -1; // None.
}

static int housedvr_feed_uptodate (const char *name, long long updated) {

    int i;
    for (i = ServersCount-1; i >= 0; --i) {
        if (Servers[i].name[0]) {
            if (!strcmp (name, Servers[i].name)) {
                return (Servers[i].updated == updated);
            }
        }
    }
    return 0; // Not found, therefore no update match.
}

static int housedvr_feed_server (const char *name, long long updated,
                                 const char *url, const char *space) {

    int i;
    int new = -1;

    int available = atoi (space);
    const char *u;
    for (u = space; *u > 0; ++u) if (isalpha(*u)) break;

    if (*u == 'G') available *= 1024;  // Align on MB.
    else if (*u != 'M') available = 0; // So little left, it does not matter.

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
        housedvr_feed_reset_metrics (i);
    }
    if (strcmp (Servers[i].url, url)) {
        snprintf (Servers[i].url, sizeof(Servers[i].url), "%s", url);
    }
    Servers[i].timestamp = time(0);

    // For compatibility with the old motionCenter discovery, ignore
    // the updated value if 0 (i.e. information not available).
    if (updated) Servers[i].updated = updated;

    // We record the free space on a minute basis, but the actual sampling
    // might be less frequent. This may cause a few slots to be skipped
    // between two recorded values: these now obsolete values must be erased.
    //
    int index = (Servers[i].timestamp / 60) % 60;
    if (Servers[i].latest_index >= 0) {
        int j;
        for (j = Servers[i].latest_index + 1; j != index; ++j) {
            if (j >= 60) {
                j = 0;
                if (index == 0) break;
            }
            Servers[i].available[j] = -1;
        }
    }
    Servers[i].latest_index = index;
    Servers[i].available[index] = available;
    Servers[i].latest_available = available;
    return new;
}

static int housedvr_feed_register (const char *name, const char *url) {

    int i;
    int new = -1;

    for (i = FeedsCount-1; i >= 0; --i) {
        if (Feeds[i].name[0]) {
            if (!strcmp (name, Feeds[i].name)) break;
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
static void housedvr_feed_scanned
               (void *origin, int status, char *data, int length) {

   static ParserToken *Tokens = 0;
   static int *InnerList = 0;
   static int TokensSize = 0;

   const char *server = (const char *) origin;
   char path[256];
   int  count;
   int  i;
   const char *space = "0";

   status = echttp_redirected("GET");
   if (!status) {
       echttp_submit (0, 0, housedvr_feed_scanned, origin);
       return;
   }

   if (status != 200) {
       houselog_trace (HOUSE_FAILURE, server, "HTTP error %d", status);
       return;
   }

   // Analyze the answer and retrieve the listed feeds.
   //
   int estimated = echttp_json_estimate (data);
   if (estimated >= TokensSize) {
       TokensSize = estimated + 128;
       Tokens = realloc (Tokens, TokensSize * sizeof(*Tokens));
       InnerList = realloc (InnerList, TokensSize * sizeof(*InnerList));
   }
   count = TokensSize;

   const char *error = echttp_json_parse (data, Tokens, &count);
   if (error) {
       houselog_trace
           (HOUSE_FAILURE, server, "JSON syntax error, %s", error);
       return;
   }
   if (count <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no data");
       return;
   }

   int host = echttp_json_search (Tokens, ".host");
   if (host <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no hostname");
       return;
   }
   char *hostname = Tokens[host].value.string;

   // If the updated field was not found, just set its value to 'not known'.
   // (Some early versions of CCTV service did not report 'updated'.)
   //
   long long updated = 0;
   int updateditem = echttp_json_search (Tokens, ".updated");
   if (updateditem > 0) {
       updated = Tokens[updateditem].value.integer;
   }

   int feeds = echttp_json_search (Tokens, ".cctv.feeds");
   if (feeds <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no feed data");
       return;
   }

   int console = echttp_json_search (Tokens, ".cctv.console");
   if (console <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no console URL");
       return;
   }
   const char *adminweb = Tokens[console].value.string;

   int available = echttp_json_search (Tokens, ".cctv.available");
   if (available >= 0 && Tokens[feeds+available].type == PARSER_STRING) {
       space = Tokens[feeds+available].value.string;
   }

   if (housedvr_feed_server (hostname, updated, adminweb, space)) {
       houselog_event ("SERVER", hostname, "ADDED", "MOTION URL %s", adminweb);
   }

   int n = Tokens[feeds].length;
   if (n <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "empty feed data");
       return;
   }

   error = echttp_json_enumerate (Tokens+feeds, InnerList);
   if (error) {
       houselog_trace (HOUSE_FAILURE, path, "%s", error);
       return;
   }

   for (i = 0; i < n; ++i) {
       char feed[128];
       ParserToken *inner = Tokens + feeds + InnerList[i];
       if (inner->type != PARSER_STRING) continue;

       snprintf (feed, sizeof(feed), "%s:%s", hostname, inner->key);
       if (housedvr_feed_register (feed, inner->value.string)) {

           DEBUG ("Feed %s discovered at %s\n",
                  inner->key, inner->value.string);
           houselog_event ("FEED", feed,
                           "ADDED", "URL %s", inner->value.string);
       }
   }
}

static void housedvr_feed_scan (const char *server) {

   char url[256];
   snprintf (url, sizeof(url), "%s/status", server);

   DEBUG ("Attempting status collection at %s\n", url);
   const char *error = echttp_client ("GET", url);
   if (error) {
       houselog_trace (HOUSE_FAILURE, server, "%s", error);
       return;
   }
   echttp_submit (0, 0, housedvr_feed_scanned, (void *)server);
}

static void housedvr_feed_checked
               (void *origin, int status, char *data, int length) {

   const char *server = (const char *) origin;
   ParserToken tokens[32];
   char path[256];
   int  count = 32;
   int  i;
   const char *space = "0";

   status = echttp_redirected("GET");
   if (!status) {
       echttp_submit (0, 0, housedvr_feed_checked, origin);
       return;
   }

   if (status != 200) {
       houselog_trace (HOUSE_FAILURE, server, "HTTP error %d", status);
       // If the target service does not support /check, force a status scan.
       if (status == 401) housedvr_feed_scan (server);
       return;
   }

   // Analyze the answer and retrieve the check stamp.
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
   char *feedname = tokens[host].value.string;

   int stamp = echttp_json_search (tokens, ".updated");
   if (stamp <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no updated field");
       return;
   }
   if (housedvr_feed_uptodate (feedname, tokens[stamp].value.integer)) return;

   // If the update stamp did not match the last known one, if any,
   // it is time to fetch the status of this very server.
   //
   housedvr_feed_scan (server);
}

static void housedvr_feed_check
                (const char *service, void *context, const char *server) {

    char url[256];

    snprintf (url, sizeof(url), "%s/check", server);

    DEBUG ("Attempting discovery at %s\n", url);
    const char *error = echttp_client ("GET", url);
    if (error) {
        houselog_trace (HOUSE_FAILURE, server, "%s", error);
        return;
    }
    echttp_submit (0, 0, housedvr_feed_checked, (void *)server);
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
        char devurl[256];
        int i;
        int j = 0;

        snprintf (devurl, sizeof(devurl), "http://%s/", admin);
        if (housedvr_feed_server (name, 0, devurl, space)) {
            houselog_event ("SERVER", name, "ADDED", "MOTION URL %s", devurl);
        }

        for (i = 0; devices[i] > 0; ++i) {
            if (devices[i] == '+') {
                device[j] = 0;
                snprintf (feed, sizeof(feed), "%s:%s", name, device);
                snprintf (devurl, sizeof(devurl), "http://%s/%s/stream", url, device);
                if (housedvr_feed_register (feed, devurl))
                    houselog_event ("FEED", feed, "ADDED", "URL %s", devurl);
                j = 0; // Switch to the next device.
            } else {
                device[j++] = devices[i];
            }
        }
        if (j > 0) {
            device[j] = 0;
            snprintf (feed, sizeof(feed), "%s:%s", name, device);
            snprintf (devurl, sizeof(devurl), "http://%s/%s/stream", url, device);
            if (housedvr_feed_register (feed, devurl))
                houselog_event ("FEED", feed, "ADDED", "URL %s", devurl);
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
                                ",\"space\":\"%d MB\",\"timestamp\":%ld}",
                            prefix, Servers[i].name, Servers[i].url,
                            Servers[i].latest_available,
                            (long)(Servers[i].timestamp));
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

static void housedvr_feed_background_sensor (time_t now) {

    static time_t LastHour = 0;

    if ((now / 3600) == LastHour) return; // Hourly report.
    LastHour = (now / 3600);

    int s;
    int donesomething = 0;

    for (s = 0; s < ServersCount; ++s) {
        int i;
        int available = INT_MAX;
        for (i = 59; i >= 0; --i) {
            if (available > Servers[s].available[i]) {
                if (Servers[s].available[i] >= 0)
                    available = Servers[s].available[i];
            }
        }
        if (available < INT_MAX) {
            struct timeval timestamp;
            timestamp.tv_sec = now - (now % 60);
            timestamp.tv_usec = 0;
            houselog_sensor_numeric (&timestamp, Servers[s].name,
                                     "videos.free", available, "MB");
            housedvr_feed_reset_metrics (s);
            donesomething = 1;
        }
    }
    if (donesomething) houselog_sensor_flush ();
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
    static time_t latestcleanup = 0;
    static time_t latestdiscovery = 0;

    if (!now) { // This is a manual reset (force a discovery refresh)
        starting = 0;
        latestdiscovery = 0;
        return;
    }
    if (starting == 0) starting = now;

    // Scan every 15s for the first 2 minutes, then scan every minute.
    // The fast start is to make the whole network recover fast from
    // an outage, when we do not know in which order the systems start.
    // Later on, there is no need to create more traffic.
    // The timing of the pruning mechanism is not impacted.
    //
    if (now <= latestcleanup + 15) return;
    latestcleanup = now;

    housedvr_feed_prune (now);
    housedvr_feed_background_sensor (now);

    if (now <= latestdiscovery + 60 && now >= starting + 120) return;
    latestdiscovery = now;

    DEBUG ("Proceeding with discovery of service %s\n", HouseFeedService);
    housediscovered (HouseFeedService, "check", housedvr_feed_check);
}

