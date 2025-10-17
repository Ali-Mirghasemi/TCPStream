#ifndef _TCP_STREAM_H_
#define _TCP_STREAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "InputStream.h"
#include "OutputStream.h"
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET TCP_SOCKET;
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    typedef int TCP_SOCKET;
#endif

struct __TCPStream;
typedef struct __TCPStream TCPStream;

// Event codes for callback
typedef enum {
    TCPStream_Event_Connected = 1,
    TCPStream_Event_ConnectFailed,
    TCPStream_Event_Disconnected
} TCPStream_Event;

// Event callback signature: event + platform errno (or 0)
typedef void (*TCPStream_EventCb)(TCPStream* stream, TCPStream_Event event, int err, void* userArg);

struct __TCPStream {
    void*           Args;
    intptr_t        Context;        // socket (cast to intptr_t) or -1
    StreamIn        Input;
    StreamOut       Output;

#if defined(_WIN32) || defined(_WIN64)
    HANDLE          PollThread;
    HANDLE          StopEvent;
#else
    pthread_t       PollThread;
#endif

    // reconnect policy
    uint8_t         ReconnectEnabled;
    uint32_t        ReconnectDelayMs;

    // event callback
    TCPStream_EventCb EventCb;
    void*           EventCbArg;

    // internal state
    uint8_t         Connected;
};

// Primary init with host + port
uint8_t TCPStream_init(
    TCPStream*      stream,
    const char*     address,    // hostname or IP
    uint16_t        port,
    uint8_t*        rxBuff,
    Stream_LenType  rxBuffSize,
    uint8_t*        txBuff,
    Stream_LenType  txBuffSize
);

// Alternative init taking "host:port" URI
uint8_t TCPStream_initUri(
    TCPStream*      stream,
    const char*     hostport,   // "host:port" or "ip:port"
    uint8_t*        rxBuff,
    Stream_LenType  rxBuffSize,
    uint8_t*        txBuff,
    Stream_LenType  txBuffSize
);

uint8_t TCPStream_close(TCPStream* stream);

uint8_t TCPStream_isConnected(TCPStream* stream);

// Configure reconnect
void TCPStream_setReconnect(TCPStream* stream, uint8_t enable, uint32_t delay_ms);

// Register event callback
void TCPStream_setEventCallback(TCPStream* stream, TCPStream_EventCb cb, void* userArg);

#ifdef __cplusplus
};
#endif

#endif
