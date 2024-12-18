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
 * housedvr_transfer.c - Transfer recordings from the feed.
 *
 * SYNOPSYS:
 *
 * This module handles transfer of new recordings from the feed server.
 * The housedvr_feed.c module is responsible for detecting recordings
 * that are available on the feeds and notify them to this modules, which
 * then decides if any are new and must be transferred.
 *
 * void housedvr_transfer_initialize (int argc, const char **argv);
 *
 *    Initialize this module.
 *
 * void housedvr_transfer_notify (const char *feed, const char *path, int size);
 *
 *    Tell this module that a specified file is available on the specified
 *    feed. The feed name is actually an URL to use as a base for the transfer.
 *
 *    The transfer does not start right away. If a transfer is necessary, it
 *    is scheduled for later, with transfers from one feed being serialized.
 *
 * void housedvr_transfer_background (time_t now);
 *
 *    The periodic function that manages the video transfers.
 *
 * int housedvr_transfer_status (char *buffer, int size);
 *
 *    A function that populates a status overview of the transfer queue in JSON.
 *
 * BUGS
 *
 * This module is dependent on the file naming and directory tree conventions
 * being the same on the local and feed servers.
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

#include "houselog.h"
#include "housediscover.h"

#include "housedvr_store.h"
#include "housedvr_store.h"

#define DEBUG if (echttp_isdebug()) printf

struct TransferFile;

#define TRANSFER_STATE_IDLE   0
#define TRANSFER_STATE_GOING  1
#define TRANSFER_STATE_DONE   2
#define TRANSFER_STATE_FAILED 3

#define TRANSFER_LIFETIME 600

struct TransferFile {
    int state;
    int size;
    int offset;
    time_t timestamp;
    char *feed;
    char *path;
    struct TransferFile *next;
};

static struct TransferFile *TransferComplete = 0;
static struct TransferFile *TransferCompleteLast = 0;
static struct TransferFile *TransferQueue = 0;
static struct TransferFile *TransferQueueLast = 0;

void housedvr_transfer_initialize (int argc, const char **argv) {

    //TBD
}

void housedvr_transfer_notify (const char *feed, const char *path, int size) {

    int i;
    time_t now = time(0);
    int cached = 0;
    int offset = 0;
    char fullpath[512];
    struct TransferFile *cursor = 0;

    // We need to make sure that the directory tree does exist.
    //
    if (strstr (path, "..")) return; // Security check: no arbitrary access.
    int fpi = snprintf (fullpath, sizeof(fullpath),
                            "%s/", housedvr_store_root());
    if (fpi >= sizeof(fullpath)) return;
    for (i = 0; path[i] > 0; ++i) {
        if (path[i] == '/') {
            fullpath[fpi] = 0;
            mkdir (fullpath, 0755);
        }
        fullpath[fpi++] = path[i];
        if (fpi >= sizeof(fullpath)) return;
    }
    fullpath[fpi] = 0;

    // Was the file already transfered recently?
    //
    for (cursor = TransferComplete; cursor; cursor = cursor->next) {
        if (!strcmp (cursor->path, path)) {
            cursor->timestamp = now; // Avoid cleaning it up for now.
            cached = 1;
            if (cursor->state == TRANSFER_STATE_FAILED) break; // Redo it.
            if (cursor->size == size) return; // Already transferred.
            if (cursor->size < size) {
                offset = cursor->size; // Transfer the additional data.
            }
            break;
        }
    }

    // Is the file queued for transfer?
    for (cursor = TransferQueue; cursor; cursor = cursor->next) {
        if (!strcmp (cursor->path, path)) {
            if (cursor->size == size) return; // Already queued.
            if (cursor->state == TRANSFER_STATE_IDLE) {
                cursor->size = size; // Update before the transfer starts.
                return;
            }
            if (cursor->size < size) {
                offset = cursor->size; // Requeue to get the additional data.
            } else {
                // The file was overwritten: retransfer.
                // Should never happen with camera recordings, BTW.
                offset = 0;
            }
            cached = 1;
            break;
        }
    }

    if (! cached) {
        // We did not find this file in our recent transfers, so the
        // next (more expensive) step is to check the local file system.
        //
        struct stat filestat;
        if (stat (fullpath, &filestat) == 0) { // File exists.
            if (filestat.st_size == size) return; // .. and is whole.
            if (size > filestat.st_size) {
                offset = filestat.st_size;
            } else {
                // The file was overwritten: retransfer.
                // Should never happen with camera recordings, BTW.
                offset = 0;
            }
        }
    }

    // The file must be new. Add it to the transfer queue.
    //
    cursor = malloc (sizeof(struct TransferFile));
    cursor->timestamp = time(0);
    cursor->feed = strdup (feed);
    cursor->path = strdup (path);
    cursor->size = size;
    cursor->offset = offset;

    cursor->state = TRANSFER_STATE_IDLE;
    cursor->next = 0;

    // Append to the queue.
    if (TransferQueueLast) {
        TransferQueueLast->next = cursor;
    } else {
        TransferQueue = cursor;
    }
    TransferQueueLast = cursor;
}

static void housedvr_transfer_free (struct TransferFile *item) {
    if (item->feed) free (item->feed);
    if (item->path) free (item->path);
    free (item);
}

static void housedvr_transfer_cleanup (time_t now) {

    struct TransferFile *cursor;
    for (cursor = TransferComplete; cursor; cursor = TransferComplete) {
        if (cursor->timestamp > now - TRANSFER_LIFETIME) break;

        TransferComplete = cursor->next;
        if (!TransferComplete) TransferCompleteLast = 0;

        housedvr_transfer_free (cursor);
    }
}

static void housedvr_transfer_end (time_t now, int status);

static int housedvr_transfer_open (struct TransferFile *item, int status) {

    char fullpath[512];
    snprintf (fullpath, sizeof(fullpath),
              "%s/%s", housedvr_store_root(), item->path);

    if (status == 206) {
        // Partial transfer, append to the existing file.
        int fd = open (fullpath, O_WRONLY);
        lseek (fd, item->offset, SEEK_SET);
        return fd;
    } else if (status == 200) {
        // Full transfer: rewrite from scratch.
        return open (fullpath, O_CREAT|O_WRONLY|O_TRUNC, 0777);
    }
    return -1;
}

static void housedvr_transfer_ready
               (void *origin, int status, char *data, int length) {

    if ((status / 100) != 2) return; // Let the response continue synchronously.

    const char *ascii = echttp_attribute_get ("Content-Length");
    if (!ascii) return; // Should never happen.
    int total = atoi(ascii);

    if (origin != TransferQueue) return; // Should never happen.

    struct TransferFile *item = TransferQueue;
    if ((!item) || (item->state != TRANSFER_STATE_GOING)) return;

    // Create the new file and write the already received data, if any.
    int fd = housedvr_transfer_open (item, status);
    if (fd < 0) return; // Should never happen,
    if (length > 0) {
        write (fd, data, length);
    }

    // Tell echttp to write the remaining portion of the data, if any.
    if (total > length) {
        echttp_transfer (fd, total-length);
    } else {
        close (fd);
    }
}

static void housedvr_transfer_complete
               (void *origin, int status, char *data, int length) {

    status = echttp_redirected("GET");
    if (!status) {
        echttp_asynchronous (housedvr_transfer_ready);
        echttp_submit (0, 0, housedvr_transfer_complete, origin);
        return;
    }

    if (origin != TransferQueue) return; // Should never happen.
    struct TransferFile *item = TransferQueue;
    if ((!item) || (item->state != TRANSFER_STATE_GOING)) return;

    if ((status / 100) == 2) {
        if (length > 0) {
            int fd = housedvr_transfer_open (item, status);
            write (fd, data, length);
            close (fd);
        }
    }

    housedvr_transfer_end (time(0), status);
}

static void housedvr_transfer_start (time_t now) {
    struct TransferFile *item = TransferQueue;
    if (!item) return;
    item->state = TRANSFER_STATE_GOING;

    char url[512];
    snprintf (url, sizeof(url), "%s/recording/%s", item->feed, item->path);
    const char *error = echttp_client ("GET", url);
    if (error) {
        houselog_trace (HOUSE_FAILURE, url, "%s", error);
        housedvr_transfer_end (now, 500);
        return;
    }
    if (item->offset > 0) {
        char rangespec[32];
        snprintf (rangespec, sizeof(rangespec), "bytes=%d-", item->offset);
        echttp_attribute_set ("Range", rangespec);
    }
    echttp_asynchronous (housedvr_transfer_ready);
    echttp_submit (0, 0, housedvr_transfer_complete, (void *)item);
}

static void housedvr_transfer_end (time_t now, int status) {

    struct TransferFile *item = TransferQueue;
    if (! item) return; // Self protection.
    if (item->state != TRANSFER_STATE_GOING) return; // Self protection.

    if (status / 100 == 2) {
        item->state = TRANSFER_STATE_DONE;
        houselog_event ("TRANSFER", "dvr", "COMPLETE",
                        "FOR FILE %s at %s", item->path, item->feed);
    } else {
        item->state = TRANSFER_STATE_FAILED;
        houselog_event ("TRANSFER", "dvr", "FAILED",
                        "CODE %d FOR FILE %s at %s",
                        status, item->path, item->feed);
    }

    // Remove from the transfer schedule queue.
    TransferQueue = TransferQueue->next;
    if (! TransferQueue) TransferQueueLast = 0;

    // Add to the transfer completed list (even if failed: cache the status).
    //
    if (TransferCompleteLast)
        TransferCompleteLast->next = item;
    else
        TransferComplete = item;
    TransferCompleteLast = item;

    if (TransferQueue) housedvr_transfer_start (now);
}

int housedvr_transfer_status (char *buffer, int size) {

    int i;
    int cursor = 0;
    struct TransferFile *queue = TransferQueue;
    const char *sep = "";

    cursor = snprintf (buffer, size, "\"queue\":[");
    if (cursor >= size) goto overflow;

    while (queue) {
        cursor += snprintf (buffer+cursor, size-cursor,
                            "%s{\"feed\":\"%s\", \"path\":\"%s\"}]",
                            sep, queue->feed, queue->path);
        if (cursor >= size) goto overflow;
        queue = queue->next;
        sep = ",";
    }
    cursor += snprintf (buffer+cursor, size-cursor, "]");
    if (cursor >= size) goto overflow;

    return cursor;

overflow:
    houselog_trace (HOUSE_FAILURE, "BUFFER", "overflow");
    buffer[0] = 0;
    return 0;
}

void housedvr_transfer_background (time_t now) {

    static time_t lastcheck = 0;
    static int lastday = 0;

    if (now == lastcheck) return;
    lastcheck = now;

    housedvr_transfer_cleanup (now);

    if (TransferQueue && (TransferQueue->state == TRANSFER_STATE_IDLE))
        housedvr_transfer_start (now);
}

