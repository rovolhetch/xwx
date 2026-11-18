#ifndef LOG_H_
#define LOG_H_

#include <stdio.h>
#include <stdlib.h>

#ifdef DEBUG
# define LOG_DEFAULT_LVL 4
#else
# define LOG_DEFAULT_LVL 2
#endif

#define LOG_LVL_ERRO 1
#define LOG_LVL_WARN 2
#define LOG_LVL_INFO 3
#define LOG_LVL_DEBG 4

#ifndef LOG_LVL
# define LOG_LVL LOG_DEFAULT_LVL
#endif

#ifndef LOG_MOD
# define LOG_PREFIX ""
#else
# define LOG_PREFIX "[" LOG_MOD "] "
#endif

#define LOG_IMPL(lvl, ...) do { \
  const int log_lvl_ = (lvl); \
  if (LOG_LVL >= log_lvl_) { \
    switch (log_lvl_) { \
    case LOG_LVL_ERRO: fprintf(stderr, LOG_PREFIX "ERRO: " __VA_ARGS__); break; \
    case LOG_LVL_WARN: fprintf(stderr, LOG_PREFIX "WARN: " __VA_ARGS__); break; \
    case LOG_LVL_INFO: fprintf(stdout, LOG_PREFIX "INFO: " __VA_ARGS__); break; \
    case LOG_LVL_DEBG: fprintf(stderr, LOG_PREFIX "DEBG: " __VA_ARGS__); break; \
    default:           fprintf(stdout, LOG_PREFIX "????: " __VA_ARGS__); break; \
    } \
  } \
} while (0)

#define LOG(lvl, ...)   LOG_IMPL(lvl, __VA_ARGS__)
#define LOG_ERRO(...)   LOG_IMPL(LOG_LVL_ERRO, __VA_ARGS__)
#define LOG_WARN(...)   LOG_IMPL(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_INFO(...)   LOG_IMPL(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_DEBG(...)   LOG_IMPL(LOG_LVL_DEBG, __VA_ARGS__)

#define LOG_FATA_AND_EXIT(...) do { \
  fprintf(stderr, LOG_PREFIX "FATA: " __VA_ARGS__); \
  exit(EXIT_FAILURE); \
} while (0)

#endif /* LOG_H_ */
