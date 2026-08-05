#include "TCPServerStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #define sleep_ms(ms) Sleep(ms)
    #define get_time_ms() GetTickCount64()
#else
    #include <unistd.h>
    #include <sys/time.h>
    #define sleep_ms(ms) usleep((ms) * 1000)
    static int64_t get_time_ms(void) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
#endif

#define MAX_CLIENTS 64
#define RX_BUF_SIZE (1460 * 44)
#define TX_BUF_SIZE (1460 * 44)
#define SERVER_PORT 65321

typedef struct {
    volatile int64_t totalBytesReceived;
    volatile int64_t totalBytesSent;
    volatile int32_t activeConnections;
    volatile int32_t totalConnections;
    volatile int32_t connectionErrors;
} ServerStats;

ServerStats g_stats;
TCPServerStream g_server;
volatile int g_running = 1;

// Simple echo: just write back what we receive
void onClientDataReceive(StreamIn* stream, Stream_LenType len) {
    TCPStream* client = (TCPStream*)IStream_getDriverArgs(stream);
    if (!client || len == 0) return;
    
    // Read all available and echo back
    Stream_LenType available;
    Stream_LenType space;

    while ((available = IStream_available(stream)) > 0 && (space = OStream_space(&client->Output)) > 0) {
        if (available > space) {
            available = space;
        }
        OStream_writeStream(&client->Output, stream, available);
        OStream_flush(&client->Output);
        __sync_fetch_and_add(&g_stats.totalBytesSent, available);
    }
    
    __sync_fetch_and_add(&g_stats.totalBytesReceived, len);
}

void onClientConnect(TCPServerStream* s, TCPStream* client) {
    __sync_fetch_and_add(&g_stats.activeConnections, 1);
    __sync_fetch_and_add(&g_stats.totalConnections, 1);
    IStream_onReceive(&client->Input, onClientDataReceive);
    
    printf("[CONNECT] %s:%u (Active: %d, Total: %d)\n", 
           client->Host, client->Port,
           g_stats.activeConnections, g_stats.totalConnections);
    fflush(stdout);
}

void onClientDisconnect(TCPServerStream* s, TCPStream* client) {
    __sync_fetch_and_add(&g_stats.activeConnections, -1);
    printf("[DISCONNECT] %s:%u (Active: %d)\n", 
           client->Host, client->Port, g_stats.activeConnections);
    fflush(stdout);
}

void onClientError(TCPServerStream* s, TCPStream* client, int error) {
    __sync_fetch_and_add(&g_stats.activeConnections, -1);
    __sync_fetch_and_add(&g_stats.connectionErrors, 1);
    printf("[ERROR] %s:%u Error: %d\n", client->Host, client->Port, error);
    fflush(stdout);
}

int main() {
    printf("=== TCP Stream Stress Test Server ===\n");
    printf("Port: %d, Max Clients: %d\n", SERVER_PORT, MAX_CLIENTS);
    
    memset(&g_stats, 0, sizeof(g_stats));
    
    if (!TCPServerStream_init(&g_server, "0.0.0.0", SERVER_PORT, MAX_CLIENTS,
                              RX_BUF_SIZE, TX_BUF_SIZE, 
                              TCPServerStream_Mode_ThreadPerClient)) {
        printf("FAILED to initialize server!\n");
        return 1;
    }
    
    TCPServerStream_onClientConnect(&g_server, onClientConnect);
    TCPServerStream_onClientDisconnect(&g_server, onClientDisconnect);
    TCPServerStream_onClientError(&g_server, onClientError);
    
    printf("Server started. Press Ctrl+C to stop.\n\n");
    
    while (g_running) {
        sleep_ms(2000);
        
        double recvMB = g_stats.totalBytesReceived / (1024.0 * 1024.0);
        double sentMB = g_stats.totalBytesSent / (1024.0 * 1024.0);
        
        printf("[STATS] Clients: %d/%d | Errors: %d | Data: %.1f MB (%lu) recv, %.1f MB (%lu) sent\n",
               g_stats.activeConnections, g_stats.totalConnections,
               g_stats.connectionErrors, recvMB, g_stats.totalBytesReceived, sentMB, g_stats.totalBytesSent);
        fflush(stdout);
    }
    
    printf("\nShutting down...\n");
    TCPServerStream_close(&g_server);
    
    return 0;
}
