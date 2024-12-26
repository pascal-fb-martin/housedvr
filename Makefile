# HouseDVR - A simple home web server To access CCTV recordings.
#
# Copyright 2024, Pascal Martin
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.

HAPP=housedvr
HROOT=/usr/local
SHARE=$(HROOT)/share/house
STORE=/storage/motion/videos

# Application build. --------------------------------------------

OBJS= housedvr_transfer.o housedvr_store.o housedvr_feed.o housedvr.o
LIBOJS=

all: housedvr

clean:
	rm -f *.o *.a housedvr

rebuild: clean all

%.o: %.c
	gcc -c -Wall -g -O -o $@ $<

housedvr: $(OBJS)
	gcc -g -O -o housedvr $(OBJS) -lhouseportal -lechttp -lssl -lcrypto -lgpiod -lrt

# Distribution agnostic file installation -----------------------

install-ui:
	mkdir -p $(SHARE)/public/dvr
	chmod 755 $(SHARE) $(SHARE)/public $(SHARE)/public/dvr
	cp public/* $(SHARE)/public/dvr
	chown root:root $(SHARE)/public/dvr/*
	chmod 644 $(SHARE)/public/dvr/*

install-app: install-ui
	grep -q '^motion:' /etc/passwd || useradd -r motion -s /usr/sbin/nologin -d /var/lib/house
	mkdir -p $(STORE)
	chown -R motion $(STORE)
	mkdir -p $(HROOT)/bin
	mkdir -p /var/lib/house
	mkdir -p /etc/house
	rm -f $(HROOT)/bin/housedvr
	cp housedvr $(HROOT)/bin
	chown root:root $(HROOT)/bin/housedvr
	chmod 755 $(HROOT)/bin/housedvr
	touch /etc/default/housedvr

uninstall-app:
	rm -f $(HROOT)/bin/housedvr
	rm -f $(SHARE)/public/dvr

purge-app:

purge-config:
	rm -rf /etc/house/dvr.config /etc/default/housedvr

# System installation. ------------------------------------------

include $(SHARE)/install.mak

