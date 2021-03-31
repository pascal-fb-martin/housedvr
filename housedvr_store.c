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
 * housedvr_store.c - Give access to the stored videos.
 *
 * SYNOPSYS:
 *
 * This module handles access to existing recordings. It implements a web
 * interface for querying the list of recording, structured by date, i.e.
 * the client digs through years, months and then days to figure out what
 * recordings are available for which periods. This structure is typically
 * reflected in the web user's interface.
 *
 * That module is also in charge of managing the disk space, i.e. delete
 * the oldest recording when the disk is getting too full.
 *
 * TBD: TV recording would also be organized by shows. The plan is to
 * eventually implement this feature as a filter tag.
 *
 * void housedvr_store_initialize (int argc, const char **argv);
 *
 *    Initialize this module.
 *
 * void housedvr_store_background (time_t now);
 *
 *    The periodic function that manages the video storage.
 *
 * int housedvr_store_status (char *buffer, int size);
 *
 *    A function that populates a status overview of the storage in JSON.
 *
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <echttp.h>
#include <echttp_static.h>
#include <echttp_json.h>

#include "houselog.h"
#include "housediscover.h"

#include "housedvr_store.h"

#define DEBUG if (echttp_isdebug()) printf

static int HouseDvrMaxSpace = 0; // Default is no automatic cleanup.

static const char *HouseDvrStorage = "/storage/motion/videos";
static const char *HouseDvrUri =     "/dvr/storage/videos";


static const char *dvr_store_top (const char *method, const char *uri,
                                        const char *data, int length) {
    static char buffer[2048];

    int  cursor = 0;
    const char *sep = "[";

    DIR *dir = opendir (HouseDvrStorage);
    if (dir) {
        struct dirent *p = readdir(dir);
        while (p) {
            if ((p->d_type == DT_DIR) && (isdigit(p->d_name[0]))) {
                cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor,
                                    "%s%d", sep, p->d_name);
                if (cursor >= sizeof(buffer)) goto nospace;
            }
            p = readdir(dir);
        }
        closedir (dir);
    }
    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, "]");
    if (cursor >= sizeof(buffer)) goto nospace;
    echttp_content_type_json();
    return buffer;

nospace:
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_yearly (const char *method, const char *uri,
                                           const char *data, int length) {
    static char buffer[2048];

    char path[1024];
    int  tail;
    int  cursor;
    int  month;

    const char *year = echttp_parameter_get("year");

    tail = snprintf (path, sizeof(path), "%s/%d",
                     HouseDvrStorage, atoi(year));

    cursor = snprintf (buffer, sizeof(buffer), "[false");

    for (month = 1; month <= 12; ++month) {
        const char *found = ",false";
        struct stat info;
        snprintf (path+tail, sizeof(path)-tail, "%02d", month);
        if (!stat (path, &info)) {
            if ((info.st_mode & S_IFMT) != S_IFDIR) {
                found = ",true";
            }
        }
        cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, ",%s", found);
        if (cursor >= sizeof(buffer)) goto nospace;
    }
    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, "]");
    if (cursor >= sizeof(buffer)) goto nospace;
    echttp_content_type_json();
    return buffer;

nospace:
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_monthly (const char *method, const char *uri,
                                            const char *data, int length) {

    int i;
    static char buffer[2048];

    const char *year = echttp_parameter_get("year");
    const char *month = echttp_parameter_get("month");
    int cursor = 0;

    if (!year || !month) {
        echttp_error (404, "Not Found");
        return "";
    }
    if (month[0] == '0') month += 1;

    struct tm local;
    local.tm_sec = local.tm_min = local.tm_hour = 0;
    local.tm_mday = 1;
    local.tm_mon = atoi(month)-1;
    local.tm_year = atoi(year)-1900;
    local.tm_isdst = -1;
    time_t base = mktime (&local);
    if (base < 0) {
        echttp_error (404, "Not Found");
        return "";
    }

    cursor = snprintf (buffer, sizeof(buffer), "[false");

    int referencemonth = local.tm_mon;
    struct stat info;
    char path[1024];
    int  tail = snprintf (path, sizeof(path), "%s/%s/%02d/",
                          HouseDvrStorage, year, local.tm_mon+1);

    for (i = 0; i < 31; ++i) {
        const char *found = ",false";
        snprintf (path+tail, sizeof(path)-tail, "%02d", local.tm_mday);
        if (!stat (path, &info)) {
            if ((info.st_mode & S_IFMT) == S_IFDIR) {
                found = ",true";
            }
        }
        cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, found);
        if (cursor >= sizeof(buffer)) goto nospace;

        base += 24*60*60;
        local = *localtime (&base);
        if (local.tm_mon != referencemonth) break;
    }
    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, "]");
    if (cursor >= sizeof(buffer)) goto nospace;
    echttp_content_type_json();
    return buffer;

nospace:
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_daily (const char *method, const char *uri,
                                          const char *data, int length) {
    static char buffer[655360];

    char path[1024];
    int  tail;
    char vuri[1024];
    struct stat info;

    int  cursor;
    const char *year = echttp_parameter_get("year");
    const char *month = echttp_parameter_get("month");
    const char *day = echttp_parameter_get("day");

    if (month[0] == '0') month += 1;
    if (day[0] == '0') day += 1;

    tail = snprintf (path, sizeof(path), "%s/%s/%02d/%02d",
                     HouseDvrStorage, year, atoi(month), atoi(day));
    DIR *dir = opendir (path);
    if (!dir) {
        echttp_error (404, "Not Found");
        return "";
    }
    snprintf (vuri, sizeof(vuri), "%s/%s/%02d/%02d",
              HouseDvrUri, year, atoi(month), atoi(day));

    const char *sep = "";
    cursor = snprintf (buffer, sizeof(buffer), "[");

    struct dirent *p = readdir(dir);
    while (p) {
        char name[1024];
        strncpy (name, p->d_name, sizeof(name));
        char *s = strrchr (name, '.');
        if (!s) continue;
        *(s++) = 0;

        if (!strcmp(s, "jpg")) continue;
        if (strcmp(s, "mkv") && strcmp(s, "mp4") && strcmp(s, "avi")) continue;

        char *dtime = name;
        char *src = strchr(name, '-');
        if (!src) continue;
        *(src++) = 0;
        s = strrchr(src, ':');
        if (s) *s = 0;

        snprintf (path+tail, sizeof(path)-tail, "/%s", p->d_name);
        info.st_size = 0;
        stat (path, &info);

        cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor,
                            "%s{\"src\":\"%s\",\"time\":\"%s\",\"size\":%ld"
                                ",\"uri\":\"%s/%s\"}",
                            sep, src, dtime, (long)(info.st_size),
                            vuri, p->d_name); 
        sep = ",";
        if (cursor >= sizeof(buffer)) goto nospace;

        p = readdir(dir);
    }

    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, "]");
    if (cursor >= sizeof(buffer)) goto nospace;

    closedir(dir);
    echttp_content_type_json();
    return buffer;

nospace:
    closedir(dir);
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

void housedvr_store_initialize (int argc, const char **argv) {

    int i;
    const char *max = 0;

    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-dvr-store=", argv[i], &HouseDvrStorage);
        echttp_option_match ("-dvr-clean=", argv[i], &max);
    }
    if (max) {
        HouseDvrMaxSpace = atoi(max);
    }
    echttp_route_uri ("/dvr/storage/top", dvr_store_top);
    echttp_route_uri ("/dvr/storage/yearly", dvr_store_yearly);
    echttp_route_uri ("/dvr/storage/monthly", dvr_store_monthly);
    echttp_route_uri ("/dvr/storage/daily", dvr_store_daily);
    echttp_static_route (HouseDvrUri, HouseDvrStorage);
}

int housedvr_store_status (char *buffer, int size) {

    int i;
    int cursor = 0;
    const char *prefix = "";

    struct statvfs storage;

    if (statvfs (HouseDvrStorage, &storage)) return 0;

    cursor = snprintf (buffer, size, "\"storage\":{\"path\":\"%s\", \"size\":%ld, \"free\":%ld}",
                       HouseDvrStorage,
                       (long)(storage.f_frsize * storage.f_blocks),
                       (long)(storage.f_frsize * storage.f_bavail));
    if (cursor >= size) goto overflow;

    return cursor;

overflow:
    houselog_trace (HOUSE_FAILURE, "BUFFER", "overflow");
    buffer[0] = 0;
    return 0;
}

static int housedvr_store_oldest (const char *parent) {

    int oldest = 9999;

    DIR *dir = opendir (parent);
    if (dir) {
        struct dirent *p = readdir(dir);
        while (p) {
            if ((p->d_type == DT_DIR) && (isdigit(p->d_name[0]))) {
                const char *name = p->d_name;
                int i = atoi ((name[0] == '0')?name+1:name);
                if (i < oldest) oldest = i;
            }
            p = readdir(dir);
        }
        closedir (dir);
    }
    return oldest;
}

static int housedvr_store_delete (const char *parent) {

    char path[512];
    int oldest = 9999;

    DEBUG ("delete %s\n", parent);
    DIR *dir = opendir (path);
    if (dir) {
        struct dirent *p = readdir(dir);
        while (p) {
            if (!strcmp(p->d_name, ".")) continue;
            if (!strcmp(p->d_name, "..")) continue;
            snprintf (path, sizeof(path), "%s/%s", parent, p->d_name);
            if (p->d_type == DT_DIR) {
                housedvr_store_delete (path);
            }
            remove (path);
            p = readdir(dir);
        }
        closedir (dir);
    }
    return oldest;
}

static void housedvr_store_cleanup (void) {

    char path[512];
    int oldestyear;
    int oldestmonth;
    int oldestday;

    oldestyear = housedvr_store_oldest (HouseDvrStorage);
    if (oldestyear == 9999) return; // No video.

    snprintf (path, sizeof(path), "%s/%d", HouseDvrStorage, oldestyear);
    oldestmonth = housedvr_store_oldest (path);
    if (oldestmonth == 9999) {
        housedvr_store_delete (path);
        houselog_event ("DIRECTORY", path, "DELETED", "EMPTY");
        return;
    }

    snprintf (path, sizeof(path),
              "%s/%d/%02d", HouseDvrStorage, oldestyear, oldestmonth);
    oldestday = housedvr_store_oldest (path);

    if (oldestday == 9999) {
        housedvr_store_delete (path);
        houselog_event ("DIRECTORY", path, "DELETED", "EMPTY");
        return;
    }
    snprintf (path, sizeof(path), "%s/%d/%02d/%02d",
              HouseDvrStorage, oldestyear, oldestmonth, oldestday);
    houselog_event ("DIRECTORY", path, "DELETED", "NEED DISK SPACE");
    housedvr_store_delete (path);
}

static void housedvr_store_link (const char *name, struct tm *reference) {

    char path[512];
    char target[512];

    if (!reference) return;

    snprintf (path, sizeof(path), "%s/%s", HouseDvrStorage, name);
    snprintf (target, sizeof(target), "%s/%d/%02d/%02d",
              HouseDvrStorage,
              reference->tm_year + 1900,
              reference->tm_mon + 1,
              reference->tm_mday);
    DEBUG ("Create link %s -> %s\n", path, target);
    houselog_event ("LINK", name, "TARGET", target);
    remove (path);
    symlink (target, path);
}

void housedvr_store_background (time_t now) {

    static time_t lastcheck = 0;
    static int lastday = 0;

    if (now > lastcheck + 60) {

        // Scan every minute for disk full.

        if (HouseDvrMaxSpace > 0) {
            struct statvfs storage;
            for (;;) {
                if (statvfs (HouseDvrStorage, &storage)) break;

                if (storage.f_bavail > storage.f_blocks / 10) break;
                DEBUG ("Proceeding with disk cleanup (disk %d%% full)\n",
                       ((storage.f_blocks - storage.f_bavail) * 100) / storage.f_blocks);
                housedvr_store_cleanup();
            }
        }

        struct tm *local = localtime (&now);
        if (local) {
            if (local->tm_mday != lastday) {
                housedvr_store_link ("Today", local);
                time_t yesterday = now - 86400;
                housedvr_store_link ("Yesterday", localtime (&yesterday));
            }
        }
        lastcheck = now;
    }
}

