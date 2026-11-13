#!/bin/sh
# Host test of main/urfconv.c: converts each captured URF job to a PBM page next to it in /tmp.
# Usage: firmware/test/run.sh path/to/job.urf   (run from firmware/)
set -e
cc -O2 -Itest -Imain -o /tmp/urfconv_test test/urfconv_host_test.c main/urfconv.c -lm
for f in "$@"; do /tmp/urfconv_test "$f" "/tmp/$(basename "$f" .urf).pbm"; done
