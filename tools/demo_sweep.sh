#!/bin/sh
for s in "$@"; do ./tools/demo_run.sh $s 100 2>&1 | head -3 | cut -c1-250; done
