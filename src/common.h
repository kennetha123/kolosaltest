#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// POSIX
#define SHM_NAME "/my_shared_memory"
#define SEM_REQUEST_NAME "/sem_request"
#define SEM_RESPONSE_NAME "/sem_response"

// Ring buffer configuration
constexpr size_t RING_CAP = 256;
constexpr size_t CHUNK_SIZE = 4096;
constexpr size_t MAX_WORKERS = 16;
constexpr size_t MAX_THREADS = 64;

struct ReqSlot {
    std::atomic<uint64_t> task_id;  // 0 = empty, > 0 = active task
    uint32_t len;
    char data[CHUNK_SIZE];
    
    ReqSlot() : task_id(0), len(0) {
        data[0] = '\0';
    }
};

struct RespSlot {
    std::atomic<uint64_t> task_id;  // 0 = empty, > 0 = completed task
    std::atomic<bool> ready;
    uint32_t len;
    char data[CHUNK_SIZE];
    
    RespSlot() : task_id(0), len(0), ready(false) {
        data[0] = '\0';
    }
};

struct WorkerInfo {
    uint32_t worker_id;
    uint32_t response_slot;              // Which response slot this worker uses
    std::atomic<bool> active;
    std::atomic<uint64_t> current_task_id;
    std::atomic<int> processed_count;
    
    // Cache line padding to prevent false sharing
    char padding[64 - (sizeof(uint32_t) * 2 + sizeof(std::atomic<bool>) + 
                      sizeof(std::atomic<uint64_t>) + sizeof(std::atomic<int>))];
    
    WorkerInfo() : worker_id(0), active(false), current_task_id(0), processed_count(0), response_slot(-1) {
        memset(padding, 0, sizeof(padding));
    }
};

struct SharedMem {
    std::atomic<size_t> head;       // Server writes here
    std::atomic<size_t> tail;       // Workers claim here
    ReqSlot req[RING_CAP];          // Ring buffer slots    
    RespSlot resp_slots[MAX_WORKERS];
    
    WorkerInfo workers[MAX_WORKERS];
    std::atomic<int> active_workers;
    std::atomic<bool> shutdown_requested;
    
    // Global counters
    std::atomic<uint64_t> next_task_id;
    std::atomic<int> total_processed;
    
    SharedMem() : head(0), tail(0), active_workers(0), shutdown_requested(false), 
                  next_task_id(1), total_processed(0) {}
};