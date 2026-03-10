#!/bin/bash
# Tail today's remote device log file.
# @@LOGDIR@@ is substituted by PhoneUtils.ps1 before this script is executed.

logdir="@@LOGDIR@@"
logfile="$logdir/$(date +%Y-%m-%d).log"

if [ ! -f "$logfile" ]; then
    echo "Waiting for $logfile..."
    while [ ! -f "$logfile" ]; do
        sleep 5
        logfile="$logdir/$(date +%Y-%m-%d).log"
    done
fi

tail -n +1 -f "$logfile"
