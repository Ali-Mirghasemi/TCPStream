#include "TCPServerStream.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if TCPSTREAM_LIB_LOG
    #include "Log.h"
#else
    #define logInfo(...)
    #define logError(...)
#endif

#if defined(_WIN32) || defined(_WIN64)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <process.h>
    typedef SOCKET ServerSocket;
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/epoll.h>
    typedef int ServerSocket;
#endif

#include "TCPStreamMacro.h"

#define DEFAULT_CLIENT_BUFF_SIZE 1024

// ===== Internal Helpers =====
extern THREAD_RET TCPStream_pollThread(void* arg);
static THREAD_RET TCPServerStream_acceptThread(void* arg);
static void TCPServerStream_removeClient(TCPServerStream* server, TCPStream* client);
static int TCPServerStream_setNonBlocking(TCPStream_Socket sock);

extern Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len);

// ===== Mutex Helpers =====
#if STREAM_MUTEX
static Stream_MutexResult TCPServerStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPServerStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPServerStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPServerStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex);

extern Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex);
extern Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex);
extern Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex);
extern Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex);

extern Stream_MutexDriver TCPStream_MutexDriver;
#endif

// ===== Callback Setter =====
void TCPServerStream_onClientConnect(TCPServerStream* server, TCPServerStream_OnClientConnectFn cb) {
    server->OnClientConnect = cb;
}

// ===== Socket Helpers =====
static int TCPServerStream_setNonBlocking(TCPStream_Socket sock) {
#if defined(_WIN32) || defined(_WIN64)
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if(flags < 0) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ===== Server Initialization =====
uint8_t TCPServerStream_init(TCPServerStream* server, const char* host, uint16_t port,
                             uint16_t maxClients, TCPServerStream_Mode mode) {
    if(!server) return 0;
    memset(server, 0, sizeof(TCPServerStream));

    strncpy(server->Host, host, sizeof(server->Host)-1);
    server->Port = port;
    server->MaxClients = maxClients;
    server->Mode = mode;

    server->Clients = (TCPStream**)malloc(sizeof(TCPStream*) * maxClients);
    if(!server->Clients) return 0;
    memset(server->Clients, 0, sizeof(TCPStream*) * maxClients);

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        logError("WSAStartup failed");
        return 0;
    }
#endif

    server->ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32) || defined(_WIN64)
    if(server->ListenSocket == INVALID_SOCKET) return 0;
#else
    if(server->ListenSocket < 0) return 0;
#endif

    TCPServerStream_setNonBlocking(server->ListenSocket);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#if defined(_WIN32) || defined(_WIN64)
    addr.sin_addr.s_addr = inet_addr(host);
#else
    if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) return 0;
#endif

    if(bind(server->ListenSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        logError("bind failed");
        return 0;
    }

    if(listen(server->ListenSocket, maxClients) < 0) {
        logError("listen failed");
        return 0;
    }

    server->Running = 1;

    // Start accept thread
#if defined(_WIN32) || defined(_WIN64)
    server->Thread = (HANDLE)_beginthreadex(NULL, 0, TCPServerStream_acceptThread, server, 0, NULL);
    if(!server->Thread) return 0;
#else
    if(pthread_create(&server->Thread, NULL, TCPServerStream_acceptThread, server) != 0) return 0;
    pthread_detach(server->Thread);
#endif

    logInfo("TCPServerStream listening on %s:%d", host, port);
    return 1;
}

// ===== Remove Client =====
static void TCPServerStream_removeClient(TCPServerStream* server, TCPStream* client) {
    for(uint16_t i = 0; i < server->MaxClients; i++) {
        if(server->Clients[i] == client) {
            server->Clients[i] = NULL;
            break;
        }
    }

    if(client) {
        if(client->Input.Buffer.Data) free(client->Input.Buffer.Data);
        if(client->Output.Buffer.Data) free(client->Output.Buffer.Data);
        TCPStream_close(client);
        free(client);
    }
}

// ===== Accept Thread =====
static THREAD_RET TCPServerStream_acceptThread(void* arg) {
    TCPServerStream* server = (TCPServerStream*)arg;
    while(server->Running) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);

        // --- Blocking accept ---
        TCPStream_Socket clientSock = accept(server->ListenSocket, (struct sockaddr*)&clientAddr, &addrLen);

        if(!server->Running) break; // server closed while waiting

#if defined(_WIN32) || defined(_WIN64)
        if(clientSock == INVALID_SOCKET) {
            int err = WSAGetLastError();
            logError("accept failed: %d", err);
            continue;
        }
#else
        if(clientSock < 0) {
            logError("accept failed: %d", errno);
            continue;
        }
#endif

        // --- Create TCPStream for client ---
        TCPStream* client = (TCPStream*)malloc(sizeof(TCPStream));
        if(!client) {
#if defined(_WIN32) || defined(_WIN64)
            closesocket(clientSock);
#else
            close(clientSock);
#endif
            continue;
        }
        memset(client, 0, sizeof(TCPStream));
        client->Socket = clientSock;
        client->Connected = 1;

        // Allocate RX/TX buffers
        client->Input.Buffer.Size = DEFAULT_CLIENT_BUFF_SIZE;
        client->Output.Buffer.Size = DEFAULT_CLIENT_BUFF_SIZE;
        client->Input.Buffer.Data = (uint8_t*)malloc(client->Input.Buffer.Size);
        client->Output.Buffer.Data = (uint8_t*)malloc(client->Output.Buffer.Size);
        if(!client->Input.Buffer.Data || !client->Output.Buffer.Data) {
            if(client->Input.Buffer.Data) free(client->Input.Buffer.Data);
            if(client->Output.Buffer.Data) free(client->Output.Buffer.Data);
            free(client);
#if defined(_WIN32) || defined(_WIN64)
            closesocket(clientSock);
#else
            close(clientSock);
#endif
            continue;
        }

        // Initialize Stream buffers
        IStream_init(&client->Input, NULL, client->Input.Buffer.Data, client->Input.Buffer.Size);
        OStream_init(&client->Output, TCPStream_transmit, client->Output.Buffer.Data, client->Output.Buffer.Size);
        OStream_setDriverArgs(&client->Output, client);
        IStream_setDriverArgs(&client->Input, client);
        __initMutex(client);

        // Add to server list
        for(uint16_t i = 0; i < server->MaxClients; i++) {
            if(server->Clients[i] == NULL) {
                server->Clients[i] = client;
                server->ClientCount++;
                break;
            }
        }

        // Start poll thread
    #if defined(_WIN32) || defined(_WIN64)
        client->Thread = (HANDLE)_beginthreadex(NULL, 0, TCPStream_pollThread, client, 0, NULL);
        if (!client->Thread) {
            // Free Client
            TCPServerStream_removeClient(server, client);
        }
    #else
        if (pthread_create(&client->Thread, NULL, TCPStream_pollThread, client) != 0) {
            // Free Client
            TCPServerStream_removeClient(server, client);
        }
        pthread_detach(client->Thread);
    #endif

        // Fire server-level callback
        if(server->OnClientConnect) server->OnClientConnect(server, client);
    }

    return 0;
}

// ===== Close Server =====
uint8_t TCPServerStream_close(TCPServerStream* server) {
    if(!server) return 0;
    server->Running = 0;

#if defined(_WIN32) || defined(_WIN64)
    if(server->ListenSocket) closesocket(server->ListenSocket);
    if(server->Thread) CloseHandle(server->Thread);
    WSACleanup();
#else
    if(server->ListenSocket > 0) close(server->ListenSocket);
#endif

    // Close all clients
    if(server->Clients) {
        for(uint16_t i = 0; i < server->MaxClients; i++) {
            if(server->Clients[i]) TCPServerStream_removeClient(server, server->Clients[i]);
        }
        free(server->Clients);
        server->Clients = NULL;
    }

    server->ClientCount = 0;
    return 1;
}

#if STREAM_MUTEX
static Stream_MutexResult TCPServerStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream) return (Stream_MutexResult)EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*) malloc(sizeof(CRITICAL_SECTION));
    if(!cs) return (Stream_MutexResult)ENOMEM;
    InitializeCriticalSection(cs);
    stream->Mutex = (void*)cs;
#else
    pthread_mutex_t* new_mutex = malloc(sizeof(pthread_mutex_t));
    if(!new_mutex) return (Stream_MutexResult)ENOMEM;
    pthread_mutexattr_t attr;
    int ret = pthread_mutexattr_init(&attr);
    if(ret != 0) { free(new_mutex); return (Stream_MutexResult)ret; }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(new_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if(ret != 0) { free(new_mutex); return (Stream_MutexResult)ret; }
    stream->Mutex = (void*)new_mutex;
#endif
    return Stream_Ok;
}

static Stream_MutexResult TCPServerStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex) {
    if(!stream || !stream->Mutex) return (Stream_MutexResult)EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    EnterCriticalSection((CRITICAL_SECTION*)stream->Mutex);
    return Stream_Ok;
#else
    return (Stream_MutexResult)pthread_mutex_lock((pthread_mutex_t*)stream->Mutex);
#endif
}

static Stream_MutexResult TCPServerStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex) {
    if(!stream || !stream->Mutex) return (Stream_MutexResult)EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    LeaveCriticalSection((CRITICAL_SECTION*)stream->Mutex);
    return Stream_Ok;
#else
    return (Stream_MutexResult)pthread_mutex_unlock((pthread_mutex_t*)stream->Mutex);
#endif
}

static Stream_MutexResult TCPServerStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex) {
    if(!stream) return (Stream_MutexResult)EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    if(stream->Mutex) {
        CRITICAL_SECTION* cs = (CRITICAL_SECTION*)stream->Mutex;
        DeleteCriticalSection(cs);
        free(cs);
        stream->Mutex = NULL;
    }
    return Stream_Ok;
#else
    pthread_mutex_t* mutex_ptr = (pthread_mutex_t*)stream->Mutex;
    int ret = pthread_mutex_destroy(mutex_ptr);
    free(mutex_ptr);
    stream->Mutex = NULL;
    if(ret != 0) return (Stream_MutexResult)ret;
    return Stream_Ok;
#endif
}
#endif // STREAM_MUTEX
