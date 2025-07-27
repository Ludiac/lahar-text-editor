#pragma once

#ifndef NDEBUG
#include <cstdio>
#include <cstdlib>
#define ENSURE_INIT(ptr)                                                                           \
  ((ptr) ? void()                                                                                  \
         : (::fprintf(stderr, "[%s:%d] Uninitialised pointer: %s (value=%p)\n", __FILE__,          \
                      __LINE__, #ptr, static_cast<const void *>(ptr)),                             \
            std::abort()))
#else
#define ENSURE_INIT(ptr) (void)0
#endif

#define EXPECTED_VOID(expr)                                                                        \
  ({                                                                                               \
    std::expected<void, std::string> expected = (expr);                                            \
    if (!expected) {                                                                               \
      std::println("{}", expected.error());                                                        \
      std::exit(1);                                                                                \
    }                                                                                              \
  })

#ifndef NOEXCEPT_ENABLED
#define NOEXCEPT noexcept
#else
#define NOEXCEPT
#endif
