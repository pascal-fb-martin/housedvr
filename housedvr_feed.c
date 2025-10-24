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
 * A video feed is eventually "erased" if its registration is not renewed.
 * A video feed is never fully removed because there might be recordings
 * associated with it.
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
 * LIMITATIONS:
 *
 * This module does not track properly when to scan individual CCTV services.
 * It only tracks when to scan all CCTV services. If there is a need to scan
 * a specific CCTV service sooner (e.g. because a new file was detected),
 * then all CCTV services will be scanned sooner.
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
#include "housedepositorstate.h"
#include "houselog.h"

#include "housedvr_feed.h"
#include "housedvr_transfer.h"

#define DEBUG if (echttp_isdebug()) printf

typedef struct {
    char   name[128];
    long long updated;
    char   adminurl[256];
    int    available;
    time_t timestamp;
} ServerRegistration;

static ServerRegistration *Servers = 0;
static int                 ServersCount = 0;
static int                 ServersSize = 0;

typedef struct {
    char  *name;
    char   server[256];
    char   url[256];
    time_t timestamp;
} FeedRegistration;

static FeedRegistration *Feeds = 0;
static int               FeedsCount = 0;
static int               FeedsSize = 0;

static const char *HouseFeedService = "cctv"; // Default is security DVR.

static time_t HouseFeedNextFullScan = 0;
static int    HouseFeedPolled = 0;
static int    HouseFeedCheckPeriod = 30;

static time_t HouseFeedStateChanged = 0;


// This function is used to stop HouseDvr when a watchdog triggers.
// Watchdogs are used to detect a situation that should never have
// happened.
//
static void crashandburn (void) {
    static char *InvalidPointer = (char *)1;
    InvalidPointer[0] = 0; // Generate a crash, by choice.
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
                                 const char *adminurl, const char *space) {

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
    }
    if (strcmp (Servers[i].adminurl, adminurl)) {
        snprintf (Servers[i].adminurl, sizeof(Servers[i].adminurl), "%s", adminurl);
    }
    Servers[i].timestamp = time(0);

    // For compatibility with the old motionCenter discovery, ignore
    // the updated value if 0 (i.e. information not available).
    if (updated) Servers[i].updated = updated;

    Servers[i].available = available;
    return new;
}

static int housedvr_feed_register (const char *name,
                                   const char *server, const char *url) {

    int i;
    int available = -1; // Probably obsolete (we don't forget old cameras).
    int new = -1;
    time_t now = time(0);

    for (i = FeedsCount-1; i >= 0; --i) {
        if (Feeds[i].name) {
            if (!strcmp (name, Feeds[i].name)) break;
        } else {
            available = i;
        }
    }
    if (i < 0) { // Feed not yet listed.
        if (available < 0) {
            if (FeedsCount >= FeedsSize) {
                FeedsSize += 16;
                Feeds = realloc (Feeds, FeedsSize * sizeof(Feeds[0]));
            }
            i = FeedsCount++;
        } else {
            i = available;
        }
        Feeds[i].name = strdup(name);
        if (!server) { // Restoring from a ghost of ancient time.
            Feeds[i].timestamp = 0;
            Feeds[i].server[0] = 0;
            Feeds[i].url[0] = 0;
            return 0;
        }
        // This is a real new camera, not even recorded as a ghost.
        HouseFeedStateChanged = now;
        new = 1;
    } else { // Feed already listed.
        if (!server) return 0; // Old news, nothing to update.
        new = 0;
    }

    if (strcmp (Feeds[i].url, url)) {
        snprintf (Feeds[i].url, sizeof(Feeds[i].url), "%s", url);
        new = 1; // This location is new.
    }
    if (strcmp (Feeds[i].server, server)) {
        snprintf (Feeds[i].server, sizeof(Feeds[i].server), "%s", server);
        new = 1; // This location is new.
    }
    Feeds[i].timestamp = now;
    return new;
}

static void housedvr_feed_refresh (const char *server) {

    int i;
    time_t now = time(0);

    for (i = FeedsCount-1; i >= 0; --i) {
        if (!strcmp (server, Feeds[i].server)) {
            Feeds[i].timestamp = now;
        }
    }

    for (i = ServersCount-1; i >= 0; --i) {
        if (!strcmp (server, Servers[i].name)) {
            Servers[i].timestamp = now;
            break;
        }
    }
}

// This function is used to prune out the feeds (cameras) no longer
// listed by a CCTV service in its status.
// The normal timeout is not used here because the service positively
// confirmed its list of feeds.
//
static void housedvr_feed_zombies (const char *server) {

    int i;
    time_t deadline = time(0) - HouseFeedCheckPeriod + 1;

    for (i = FeedsCount-1; i >= 0; --i) {
        if (!strcmp (server, Feeds[i].server)) {
            if (Feeds[i].timestamp < deadline) {
                DEBUG ("Feed %s at %s pruned\n", Feeds[i].name, Feeds[i].url);
                houselog_event ("FEED", Feeds[i].name,
                                "PRUNED", "STREAM %s", Feeds[i].url);
                Feeds[i].timestamp = 0;
                Feeds[i].server[0] = 0;
                Feeds[i].url[0] = 0;
            }
        }
    }
}

// This function is used to prune out the server and associated feeds (cameras)
// when a CCTV service stops responding.
//
static void housedvr_feed_prune (time_t now) {

    static time_t FeedWatchDog = 0;
    static time_t ServerWatchDog = 0;

    int i;
    int feedlive = 0;
    int serverlive = 0;
    time_t deadline = now - 180;

    for (i = FeedsCount-1; i >= 0; --i) {
        if (Feeds[i].timestamp > deadline) {
            feedlive += 1;
            continue;
        }
        Feeds[i].timestamp = 0;
        if (Feeds[i].server[0]) {
            // Forget where the camera came from but do not delete this
            // feed entry, as we may have stored video recordings from it.
            //
            DEBUG ("Feed %s at %s pruned\n", Feeds[i].name, Feeds[i].url);
            houselog_event
                ("FEED", Feeds[i].name, "PRUNED", "STREAM %s", Feeds[i].url);
            Feeds[i].server[0] = 0;
            Feeds[i].url[0] = 0;
        }
    }
    for (i = ServersCount-1; i >= 0; --i) {
        if (Servers[i].timestamp > deadline) {
            serverlive += 1;
            continue;
        }
        if (Servers[i].name[0]) {
            houselog_event
                ("CCTV", Servers[i].name, "PRUNED", "ADMIN %s", Servers[i].adminurl);
            Servers[i].timestamp = 0;
            Servers[i].name[0] = 0;
            Servers[i].adminurl[0] = 0;
        }
    }

    // Once HouseDvr ended up unable of discovering any other service, but
    // kept running. Still no idea how it happened. Use watchdogs to detect
    // this type of situation and die with a coredump. The Linux service
    // system is responsible for keeping the coredump and restarting HouseDvr.
    // This way there is data to analyze, and the restart helps the system
    // to "repair" itself.
    //
    if (feedlive > 0) {
        FeedWatchDog = 0;
    } else if (FeedsCount > 0) {
        if (FeedWatchDog == 0) {
            FeedWatchDog = now;
        } else if (FeedWatchDog + 300 < now) {
            crashandburn();
        }
    }
    if (serverlive > 0) {
        ServerWatchDog = 0;
    } else if (ServersCount > 0) {
        if (ServerWatchDog == 0) {
            ServerWatchDog = now;
        } else if (ServerWatchDog + 300 < now) {
            crashandburn();
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
   char *feedname = Tokens[host].value.string;

   // If the updated field was not found, just set its value to 'not known'.
   // (Some early versions of CCTV service did not report 'updated'.)
   //
   long long updated = 0;
   int updateditem = echttp_json_search (Tokens, ".updated");
   if (updateditem > 0) {
       updated = Tokens[updateditem].value.integer;
   }

   int console = echttp_json_search (Tokens, ".cctv.console");
   if (console <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no console URL");
       return;
   }
   const char *adminweb = Tokens[console].value.string;

   int available = echttp_json_search (Tokens, ".cctv.available");
   if (available >= 0 && Tokens[available].type == PARSER_STRING) {
       space = Tokens[available].value.string;
   }

   if (housedvr_feed_server (feedname, updated, adminweb, space)) {
       houselog_event ("CCTV", feedname, "ADDED", "ADMIN %s", adminweb);
   }

   int feeds = echttp_json_search (Tokens, ".cctv.feeds");
   if (feeds <= 0) {
       houselog_trace (HOUSE_FAILURE, server, "no feed data");
       return;
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
       char device[128];
       ParserToken *inner = Tokens + feeds + InnerList[i];
       if (inner->type != PARSER_STRING) continue;

       snprintf (device, sizeof(device), "%s:%s", feedname, inner->key);
       if (housedvr_feed_register (device, feedname, inner->value.string)) {

           DEBUG ("Feed %s discovered at %s\n",
                  inner->key, inner->value.string);
           houselog_event ("FEED", device,
                           "ADDED", "STREAM %s", inner->value.string);
       }
   }
   housedvr_feed_zombies (feedname); // Prune the feeds not listed here.

   // Report the recording files reported to the transfer module.
   // (Skip the files that are too recent, as these could still be written to.)
   //
   int records = echttp_json_search (Tokens, ".cctv.recordings");
   if (records <= 0) return;

   time_t now = time(0);
   for (n = Tokens[records].length - 1; n >= 0; --n) {
       char jsonpath[64];
       snprintf (jsonpath, sizeof(jsonpath), "[%d]", n);
       int file = echttp_json_search (Tokens+records, jsonpath);
       if (file <= 0) continue;
       ParserToken *fileinfo = Tokens + records + file;
       if (fileinfo->type != PARSER_ARRAY) continue;
       if (fileinfo->length < 3) continue;

       int filepath = echttp_json_search (fileinfo, "[1]");
       if (filepath <= 0) continue;
       if (fileinfo[filepath].type != PARSER_STRING) continue;
       int size = echttp_json_search (fileinfo, "[2]");
       if (size <= 0) continue;
       if (fileinfo[size].type != PARSER_INTEGER) continue;

       int stable = 0;
       if (fileinfo->length >= 4) {
           int stableitem = echttp_json_search (fileinfo, "[3]");
           if ((stableitem > 0) && (fileinfo[stableitem].type == PARSER_BOOL))
               stable = fileinfo[stableitem].value.boolean;
       } else {
           int timeitem = echttp_json_search (fileinfo, "[0]");
           if ((timeitem > 0) && (fileinfo[timeitem].type == PARSER_INTEGER)) {
               time_t ts = (time_t)(fileinfo[timeitem].value.integer);
               if (ts < now - 60) stable = 1;
           }
       }
       if (stable) {
           int r = housedvr_transfer_notify (origin,
                                             fileinfo[filepath].value.string,
                                             (int)(fileinfo[size].value.integer));
           if (!r) HouseFeedNextFullScan = now + 10; // Rush a full scan soon.
       }
   }
}

static void housedvr_feed_scan (const char *serverurl) {

   char url[256];
   snprintf (url, sizeof(url), "%s/status", serverurl);

   DEBUG ("Attempting status collection at %s\n", url);
   const char *error = echttp_client ("GET", url);
   if (error) {
       houselog_trace (HOUSE_FAILURE, serverurl, "%s", error);
       return;
   }
   echttp_submit (0, 0, housedvr_feed_scanned, (void *)serverurl);
}

static void housedvr_feed_checked
               (void *origin, int status, char *data, int length) {

   const char *serverurl = (const char *) origin;
   ParserToken tokens[32];
   int  count = 32;

   status = echttp_redirected("GET");
   if (!status) {
       echttp_submit (0, 0, housedvr_feed_checked, origin);
       return;
   }

   if (status != 200) {
       houselog_trace (HOUSE_FAILURE, serverurl, "HTTP error %d", status);
       // If the target service does not support /check, force a status scan.
       if (status == 401) housedvr_feed_scan (serverurl);
       return;
   }

   // Analyze the answer and retrieve the check stamp.
   //
   const char *error = echttp_json_parse (data, tokens, &count);
   if (error) {
       houselog_trace
           (HOUSE_FAILURE, serverurl, "JSON syntax error, %s", error);
       return;
   }
   if (count <= 0) {
       houselog_trace (HOUSE_FAILURE, serverurl, "no data");
       return;
   }

   int host = echttp_json_search (tokens, ".host");
   if (host <= 0) {
       houselog_trace (HOUSE_FAILURE, serverurl, "no hostname");
       return;
   }
   char *feedname = tokens[host].value.string;

   int stamp = echttp_json_search (tokens, ".updated");
   if (stamp <= 0) {
       houselog_trace (HOUSE_FAILURE, serverurl, "no updated field");
       return;
   }
   if (housedvr_feed_uptodate (feedname, tokens[stamp].value.integer)) {
       housedvr_feed_refresh (feedname);
   } else {
       // If the update stamp did not match the last known one, if any,
       // it is time to fetch the status of this very server.
       //
       housedvr_feed_scan (serverurl);
   }
}

static void housedvr_feed_check (const char *serverurl) {

    char url[256];

    snprintf (url, sizeof(url), "%s/check", serverurl);

    DEBUG ("Attempting discovery at %s\n", url);
    const char *error = echttp_client ("GET", url);
    if (error) {
        houselog_trace (HOUSE_FAILURE, serverurl, "%s", error);
        return;
    }
    echttp_submit (0, 0, housedvr_feed_checked, (void *)serverurl);
}

static void housedvr_feed_poll
                (const char *service, void *context, const char *serverurl) {

    time_t now = *((time_t *)context);

    if (now < HouseFeedNextFullScan) {
        housedvr_feed_check (serverurl);
    } else {
        housedvr_feed_scan (serverurl);
    }
    HouseFeedPolled += 1;
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
        char device[64];
        char feed[128];
        char devurl[256];
        int i;
        int j = 0;

        snprintf (devurl, sizeof(devurl), "http://%s/", admin);
        if (housedvr_feed_server (name, 0, devurl, space)) {
            houselog_event ("CCTV", name, "ADDED", "ADMIN %s", devurl);
        }

        for (i = 0; devices[i] > 0; ++i) {
            if (devices[i] == '+') {
                device[j] = 0;
                snprintf (feed, sizeof(feed), "%s:%s", name, device);
                snprintf (devurl, sizeof(devurl), "http://%s/%s/stream", url, device);
                if (housedvr_feed_register (feed, name, devurl))
                    houselog_event ("FEED", feed, "ADDED", "STREAM %s", devurl);
                j = 0; // Switch to the next device.
            } else {
                device[j++] = devices[i];
            }
        }
        if (j > 0) {
            device[j] = 0;
            snprintf (feed, sizeof(feed), "%s:%s", name, device);
            snprintf (devurl, sizeof(devurl), "http://%s/%s/stream", url, device);
            if (housedvr_feed_register (feed, name, devurl))
                houselog_event ("FEED", feed, "ADDED", "STREAM %s", devurl);
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
                            prefix, Servers[i].name, Servers[i].adminurl,
                            Servers[i].available,
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

        if (!Feeds[i].name) continue;

        cursor += snprintf (buffer+cursor, size-cursor,
                            "%s{\"name\":\"%s\",\"url\":\"%s\""
                                ",\"timestamp\":%lld}",
                            prefix, Feeds[i].name, Feeds[i].url,
                                (long long)(Feeds[i].timestamp));
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

static void housedvr_feed_restore (void) {

    DEBUG ("Restore from state backup\n");
    int i = 0;
    for (;;) {
        char path[128];
        snprintf (path, sizeof(path), ".cameras[%d]", i++);
        const char *name = housedepositor_state_get_string (path);
        if (!name) break;
        housedvr_feed_register (name, 0, 0);
    }
}

static int housedvr_feed_save (char *buffer, int size) {

    DEBUG ("Save %d feeds to state backup\n", FeedsCount);
    int cursor = snprintf (buffer, size, ",\"cameras\":[");
    if (cursor >= size) return 0;

    int i;
    const char *sep = "";
    for (i = 0; i < FeedsCount; ++i) {
        if (Feeds[i].name) {
            DEBUG ("Save feed %s\n", Feeds[i].name);
            cursor += snprintf (buffer+cursor, size-cursor,
                                "%s\"%s\"", sep, Feeds[i].name);
            if (cursor >= size) return 0;
            sep = ",";
        }
    }
    cursor += snprintf (buffer+cursor, size-cursor, "]");
    if (cursor >= size) return 0;
    return cursor;
}

void housedvr_feed_initialize (int argc, const char **argv) {

    int i;
    const char *period = 0;
    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-dvr-feed=", argv[i], &HouseFeedService);
        echttp_option_match ("-dvr-check=", argv[i], &period);
    }
    if (period)
        HouseFeedCheckPeriod = atoi(period);

    // Support the legacy mode (each server declares its video feeds):
    echttp_route_uri ("/dvr/source/declare", dvr_feed_declare);

    // Restore the list of known camera. Some video recording might still
    // originate from cameras that are no longer operational, so we keep
    // the full list.
    // TBD: until when?
    // 
    housedepositor_state_listen (housedvr_feed_restore);
    housedepositor_state_register (housedvr_feed_save);
}

void housedvr_feed_background (time_t now) {

    static time_t StartPeriodEnd = 0;
    static time_t NextCleanup = 0;
    static time_t NextDiscovery = 0;

    if (!now) { // This is a manual reset (force a discovery refresh)
        NextDiscovery = 0;
        return;
    }
    if (StartPeriodEnd == 0) StartPeriodEnd = now + 60;

    if (now >= NextCleanup) {
        NextCleanup = now + 10;
        housedvr_feed_prune (now);
    }

    // Delay saving the changed state until after the start period.
    // This is done so to avoid saving an incomplete state.
    //
    if (HouseFeedStateChanged > 0) {
        if (now > StartPeriodEnd) {
            housedepositor_state_changed();
            HouseFeedStateChanged = 0;
        }
    }

    // Poll every 10s for the first minute, then poll every 30 seconds (or
    // whatever was in the command line options) afterward.
    // If a full scan is overdue, force it regardless of the timing above.
    //
    // The fast start is to make the whole network recover fast from
    // an outage, when we do not know in which order the systems start.
    // Later on, there is no need to create more traffic.
    //
    if ((now < HouseFeedNextFullScan) && (now < NextDiscovery)) return;
    if (now < StartPeriodEnd)
        NextDiscovery = now + 10;
    else
        NextDiscovery = now + HouseFeedCheckPeriod;

    DEBUG ("Proceeding with discovery of service %s\n", HouseFeedService);
    HouseFeedPolled = 0;
    housediscovered (HouseFeedService, &now, housedvr_feed_poll);

    if (HouseFeedPolled) {
        if (now >= HouseFeedNextFullScan) {
            HouseFeedNextFullScan = now + 300; // Next full scan in 5 minutes.
        }
    } else if (HouseFeedNextFullScan > 0) {
        // We lost contact with all CCTV servers. Time to resync.
        HouseFeedNextFullScan = 0;
    }
}

