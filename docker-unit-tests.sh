#!/bin/bash

echo "Mounting $(pwd)/io in ubuntu and running unit tests"
docker run --rm -i -v $(pwd):/io ubuntu sh -c '/io/release-build/xahaud -u'
