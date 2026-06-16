


#include "Precomp.h"

#include "Bra.h"
#include "CpuArch.h"


#if defined(MY_CPU_SIZEOF_POINTER) \
    && ( MY_CPU_SIZEOF_POINTER == 4 \
      || MY_CPU_SIZEOF_POINTER == 8)
  #define BR_CONV_USE_OPT_PC_PTR
#endif

#ifdef BR_CONV_USE_OPT_PC_PTR
#define BR_PC_INIT  pc -= (UInt32)(SizeT)p;
#define BR_PC_GET   (pc + (UInt32)(SizeT)p)
#else
#define BR_PC_INIT  pc += (UInt32)size;
#define BR_PC_GET   (pc - (UInt32)(SizeT)(lim - p))


#endif

#define BR_CONVERT_VAL(v, c) if (encoding) v += c; else v -= c;


#define Z7_BRANCH_CONV_ST(name) z7_BranchConvSt_ ## name

#define BR86_NEED_CONV_FOR_MS_BYTE(b) ((((b) + 1) & 0xfe) == 0)

#ifdef MY_CPU_LE_UNALIGN
  #define BR86_PREPARE_BCJ_SCAN  const UInt32 v = GetUi32(p) ^ 0xe8e8e8e8;
  #define BR86_IS_BCJ_BYTE(n)    ((v & ((UInt32)0xfe << (n) * 8)) == 0)
#else
  #define BR86_PREPARE_BCJ_SCAN

  #define BR86_IS_BCJ_BYTE(n)    ((p[n - 4] & 0xfe) == 0xe8)


#endif

static
Z7_FORCE_INLINE
Z7_ATTRIB_NO_VECTOR
Byte *Z7_BRANCH_CONV_ST(X86)(Byte *p, SizeT size, UInt32 pc, UInt32 *state, int encoding)
{
  if (size < 5)
    return p;
 {

  /*
     The x86 BCJ converter scans for CALL/JMP opcodes and normalizes their
     relative target operands against the logical program counter. The compact
     label structure mirrors the upstream hot path and keeps chunk-state
     handling branch-predictable.
  */
  const Byte *lim = p + size - 4;
  unsigned mask = (unsigned)*state;
#ifdef BR_CONV_USE_OPT_PC_PTR





  pc += 4;
#endif
  BR_PC_INIT
  goto start;

  for (;; mask |= 4)
  {

  start:
    if (p >= lim)
      goto fin;
    {
      /*
         Four bytes are scanned as a group. The mask records partial opcode
         matches near a chunk boundary so the next call can continue conversion
         without rereading data before the current buffer.
      */
      BR86_PREPARE_BCJ_SCAN
      p += 4;
      if (BR86_IS_BCJ_BYTE(0))  { goto m0; }  mask >>= 1;
      if (BR86_IS_BCJ_BYTE(1))  { goto m1; }  mask >>= 1;
      if (BR86_IS_BCJ_BYTE(2))  { goto m2; }  mask = 0;
      if (BR86_IS_BCJ_BYTE(3))  { goto a3; }
    }
    goto main_loop;

  m0: p--;
  m1: p--;
  m2: p--;
    if (mask == 0)
      goto a3;
    if (p > lim)
      goto fin_p;


    if (mask > 4 || mask == 3)
    {
      mask >>= 1;
      continue;
    }
    mask >>= 1;
    /*
       A candidate instruction is converted only when the high operand byte
       matches the signed range used by x86 relative branch encodings.
    */
    if (BR86_NEED_CONV_FOR_MS_BYTE(p[mask]))
      continue;

    {
      UInt32 v = GetUi32(p);
      UInt32 c;
      v += (1 << 24);  if (v & 0xfe000000) continue;
      c = BR_PC_GET;
      BR_CONVERT_VAL(v, c)
      {
        mask <<= 3;
        if (BR86_NEED_CONV_FOR_MS_BYTE(v >> mask))
        {
          v ^= (((UInt32)0x100 << mask) - 1);
          #ifdef MY_CPU_X86

            c = BR_PC_GET;
          #endif
          BR_CONVERT_VAL(v, c)
        }
        mask = 0;
      }

      v &= (1 << 25) - 1;  v -= (1 << 24);
      SetUi32(p, v)
      p += 4;
      goto main_loop;
    }

  main_loop:
    if (p >= lim)
      goto fin;
    for (;;)
    {
      /*
         Once no boundary state is pending, the main loop skips directly to
         the next branch opcode byte and leaves non-branch data untouched.
      */
      BR86_PREPARE_BCJ_SCAN
      p += 4;
      if (BR86_IS_BCJ_BYTE(0))  { goto a0; }
      if (BR86_IS_BCJ_BYTE(1))  { goto a1; }
      if (BR86_IS_BCJ_BYTE(2))  { goto a2; }
      if (BR86_IS_BCJ_BYTE(3))  { goto a3; }
      if (p >= lim)
        goto fin;
    }

  a0: p--;
  a1: p--;
  a2: p--;
  a3:
    if (p > lim)
      goto fin_p;

    {
      UInt32 v = GetUi32(p);
      UInt32 c;
      v += (1 << 24);  if (v & 0xfe000000) continue;
      c = BR_PC_GET;
      BR_CONVERT_VAL(v, c)

      v &= (1 << 25) - 1;  v -= (1 << 24);
      SetUi32(p, v)
      p += 4;
      goto main_loop;
    }
  }

fin_p:
  p--;
fin:







  /*
     Persist the boundary mask for the caller. The next chunk uses this state
     to finish a branch operand that started near the end of the current chunk.
  */
  *state = (UInt32)mask;
  return p;
 }
}


#define Z7_BRANCH_CONV_ST_FUNC_IMP(name, m, encoding) \
Z7_NO_INLINE \
Z7_ATTRIB_NO_VECTOR \
Byte *m(name)(Byte *data, SizeT size, UInt32 pc, UInt32 *state) \
  { return Z7_BRANCH_CONV_ST(name)(data, size, pc, state, encoding); }

Z7_BRANCH_CONV_ST_FUNC_IMP(X86, Z7_BRANCH_CONV_ST_DEC, 0)
#ifndef Z7_EXTRACT_ONLY
Z7_BRANCH_CONV_ST_FUNC_IMP(X86, Z7_BRANCH_CONV_ST_ENC, 1)
#endif
