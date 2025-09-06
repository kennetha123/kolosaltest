# Kolosal.AI Test

The shared memory communication in this system is structured around a lock-free ring buffer, defined in the SharedMem struct. The server and worker processes both map the same shared memory region, allowing them to exchange requests and responses efficiently. Requests are placed into the req array by the server, with each slot containing a task ID and data payload. Workers claim tasks by advancing the tail index and process them, writing results into their assigned slot in the resp_slots array. Each worker's status and progress are tracked in the workers array, enabling the server to monitor activity and manage worker lifecycle.

Synchronization between the server and workers is achieved using atomic variables and POSIX semaphores. Atomics ensure that updates to indices, task IDs, and status flags are thread-safe and visible across processes without traditional locking. Semaphores (referenced by SEM_REQUEST_NAME and SEM_RESPONSE_NAME) are used to signal when new requests are available or when responses are ready, preventing busy-waiting and coordinating access to shared resources.

## Build Instructions

```bash
dos2unix build.sh
./build.sh
```
first we run dos2unix to convert build.sh to LF line endings.
This script compiles both the server and worker binaries.

## Running the System

### Start the Server

```bash
./build/server
```

### Start Workers

You can start as many workers as you need (up to the configured maximum in common.h):

```bash
./build/worker
./build/worker
# ...repeat for more workers
```

## Logs Location

All logs and benchmark results are stored in the `logs/` directory.  
Each benchmark run creates a timestamped subfolder, e.g.:

```
logs/20250906_153000/
    result.txt
    server.log
    worker_1.log
    worker_2.log
    ...
```

## Dependencies

- **C++17 compiler** (e.g., g++, clang++)
- **POSIX shared memory** (`shm_open`, `mmap`)
- **POSIX semaphores** (`sem_open`, `sem_post`, `sem_wait`)
- **pthread** and **rt** libraries
- **wrk** (optional, for benchmarking)
- **curl** (optional, for manual API testing)

## Required Tools

- **gcc** or **clang** (for building)
- **cmake VERSION 3.15 or later**
- **wrk** (for performance testing: `sudo apt install wrk`)
- **curl** (for API testing: `sudo apt install curl`)

## Example Usage

```bash

# Process request
curl -X POST -d '{"message": "test"}' http://localhost:8000/process
```
or
Modify the `post_test.lua`

## Benchmarking

Run the benchmark suite to test throughput and scaling:

```bash
dos2unix benchmark.sh
./benchmark.sh
```

We need to change the `benchmark.sh` to UNIX line endings first then we can execute it.
Results are saved in the corresponding `logs/<timestamp>/result.txt` file.

## Notes

- Make sure to start the server before launching any workers.
- All logs and results are organized in the `logs/` folder for each run.