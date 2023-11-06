#!/bin/bash

docker run --rm -i -v /data/builds:/data/builds ubuntu sh -c "/data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$GITHUB_RUN_NUMBER -u"