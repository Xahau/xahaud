#!/bin/bash

docker run --rm -i -v $(pwd):/io ubuntu sh -c '/io/release-build/xahaud -u'

