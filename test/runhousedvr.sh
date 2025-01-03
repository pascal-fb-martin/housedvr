#!/bin/bash
cd `dirname $0`
current=`pwd`
storage=`dirname $current`/donotcommit/videos
mkdir -p $storage
# exec ../housedvr --http-debug --dvr-store=$storage
exec ../housedvr --dvr-store=$storage --dvr-queue=32

