#!/usr/bin/bash

make -j2 -k ./build/x86_64/tests/object_test && sudo opcontrol --start && sudo opcontrol --reset &&  ./build/x86_64/tests/object_test && sudo opcontrol --stop && opreport -l ./build/x86_64/tests/object_test | head -n 30
