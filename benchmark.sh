echo "Performance Benchmark"
echo "=============================================="

TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
LOG_DIR="logs/$TIMESTAMP"
mkdir -p "$LOG_DIR"
RESULTS_FILE="$LOG_DIR/result.log"

# Build the project
echo "Building project..."
./build.sh > /dev/null 2>&1

if [ ! -f "./build/server" ] || [ ! -f "./build/worker" ]; then
    echo "Build failed"
    exit 1
fi

echo "Build complete"

# Function to test with N workers
test_workers() {
    local num_workers=$1
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo ""
    echo "Testing with $num_workers workers..."
    
    # Clean up any existing processes
    pkill -f "./build/server" 2>/dev/null
    pkill -f "./build/worker" 2>/dev/null
    sleep 1

    # Start server
    ./build/server > "$LOG_DIR/server.log" 2>&1 &
    SERVER_PID=$!
    sleep 2

    # Start workers
    for ((i=1; i<=num_workers; i++)); do
        ./build/worker > "$LOG_DIR/worker_${i}.log" 2>&1 &
    done
    sleep 2

    echo "Running load test..."
    echo '{"message": "test request"}' > "$LOG_DIR/test_request.json"

    wrk_result=$(timeout 30s wrk -t16 -c256 -d10s -s "post_test.lua" http://localhost:8000/process 2>/dev/null)

    if [ $? -eq 0 ] && echo "$wrk_result" | grep -q "Requests/sec"; then
        rps=$(echo "$wrk_result" | grep "Requests/sec" | awk '{print $2}')
        latency=$(echo "$wrk_result" | grep "Latency" | awk '{print $2}')
        echo "Throughput: $rps requests/sec"
        echo "Latency: $latency"
    else
        echo "Load test failed"
        rps="0"
    fi

    status=$(echo -e "GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n" | nc localhost 8000 2>/dev/null | tail -1)
    if echo "$status" | grep -q "active_workers"; then
        active=$(echo "$status" | grep -o '"active_workers": [0-9]*' | grep -o '[0-9]*')
        echo "Active workers: $active/$num_workers"
    fi

    pkill -f "./build/server" 2>/dev/null
    pkill -f "./build/worker" 2>/dev/null
    sleep 1

    echo "[$timestamp] Workers: $num_workers, Requests/sec: $rps" >> "$RESULTS_FILE"
}

if ! command -v wrk &> /dev/null; then
    echo "wrk is not installed. Installing..."
    sudo apt update && sudo apt install -y wrk
fi



echo "Benchmark Results:" > "$RESULTS_FILE"

echo "Starting performance tests..."
test_workers 1
test_workers 4
test_workers 8
test_workers 16

echo ""
echo "Results Summary:"
echo "=================="
cat "$RESULTS_FILE"

echo ""
echo "Performance Test Complete"
echo "Results saved to $RESULTS_FILE"

# Cleanup
rm -f "$LOG_DIR/test_request.json"