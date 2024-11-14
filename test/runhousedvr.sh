#!/bin/bash
cd `dirname $0`
current=`pwd`
storage=`dirname $current`/donotcommit
exec ../housedvr --http-debug --http-ttl=150 --dvr-store=$storage

