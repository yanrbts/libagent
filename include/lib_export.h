/**
 * Copyright (c) 2026 yanrbts
 *
 * Shared library symbol visibility macros for libagent modules.
 */
#ifndef __LIB_EXPORT_H__
#define __LIB_EXPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(LIBAGENT_BUILD_SHARED)
#    define LIBAGENT_API __declspec(dllexport)
#  elif defined(LIBAGENT_USE_SHARED)
#    define LIBAGENT_API __declspec(dllimport)
#  else
#    define LIBAGENT_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define LIBAGENT_API __attribute__((visibility("default")))
#else
#  define LIBAGENT_API
#endif

#ifdef __cplusplus
}
#endif

#endif /* __LIB_EXPORT_H__ */