// ===== TCPStream.c (Fixed for Stream Library Compatibility) =====
#include "TCPStream.h"
#include <string.h>
#include <stdio.h>

#include "TCPStreamMacro.h"

#if TCPSTREAM_LIB_LOG
    #include "Log.h"
#else
    #define logInfo(...)
    #define logError(...)
    #define logDebug(...)
    #define logWarn(...)
#endif

// ===== Platform Detection =====
#if defined(_WIN32) || defined(_WIN64)
    #define SOCKET_IS_VALID(s) ((s) != INVALID_SOCKET)
    #define SOCKET_CLOSE(s) closesocket(s)
#else
    #define SOCKET_IS_VALID(s) ((s) >= 0)
    #define SOCKET_CLOSE(s) close(s)
#endif

// ===== Internal Helpers =====
THREAD_RET TCPStream_pollThread(void* arg);
static void TCPStream_errorHandle(TCPStream* stream, int err);
static void TCPStream_handleDisconnect(TCPStream* stream);
static int TCPStream_setNonBlocking(TCPStream_Socket sock);

Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len);

// ===== Mutex helpers =====
#if STREAM_MUTEX
Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex);
Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex);
Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex);
Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex);

__defineMutexDriver();
#endif

// ===== Callback Setters =====
void TCPStream_onConnect(TCPStream* stream, TCPStream_OnConnectFn cb) { 
    if (stream) stream->OnConnect = cb; 
}
void TCPStream_onDisconnect(TCPStream* stream, TCPStream_OnDisconnectFn cb) { 
    if (stream) stream->OnDisconnect = cb; 
}
void TCPStream_onError(TCPStream* stream, TCPStream_OnErrorFn cb) { 
    if (stream) stream->OnError = cb; 
}
void TCPStream_setServerCallbacks(TCPStream* stream, void* serverContext, 
                                   TCPStream_ServerOnDisconnectFn onDisconnect, 
                                   TCPStream_ServerOnErrorFn onError) {
    if (!stream) return;
    stream->ServerContext = serverContext;
    stream->ServerOnDisconnect = onDisconnect;
    stream->ServerOnError = onError;
}

// ===== Reconnect =====
void TCPStream_enableReconnect(TCPStream* stream, uint8_t enable, uint32_t delay_ms) {
    if (!stream) return;
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
    if (!stream) return;
    stream->Connected = 0;
    if(stream->OnError) stream->OnError(stream, err);
    // Don't reset IO here - let disconnect handler do it
}

// ===== Handle Disconnect (cleanup and notify) =====
static void TCPStream_handleDisconnect(TCPStream* stream) {
    if (!stream) return;
    
    stream->Connected = 0;
    stream->Running = 0;
    
    // Close socket
    if (SOCKET_IS_VALID(stream->Socket)) {
        SOCKET_CLOSE(stream->Socket);
        stream->Socket = (TCPStream_Socket)(-1);
    }
    
#if !(defined(_WIN32) || defined(_WIN64))
    if (stream->EpollFD >= 0) {
        close(stream->EpollFD);
        stream->EpollFD = -1;
    }
#endif
    
    // Reset IO
    IStream_resetIO(&stream->Input);
    OStream_resetIO(&stream->Output);
    
    // Notify callbacks
    if (stream->OnDisconnect) {
        stream->OnDisconnect(stream);
    }
}

// ===== Transmit Function =====
Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len) {
    TCPStream* tcp = (TCPStream*) OStream_getDriverArgs(stream);
    if(!tcp || !SOCKET_IS_VALID(tcp->Socket) || !tcp->Connected) return Stream_NoTransmit;

#if defined(_WIN32) || defined(_WIN64)
    int sent = send(tcp->Socket, (const char*)buff, (int)len, 0);
    int lastErr = (sent < 0) ? WSAGetLastError() : 0;
#else
    int sent = send(tcp->Socket, buff, len, MSG_NOSIGNAL);
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
            TCPStream_handleDisconnect(tcp);
            return Stream_CustomError | lastErr;
        }
        return Stream_NoTransmit;
    }

    if(sent > 0) {
        // Use OStream_handle to properly advance write position
        return OStream_handle(stream, sent);
    }

    return Stream_Ok;
}

// ===== Poll Thread =====
THREAD_RET TCPStream_pollThread(void* arg) {
    TCPStream* stream = (TCPStream*)arg;
    if (!stream) return 0;
    
    stream->Running = 1;

    while (stream->Running) {
#if defined(_WIN32) || defined(_WIN64)
        // ===== Windows Path =====
        WSAPOLLFD fds[1];
        fds[0].fd = stream->Socket;
        fds[0].events = POLLIN | POLLOUT;
        int timeout = stream->Connected ? -1 : 100;
        int ret = WSAPoll(fds, 1, timeout);
        
        if (ret < 0) {
            int err = WSAGetLastError();
            if (err == WSAENOTSOCK) break;
            if (err != WSAEINTR) {
                TCPStream_errorHandle(stream, err);
                TCPStream_handleDisconnect(stream);
                if (!stream->AutoReconnect) return 0;
                // Reconnect
                Sleep(stream->ReconnectDelay);
                stream->Running = 1;
                TCPStream_init(stream, stream->Host, stream->Port,
                             stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                             stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                return 0;
            }
            continue;
        }
        
        // Check connection completion (timeout or event)
        if (!stream->Connected) {
            int optval = 0;
            int optlen = sizeof(optval);
            if (getsockopt(stream->Socket, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen) == 0) {
                if (optval == 0) {
                    // Connected!
                    stream->Connected = 1;
                    logInfo("TCPStream connected to %s:%d", stream->Host, stream->Port);
                    if (stream->OnConnect) stream->OnConnect(stream);
                } else {
                    // Connection failed
                    TCPStream_errorHandle(stream, optval);
                    SOCKET_CLOSE(stream->Socket);
                    stream->Socket = INVALID_SOCKET;
                    if (stream->AutoReconnect) {
                        Sleep(stream->ReconnectDelay);
                        stream->Running = 1;
                        TCPStream_init(stream, stream->Host, stream->Port,
                                     stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                     stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                    }
                    continue;
                }
            }
            if (ret == 0) continue; // Timeout, try again
        }
        
        // Check for events
        if (ret > 0 && stream->Connected) {
            // Read available data
            if (fds[0].revents & POLLIN) {
                uint8_t* buf = IStream_getDataPtr(&stream->Input);
                Stream_LenType space = IStream_directSpace(&stream->Input);
                
                if (buf && space > 0) {
                    int read_bytes = recv(stream->Socket, (char*)buf, (int)space, 0);
                    
                    if (read_bytes > 0) {
                        // KEY: Use IStream_handle to process received data
                        // This triggers the onReceive callback
                        stream->Input.Buffer.PendingBytes = read_bytes;
                        stream->Input.Buffer.InReceive = 1;
                        IStream_handle(&stream->Input, read_bytes);
                    } else if (read_bytes == 0) {
                        // Disconnected
                        TCPStream_handleDisconnect(stream);
                        if (!stream->AutoReconnect) return 0;
                        Sleep(stream->ReconnectDelay);
                        stream->Running = 1;
                        TCPStream_init(stream, stream->Host, stream->Port,
                                     stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                     stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                        return 0;
                    } else {
                        int err = WSAGetLastError();
                        if (err != WSAEWOULDBLOCK) {
                            TCPStream_errorHandle(stream, err);
                            TCPStream_handleDisconnect(stream);
                            if (!stream->AutoReconnect) return 0;
                            Sleep(stream->ReconnectDelay);
                            stream->Running = 1;
                            TCPStream_init(stream, stream->Host, stream->Port,
                                         stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                         stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                            return 0;
                        }
                    }
                }
            }
            
            // Write pending data
            if (fds[0].revents & POLLOUT) {
                OStream_handle(&stream->Output, 0);
                OStream_flush(&stream->Output);
            }
            
            // Handle errors
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                TCPStream_handleDisconnect(stream);
                if (!stream->AutoReconnect) return 0;
                Sleep(stream->ReconnectDelay);
                stream->Running = 1;
                TCPStream_init(stream, stream->Host, stream->Port,
                             stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                             stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                return 0;
            }
        }
        
#else
        // ===== Linux Path =====
        struct epoll_event events[2];
        int timeout = stream->Connected ? -1 : 100;
        int ret = epoll_wait(stream->EpollFD, events, 2, timeout);
        
        if (ret < 0) {
            if (errno != EINTR) {
                TCPStream_errorHandle(stream, errno);
                TCPStream_handleDisconnect(stream);
                if (!stream->AutoReconnect) return 0;
                usleep(stream->ReconnectDelay * 1000);
                stream->Running = 1;
                TCPStream_init(stream, stream->Host, stream->Port,
                             stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                             stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                return 0;
            }
            continue;
        }
        
        // Check connection completion on timeout
        if (ret == 0 && !stream->Connected) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(stream->Socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
                if (error == 0) {
                    // Connected!
                    stream->Connected = 1;
                    logInfo("TCPStream connected to %s:%d", stream->Host, stream->Port);
                    if (stream->OnConnect) stream->OnConnect(stream);
                } else {
                    TCPStream_errorHandle(stream, error);
                    SOCKET_CLOSE(stream->Socket);
                    stream->Socket = -1;
                    if (stream->AutoReconnect) {
                        usleep(stream->ReconnectDelay * 1000);
                        stream->Running = 1;
                        TCPStream_init(stream, stream->Host, stream->Port,
                                     stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                     stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                    }
                }
            }
            continue;
        }
        
        // Process epoll events
        for (int i = 0; i < ret; i++) {
            uint32_t ev = events[i].events;
            
            // Check connection completion via EPOLLOUT
            if (!stream->Connected && (ev & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(stream->Socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
                    if (error == 0) {
                        stream->Connected = 1;
                        logInfo("TCPStream connected to %s:%d", stream->Host, stream->Port);
                        if (stream->OnConnect) stream->OnConnect(stream);
                    } else {
                        TCPStream_errorHandle(stream, error);
                        SOCKET_CLOSE(stream->Socket);
                        stream->Socket = -1;
                        if (stream->AutoReconnect) {
                            usleep(stream->ReconnectDelay * 1000);
                            stream->Running = 1;
                            TCPStream_init(stream, stream->Host, stream->Port,
                                         stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                         stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                        }
                        break;
                    }
                }
            }
            
            // Read data
            if (stream->Connected && (ev & EPOLLIN)) {
                uint8_t* buf = IStream_getDataPtr(&stream->Input);
                Stream_LenType space = IStream_directSpace(&stream->Input);
                
                if (buf && space > 0) {
                    int read_bytes = read(stream->Socket, buf, space);
                    
                    if (read_bytes > 0) {
                        // KEY: Use IStream_handle to process received data
                        // This triggers the onReceive callback
                        stream->Input.Buffer.PendingBytes = read_bytes;
                        stream->Input.Buffer.InReceive = 1;
                        IStream_handle(&stream->Input, read_bytes);
                    } else if (read_bytes == 0) {
                        // Peer disconnected
                        TCPStream_handleDisconnect(stream);
                        if (!stream->AutoReconnect) return 0;
                        usleep(stream->ReconnectDelay * 1000);
                        stream->Running = 1;
                        TCPStream_init(stream, stream->Host, stream->Port,
                                     stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                     stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                        return 0;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            TCPStream_errorHandle(stream, errno);
                            TCPStream_handleDisconnect(stream);
                            if (!stream->AutoReconnect) return 0;
                            usleep(stream->ReconnectDelay * 1000);
                            stream->Running = 1;
                            TCPStream_init(stream, stream->Host, stream->Port,
                                         stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                         stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                            return 0;
                        }
                    }
                }
            }
            
            // Write data
            if (stream->Connected && (ev & EPOLLOUT)) {
                OStream_handle(&stream->Output, 0);
                OStream_flush(&stream->Output);
            }
            
            // Handle errors
            if (ev & (EPOLLERR | EPOLLHUP)) {
                if (stream->Connected) {
                    TCPStream_handleDisconnect(stream);
                    if (!stream->AutoReconnect) return 0;
                    usleep(stream->ReconnectDelay * 1000);
                    stream->Running = 1;
                    TCPStream_init(stream, stream->Host, stream->Port,
                                 stream->Input.Buffer.Data, stream->Input.Buffer.Size,
                                 stream->Output.Buffer.Data, stream->Output.Buffer.Size);
                    return 0;
                }
            }
        }
#endif
    }

    return 0;
}

// ===== Initialization Helper =====
static uint8_t TCPStream_internalInit(TCPStream* stream, const char* host, uint16_t port,
                                     uint8_t* rxBuff, Stream_LenType rxSize,
                                     uint8_t* txBuff, Stream_LenType txSize) {
    if (!stream) return 0;
    
    // Don't zero out the whole struct - preserve Args and callbacks
    stream->Host[0] = '\0';
    strncpy(stream->Host, host, sizeof(stream->Host) - 1);
    stream->Host[sizeof(stream->Host) - 1] = '\0';
    stream->Port = port;
    stream->Connected = 0;
    stream->Running = 1;
    stream->Socket = (TCPStream_Socket)(-1);
#if !(defined(_WIN32) || defined(_WIN64))
    stream->EpollFD = -1;
#endif

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        logError("WSAStartup failed");
        return 0;
    }
#endif

    TCPStream_Socket sock = socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32) || defined(_WIN64)
    if (sock == INVALID_SOCKET) return 0;
#else
    if (sock < 0) return 0;
#endif

    // Set SO_REUSEADDR
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 
#if defined(_WIN32) || defined(_WIN64)
               (const char*)&opt, sizeof(opt));
#else
               &opt, sizeof(opt));
#endif

    TCPStream_setNonBlocking(sock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#if defined(_WIN32) || defined(_WIN64)
    addr.sin_addr.s_addr = inet_addr(host);
#else
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(sock);
        return 0;
    }
#endif

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

#if defined(_WIN32) || defined(_WIN64)
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(sock);
            return 0;
        }
    }
#else
    if (ret < 0 && errno != EINPROGRESS) {
        close(sock);
        return 0;
    }
#endif
    stream->Socket = sock;

#if !(defined(_WIN32) || defined(_WIN64))
    // Setup epoll
    stream->EpollFD = epoll_create1(0);
    if (stream->EpollFD < 0) {
        close(sock);
        stream->Socket = -1;
        return 0;
    }
    
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = sock;
    if (epoll_ctl(stream->EpollFD, EPOLL_CTL_ADD, sock, &ev) < 0) {
        close(stream->EpollFD);
        close(sock);
        stream->EpollFD = -1;
        stream->Socket = -1;
        return 0;
    }
#endif

    // Initialize StreamIn with NO receive function (we handle it manually)
    IStream_init(&stream->Input, NULL, rxBuff, rxSize);
    IStream_setDriverArgs(&stream->Input, stream);
    
    // Initialize StreamOut with transmit function
    OStream_init(&stream->Output, TCPStream_transmit, txBuff, txSize);
    OStream_setDriverArgs(&stream->Output, stream);
    
    __initMutex(stream);

    // Start poll thread
#if defined(_WIN32) || defined(_WIN64)
    stream->Thread = (HANDLE)_beginthreadex(NULL, 0, 
        (unsigned(__stdcall*)(void*))TCPStream_pollThread, stream, 0, NULL);
    if (!stream->Thread) return 0;
#else
    if (pthread_create(&stream->Thread, NULL, TCPStream_pollThread, stream) != 0) {
        close(stream->EpollFD);
        close(sock);
        stream->EpollFD = -1;
        stream->Socket = -1;
        return 0;
    }
    pthread_detach(stream->Thread);
#endif

    logInfo("TCPStream initialized (non-blocking) to %s:%d", host, port);
    return 1;
}

// ===== Public Init =====
uint8_t TCPStream_init(TCPStream* stream, const char* address, uint16_t port,
                       uint8_t* rxBuff, Stream_LenType rxSize,
                       uint8_t* txBuff, Stream_LenType txSize) {
    // Save callbacks and server context before clearing
    TCPStream_OnConnectFn savedOnConnect = stream->OnConnect;
    TCPStream_OnDisconnectFn savedOnDisconnect = stream->OnDisconnect;
    TCPStream_OnErrorFn savedOnError = stream->OnError;
    void* savedServerContext = stream->ServerContext;
    TCPStream_ServerOnDisconnectFn savedServerOnDisconnect = stream->ServerOnDisconnect;
    TCPStream_ServerOnErrorFn savedServerOnError = stream->ServerOnError;
    void* savedArgs = stream->Args;
    uint8_t savedAutoReconnect = stream->AutoReconnect;
    uint32_t savedReconnectDelay = stream->ReconnectDelay;
    
    memset(stream, 0, sizeof(TCPStream));
    
    // Restore
    stream->OnConnect = savedOnConnect;
    stream->OnDisconnect = savedOnDisconnect;
    stream->OnError = savedOnError;
    stream->ServerContext = savedServerContext;
    stream->ServerOnDisconnect = savedServerOnDisconnect;
    stream->ServerOnError = savedServerOnError;
    stream->Args = savedArgs;
    stream->AutoReconnect = savedAutoReconnect;
    stream->ReconnectDelay = savedReconnectDelay;
    
    return TCPStream_internalInit(stream, address, port, rxBuff, rxSize, txBuff, txSize);
}

// ===== Public Init URI =====
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
    return TCPStream_init(stream, host, port, rxBuff, rxSize, txBuff, txSize);
}

// ===== Close =====
uint8_t TCPStream_close(TCPStream* stream) {
    if (!stream) return 0;
    
    stream->Running = 0;
    stream->Connected = 0;
    
#if defined(_WIN32) || defined(_WIN64)
    if(SOCKET_IS_VALID(stream->Socket)) {
        closesocket(stream->Socket);
        stream->Socket = INVALID_SOCKET;
    }
#else
    if(stream->Socket >= 0) {
        close(stream->Socket);
        stream->Socket = -1;
    }
    if(stream->EpollFD >= 0) {
        close(stream->EpollFD);
        stream->EpollFD = -1;
    }
#endif
    
    return 1;
}

// ===== Is Connected =====
uint8_t TCPStream_isConnected(TCPStream* stream) {
    return stream ? stream->Connected : 0;
}

#if STREAM_MUTEX
#include <errno.h>

Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex) {
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

Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex) {
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

Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex) {
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

Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex) {
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
