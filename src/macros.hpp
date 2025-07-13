#pragma once

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
