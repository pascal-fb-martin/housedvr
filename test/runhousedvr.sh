#!/bin/bash
cd `dirname $0`
current=`pwd`
storage=`dirname $current`/donotcommit/videos
mkdir -p $storage
# Note: the --no-local-storage option does not apply to video recording,
# only to event, trace, config and state storage.
#
# exec ../housedvr --http-debug --dvr-store=$storage --no-local-storage
exec ../housedvr --dvr-store=$storage --dvr-queue=32 --no-local-storage

