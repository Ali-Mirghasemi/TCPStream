#include "TCPStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#else
    #include <unistd.h>
    #include <pthread.h>
    #include <sys/time.h>
    #define sleep_ms(ms) usleep((ms) * 1000)
    typedef pthread_t thread_t;
    #define thread_create(t, f, a) (pthread_create(t, NULL, f, a) == 0)
    #define thread_join(t) pthread_join(t, NULL)
    static int64_t get_time_ms(void) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
#endif

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 65321
#define MAX_PACKET_SIZE 1460

typedef enum {
    TEST_DATA_TRANSFER = 1,
    TEST_RAPID_CONNECT = 2,
    TEST_PARALLEL_DATA = 3,
    TEST_PARALLEL_RAPID = 4,
    TEST_ALL = 5
} TestType;

typedef struct {
    int clientId;
    int testType;
    int64_t dataSize;
    int iterations;
    volatile int64_t* totalSent;
    volatile int64_t* totalRecv;
    volatile int64_t* totalErrors;
    volatile int64_t* totalConnections;
} ClientArgs;

// ===== Data Transfer Test =====
static int testDataTransfer(TCPStream* stream, int64_t totalBytes,
                           volatile int64_t* sent, volatile int64_t* recv) {
    uint8_t sendBuf[MAX_PACKET_SIZE];
    uint8_t recvBuf[MAX_PACKET_SIZE];
    int64_t bytesSent = 0;
    int64_t bytesRecv = 0;
    int64_t startTime = get_time_ms();
    int64_t lastReport = startTime;
    uint8_t firstTime = 0;
    
    printf("[TEST] Transferring %.2f GB...\n", totalBytes / (1024.0*1024.0*1024.0));
    
    while (bytesSent < totalBytes) {
        if (!stream->Connected) {
            printf("[TEST] Connection lost at %lld bytes!\n", (long long)bytesSent);
            return 0;
        }
        
        // Calculate packet size
        uint32_t packetSize = MAX_PACKET_SIZE;
        if (bytesSent + packetSize > totalBytes) {
            packetSize = (uint32_t)(totalBytes - bytesSent);
        }
        if (packetSize > OStream_space(&stream->Output)) {
            packetSize = OStream_space(&stream->Output);
        }
        
        // Generate test pattern
        for (uint32_t i = 0; i < packetSize; i++) {
            sendBuf[i] = (uint8_t)((bytesSent + i) & 0xFF);
        }
        
        // Send
        OStream_writeBytes(&stream->Output, sendBuf, packetSize);
        OStream_flush(&stream->Output);
        
        // Wait for echo - poll until we have enough data
        int64_t waitStart = get_time_ms();
        Stream_LenType needed = packetSize;
        
        while (IStream_available(&stream->Input) < needed) {
            if (!stream->Connected) {
                printf("[TEST] Connection lost while waiting at %lld\n", (long long)bytesSent);
                return 0;
            }
            if (get_time_ms() - waitStart > 10000) {
                printf("[TEST] Timeout! Sent=%lld, Recv=%lld, Available=%d, Needed=%d\n",
                       (long long)bytesSent, (long long)bytesRecv,
                       (int)IStream_available(&stream->Input), (int)needed);
                return 0;
            }
            sleep_ms(1);
        }
        
        // Read echo back
        IStream_readBytes(&stream->Input, recvBuf, packetSize);
        
        // Verify
        for (uint32_t i = 0; i < packetSize; i++) {
            uint8_t expected = (uint8_t)((bytesSent + i) & 0xFF);
            if (recvBuf[i] != expected) {
                printf("[TEST] CORRUPTION at byte %lld: exp=0x%02X got=0x%02X\n",
                       (long long)(bytesSent + i), expected, recvBuf[i]);
                return 0;
            }
        }
        
        bytesSent += packetSize;
        bytesRecv += packetSize;
        
        if (sent) __sync_fetch_and_add(sent, packetSize);
        if (recv) __sync_fetch_and_add(recv, packetSize);
        
        // Progress
        int64_t now = get_time_ms();
        if (!firstTime || packetSize != MAX_PACKET_SIZE || now - lastReport >= 2000) {
            firstTime = 1;
            double speed = (bytesSent / (1024.0*1024.0)) / ((now - startTime) / 1000.0);
            printf("[TEST] %.1f MB (%lu) / %.1f GB (%.1f MB/s)\n",
                   bytesSent / (1024.0*1024.0), bytesSent,
                   totalBytes / (1024.0*1024.0*1024.0),
                   speed);
            lastReport = now;
        }
    }
    
    double elapsed = (get_time_ms() - startTime) / 1000.0;
    double speed = (totalBytes / (1024.0*1024.0)) / elapsed;
    printf("[TEST] Complete: %.2f GB in %.1fs (%.1f MB/s)\n",
           totalBytes / (1024.0*1024.0*1024.0), elapsed, speed);
    return 1;
}

// ===== Rapid Connect Test =====
static int testRapidConnect(int clientId, int iterations,
                           volatile int64_t* connections, volatile int64_t* errors) {
    int ok = 0, fail = 0;
    int64_t startTime = get_time_ms();
    
    printf("[CLIENT %d] %d rapid connects...\n", clientId, iterations);
    
    for (int i = 0; i < iterations; i++) {
        uint8_t rxBuf[MAX_PACKET_SIZE * 44], txBuf[MAX_PACKET_SIZE * 44];
        TCPStream stream;
        memset(&stream, 0, sizeof(stream));
        
        if (!TCPStream_init(&stream, SERVER_IP, SERVER_PORT,
                           rxBuf, sizeof(rxBuf), txBuf, sizeof(txBuf))) {
            fail++;
            if (errors) __sync_fetch_and_add(errors, 1);
            sleep_ms(10);
            continue;
        }
        
        // Wait for connect
        int64_t timeout = get_time_ms() + 5000;
        while (!stream.Connected && get_time_ms() < timeout) {
            sleep_ms(5);
        }
        
        if (stream.Connected) {
            ok++;
            if (connections) __sync_fetch_and_add(connections, 1);
        } else {
            fail++;
            if (errors) __sync_fetch_and_add(errors, 1);
        }
        
        TCPStream_close(&stream);
        sleep_ms(10); // Small delay between connects
        
        if ((i+1) % 100 == 0) {
            printf("[CLIENT %d] %d/%d (ok:%d fail:%d)\n", clientId, i+1, iterations, ok, fail);
        }
    }
    
    double elapsed = (get_time_ms() - startTime) / 1000.0;
    printf("[CLIENT %d] Done: %d ok, %d fail in %.1fs (%.0f/sec)\n",
           clientId, ok, fail, elapsed, iterations/elapsed);
    return ok;
}

// ===== Client Thread =====
static void* clientThread(void* arg) {
    ClientArgs* a = (ClientArgs*)arg;
    
    if (a->testType == TEST_DATA_TRANSFER || a->testType == TEST_PARALLEL_DATA) {
        uint8_t rxBuf[MAX_PACKET_SIZE * 44], txBuf[MAX_PACKET_SIZE * 44];
        TCPStream stream;
        memset(&stream, 0, sizeof(stream));
        
        if (!TCPStream_init(&stream, SERVER_IP, SERVER_PORT,
                           rxBuf, sizeof(rxBuf), txBuf, sizeof(txBuf))) {
            printf("[CLIENT %d] Connect failed\n", a->clientId);
            if (a->totalErrors) __sync_fetch_and_add(a->totalErrors, 1);
            return NULL;
        }
        
        int64_t timeout = get_time_ms() + 10000;
        while (!stream.Connected && get_time_ms() < timeout) sleep_ms(10);
        
        if (!stream.Connected) {
            printf("[CLIENT %d] Connect timeout\n", a->clientId);
            if (a->totalErrors) __sync_fetch_and_add(a->totalErrors, 1);
            TCPStream_close(&stream);
            return NULL;
        }
        
        if (a->totalConnections) __sync_fetch_and_add(a->totalConnections, 1);
        testDataTransfer(&stream, a->dataSize, a->totalSent, a->totalRecv);
        TCPStream_close(&stream);
    } else {
        testRapidConnect(a->clientId, a->iterations, a->totalConnections, a->totalErrors);
    }
    return NULL;
}

// ===== Main =====
int main(int argc, char* argv[]) {
    int testType = 0;
    const char* serverIp = SERVER_IP;
    int serverPort = SERVER_PORT;
    int numClients = 4;
    int iterations = 100;
    double dataSizeGB = 0.1; // Default 100MB for quick testing
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i+1 < argc) serverIp = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc) serverPort = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) numClients = atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i+1 < argc) iterations = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) dataSizeGB = atof(argv[++i]);
        else if (atoi(argv[i]) >= 1 && atoi(argv[i]) <= 5) testType = atoi(argv[i]);
    }
    
    if (testType == 0) {
        printf("Usage: %s <1-5> [-s ip] [-p port] [-n clients] [-i iterations] [-d size_gb]\n", argv[0]);
        printf("  1=DataTransfer  2=RapidConnect  3=ParallelData  4=ParallelRapid  5=All\n");
        return 1;
    }
    
    printf("=== TCP Stream Stress Test ===\n");
    printf("Server: %s:%d\n\n", serverIp, serverPort);
    
    int64_t dataSize = (int64_t)(dataSizeGB * 1024.0 * 1024.0 * 1024.0);
    volatile int64_t totalSent = 0, totalRecv = 0, totalErrors = 0, totalConnections = 0;
    int64_t overallStart = get_time_ms();
    
    // Test 1: Single Data Transfer
    if (testType == 1 || testType == 5) {
        printf("--- Test 1: Data Transfer (%.1f GB) ---\n", dataSizeGB);
        totalSent = totalRecv = totalErrors = totalConnections = 0;
        ClientArgs args = {1, TEST_DATA_TRANSFER, dataSize, 0, &totalSent, &totalRecv, &totalErrors, &totalConnections};
        clientThread(&args);
        printf("Result: Sent=%.1fMB Recv=%.1fMB Errors=%lld => %s\n\n",
               totalSent/(1024.0*1024.0), totalRecv/(1024.0*1024.0),
               (long long)totalErrors, totalErrors ? "FAIL" : "PASS");
    }
    
    // Test 2: Rapid Connect
    if (testType == 2 || testType == 5) {
        printf("--- Test 2: Rapid Connect (%d iters) ---\n", iterations);
        totalSent = totalRecv = totalErrors = totalConnections = 0;
        ClientArgs args = {1, TEST_RAPID_CONNECT, 0, iterations, &totalSent, &totalRecv, &totalErrors, &totalConnections};
        clientThread(&args);
        printf("Result: Conn=%lld Errors=%lld => %s\n\n",
               (long long)totalConnections, (long long)totalErrors,
               totalErrors == 0 ? "PASS" : "FAIL");
    }
    
    // Test 3: Parallel Data
    if (testType == 3 || testType == 5) {
        printf("--- Test 3: Parallel Data (%d clients x %.1f GB) ---\n", numClients, dataSizeGB);
        totalSent = totalRecv = totalErrors = totalConnections = 0;
        thread_t* threads = malloc(sizeof(thread_t) * numClients);
        ClientArgs* argsArr = malloc(sizeof(ClientArgs) * numClients);
        for (int i = 0; i < numClients; i++) {
            memset(&argsArr[i], 0, sizeof(ClientArgs));
            argsArr[i].clientId = i+1;
            argsArr[i].testType = TEST_PARALLEL_DATA;
            argsArr[i].dataSize = dataSize;
            argsArr[i].totalSent = &totalSent;
            argsArr[i].totalRecv = &totalRecv;
            argsArr[i].totalErrors = &totalErrors;
            argsArr[i].totalConnections = &totalConnections;
            thread_create(&threads[i], clientThread, &argsArr[i]);
            sleep_ms(10);
        }
        for (int i = 0; i < numClients; i++) thread_join(threads[i]);
        free(threads);
        free(argsArr);
        printf("Result: Sent=%.1fMB Recv=%.1fMB Conn=%lld Errors=%lld => %s\n\n",
               totalSent/(1024.0*1024.0), totalRecv/(1024.0*1024.0),
               (long long)totalConnections, (long long)totalErrors,
               totalErrors ? "FAIL" : "PASS");
    }
    
    // Test 4: Parallel Rapid
    if (testType == 4 || testType == 5) {
        printf("--- Test 4: Parallel Rapid (%d clients x %d iters) ---\n", numClients, iterations);
        totalSent = totalRecv = totalErrors = totalConnections = 0;
        thread_t* threads = malloc(sizeof(thread_t) * numClients);
        ClientArgs* argsArr = malloc(sizeof(ClientArgs) * numClients);
        for (int i = 0; i < numClients; i++) {
            memset(&argsArr[i], 0, sizeof(ClientArgs));
            argsArr[i].clientId = i+1;
            argsArr[i].testType = TEST_PARALLEL_RAPID;
            argsArr[i].iterations = iterations;
            argsArr[i].totalSent = &totalSent;
            argsArr[i].totalRecv = &totalRecv;
            argsArr[i].totalErrors = &totalErrors;
            argsArr[i].totalConnections = &totalConnections;
            thread_create(&threads[i], clientThread, &argsArr[i]);
            sleep_ms(5);
        }
        for (int i = 0; i < numClients; i++) thread_join(threads[i]);
        free(threads);
        free(argsArr);
        printf("Result: Conn=%lld Errors=%lld => %s\n\n",
               (long long)totalConnections, (long long)totalErrors,
               totalErrors ? "FAIL" : "PASS");
    }
    
    printf("=== All Tests Complete (%.1f sec) ===\n", 
           (get_time_ms() - overallStart) / 1000.0);
    return 0;
}
