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

// ===== Callback Functions =====
void onConnect(TCPStream* s) {
    printf("[INFO] Connected to server!\n");
}

void onDisconnect(TCPStream* s) {
    printf("[INFO] Disconnected from server!\n");
}

void onError(TCPStream* s, int err) {
    printf("[ERROR] TCP error: %d\n", err);
}

void onReceive(StreamIn* stream, Stream_LenType len) {
    printf("[INFO] Data Received: %u!\n", len);
}

void onClientConnect(TCPServerStream* s, TCPStream* client) {
    printf("[INFO] New client connected! Socket: %d\n", (int)client->Socket);

    // Set client callbacks
    TCPStream_onConnect(client, onConnect);
    TCPStream_onDisconnect(client, onDisconnect);
    TCPStream_onError(client, onError);
    IStream_onReceive(&client->Input, onReceive);
}

int main() {
    // Initialize server
    if(!TCPServerStream_init(&server, "0.0.0.0", 65321, MAX_CLIENTS, TCPServerStream_Mode_ThreadPerClient)) {
        printf("Failed to initialize TCPServerStream\n");
        return 1;
    }

    TCPServerStream_onClientConnect(&server, onClientConnect);

    uint32_t counter = 0;
    static uint32_t lastTime = 0;

    while(1) {
        // --- Echo received data for all clients ---
        for(uint16_t i = 0; i < server.MaxClients; i++) {
            TCPStream* client = server.Clients[i];
            if(!client || !client->Connected) continue;

            Stream_LenType len = IStream_available(&client->Input);
            if(len > 0) {
                OStream_writeStream(&client->Output, &client->Input, len);
                OStream_flush(&client->Output);
            }
        }

        // --- Send counter every 5 seconds ---
#if defined(_WIN32) || defined(_WIN64)
        uint32_t now = GetTickCount();
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
        if(now - lastTime >= 5000) {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "Counter: %u\n", counter++);
            for(uint16_t i = 0; i < server.MaxClients; i++) {
                TCPStream* client = server.Clients[i];
                if(!client || !client->Connected) continue;
                OStream_writeBytes(&client->Output, (uint8_t*)buf, n);
                OStream_flush(&client->Output);
            }
            lastTime = now;
        }

#if defined(_WIN32) || defined(_WIN64)
        Sleep(10);
#else
        usleep(10000);
#endif
    }

    TCPServerStream_close(&server);
    return 0;
}
