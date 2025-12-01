/**
 * @file TCPStreamMacro.h
 * @author Ali Mirghasemi (ali.mirgahsemi1376@gamil.com)
 * @brief This is TCPStream macro helpers
 * @version 0.1
 * @date 2025-12-01
 * 
 * @copyright Copyright (c) 2025
 * 
 */
#ifndef _TCP_STREAM_MACRO_H_
#define _TCP_STREAM_MACRO_H_

// Mutex macro mapping (unchanged semantics)
#if STREAM_MUTEX
    #if STREAM_MUTEX == STREAM_MUTEX_CUSTOM
        #define __initMutex(STREAM)                 IStream_setMutex(&(STREAM)->Input, TCPStream_mutexInit, TCPStream_mutexLock, TCPStream_mutexUnlock, TCPStream_mutexDeInit); \
                                                    OStream_setMutex(&(STREAM)->Output, TCPStream_mutexInit, TCPStream_mutexLock, TCPStream_mutexUnlock, TCPStream_mutexDeInit); \
                                                    IStream_mutexInit(&(STREAM)->Input); \
                                                    OStream_mutexInit(&(STREAM)->Output)
    #elif STREAM_MUTEX == STREAM_MUTEX_DRIVER
        #define __defineMutexDriver()               const Stream_MutexDriver TCPStream_MutexDriver = { \
                                                        .init = TCPStream_mutexInit, \
                                                        .lock = TCPStream_mutexLock, \
                                                        .unlock = TCPStream_mutexUnlock, \
                                                        .deinit = TCPStream_mutexDeInit \
                                                    }

        #define __initMutex(STREAM)                 IStream_setMutex(&(STREAM)->Input, &TCPStream_MutexDriver); \
                                                    OStream_setMutex(&(STREAM)->Output, &TCPStream_MutexDriver); \
                                                    IStream_mutexInit(&(STREAM)->Input); \
                                                    OStream_mutexInit(&(STREAM)->Output)
    #elif STREAM_MUTEX == STREAM_MUTEX_GLOBAL_DRIVER
        #define __defineMutexDriver()               const Stream_MutexDriver TCPStream_MutexDriver = { \
                                                        .init = TCPStream_mutexInit, \
                                                        .lock = TCPStream_mutexLock, \
                                                        .unlock = TCPStream_mutexUnlock, \
                                                        .deinit = TCPStream_mutexDeInit \
                                                    }

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

#if !defined(_WIN32) && !defined(_WIN64)
    #ifndef PTHREAD_MUTEX_RECURSIVE
        #define PTHREAD_MUTEX_RECURSIVE PTHREAD_MUTEX_RECURSIVE_NP
    #endif
#endif

#endif // _TCP_STREAM_MACRO_H_
