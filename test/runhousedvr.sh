#!/bin/bash
cd `dirname $0`
current=`pwd`
storage=`dirname $current`/donotcommit
# exec ../housedvr --http-debug --dvr-store=$storage
exec ../housedvr --dvr-store=$storage

