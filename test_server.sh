#!/bin/bash

echo "Starting Oat++ server..."
./build/server &
SERVER_PID=$!

# Wait for server to start
sleep 3

echo "Testing health endpoint:"
curl -s http://localhost:8000/health
echo

echo -e "\nTesting status endpoint:"
curl -s http://localhost:8000/status
echo

echo -e "\nStarting a worker..."
./build/worker &
WORKER_PID=$!

# Wait for worker to initialize
sleep 2

echo -e "\nTesting process endpoint:"
curl -s -X POST -H "Content-Type: text/plain" -d "Hello from Oat++ server!" http://localhost:8000/process
echo

echo -e "\nChecking status again:"
curl -s http://localhost:8000/status
echo

# Cleanup
echo -e "\nCleaning up..."
kill $WORKER_PID 2>/dev/null
curl -s -X POST http://localhost:8000/shutdown
sleep 2
kill $SERVER_PID 2>/dev/null

echo "Test completed!"
