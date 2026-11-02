#!/bin/bash

cd /home/aagam/Downloads/26CS60R61_Terminator || exit 1

echo "Launcher started at $(date)" >> /tmp/terminator_launcher.log

./terminator >> /tmp/terminator_launcher.log 2>&1

echo "Terminator exited with code: $?" >> /tmp/terminator_launcher.log
