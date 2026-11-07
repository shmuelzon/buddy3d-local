/* ITU-T G.711 A-law / u-law conversion */
#include "camd.h"

uint8_t linear_to_alaw(int16_t pcm)
{
	int sign = (pcm >> 8) & 0x80, exp, mant, v = pcm;
	if (sign)
		v = -v - 1; /* one's complement magnitude, avoids overflow at -32768 */
	if (v > 32635)
		v = 32635;
	if (v >= 256) {
		exp = 7;
		for (int mask = 0x4000; !(v & mask) && exp > 1; mask >>= 1)
			exp--;
		mant = (v >> (exp + 3)) & 0x0f;
	} else {
		exp = 0;
		mant = v >> 4;
	}
	return (uint8_t)((sign | (exp << 4) | mant) ^ 0xd5);
}

int16_t alaw_to_linear(uint8_t a)
{
	int t, seg;
	a ^= 0x55;
	t = (a & 0x0f) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else
		t = (t + 0x108) << (seg - 1);
	return (a & 0x80) ? t : -t;
}

int16_t ulaw_to_linear(uint8_t u)
{
	int t;
	u = ~u;
	t = ((u & 0x0f) << 3) + 0x84;
	t <<= (u & 0x70) >> 4;
	return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}
