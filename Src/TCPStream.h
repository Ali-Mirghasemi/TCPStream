#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "InputStream.h"
#include "OutputStream.h"

#if defined(_WIN32) || defined(_WIN64)
    #define WIN32_LEAN_AND_MEAN
    #define _WIN32_WINNT 0x0600
    
    #ifndef _WINSOCKAPI_
        #define _WINSOCKAPI_   // Prevent winsock.h from being included by windows.h
    #endif

    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <process.h>
    typedef SOCKET TCPStream_Socket;
    #define THREAD_RET unsigned __stdcall
#else
    #include <pthread.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/epoll.h>
    typedef int TCPStream_Socket;
    #include <stdlib.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define THREAD_RET void*
#endif

// Forward declaration
struct __TCPStream;
typedef struct __TCPStream TCPStream;

// ===== Callback Typedefs =====
typedef void (*TCPStream_OnConnectFn)(TCPStream* stream);
typedef void (*TCPStream_OnDisconnectFn)(TCPStream* stream);
typedef void (*TCPStream_OnErrorFn)(TCPStream* stream, int err);

// ===== TCPStream Structure =====
struct __TCPStream {
    void*                   Args;
    TCPStream_Socket        Socket;
    StreamIn                Input;
    StreamOut               Output;

#if defined(_WIN32) || defined(_WIN64)
    HANDLE                  Thread;
    CRITICAL_SECTION        Mutex;
#else
    pthread_t               Thread;
    pthread_mutex_t         Mutex;
    int                     EpollFD;
#endif

    uint8_t                 Connected;
    uint8_t                 AutoReconnect;
    uint32_t                ReconnectDelay;      // in milliseconds

    char                    Host[128];
    uint16_t                Port;

    TCPStream_OnConnectFn    OnConnect;
    TCPStream_OnDisconnectFn OnDisconnect;
    TCPStream_OnErrorFn      OnError;

    uint8_t                 Running;
};

// ===== Public API =====

// --- Initialization ---
uint8_t TCPStream_init(
    TCPStream*      stream,
    const char*     address,    // "192.168.1.10"
    uint16_t        port,       // 8080
    uint8_t*        rxBuff,
    Stream_LenType  rxBuffSize,
    uint8_t*        txBuff,
    Stream_LenType  txBuffSize
);

uint8_t TCPStream_initUri(
    TCPStream*      stream,
    const char*     uri,        // "example.com:8080"
    uint8_t*        rxBuff,
    Stream_LenType  rxBuffSize,
    uint8_t*        txBuff,
    Stream_LenType  txBuffSize
);

// --- Lifecycle ---
uint8_t TCPStream_close(TCPStream* stream);
uint8_t TCPStream_isConnected(TCPStream* stream);

// --- Reconnect ---
void TCPStream_enableReconnect(TCPStream* stream, uint8_t enable, uint32_t delay_ms);

// --- Callback Registration ---
void TCPStream_onConnect(TCPStream* stream, TCPStream_OnConnectFn cb);
void TCPStream_onDisconnect(TCPStream* stream, TCPStream_OnDisconnectFn cb);
void TCPStream_onError(TCPStream* stream, TCPStream_OnErrorFn cb);

#ifdef __cplusplus
}
#endif

#endif // TCP_STREAM_H
