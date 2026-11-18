/* long runs: every block x type at extreme knobs, 3 s pluck + 8 s silence: runaway / no-decay / NaN.
 * ARGS: only the models so named (e.g. the Bluetooth image's own: build/long "LOW OCTAVE" PAD) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "px_dsp.h"
static px_engine e;
static float sig[44100*3];
static void mksig(void){ int pos=0; float f0s[3]={82.4f,110.0f,196.0f}; unsigned s=7;
  for(int n=0;n<3;n++){ int N=(int)(44100.0f/f0s[n]); float buf[600]; for(int i=0;i<N;i++){ s=s*1103515245u+12345u; buf[i]=((s>>8)&65535)/32768.0f-1.0f; }
    for(int i=0;i<44100;i++){ sig[pos++]=0.35f*buf[i%N]; int j=i%N; buf[j]=0.996f*0.5f*(buf[j]+buf[(j+1)%N]); } } }
static int named(int argc, char **argv, const char *nm){ if(argc<2) return 1; for(int i=1;i<argc;i++) if(!strcmp(argv[i],nm)) return 1; return 0; }
int main(int argc, char **argv){ mksig(); int bad=0, n=0; static const int V[3]={0,50,100};
  for(int blk=0;blk<PX_NBLK;blk++) for(int t=0;t<px_ntypes(blk);t++) for(int a=0;a<3;a++){
    if(!named(argc,argv,px_type_name(blk,t))) continue;
    px_preset_t p; px_preset_factory(&p,7); for(int z=0;z<PX_NBLK;z++) p.on[z]=(z==PX_B_AMP||z==PX_B_CAB||z==blk);
    p.type[blk]=t; for(int k=0;k<5;k++) p.p[blk][k]=(a==2)?100:V[(a+k)%3];
    px_engine_init(&e,44100.0f); px_engine_set(&e,&p); n++;
    float out[64], in[64]={0}; double pk=0, last=0; long cnt=0; int nan=0;
    for(int b=0;b<44100*11/64;b++){ const float*src = (b*64 + 64 <= 44100*3)? sig+b*64 : in;
      px_engine_process(&e,src,out,64);
      for(int i=0;i<64;i++){ float v=out[i]; if(v!=v) nan=1; if(fabs(v)>pk) pk=fabs(v); if(b*64>=44100*10){ last+=v*v; cnt++; } } }
    last=sqrt(last/cnt);
    int lim_ok = !(blk==PX_B_REVERB && (t==PX_REV_AMBIENT||t==PX_REV_CATHEDRAL||t==PX_REV_SHIMMER));
    if(nan || (lim_ok && last>0.02) || last>0.2){ bad++; printf("%-12s %-14s set%d  peak %.2f  tail(10-11s) %.4f %s\n",px_block_name(blk),px_type_name(blk,t),a,pk,last,nan?"NaN":""); }
  }
  printf("%d runs, %d flagged\n",n,bad); }
