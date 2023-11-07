#!/bin/bash
set -e

# Create a temporary container
container_id=$(docker create transia/xahaud-binary:$1)

# Copy the xahaud and release.info files from the container to the host
docker cp $container_id:/io/release-build/xahaud /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$2
docker cp $container_id:/io/release-build/release.info /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$2.releaseinfo

# Remove the temporary container
docker rm $container_id

# Print the published build
echo "Published build to: http://build.xahau.tech/"
echo $(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$2