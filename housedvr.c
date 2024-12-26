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
 * housedvr.c - Main loop of the HouseDvr program.
 *
 * SYNOPSYS:
 *
 * This program stores video files comming from various sources:
 * - Security cameras with their own motion detection.
 * - Security cameras behind the motion application.
 * - Networked TV tuners, such as the HdHomeRun line from SiliconDust.
 *
 * At this time, the goal is to manage one set of video files.
 * The first source to be supported is the motion application.
 * When a service interface to HdHomeRun is completed, these video
 * recordings will probably be managed by another instance of this
 * software (to keep security and TV recording separate).
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "echttp.h"
#include "echttp_cors.h"
// #include "echttp_json.h"
#include "echttp_static.h"

#include "houseportalclient.h"
#include "housediscover.h"
#include "houselog.h"

#include "housedvr_feed.h"
#include "housedvr_store.h"
#include "housedvr_transfer.h"

static int use_houseportal = 0;
static char HostName[256];


static const char *dvr_status (const char *method, const char *uri,
                                  const char *data, int length) {
    static char buffer[65537];
    int cursor = 0;

    cursor += snprintf (buffer, sizeof(buffer),
                        "{\"host\":\"%s\",\"proxy\":\"%s\",\"timestamp\":%lld,"
                            "\"dvr\":{",
                        HostName, houseportal_server(), (long long)time(0));

    cursor += housedvr_feed_status (buffer+cursor, sizeof(buffer)-cursor);
    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, ",");
    cursor += housedvr_store_status (buffer+cursor, sizeof(buffer)-cursor);
    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, ",");
    cursor += housedvr_transfer_status (buffer+cursor, sizeof(buffer)-cursor);
    cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor, "}}");
    echttp_content_type_json ();
    return buffer;
}

static void dvr_background (int fd, int mode) {

    static time_t LastRenewal = 0;
    time_t now = time(0);

    if (use_houseportal) {
        static const char *path[] = {"dvr:/dvr"};
        if (now >= LastRenewal + 60) {
            if (LastRenewal > 0)
                houseportal_renew();
            else
                houseportal_register (echttp_port(4), path, 1);
            LastRenewal = now;
        }
    }
    housedvr_store_background(now);
    housedvr_feed_background(now);
    housedvr_transfer_background(now);

    housediscover (now);
    houselog_background (now);
}

static void dvr_protect (const char *method, const char *uri) {
    echttp_cors_protect(method, uri);
}

int main (int argc, const char **argv) {

    // These strange statements are to make sure that fds 0 to 2 are
    // reserved, since this application might output some errors.
    // 3 descriptors are wasted if 0, 1 and 2 are already open. No big deal.
    //
    open ("/dev/null", O_RDONLY);
    dup(open ("/dev/null", O_WRONLY));

    signal(SIGPIPE, SIG_IGN);

    gethostname (HostName, sizeof(HostName));

    echttp_default ("-http-service=dynamic");

    argc = echttp_open (argc, argv);
    if (echttp_dynamic_port()) {
        houseportal_initialize (argc, argv);
        use_houseportal = 1;
    }
    housediscover_initialize (argc, argv);
    houselog_initialize ("dvr", argc, argv);

    echttp_cors_allow_method("GET");
    echttp_protect (0, dvr_protect);

    housedvr_feed_initialize (argc, argv);
    housedvr_store_initialize (argc, argv);
    housedvr_transfer_initialize (argc, argv);

    echttp_route_uri ("/dvr/status", dvr_status);
    echttp_static_route ("/", "/usr/local/share/house/public");

    echttp_background (&dvr_background);

    houselog_event ("SERVICE", "dvr", "START", "ON %s", HostName);
    echttp_loop();
}

