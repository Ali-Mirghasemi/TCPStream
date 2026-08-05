#include "TCPServerStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #define atomic_add(p, v) InterlockedExchangeAdd((LONG volatile*)p, v)
    #define atomic_load(p) InterlockedExchangeAdd((LONG volatile*)p, 0)
#else
    #include <unistd.h>
    #include <pthread.h>
    #include <sys/time.h>
    #include <arpa/inet.h>
    #define atomic_add(p, v) __sync_fetch_and_add(p, v)
    #define atomic_load(p) __sync_fetch_and_add(p, 0)
#endif

#define MAX_CLIENTS 64
#define RX_BUF_SIZE (1460 * 64)  // Large enough for max packet size * max clients
#define TX_BUF_SIZE (1460 * 64)
#define SERVER_PORT 65321

// Statistics
typedef struct {
    volatile int64_t totalBytesReceived;
    volatile int64_t totalBytesSent;
    volatile int32_t activeConnections;
    volatile int32_t totalConnections;
    volatile int32_t failedConnections;
    volatile int32_t connectionErrors;
} ServerStats;

ServerStats g_stats;
TCPServerStream g_server;
volatile int g_running = 1;

// Mutex for stats
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION g_statsMutex;
    #define STATS_LOCK() EnterCriticalSection(&g_statsMutex)
    #define STATS_UNLOCK() LeaveCriticalSection(&g_statsMutex)
    #define STATS_INIT() InitializeCriticalSection(&g_statsMutex)
    #define STATS_DESTROY() DeleteCriticalSection(&g_statsMutex)
#else
    pthread_mutex_t g_statsMutex = PTHREAD_MUTEX_INITIALIZER;
    #define STATS_LOCK() pthread_mutex_lock(&g_statsMutex)
    #define STATS_UNLOCK() pthread_mutex_unlock(&g_statsMutex)
    #define STATS_INIT() ((void)0)
    #define STATS_DESTROY() pthread_mutex_destroy(&g_statsMutex)
#endif

// Get current time in milliseconds
static int64_t getTimeMs(void) {
#if defined(_WIN32) || defined(_WIN64)
    return GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

// Print statistics periodically
static void* statsThread(void* arg) {
    (void)arg;
    int64_t lastBytesRecv = 0;
    int64_t lastTime = getTimeMs();
    
    while (g_running) {
#if defined(_WIN32) || defined(_WIN64)
        Sleep(1000);
#else
        sleep(1);
#endif
        
        int64_t now = getTimeMs();
        int64_t currentBytesRecv = atomic_load(&g_stats.totalBytesReceived);
        int64_t currentBytesSent = atomic_load(&g_stats.totalBytesSent);
        int32_t currentActive = atomic_load(&g_stats.activeConnections);
        int32_t currentTotal = atomic_load(&g_stats.totalConnections);
        int32_t currentErrors = atomic_load(&g_stats.connectionErrors);
        
        double elapsed = (now - lastTime) / 1000.0;
        double recvSpeed = (currentBytesRecv - lastBytesRecv) / elapsed;
        
        printf("\n[STATS] Active: %d | Total: %d | Errors: %d\n", 
               currentActive, currentTotal, currentErrors);
        printf("[STATS] Recv: %.2f MB (%.2f MB/s) | Sent: %.2f MB\n",
               currentBytesRecv / (1024.0 * 1024.0),
               recvSpeed / (1024.0 * 1024.0),
               currentBytesSent / (1024.0 * 1024.0));
        fflush(stdout);
        
        lastBytesRecv = currentBytesRecv;
        lastTime = now;
    }
    return NULL;
}

// Client data receive callback
void onClientDataReceive(StreamIn* stream, Stream_LenType len) {
    TCPStream* client = (TCPStream*)IStream_getDriverArgs(stream);
    if (!client || !len) return;
    
    // Echo data back - this is the echo test
    Stream_LenType available = IStream_available(stream);
    if (available > 0) {
        OStream_writeStream(&client->Output, stream, available);
        OStream_flush(&client->Output);
    }
    
    // Update stats
    atomic_add(&g_stats.totalBytesReceived, len);
    atomic_add(&g_stats.totalBytesSent, len);
}

// Server callbacks
void onClientConnect(TCPServerStream* s, TCPStream* client) {
    atomic_add(&g_stats.activeConnections, 1);
    atomic_add(&g_stats.totalConnections, 1);
    
    // Set client callbacks
    IStream_onReceive(&client->Input, onClientDataReceive);
    
    printf("[CONNECT] Client %s:%u (Active: %d, Total: %d)\n", 
           client->Host, client->Port,
           (int)atomic_load(&g_stats.activeConnections),
           (int)atomic_load(&g_stats.totalConnections));
    fflush(stdout);
}

void onClientDisconnect(TCPServerStream* s, TCPStream* client) {
    atomic_add(&g_stats.activeConnections, -1);
    
    printf("[DISCONNECT] Client %s:%u (Active: %d)\n", 
           client->Host, client->Port,
           (int)atomic_load(&g_stats.activeConnections));
    fflush(stdout);
}

void onClientError(TCPServerStream* s, TCPStream* client, int error) {
    atomic_add(&g_stats.activeConnections, -1);
    atomic_add(&g_stats.connectionErrors, 1);
    
    printf("[ERROR] Client %s:%u Error: %d (Active: %d, Total Errors: %d)\n", 
           client->Host, client->Port, error,
           (int)atomic_load(&g_stats.activeConnections),
           (int)atomic_load(&g_stats.connectionErrors));
    fflush(stdout);
}

int main() {
    printf("=== TCP Stream Stress Test Server ===\n");
    printf("Starting server on port %d...\n", SERVER_PORT);
    
    memset(&g_stats, 0, sizeof(g_stats));
    STATS_INIT();
    
    // Initialize server with large buffers for stress testing
    if (!TCPServerStream_init(&g_server, "0.0.0.0", SERVER_PORT, MAX_CLIENTS,
                              RX_BUF_SIZE, TX_BUF_SIZE, 
                              TCPServerStream_Mode_ThreadPerClient)) {
        printf("FAILED to initialize server!\n");
        return 1;
    }
    
    // Register callbacks
    TCPServerStream_onClientConnect(&g_server, onClientConnect);
    TCPServerStream_onClientDisconnect(&g_server, onClientDisconnect);
    TCPServerStream_onClientError(&g_server, onClientError);
    
    printf("Server started successfully!\n");
    printf("Waiting for test clients...\n\n");
    
    // Start statistics thread
#if defined(_WIN32) || defined(_WIN64)
    HANDLE statsThreadHandle;
    statsThreadHandle = (HANDLE)_beginthreadex(NULL, 0, 
        (unsigned(__stdcall*)(void*))statsThread, NULL, 0, NULL);
#else
    pthread_t statsThreadHandle;
    pthread_create(&statsThreadHandle, NULL, statsThread, NULL);
#endif
    
    // Main loop - just keep running
    while (g_running) {
#if defined(_WIN32) || defined(_WIN64)
        Sleep(1000);
#else
        sleep(1);
#endif
    }
    
    printf("\nShutting down server...\n");
    
    // Cleanup
    TCPServerStream_close(&g_server);
    
#if defined(_WIN32) || defined(_WIN64)
    WaitForSingleObject(statsThreadHandle, INFINITE);
    CloseHandle(statsThreadHandle);
#else
    pthread_join(statsThreadHandle, NULL);
#endif
    
    STATS_DESTROY();
    
    printf("Final Statistics:\n");
    printf("  Total Data Received: %.2f MB\n", 
           (double)g_stats.totalBytesReceived / (1024.0 * 1024.0));
    printf("  Total Data Sent: %.2f MB\n", 
           (double)g_stats.totalBytesSent / (1024.0 * 1024.0));
    printf("  Total Connections: %d\n", (int)g_stats.totalConnections);
    printf("  Connection Errors: %d\n", (int)g_stats.connectionErrors);
    
    return 0;
}
