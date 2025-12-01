#include "TCPStream.h"
#include <string.h>
#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <process.h>
#  define THREAD_RET unsigned __stdcall
#else
#  include <stdlib.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define THREAD_RET void*
#endif

#if TCPSTREAM_LIB_LOG
#  include "Log.h"
#else
#  define logInfo(...)
#  define logError(...)
#endif

// ===== Internal Helpers =====
static void TCPStream_errorHandle(TCPStream* stream, int err);
static Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len);
static THREAD_RET TCPStream_pollThread(void* arg);
static int TCPStream_setNonBlocking(TCPStream_Socket sock);

// ===== Mutex helpers =====
#if STREAM_MUTEX
static Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex);
#endif

// Mutex macro mapping (unchanged semantics)
#if STREAM_MUTEX
    #if STREAM_MUTEX == STREAM_MUTEX_CUSTOM
        #define __initMutex(STREAM)                 IStream_setMutex(&(STREAM)->Input, TCPStream_mutexInit, TCPStream_mutexLock, TCPStream_mutexUnlock, TCPStream_mutexDeInit); \
                                                    OStream_setMutex(&(STREAM)->Output, TCPStream_mutexInit, TCPStream_mutexLock, TCPStream_mutexUnlock, TCPStream_mutexDeInit); \
                                                    IStream_mutexInit(&(STREAM)->Input); \
                                                    OStream_mutexInit(&(STREAM)->Output)
    #elif STREAM_MUTEX == STREAM_MUTEX_DRIVER
        static const Stream_MutexDriver TCPStream_MutexDriver = {
            .init = TCPStream_mutexInit,
            .lock = TCPStream_mutexLock,
            .unlock = TCPStream_mutexUnlock,
            .deinit = TCPStream_mutexDeInit
        };

        #define __initMutex(STREAM)                 IStream_setMutex(&(STREAM)->Input, &TCPStream_MutexDriver); \
                                                    OStream_setMutex(&(STREAM)->Output, &TCPStream_MutexDriver); \
                                                    IStream_mutexInit(&(STREAM)->Input); \
                                                    OStream_mutexInit(&(STREAM)->Output)
    #elif STREAM_MUTEX == STREAM_MUTEX_GLOBAL_DRIVER
        static const Stream_MutexDriver TCPStream_MutexDriver = {
            .init = TCPStream_mutexInit,
            .lock = TCPStream_mutexLock,
            .unlock = TCPStream_mutexUnlock,
            .deinit = TCPStream_mutexDeInit
        };

        #define __initMutex(STREAM)                 IStream_setMutex(&TCPStream_MutexDriver); \
                                                    OStream_setMutex(&TCPStream_MutexDriver); \
                                                    IStream_mutexInit(&(STREAM)->Input); \
                                                    OStream_mutexInit(&(STREAM)->Output)
    #endif

    #define __lockMutex(STREAM)                 Stream_mutexLock(&(STREAM)->Buffer)
    #define __unlockMutex(STREAM)               Stream_mutexUnlock(&(STREAM)->Buffer)
    #define __deinitMutex(STREAM)               Stream_mutexDeInit(&(STREAM)->Buffer)
#else
    #define __initMutex(STREAM)                 // No mutex
    #define __lockMutex(STREAM)                 // No mutex
    #define __unlockMutex(STREAM)               // No mutex
    #define __deinitMutex(STREAM)               // No mutex
#endif

// ===== Callback Setters =====
void TCPStream_onConnect(TCPStream* stream, TCPStream_OnConnectFn cb) { stream->OnConnect = cb; }
void TCPStream_onDisconnect(TCPStream* stream, TCPStream_OnDisconnectFn cb) { stream->OnDisconnect = cb; }
void TCPStream_onError(TCPStream* stream, TCPStream_OnErrorFn cb) { stream->OnError = cb; }

// ===== Reconnect =====
void TCPStream_enableReconnect(TCPStream* stream, uint8_t enable, uint32_t delay_ms) {
    stream->AutoReconnect = enable;
    stream->ReconnectDelay = delay_ms;
}

// ===== Socket Helpers =====
static int TCPStream_setNonBlocking(TCPStream_Socket sock) {
#if defined(_WIN32) || defined(_WIN64)
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if(flags < 0) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ===== Internal Error Handler =====
static void TCPStream_errorHandle(TCPStream* stream, int err) {
    stream->Connected = 0;
    if(stream->OnError) stream->OnError(stream, err);
    IStream_resetIO(&stream->Input);
    OStream_resetIO(&stream->Output);
}

// ===== Transmit Function =====
static Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len) {
    TCPStream* tcp = (TCPStream*) OStream_getDriverArgs(stream);
    if(!tcp || tcp->Socket <= 0 || !tcp->Connected) return Stream_NoTransmit;

#if defined(_WIN32) || defined(_WIN64)
    int sent = send(tcp->Socket, (const char*)buff, (int)len, 0);
    int lastErr = (sent < 0) ? WSAGetLastError() : 0;
#else
    int sent = send(tcp->Socket, buff, len, 0);
    int lastErr = (sent < 0) ? errno : 0;
#endif

    if(sent < 0) {
#if defined(_WIN32) || defined(_WIN64)
        if(lastErr != WSAEWOULDBLOCK)
#else
        if(lastErr != EAGAIN && lastErr != EWOULDBLOCK)
#endif
        {
            TCPStream_errorHandle(tcp, lastErr);
            return Stream_CustomError | lastErr;
        }
        return Stream_NoTransmit;
    }

    if(sent > 0) {
        stream->Buffer.InTransmit = 1;
        return OStream_handle(stream, sent);
    }

    return Stream_Ok;
}

// ===== Poll Thread =====
static THREAD_RET TCPStream_pollThread(void* arg) {
    TCPStream* stream = (TCPStream*)arg;
    stream->Running = 1;

    while(stream->Running) {
#if defined(_WIN32) || defined(_WIN64)
        WSAPOLLFD fds[1];
        fds[0].fd = stream->Socket;
        fds[0].events = POLLIN | POLLOUT;
        int timeout = INFINITE; // block indefinitely
        int ret = WSAPoll(fds, 1, timeout);
#else
        struct epoll_event events[2];
        int timeout = -1; // block indefinitely
        int ret = epoll_wait(stream->EpollFD, events, 2, timeout);
#endif
        if(ret < 0) {
#if defined(_WIN32) || defined(_WIN64)
            int err = WSAGetLastError();
            if(err != WSAEINTR) TCPStream_errorHandle(stream, err);
#else
            if(errno != EINTR) TCPStream_errorHandle(stream, errno);
#endif
            continue;
        }
        if(ret == 0) continue; // timeout

        // --- Read Data ---
        uint8_t* buf = IStream_getDataPtr(&stream->Input);
        Stream_LenType len = IStream_directSpace(&stream->Input);
        int read_bytes = 0;

#if defined(_WIN32) || defined(_WIN64)
        read_bytes = recv(stream->Socket, (char*)buf, (int)len, 0);
#else
        read_bytes = read(stream->Socket, buf, len);
#endif
        if(read_bytes > 0) {
            stream->Input.Buffer.InReceive = 1;
            stream->Input.Buffer.PendingBytes = read_bytes;
            IStream_handle(&stream->Input, read_bytes);
        } else if(read_bytes == 0) {
            // disconnected
            stream->Connected = 0;
            if(stream->OnDisconnect) stream->OnDisconnect(stream);
            if(stream->AutoReconnect) {
#if defined(_WIN32) || defined(_WIN64)
                closesocket(stream->Socket);
#else
                close(stream->Socket);
#endif
                stream->Socket = 0;
            }
        } else {
#if defined(_WIN32) || defined(_WIN64)
            int err = WSAGetLastError();
            if(err != WSAEWOULDBLOCK) TCPStream_errorHandle(stream, err);
#else
            if(errno != EAGAIN && errno != EWOULDBLOCK) TCPStream_errorHandle(stream, errno);
#endif
        }

        // --- Write Data ---
        OStream_handle(&stream->Output, 0);

        // --- Reconnect ---
        if(stream->AutoReconnect && !stream->Connected) {
#if defined(_WIN32) || defined(_WIN64)
            Sleep(stream->ReconnectDelay);
#else
            usleep(stream->ReconnectDelay * 1000);
#endif
            TCPStream_init(stream, stream->Host, stream->Port,
                           IStream_getDataPtr(&stream->Input), stream->Input.Buffer.Size,
                           OStream_getDataPtr(&stream->Output), stream->Output.Buffer.Size);
        }
    }

    return 0;
}

// ===== Initialization Helper =====
static uint8_t TCPStream_internalInit(TCPStream* stream, const char* host, uint16_t port,
                                     uint8_t* rxBuff, Stream_LenType rxSize,
                                     uint8_t* txBuff, Stream_LenType txSize) {
    strncpy(stream->Host, host, sizeof(stream->Host)-1);
    stream->Port = port;
    stream->Connected = 0;

#if defined(_WIN32) || defined(_WIN64)
    // WSAStartup per instance
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        logError("WSAStartup failed");
        return 0;
    }
#endif

    TCPStream_Socket sock = socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32) || defined(_WIN64)
    if(sock == INVALID_SOCKET) return 0;
#else
    if(sock < 0) return 0;
#endif

    TCPStream_setNonBlocking(sock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#if defined(_WIN32) || defined(_WIN64)
    addr.sin_addr.s_addr = inet_addr(host);
#else
    if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(sock);
        return 0;
    }
#endif

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
#if defined(_WIN32) || defined(_WIN64)
    if(ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if(err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(sock);
            TCPStream_errorHandle(stream, err);
            return 0;
        }
    }
#else
    if(ret < 0 && errno != EINPROGRESS) {
        close(sock);
        TCPStream_errorHandle(stream, errno);
        return 0;
    }
#endif

    stream->Socket = sock;

#if !(defined(_WIN32) || defined(_WIN64))
    // setup epoll
    stream->EpollFD = epoll_create1(0);
    if(stream->EpollFD < 0) {
        close(sock);
        return 0;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = sock;
    epoll_ctl(stream->EpollFD, EPOLL_CTL_ADD, sock, &ev);
#endif

    // Initialize streams
    IStream_init(&stream->Input, NULL, rxBuff, rxSize);
    IStream_setDriverArgs(&stream->Input, stream);
    OStream_init(&stream->Output, TCPStream_transmit, txBuff, txSize);
    OStream_setDriverArgs(&stream->Output, stream);

    __initMutex(stream);

    // Start poll thread
#if defined(_WIN32) || defined(_WIN64)
    stream->Thread = (HANDLE)_beginthreadex(NULL, 0, TCPStream_pollThread, stream, 0, NULL);
    if(!stream->Thread) return 0;
#else
    if(pthread_create(&stream->Thread, NULL, TCPStream_pollThread, stream) != 0) return 0;
    pthread_detach(stream->Thread);
#endif

    stream->Connected = 1;
    if(stream->OnConnect) stream->OnConnect(stream);

    logInfo("TCPStream connected to %s:%d", host, port);
    return 1;
}

// ===== Public Init =====
uint8_t TCPStream_init(TCPStream* stream, const char* address, uint16_t port,
                       uint8_t* rxBuff, Stream_LenType rxSize,
                       uint8_t* txBuff, Stream_LenType txSize) {
    return TCPStream_internalInit(stream, address, port, rxBuff, rxSize, txBuff, txSize);
}

// Parse URI "host:port"
uint8_t TCPStream_initUri(TCPStream* stream, const char* uri,
                          uint8_t* rxBuff, Stream_LenType rxSize,
                          uint8_t* txBuff, Stream_LenType txSize) {
    char host[128];
    uint16_t port = 0;
    const char* sep = strchr(uri, ':');
    if(!sep) return 0;
    size_t len = sep - uri;
    if(len >= sizeof(host)) return 0;
    memcpy(host, uri, len);
    host[len] = '\0';
    port = (uint16_t)atoi(sep+1);
    return TCPStream_internalInit(stream, host, port, rxBuff, rxSize, txBuff, txSize);
}

// ===== Close =====
uint8_t TCPStream_close(TCPStream* stream) {
    stream->Running = 0;
#if defined(_WIN32) || defined(_WIN64)
    if(stream->Socket) closesocket(stream->Socket); // unblock WSAPoll
    if(stream->Thread) CloseHandle(stream->Thread);
    WSACleanup();
#else
    if(stream->Socket > 0) close(stream->Socket);    // unblock epoll_wait
    if(stream->EpollFD > 0) close(stream->EpollFD);
#endif
    IStream_deinit(&stream->Input);
    OStream_deinit(&stream->Output);
    stream->Connected = 0;
    return 1;
}

// ===== Is Connected =====
uint8_t TCPStream_isConnected(TCPStream* stream) {
    return stream->Connected;
}

#if STREAM_MUTEX
#include <errno.h>

static Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream) {
        return (Stream_MutexResult) EINVAL;
    }

#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*) malloc(sizeof(CRITICAL_SECTION));
    if (!cs) {
        return (Stream_MutexResult) ENOMEM;
    }
    InitializeCriticalSection(cs);
    stream->Mutex = (void*)cs;
#else
    pthread_mutex_t* new_mutex = malloc(sizeof(pthread_mutex_t));
    if (!new_mutex) {
        return (Stream_MutexResult) ENOMEM;
    }

    pthread_mutexattr_t attr;
    int ret = pthread_mutexattr_init(&attr);
    if (ret != 0) {
        free(new_mutex);
        return (Stream_MutexResult) ret;
    }

    ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (ret != 0) {
        pthread_mutexattr_destroy(&attr);
        free(new_mutex);
        return (Stream_MutexResult) ret;
    }

    ret = pthread_mutex_init(new_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (ret != 0) {
        free(new_mutex);
        return (Stream_MutexResult) ret;
    }

    stream->Mutex = (void*)new_mutex;
#endif

    return Stream_Ok;
}

static Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream || !stream->Mutex) {
        return (Stream_MutexResult) EINVAL;
    }

#if defined(_WIN32) || defined(_WIN64)
    EnterCriticalSection((CRITICAL_SECTION*)stream->Mutex);
    return Stream_Ok;
#else
    return (Stream_MutexResult) pthread_mutex_lock((pthread_mutex_t*)stream->Mutex);
#endif
}

static Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream || !stream->Mutex) {
        return (Stream_MutexResult) EINVAL;
    }

#if defined(_WIN32) || defined(_WIN64)
    LeaveCriticalSection((CRITICAL_SECTION*)stream->Mutex);
    return Stream_Ok;
#else
    return (Stream_MutexResult) pthread_mutex_unlock((pthread_mutex_t*)stream->Mutex);
#endif
}

static Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream) {
        return (Stream_MutexResult) EINVAL;
    }

#if defined(_WIN32) || defined(_WIN64)
    if (stream->Mutex) {
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
    if (ret != 0) {
        return (Stream_MutexResult) ret;
    }
    return Stream_Ok;
#endif
}
#endif // STREAM_MUTEX
