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

// Oat++ includes
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

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

#include OATPP_CODEGEN_BEGIN(ApiController)

class ApiController : public oatpp::web::server::api::ApiController {
public:
    ApiController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    static std::shared_ptr<ApiController> createShared(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<ApiController>(objectMapper);
    }

    ENDPOINT("GET", "/health", health) {
        auto response = createResponse(Status::CODE_200, "Server is running");
        response->putHeader(Header::CONTENT_TYPE, "text/plain");
        return response;
    }

    ENDPOINT("GET", "/status", status) {
        if (!g_shm_fd) {
            return createResponse(Status::CODE_500, "Shared memory not initialized");
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
        
        auto response = createResponse(Status::CODE_200, json.str().c_str());
        response->putHeader(Header::CONTENT_TYPE, "application/json");
        return response;
    }

    ENDPOINT("POST", "/process", process, 
             BODY_STRING(String, body)) {
        if (!body || body->empty()) {
            return createResponse(Status::CODE_400, "Request body is required");
        }
        
        std::cout << "[SERVER] Received request: " << body->c_str() << std::endl;
        std::string response = processRequest(body->c_str());
        std::cout << "[SERVER] Response from worker: " << response << std::endl;
        
        return createResponse(Status::CODE_200, response.c_str());
    }

    ENDPOINT("POST", "/shutdown", shutdown) {
        std::cout << "Shutdown requested via HTTP API" << std::endl;
        
        // Trigger graceful shutdown
        g_shutdown_requested.store(true);
        if (g_shm_fd) {
            g_shm_fd->shutdown_requested.store(true, std::memory_order_release);
        }
        
        return createResponse(Status::CODE_200, "Server shutdown initiated...");
    }
};

#include OATPP_CODEGEN_END(ApiController)

class AppComponent {
public:
    
    /**
     * Create ConnectionProvider component which listens on the port
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, serverConnectionProvider)([] {
        return oatpp::network::tcp::server::ConnectionProvider::createShared(
            {"0.0.0.0", 8000, oatpp::network::Address::IP_4});
    }());
    
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, httpRouter)([] {
        return oatpp::web::server::HttpRouter::createShared();
    }());
    
    /**
     * Create ConnectionHandler component which uses Router component to route requests
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, serverConnectionHandler)([] {
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
        return oatpp::web::server::HttpConnectionHandler::createShared(router);
    }());
    
    /**
     * Create ObjectMapper component to serialize/deserialize DTOs in Controller's API
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper)([] {
        return oatpp::parser::json::mapping::ObjectMapper::createShared();
    }());

};

void oatppServerThread() {
    oatpp::base::Environment::init();
    
    AppComponent components;
    
    OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
    
    /* Create ApiController and add all of its endpoints to router */
    auto controller = ApiController::createShared();
    router->addController(controller);
    
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, connectionHandler);
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, connectionProvider);
    
    oatpp::network::Server server(connectionProvider, connectionHandler);
    
    std::cout << "Oat++ HTTP server listening on port 8000" << std::endl;
    
    server.run();
    
    std::cout << "Oat++ HTTP server shutting down..." << std::endl;
    
    oatpp::base::Environment::destroy();
}

int main(int argc, const char* argv[]) {
    std::cout << "Oat++ HTTP Server starting..." << std::endl;
    
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

    std::thread oatppServer(oatppServerThread);
    oatppServer.detach();

    std::cout << " Server initialized successfully!" << std::endl;
    std::cout << " - Ring capacity: " << RING_CAP << " slots" << std::endl;
    std::cout << " - Max workers: " << MAX_WORKERS << std::endl;
    std::cout << " - Chunk size: " << CHUNK_SIZE << " bytes" << std::endl;
    std::cout << " - Using Oat++ HTTP framework" << std::endl;
    std::cout << " Start worker processes to handle requests" << std::endl;
    std::cout << " Server running... Press Ctrl+C to stop" << std::endl;
    
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << " Main loop exiting..." << std::endl;
    
    cleanupIPC();
    return 0;
}