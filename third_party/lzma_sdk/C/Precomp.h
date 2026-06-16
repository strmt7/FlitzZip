


#ifndef ZIP7_INC_PRECOMP_H
#define ZIP7_INC_PRECOMP_H


















#include "Compiler.h"

#ifdef _MSC_VER

#if _MSC_VER >= 1912

#endif
#endif











#ifndef Z7_LARGE_PAGES
#if !defined(Z7_NO_LARGE_PAGES) && !defined(UNDER_CE)
#define Z7_LARGE_PAGES 1
#endif
#endif

#ifdef _WIN32





#ifndef Z7_LONG_PATH
#ifndef Z7_NO_LONG_PATH
#define Z7_LONG_PATH 1
#endif
#endif

#ifndef Z7_DEVICE_FILE
#ifndef Z7_NO_DEVICE_FILE

#endif
#endif


#ifndef _WINDOWS_

#ifndef Z7_WIN32_WINNT_MIN
  #if defined(_M_ARM64) || defined(__aarch64__)

    #define Z7_WIN32_WINNT_MIN  0x0600
  #elif defined(_M_ARM) && defined(_M_ARMT) && defined(_M_ARM_NT)

    #define Z7_WIN32_WINNT_MIN  0x0600
  #elif defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(_M_IA64)
    #define Z7_WIN32_WINNT_MIN  0x0503


  #else
    #define Z7_WIN32_WINNT_MIN  0x0500

  #endif
#endif


#ifndef Z7_DO_NOT_DEFINE_WIN32_WINNT
#ifdef _WIN32_WINNT

#else
  #ifndef Z7_NO_DEFINE_WIN32_WINNT
Z7_DIAGNOSTIC_IGNORE_BEGIN_RESERVED_MACRO_IDENTIFIER
    #define _WIN32_WINNT  Z7_WIN32_WINNT_MIN
Z7_DIAGNOSTIC_IGNORE_END_RESERVED_MACRO_IDENTIFIER
  #endif
#endif

#ifndef WINVER
  #define WINVER  _WIN32_WINNT
#endif
#endif


#ifndef _MBCS
#ifndef Z7_NO_UNICODE


#ifndef UNICODE
#define UNICODE 1
#endif

#ifndef _UNICODE
Z7_DIAGNOSTIC_IGNORE_BEGIN_RESERVED_MACRO_IDENTIFIER
#define _UNICODE 1
Z7_DIAGNOSTIC_IGNORE_END_RESERVED_MACRO_IDENTIFIER
#endif

#endif
#endif
#endif



#endif

#endif
