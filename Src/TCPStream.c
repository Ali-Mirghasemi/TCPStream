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
    #include <windows.h>
    #define SOCKET_IS_VALID(s) ((s) != INVALID_SOCKET)
    #define SOCKET_CLOSE(s) do { if((s) != INVALID_SOCKET) { closesocket(s); (s) = INVALID_SOCKET; } } while(0)
    #define SOCKET_INVALID INVALID_SOCKET
    #define SLEEP_MS(ms) Sleep(ms)
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/epoll.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    
    #define SOCKET_IS_VALID(s) ((s) >= 0)
    #define SOCKET_CLOSE(s) do { if((s) >= 0) { close(s); (s) = -1; } } while(0)
    #define SOCKET_INVALID (-1)
    #define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

// ===== Internal Helpers =====
THREAD_RET TCPStream_pollThread(void* arg);
static void TCPStream_errorHandle(TCPStream* stream, int err);
static void TCPStream_handleDisconnect(TCPStream* stream);
static int TCPStream_setNonBlocking(TCPStream_Socket sock);
static int TCPStream_tryConnect(TCPStream* stream);

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
    if(stream->OnError) stream->OnError(stream, err);
}

// ===== Handle Disconnect =====
static void TCPStream_handleDisconnect(TCPStream* stream) {
    if (!stream) return;
    
    int wasConnected = stream->Connected;
    stream->Connected = 0;
    
    // Close socket
    SOCKET_CLOSE(stream->Socket);
    
#if !(defined(_WIN32) || defined(_WIN64))
    if (stream->EpollFD >= 0) {
        close(stream->EpollFD);
        stream->EpollFD = -1;
    }
#endif
    
    // Reset IO state
    IStream_resetIO(&stream->Input);
    OStream_resetIO(&stream->Output);
    
    // Notify disconnect callback
    if (wasConnected && stream->OnDisconnect) {
        stream->OnDisconnect(stream);
    }
}

// ===== Try Connect (non-blocking) =====
static int TCPStream_tryConnect(TCPStream* stream) {
    if (!stream) return 0;
    
    // Clean up any old socket
    SOCKET_CLOSE(stream->Socket);
#if !(defined(_WIN32) || defined(_WIN64))
    if (stream->EpollFD >= 0) {
        close(stream->EpollFD);
        stream->EpollFD = -1;
    }
#endif
    
    TCPStream_Socket sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!SOCKET_IS_VALID(sock)) {
        logError("socket creation failed during reconnect");
        return 0;
    }

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
    addr.sin_port = htons(stream->Port);
#if defined(_WIN32) || defined(_WIN64)
    addr.sin_addr.s_addr = inet_addr(stream->Host);
#else
    if (inet_pton(AF_INET, stream->Host, &addr.sin_addr) <= 0) {
        SOCKET_CLOSE(sock);
        return 0;
    }
#endif

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

#if defined(_WIN32) || defined(_WIN64)
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            SOCKET_CLOSE(sock);
            TCPStream_errorHandle(stream, err);
            return 0;
        }
    }
#else
    if (ret < 0 && errno != EINPROGRESS) {
        SOCKET_CLOSE(sock);
        TCPStream_errorHandle(stream, errno);
        return 0;
    }
#endif
    
    stream->Socket = sock;
    stream->Connected = 0;

#if !(defined(_WIN32) || defined(_WIN64))
    // Setup epoll
    stream->EpollFD = epoll_create1(0);
    if (stream->EpollFD < 0) {
        SOCKET_CLOSE(sock);
        return 0;
    }
    
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = sock;
    if (epoll_ctl(stream->EpollFD, EPOLL_CTL_ADD, sock, &ev) < 0) {
        close(stream->EpollFD);
        stream->EpollFD = -1;
        SOCKET_CLOSE(sock);
        return 0;
    }
#endif

    return 1;
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
            return Stream_CustomError | lastErr;
        }
        return Stream_NoTransmit;
    }

    if(sent > 0) {
        return OStream_handle(stream, sent);
    }

    return Stream_Ok;
}

// ===== Poll Thread (with reconnect loop) =====
THREAD_RET TCPStream_pollThread(void* arg) {
    TCPStream* stream = (TCPStream*)arg;
    if (!stream) return 0;
    
    stream->Running = 1;

    // Main poll loop - handles reconnection internally
    while (stream->Running) {
        
        // Check if we need to (re)connect
        if (!SOCKET_IS_VALID(stream->Socket)) {
            if (!stream->AutoReconnect) {
                // No reconnect - exit
                break;
            }
            
            logDebug("Attempting reconnect to %s:%d in %ums...", 
                     stream->Host, stream->Port, stream->ReconnectDelay);
            SLEEP_MS(stream->ReconnectDelay);
            
            if (!stream->Running) break;
            
            if (!TCPStream_tryConnect(stream)) {
                // Connection attempt failed - will retry after delay
                logDebug("Reconnect attempt failed, retrying...");
                continue;
            }
        }
        
#if defined(_WIN32) || defined(_WIN64)
        // ===== Windows Poll =====
        WSAPOLLFD fds[1];
        fds[0].fd = stream->Socket;
        fds[0].events = POLLIN | POLLOUT;
        // Use shorter timeout for connecting state so we can check SO_ERROR
        int timeout = stream->Connected ? -1 : 100;
        int ret = WSAPoll(fds, 1, timeout);
        
        if (ret < 0) {
            int err = WSAGetLastError();
            if (err == WSAENOTSOCK) {
                stream->Socket = INVALID_SOCKET;
                TCPStream_handleDisconnect(stream);
                continue; // Will trigger reconnect
            }
            if (err != WSAEINTR) {
                TCPStream_errorHandle(stream, err);
            }
            continue;
        }
        
        // Check connection status
        if (!stream->Connected && SOCKET_IS_VALID(stream->Socket)) {
            int optval = 0;
            int optlen = sizeof(optval);
            if (getsockopt(stream->Socket, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen) == 0) {
                if (optval == 0) {
                    // Connected!
                    stream->Connected = 1;
                    logInfo("TCPStream connected to %s:%d", stream->Host, stream->Port);
                    if (stream->OnConnect) stream->OnConnect(stream);
                } else if (ret > 0 || optval != WSAEWOULDBLOCK) {
                    // Connection failed
                    TCPStream_errorHandle(stream, optval);
                    SOCKET_CLOSE(stream->Socket);
                    continue; // Will trigger reconnect
                }
            }
        }
        
        // Process events
        if (ret > 0 && stream->Connected) {
            if (fds[0].revents & POLLIN) {
                uint8_t* buf = IStream_getDataPtr(&stream->Input);
                Stream_LenType space = IStream_directSpace(&stream->Input);
                
                if (buf && space > 0) {
                    int read_bytes = recv(stream->Socket, (char*)buf, (int)space, 0);
                    
                    if (read_bytes > 0) {
                        stream->Input.Buffer.PendingBytes = read_bytes;
                        stream->Input.Buffer.InReceive = 1;
                        IStream_handle(&stream->Input, read_bytes);
                    } else if (read_bytes == 0) {
                        // Peer disconnected
                        TCPStream_handleDisconnect(stream);
                        continue; // Will trigger reconnect
                    } else {
                        int err = WSAGetLastError();
                        if (err != WSAEWOULDBLOCK) {
                            TCPStream_errorHandle(stream, err);
                            TCPStream_handleDisconnect(stream);
                            continue; // Will trigger reconnect
                        }
                    }
                }
            }
            
            if (fds[0].revents & POLLOUT) {
                OStream_handle(&stream->Output, 0);
                OStream_flush(&stream->Output);
            }
            
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                TCPStream_handleDisconnect(stream);
                continue; // Will trigger reconnect
            }
        }
        
#else
        // ===== Linux Poll (epoll) =====
        struct epoll_event events[2];
        int timeout = stream->Connected ? -1 : 100;
        int ret = epoll_wait(stream->EpollFD, events, 2, timeout);
        
        if (ret < 0) {
            if (errno == EBADF || errno == EINVAL) {
                // Bad fd - will reconnect
                stream->Socket = SOCKET_INVALID;
                TCPStream_handleDisconnect(stream);
                continue;
            }
            if (errno != EINTR) {
                TCPStream_errorHandle(stream, errno);
            }
            continue;
        }
        
        // Check connection on timeout
        if (ret == 0 && !stream->Connected && SOCKET_IS_VALID(stream->Socket)) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(stream->Socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
                if (error == 0) {
                    stream->Connected = 1;
                    logInfo("TCPStream connected to %s:%d", stream->Host, stream->Port);
                    if (stream->OnConnect) stream->OnConnect(stream);
                } else if (error != EINPROGRESS) {
                    TCPStream_errorHandle(stream, error);
                    TCPStream_handleDisconnect(stream);
                    continue; // Will trigger reconnect
                }
            }
        }
        
        // Process events
        for (int i = 0; i < ret; i++) {
            uint32_t ev = events[i].events;
            
            // Connection completion
            if (!stream->Connected && (ev & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(stream->Socket, SOL_SOCKET, SO_ERROR, &error, &len) == 0) {
                    if (error == 0) {
                        stream->Connected = 1;
                        logInfo("TCPStream connected to %s:%d", stream->Host, stream->Port);
                        if (stream->OnConnect) stream->OnConnect(stream);
                        continue;
                    } else {
                        TCPStream_errorHandle(stream, error);
                        TCPStream_handleDisconnect(stream);
                        continue; // Will trigger reconnect
                    }
                }
            }
            
            // Read data
            if (stream->Connected && (ev & EPOLLIN)) {
                uint8_t* buf = IStream_getDataPtr(&stream->Input);
                Stream_LenType space = IStream_directSpace(&stream->Input);
                
                if (buf && space > 0) {
                    int read_bytes = (int)read(stream->Socket, buf, space);
                    
                    if (read_bytes > 0) {
                        stream->Input.Buffer.PendingBytes = read_bytes;
                        stream->Input.Buffer.InReceive = 1;
                        IStream_handle(&stream->Input, read_bytes);
                    } else if (read_bytes == 0) {
                        TCPStream_handleDisconnect(stream);
                        continue; // Will trigger reconnect
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            TCPStream_errorHandle(stream, errno);
                            TCPStream_handleDisconnect(stream);
                            continue; // Will trigger reconnect
                        }
                    }
                }
            }
            
            // Write data
            if (stream->Connected && (ev & EPOLLOUT)) {
                OStream_handle(&stream->Output, 0);
                OStream_flush(&stream->Output);
            }
            
            // Errors
            if (ev & (EPOLLERR | EPOLLHUP)) {
                TCPStream_handleDisconnect(stream);
                continue; // Will trigger reconnect
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
    
    strncpy(stream->Host, host, sizeof(stream->Host) - 1);
    stream->Host[sizeof(stream->Host) - 1] = '\0';
    stream->Port = port;
    stream->Connected = 0;
    stream->Running = 1;

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
#endif

    // Initialize stream buffers (reuse existing or set new)
    if (rxBuff) {
        IStream_init(&stream->Input, NULL, rxBuff, rxSize);
        IStream_setDriverArgs(&stream->Input, stream);
    }
    if (txBuff) {
        OStream_init(&stream->Output, TCPStream_transmit, txBuff, txSize);
        OStream_setDriverArgs(&stream->Output, stream);
    }
    __initMutex(stream);

    // Start the initial connection
    if (!TCPStream_tryConnect(stream)) {
        // If auto-reconnect is enabled, the poll thread will keep trying
        if (!stream->AutoReconnect) {
            return 0;
        }
    }

    // Start poll thread (only once!)
    if (!stream->Thread) {
#if defined(_WIN32) || defined(_WIN64)
        stream->Thread = (HANDLE)_beginthreadex(NULL, 0, 
            (unsigned(__stdcall*)(void*))TCPStream_pollThread, stream, 0, NULL);
        if (!stream->Thread) {
            SOCKET_CLOSE(stream->Socket);
            return 0;
        }
#else
        pthread_t thread;
        if (pthread_create(&thread, NULL, TCPStream_pollThread, stream) != 0) {
            SOCKET_CLOSE(stream->Socket);
            return 0;
        }
        stream->Thread = thread;
        pthread_detach(thread);
#endif
    }

    logInfo("TCPStream initialized to %s:%d", host, port);
    return 1;
}

// ===== Public Init =====
uint8_t TCPStream_init(TCPStream* stream, const char* address, uint16_t port,
                       uint8_t* rxBuff, Stream_LenType rxSize,
                       uint8_t* txBuff, Stream_LenType txSize) {
    if (!stream) return 0;
    
    // Save user-set values
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
    return TCPStream_init(stream, host, port, rxBuff, rxSize, txBuff, txSize);
}

// ===== Close =====
uint8_t TCPStream_close(TCPStream* stream) {
    if (!stream) return 0;
    
    stream->Running = 0;
    stream->Connected = 0;
    
    SOCKET_CLOSE(stream->Socket);
    
#if !(defined(_WIN32) || defined(_WIN64))
    if (stream->EpollFD >= 0) {
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

    ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
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
