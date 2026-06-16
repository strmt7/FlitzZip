


#include "Precomp.h"



#include "CpuArch.h"

#ifdef MY_CPU_X86_OR_AMD64

#undef NEED_CHECK_FOR_CPUID
#if !defined(MY_CPU_AMD64)
#define NEED_CHECK_FOR_CPUID
#endif












#if defined(__GNUC__)  \
    || defined(__clang__)

















#define ASM_LN "\n"

#if defined(MY_CPU_AMD64) && defined(__PIC__) \
    && ((defined (__GNUC__) && (__GNUC__ < 5)) || defined(__clang__))





#define x86_cpuid_MACRO_2(p, func, subFunc) { \
  __asm__ __volatile__ ( \
    ASM_LN   "mov     %%rbx, %q1"  \
    ASM_LN   "cpuid"               \
    ASM_LN   "xchg    %%rbx, %q1"  \
    : "=a" ((p)[0]), "=&r" ((p)[1]), "=c" ((p)[2]), "=d" ((p)[3]) : "0" (func), "2"(subFunc)); }

#elif defined(MY_CPU_X86) && defined(__PIC__) \
    && ((defined (__GNUC__) && (__GNUC__ < 5)) || defined(__clang__))

#define x86_cpuid_MACRO_2(p, func, subFunc) { \
  __asm__ __volatile__ ( \
    ASM_LN   "mov     %%ebx, %k1"  \
    ASM_LN   "cpuid"               \
    ASM_LN   "xchg    %%ebx, %k1"  \
    : "=a" ((p)[0]), "=&r" ((p)[1]), "=c" ((p)[2]), "=d" ((p)[3]) : "0" (func), "2"(subFunc)); }

#else

#define x86_cpuid_MACRO_2(p, func, subFunc) { \
  __asm__ __volatile__ ( \
    ASM_LN   "cpuid"               \
    : "=a" ((p)[0]), "=b" ((p)[1]), "=c" ((p)[2]), "=d" ((p)[3]) : "0" (func), "2"(subFunc)); }

#endif

#define x86_cpuid_MACRO(p, func)  x86_cpuid_MACRO_2(p, func, 0)

void Z7_FASTCALL z7_x86_cpuid(UInt32 p[4], UInt32 func)
{
  x86_cpuid_MACRO(p, func)
}

static
void Z7_FASTCALL z7_x86_cpuid_subFunc(UInt32 p[4], UInt32 func, UInt32 subFunc)
{
  x86_cpuid_MACRO_2(p, func, subFunc)
}


Z7_NO_INLINE
UInt32 Z7_FASTCALL z7_x86_cpuid_GetMaxFunc(void)
{
 #if defined(NEED_CHECK_FOR_CPUID)
  #define EFALGS_CPUID_BIT 21
  UInt32 a;
  __asm__ __volatile__ (
    ASM_LN   "pushf"
    ASM_LN   "pushf"
    ASM_LN   "pop     %0"


    ASM_LN   "btc     %1, %0"
    ASM_LN   "push    %0"
    ASM_LN   "popf"
    ASM_LN   "pushf"
    ASM_LN   "pop     %0"
    ASM_LN   "xorl    (%%esp), %0"

    ASM_LN   "popf"
    ASM_LN
    : "=&r" (a)
    : "i" (EFALGS_CPUID_BIT)
    );
  if ((a & (1 << EFALGS_CPUID_BIT)) == 0)
    return 0;
 #endif
  {
    UInt32 p[4];
    x86_cpuid_MACRO(p, 0)
    return p[0];
  }
}

#undef ASM_LN

#elif !defined(_MSC_VER)














void Z7_FASTCALL z7_x86_cpuid(UInt32 p[4], UInt32 func)
{
  UNUSED_VAR(func)
  p[0] = p[1] = p[2] = p[3] = 0;
}
UInt32 Z7_FASTCALL z7_x86_cpuid_GetMaxFunc(void)
{
  return 0;
}

#else

#if !defined(MY_CPU_AMD64)

UInt32 __declspec(naked) Z7_FASTCALL z7_x86_cpuid_GetMaxFunc(void)
{
  #if defined(NEED_CHECK_FOR_CPUID)
  #define EFALGS_CPUID_BIT 21
  __asm   pushfd
  __asm   pushfd






  __asm   btc     dword ptr [esp], EFALGS_CPUID_BIT
  __asm   popfd
  __asm   pushfd
  __asm   pop     eax

  __asm   xor     eax, [esp]

  __asm   popfd
  __asm   and     eax, (1 shl EFALGS_CPUID_BIT)
  __asm   jz end_func
  #endif
  __asm   push    ebx
  __asm   xor     eax, eax
  __asm   xor     ecx, ecx
  __asm   cpuid
  __asm   pop     ebx
  #if defined(NEED_CHECK_FOR_CPUID)
  end_func:
  #endif
  __asm   ret 0
}

void __declspec(naked) Z7_FASTCALL z7_x86_cpuid(UInt32 p[4], UInt32 func)
{
  UNUSED_VAR(p)
  UNUSED_VAR(func)
  __asm   push    ebx
  __asm   push    edi
  __asm   mov     edi, ecx
  __asm   mov     eax, edx
  __asm   xor     ecx, ecx
  __asm   cpuid
  __asm   mov     [edi     ], eax
  __asm   mov     [edi +  4], ebx
  __asm   mov     [edi +  8], ecx
  __asm   mov     [edi + 12], edx
  __asm   pop     edi
  __asm   pop     ebx
  __asm   ret     0
}

static
void __declspec(naked) Z7_FASTCALL z7_x86_cpuid_subFunc(UInt32 p[4], UInt32 func, UInt32 subFunc)
{
  UNUSED_VAR(p)
  UNUSED_VAR(func)
  UNUSED_VAR(subFunc)
  __asm   push    ebx
  __asm   push    edi
  __asm   mov     edi, ecx
  __asm   mov     eax, edx
  __asm   mov     ecx, [esp + 12]
  __asm   cpuid
  __asm   mov     [edi     ], eax
  __asm   mov     [edi +  4], ebx
  __asm   mov     [edi +  8], ecx
  __asm   mov     [edi + 12], edx
  __asm   pop     edi
  __asm   pop     ebx
  __asm   ret     4
}

#else

    #if _MSC_VER >= 1600
      #include <intrin.h>
      #define MY_cpuidex  __cpuidex

static
void Z7_FASTCALL z7_x86_cpuid_subFunc(UInt32 p[4], UInt32 func, UInt32 subFunc)
{
  __cpuidex((int *)p, func, subFunc);
}

    #else














static
Z7_NO_INLINE void Z7_FASTCALL MY_cpuidex_HACK(Int32 subFunction, Int32 func, Int32 *CPUInfo)
{
  UNUSED_VAR(subFunction)
  __cpuid(CPUInfo, func);
}
      #define MY_cpuidex(info, func, func2)  MY_cpuidex_HACK(func2, func, info)
static
void Z7_FASTCALL z7_x86_cpuid_subFunc(UInt32 p[4], UInt32 func, UInt32 subFunc)
{
  MY_cpuidex_HACK(subFunc, func, (Int32 *)p);
}
    #endif

#if !defined(MY_CPU_AMD64)


Z7_NO_INLINE
#endif
void Z7_FASTCALL z7_x86_cpuid(UInt32 p[4], UInt32 func)
{
  MY_cpuidex((Int32 *)p, (Int32)func, 0);
}

Z7_NO_INLINE
UInt32 Z7_FASTCALL z7_x86_cpuid_GetMaxFunc(void)
{
  Int32 a[4];
  MY_cpuidex(a, 0, 0);
  return a[0];
}

#endif
#endif

#if defined(NEED_CHECK_FOR_CPUID)
#define CHECK_CPUID_IS_SUPPORTED { if (z7_x86_cpuid_GetMaxFunc() == 0) return 0; }
#else
#define CHECK_CPUID_IS_SUPPORTED
#endif
#undef NEED_CHECK_FOR_CPUID


static
BoolInt x86cpuid_Func_1(UInt32 *p)
{
  CHECK_CPUID_IS_SUPPORTED
  z7_x86_cpuid(p, 1);
  return True;
}










































































#ifdef _WIN32
#include "7zWindows.h"
#endif

#if !defined(MY_CPU_AMD64) && defined(_WIN32)






Z7_FORCE_INLINE
static BoolInt CPU_Sys_Is_SSE_Supported(void)
{
#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable : 4996)
#endif



  return (Byte)GetVersion() >= 5;
#if defined(_MSC_VER)
  #pragma warning(pop)
#endif
}
#define CHECK_SYS_SSE_SUPPORT if (!CPU_Sys_Is_SSE_Supported()) return False;
#else
#define CHECK_SYS_SSE_SUPPORT
#endif


#if !defined(MY_CPU_AMD64)

BoolInt CPU_IsSupported_CMOV(void)
{
  UInt32 a[4];
  if (!x86cpuid_Func_1(&a[0]))
    return 0;
  return (BoolInt)(a[3] >> 15) & 1;
}

BoolInt CPU_IsSupported_SSE(void)
{
  UInt32 a[4];
  CHECK_SYS_SSE_SUPPORT
  if (!x86cpuid_Func_1(&a[0]))
    return 0;
  return (BoolInt)(a[3] >> 25) & 1;
}

BoolInt CPU_IsSupported_SSE2(void)
{
  UInt32 a[4];
  CHECK_SYS_SSE_SUPPORT
  if (!x86cpuid_Func_1(&a[0]))
    return 0;
  return (BoolInt)(a[3] >> 26) & 1;
}

#endif


static UInt32 x86cpuid_Func_1_ECX(void)
{
  UInt32 a[4];
  CHECK_SYS_SSE_SUPPORT
  if (!x86cpuid_Func_1(&a[0]))
    return 0;
  return a[2];
}

BoolInt CPU_IsSupported_AES(void)
{
  return (BoolInt)(x86cpuid_Func_1_ECX() >> 25) & 1;
}

BoolInt CPU_IsSupported_SSSE3(void)
{
  return (BoolInt)(x86cpuid_Func_1_ECX() >> 9) & 1;
}

BoolInt CPU_IsSupported_SSE41(void)
{
  return (BoolInt)(x86cpuid_Func_1_ECX() >> 19) & 1;
}

BoolInt CPU_IsSupported_SHA(void)
{
  CHECK_SYS_SSE_SUPPORT

  if (z7_x86_cpuid_GetMaxFunc() < 7)
    return False;
  {
    UInt32 d[4];
    z7_x86_cpuid(d, 7);
    return (BoolInt)(d[1] >> 29) & 1;
  }
}


BoolInt CPU_IsSupported_SHA512(void)
{
  if (!CPU_IsSupported_AVX2()) return False;

  if (z7_x86_cpuid_GetMaxFunc() < 7)
    return False;
  {
    UInt32 d[4];
    z7_x86_cpuid_subFunc(d, 7, 0);
    if (d[0] < 1)
      return False;
    z7_x86_cpuid_subFunc(d, 7, 1);
    return (BoolInt)(d[0]) & 1;
  }
}

























#if    defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 1100) \
    || defined(_MSC_VER) && (_MSC_VER >= 1600) && (_MSC_FULL_VER >= 160040219)  \
    || defined(__GNUC__) && (__GNUC__ >= 9) \
    || defined(__clang__) && (__clang_major__ >= 9)

#if defined(__INTEL_COMPILER)
#define ATTRIB_XGETBV
#elif defined(__GNUC__) || defined(__clang__)


#else
#define ATTRIB_XGETBV
#endif
#endif

#if defined(ATTRIB_XGETBV)
#include <immintrin.h>
#endif



#define MY_XCR_XFEATURE_ENABLED_MASK 0

#if defined(ATTRIB_XGETBV)
ATTRIB_XGETBV
#endif
static UInt64 x86_xgetbv_0(UInt32 num)
{
#if defined(ATTRIB_XGETBV)
  {
    return
      #if (defined(_MSC_VER))
        _xgetbv(num);
      #else
        __builtin_ia32_xgetbv(
          #if !defined(__clang__)
            (int)
          #endif
            num);
      #endif
  }

#elif defined(__GNUC__) || defined(__clang__) || defined(__SUNPRO_CC)

  UInt32 a, d;
 #if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4))
  __asm__
  (
    "xgetbv"
    : "=a"(a), "=d"(d) : "c"(num) : "cc"
  );
 #else
  __asm__
  (
    ".byte 0x0f, 0x01, 0xd0" "\n\t"
    : "=a"(a), "=d"(d) : "c"(num) : "cc"
  );
 #endif
  return ((UInt64)d << 32) | a;


#elif defined(_MSC_VER) && !defined(MY_CPU_AMD64)

  UInt32 a, d;
  __asm {
    push eax
    push edx
    push ecx
    mov ecx, num;

    _emit 0x0f
    _emit 0x01
    _emit 0xd0
    mov a, eax
    mov d, edx
    pop ecx
    pop edx
    pop eax
  }
  return ((UInt64)d << 32) | a;


#else

  UNUSED_VAR(num)




  return

        (1 << 1)
      | (1 << 2);

#endif
}

#ifdef _WIN32











#define MY_PF_XSAVE_ENABLED                            17






#endif

BoolInt CPU_IsSupported_AVX(void)
{
  #ifdef _WIN32
  if (!IsProcessorFeaturePresent(MY_PF_XSAVE_ENABLED))
    return False;







  #endif















  {
    const UInt32 c = x86cpuid_Func_1_ECX();
    if (0 == (1
        & (c >> 28)
        & (c >> 27)))
      return False;
  }














  {
    const UInt32 bm = (UInt32)x86_xgetbv_0(MY_XCR_XFEATURE_ENABLED_MASK);

    return 1
        & (BoolInt)(bm >> 1)
        & (BoolInt)(bm >> 2);
  }

}


BoolInt CPU_IsSupported_AVX2(void)
{
  if (!CPU_IsSupported_AVX())
    return False;
  if (z7_x86_cpuid_GetMaxFunc() < 7)
    return False;
  {
    UInt32 d[4];
    z7_x86_cpuid(d, 7);

    return 1
      & (BoolInt)(d[1] >> 5);
  }
}

BoolInt CPU_IsSupported_VAES_AVX2(void)
{
  if (!CPU_IsSupported_AVX())
    return False;
  if (z7_x86_cpuid_GetMaxFunc() < 7)
    return False;
  {
    UInt32 d[4];
    z7_x86_cpuid(d, 7);

    return 1
      & (BoolInt)(d[1] >> 5)

      & (BoolInt)(d[2] >> 9);
  }
}

BoolInt CPU_IsSupported_PageGB(void)
{
  CHECK_CPUID_IS_SUPPORTED
  {
    UInt32 d[4];
    z7_x86_cpuid(d, 0x80000000);
    if (d[0] < 0x80000001)
      return False;
    z7_x86_cpuid(d, 0x80000001);
    return (BoolInt)(d[3] >> 26) & 1;
  }
}


#elif defined(MY_CPU_ARM_OR_ARM64)

#ifdef _WIN32

#include "7zWindows.h"

BoolInt CPU_IsSupported_CRC32(void)  { return IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) ? 1 : 0; }
BoolInt CPU_IsSupported_CRYPTO(void) { return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) ? 1 : 0; }
BoolInt CPU_IsSupported_NEON(void)   { return IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE) ? 1 : 0; }

#else

#if defined(__APPLE__)























static BoolInt z7_sysctlbyname_Get_BoolInt(const char *name)
{
  UInt32 val = 0;
  if (z7_sysctlbyname_Get_UInt32(name, &val) == 0 && val == 1)
    return 1;
  return 0;
}

BoolInt CPU_IsSupported_CRC32(void)
{
  return z7_sysctlbyname_Get_BoolInt("hw.optional.armv8_crc32");
}

BoolInt CPU_IsSupported_NEON(void)
{
  return z7_sysctlbyname_Get_BoolInt("hw.optional.neon");
}

BoolInt CPU_IsSupported_SHA512(void)
{
  return z7_sysctlbyname_Get_BoolInt("hw.optional.armv8_2_sha512");
}








#ifdef MY_CPU_ARM64
#define APPLE_CRYPTO_SUPPORT_VAL 1
#else
#define APPLE_CRYPTO_SUPPORT_VAL 0
#endif

BoolInt CPU_IsSupported_SHA2(void) { return APPLE_CRYPTO_SUPPORT_VAL; }
BoolInt CPU_IsSupported_AES (void) { return APPLE_CRYPTO_SUPPORT_VAL; }


#else

#if defined(__GLIBC__) && (__GLIBC__ * 100 + __GLIBC_MINOR__ >= 216)
  #define Z7_GETAUXV_AVAILABLE
#elif !defined(__QNXNTO__)

  #if defined __has_include
  #if __has_include (<sys/auxv.h>)

    #define Z7_GETAUXV_AVAILABLE
  #endif
  #endif
#endif

#ifdef Z7_GETAUXV_AVAILABLE

#include <sys/auxv.h>
#define USE_HWCAP
#endif

#ifdef USE_HWCAP

#if defined(__FreeBSD__) || defined(__OpenBSD__)
static unsigned long MY_getauxval(int aux)
{
  unsigned long val;
  if (elf_aux_info(aux, &val, sizeof(val)))
    return 0;
  return val;
}
#else
#define MY_getauxval  getauxval
  #if defined __has_include
  #if __has_include (<asm/hwcap.h>)
#include <asm/hwcap.h>
  #endif
  #endif
#endif

  #define MY_HWCAP_CHECK_FUNC_2(name1, name2) \
  BoolInt CPU_IsSupported_ ## name1(void) { return (MY_getauxval(AT_HWCAP)  & (HWCAP_  ## name2)); }

#ifdef MY_CPU_ARM64
  #define MY_HWCAP_CHECK_FUNC(name) \
  MY_HWCAP_CHECK_FUNC_2(name, name)
#if 1 || defined(__ARM_NEON)
  BoolInt CPU_IsSupported_NEON(void) { return True; }
#else
  MY_HWCAP_CHECK_FUNC_2(NEON, ASIMD)
#endif

#elif defined(MY_CPU_ARM)
  #define MY_HWCAP_CHECK_FUNC(name) \
  BoolInt CPU_IsSupported_ ## name(void) { return (MY_getauxval(AT_HWCAP2) & (HWCAP2_ ## name)); }
  MY_HWCAP_CHECK_FUNC_2(NEON, NEON)
#endif

#else

  #define MY_HWCAP_CHECK_FUNC(name) \
  BoolInt CPU_IsSupported_ ## name(void) { return 0; }
#if defined(__ARM_NEON)
  BoolInt CPU_IsSupported_NEON(void) { return True; }
#else
  MY_HWCAP_CHECK_FUNC(NEON)
#endif

#endif

MY_HWCAP_CHECK_FUNC (CRC32)
MY_HWCAP_CHECK_FUNC (SHA2)
MY_HWCAP_CHECK_FUNC (AES)
#ifdef MY_CPU_ARM64


#ifndef HWCAP_SHA3

#endif
#ifndef HWCAP_SHA512

#define HWCAP_SHA512  (1 << 21)
#endif
MY_HWCAP_CHECK_FUNC (SHA512)

#endif

#endif
#endif

#endif



#ifdef __APPLE__

#include <sys/sysctl.h>

int z7_sysctlbyname_Get(const char *name, void *buf, size_t *bufSize)
{
  return sysctlbyname(name, buf, bufSize, NULL, 0);
}

int z7_sysctlbyname_Get_UInt32(const char *name, UInt32 *val)
{
  size_t bufSize = sizeof(*val);
  const int res = z7_sysctlbyname_Get(name, val, &bufSize);
  if (res == 0 && bufSize != sizeof(*val))
    return EFAULT;
  return res;
}

#endif
