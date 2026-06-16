





#ifndef ZIP7_INC_PPMD7_H
#define ZIP7_INC_PPMD7_H

#include "Ppmd.h"

EXTERN_C_BEGIN

#define PPMD7_MIN_ORDER 2
#define PPMD7_MAX_ORDER 64

#define PPMD7_MIN_MEM_SIZE (1 << 11)
#define PPMD7_MAX_MEM_SIZE (0xFFFFFFFF - 12 * 3)

struct CPpmd7_Context_;

typedef Ppmd_Ref_Type(struct CPpmd7_Context_) CPpmd7_Context_Ref;



typedef struct CPpmd7_Context_
{
  UInt16 NumStats;


  union
  {
    UInt16 SummFreq;
    CPpmd_State2 State2;
  } Union2;

  union
  {
    CPpmd_State_Ref Stats;
    CPpmd_State4 State4;
  } Union4;

  CPpmd7_Context_Ref Suffix;
} CPpmd7_Context;



#define Ppmd7Context_OneState(p) ((CPpmd_State *)&(p)->Union2)




typedef struct
{
  UInt32 Range;
  UInt32 Code;
  UInt32 Low;
  IByteInPtr Stream;
} CPpmd7_RangeDec;


typedef struct
{
  UInt32 Range;
  Byte Cache;

  UInt64 Low;
  UInt64 CacheSize;
  IByteOutPtr Stream;
} CPpmd7z_RangeEnc;


typedef struct
{
  CPpmd7_Context *MinContext, *MaxContext;
  CPpmd_State *FoundState;
  unsigned OrderFall, InitEsc, PrevSuccess, MaxOrder, HiBitsFlag;
  Int32 RunLength, InitRL;

  UInt32 Size;
  UInt32 GlueCount;
  UInt32 AlignOffset;
  Byte *Base, *LoUnit, *HiUnit, *Text, *UnitsStart;




  union
  {
    CPpmd7_RangeDec dec;
    CPpmd7z_RangeEnc enc;
  } rc;

  Byte Indx2Units[PPMD_NUM_INDEXES + 2];
  Byte Units2Indx[128];
  CPpmd_Void_Ref FreeList[PPMD_NUM_INDEXES];

  Byte NS2BSIndx[256], NS2Indx[256];
  Byte ExpEscape[16];
  CPpmd_See DummySee, See[25][16];
  UInt16 BinSumm[128][64];

} CPpmd7;


void Ppmd7_Construct(CPpmd7 *p);
BoolInt Ppmd7_Alloc(CPpmd7 *p, UInt32 size, ISzAllocPtr alloc);
void Ppmd7_Free(CPpmd7 *p, ISzAllocPtr alloc);
void Ppmd7_Init(CPpmd7 *p, unsigned maxOrder);
#define Ppmd7_WasAllocated(p) ((p)->Base != NULL)




#define Ppmd7_GetPtr(p, ptr)     Ppmd_GetPtr(p, ptr)
#define Ppmd7_GetContext(p, ptr) Ppmd_GetPtr_Type(p, ptr, CPpmd7_Context)
#define Ppmd7_GetStats(p, ctx)   Ppmd_GetPtr_Type(p, (ctx)->Union4.Stats, CPpmd_State)

void Ppmd7_Update1(CPpmd7 *p);
void Ppmd7_Update1_0(CPpmd7 *p);
void Ppmd7_Update2(CPpmd7 *p);

#define PPMD7_HiBitsFlag_3(sym) ((((unsigned)sym + 0xC0) >> (8 - 3)) & (1 << 3))
#define PPMD7_HiBitsFlag_4(sym) ((((unsigned)sym + 0xC0) >> (8 - 4)) & (1 << 4))



#define Ppmd7_GetBinSumm(p) \
    &p->BinSumm[(size_t)(unsigned)Ppmd7Context_OneState(p->MinContext)->Freq - 1] \
    [ p->PrevSuccess + ((p->RunLength >> 26) & 0x20) \
    + p->NS2BSIndx[(size_t)Ppmd7_GetContext(p, p->MinContext->Suffix)->NumStats - 1] \
    + PPMD7_HiBitsFlag_4(Ppmd7Context_OneState(p->MinContext)->Symbol) \
    + (p->HiBitsFlag = PPMD7_HiBitsFlag_3(p->FoundState->Symbol)) ]

CPpmd_See *Ppmd7_MakeEscFreq(CPpmd7 *p, unsigned numMasked, UInt32 *scale);











#define PPMD7_SYM_END    (-1)
#define PPMD7_SYM_ERROR  (-2)












BoolInt Ppmd7a_RangeDec_Init(CPpmd7_RangeDec *p);
#define Ppmd7a_RangeDec_IsFinishedOK(p) ((p)->Code == 0)
int Ppmd7a_DecodeSymbol(CPpmd7 *p);


BoolInt Ppmd7z_RangeDec_Init(CPpmd7_RangeDec *p);
#define Ppmd7z_RangeDec_IsFinishedOK(p) ((p)->Code == 0)
int Ppmd7z_DecodeSymbol(CPpmd7 *p);





void Ppmd7z_Init_RangeEnc(CPpmd7 *p);
void Ppmd7z_Flush_RangeEnc(CPpmd7 *p);

void Ppmd7z_EncodeSymbols(CPpmd7 *p, const Byte *buf, const Byte *lim);

EXTERN_C_END

#endif
