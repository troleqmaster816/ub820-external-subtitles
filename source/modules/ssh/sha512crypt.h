/* Self-contained SHA-512 and $6$ crypt (SHA-512-crypt), so the statically
 * linked helper can hash the SSH password without a crypt() from libc. The
 * output matches glibc / musl / openssl $6$, which is what dropbear verifies
 * (see tests/ssh_test.c, checked against Python's crypt). Algorithm:
 * Ulrich Drepper's SHA-crypt specification. */
#ifndef UB_SHA512CRYPT_H
#define UB_SHA512CRYPT_H
#include <stdint.h>
#include <string.h>

/* --- SHA-512 --- */
struct ub_sha512 { uint64_t h[8]; uint64_t len; unsigned char buf[128]; unsigned used; };
static uint64_t ub_ror64(uint64_t x,unsigned n){return (x>>n)|(x<<(64-n));}
static void ub_sha512_block(struct ub_sha512 *s,const unsigned char *p){
 static const uint64_t K[80]={
  0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
  0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
  0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
  0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
 uint64_t w[80],a,b,c,d,e,f,g,h,t1,t2;unsigned i;
 for(i=0;i<16;i++)w[i]=((uint64_t)p[i*8]<<56)|((uint64_t)p[i*8+1]<<48)|((uint64_t)p[i*8+2]<<40)|((uint64_t)p[i*8+3]<<32)|((uint64_t)p[i*8+4]<<24)|((uint64_t)p[i*8+5]<<16)|((uint64_t)p[i*8+6]<<8)|(uint64_t)p[i*8+7];
 for(i=16;i<80;i++){uint64_t s0=ub_ror64(w[i-15],1)^ub_ror64(w[i-15],8)^(w[i-15]>>7),s1=ub_ror64(w[i-2],19)^ub_ror64(w[i-2],61)^(w[i-2]>>6);w[i]=w[i-16]+s0+w[i-7]+s1;}
 a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
 for(i=0;i<80;i++){t1=h+(ub_ror64(e,14)^ub_ror64(e,18)^ub_ror64(e,41))+((e&f)^((~e)&g))+K[i]+w[i];t2=(ub_ror64(a,28)^ub_ror64(a,34)^ub_ror64(a,39))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
 s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void ub_sha512_init(struct ub_sha512 *s){
 static const uint64_t iv[8]={0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
 memcpy(s->h,iv,sizeof iv);s->len=0;s->used=0;
}
static void ub_sha512_update(struct ub_sha512 *s,const void *v,unsigned n){
 const unsigned char *p=v;s->len+=n;
 while(n){unsigned t=128-s->used;if(t>n)t=n;memcpy(s->buf+s->used,p,t);s->used+=t;p+=t;n-=t;if(s->used==128){ub_sha512_block(s,s->buf);s->used=0;}}
}
static void ub_sha512_final(struct ub_sha512 *s,unsigned char out[64]){
 uint64_t bits=s->len*8;unsigned char pad=0x80;unsigned i;
 ub_sha512_update(s,&pad,1);pad=0;
 while(s->used!=112)ub_sha512_update(s,&pad,1);
 for(i=0;i<8;i++)s->buf[112+i]=0;
 for(i=0;i<8;i++)s->buf[120+i]=(unsigned char)(bits>>(56-8*i));
 ub_sha512_block(s,s->buf);
 for(i=0;i<8;i++){out[i*8]=(unsigned char)(s->h[i]>>56);out[i*8+1]=(unsigned char)(s->h[i]>>48);out[i*8+2]=(unsigned char)(s->h[i]>>40);out[i*8+3]=(unsigned char)(s->h[i]>>32);out[i*8+4]=(unsigned char)(s->h[i]>>24);out[i*8+5]=(unsigned char)(s->h[i]>>16);out[i*8+6]=(unsigned char)(s->h[i]>>8);out[i*8+7]=(unsigned char)s->h[i];}
}

/* --- $6$ crypt --- */
static void ub_b64_from_24(unsigned char **cp,unsigned b2,unsigned b1,unsigned b0,int n){
 static const char itoa64[]="./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
 unsigned w=(b2<<16)|(b1<<8)|b0;
 while(n-->0){*(*cp)++=itoa64[w&0x3f];w>>=6;}
}
/* Writes "$6$<salt>$<43-char hash>" into out (>=100 bytes). salt up to 16 chars. */
static void ub_sha512_crypt(const char *pw,const char *salt,char *out){
 unsigned plen=(unsigned)strlen(pw),slen=(unsigned)strlen(salt);unsigned i,cnt;
 unsigned char A[64],B[64],DP[64],DS[64];struct ub_sha512 a,b;char *o;
 if(slen>16)slen=16;
 /* digest B = SHA512(pw + salt + pw) */
 ub_sha512_init(&b);ub_sha512_update(&b,pw,plen);ub_sha512_update(&b,salt,slen);ub_sha512_update(&b,pw,plen);ub_sha512_final(&b,B);
 /* digest A */
 ub_sha512_init(&a);ub_sha512_update(&a,pw,plen);ub_sha512_update(&a,salt,slen);
 for(cnt=plen;cnt>64;cnt-=64)ub_sha512_update(&a,B,64);
 ub_sha512_update(&a,B,cnt);
 for(cnt=plen;cnt>0;cnt>>=1){if(cnt&1)ub_sha512_update(&a,B,64);else ub_sha512_update(&a,pw,plen);}
 ub_sha512_final(&a,A);
 /* DP = SHA512(pw repeated plen times) */
 ub_sha512_init(&b);
 for(i=0;i<plen;i++)ub_sha512_update(&b,pw,plen);
 ub_sha512_final(&b,DP);
 /* DS = SHA512(salt repeated 16+A[0] times) */
 ub_sha512_init(&b);
 for(i=0;i<(unsigned)(16+A[0]);i++)ub_sha512_update(&b,salt,slen);
 ub_sha512_final(&b,DS);
 /* 5000 rounds over sequences P (pw) and S (salt) built from DP/DS */
 {
  unsigned char C[64];memcpy(C,A,64);
  static unsigned char P[256],S[256];unsigned Pl=0,Sl=0;
  for(cnt=plen;cnt>=64;cnt-=64){memcpy(P+Pl,DP,64);Pl+=64;}
  if(cnt){memcpy(P+Pl,DP,cnt);Pl+=cnt;}
  for(cnt=slen;cnt>=64;cnt-=64){memcpy(S+Sl,DS,64);Sl+=64;}
  if(cnt){memcpy(S+Sl,DS,cnt);Sl+=cnt;}
  for(cnt=0;cnt<5000;cnt++){
   ub_sha512_init(&b);
   if(cnt&1)ub_sha512_update(&b,P,Pl);else ub_sha512_update(&b,C,64);
   if(cnt%3)ub_sha512_update(&b,S,Sl);
   if(cnt%7)ub_sha512_update(&b,P,Pl);
   if(cnt&1)ub_sha512_update(&b,C,64);else ub_sha512_update(&b,P,Pl);
   ub_sha512_final(&b,C);
  }
  memcpy(A,C,64);
 }
 /* format output */
 o=out;*o++='$';*o++='6';*o++='$';memcpy(o,salt,slen);o+=slen;*o++='$';
 {
  unsigned char *c=A;unsigned char *cp=(unsigned char *)o;
  ub_b64_from_24(&cp,c[0],c[21],c[42],4);ub_b64_from_24(&cp,c[22],c[43],c[1],4);
  ub_b64_from_24(&cp,c[44],c[2],c[23],4);ub_b64_from_24(&cp,c[3],c[24],c[45],4);
  ub_b64_from_24(&cp,c[25],c[46],c[4],4);ub_b64_from_24(&cp,c[47],c[5],c[26],4);
  ub_b64_from_24(&cp,c[6],c[27],c[48],4);ub_b64_from_24(&cp,c[28],c[49],c[7],4);
  ub_b64_from_24(&cp,c[50],c[8],c[29],4);ub_b64_from_24(&cp,c[9],c[30],c[51],4);
  ub_b64_from_24(&cp,c[31],c[52],c[10],4);ub_b64_from_24(&cp,c[53],c[11],c[32],4);
  ub_b64_from_24(&cp,c[12],c[33],c[54],4);ub_b64_from_24(&cp,c[34],c[55],c[13],4);
  ub_b64_from_24(&cp,c[56],c[14],c[35],4);ub_b64_from_24(&cp,c[15],c[36],c[57],4);
  ub_b64_from_24(&cp,c[37],c[58],c[16],4);ub_b64_from_24(&cp,c[59],c[17],c[38],4);
  ub_b64_from_24(&cp,c[18],c[39],c[60],4);ub_b64_from_24(&cp,c[40],c[61],c[19],4);
  ub_b64_from_24(&cp,c[62],c[20],c[41],4);ub_b64_from_24(&cp,0,0,c[63],2);
  *cp=0;
 }
}
#endif
