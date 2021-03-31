# HouseDvr
A web service to store and give access to video recordings

## Overview
This project is a specialized web site for accessing video recordings.

The goal is to eventually support both security videos (i.e. be used as
a security DVR) and TV recording (i.e. be used as a TV DVR).

A video recordings is primarily identified by when it was created, so
the file store is organized by years, months and days. A video record
is stored as two files: the video file itself, and a "title" screenshot.

Recordings are kept until the disk becomes full, in which case the oldest
files are eliminated.

The video feeds declare themselves, similar to the [MotionCenter](https://github.com/pascal-fb-martin/motionCenter) API.

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

