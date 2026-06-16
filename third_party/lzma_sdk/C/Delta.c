


#include "Precomp.h"

#include "Delta.h"

void Delta_Init(Byte *state)
{
  unsigned i;
  for (i = 0; i < DELTA_STATE_SIZE; i++)
    state[i] = 0;
}


void Delta_Encode(Byte *state, unsigned delta, Byte *data, SizeT size)
{
  Byte temp[DELTA_STATE_SIZE];

  if (size == 0)
    return;

  {
    unsigned i = 0;
    do
      temp[i] = state[i];
    while (++i != delta);
  }

  if (size <= delta)
  {
    unsigned i = 0, k;
    do
    {
      Byte b = *data;
      *data++ = (Byte)(b - temp[i]);
      temp[i] = b;
    }
    while (++i != size);

    k = 0;

    do
    {
      if (i == delta)
        i = 0;
      state[k] = temp[i++];
    }
    while (++k != delta);

    return;
  }

  {
    Byte *p = data + size - delta;
    {
      unsigned i = 0;
      do
        state[i] = *p++;
      while (++i != delta);
    }
    {
      const Byte *lim = data + delta;
      ptrdiff_t dif = -(ptrdiff_t)delta;

      if (((ptrdiff_t)size + dif) & 1)
      {
        --p;  *p = (Byte)(*p - p[dif]);
      }

      while (p != lim)
      {
        --p;  *p = (Byte)(*p - p[dif]);
        --p;  *p = (Byte)(*p - p[dif]);
      }

      dif = -dif;

      do
      {
        --p;  *p = (Byte)(*p - temp[--dif]);
      }
      while (dif != 0);
    }
  }
}


void Delta_Decode(Byte *state, unsigned delta, Byte *data, SizeT size)
{
  unsigned i;
  const Byte *lim;

  if (size == 0)
    return;

  i = 0;
  lim = data + size;

  if (size <= delta)
  {
    do
      *data = (Byte)(*data + state[i++]);
    while (++data != lim);

    for (; delta != i; state++, delta--)
      *state = state[i];
    data -= i;
  }
  else
  {




































    {
      do
      {
        *data = (Byte)(*data + state[i++]);
        data++;
      }
      while (i != delta);

      {
        ptrdiff_t dif = -(ptrdiff_t)delta;
        do
          *data = (Byte)(*data + data[dif]);
        while (++data != lim);
        data += dif;
      }
    }
  }

  do
    *state++ = *data;
  while (++data != lim);
}
