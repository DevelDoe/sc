#!/bin/bash
while true; do
    echo "Starting scanner..."
    ./scanner
    echo "Scanner crashed or exited. Restarting in 5 seconds..."
    sleep 5
done
