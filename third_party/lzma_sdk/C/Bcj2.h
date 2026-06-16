


#ifndef ZIP7_INC_BCJ2_H
#define ZIP7_INC_BCJ2_H

#include "7zTypes.h"

EXTERN_C_BEGIN

#define BCJ2_NUM_STREAMS 4

enum
{
  BCJ2_STREAM_MAIN,
  BCJ2_STREAM_CALL,
  BCJ2_STREAM_JUMP,
  BCJ2_STREAM_RC
};

enum
{
  BCJ2_DEC_STATE_ORIG_0 = BCJ2_NUM_STREAMS,
  BCJ2_DEC_STATE_ORIG_1,
  BCJ2_DEC_STATE_ORIG_2,
  BCJ2_DEC_STATE_ORIG_3,

  BCJ2_DEC_STATE_ORIG,
  BCJ2_DEC_STATE_ERROR
};

enum
{
  BCJ2_ENC_STATE_ORIG = BCJ2_NUM_STREAMS,
  BCJ2_ENC_STATE_FINISHED
};



#define BCJ2_IS_32BIT_STREAM(s) ((unsigned)((unsigned)(s) - (unsigned)BCJ2_STREAM_CALL) < 2)











typedef UInt16 CBcj2Prob;








































typedef struct
{
  const Byte *bufs[BCJ2_NUM_STREAMS];
  const Byte *lims[BCJ2_NUM_STREAMS];
  Byte *dest;
  const Byte *destLim;

  unsigned state;

  UInt32 ip;
  UInt32 temp;
  UInt32 range;
  UInt32 code;
  CBcj2Prob probs[2 + 256];
} CBcj2Dec;






void Bcj2Dec_Init(CBcj2Dec *p);







SRes Bcj2Dec_Decode(CBcj2Dec *p);


















#define Bcj2Dec_IsMaybeFinished_state_MAIN(_p_) ((_p_)->state == BCJ2_STREAM_MAIN)





#define Bcj2Dec_IsMaybeFinished_code(_p_) ((_p_)->code == 0)






#define Bcj2Dec_IsMaybeFinished(_p_) ( \
        Bcj2Dec_IsMaybeFinished_state_MAIN(_p_) && \
        Bcj2Dec_IsMaybeFinished_code(_p_))





typedef enum
{
  BCJ2_ENC_FINISH_MODE_CONTINUE,
  BCJ2_ENC_FINISH_MODE_END_BLOCK,
  BCJ2_ENC_FINISH_MODE_END_STREAM
} EBcj2Enc_FinishMode;



































































typedef UInt64 CBcj2Enc_ip_unsigned;
typedef  Int64 CBcj2Enc_ip_signed;


#define BCJ2_ENC_FileSize_MAX             ((CBcj2Enc_ip_unsigned)0 - 2)



#define BCJ2_ENC_FileSizeField_UNLIMITED  ((CBcj2Enc_ip_unsigned)0 - 1)


#define BCJ2_ENC_GET_FileSizeField_VAL_FROM_FileSize(fileSize) \
    ((CBcj2Enc_ip_unsigned)(fileSize) - 1)


#define Bcj2Enc_SET_FileSize(p, fileSize) \
    (p)->fileSize64_minus1 = BCJ2_ENC_GET_FileSizeField_VAL_FROM_FileSize(fileSize);


typedef struct
{
  Byte *bufs[BCJ2_NUM_STREAMS];
  const Byte *lims[BCJ2_NUM_STREAMS];
  const Byte *src;
  const Byte *srcLim;

  unsigned state;
  EBcj2Enc_FinishMode finishMode;

  Byte context;
  Byte flushRem;
  Byte isFlushState;

  Byte cache;
  UInt32 range;
  UInt64 low;
  UInt64 cacheSize;





  CBcj2Enc_ip_unsigned ip64;
  CBcj2Enc_ip_unsigned fileIp64;
  CBcj2Enc_ip_unsigned fileSize64_minus1;
  UInt32 relatLimit;


  UInt32 tempTarget;
  unsigned tempPos;


  Byte temp[8];
  CBcj2Prob probs[2 + 256];
} CBcj2Enc;

void Bcj2Enc_Init(CBcj2Enc *p);










void Bcj2Enc_Encode(CBcj2Enc *p);













#define Bcj2Enc_Get_AvailInputSize_in_Temp(p) ((p)->tempPos)

#define Bcj2Enc_IsFinished(p) ((p)->flushRem == 0)







#define BCJ2_ENC_RELAT_LIMIT_DEFAULT  ((UInt32)0x0f << 24)
#define BCJ2_ENC_RELAT_LIMIT_MAX      ((UInt32)1 << 31)


EXTERN_C_END

#endif
