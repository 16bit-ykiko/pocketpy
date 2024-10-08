#pragma once

// clang-format off

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    //define something for Windows (32-bit and 64-bit, this part is common)
    #define PK_EXPORT __declspec(dllexport)
    #define PY_SYS_PLATFORM     0
    #define PY_SYS_PLATFORM_STRING "win32"
#elif __EMSCRIPTEN__
    #define PK_EXPORT
    #define PY_SYS_PLATFORM     1
    #define PY_SYS_PLATFORM_STRING "emscripten"
#elif __APPLE__
    #include <TargetConditionals.h>
    #if TARGET_IPHONE_SIMULATOR
        // iOS, tvOS, or watchOS Simulator
        #define PY_SYS_PLATFORM     2
        #define PY_SYS_PLATFORM_STRING "ios"
    #elif TARGET_OS_IPHONE
        // iOS, tvOS, or watchOS device
        #define PY_SYS_PLATFORM     2
        #define PY_SYS_PLATFORM_STRING "ios"
    #elif TARGET_OS_MAC
        #define PY_SYS_PLATFORM     3
        #define PY_SYS_PLATFORM_STRING "darwin"
    #else
    #   error "Unknown Apple platform"
    #endif
    #define PK_EXPORT __attribute__((visibility("default")))
#elif __ANDROID__
    #define PK_EXPORT __attribute__((visibility("default")))
    #define PY_SYS_PLATFORM     4
    #define PY_SYS_PLATFORM_STRING "android"
#elif __linux__
    #define PK_EXPORT __attribute__((visibility("default")))
    #define PY_SYS_PLATFORM     5
    #define PY_SYS_PLATFORM_STRING "linux"
#else
    #define PK_EXPORT
    #define PY_SYS_PLATFORM     6
    #define PY_SYS_PLATFORM_STRING "unknown"
#endif

#if PY_SYS_PLATFORM == 0 || PY_SYS_PLATFORM == 3 || PY_SYS_PLATFORM == 5
    #define PK_IS_DESKTOP_PLATFORM 1
#else
    #define PK_IS_DESKTOP_PLATFORM 0
#endif
