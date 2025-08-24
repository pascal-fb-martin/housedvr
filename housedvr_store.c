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
 * const char *housedvr_store_root (void);
 *
 *    Return the path to the recordings root directory. This is used to
 *    share the same default and selected value with the transfer module.
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
#include <fcntl.h>

#include <echttp.h>
#include <echttp_static.h>
#include <echttp_json.h>

#include <zip.h>

#include "houselog.h"
#include "housediscover.h"

#include "housedvr_store.h"

#define DEBUG if (echttp_isdebug()) printf

static int HouseDvrMaxSpace = 0; // Default is no automatic cleanup.

static const char *HouseDvrStorage = "/storage/motion/videos";
static const char *HouseDvrUri =     "/dvr/storage/videos";

static char WebBuffer[131072];

const char *housedvr_store_root (void) {
    return HouseDvrStorage;
}

static const char *dvr_store_top (const char *method, const char *uri,
                                  const char *data, int length) {

    int  cursor = 0;
    const char *sep = "[";

    DIR *dir = opendir (HouseDvrStorage);
    if (dir) {
        struct dirent *p = readdir(dir);
        while (p) {
            if ((p->d_type == DT_DIR) && (isdigit(p->d_name[0]))) {
                cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor,
                                    "%s%s", sep, p->d_name);
                if (cursor >= sizeof(WebBuffer)) goto nospace;
                sep = ",";
            }
            p = readdir(dir);
        }
        closedir (dir);
    }
    cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor, "]");
    if (cursor >= sizeof(WebBuffer)) goto nospace;
    echttp_content_type_json();
    return WebBuffer;

nospace:
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_yearly (const char *method, const char *uri,
                                     const char *data, int length) {

    char path[1024];
    int  tail;
    int  cursor;
    int  month;

    const char *year = echttp_parameter_get("year");

    tail = snprintf (path, sizeof(path), "%s/%d",
                     HouseDvrStorage, atoi(year));

    cursor = snprintf (WebBuffer, sizeof(WebBuffer), "[false");

    for (month = 1; month <= 12; ++month) {
        const char *found = ",false";
        struct stat info;
        snprintf (path+tail, sizeof(path)-tail, "%02d", month);
        if (!stat (path, &info)) {
            if ((info.st_mode & S_IFMT) != S_IFDIR) {
                found = ",true";
            }
        }
        cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor, "%s", found);
        if (cursor >= sizeof(WebBuffer)) goto nospace;
    }
    cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor, "]");
    if (cursor >= sizeof(WebBuffer)) goto nospace;
    echttp_content_type_json();
    return WebBuffer;

nospace:
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_monthly (const char *method, const char *uri,
                                      const char *data, int length) {

    int i;

    const char *year = echttp_parameter_get("year");
    const char *month = echttp_parameter_get("month");
    int cursor = 0;

    if (!year || !month) {
        echttp_error (404, "Not Found");
        return "";
    }
    if (month[0] == '0') month += 1;

    struct tm local;
    // The reference time must be slightly past 2 AM to avoid being fooled
    // by a daylight saving time change in the fall.
    local.tm_sec = local.tm_min = local.tm_hour = 2; // 2:02:02 AM
    local.tm_mday = 1;
    local.tm_mon = atoi(month)-1;
    local.tm_year = atoi(year)-1900;
    local.tm_isdst = -1;
    time_t base = mktime (&local);
    if (base < 0) {
        echttp_error (404, "Not Found");
        return "";
    }

    cursor = snprintf (WebBuffer, sizeof(WebBuffer), "[false");

    int referencemonth = local.tm_mon;
    struct stat info;
    char path[1024];
    int  tail = snprintf (path, sizeof(path), "%s/%s/%02d/",
                          HouseDvrStorage, year, local.tm_mon+1);

    for (i = 1; i <= 31; ++i) {
        const char *found = ",false";
        snprintf (path+tail, sizeof(path)-tail, "%02d", local.tm_mday);
        if (!stat (path, &info)) {
            if ((info.st_mode & S_IFMT) == S_IFDIR) {
                found = ",true";
            }
        }
        cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor, found);
        if (cursor >= sizeof(WebBuffer)) goto nospace;

        base += 24*60*60;
        local = *localtime (&base);
        if (local.tm_mon != referencemonth) break;
    }
    cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor, "]");
    if (cursor >= sizeof(WebBuffer)) goto nospace;
    echttp_content_type_json();
    return WebBuffer;

nospace:
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_daily (const char *method, const char *uri,
                                    const char *data, int length) {

    char path[1024];
    int  tail;
    char vuri[1024];
    struct stat info;

    int  cursor;
    const char *year = echttp_parameter_get("year");
    const char *month = echttp_parameter_get("month");
    const char *day = echttp_parameter_get("day");

    if (!year || !month || !day) {
        echttp_error (400, "Missing parameters");
        return "";
    }
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
    cursor = snprintf (WebBuffer, sizeof(WebBuffer), "[");

    for (;;) {
        char name[1024];

        struct dirent *p = readdir(dir);
        if (!p) break;
        if (p->d_name[0] == '.') continue;

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

        char image[1024];
        snprintf (image, sizeof(image), "%s", p->d_name);
        s = strrchr (image, '.');
        if (s) snprintf (s, sizeof(image)-(s-image), "%s", ".jpg");

        cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor,
                            "%s{\"src\":\"%s\",\"time\":\"%s\",\"size\":%ld"
                                ",\"video\":\"%s/%s\",\"image\":\"%s/%s\"}",
                            sep, src, dtime, (long)(info.st_size),
                            vuri, p->d_name, vuri, image); 
        sep = ",";
        if (cursor >= sizeof(WebBuffer)) goto nospace;
    }

    cursor += snprintf (WebBuffer+cursor, sizeof(WebBuffer)-cursor, "]");
    if (cursor >= sizeof(WebBuffer)) goto nospace;

    closedir(dir);
    echttp_content_type_json();
    return WebBuffer;

nospace:
    closedir(dir);
    echttp_error (413, "Out Of Space");
    return "HTTP Error 413: Out of space, response too large";
}

static const char *dvr_store_download (const char *method, const char *uri,
                                       const char *data, int length) {

    const char *year = echttp_parameter_get("year");
    const char *month = echttp_parameter_get("month");
    const char *day = echttp_parameter_get("day");
    const char *cams = echttp_parameter_get("cam");
    const char *hours = echttp_parameter_get("hour");

    if (!year || !month || !day) {
        echttp_error (400, "Missing parameters");
        return "";
    }
    if (month[0] == '0') month += 1;
    if (day[0] == '0') day += 1;

    const char *camlist[32];
    int camcount = 0;
    int starthour = 0;
    int endhour = 24;

    if (hours) {
       starthour = atoi (hours);
       hours = strchr (hours, '+');
       if (hours) endhour = atoi (hours+1);
    }

    if (cams && (cams[0] != 0)) {
        int i;
        for (i = strlen(cams)-1; i > 0; --i) {
            if (cams[i] == '+') {
                camlist[camcount++] = cams + i + 1;
                if (camcount >= 32) break;
            }
        }
        if (camcount < 32) camlist[camcount++] = cams;
    }

    char path[1024];
    int tail = snprintf (path, sizeof(path), "%s/%s/%02d/%02d",
                         HouseDvrStorage, year, atoi(month), atoi(day));
    DIR *dir = opendir (path);
    if (!dir) {
        echttp_error (404, "Not Found");
        return "";
    }

    char archivename[1024];
    snprintf (archivename, sizeof(archivename),
              "/tmp/videos-%s-%s-%s.zip", year, month, day);
    zip_t *archive = zip_open (archivename, ZIP_CREATE+ZIP_EXCL, 0);
    if (!archive) goto failure;

    int filecount = 0;
    for (;;) {
        struct dirent *p = readdir(dir);
        if (!p) break;
        if (p->d_name[0] == '.') continue;

        int t = atoi (p->d_name);
        if ((t < starthour) || (t >= endhour)) continue;

        const char *c = strchr (p->d_name, '-');
        if (!c) goto failure;
        c += 1; // Skip '-'.
        if (camcount) {
            int i;
            for (i = 0; i < camcount; ++i) {
                int j;
                int same = 1;
                const char *ref = camlist[i];
                for (j = 0; c[j] > 0; ++j) {
                    if (ref[j] == c[j]) continue;
                    if (c[j] == ':') {
                        if (ref[j] == '+' || ref[j] <= 0) break; // Match.
                    }
                    same = 0;
                    break;
                }
                if (same) break;
            }
            if (i >= camcount) continue; // Not a camera match.
        }

        snprintf (path+tail, sizeof(path)-tail, "/%s", p->d_name);
        zip_source_t *source = zip_source_file (archive, path, 0, 0);
        if (! source) goto failure;
        int index = zip_file_add (archive, p->d_name,
                                  source, ZIP_FL_ENC_UTF_8);
        if (index < 0) goto failure;
        zip_set_file_compression (archive, index, ZIP_CM_STORE, 0);
        filecount += 1;
    }
    if (filecount <= 0) goto failure;

    closedir(dir);
    dir = 0;
    zip_close (archive);
    archive = 0;

    struct stat info;
    int fd = open (archivename, O_RDONLY);
    if (fd < 0) goto failure;
    fstat (fd, &info);
    if (info.st_size > 0xffffffff) goto failure;
    echttp_transfer (fd, info.st_size);
    unlink (archivename); // So that the file will disappear once closed.
    echttp_content_type_set ("application/zip");
    return "";

failure:
    if (archive) zip_discard (archive);
    if (dir) closedir(dir);
    unlink (archivename); // Cleanup. Who cares if the file did not exist.
    echttp_error (500, "Internal error");
    return "";
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
    echttp_route_uri ("/dvr/storage/download", dvr_store_download);
    echttp_static_route (HouseDvrUri, HouseDvrStorage);
}

// Calculate storage space information (total, free, %used).
// Using the statvfs data is tricky because there are two different units:
// fragments and blocks, which can have different sizes. This code strictly
// follows the documentation in "man statvfs".
// The problem is compounded by these sizes being the same value for ext4,
// making it difficult to notice mistakes..
//
static long long housedvr_store_free (const struct statvfs *fs) {
    return (long long)(fs->f_bavail) * fs->f_bsize;
}

static long long housedvr_store_total (const struct statvfs *fs) {
    return (long long)(fs->f_blocks) * fs->f_frsize;
}

static int housedvr_store_used (const struct statvfs *fs) {

    long long total = housedvr_store_total (fs);
    return (int)(((total - housedvr_store_free(fs)) * 100) / total);
}

int housedvr_store_status (char *buffer, int size) {

    int cursor = 0;

    struct statvfs storage;

    if (statvfs (HouseDvrStorage, &storage)) return 0;

    cursor = snprintf (buffer, size, "\"storage\":[{\"path\":\"%s\", \"used\":%d, \"size\":%lld, \"free\":%lld}]",
                       HouseDvrStorage,
                       housedvr_store_used (&storage),
                       housedvr_store_total (&storage),
                       housedvr_store_free (&storage));
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
    if (!dir) return 0;

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
    if (oldest == 9999) return 0; // Found nothing.
    return oldest;
}

static void housedvr_store_delete (const char *parent) {

    char path[1024];

    DEBUG ("delete %s\n", parent);
    DIR *dir = opendir (parent);
    if (dir) {
        for (;;) {
            struct dirent *p = readdir(dir);
            if (!p) break;
            if (!strcmp(p->d_name, ".")) continue;
            if (!strcmp(p->d_name, "..")) continue;
            snprintf (path, sizeof(path), "%s/%s", parent, p->d_name);
            if (p->d_type == DT_DIR) {
                housedvr_store_delete (path);
            } else {
                unlink (path);
            }
        }
        closedir (dir);
    }
    rmdir (parent);
}

static void housedvr_store_cleanup (void) {

    char path[1024];
    int oldestyear;
    int oldestmonth;
    int oldestday;

    oldestyear = housedvr_store_oldest (HouseDvrStorage);
    if (!oldestyear) return; // No video.

    snprintf (path, sizeof(path), "%s/%d", HouseDvrStorage, oldestyear);
    oldestmonth = housedvr_store_oldest (path);
    if (!oldestmonth) {
        housedvr_store_delete (path);
        houselog_event ("DIRECTORY", path, "DELETED", "EMPTY");
        return;
    }

    snprintf (path, sizeof(path),
              "%s/%d/%02d", HouseDvrStorage, oldestyear, oldestmonth);
    oldestday = housedvr_store_oldest (path);

    if (!oldestday) {
        housedvr_store_delete (path);
        houselog_event ("DIRECTORY", path, "DELETED", "EMPTY");
        return;
    }
    snprintf (path, sizeof(path), "%s/%d/%02d/%02d",
              HouseDvrStorage, oldestyear, oldestmonth, oldestday);
    housedvr_store_delete (path);

    snprintf (path, sizeof(path), "%d/%02d/%02d",
              oldestyear, oldestmonth, oldestday);
    houselog_event ("DIRECTORY", path, "DELETED", "TO FREE DISK SPACE");
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
            // The numer of loops is limited to avoid infinit loops if the
            // filesystem cleanup fails (or it is full for some other reason).
            int i;
            for (i=0; i < 10; ++i) {
                struct statvfs storage;
                if (statvfs (HouseDvrStorage, &storage)) break;
                int used = housedvr_store_used (&storage);

                if (used <= HouseDvrMaxSpace) break;

                DEBUG ("Proceeding with disk cleanup (disk %d%% full)\n",
                       (int)(((storage.f_blocks - storage.f_bavail) * 100) / storage.f_blocks));
                houselog_event ("DISK", HouseDvrStorage, "FULL", "%d%% USED", used);
                housedvr_store_cleanup();
            }
        }

        struct tm *local = localtime (&now);
        if (local) {
            int today = local->tm_mday;
            if (today != lastday) {
                housedvr_store_link ("Today", local);
                time_t yesterday = now - 86400;
                housedvr_store_link ("Yesterday", localtime (&yesterday));
                lastday = today;
            }
        }
        lastcheck = now;
    }
}

