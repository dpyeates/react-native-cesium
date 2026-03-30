// Shadow header: must appear on the include path before Cesium Native's bundled fmt.
// Apple Clang rejects fmt's default FMT_STRING (consteval / FMT_COMPILE_STRING) in some
// spdlog helpers; runtime format strings are sufficient for this library.
#pragma once

#include_next <fmt/format.h>

#undef FMT_STRING
#define FMT_STRING(s) ::fmt::runtime(s)
