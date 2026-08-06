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

// Forward declarations
struct __TCPServerStream;
typedef struct __TCPServerStream TCPServerStream;
struct __TCPStream;
typedef struct __TCPStream TCPStream;

// ===== Callback Typedefs =====
typedef void (*TCPServerStream_OnClientConnectFn)(TCPServerStream* server, TCPStream* client);
typedef void (*TCPServerStream_OnClientDisconnectFn)(TCPServerStream* server, TCPStream* client);
typedef void (*TCPServerStream_OnClientErrorFn)(TCPServerStream* server, TCPStream* client, int error);

// ===== Internal Client Node (for linked list) =====
typedef struct TCPClientNode {
    TCPStream* client;
    struct TCPClientNode* next;
    struct TCPClientNode* prev;
    uint8_t active;
} TCPClientNode;

// ===== TCPServerStream Structure =====
struct __TCPServerStream {
    void*                                   Args;
    TCPStream_Socket                        ListenSocket;

#if defined(_WIN32) || defined(_WIN64)
    HANDLE                                  Thread;
    CRITICAL_SECTION                        Mutex;
#else
    pthread_t                               Thread;
    pthread_mutex_t                         Mutex;
#endif

    uint8_t                                 Running;
    char                                    Host[128];
    uint16_t                                Port;
    uint16_t                                MaxClients;
    Stream_LenType                          TxBufferSize;
    Stream_LenType                          RxBufferSize;
    TCPServerStream_Mode                    Mode;

    // Callbacks
    TCPServerStream_OnClientConnectFn       OnClientConnect;
    TCPServerStream_OnClientDisconnectFn    OnClientDisconnect;
    TCPServerStream_OnClientErrorFn         OnClientError;

    // Client management - using linked list for better performance
    TCPClientNode*                          ClientList;
    uint16_t                                ClientCount;
    uint16_t                                CurrentClients;
};

// ===== Public API =====

// --- Initialization ---
uint8_t TCPServerStream_init(
    TCPServerStream* server,
    const char* host,
    uint16_t port,
    uint16_t maxClients,
    Stream_LenType rxBufferSize,
    Stream_LenType txBufferSize,
    TCPServerStream_Mode mode
);

// --- Lifecycle ---
uint8_t TCPServerStream_close(TCPServerStream* server);

// --- Callback Registration ---
void TCPServerStream_onClientConnect(TCPServerStream* server, TCPServerStream_OnClientConnectFn cb);
void TCPServerStream_onClientDisconnect(TCPServerStream* server, TCPServerStream_OnClientDisconnectFn cb);
void TCPServerStream_onClientError(TCPServerStream* server, TCPServerStream_OnClientErrorFn cb);

// --- Client Management ---
uint16_t TCPServerStream_getClientCount(TCPServerStream* server);
void TCPServerStream_broadcast(TCPServerStream* server, const uint8_t* data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // _TCP_SERVER_STREAM_H_
