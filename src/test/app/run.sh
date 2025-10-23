#!/bin/bash
set -ex

python3 ./build_test_hooks.py --log-level debug
run-tests  --conan-2 --conan --stop-on-fail --times=1 --build ripple.app.SetHook0