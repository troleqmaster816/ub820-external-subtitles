/* Style submits one verified multi-site transaction, never a live byte write. */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#define UB_STYLE_PROC_ROOT "/tmp/ub820-subtitle-style-test"
#include <unistd.h>
static ssize_t chunked_read(int,void *,size_t);
#define read chunked_read
#define main unused_engage_main
#define style_patch legacy_style_patch
#define font_size_set legacy_font_size_set
#define playdat_set_on legacy_playdat_set_on
#include "../modules/external_subtitles/engage_core.c"
#undef read
#undef main
#undef style_patch
#undef font_size_set
#undef playdat_set_on
#include "ub.h"
#include "../modules/external_subtitles/style.h"
#include <assert.h>
#include <sys/stat.h>

static int known_library=1,transactions,hashes;
static int short_reads,interrupt_read,read_calls;
static ssize_t chunked_read(int fd,void *buf,size_t size){
 read_calls++;
 if(interrupt_read){interrupt_read=0;errno=EINTR;return -1;}
 return read(fd,buf,short_reads && size>64?64:size);
}
static uint32_t seen_count;
static unsigned char seen_rows[16];
int ub_hash_file(const char *path,char result[65]){
 assert(!strcmp(path,"/known/libtiwin.so.0.0.0"));hashes++;
 strcpy(result,known_library?"6d2216fcd15814defdfc2357ddb5b517bf85398d26d1f530d61bed22087994bf":
                            "0000000000000000000000000000000000000000000000000000000000000000");
 return 0;
}
int ub_patch_code(int pid,const struct ub_code_patch *patches,unsigned count){
 assert(pid==42);transactions++;seen_count=count;assert(count==6);
 assert(patches[0].address==STYLE_FONT_ADDR && patches[0].length==22);
 assert(!memcmp(patches[0].expected,style_font_stock,22));
 assert(patches[4].address==0x293b5462 && patches[4].length==2);
 assert(!memcmp(patches[4].expected,"\x00\x20",2));
 assert(!memcmp(patches[4].replacement,"\x04\x20",2));
 assert(patches[5].address==UB_DISC_ROWS_ADDR && patches[5].length==16);
 assert(patches[5].expected==disc_rows_stock && patches[5].alternate==disc_rows_aligned);
 memcpy(seen_rows,patches[5].replacement,sizeof seen_rows);return 0;
}
void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;(void)format;}
static void maps(const char *text){
 FILE *file=fopen(UB_STYLE_PROC_ROOT "/42/maps","w");assert(file);assert(fputs(text,file)>=0);assert(!fclose(file));
}
int main(void){
 mkdir(UB_STYLE_PROC_ROOT,0700);mkdir(UB_STYLE_PROC_ROOT "/42",0700);
 maps("293a0000-293d8000 r-xp 00000000 00:01 1 /known/libtiwin.so.0.0.0\n");
 assert(!style_patch(42));assert(transactions==1 && seen_count==6 && hashes==1);
 assert(!memcmp(seen_rows,disc_rows_aligned,16));
 disc_style.bottom_align=0;assert(!style_patch(42));assert(transactions==2);
 assert(!memcmp(seen_rows,disc_rows_stock,16));
 known_library=0;assert(style_patch(42)<0);assert(transactions==2);
 known_library=1;maps("");assert(style_patch(42)<0);assert(transactions==2);
 maps("293a0000-293d8000 r-xp 00001000 00:01 1 /known/libtiwin.so.0.0.0\n");
 assert(style_patch(42)<0);assert(transactions==2);
 /* Kernel-style short reads and EINTR before the library beyond page one. */
 FILE *file=fopen(UB_STYLE_PROC_ROOT "/42/maps","w");assert(file);
 for(int i=0;i<100;i++)assert(fputs("28000000-28010000 r-xp 00000000 00:01 2 /known/other.so\n",file)>=0);
 assert(fputs("293a0000-293d8000 r-xp 00000000 00:01 1 /known/libtiwin.so.0.0.0\n",file)>=0);assert(!fclose(file));
 short_reads=interrupt_read=1;read_calls=0;
 assert(!style_patch(42));assert(transactions==3 && read_calls>64 && !interrupt_read);
 /* Never accept a partial maps snapshot, even if it starts with the library. */
 file=fopen(UB_STYLE_PROC_ROOT "/42/maps","w");assert(file);
 assert(fputs("293a0000-293d8000 r-xp 00000000 00:01 1 /known/libtiwin.so.0.0.0\n",file)>=0);
 for(int i=0;i<2000;i++)assert(fputs("28000000-28010000 r-xp 00000000 00:01 2 /known/other.so\n",file)>=0);
 assert(!fclose(file));assert(style_patch(42)==-3);assert(transactions==3);
 unlink(UB_STYLE_PROC_ROOT "/42/maps");rmdir(UB_STYLE_PROC_ROOT "/42");rmdir(UB_STYLE_PROC_ROOT);
 puts("subtitle_style_test passed: six-site transaction; hash/base rejection; short reads/EINTR; truncation rejected");
 return 0;
}
