#!/bin/bash

LOG_FILE="/var/log/scanner.log"
EXECUTABLE="/opt/scanner./scanner_unix "

while true; do
    echo "$(date +"%Y-%m-%d %H:%M:%S") - Starting scanner..." | tee -a $LOG_FILE
    $EXECUTABLE >> $LOG_FILE 2>&1

    echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner crashed or exited. Restarting in 5 seconds..." | tee -a $LOG_FILE
    sleep 5
done

