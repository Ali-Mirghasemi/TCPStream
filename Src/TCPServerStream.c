#include "TCPServerStream.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if TCPSTREAM_LIB_LOG
    #include "Log.h"
#else
    #define logInfo(...)
    #define logError(...)
    #define logDebug(...)
    #define logWarn(...)
#endif

#if defined(_WIN32) || defined(_WIN64)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <process.h>
    typedef SOCKET ServerSocket;
    #define CLOSESOCKET(s) closesocket(s)
    #define SOCKET_IS_VALID(s) ((s) != INVALID_SOCKET)
    #define LAST_ERROR() WSAGetLastError()
    #define WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
    #define SLEEP_MS(ms) Sleep(ms)
    #define MUTEX_INIT(m) InitializeCriticalSection(m)
    #define MUTEX_DESTROY(m) DeleteCriticalSection(m)
    #define MUTEX_LOCK(m) EnterCriticalSection(m)
    #define MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#else
    #include <pthread.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/select.h>      // <-- Added for fd_set, select()
    #include <sys/time.h>        // <-- Added for struct timeval
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/epoll.h>
    typedef int ServerSocket;
    #define CLOSESOCKET(s) close(s)
    #define SOCKET_IS_VALID(s) ((s) >= 0)
    #define LAST_ERROR() errno
    #define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
    #define SLEEP_MS(ms) usleep((ms) * 1000)
    
    // Fixed MUTEX_INIT macro for Linux compatibility
    #define MUTEX_INIT(m) do { \
        pthread_mutexattr_t attr; \
        pthread_mutexattr_init(&attr); \
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP); \
        pthread_mutex_init(m, &attr); \
        pthread_mutexattr_destroy(&attr); \
    } while(0)
    #define MUTEX_DESTROY(m) pthread_mutex_destroy(m)
    #define MUTEX_LOCK(m) pthread_mutex_lock(m)
    #define MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#endif

// ===== Define POSIX source for usleep =====
#if !defined(_WIN32) && !defined(_WIN64)
    #ifndef _POSIX_C_SOURCE
        #define _POSIX_C_SOURCE 200809L
    #endif
    #ifndef _BSD_SOURCE
        #define _BSD_SOURCE
    #endif
    #ifndef _DEFAULT_SOURCE
        #define _DEFAULT_SOURCE
    #endif
#endif

// ===== Internal Function Declarations =====
static THREAD_RET TCPServerStream_acceptThread(void* arg);
static void TCPServerStream_addClient(TCPServerStream* server, TCPStream* client);
static void TCPServerStream_removeClientInternal(TCPServerStream* server, TCPStream* client, int lock);
static void TCPServerStream_cleanupClient(TCPStream* client);
static int TCPServerStream_setNonBlocking(TCPStream_Socket sock);
extern THREAD_RET TCPStream_pollThread(void* arg);
extern Stream_Result TCPStream_transmit(StreamOut* stream, uint8_t* buff, Stream_LenType len);

// ===== Callback Setters =====
void TCPServerStream_onClientConnect(TCPServerStream* server, TCPServerStream_OnClientConnectFn cb) {
    if (server) server->OnClientConnect = cb;
}

void TCPServerStream_onClientDisconnect(TCPServerStream* server, TCPServerStream_OnClientDisconnectFn cb) {
    if (server) server->OnClientDisconnect = cb;
}

void TCPServerStream_onClientError(TCPServerStream* server, TCPServerStream_OnClientErrorFn cb) {
    if (server) server->OnClientError = cb;
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

// ===== Client List Management (Thread-Safe) =====
static void TCPServerStream_addClient(TCPServerStream* server, TCPStream* client) {
    if (!server || !client) return;
    
    MUTEX_LOCK(&server->Mutex);
    
    TCPClientNode* node = (TCPClientNode*)calloc(1, sizeof(TCPClientNode));
    if (!node) {
        MUTEX_UNLOCK(&server->Mutex);
        logError("Failed to allocate client node");
        return;
    }
    
    node->client = client;
    node->active = 1;
    node->next = server->ClientList;
    if (server->ClientList) {
        server->ClientList->prev = node;
    }
    server->ClientList = node;
    server->CurrentClients++;
    
    MUTEX_UNLOCK(&server->Mutex);
}

static void TCPServerStream_removeClientInternal(TCPServerStream* server, TCPStream* client, int lock) {
    if (!server || !client) return;
    
    if (lock) MUTEX_LOCK(&server->Mutex);
    
    TCPClientNode* current = server->ClientList;
    while (current) {
        if (current->client == client && current->active) {
            current->active = 0;
            
            // Remove from linked list
            if (current->prev) {
                current->prev->next = current->next;
            } else {
                server->ClientList = current->next;
            }
            if (current->next) {
                current->next->prev = current->prev;
            }
            
            server->CurrentClients--;
            free(current);
            break;
        }
        current = current->next;
    }
    
    if (lock) MUTEX_UNLOCK(&server->Mutex);
}

// ===== Client Cleanup (Called automatically on disconnect/error) =====
static void TCPServerStream_cleanupClient(TCPStream* client) {
    if (!client) return;
    
    // Get server reference before cleanup
    TCPServerStream* server = (TCPServerStream*)client->Args;
    
    // Save info for logging
    char host[128];
    uint16_t port;
    strncpy(host, client->Host, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    port = client->Port;
    
    // Notify server of disconnection (before removing from list)
    if (server && server->OnClientDisconnect) {
        server->OnClientDisconnect(server, client);
    }
    
    // Remove from server's client list
    if (server) {
        TCPServerStream_removeClientInternal(server, client, 1);
    }
    
    // Close socket and cleanup resources
    if (SOCKET_IS_VALID(client->Socket)) {
        CLOSESOCKET(client->Socket);
        client->Socket = (TCPStream_Socket)(-1);
    }
    
#if !(defined(_WIN32) || defined(_WIN64))
    if (client->EpollFD > 0) {
        close(client->EpollFD);
        client->EpollFD = -1;
    }
#endif
    
    // Free buffers
    if (client->Input.Buffer.Data) {
        free(client->Input.Buffer.Data);
        client->Input.Buffer.Data = NULL;
    }
    if (client->Output.Buffer.Data) {
        free(client->Output.Buffer.Data);
        client->Output.Buffer.Data = NULL;
    }
    
    // Free the client structure itself
    free(client);
    
    logInfo("Client %s:%u cleaned up", host, port);
}

// ===== Modified TCPStream for server mode =====
// This wrapper handles automatic cleanup
static THREAD_RET TCPServerStream_clientThread(void* arg) {
    TCPStream* client = (TCPStream*)arg;
    TCPServerStream* server = (TCPServerStream*)client->Args;
    
    // Set running flag
    client->Running = 1;
    
    // Run the standard poll thread
    THREAD_RET ret = TCPStream_pollThread(client);
    
    // Auto-cleanup when thread exits (only if server still valid)
    if (server) {
        TCPServerStream_cleanupClient(client);
    }
    
    return ret;
}

// ===== Server Initialization =====
uint8_t TCPServerStream_init(
    TCPServerStream* server, 
    const char* host, 
    uint16_t port,
    uint16_t maxClients,
    Stream_LenType rxBufferSize,
    Stream_LenType txBufferSize,
    TCPServerStream_Mode mode
) {
    if(!server) return 0;
    memset(server, 0, sizeof(TCPServerStream));

    strncpy(server->Host, host, sizeof(server->Host)-1);
    server->Host[sizeof(server->Host)-1] = '\0';
    server->Port = port;
    server->MaxClients = maxClients;
    server->TxBufferSize = txBufferSize;
    server->RxBufferSize = rxBufferSize;
    server->Mode = mode;
    
    // Initialize mutex
    MUTEX_INIT(&server->Mutex);

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        logError("WSAStartup failed");
        MUTEX_DESTROY(&server->Mutex);
        return 0;
    }
#endif

    server->ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(!SOCKET_IS_VALID(server->ListenSocket)) {
        logError("socket creation failed");
        MUTEX_DESTROY(&server->Mutex);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return 0;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server->ListenSocket, SOL_SOCKET, SO_REUSEADDR, 
#if defined(_WIN32) || defined(_WIN64)
                   (const char*)&opt, sizeof(opt)) < 0) {
#else
                   &opt, sizeof(opt)) < 0) {
#endif
        logError("setsockopt failed: %d", LAST_ERROR());
        CLOSESOCKET(server->ListenSocket);
        MUTEX_DESTROY(&server->Mutex);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return 0;
    }
    
    TCPServerStream_setNonBlocking(server->ListenSocket);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#if defined(_WIN32) || defined(_WIN64)
    addr.sin_addr.s_addr = inet_addr(host);
#else
    if(inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        logError("inet_pton failed for host: %s", host);
        CLOSESOCKET(server->ListenSocket);
        MUTEX_DESTROY(&server->Mutex);
        return 0;
    }
#endif

    if(bind(server->ListenSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        logError("bind failed: %d", LAST_ERROR());
        CLOSESOCKET(server->ListenSocket);
        MUTEX_DESTROY(&server->Mutex);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return 0;
    }

    if(listen(server->ListenSocket, SOMAXCONN) < 0) {
        logError("listen failed: %d", LAST_ERROR());
        CLOSESOCKET(server->ListenSocket);
        MUTEX_DESTROY(&server->Mutex);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return 0;
    }

    server->Running = 1;

    // Start accept thread
#if defined(_WIN32) || defined(_WIN64)
    server->Thread = (HANDLE)_beginthreadex(NULL, 0, TCPServerStream_acceptThread, server, 0, NULL);
    if(!server->Thread) {
        server->Running = 0;
        CLOSESOCKET(server->ListenSocket);
        MUTEX_DESTROY(&server->Mutex);
        WSACleanup();
        return 0;
    }
#else
    if(pthread_create(&server->Thread, NULL, TCPServerStream_acceptThread, server) != 0) {
        server->Running = 0;
        CLOSESOCKET(server->ListenSocket);
        MUTEX_DESTROY(&server->Mutex);
        return 0;
    }
    pthread_detach(server->Thread);
#endif

    logInfo("TCPServerStream listening on %s:%d", host, port);
    return 1;
}

// ===== Accept Thread =====
static THREAD_RET TCPServerStream_acceptThread(void* arg) {
    TCPServerStream* server = (TCPServerStream*)arg;
    
    while(server->Running) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        
        // Accept with timeout to allow checking Running flag
        fd_set readfds;
        FD_ZERO(&readfds);
        
#if defined(_WIN32) || defined(_WIN64)
        FD_SET(server->ListenSocket, &readfds);
#else
        if (server->ListenSocket < FD_SETSIZE) {
            FD_SET(server->ListenSocket, &readfds);
        } else {
            logError("Socket fd too large for select()");
            SLEEP_MS(500);
            continue;
        }
#endif
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000; // 500ms timeout
        
        int selectResult = select(
#if defined(_WIN32) || defined(_WIN64)
            0,
#else
            server->ListenSocket + 1,
#endif
            &readfds, NULL, NULL, &tv);
        
        if (selectResult < 0) {
            if (!server->Running) break;
#if defined(_WIN32) || defined(_WIN64)
            int err = WSAGetLastError();
            if (err != WSAEINTR) {
                logError("select failed: %d", err);
            }
#else
            if (errno != EINTR) {
                logError("select failed: %d", errno);
            }
#endif
            continue;
        }
        
        if (selectResult == 0 || !FD_ISSET(server->ListenSocket, &readfds)) {
            // Timeout - check if we should continue
            continue;
        }
        
        TCPStream_Socket clientSock = accept(server->ListenSocket, (struct sockaddr*)&clientAddr, &addrLen);
        
        if (!server->Running) {
            if (SOCKET_IS_VALID(clientSock)) CLOSESOCKET(clientSock);
            break;
        }
        
        if (!SOCKET_IS_VALID(clientSock)) {
            int err = LAST_ERROR();
            if (server->Running && !WOULD_BLOCK(err)) {
                logError("accept failed: %d", err);
            }
            continue;
        }
        
        // Check client limit
        if (server->CurrentClients >= server->MaxClients) {
            logWarn("Max clients reached (%d/%d), rejecting connection", 
                    server->CurrentClients, server->MaxClients);
            CLOSESOCKET(clientSock);
            continue;
        }
        
        // Create TCPStream for client
        TCPStream* client = (TCPStream*)calloc(1, sizeof(TCPStream));
        if (!client) {
            CLOSESOCKET(clientSock);
            continue;
        }
        
        client->Socket = clientSock;
        client->Connected = 1;
        client->Running = 1;
        client->Args = server; // Store server reference for cleanup
        
        // Store client address
#if defined(_WIN32) || defined(_WIN64)
        InetNtopA(AF_INET, &clientAddr.sin_addr, client->Host, sizeof(client->Host));
#else
        inet_ntop(AF_INET, &clientAddr.sin_addr, client->Host, sizeof(client->Host));
#endif
        client->Port = ntohs(clientAddr.sin_port);
        
        TCPServerStream_setNonBlocking(clientSock);
        
        // Allocate and initialize buffers
        client->Input.Buffer.Size = server->RxBufferSize;
        client->Output.Buffer.Size = server->TxBufferSize;
        client->Input.Buffer.Data = (uint8_t*)malloc(server->RxBufferSize);
        client->Output.Buffer.Data = (uint8_t*)malloc(server->TxBufferSize);
        
        if (!client->Input.Buffer.Data || !client->Output.Buffer.Data) {
            logError("Failed to allocate buffers for client %s:%u", client->Host, client->Port);
            if (client->Input.Buffer.Data) free(client->Input.Buffer.Data);
            if (client->Output.Buffer.Data) free(client->Output.Buffer.Data);
            CLOSESOCKET(clientSock);
            free(client);
            continue;
        }
        
        // Initialize streams
        IStream_init(&client->Input, NULL, client->Input.Buffer.Data, client->Input.Buffer.Size);
        OStream_init(&client->Output, TCPStream_transmit, client->Output.Buffer.Data, client->Output.Buffer.Size);
        OStream_setDriverArgs(&client->Output, client);
        IStream_setDriverArgs(&client->Input, client);
        
#if !(defined(_WIN32) || defined(_WIN64))
        // Setup epoll
        client->EpollFD = epoll_create1(0);
        if (client->EpollFD < 0) {
            logError("epoll_create1 failed for client: %d", errno);
            free(client->Input.Buffer.Data);
            free(client->Output.Buffer.Data);
            CLOSESOCKET(clientSock);
            free(client);
            continue;
        }
        
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = client;
        if (epoll_ctl(client->EpollFD, EPOLL_CTL_ADD, clientSock, &ev) < 0) {
            logError("epoll_ctl failed: %d", errno);
            close(client->EpollFD);
            free(client->Input.Buffer.Data);
            free(client->Output.Buffer.Data);
            CLOSESOCKET(clientSock);
            free(client);
            continue;
        }
#endif
        
        // Add to client list BEFORE notifying user
        TCPServerStream_addClient(server, client);
        
        // Notify user of new connection
        if (server->OnClientConnect) {
            server->OnClientConnect(server, client);
        }
        
        // Start client thread with auto-cleanup wrapper
#if defined(_WIN32) || defined(_WIN64)
        client->Thread = (HANDLE)_beginthreadex(NULL, 0, TCPServerStream_clientThread, client, 0, NULL);
        if (!client->Thread) {
            logError("Failed to create client thread");
            TCPServerStream_cleanupClient(client);
        }
#else
        pthread_t thread;
        if (pthread_create(&thread, NULL, TCPServerStream_clientThread, client) != 0) {
            logError("Failed to create client thread");
            TCPServerStream_cleanupClient(client);
        } else {
            client->Thread = thread;
            pthread_detach(thread);
        }
#endif
        
        logInfo("New client connected from %s:%u (total: %d/%d)", 
                client->Host, client->Port, server->CurrentClients, server->MaxClients);
    }
    
    return 0;
}

// ===== Close Server =====
uint8_t TCPServerStream_close(TCPServerStream* server) {
    if (!server) return 0;
    
    server->Running = 0;
    
    // Close listening socket to unblock accept
    if (SOCKET_IS_VALID(server->ListenSocket)) {
        CLOSESOCKET(server->ListenSocket);
        server->ListenSocket = (TCPStream_Socket)(-1);
    }
    
    // Wait for accept thread to finish
    SLEEP_MS(100);
    
    // Close all clients
    MUTEX_LOCK(&server->Mutex);
    
    TCPClientNode* current = server->ClientList;
    while (current) {
        TCPClientNode* next = current->next;
        if (current->client && current->active) {
            TCPStream* client = current->client;
            current->active = 0;
            
            // Signal client to stop
            client->Running = 0;
            
            // Close client socket to unblock poll thread
            if (SOCKET_IS_VALID(client->Socket)) {
                CLOSESOCKET(client->Socket);
                client->Socket = (TCPStream_Socket)(-1);
            }
        }
        free(current);
        current = next;
    }
    server->ClientList = NULL;
    server->CurrentClients = 0;
    
    MUTEX_UNLOCK(&server->Mutex);
    MUTEX_DESTROY(&server->Mutex);
    
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    
    logInfo("TCPServerStream closed");
    return 1;
}

// ===== Utility Functions =====
uint16_t TCPServerStream_getClientCount(TCPServerStream* server) {
    if (!server) return 0;
    uint16_t count;
    MUTEX_LOCK(&server->Mutex);
    count = server->CurrentClients;
    MUTEX_UNLOCK(&server->Mutex);
    return count;
}

void TCPServerStream_broadcast(TCPServerStream* server, const uint8_t* data, uint32_t len) {
    if (!server || !data || !len) return;
    
    MUTEX_LOCK(&server->Mutex);
    
    TCPClientNode* current = server->ClientList;
    while (current) {
        if (current->client && current->active && current->client->Connected) {
            OStream_writeBytes(&current->client->Output, (uint8_t*)data, len);
            OStream_flush(&current->client->Output);
        }
        current = current->next;
    }
    
    MUTEX_UNLOCK(&server->Mutex);
}
