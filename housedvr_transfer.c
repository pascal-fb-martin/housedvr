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

#define TRANSFER_STATE_EMPTY  0 // MUST BE ZERO
#define TRANSFER_STATE_IDLE   1
#define TRANSFER_STATE_ACTIVE 2
#define TRANSFER_STATE_DONE   3
#define TRANSFER_STATE_FAILED 4

// This module uses a queue of transfer requests. All transfers are
// serialized: there is only one transfer going at any time.
//
// The queue is implemented as a circular list (fixed array):
// * avoid heap problems (leaks, double free, dangling pointer, etc.)
// * No unbounded memory allocation.
// * keep the most recent transfer completed as a cache.
//
// New transfer requests are added using the TransferProducer cursor.
// The TransferConsumer cursor points to the next transfer to start.
// TransferProducer == TransferConsumer: the queue is empty.
// next(TransferProducer) == TransferConsumer: the queue is full.
//
// If the queue becomes full, the ignored files will be periodically
// notified again and again anyway, so no need for an infinite queue.
//
// All items from TransferConsumer up to and excluding TransferProducer are
// transfers either ongoing or idle.
//
// All items from TransferProducer up to and excluding TransferConsumer are
// either empty of transfers already executed, kept as a cache.
//
struct TransferFile {
    int state;
    int size;
    int offset;
    time_t initiated;
    char feed[128];
    char path[256];
};
static struct TransferFile *TransferQueue = 0;
static int TransferQueueSize = 0;

static int TransferConsumer = 0;
static int TransferProducer = 0;


static void crashandburn (const char *file, int line) {
    char *invalid = (char *)1;
    printf ("Invalid programm state at %s line %d\n", file, line);
    fflush (stdout);
    *invalid = 0; // Crash on purpose.
}

void housedvr_transfer_initialize (int argc, const char **argv) {
    int i;
    const char *size = 0;
    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-dvr-queue=", argv[i], &size);
    }
    TransferQueueSize = 128; // Default size.
    if (size) TransferQueueSize = atoi (size);
    if (TransferQueueSize < 16) TransferQueueSize = 16; // self protection

    TransferQueue = calloc (TransferQueueSize, sizeof(struct TransferFile));
}

int housedvr_transfer_next (int index) {
    if (++index >= TransferQueueSize) return 0;
    return index;
}

void housedvr_transfer_notify (const char *feed, const char *path, int size) {

    int cached = 0;
    struct TransferFile *cursor;

    // Was the file already transfered recently?
    //
    int index = TransferProducer;
    if (index == TransferConsumer) // If empty, all slots are "past" transfer.
        index = housedvr_transfer_next(index);
    for (; index != TransferConsumer; index = housedvr_transfer_next(index)) {

        cursor = TransferQueue + index;
        if (strcmp (cursor->path, path)) continue;

        cached = 1;
        switch (cursor->state) {
            case TRANSFER_STATE_DONE:
                if (cursor->size == size) return; // Already done.
                break;
            case TRANSFER_STATE_FAILED:
                break; // Keep looking for a successful one.
            default:
                crashandburn (__FILE__, __LINE__); // Should never happen.
        }
    }

    // Is the file already queued for transfer?
    //
    for (index = TransferConsumer;
         index != TransferProducer; index = housedvr_transfer_next(index)) {

        cursor = TransferQueue + index;
        if (strcmp (cursor->path, path)) continue;

        cached = 1;
        switch (cursor->state) {
            case TRANSFER_STATE_ACTIVE:
                if (cursor->size == size) return; // Already in progress.
                break; // Need to request the transfer again.
            case TRANSFER_STATE_IDLE:
                cursor->size = size; // Update the upcoming transfer.
                return; // Already queued.
            default:
                crashandburn (__FILE__, __LINE__); // Should never happen.
        }
    }

    // We need to make sure that the directory tree does exist, eventually.
    //
    char fullpath[512];
    if (strstr (path, "..")) return; // Security check: no arbitrary access.
    int fpi = snprintf (fullpath, sizeof(fullpath),
                            "%s/", housedvr_store_root());
    if (fpi >= sizeof(fullpath)) return;
    int i;
    for (i = 0; path[i] > 0; ++i) {
        if (path[i] == '/') {
            fullpath[fpi] = 0;
            mkdir (fullpath, 0755);
        }
        fullpath[fpi++] = path[i];
        if (fpi >= sizeof(fullpath)) return;
    }
    fullpath[fpi] = 0;

    if (! cached) {
        // We did not find this file in our recent transfers, so the
        // next (more expensive) step is to check the local file system.
        //
        struct stat filestat;
        if (stat (fullpath, &filestat) == 0) { // File exists.
            if (filestat.st_size == size) return; // .. and is whole.
        }
    }

    // The file may be new or have changed. Add it to the transfer queue, if
    // there is room.
    //
    int next = housedvr_transfer_next(TransferProducer);
    if (next == TransferConsumer) {
        // The queue is full. Ignore this file for now. The notification
        // will keep coming back anyway.
        return;
    }
    cursor = TransferQueue + TransferProducer;
    if ((cursor->state == TRANSFER_STATE_ACTIVE) ||
        (cursor->state == TRANSFER_STATE_IDLE))
        crashandburn (__FILE__, __LINE__); // Should never happen.

    snprintf (cursor->feed, sizeof(cursor->feed), "%s", feed);
    snprintf (cursor->path, sizeof(cursor->path), "%s", path);
    cursor->size = size;
    cursor->offset = 0;
    cursor->state = TRANSFER_STATE_IDLE;

    TransferProducer = next;
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

    struct TransferFile *item = TransferQueue + TransferConsumer;
    if (origin != item)
        crashandburn (__FILE__, __LINE__); // Should never happen.
    if (item->state != TRANSFER_STATE_ACTIVE)
        crashandburn (__FILE__, __LINE__); // Should never happen.

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

    struct TransferFile *item = TransferQueue + TransferConsumer;
    if (origin != item)
        crashandburn (__FILE__, __LINE__); // Should never happen.
    if (item->state != TRANSFER_STATE_ACTIVE)
        crashandburn (__FILE__, __LINE__); // Should never happen.

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

    if (TransferConsumer == TransferProducer) return; // Nothing to start.

    struct TransferFile *item = TransferQueue + TransferConsumer;
    if (item->state == TRANSFER_STATE_ACTIVE) return; // Busy.

    if (item->state != TRANSFER_STATE_IDLE)
        crashandburn (__FILE__, __LINE__); // Should never happen.

    item->state = TRANSFER_STATE_ACTIVE;
    item->initiated = now;

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

    struct TransferFile *item = TransferQueue + TransferConsumer;
    if (item->state != TRANSFER_STATE_ACTIVE)
        crashandburn (__FILE__, __LINE__); // Should never happen.

    if (status / 100 == 2) {
        char ascii[16];
        long long lapsed = (int)(now - item->initiated);
        if (lapsed > 1) {
            if (lapsed > 120)
                snprintf (ascii, sizeof(ascii), " (slow)");
            else
                snprintf (ascii, sizeof(ascii), " (%ds)", (int)lapsed);
        } else {
            ascii[0] = 0;
        }
        houselog_event ("TRANSFER", "dvr", "COMPLETE",
                        "FOR FILE %s at %s%s", item->path, item->feed, ascii);
        item->state = TRANSFER_STATE_DONE;
    } else {
        houselog_event ("TRANSFER", "dvr", "FAILED",
                        "CODE %d FOR FILE %s at %s",
                        status, item->path, item->feed);
        item->state = TRANSFER_STATE_FAILED;
    }
    TransferConsumer = housedvr_transfer_next (TransferConsumer);
    housedvr_transfer_start (now);
}

int housedvr_transfer_status (char *buffer, int size) {

    int cursor = 0;
    const char *sep = "";

    cursor = snprintf (buffer, size, "\"queue\":[");
    if (cursor >= size) goto overflow;

    // List all entries in the queue, in FIFO order, i.e. oldest first.
    //
    int index = TransferProducer;
    if (index == TransferConsumer) // If empty, all slots are "past" transfer.
        index = housedvr_transfer_next(index);
    for (; index != TransferConsumer; index = housedvr_transfer_next(index)) {

        struct TransferFile *item = TransferQueue + index;
        const char *state = 0;
        switch (item->state) {
            case TRANSFER_STATE_EMPTY:
                break;
            case TRANSFER_STATE_FAILED:
                state = ",\"state\":\"failed\"";
                break;
            case TRANSFER_STATE_DONE:
                state = ",\"state\":\"done\"";
                break;
            default:
                crashandburn (__FILE__,__LINE__);
        }
        if (!state) continue; // Ignore empty slots.

        cursor += snprintf (buffer+cursor, size-cursor,
                            "%s{\"feed\":\"%s\", \"path\":\"%s\"%s}",
                            sep, item->feed, item->path, state);
        if (cursor >= size) goto overflow;
        sep = ",";
    }
    for (index = TransferConsumer;
         index != TransferProducer; index = housedvr_transfer_next(index)) {

        struct TransferFile *item = TransferQueue + index;
        const char *state = 0;
        switch (item->state) {
            case TRANSFER_STATE_ACTIVE:
                state = ",\"state\":\"active\"";
                break;
            case TRANSFER_STATE_IDLE:
                state = "";
                break;
            default:
                crashandburn (__FILE__,__LINE__);
        }

        cursor += snprintf (buffer+cursor, size-cursor,
                            "%s{\"feed\":\"%s\", \"path\":\"%s\"%s}",
                            sep, item->feed, item->path, state);
        if (cursor >= size) goto overflow;
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

    if (now == lastcheck) return;
    lastcheck = now;
    housedvr_transfer_start (now);
}

