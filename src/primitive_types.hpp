#pragma once

#include <cstddef>
#include <cstdint> // For standard fixed-width types

// ==============================================
// Unsigned integer aliases
// ==============================================
using u8 = std::uint8_t;   // 8-bit unsigned
using u16 = std::uint16_t; // 16-bit unsigned
using u32 = std::uint32_t; // 32-bit unsigned
using u64 = std::uint64_t; // 64-bit unsigned

// ==============================================
// Signed integer aliases
// ==============================================
using i8 = std::int8_t;   // 8-bit signed
using i16 = std::int16_t; // 16-bit signed
using i32 = std::int32_t; // 32-bit signed
using i64 = std::int64_t; // 64-bit signed

// ==============================================
// Size-specific aliases
// ==============================================
using usize = std::size_t;    // Typical size type
using ssize = std::ptrdiff_t; // Signed size type

// ==============================================
// Fast/compact variants
// ==============================================
using ufast32 = std::uint_fast32_t;
using ifast32 = std::int_fast32_t;

// ==============================================
// Floating Point Aliases (with size validation)
// ==============================================
using f32 = float;       // IEEE-754 single precision
using f64 = double;      // IEEE-754 double precision
using f80 = long double; // Extended precision (implementation-dependent)
