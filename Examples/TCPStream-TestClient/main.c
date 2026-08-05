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
#define PIPELINE_DEPTH 64

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

// ===== State for async data transfer =====
typedef struct {
    volatile int64_t bytesRecv;
    volatile int64_t bytesSent;
    volatile int done;
    volatile int error;
    int64_t totalBytes;
    int64_t startTime;
    int64_t lastReport;
    TCPStream* stream;
    volatile int64_t* globalSent;
    volatile int64_t* globalRecv;
} TransferState;

// Called by poll thread when data arrives
void onTransferReceive(StreamIn* input, Stream_LenType len) {
    TCPStream* stream = (TCPStream*)IStream_getDriverArgs(input);
    TransferState* state = (TransferState*)IStream_getArgs(input);
    
    if (!state || state->done) return;
    
    uint8_t buf[8192];
    Stream_LenType available = IStream_available(input);
    
    while (available > 0) {
        Stream_LenType toRead = available;
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        IStream_readBytes(input, buf, toRead);
        
        // Verify
        int64_t base = state->bytesRecv;
        for (Stream_LenType i = 0; i < toRead; i++) {
            uint8_t expected = (uint8_t)((base + i) & 0xFF);
            if (buf[i] != expected) {
                printf("[TEST] CORRUPTION at byte %lld: exp=0x%02X got=0x%02X\n",
                       (long long)(base + i), expected, buf[i]);
                state->error = 1;
                state->done = 1;
                return;
            }
        }
        
        state->bytesRecv += toRead;
        if (state->globalRecv) __sync_fetch_and_add(state->globalRecv, toRead);
        
        // Check if done
        if (state->bytesRecv >= state->totalBytes) {
            state->done = 1;
            return;
        }
        
        available = IStream_available(input);
    }
}

// ===== Fast Data Transfer (Pipelined, Callback-driven receive) =====
static int testDataTransfer(TCPStream* stream, int64_t totalBytes,
                           volatile int64_t* sent, volatile int64_t* recv) {
    uint8_t sendBuf[MAX_PACKET_SIZE];
    int64_t bytesSent = 0;
    int64_t startTime = get_time_ms();
    int64_t lastReport = startTime;
    
    TransferState state;
    memset(&state, 0, sizeof(state));
    state.totalBytes = totalBytes;
    state.startTime = startTime;
    state.lastReport = startTime;
    state.stream = stream;
    state.globalSent = sent;
    state.globalRecv = recv;
    
    // Set up callback for receiving data
    IStream_setArgs(&stream->Input, &state);
    IStream_onReceive(&stream->Input, onTransferReceive);
    
    printf("[TEST] Transferring %.2f GB (pipelined)...\n", totalBytes / (1024.0*1024.0*1024.0));
    
    int64_t sendAhead = 0;
    
    while (!state.done && !state.error) {
        if (!stream->Connected) {
            printf("[TEST] Connection lost!\n");
            return 0;
        }
        
        // ===== SEND: Keep the pipe full =====
        while (bytesSent < totalBytes && sendAhead < (PIPELINE_DEPTH * MAX_PACKET_SIZE)) {
            uint32_t packetSize = MAX_PACKET_SIZE;
            if (bytesSent + packetSize > totalBytes) {
                packetSize = (uint32_t)(totalBytes - bytesSent);
            }
            
            if (OStream_space(&stream->Output) < (Stream_LenType)packetSize) {
                OStream_flush(&stream->Output);
                if (OStream_space(&stream->Output) < (Stream_LenType)packetSize) break;
            }
            
            for (uint32_t i = 0; i < packetSize; i++) {
                sendBuf[i] = (uint8_t)((bytesSent + i) & 0xFF);
            }
            
            OStream_writeBytes(&stream->Output, sendBuf, packetSize);
            bytesSent += packetSize;
            sendAhead = bytesSent - state.bytesRecv;
            if (sent) __sync_fetch_and_add(sent, packetSize);
        }
        OStream_flush(&stream->Output);
        
        // Update sendAhead
        sendAhead = bytesSent - state.bytesRecv;
        
        // Small sleep to avoid busy-waiting
        if (bytesSent >= totalBytes) {
            sleep_ms(1);
        }
        
        // Progress report
        int64_t now = get_time_ms();
        if (now - lastReport >= 2000) {
            int64_t recvBytes = state.bytesRecv;
            double elapsed = (now - startTime) / 1000.0;
            double speed = (recvBytes / (1024.0*1024.0)) / (elapsed > 0 ? elapsed : 0.001);
            printf("[TEST] %.1f MB / %.1f GB (%.1f MB/s) [pipe: %lld]\n",
                   recvBytes / (1024.0*1024.0),
                   totalBytes / (1024.0*1024.0*1024.0),
                   speed, (long long)sendAhead);
            lastReport = now;
        }
    }
    
    // Wait a bit more for any straggling data
    for (int i = 0; i < 50 && !state.done; i++) {
        sleep_ms(100);
    }
    
    double elapsed = (get_time_ms() - startTime) / 1000.0;
    
    if (state.error) {
        printf("[TEST] FAILED - data corruption\n");
        return 0;
    }
    
    if (state.bytesRecv < totalBytes) {
        printf("[TEST] FAILED - incomplete: recv=%lld / %lld\n",
               (long long)state.bytesRecv, (long long)totalBytes);
        return 0;
    }
    
    double speed = (totalBytes / (1024.0*1024.0)) / (elapsed > 0 ? elapsed : 0.001);
    printf("[TEST] Complete: %.2f GB in %.1fs (%.1f MB/s)\n",
           totalBytes / (1024.0*1024.0*1024.0), elapsed, speed);
    
    // Clean up callback
    IStream_onReceive(&stream->Input, NULL);
    IStream_setArgs(&stream->Input, NULL);
    
    return 1;
}

// ===== Rapid Connect Test (unchanged) =====
static int testRapidConnect(int clientId, int iterations,
                           volatile int64_t* connections, volatile int64_t* errors) {
    int ok = 0, fail = 0;
    int64_t startTime = get_time_ms();
    
    printf("[CLIENT %d] %d rapid connects...\n", clientId, iterations);
    
    for (int i = 0; i < iterations; i++) {
        uint8_t rxBuf[4096], txBuf[4096];
        TCPStream stream;
        memset(&stream, 0, sizeof(stream));
        
        if (!TCPStream_init(&stream, SERVER_IP, SERVER_PORT,
                           rxBuf, sizeof(rxBuf), txBuf, sizeof(txBuf))) {
            fail++;
            if (errors) __sync_fetch_and_add(errors, 1);
            sleep_ms(10);
            continue;
        }
        
        int64_t timeout = get_time_ms() + 5000;
        while (!stream.Connected && get_time_ms() < timeout) sleep_ms(5);
        
        if (stream.Connected) {
            ok++;
            if (connections) __sync_fetch_and_add(connections, 1);
        } else {
            fail++;
            if (errors) __sync_fetch_and_add(errors, 1);
        }
        
        TCPStream_close(&stream);
        sleep_ms(10);
        
        if ((i+1) % 500 == 0) {
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
        uint8_t rxBuf[65536], txBuf[65536];
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
    double dataSizeGB = 0.1;
    
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
    
    if (testType == 1 || testType == 5) {
        printf("--- Test 1: Data Transfer (%.1f GB) ---\n", dataSizeGB);
        totalSent = totalRecv = totalErrors = totalConnections = 0;
        ClientArgs args = {1, TEST_DATA_TRANSFER, dataSize, 0, &totalSent, &totalRecv, &totalErrors, &totalConnections};
        clientThread(&args);
        printf("Result: Sent=%.1fMB Recv=%.1fMB Errors=%lld => %s\n\n",
               totalSent/(1024.0*1024.0), totalRecv/(1024.0*1024.0),
               (long long)totalErrors, totalErrors ? "FAIL" : "PASS");
    }
    
    if (testType == 2 || testType == 5) {
        printf("--- Test 2: Rapid Connect (%d iters) ---\n", iterations);
        totalSent = totalRecv = totalErrors = totalConnections = 0;
        ClientArgs args = {1, TEST_RAPID_CONNECT, 0, iterations, &totalSent, &totalRecv, &totalErrors, &totalConnections};
        clientThread(&args);
        printf("Result: Conn=%lld Errors=%lld => %s\n\n",
               (long long)totalConnections, (long long)totalErrors,
               totalErrors == 0 ? "PASS" : "FAIL");
    }
    
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
