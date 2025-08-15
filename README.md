# HouseDvr

A web service to store and give access to video recordings

## Overview

This project is a specialized web server for consolidating, and providing web access to, security video recordings.

This service implements the central server portion of what is a distributed video surveillance system:

* HouseDvr handles the "long term" storage of video recordings. It provides a single consolidated access to all security camera recordings.
* A number of CCTV services on multiple machines collect and report new recordings from motion detection systems.

The video data flows from the camera to HouseDvr. Cameras provide video streams to motion detection systems that generate video recordings. These recordings are detected by a sidekick CCTV service and transferred to the central HouseDvr storage.

The HouseDvr service automatically discovers the CCTV services, retrieves the list of cameras and periodically polls for available recordings. The HouseDvr service detect which of the available recordings have not been downloaded yet and proceeds with the transfer. Each CCTV service is expected to keep recordings for a short while as long as there is enough local storage space. There should be no issue running multiple HouseDvr services polling for the same recordings concurrently since the HouseDvr poll requests are idempotent (no impact on the state of the CCTV services).

The HouseDvr CCTV discovery is based on the [HousePortal]((https://github.com/pascal-fb-martin/houseportal) general discovery mechanism, which is itself limited to 256 services at this time. In a _large_ network, HouseDvr should be able to discover more than a hundred CCTV services.

The intent is to provide support for multiple motion detection software by implementing a custom CCTV service for each one. The purpose of the CCTV service is to provide a uniform web API to HouseDvr, regardless of the motion detection software used. At that time, there is only one CCTV service implementation: a sidekick service to the Motion software, see [HouseMotion](https://github.com/pascal-fb-martin/housemotion).

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
* -dvr-clean=NUMBER: the disk usage level (percentage) at which the oldest files are deleted. HouseDvr will delete files until the disk utilization falls below this limit.
* -dvr-feed: the name of the video feed service (reserved for future use).

Otherwise, HouseDvr retrieves the remaining of the system configuration by polling the CCTV services present.

## File storage

A video recordings is primarily identified by when it was created, so the file store is organized as a directory tree representing years, months and days. A video record is stored as two files: the video file itself, and a "title" screenshot (JPEG). The CCTV services are expected to provide the recording in that manner.

Recordings are kept by HouseDvr until the storage becomes full, at which time the oldest recordings are eliminated.

## Debian Packaging

The provided Makefile supports building private Debian packages. These are _not_ official packages:

- They do not follow all Debian policies.

- They are not built using Debian standard conventions and tools.

- The packaging is not separate from the upstream sources, and there is
  no source package.

To build a Debian package, use the `debian-package` target:

```
make debian-package
```

