#include "common.h"
#include <iostream>
#include <cstring>
#include <string>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>

int registerWorker(SharedMem* shared_mem) {
    // Find an available worker slot
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (shared_mem->workers[i].worker_id == 0) {
            shared_mem->workers[i].worker_id = i + 1;  // 1-based worker ID
            shared_mem->workers[i].response_slot = i;   // Each worker gets its own response slot
            shared_mem->workers[i].active.store(true);
            shared_mem->workers[i].current_task_id.store(0);
            shared_mem->workers[i].processed_count.store(0);
            
            // Increment active worker count
            shared_mem->active_workers.fetch_add(1);
            
            return i + 1;
        }
    }
    return -1;
}

ReqSlot* claimNextRequest(SharedMem* shared_mem) {
    // Limit retry attempts to prevent excessive spinning
    const int MAX_RETRIES = 10;
    int retry_count = 0;
    
    while (retry_count < MAX_RETRIES) {
        size_t current_tail = shared_mem->tail.load(std::memory_order_acquire);
        size_t current_head = shared_mem->head.load(std::memory_order_acquire);
        
        if (current_tail == current_head) {
            return nullptr; // No work available
        }
        
        // Try to claim the next slot
        size_t next_tail = (current_tail + 1) % RING_CAP;
        if (shared_mem->tail.compare_exchange_weak(current_tail, next_tail, std::memory_order_acq_rel)) {
            // Successfully claimed slot at current_tail
            ReqSlot* slot = &shared_mem->req[current_tail];
            
            // Wait for the slot to be fully written by server (with timeout)
            uint64_t task_id;
            int wait_cycles = 0;
            while ((task_id = slot->task_id.load(std::memory_order_acquire)) == 0) {
                if (++wait_cycles > 1000) {
                    // Slot not ready, back out of claim
                    shared_mem->tail.store(current_tail, std::memory_order_release);
                    return nullptr;
                }
                std::this_thread::sleep_for(std::chrono::nanoseconds(100));
            }
            
            return slot;
        }
        
        retry_count++;
        // Small backoff to reduce contention
        if (retry_count > 5) {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
    
    return nullptr; // Failed to claim after max retries
}

std::string processRequest(const std::string& input, int worker_id) {
    std::string message_content;
    
    // Simple JSON parsing to extract "message" value
    size_t start = input.find("\"message\"");
    if (start != std::string::npos) {
        start = input.find(":", start);
        if (start != std::string::npos) {
            start = input.find("\"", start);
            if (start != std::string::npos) {
                start++; // Skip opening quote
                size_t end = input.find("\"", start);
                if (end != std::string::npos) {
                    message_content = input.substr(start, end - start);
                }
            }
        }
    }
    
    // If no message found, use the whole input
    if (message_content.empty()) {
        message_content = input;
    }
    
    std::string output = message_content;
    std::reverse(output.begin(), output.end());
    std::cout << "[WORKER " << worker_id << "] processed: " << output << std::endl;
    return output;
}

void publishResponse(SharedMem* shared_mem, int worker_id, uint64_t task_id, const std::string& response) {
    int slot_idx = worker_id - 1; // Convert to 0-based index
    RespSlot* resp_slot = &shared_mem->resp_slots[slot_idx];
    
    // Copy response data
    size_t copy_len = std::min(response.length(), (size_t)(CHUNK_SIZE - 1));
    std::strncpy(resp_slot->data, response.c_str(), copy_len);
    resp_slot->data[copy_len] = '\0';
    resp_slot->len = copy_len;
    
    // Atomically publish the response
    resp_slot->task_id.store(task_id, std::memory_order_release);
    resp_slot->ready.store(true, std::memory_order_release);
    
    // Signal server that response is ready
    sem_t* sem_response = sem_open(SEM_RESPONSE_NAME, 0);
    if (sem_response != SEM_FAILED) {
        sem_post(sem_response);
        sem_close(sem_response);
    }
}

int main() {    
    // 1. Open existing shared memory (created by server)
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Worker: Failed to open shared memory" << std::endl;
        return 1;
    }

    // 2. Map it into memory
    SharedMem* shared_mem = (SharedMem*)mmap(nullptr, sizeof(SharedMem),
        PROT_READ | PROT_WRITE, MAP_SHARED,
        shm_fd, 0);
    if (shared_mem == MAP_FAILED) {
        std::cerr << "Worker: Failed to map shared memory" << std::endl;
        close(shm_fd);
        return 1;
    }

    // 3. Open semaphores (created by server)
    sem_t* sem_request = sem_open(SEM_REQUEST_NAME, 0);
    sem_t* sem_response = sem_open(SEM_RESPONSE_NAME, 0);

    if (sem_request == SEM_FAILED || sem_response == SEM_FAILED) {
        std::cerr << "Worker: Failed to open semaphores" << std::endl;
        munmap(shared_mem, sizeof(SharedMem));
        close(shm_fd);
        return 1;
    }

    // 4. Register this worker
    int worker_id = registerWorker(shared_mem);
    if (worker_id == -1) {
        std::cerr << "Worker: Failed to register - no available slots" << std::endl;
        munmap(shared_mem, sizeof(SharedMem));
        close(shm_fd);
        sem_close(sem_request);
        sem_close(sem_response);
        return 1;
    }

    std::cout << "Worker " << worker_id << " started, waiting for tasks..." << std::endl;

    // 5. Main loop: wait for tasks
    int empty_polls = 0;
    const int MAX_EMPTY_POLLS = 100;
    
    while (!shared_mem->shutdown_requested.load(std::memory_order_acquire)) {
        ReqSlot* req_slot = claimNextRequest(shared_mem);
        
        if (req_slot != nullptr) {
            empty_polls = 0; // Reset empty poll counter
            
            uint64_t task_id = req_slot->task_id.load(std::memory_order_acquire);
            std::string input(req_slot->data, req_slot->len);
            
            // Update worker status
            shared_mem->workers[worker_id-1].current_task_id.store(task_id);

            // Process the request
            std::string result = processRequest(input, worker_id);

            // Publish response
            publishResponse(shared_mem, worker_id, task_id, result);

            // Clear the request slot (mark as processed)
            req_slot->task_id.store(0, std::memory_order_release);
            
            // Update worker statistics (only local counter to reduce contention)
            shared_mem->workers[worker_id-1].current_task_id.store(0);
            shared_mem->workers[worker_id-1].processed_count.fetch_add(1);
            // Note: Removed global total_processed update to reduce contention
        }
        else {
            empty_polls++;
            
            // If we've polled empty many times, wait on semaphore
            if (empty_polls >= MAX_EMPTY_POLLS) {
                if (sem_wait(sem_request) != 0) {
                    std::cerr << "Worker " << worker_id << ": sem_wait failed" << std::endl;
                    break;
                }
                empty_polls = 0; // Reset after semaphore wait
            } else {
                // Short sleep to prevent excessive CPU usage
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }

    // Removed shutdown message for performance

    // Mark worker as inactive
    shared_mem->workers[worker_id-1].active.store(false);
    shared_mem->active_workers.fetch_sub(1);

    // Cleanup
    munmap(shared_mem, sizeof(SharedMem));
    close(shm_fd);
    sem_close(sem_request);
    sem_close(sem_response);

    return 0;
}
