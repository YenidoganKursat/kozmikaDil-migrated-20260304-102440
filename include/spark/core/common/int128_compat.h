#pragma once

#include <cstdint>

namespace spark {

#if defined(_MSC_VER) && !defined(__clang__)
using spark_i128 = long long;
using spark_u128 = unsigned long long;
constexpr bool spark_has_native_i128 = false;
#else
using spark_i128 = __int128_t;
using spark_u128 = __uint128_t;
constexpr bool spark_has_native_i128 = true;
#endif

}  // namespace spark
