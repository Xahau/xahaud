#!/bin/bash

echo "START BUILDING (HOST)"

which docker 2> /dev/null 2> /dev/null
if [ "$?" -eq "1" ]
then
  echo 'Docker not found. Install it first.'
  exit 1
fi

stat .git 2> /dev/null 2> /dev/null
if [ "$?" -eq "1" ]
then
  echo 'Run this inside the source directory. (.git dir not found).'
  exit 1
fi
docker run -i --user 0:$(id -g) --rm  -v `pwd`:/io --network host ghcr.io/foobarwidget/holy-build-box-x64 /hbb_exe/activate-exec bash -x /io/build-inner.sh

echo "DONE BUILDING (HOST)"
