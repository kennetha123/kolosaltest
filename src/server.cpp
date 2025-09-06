#include "common.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <sstream>
#include <string>
#include <signal.h>

// POSIX includes for IPC
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>

// Socket includes for HTTP server
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Global shared memory
int g_shm = -1;
SharedMem* g_shm_fd = nullptr;
sem_t* g_sem_request = nullptr;
sem_t* g_sem_response = nullptr;
sem_t* g_thread_limit = nullptr;

std::atomic<uint64_t> g_next_task_id{1};
std::atomic<bool> g_shutdown_requested{false};

void cleanupIPC();

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "\nReceived shutdown signal (" << signal << "). Initiating graceful shutdown..." << std::endl;
    
    g_shutdown_requested.store(true);
    
    if (g_shm_fd) {
        g_shm_fd->shutdown_requested.store(true, std::memory_order_release);
        std::cout << "Signaled workers to shutdown..." << std::endl;
    }
    
    // Wait for workers to shutdown gracefully
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    std::cout << "Cleaning up resources..." << std::endl;
    cleanupIPC();
    std::cout << "Server shutdown complete!" << std::endl;
    exit(0);
}

bool initializeIPC() {

    // Clean up existing shared memory and semaphores first
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_REQUEST_NAME);
    sem_unlink(SEM_RESPONSE_NAME);

    g_shm = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666); //0666 for read/write for all users (permission)
    if (g_shm == -1) {
        perror("shm_open");
        cleanupIPC();
        return false;
    }

    if (ftruncate(g_shm, sizeof(SharedMem)) == -1) {
        perror("ftruncate");
        cleanupIPC();
        return false;
    }

    g_shm_fd = (SharedMem*)mmap(0, sizeof(SharedMem), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm, 0);
    if (g_shm_fd == MAP_FAILED || g_shm_fd == nullptr) {
        perror("mmap");
        cleanupIPC();
        return false;
    }

    // Initialize shared memory
    g_shm_fd->head.store(0);
    g_shm_fd->tail.store(0);
    g_shm_fd->active_workers.store(0);
    g_shm_fd->total_processed.store(0);
    
    for (size_t i = 0; i < RING_CAP; i++) {
        g_shm_fd->req[i].task_id.store(0);
        g_shm_fd->req[i].len = 0;
        g_shm_fd->req[i].data[0] = '\0';
    }
    for (size_t i = 0; i < MAX_WORKERS; i++) {
        g_shm_fd->resp_slots[i].task_id.store(0);
        g_shm_fd->resp_slots[i].ready.store(false);
        g_shm_fd->resp_slots[i].len = 0;
        g_shm_fd->resp_slots[i].data[0] = '\0';
        
        g_shm_fd->workers[i].worker_id = 0;
        g_shm_fd->workers[i].response_slot = 0;
        g_shm_fd->workers[i].active.store(false);
        g_shm_fd->workers[i].current_task_id.store(0);
        g_shm_fd->workers[i].processed_count.store(0);
    }

    // Initialize shutdown flag
    g_shm_fd->shutdown_requested.store(false);

    g_sem_request = sem_open(SEM_REQUEST_NAME, O_CREAT, 0666, 0);
    if (g_sem_request == SEM_FAILED || g_sem_request == nullptr) {
        perror("sem_open request");
        return false;
    }

    g_sem_response = sem_open(SEM_RESPONSE_NAME, O_CREAT, 0666, 0);
    if (g_sem_response == SEM_FAILED || g_sem_response == nullptr) {
        perror("sem_open response");
        return false;
    }

    g_thread_limit = sem_open("/thread_limit", O_CREAT, 0666, MAX_THREADS);
    if (g_thread_limit == SEM_FAILED) {
        perror("sem_open thread_limit");
        return false;
    }
    std::cout << "DEBUG: Thread limit semaphore initialized with " << MAX_THREADS << " slots" << std::endl;

    std::cout << "IPC initialized successfully" << std::endl;
    return true;
}

// Clean up IPC resources
void cleanupIPC() {
    if (g_shm_fd) {
        munmap(g_shm_fd, sizeof(SharedMem));
        g_shm_fd = nullptr;
    }
    if (g_shm != -1) {
        close(g_shm);
        shm_unlink(SHM_NAME);
    }
    if (g_sem_request) {
        sem_close(g_sem_request);
        sem_unlink(SEM_REQUEST_NAME);
    }
    if (g_sem_response) {
        sem_close(g_sem_response);
        sem_unlink(SEM_RESPONSE_NAME);
    }
    if (g_thread_limit) {
        sem_close(g_thread_limit);
        sem_unlink("/thread_limit");
    }
}

// Add request to lock-free ring buffer
bool addRequestToRing(const std::string& request, uint64_t task_id) {
    if (!g_shm_fd) return false;

    size_t head = g_shm_fd->head.load(std::memory_order_acquire);
    size_t next_head = (head + 1) % RING_CAP;
    
    size_t tail = g_shm_fd->tail.load(std::memory_order_acquire);
    if (next_head == tail) {
        return false;
    }

    ReqSlot& slot = g_shm_fd->req[head];
    size_t copy_len = std::min(request.length(), (size_t)(CHUNK_SIZE - 1));
    strncpy(slot.data, request.c_str(), copy_len);
    slot.data[copy_len] = '\0';
    slot.len = copy_len;
    
    slot.task_id.store(task_id, std::memory_order_release);
    g_shm_fd->head.store(next_head, std::memory_order_release);
    
    // Reduce semaphore contention by posting less frequently
    // Only post semaphore every few requests or when queue is small
    static thread_local int post_counter = 0;
    size_t queue_size = (next_head >= tail) ? (next_head - tail) : (RING_CAP - tail + next_head);
    
    if (++post_counter >= 3 || queue_size <= 2) {
        // Signal workers that work is available
        if(sem_post(g_sem_request) == -1)
        {
            perror("sem_post");
            return false;
        }
        post_counter = 0;
    }
    
    return true;
}

// Wait for response from any worker
std::string waitForResponse(uint64_t task_id, int timeout_ms = 10000) {
    if (!g_shm_fd) {
        return "ERROR: Shared memory not initialized";
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    while (!g_shutdown_requested.load()) {
        // Check all worker response slots
        for (size_t i = 0; i < MAX_WORKERS; i++) {
            if (!g_shm_fd) {
                return "ERROR: Shared memory lost during processing";
            }
            
            RespSlot& slot = g_shm_fd->resp_slots[i];
            if (slot.ready.load(std::memory_order_acquire) && 
                slot.task_id.load(std::memory_order_acquire) == task_id) {
                
                // Found our response - use length for proper string construction
                std::string result(slot.data, slot.len);
                
                // Clear the slot
                slot.task_id.store(0, std::memory_order_release);
                slot.ready.store(false, std::memory_order_release);
                slot.data[0] = '\0';
                slot.len = 0;
                
                return result;
            }
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > timeout_ms) {
            return "ERROR: Request timeout";
        }
        
        // Brief wait before next check
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    return "ERROR: Server shutting down";
}

std::string processRequest(const std::string& request) {
    if (!g_shm_fd) {
        return "ERROR: Shared memory not initialized";
    }
    
    uint64_t task_id = g_next_task_id.fetch_add(1);
    
    if (!addRequestToRing(request, task_id)) {
        return "ERROR: Request queue full";
    }
    
    return waitForResponse(task_id);
}

// Monitor thread to show system status
void monitorThread() {
    int last_queue_size = -1;
    int last_active_workers = -1;
    
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Reduced frequency for better performance
        
        if (!g_shm_fd || g_shutdown_requested.load()) break;
        
        size_t head = g_shm_fd->head.load();
        size_t tail = g_shm_fd->tail.load();
        size_t queue_size = (head >= tail) ? (head - tail) : (RING_CAP - tail + head);
        
        int active_workers = 0;
        for (size_t i = 0; i < MAX_WORKERS; i++) {
            if (g_shm_fd->workers[i].active.load()) {
                active_workers++;
            }
        }
        
        // Only print when there are changes
        if (queue_size != last_queue_size || active_workers != last_active_workers) {
            std::cout << "Status - Queue: " << queue_size << "/" << RING_CAP 
                      << ", Workers: " << active_workers << "/" << MAX_WORKERS << std::endl;
            last_queue_size = queue_size;
            last_active_workers = active_workers;
        }
    }
    
    std::cout << "Monitor thread shutting down..." << std::endl;
}

// Simple HTTP response helper
std::string createHttpResponse(int status, const std::string& content, const std::string& contentType = "text/plain") {
    std::ostringstream response;
    response << "HTTP/1.1 " << status;
    
    if (status == 200) response << " OK";
    else if (status == 404) response << " Not Found";
    else if (status == 500) response << " Internal Server Error";
    
    response << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << content;
    
    return response.str();
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    
    static HttpRequest parse(const std::string& request) {
        HttpRequest req;
        std::istringstream stream(request);
        std::string line;
        
        // Parse first line: METHOD PATH HTTP/1.1
        if (std::getline(stream, line)) {
            std::istringstream firstLine(line);
            firstLine >> req.method >> req.path;
        }
        
        // Skip headers until empty line
        int contentLength = 0;
        while (std::getline(stream, line) && line != "\r") {
            if (line.find("Content-Length:") == 0) {
                contentLength = std::stoi(line.substr(16));
            }
        }
        
        // Read body if present
        if (contentLength > 0) {
            req.body.resize(contentLength);
            stream.read(&req.body[0], contentLength);
        }
        
        return req;
    }
};

// HTTP request handler
std::string handleHttpRequest(const HttpRequest& request) {
    if (request.method == "GET" && request.path == "/health") {
        return createHttpResponse(200, "Server is running");
    }
    
    if (request.method == "GET" && request.path == "/status") {
        if (!g_shm_fd) {
            return createHttpResponse(500, "Shared memory not initialized");
        }
        
        size_t head = g_shm_fd->head.load();
        size_t tail = g_shm_fd->tail.load();
        size_t queue_size = (head >= tail) ? (head - tail) : (RING_CAP - tail + head);
        
        int active_workers = 0;
        for (size_t i = 0; i < MAX_WORKERS; i++) {
            if (g_shm_fd->workers[i].active.load()) {
                active_workers++;
            }
        }
        
        std::ostringstream json;
        json << "{\n";
        json << "  \"queue_size\": " << queue_size << ",\n";
        json << "  \"queue_capacity\": " << RING_CAP << ",\n";
        json << "  \"active_workers\": " << active_workers << ",\n";
        json << "  \"max_workers\": " << MAX_WORKERS << ",\n";
        json << "  \"architecture\": \"lock-free-ring-buffer\"\n";
        json << "}";
        
        return createHttpResponse(200, json.str(), "application/json");
    }
    
    if (request.method == "POST" && request.path == "/process") {
        if (request.body.empty()) {
            return createHttpResponse(400, "Request body is required");
        }
        std::cout << "[SERVER] Received request: " << request.body << std::endl;
        std::string response = processRequest(request.body);
        std::cout << "[SERVER] Response from worker: " << response << std::endl;
        return createHttpResponse(200, response);
    }
    
    if (request.method == "POST" && request.path == "/shutdown") {
        std::cout << "Shutdown requested via HTTP API" << std::endl;
        
        // Trigger graceful shutdown
        g_shutdown_requested.store(true);
        if (g_shm_fd) {
            g_shm_fd->shutdown_requested.store(true, std::memory_order_release);
        }
        
        return createHttpResponse(200, "Server shutdown initiated...");
    }
    
    return createHttpResponse(404, "Not Found");
}

// HTTP server thread
void httpServerThread() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed");
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8000);
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return;
    }
    
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        return;
    }
    
    std::cout << "HTTP server listening on port 8000" << std::endl;
    
    while (!g_shutdown_requested.load()) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0 || g_shutdown_requested.load()) {
            break;
        }
        
        // Handle request in a separate thread
        std::thread([client_socket]() {
            char buffer[4096] = {0};
            ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
            
            if (bytes_read > 0) {
                HttpRequest request = HttpRequest::parse(std::string(buffer, bytes_read));
                std::string response = handleHttpRequest(request);
                write(client_socket, response.c_str(), response.length());
            }
            
            close(client_socket);
        }).detach();
    }
    
    std::cout << "HTTP server shutting down..." << std::endl;
    close(server_fd);
}

int main(int argc, const char* argv[]) {
    std::cout << "HTTP Server starting..." << std::endl;
    
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);   // Ctrl+C
    signal(SIGTERM, signalHandler);  // Termination signal
    signal(SIGQUIT, signalHandler);  // Quit signal
        
    if (!initializeIPC()) {
        std::cerr << "Failed to initialize IPC" << std::endl;
        cleanupIPC();
        return 1;
    }

    // Start monitor thread
    std::thread monitor(monitorThread);
    monitor.detach();

    // Start HTTP server thread
    std::thread httpServer(httpServerThread);
    httpServer.detach();

    std::cout << " Server initialized successfully!" << std::endl;
    std::cout << " - Ring capacity: " << RING_CAP << " slots" << std::endl;
    std::cout << " - Max workers: " << MAX_WORKERS << std::endl;
    std::cout << " - Chunk size: " << CHUNK_SIZE << " bytes" << std::endl;
    std::cout << " Start worker processes to handle requests" << std::endl;
    std::cout << " Server running... Press Ctrl+C to stop" << std::endl;
    
    // Keep server running until shutdown is requested
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << " Main loop exiting..." << std::endl;
    
    cleanupIPC();
    return 0;
}