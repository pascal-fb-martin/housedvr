# HouseDVR - A simple home web server To access CCTV recordings.
#
# Copyright 2025, Pascal Martin
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
#
# WARNING
#
# This Makefile depends on echttp and houseportal (dev) being installed.

prefix=/usr/local
SHARE=$(prefix)/share/house

INSTALL=/usr/bin/install

HAPP=housedvr
HCAT=automation
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
	gcc -g -O -o housedvr $(OBJS) -lhouseportal -lechttp -lssl -lcrypto -lmagic -lrt

# Distribution agnostic file installation -----------------------

install-ui: install-preamble
	$(INSTALL) -m 0755 -d $(DESTDIR)$(SHARE)/public/dvr
	$(INSTALL) -m 0644 public/* $(DESTDIR)$(SHARE)/public/dvr

install-runtime: install-preamble
	if [ "x$(DESTDIR)" = "x" ] ; then grep -q '^motion:' /etc/passwd || useradd -r motion -s /usr/sbin/nologin -d /var/lib/house ; fi
	if [ "x$(DESTDIR)" = "x" ] ; then $(INSTALL) -m 0755 -d $(STORE) ; chown -R motion $(STORE) ; fi
	$(INSTALL) -m 0755 -s housedvr $(DESTDIR)$(prefix)/bin
	touch $(DESTDIR)/etc/default/housedvr

install-app: install-ui install-runtime

uninstall-app:
	rm -f $(DESTDIR)$(prefix)/bin/housedvr
	rm -f $(DESTDIR)$(SHARE)/public/dvr

purge-app:

purge-config:
	rm -rf $(DESTDIR)/etc/house/dvr.config
	rm -rf $(DESTDIR)/etc/default/housedvr

# Build a private Debian package. -------------------------------

install-package: install-ui install-runtime install-systemd

debian-package: debian-package-generic

# System installation. ------------------------------------------

include $(SHARE)/install.mak

