#include "TCPServerStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

#define MAX_CLIENTS 5
#define RX_BUF_SIZE 1024
#define TX_BUF_SIZE 1024

TCPServerStream server;

// ===== User Callbacks =====
void onReceive(StreamIn* stream, Stream_LenType len) {
    TCPStream* client = (TCPStream*)IStream_getDriverArgs(stream);
    printf("[CLIENT %s:%u] Received %u bytes\n", client->Host, client->Port, len);
    // Echo Data
    OStream_writeStream(&client->Output, stream, len);
    OStream_flush(&client->Output);
}

void onClientConnect(TCPServerStream* s, TCPStream* client) {
    printf("[SERVER] New client connected from %s:%u (total: %d)\n", 
           client->Host, client->Port, TCPServerStream_getClientCount(s));
    
    IStream_onReceive(&client->Input, onReceive);
}

void onClientDisconnect(TCPServerStream* s, TCPStream* client) {
    printf("[SERVER] Client %s:%u disconnected (remaining: %d)\n", 
           client->Host, client->Port, TCPServerStream_getClientCount(s));
}

void onClientError(TCPServerStream* s, TCPStream* client, int error) {
    printf("[SERVER] Client %s:%u error: %d\n", client->Host, client->Port, error);
}

static void sleep_ms(unsigned int ms) {
#if defined(_WIN32) || defined(_WIN64)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int main() {
    // Initialize server
    if(!TCPServerStream_init(&server, "0.0.0.0", 65321, MAX_CLIENTS, 
                             RX_BUF_SIZE, TX_BUF_SIZE, 
                             TCPServerStream_Mode_ThreadPerClient)) {
        printf("Failed to initialize TCPServerStream\n");
        return 1;
    }

    // Register server callbacks
    TCPServerStream_onClientConnect(&server, onClientConnect);
    TCPServerStream_onClientDisconnect(&server, onClientDisconnect);
    TCPServerStream_onClientError(&server, onClientError);

    uint32_t counter = 0;
    uint32_t lastTime = 0;

    while(1) {
        // Broadcast counter every 5 seconds
#if defined(_WIN32) || defined(_WIN64)
        uint32_t now = GetTickCount();
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
        if(now - lastTime >= 5000) {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "Server Counter: %u\nClients: %d\n", 
                           counter++, TCPServerStream_getClientCount(&server));
            
            TCPServerStream_broadcast(&server, (uint8_t*)buf, n);
            lastTime = now;
        }

        sleep_ms(10);
    }

    TCPServerStream_close(&server);
    return 0;
}
