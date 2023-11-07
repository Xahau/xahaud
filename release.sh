#!/bin/bash

# Define the build identifier and git abbreviation
GITHUB_BRANCH=$1
GIT_SHA=$2

# Create a temporary container
container_id=$(docker create transia/xahaud-binary:$GIT_SHA)

# Copy the xahaud and release.info files from the container to the host
docker cp $container_id:/io/release-build/xahaud /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$GITHUB_BRANCH+$GITHUB_RUN_NUMBER
docker cp $container_id:/io/release-build/release.info /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$GITHUB_BRANCH+$GITHUB_RUN_NUMBER.releaseinfo

# Remove the temporary container
docker rm $container_id

# Print the published build
echo "Published build to: http://build.xahau.tech/"
echo $(date +%Y).$(date +%-m).$(date +%-d)-$GITHUB_BRANCH+$GITHUB_RUN_NUMBER