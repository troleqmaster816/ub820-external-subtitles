/* Pure policy shared by the helper and the pre-main region initializer. */
#ifndef UB_BD_POLICY_H
#define UB_BD_POLICY_H
#include <stdint.h>
static uint32_t ub_bd_be32(const unsigned char *b){return (uint32_t)b[0]<<24|(uint32_t)b[1]<<16|(uint32_t)b[2]<<8|b[3];}
static uint16_t ub_bd_be16(const unsigned char *b){return (uint16_t)(b[0]<<8|b[1]);}
static uint32_t ub_bd_region_for(char mode,unsigned value,uint32_t player){
 if(mode=='F'&&(value==1||value==2||value==4||value==7))return value;
 if(mode=='M'&&(value&7)){
  if(value&player&7)return player;
  return value&1?1:value&2?2:4;
 }
 return player;
}
/* Infer the masks compared against a PSR20 value, retaining the helper's
 * existing experimental HDMV heuristic. No BD-J analysis is attempted. */
static unsigned ub_bd_hdmv_mask(const unsigned char *c,unsigned cmds){
 uint32_t tracked[8];unsigned count=0,mask=0,i,k;
 for(i=0;i<cmds;i++,c+=12){
  unsigned grp=(c[0]>>3)&3,sub=c[0]&7,imm1=c[1]>>7,imm2=(c[1]>>6)&1,compare=c[2]&15,setop=c[3]&31;
  uint32_t d=ub_bd_be32(c+4),s=ub_bd_be32(c+8);int dt=0,st=0;
  for(k=0;k<count;k++){if(!imm1&&d==tracked[k])dt=1;if(!imm2&&s==tracked[k])st=1;}
  if(!imm1&&d==0x80000014u)dt=1;
  if(!imm2&&s==0x80000014u)st=1;
  if(grp==2&&sub==0&&setop==1&&!imm1&&!(d&0x80000000u)){
   for(k=0;k<count;k++)if(tracked[k]==d)tracked[k]=tracked[--count];
   if(st&&count<8)tracked[count++]=d;
   continue;
  }
  if((grp==1&&compare>=1&&compare<=3)||(grp==2&&sub==0&&setop==9)){
   if(dt&&imm2&&s>=1&&s<=7)mask|=s;
   if(st&&imm1&&d>=1&&d<=7)mask|=d;
  }
 }
 return mask;
}
#endif
