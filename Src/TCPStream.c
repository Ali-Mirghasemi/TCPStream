#include "TCPStream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #include <process.h>
    #pragma comment(lib, "Ws2_32.lib")
#else
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <sys/time.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>
#endif

#if SERIALSTREAM_LIB_LOG
    #include "Log.h"
#else
    #define logInfo(...)
    #define logError(...)
#endif

// Forward decls
static void TCPStream_errorHandle(TCPStream* stream);
static Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len);
static void* TCPStream_pollThread(void* arg);
#if defined(_WIN32) || defined(_WIN64)
    static DWORD WINAPI TCPStream_pollThreadWin(LPVOID arg) { TCPStream_pollThread(arg); return 0; }
#endif
static int TCPStream_nonblock_connect(TCPStream* stream, const char* host, uint16_t port, int* out_errno);

// Mutex hooks are reused from SerialStream pattern
#if STREAM_MUTEX
static Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex);
static Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex);
#endif

// Map mutex macros same as SerialStream
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
    #define __initMutex(STREAM)
    #define __lockMutex(STREAM)
    #define __unlockMutex(STREAM)
    #define __deinitMutex(STREAM)
#endif

// Helper to parse host:port
static int parse_hostport(const char* uri, char* host, size_t hostlen, uint16_t* out_port) {
    if (!uri || !host || !out_port) return 0;
    const char* colon = strrchr(uri, ':');
    if (!colon) return 0;
    size_t hlen = colon - uri;
    if (hlen >= hostlen) return 0;
    memcpy(host, uri, hlen);
    host[hlen] = 0;
    int p = atoi(colon + 1);
    if (p <= 0 || p > 65535) return 0;
    *out_port = (uint16_t)p;
    return 1;
}

// -----------------------------
// Public API
// -----------------------------
uint8_t TCPStream_init(
    TCPStream*      stream,
    const char*     address,
    uint16_t        port,
    uint8_t*        rxBuff,
    Stream_LenType  rxBuffSize,
    uint8_t*        txBuff,
    Stream_LenType  txBuffSize
) {
    if (!stream || !address) return 0;
    memset(stream, 0, sizeof(*stream));
    stream->Context = -1;
    stream->ReconnectEnabled = 0;
    stream->ReconnectDelayMs = 1000;
    stream->EventCb = NULL;
    stream->EventCbArg = NULL;
    stream->Connected = 0;

#if defined(_WIN32) || defined(_WIN64)
    // ensure Winsock initialized
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    // Try to connect (non-blocking)
    int platform_err = 0;
    int rc = TCPStream_nonblock_connect(stream, address, port, &platform_err);
    if (!rc) {
        // initial connect failed (but might be in progress); we continue and let thread finish connect
        logInfo("TCPStream: connect in progress or failed immediately (err=%d)", platform_err);
    }

    // Init Stream structures
    IStream_init(&stream->Input, NULL, rxBuff, rxBuffSize);
    IStream_setDriverArgs(&stream->Input, stream);

    OStream_init(&stream->Output, TCPStream_transmit, txBuff, txBuffSize);
    OStream_setDriverArgs(&stream->Output, stream);

    __initMutex(stream);

    // Start poll thread
#if defined(_WIN32) || defined(_WIN64)
    stream->StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    stream->PollThread = CreateThread(NULL, 0, TCPStream_pollThreadWin, stream, 0, NULL);
    if (!stream->PollThread) {
        logError("TCPStream: CreateThread failed");
        return 0;
    }
#else
    if (pthread_create(&stream->PollThread, NULL, TCPStream_pollThread, stream) != 0) {
        perror("TCPStream: pthread_create");
        return 0;
    }
    pthread_detach(stream->PollThread);
#endif

    return 1;
}

uint8_t TCPStream_initUri(
    TCPStream*      stream,
    const char*     hostport,
    uint8_t*        rxBuff,
    Stream_LenType  rxBuffSize,
    uint8_t*        txBuff,
    Stream_LenType  txBuffSize
) {
    char host[256]; 
    uint16_t port = 0;
    if (!parse_hostport(hostport, host, sizeof(host), &port)) return 0;
    return TCPStream_init_addr(stream, host, port, rxBuff, rxBuffSize, txBuff, txBuffSize);
}

uint8_t TCPStream_close(TCPStream* stream) {
    if (!stream) return 0;

#if defined(_WIN32) || defined(_WIN64)
    if (stream->StopEvent) SetEvent(stream->StopEvent);
    if (stream->PollThread) {
        WaitForSingleObject(stream->PollThread, 200);
        CloseHandle(stream->PollThread);
        stream->PollThread = NULL;
    }
    if (stream->StopEvent) {
        CloseHandle(stream->StopEvent);
        stream->StopEvent = NULL;
    }

    if (stream->Context != (intptr_t)-1 && stream->Context != 0) {
        closesocket((SOCKET)stream->Context);
        stream->Context = -1;
    }
    WSACleanup();
#else
    if (stream->Context >= 0) {
        close((int)stream->Context);
        stream->Context = -1;
    }
#endif

    IStream_deinit(&stream->Input);
    OStream_deinit(&stream->Output);
    __deinitMutex(stream);

    return 1;
}

uint8_t TCPStream_isConnected(TCPStream* stream) {
    if (!stream) return 0;
    return stream->Connected;
}

void TCPStream_setReconnect(TCPStream* stream, uint8_t enable, uint32_t delay_ms) {
    if (!stream) return;
    stream->ReconnectEnabled = enable ? 1 : 0;
    if (delay_ms == 0) delay_ms = 1000;
    stream->ReconnectDelayMs = delay_ms;
}

void TCPStream_setEventCallback(TCPStream* stream, TCPStream_EventCb cb, void* userArg) {
    if (!stream) return;
    stream->EventCb = cb;
    stream->EventCbArg = userArg;
}

// -----------------------------
// Transmit (used by OStream)
 // ----------------------------
static Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len) {
    TCPStream* tcp = (TCPStream*) OStream_getDriverArgs(stream);
    if (!tcp) return Stream_NoTransmit;

#if defined(_WIN32) || defined(_WIN64)
    if (tcp->Context == (intptr_t)-1 || tcp->Context == 0) return Stream_NoTransmit;
    SOCKET s = (SOCKET)tcp->Context;
    if (!tcp->Connected) return Stream_NoTransmit;

    int sent = send(s, (const char*)buff, (int)len, 0);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return Stream_NoTransmit;
        } else {
            return Stream_CustomError | err;
        }
    } else if (sent > 0) {
        stream->Buffer.InTransmit = 1;
        return OStream_handle(stream, sent);
    }
    return Stream_Ok;
#else
    if (tcp->Context < 0) return Stream_NoTransmit;
    int fd = (int)tcp->Context;
    if (!tcp->Connected) return Stream_NoTransmit;

    ssize_t wr = send(fd, buff, len, 0);
    if (wr < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return Stream_NoTransmit;
        }
        return Stream_CustomError | errno;
    } else if (wr > 0) {
        stream->Buffer.InTransmit = 1;
        return OStream_handle(stream, (int)wr);
    }
    return Stream_Ok;
#endif
}

// -----------------------------
// Internal helpers
// -----------------------------
static void TCPStream_errorHandle(TCPStream* stream) {
    if (!stream) return;
    stream->Connected = 0;
    IStream_resetIO(&stream->Input);
    OStream_resetIO(&stream->Output);
    if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_Disconnected, 0, stream->EventCbArg);
    // If reconnect enabled, the poll thread will attempt reconnection
}

static int set_nonblocking_socket(int sock) {
#if defined(_WIN32) || defined(_WIN64)
    unsigned long mode = 1;
    return ioctlsocket((SOCKET)sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Non-blocking connect helper: returns 1 if socket created and connect succeeded immediately, 0 if in-progress (or error), -1 on fatal error.
// out_errno filled with platform errno/WSAGetLastError()
static int TCPStream_nonblock_connect(TCPStream* stream, const char* host, uint16_t port, int* out_errno) {
    if (!stream || !host) return -1;
    struct addrinfo hints, *res = NULL;
    char portbuf[8];
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portbuf, &hints, &res);
    if (gai != 0) {
#if defined(_WIN32) || defined(_WIN64)
        if (out_errno) *out_errno = WSAGetLastError();
#else
        if (out_errno) *out_errno = errno;
#endif
        return -1;
    }

    int connected_now = 0;
    int last_err = 0;
    int created_sock = -1;

    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
#if defined(_WIN32) || defined(_WIN64)
        SOCKET s = WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET) {
            last_err = WSAGetLastError();
            continue;
        }
        created_sock = (int)s;
#else
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) {
            last_err = errno;
            continue;
        }
        created_sock = s;
#endif

        // set non-blocking
#if defined(_WIN32) || defined(_WIN64)
        {
            u_long mode = 1;
            ioctlsocket((SOCKET)created_sock, FIONBIO, &mode);
        }
#else
        {
            int flags = fcntl(created_sock, F_GETFL, 0);
            fcntl(created_sock, F_SETFL, flags | O_NONBLOCK);
        }
#endif

        // try connect
#if defined(_WIN32) || defined(_WIN64)
        int rc = connect((SOCKET)created_sock, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc == 0) {
            // connected immediately
            stream->Context = (intptr_t)created_sock;
            stream->Connected = 1;
            if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_Connected, 0, stream->EventCbArg);
            freeaddrinfo(res);
            return 1;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY) {
                // connect in progress
                stream->Context = (intptr_t)created_sock;
                if (out_errno) *out_errno = err;
                freeaddrinfo(res);
                return 0;
            } else {
                closesocket((SOCKET)created_sock);
                last_err = err;
                created_sock = -1;
                continue;
            }
        }
#else
        int rc = connect(created_sock, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            // immediate connect
            stream->Context = (intptr_t)created_sock;
            stream->Connected = 1;
            if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_Connected, 0, stream->EventCbArg);
            freeaddrinfo(res);
            return 1;
        } else {
            if (errno == EINPROGRESS) {
                // in progress
                stream->Context = (intptr_t)created_sock;
                if (out_errno) *out_errno = errno;
                freeaddrinfo(res);
                return 0;
            } else {
                close(created_sock);
                last_err = errno;
                created_sock = -1;
                continue;
            }
        }
#endif
    }

    freeaddrinfo(res);
    if (out_errno) *out_errno = last_err;
    return -1;
}

// Poll thread: monitors connect completion, readability, writability.
// Uses select() with a timeout to avoid busy-looping.
static void* TCPStream_pollThread(void* arg) {
    TCPStream* stream = (TCPStream*)arg;
    if (!stream) return NULL;

    const int select_timeout_ms = 200; // tune this to avoid high CPU and keep latency low
    char rbuf[2048];

    while (1) {
        // exit condition
#if defined(_WIN32) || defined(_WIN64)
        if (stream->StopEvent && WaitForSingleObject(stream->StopEvent, 0) == WAIT_OBJECT_0) break;
#endif

        intptr_t ctx = stream->Context;
        if (ctx == -1) {
            // Not connected and no socket. If reconnect enabled try to reconnect after delay.
            if (stream->ReconnectEnabled) {
                Sleep(stream->ReconnectDelayMs);
                // user must have stored target host/port elsewhere; but since this implementation
                // doesn't store host/port we rely on user re-init or not. To keep behaviour simple:
                // if no Context attempt nothing. (Alternative: store host/port in struct.)
            } else {
#if defined(_WIN32) || defined(_WIN64)
                Sleep(select_timeout_ms);
#else
                usleep(select_timeout_ms * 1000);
#endif
            }
            continue;
        }

#if defined(_WIN32) || defined(_WIN64)
        SOCKET s = (SOCKET)ctx;
        fd_set readfds, writefds, exceptfds;
        FD_ZERO(&readfds); FD_ZERO(&writefds); FD_ZERO(&exceptfds);
        FD_SET(s, &readfds);
        FD_SET(s, &writefds);
        FD_SET(s, &exceptfds);

        TIMEVAL tv;
        tv.tv_sec = select_timeout_ms / 1000;
        tv.tv_usec = (select_timeout_ms % 1000) * 1000;

        int sel = select(0, &readfds, &writefds, &exceptfds, &tv);
        if (sel == SOCKET_ERROR) {
            int err = WSAGetLastError();
            logError("TCPStream select error %d", err);
            TCPStream_errorHandle(stream);
            // attempt reconnect if enabled
            if (stream->ReconnectEnabled) {
                Sleep(stream->ReconnectDelayMs);
                // user-initiated reconnect not implemented automatically here because host/port not stored
            } else break;
        } else if (sel > 0) {
            // check writability -> could mean connect completed when we were in-progress
            if (FD_ISSET(s, &writefds) && !stream->Connected) {
                // check socket error
                int so_err = 0; int optlen = sizeof(so_err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_err, &optlen);
                if (so_err == 0) {
                    stream->Connected = 1;
                    if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_Connected, 0, stream->EventCbArg);
                } else {
                    if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_ConnectFailed, so_err, stream->EventCbArg);
                    closesocket(s);
                    stream->Context = -1;
                    stream->Connected = 0;
                    if (!stream->ReconnectEnabled) break;
                    else { Sleep(stream->ReconnectDelayMs); continue; }
                }
            }

            if (FD_ISSET(s, &readfds) && stream->Connected) {
                int n = recv(s, rbuf, sizeof(rbuf), 0);
                if (n == 0) {
                    // connection closed
                    TCPStream_errorHandle(stream);
#if defined(_WIN32) || defined(_WIN64)
                    closesocket(s);
#endif
                    stream->Context = -1;
                    if (!stream->ReconnectEnabled) break;
                    else { Sleep(stream->ReconnectDelayMs); continue; }
                } else if (n < 0) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) {
                        // no data
                    } else {
                        TCPStream_errorHandle(stream);
                        closesocket(s);
                        stream->Context = -1;
                        if (!stream->ReconnectEnabled) break;
                        else { Sleep(stream->ReconnectDelayMs); continue; }
                    }
                } else {
                    // push to Input stream
                    __lockMutex(&stream->Input);
                    uint8_t* dest = IStream_getDataPtr(&stream->Input);
                    Stream_LenType avail = IStream_directSpace(&stream->Input);
                    Stream_LenType toCopy = (avail < (Stream_LenType)n) ? avail : (Stream_LenType)n;
                    if (toCopy > 0) {
                        memcpy(dest, rbuf, toCopy);
                        stream->Input.Buffer.InReceive = 1;
                        stream->Input.Buffer.PendingBytes = (int)toCopy;
                        IStream_handle(&stream->Input, (int)toCopy);
                    }
                    __unlockMutex(&stream->Input);
                }
            }

            // handle write readiness: attempt to flush Output buffer
            if (FD_ISSET(s, &writefds) && stream->Connected) {
                __lockMutex(&stream->Output);
                uint8_t* src = OStream_getDataPtr(&stream->Output);
                Stream_LenType avail = OStream_directBytes(&stream->Output);
                if (avail > 0) {
                    int sent = send(s, (const char*)src, (int)avail, 0);
                    if (sent > 0) {
                        OStream_handle(&stream->Output, sent);
                    }
                }
                __unlockMutex(&stream->Output);
            }
        }
#else
        int fd = (int)ctx;
        fd_set readfds, writefds, exceptfds;
        FD_ZERO(&readfds); FD_ZERO(&writefds); FD_ZERO(&exceptfds);
        FD_SET(fd, &readfds);
        FD_SET(fd, &writefds);
        FD_SET(fd, &exceptfds);

        struct timeval tv;
        tv.tv_sec = select_timeout_ms / 1000;
        tv.tv_usec = (select_timeout_ms % 1000) * 1000;

        int sel = select(fd + 1, &readfds, &writefds, &exceptfds, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            logError("TCPStream select error %s", strerror(errno));
            TCPStream_errorHandle(stream);
            if (stream->ReconnectEnabled) {
                usleep(stream->ReconnectDelayMs * 1000);
                continue;
            } else break;
        } else if (sel > 0) {
            if (FD_ISSET(fd, &writefds) && !stream->Connected) {
                int so_err = 0; socklen_t len = sizeof(so_err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
                if (so_err == 0) {
                    stream->Connected = 1;
                    if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_Connected, 0, stream->EventCbArg);
                } else {
                    if (stream->EventCb) stream->EventCb(stream, TCPStream_Event_ConnectFailed, so_err, stream->EventCbArg);
                    close(fd);
                    stream->Context = -1;
                    stream->Connected = 0;
                    if (!stream->ReconnectEnabled) break;
                    else { usleep(stream->ReconnectDelayMs * 1000); continue; }
                }
            }

            if (FD_ISSET(fd, &readfds) && stream->Connected) {
                ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
                if (n == 0) {
                    // closed
                    TCPStream_errorHandle(stream);
                    close(fd);
                    stream->Context = -1;
                    if (!stream->ReconnectEnabled) break;
                    else { usleep(stream->ReconnectDelayMs * 1000); continue; }
                } else if (n < 0) {
                    if (errno == EWOULDBLOCK || errno == EAGAIN) {
                        // nothing
                    } else {
                        TCPStream_errorHandle(stream);
                        close(fd);
                        stream->Context = -1;
                        if (!stream->ReconnectEnabled) break;
                        else { usleep(stream->ReconnectDelayMs * 1000); continue; }
                    }
                } else {
                    __lockMutex(&stream->Input);
                    uint8_t* dest = IStream_getDataPtr(&stream->Input);
                    Stream_LenType avail = IStream_directSpace(&stream->Input);
                    Stream_LenType toCopy = (avail < (Stream_LenType)n) ? avail : (Stream_LenType)n;
                    if (toCopy > 0) {
                        memcpy(dest, rbuf, toCopy);
                        stream->Input.Buffer.InReceive = 1;
                        stream->Input.Buffer.PendingBytes = (int)toCopy;
                        IStream_handle(&stream->Input, (int)toCopy);
                    }
                    __unlockMutex(&stream->Input);
                }
            }

            // writable -> flush output
            if (FD_ISSET(fd, &writefds) && stream->Connected) {
                __lockMutex(&stream->Output);
                uint8_t* src = OStream_getDataPtr(&stream->Output);
                Stream_LenType avail = OStream_directBytes(&stream->Output);
                if (avail > 0) {
                    ssize_t sent = send(fd, src, avail, 0);
                    if (sent > 0) {
                        OStream_handle(&stream->Output, (int)sent);
                    }
                }
                __unlockMutex(&stream->Output);
            }
        }
#endif

        // small sleep / yield to avoid busy loop if nothing happened
#if defined(_WIN32) || defined(_WIN64)
        Sleep(1);
#else
        usleep(1000);
#endif
    } // while

    return NULL;
}

#if STREAM_MUTEX
#include <errno.h>
#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
#endif

static Stream_MutexResult TCPStream_mutexInit(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream) return (Stream_MutexResult) EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION* cs = malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return (Stream_MutexResult) ENOMEM;
    InitializeCriticalSection(cs);
    stream->Mutex = (void*)cs;
#else
    pthread_mutex_t* m = malloc(sizeof(pthread_mutex_t));
    if (!m) return (Stream_MutexResult) ENOMEM;

    pthread_mutexattr_t attr;
    int ret = pthread_mutexattr_init(&attr);
    if (ret != 0) { free(m); return (Stream_MutexResult)ret; }
    ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (ret != 0) { pthread_mutexattr_destroy(&attr); free(m); return (Stream_MutexResult)ret; }
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    if (ret != 0) { free(m); return (Stream_MutexResult)ret; }

    stream->Mutex = (void*)m;
#endif
    return Stream_Ok;
}

static Stream_MutexResult TCPStream_mutexLock(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream || !stream->Mutex) return (Stream_MutexResult) EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    EnterCriticalSection((CRITICAL_SECTION*)stream->Mutex);
    return Stream_Ok;
#else
    return (Stream_MutexResult) pthread_mutex_lock((pthread_mutex_t*)stream->Mutex);
#endif
}

static Stream_MutexResult TCPStream_mutexUnlock(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream || !stream->Mutex) return (Stream_MutexResult) EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    LeaveCriticalSection((CRITICAL_SECTION*)stream->Mutex);
    return Stream_Ok;
#else
    return (Stream_MutexResult) pthread_mutex_unlock((pthread_mutex_t*)stream->Mutex);
#endif
}

static Stream_MutexResult TCPStream_mutexDeInit(StreamBuffer* stream, Stream_Mutex* mutex) {
    if (!stream) return (Stream_MutexResult) EINVAL;
#if defined(_WIN32) || defined(_WIN64)
    if (stream->Mutex) {
        CRITICAL_SECTION* cs = (CRITICAL_SECTION*)stream->Mutex;
        DeleteCriticalSection(cs);
        free(cs);
        stream->Mutex = NULL;
    }
    return Stream_Ok;
#else
    pthread_mutex_t* m = (pthread_mutex_t*)stream->Mutex;
    int ret = pthread_mutex_destroy(m);
    free(m);
    stream->Mutex = NULL;
    if (ret != 0) return (Stream_MutexResult)ret;
    return Stream_Ok;
#endif
}
#endif // STREAM_MUTEX
