/**
 * Copyright (c) 2026 yanrbts
 *
 * This library is free software; you can redistribute it and/or modify it
 */
#ifndef __LIB_CONFIG_H__
#define __LIB_CONFIG_H__


#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_THREADS__)
#define LIB_ERR_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define LIB_ERR_TLS __thread
#else
#error "Thread-local storage is required for thread-safe error reporting"
#endif

#endif /* __LIB_CONFIG_H__ */