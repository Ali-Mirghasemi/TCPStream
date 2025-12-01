#ifndef _TCP_SERVER_STREAM_H_
#define _TCP_SERVER_STREAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "TCPStream.h"

// ===== Server Mode =====
typedef enum {
    TCPServerStream_Mode_Epoll = 0,
    TCPServerStream_Mode_ThreadPerClient
} TCPServerStream_Mode;

// Forward declaration
struct __TCPServerStream;
typedef struct __TCPServerStream TCPServerStream;

// ===== Callback Typedefs =====
typedef void (*TCPServerStream_OnClientConnectFn)(TCPServerStream* server, TCPStream* client);

// ===== TCPServerStream Structure =====
struct __TCPServerStream {
    void* Args;

    TCPStream_Socket ListenSocket;

#if defined(_WIN32) || defined(_WIN64)
    HANDLE Thread;
    CRITICAL_SECTION Mutex;
#else
    pthread_t Thread;
    pthread_mutex_t Mutex;
#endif

    uint8_t Running;
    char Host[128];
    uint16_t Port;
    uint16_t MaxClients;
    TCPServerStream_Mode Mode;

    TCPServerStream_OnClientConnectFn OnClientConnect;

    TCPStream** Clients; // array of TCPStream pointers
    uint16_t ClientCount;
};

// ===== Public API =====

// --- Initialization ---
uint8_t TCPServerStream_init(
    TCPServerStream* server,
    const char* host,
    uint16_t port,
    uint16_t maxClients,
    TCPServerStream_Mode mode
);

// --- Lifecycle ---
uint8_t TCPServerStream_close(TCPServerStream* server);

// --- Callback Registration ---
void TCPServerStream_onClientConnect(TCPServerStream* server, TCPServerStream_OnClientConnectFn cb);

#ifdef __cplusplus
}
#endif

#endif // TCP_SERVER_STREAM_H
