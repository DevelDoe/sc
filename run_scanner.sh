#!/bin/bash

LOG_FILE="/var/log/scanner.log"

# Extract scanner name from hostname (e.g., ss1, ss2, etc.)
SCANNER_ID=$(hostname | cut -d'.' -f1)  # or manually set if needed
EXECUTABLE="/opt/scanner/${SCANNER_ID}_unix"

echo "$(date +"%Y-%m-%d %H:%M:%S") - Starting scanner: $SCANNER_ID" | tee -a $LOG_FILE
echo "$(date +"%Y-%m-%d %H:%M:%S") - Using executable: $EXECUTABLE" | tee -a $LOG_FILE
echo "$(date +"%Y-%m-%d %H:%M:%S") - Environment variables:" | tee -a $LOG_FILE
env | tee -a $LOG_FILE

while true; do
    echo "$(date +"%Y-%m-%d %H:%M:%S") - Running command: $EXECUTABLE" | tee -a $LOG_FILE
    $EXECUTABLE >> $LOG_FILE 2>&1
    EXIT_CODE=$?
    echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner exited with code: $EXIT_CODE" | tee -a $LOG_FILE

    if [ $EXIT_CODE -eq 0 ]; then
        echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner exited normally. Restarting in 5 seconds..." | tee -a $LOG_FILE
        sleep 5
    else
        RANDOM_DELAY=$((RANDOM % 26 + 5))
        echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner crashed. Restarting in $RANDOM_DELAY seconds..." | tee -a $LOG_FILE
        sleep $RANDOM_DELAY
    fi
done 
