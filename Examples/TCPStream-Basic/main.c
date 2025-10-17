#include "TCPStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define RX_BUF_SIZE 1024
#define TX_BUF_SIZE 1024

uint8_t rxBuff[RX_BUF_SIZE];
uint8_t txBuff[TX_BUF_SIZE];

TCPStream stream;

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

int main() {
    // Initialize TCPStream
    if(!TCPStream_initUri(&stream, "127.0.0.1:9000", rxBuff, RX_BUF_SIZE, txBuff, TX_BUF_SIZE)) {
        printf("Failed to initialize TCPStream\n");
        return 1;
    }

    // Set callbacks
    TCPStream_onConnect(&stream, onConnect);
    TCPStream_onDisconnect(&stream, onDisconnect);
    TCPStream_onError(&stream, onError);

    // Enable auto reconnect every 3 seconds
    TCPStream_enableReconnect(&stream, 1, 3000);

    uint32_t counter = 0;

    while(1) {
        // --- Echo received data ---
        Stream_LenType len = IStream_available(&stream.Input);
        if(len > 0) {
            uint8_t* data = IStream_getDataPtr(&stream.Input);
            // Send back the same data
            OStream_write(&stream.Output, data, len);
            IStream_commit(&stream.Input, len); // consume data
        }

        // --- Send counter every 5 seconds ---
        static uint32_t lastTime = 0;
#if defined(_WIN32) || defined(_WIN64)
        uint32_t now = GetTickCount();
        if(now - lastTime >= 5000) {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "Counter: %u\n", counter++);
            OStream_write(&stream.Output, (uint8_t*)buf, n);
            lastTime = now;
        }
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if(now - lastTime >= 5000) {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "Counter: %u\n", counter++);
            OStream_write(&stream.Output, (uint8_t*)buf, n);
            lastTime = now;
        }
#endif

#if defined(_WIN32) || defined(_WIN64)
        Sleep(10); // small delay to reduce CPU usage
#else
        usleep(10000);
#endif
    }

    TCPStream_close(&stream);
    return 0;
}
