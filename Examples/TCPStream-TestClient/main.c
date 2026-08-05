#include "TCPStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #include <process.h>
    #define sleep_ms(ms) Sleep(ms)
    #define get_time_ms() GetTickCount64()
    typedef HANDLE thread_t;
    #define thread_create(t, f, a) ((*(t) = (HANDLE)_beginthreadex(NULL, 0, \
        (unsigned(__stdcall*)(void*))f, a, 0, NULL)) != NULL)
    #define thread_join(t) WaitForSingleObject(t, INFINITE); CloseHandle(t)
    #define atomic_add(p, v) InterlockedExchangeAdd64((LONG64 volatile*)p, v)
    #define atomic_load(p) InterlockedExchangeAdd64((LONG64 volatile*)p, 0)
#else
    #include <unistd.h>
    #include <pthread.h>
    #include <sys/time.h>
    #include <arpa/inet.h>
    #define sleep_ms(ms) usleep((ms) * 1000)
    typedef pthread_t thread_t;
    #define thread_create(t, f, a) (pthread_create(t, NULL, f, a) == 0)
    #define thread_join(t) pthread_join(t, NULL)
    #define atomic_add(p, v) __sync_fetch_and_add(p, v)
    #define atomic_load(p) __sync_fetch_and_add(p, 0)
    
    static int64_t get_time_ms(void) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
#endif

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 65321
#define MAX_PACKET_SIZE 1460
#define RX_BUF_SIZE (MAX_PACKET_SIZE * 4)
#define TX_BUF_SIZE (MAX_PACKET_SIZE * 4)

// Test configuration
typedef enum {
    TEST_NONE = 0,
    TEST_DATA_TRANSFER = 1,
    TEST_RAPID_CONNECT = 2,
    TEST_PARALLEL_DATA = 3,
    TEST_PARALLEL_RAPID = 4,
    TEST_ALL = 5
} TestType;

// Client thread arguments
typedef struct {
    int clientId;
    int testType;
    int64_t dataSize;  // For data transfer tests
    int iterations;    // For connect/disconnect tests
    volatile int* running;
    volatile int64_t* totalSent;
    volatile int64_t* totalRecv;
    volatile int64_t* totalErrors;
    volatile int64_t* totalConnections;
} ClientArgs;

// Generate test data pattern
static void generateTestData(uint8_t* buffer, uint32_t size, uint32_t offset) {
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)((offset + i) & 0xFF);
    }
}

// Verify received data
static int verifyData(uint8_t* buffer, uint32_t size, uint32_t offset) {
    for (uint32_t i = 0; i < size; i++) {
        if (buffer[i] != (uint8_t)((offset + i) & 0xFF)) {
            printf("[CLIENT] Data verification FAILED at offset %u: expected 0x%02X, got 0x%02X\n",
                   offset + i, (uint8_t)((offset + i) & 0xFF), buffer[i]);
            return 0;
        }
    }
    return 1;
}

// Test 1 & 3: Data transfer test
static int testDataTransfer(TCPStream* stream, int64_t totalBytes, 
                           volatile int64_t* sent, volatile int64_t* recv) {
    uint8_t sendBuf[MAX_PACKET_SIZE];
    uint8_t recvBuf[MAX_PACKET_SIZE];
    int64_t bytesSent = 0;
    int64_t bytesRecv = 0;
    int64_t startTime = get_time_ms();
    int64_t lastReport = startTime;
    int64_t lastBytes = 0;
    
    printf("[TEST] Starting data transfer: %.2f GB\n", totalBytes / (1024.0 * 1024.0 * 1024.0));
    
    while (bytesSent < totalBytes) {
        if (!stream->Connected) {
            printf("[TEST] Connection lost during data transfer\n");
            return 0;
        }
        
        // Determine packet size
        uint32_t packetSize = MAX_PACKET_SIZE;
        if (bytesSent + packetSize > totalBytes) {
            packetSize = (uint32_t)(totalBytes - bytesSent);
        }
        
        // Generate and send data
        generateTestData(sendBuf, packetSize, (uint32_t)bytesSent);
        OStream_writeBytes(&stream->Output, sendBuf, packetSize);
        OStream_flush(&stream->Output);
        bytesSent += packetSize;
        
        // Read echo
        int64_t recvStart = get_time_ms();
        while (bytesRecv < bytesSent) {
            Stream_LenType available = IStream_available(&stream->Input);
            if (available > 0) {
                uint32_t toRead = available;
                if (toRead > MAX_PACKET_SIZE) toRead = MAX_PACKET_SIZE;
                
                uint32_t actuallyRead = IStream_readBytes(&stream->Input, recvBuf, toRead);
                if (actuallyRead > 0) {
                    // Verify data
                    if (!verifyData(recvBuf, actuallyRead, (uint32_t)bytesRecv)) {
                        return 0;
                    }
                    bytesRecv += actuallyRead;
                }
            } else {
                // Timeout check
                if (get_time_ms() - recvStart > 30000) { // 30 second timeout
                    printf("[TEST] Receive timeout after %d seconds\n", 30);
                    return 0;
                }
                sleep_ms(1);
            }
        }
        
        // Progress report every 5 seconds
        int64_t now = get_time_ms();
        if (now - lastReport >= 5000) {
            double elapsed = (now - lastReport) / 1000.0;
            double speed = (bytesSent - lastBytes) / elapsed / (1024.0 * 1024.0);
            double totalElapsed = (now - startTime) / 1000.0;
            double totalSpeed = bytesSent / totalElapsed / (1024.0 * 1024.0);
            
            printf("[TEST] Progress: %.2f MB sent/recv | Speed: %.2f MB/s (Avg: %.2f MB/s)\n",
                   bytesSent / (1024.0 * 1024.0), speed, totalSpeed);
            
            lastReport = now;
            lastBytes = bytesSent;
            
            // Update global stats
            atomic_add(sent, bytesSent - atomic_load(sent));
            atomic_add(recv, bytesRecv - atomic_load(recv));
        }
    }
    
    int64_t endTime = get_time_ms();
    double totalElapsed = (endTime - startTime) / 1000.0;
    double totalSpeed = bytesSent / totalElapsed / (1024.0 * 1024.0);
    
    printf("[TEST] Data transfer complete: %.2f GB in %.2f seconds (%.2f MB/s)\n",
           bytesSent / (1024.0 * 1024.0 * 1024.0), totalElapsed, totalSpeed);
    
    return 1;
}

// Test 2 & 4: Rapid connect/disconnect test
static int testRapidConnect(int clientId, int iterations, 
                           volatile int64_t* connections, volatile int64_t* errors) {
    uint8_t rxBuf[1024];
    uint8_t txBuf[1024];
    TCPStream stream;
    int successCount = 0;
    int failCount = 0;
    
    printf("[CLIENT %d] Starting rapid connect test: %d iterations\n", clientId, iterations);
    
    int64_t startTime = get_time_ms();
    
    for (int i = 0; i < iterations; i++) {
        memset(&stream, 0, sizeof(stream));
        
        if (!TCPStream_init(&stream, SERVER_IP, SERVER_PORT, rxBuf, sizeof(rxBuf), txBuf, sizeof(txBuf))) {
            failCount++;
            atomic_add(errors, 1);
            printf("[CLIENT %d] Connect %d/%d FAILED\n", clientId, i + 1, iterations);
            
            // Small delay before retry
            sleep_ms(10);
            continue;
        }
        
        // Wait for connection
        int64_t timeout = get_time_ms() + 5000; // 5 second timeout
        while (!stream.Connected) {
            if (get_time_ms() > timeout) {
                printf("[CLIENT %d] Connection timeout\n", clientId);
                failCount++;
                atomic_add(errors, 1);
                TCPStream_close(&stream);
                break;
            }
            sleep_ms(10);
        }
        
        if (stream.Connected) {
            successCount++;
            atomic_add(connections, 1);
            
            // Send a small test message
            const char* msg = "Hello";
            OStream_writeBytes(&stream.Output, (uint8_t*)msg, (Stream_LenType)strlen(msg));
            OStream_flush(&stream.Output);
            
            // Immediate disconnect
            TCPStream_close(&stream);
        }
        
        // Very short delay between connects (stress test)
        sleep_ms(5);
        
        // Progress every 100 iterations
        if ((i + 1) % 100 == 0) {
            printf("[CLIENT %d] Progress: %d/%d (Success: %d, Failed: %d)\n",
                   clientId, i + 1, iterations, successCount, failCount);
        }
    }
    
    int64_t endTime = get_time_ms();
    double elapsed = (endTime - startTime) / 1000.0;
    
    printf("[CLIENT %d] Rapid connect test complete: %d success, %d failed in %.2f seconds (%.1f conn/sec)\n",
           clientId, successCount, failCount, elapsed, iterations / elapsed);
    
    return successCount;
}

// Client thread for parallel tests
static void* clientThread(void* arg) {
    ClientArgs* args = (ClientArgs*)arg;
    uint8_t rxBuf[RX_BUF_SIZE];
    uint8_t txBuf[TX_BUF_SIZE];
    
    printf("[CLIENT %d] Starting test type %d\n", args->clientId, args->testType);
    
    if (args->testType == TEST_DATA_TRANSFER || args->testType == TEST_PARALLEL_DATA) {
        TCPStream stream;
        memset(&stream, 0, sizeof(stream));
        
        if (!TCPStream_init(&stream, SERVER_IP, SERVER_PORT, rxBuf, RX_BUF_SIZE, txBuf, TX_BUF_SIZE)) {
            printf("[CLIENT %d] Failed to connect\n", args->clientId);
            atomic_add(args->totalErrors, 1);
            return NULL;
        }
        
        // Wait for connection
        int64_t timeout = get_time_ms() + 10000; // 10 second timeout
        while (!stream.Connected) {
            if (get_time_ms() > timeout) {
                printf("[CLIENT %d] Connection timeout\n", args->clientId);
                atomic_add(args->totalErrors, 1);
                TCPStream_close(&stream);
                return NULL;
            }
            sleep_ms(10);
        }
        
        atomic_add(args->totalConnections, 1);
        
        // Run data transfer test
        if (!testDataTransfer(&stream, args->dataSize, args->totalSent, args->totalRecv)) {
            printf("[CLIENT %d] Data transfer FAILED\n", args->clientId);
            atomic_add(args->totalErrors, 1);
        }
        
        TCPStream_close(&stream);
        
    } else if (args->testType == TEST_RAPID_CONNECT || args->testType == TEST_PARALLEL_RAPID) {
        testRapidConnect(args->clientId, args->iterations, 
                        args->totalConnections, args->totalErrors);
    }
    
    printf("[CLIENT %d] Test complete\n", args->clientId);
    return NULL;
}

// Print usage
void printUsage(const char* prog) {
    printf("Usage: %s <test_number> [options]\n\n", prog);
    printf("Tests:\n");
    printf("  1 - Single client data transfer (10 GB)\n");
    printf("  2 - Single client rapid connect/disconnect (1000 times)\n");
    printf("  3 - Parallel clients data transfer (32 clients, 10 GB each)\n");
    printf("  4 - Parallel clients rapid connect/disconnect (32 clients)\n");
    printf("  5 - Run all tests sequentially\n\n");
    printf("Options:\n");
    printf("  -s <server_ip>   Server IP address (default: 127.0.0.1)\n");
    printf("  -p <port>        Server port (default: 65321)\n");
    printf("  -n <number>      Number of clients (default: 32 for parallel tests)\n");
    printf("  -i <iterations>  Number of iterations (default: 1000 for rapid tests)\n");
    printf("  -d <size_gb>     Data size in GB (default: 10)\n");
}

int main(int argc, char* argv[]) {
    int testType = 0;
    const char* serverIp = SERVER_IP;
    int serverPort = SERVER_PORT;
    int numClients = 32;
    int iterations = 1000;
    double dataSizeGB = 10.0;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            serverIp = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            serverPort = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            numClients = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dataSizeGB = atof(argv[++i]);
        } else if (atoi(argv[i]) >= 1 && atoi(argv[i]) <= 5) {
            testType = atoi(argv[i]);
        }
    }
    
    if (testType == 0) {
        printUsage(argv[0]);
        return 1;
    }
    
    printf("=== TCP Stream Stress Test Client ===\n");
    printf("Server: %s:%d\n", serverIp, serverPort);
    
    int64_t totalDataSize = (int64_t)(dataSizeGB * 1024.0 * 1024.0 * 1024.0);
    volatile int running = 1;
    volatile int64_t totalSent = 0;
    volatile int64_t totalRecv = 0;
    volatile int64_t totalErrors = 0;
    volatile int64_t totalConnections = 0;
    
    int64_t overallStart = get_time_ms();
    
    if (testType == 1 || testType == 5) {
        printf("\n========================================\n");
        printf("TEST 1: Single Client Data Transfer (%.1f GB)\n", dataSizeGB);
        printf("========================================\n");
        
        ClientArgs args;
        memset(&args, 0, sizeof(args));
        args.clientId = 1;
        args.testType = TEST_DATA_TRANSFER;
        args.dataSize = totalDataSize;
        args.running = &running;
        args.totalSent = &totalSent;
        args.totalRecv = &totalRecv;
        args.totalErrors = &totalErrors;
        args.totalConnections = &totalConnections;
        
        int64_t startTime = get_time_ms();
        clientThread(&args);
        int64_t endTime = get_time_ms();
        
        printf("\nTest 1 Results:\n");
        printf("  Time: %.2f seconds\n", (endTime - startTime) / 1000.0);
        printf("  Data Sent: %.2f GB\n", totalSent / (1024.0 * 1024.0 * 1024.0));
        printf("  Data Recv: %.2f GB\n", totalRecv / (1024.0 * 1024.0 * 1024.0));
        printf("  Errors: %lld\n", (long long)totalErrors);
        printf("  Result: %s\n", totalErrors == 0 ? "PASS" : "FAIL");
    }
    
    if (testType == 2 || testType == 5) {
        printf("\n========================================\n");
        printf("TEST 2: Single Client Rapid Connect/Disconnect (%d iterations)\n", iterations);
        printf("========================================\n");
        
        ClientArgs args;
        memset(&args, 0, sizeof(args));
        args.clientId = 1;
        args.testType = TEST_RAPID_CONNECT;
        args.iterations = iterations;
        args.running = &running;
        args.totalSent = &totalSent;
        args.totalRecv = &totalRecv;
        args.totalErrors = &totalErrors;
        args.totalConnections = &totalConnections;
        
        int64_t startTime = get_time_ms();
        clientThread(&args);
        int64_t endTime = get_time_ms();
        
        printf("\nTest 2 Results:\n");
        printf("  Time: %.2f seconds\n", (endTime - startTime) / 1000.0);
        printf("  Connections: %lld\n", (long long)totalConnections);
        printf("  Errors: %lld\n", (long long)totalErrors);
        printf("  Rate: %.1f conn/sec\n", iterations / ((endTime - startTime) / 1000.0));
        printf("  Result: %s\n", totalErrors == 0 ? "PASS" : "FAIL");
    }
    
    if (testType == 3 || testType == 5) {
        printf("\n========================================\n");
        printf("TEST 3: Parallel Clients Data Transfer (%d clients, %.1f GB each)\n", 
               numClients, dataSizeGB);
        printf("========================================\n");
        
        thread_t* threads = (thread_t*)malloc(sizeof(thread_t) * numClients);
        ClientArgs* args = (ClientArgs*)malloc(sizeof(ClientArgs) * numClients);
        
        int64_t startTime = get_time_ms();
        
        // Start all clients
        for (int i = 0; i < numClients; i++) {
            memset(&args[i], 0, sizeof(ClientArgs));
            args[i].clientId = i + 1;
            args[i].testType = TEST_PARALLEL_DATA;
            args[i].dataSize = totalDataSize;
            args[i].running = &running;
            args[i].totalSent = &totalSent;
            args[i].totalRecv = &totalRecv;
            args[i].totalErrors = &totalErrors;
            args[i].totalConnections = &totalConnections;
            
            thread_create(&threads[i], clientThread, &args[i]);
            sleep_ms(10); // Small stagger
        }
        
        // Wait for all clients
        for (int i = 0; i < numClients; i++) {
            thread_join(threads[i]);
        }
        
        int64_t endTime = get_time_ms();
        
        printf("\nTest 3 Results:\n");
        printf("  Time: %.2f seconds\n", (endTime - startTime) / 1000.0);
        printf("  Total Data: %.2f GB\n", (totalSent + totalRecv) / (1024.0 * 1024.0 * 1024.0));
        printf("  Connections: %lld\n", (long long)totalConnections);
        printf("  Errors: %lld\n", (long long)totalErrors);
        printf("  Result: %s\n", totalErrors == 0 ? "PASS" : "FAIL");
        
        free(threads);
        free(args);
    }
    
    if (testType == 4 || testType == 5) {
        printf("\n========================================\n");
        printf("TEST 4: Parallel Clients Rapid Connect/Disconnect (%d clients, %d iterations each)\n", 
               numClients, iterations);
        printf("========================================\n");
        
        thread_t* threads = (thread_t*)malloc(sizeof(thread_t) * numClients);
        ClientArgs* args = (ClientArgs*)malloc(sizeof(ClientArgs) * numClients);
        
        int64_t startTime = get_time_ms();
        
        // Start all clients
        for (int i = 0; i < numClients; i++) {
            memset(&args[i], 0, sizeof(ClientArgs));
            args[i].clientId = i + 1;
            args[i].testType = TEST_PARALLEL_RAPID;
            args[i].iterations = iterations;
            args[i].running = &running;
            args[i].totalSent = &totalSent;
            args[i].totalRecv = &totalRecv;
            args[i].totalErrors = &totalErrors;
            args[i].totalConnections = &totalConnections;
            
            thread_create(&threads[i], clientThread, &args[i]);
            sleep_ms(5); // Small stagger
        }
        
        // Wait for all clients
        for (int i = 0; i < numClients; i++) {
            thread_join(threads[i]);
        }
        
        int64_t endTime = get_time_ms();
        
        printf("\nTest 4 Results:\n");
        printf("  Time: %.2f seconds\n", (endTime - startTime) / 1000.0);
        printf("  Total Connections: %lld\n", (long long)totalConnections);
        printf("  Total Errors: %lld\n", (long long)totalErrors);
        printf("  Result: %s\n", totalErrors == 0 ? "PASS" : "FAIL");
        
        free(threads);
        free(args);
    }
    
    int64_t overallEnd = get_time_ms();
    
    printf("\n========================================\n");
    printf("ALL TESTS COMPLETE\n");
    printf("Total Time: %.2f seconds\n", (overallEnd - overallStart) / 1000.0);
    printf("========================================\n");
    
    return 0;
}
