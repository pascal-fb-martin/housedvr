# HouseDvr
A web service to store and give access to video recordings

## Overview
This project is a specialized web site for accessing security video recordings.

A video recordings is primarily identified by when it was created, so
the file store is organized by years, months and days. A video record
is stored as two files: the video file itself, and a "title" screenshot.

Recordings are kept until the disk becomes full, in which case the oldest
files are eliminated.

The video feeds are maint to be automatically detected through discovery of the `cctv` service. There will be one implementation of the `cctv` service for each motion detection software supported (future).

This service also provides a compatibility API similar to the old [MotionCenter](https://github.com/pascal-fb-martin/motionCenter) API. The existing motion-join script from that project can be used with only minor modifications:
- add an `admin` HTTP parameter to provide the URL of the Motion web site.
- change the web server port number to 80.
- change the web server URI from /api/camera/declare to /dvr/source/declare.

## Installation

This service depends on the House series environment:
* Install git, icoutils, openssl (libssl-dev).
* Install [echttp](https://github.com/pascal-fb-martin/echttp)
* Install [houseportal](https://github.com/pascal-fb-martin/houseportal)
* Clone this repository.
* make rebuild
* sudo make install

## Configuration

This service takes the following specific command line options:

* -dvr-store=PATH: the full path where the video recording files are stored.
* -dvr-clean=NN: the disk usage level (percentage) at which the oldest files are being deleted. HouseDvr will delete files until the disk utilization falls below this limit.
* -dvr-feed: the name of the video feed service (reserved for future use).

