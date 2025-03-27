#!/bin/bash

LOG_FILE="/var/log/scanner.log"
SCANNER_ID=$(hostname | cut -d'.' -f1)
EXECUTABLE="/opt/scanner/scanner_unix"

# Load environment variables if .env exists
ENV_FILE="/opt/scanner/.env"
if [ -f "$ENV_FILE" ]; then
    echo "$(date +"%Y-%m-%d %H:%M:%S") - Loading environment from $ENV_FILE" | tee -a $LOG_FILE
    export $(grep -v '^#' "$ENV_FILE" | xargs)
fi

echo "$(date +"%Y-%m-%d %H:%M:%S") - Starting scanner: $SCANNER_ID" | tee -a $LOG_FILE
echo "$(date +"%Y-%m-%d %H:%M:%S") - Using executable: $EXECUTABLE $SCANNER_ID" | tee -a $LOG_FILE
echo "$(date +"%Y-%m-%d %H:%M:%S") - Environment variables:" | tee -a $LOG_FILE
env | tee -a $LOG_FILE

# Initial randomized delay (5–30s)
RANDOM_START_DELAY=$((RANDOM % 26 + 5))
echo "$(date +"%Y-%m-%d %H:%M:%S") - Delaying startup by $RANDOM_START_DELAY seconds" | tee -a $LOG_FILE
sleep $RANDOM_START_DELAY

while true; do
    echo "$(date +"%Y-%m-%d %H:%M:%S") - Running command: $EXECUTABLE $SCANNER_ID" | tee -a $LOG_FILE
    $EXECUTABLE $SCANNER_ID >> $LOG_FILE 2>&1
    EXIT_CODE=$?
    echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner exited with code: $EXIT_CODE" | tee -a $LOG_FILE

    if [ $EXIT_CODE -eq 0 ]; then
        echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner exited normally. Restarting in 5 seconds..." | tee -a $LOG_FILE
        sleep 5
    else
        RANDOM_DELAY=$((RANDOM % 26 + 5)) # 5–30s delay on crash
        echo "$(date +"%Y-%m-%d %H:%M:%S") - Scanner crashed. Restarting in $RANDOM_DELAY seconds..." | tee -a $LOG_FILE
        sleep $RANDOM_DELAY
    fi
done 
