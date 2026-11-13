#ifndef UB_POKE_H
#define UB_POKE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

typedef int (*ub_poke_read_fn)(int,uint32_t,void *,size_t);
typedef int (*ub_poke_write_fn)(int,uint32_t,const void *,size_t);

/* Diagnostic byte-width write/readback only; this is not an atomic patch
 * transaction. Explicit callbacks allow the actual comparison/error paths to
 * be tested without accessing any process memory or device. */
static inline int ub_poke_value(int pid,uint32_t address,uint32_t value,
 uint32_t width,ub_poke_write_fn write_mem,ub_poke_read_fn read_mem,
 FILE *out,FILE *err){
 unsigned char expected[4],actual[4]={0};uint32_t readback=0;int equal;
 if(width<1 || width>sizeof expected || (uint64_t)address+width>UINT64_C(0x100000000)){
  fprintf(err,"poke requires 1..4 bytes within the 32-bit address range\n");return 1;
 }
 memcpy(expected,&value,sizeof expected);
 if(write_mem(pid,address,expected,width)){
  fprintf(err,"poke PID%d @%08x <- %08x failed\n",pid,address,value);return 1;
 }
 if(read_mem(pid,address,actual,width)){fprintf(err,"readback failed\n");return 1;}
 memcpy(&readback,actual,sizeof readback);
 equal=memcmp(expected,actual,width)==0;
 fprintf(out,"PID%d @%08x: wrote %08x, readback %08x %s\n",pid,address,value,readback,equal?"OK":"MISMATCH");
 return equal?0:1;
}

/* Reject truncation, signs, whitespace and partially parsed arguments before
 * any memory callback. PID is decimal; addresses/values keep CLI base-0 syntax. */
static inline int ub_poke_number(const char *text,int base,uint32_t limit,uint32_t *value){
 char *end;unsigned long long parsed;
 if(!text || text[0]<'0' || text[0]>'9')return -1;
 errno=0;parsed=strtoull(text,&end,base);
 if(errno==ERANGE || *end || parsed>limit)return -1;
 *value=(uint32_t)parsed;return 0;
}
static inline int ub_poke_command(int argc,char **argv,
 ub_poke_write_fn write_mem,ub_poke_read_fn read_mem,FILE *out,FILE *err){
 uint32_t pid,address,value,width=4;
 if((argc!=5 && argc!=6) || ub_poke_number(argv[2],10,INT_MAX,&pid) || !pid ||
    ub_poke_number(argv[3],0,UINT32_MAX,&address) ||
    ub_poke_number(argv[4],0,UINT32_MAX,&value) ||
    (argc==6 && ub_poke_number(argv[5],0,4,&width)) || !width){
  fprintf(err,"usage: --poke PID ADDRESS VALUE [1..4] (32-bit address/value)\n");return 1;
 }
 return ub_poke_value((int)pid,address,value,width,write_mem,read_mem,out,err);
}
#endif
