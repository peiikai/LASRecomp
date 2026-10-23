// The FMA emulation (simd.h, built without -mfma) against the FMA instruction itself, bit for bit: 4 rounding modes,
// MXCSR FTZ/DAZ off and on, denormals, double-rounding midpoints. Needs a CPU with FMA to run.
// Build: clang -O1 -march=x86-64-v2 -mavx -mf16c -Iruntime tools/test_fma_hw.c -o build/test_fma_hw -lm
#include <stdio.h>
#include <xmmintrin.h>
#include "cpu.h"
static uint64_t rs=88172645463325252ull; static uint64_t rnd(void){rs^=rs<<13;rs^=rs>>7;rs^=rs<<17;return rs;}
static float fb(uint32_t u){float f;memcpy(&f,&u,4);return f;} static uint32_t bf(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static float rf(void){switch(rnd()%4){case 0:return fb((uint32_t)rnd());case 1:return fb((uint32_t)rnd()&0x80ffffff);case 2:return fb(((uint32_t)rnd()&0x807fffff)|(uint32_t)(rnd()%40+50)<<23);default:return fb(((uint32_t)rnd()&0x807fffff)|(uint32_t)(rnd()%30+110)<<23);}}
static float hw(float a,float b,float c){ __asm__("vfmadd231ss %2,%1,%0":"+x"(c):"x"(a),"x"(b)); return c; }
int main(void){ long bad=0,n=0;
 for(int mode=0;mode<4;mode++) for(int fz=0;fz<2;fz++){ _mm_setcsr(0x1f80 | mode<<13 | (fz?0x8040:0));
  for(long i=0;i<3000000;i++){ float a=rf(),b=rf(),c; if(i&1){double p=(double)a*b; float h=(float)p; c=(float)-(p-(double)h); if(rnd()&1)c=fb(bf(c)^1);} else c=rf();
   float w=hw(a,b,c), g=simd_fmaf(a,b,c); vf32 gv=simd_vfmaf((vf32){a,b,c,a},(vf32){b,a,a,c},(vf32){c,c,b,b});
   float wv[4]={hw(a,b,c),hw(b,a,c),hw(c,a,b),hw(a,c,b)}; int e=!(bf(w)==bf(g)||(w!=w&&g!=g)); for(int k=0;k<4;k++) e|=!(bf(wv[k])==bf(gv[k])||(wv[k]!=wv[k]&&gv[k]!=gv[k]));
   n++; if(e&&bad++<5) printf("mode %d fz %d: %a %a %a hw %a emu %a\n",mode,fz,a,b,c,w,g);} }
 _mm_setcsr(0x1f80); printf("%ld cases vs the FMA instruction (4 rounding modes, FTZ/DAZ off and on): %ld differ\n",n,bad); return bad!=0; }
