#ifndef __AUSTERE_H__
#define __AUSTERE_H__
#include <stdlib.h>
typedef unsigned long long	u64;
typedef unsigned int		u32;
typedef unsigned short		u16;
typedef unsigned char		u8;
typedef signed long long	s64;
typedef signed int			s32;
typedef signed short		s16;
typedef signed char			s8;
typedef double				f64;
typedef float				f32;
typedef unsigned short      f16;
typedef int                 bool;
const static bool           false = 0;
const static bool           true = 1;
#endif
#ifndef DLLEXPORT
    #ifdef _MSC_VER
        #define DLLEXPORT __declspec(dllexport)
    #else
        #define DLLEXPORT __attribute__((visibility("default")))
    #endif
#endif
#ifndef DLLIMPORT
    #ifdef _MSC_VER
        #define DLLIMPORT __declspec(dllimport)
    #else
        #define DLLIMPORT
    #endif
#endif
#ifdef _WIN32
    #ifndef OS_WINDOWS
    #define OS_WINDOWS
    #endif
#elif defined(__APPLE__)
    #ifndef OS_APPLE
    #define OS_APPLE
    #endif
#else
    #ifndef OS_LINUX
    #define OS_LINUX
    #endif
#endif
