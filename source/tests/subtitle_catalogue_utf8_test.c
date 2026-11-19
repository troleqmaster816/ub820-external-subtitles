/* Real catalogue scan: a name longer than the 128-byte item label is cut on a
 * UTF-8 character boundary (the native CTK drops a label with a split tail). */
#define _GNU_SOURCE
#define UB_HOST_TEST 1
#include "../modules/external_subtitles/backend.c"
#include <assert.h>
#include <string.h>

static const char *names[8];static unsigned next_name;
int UFS_OpenDir(const char *path,uint32_t *handle){(void)path;*handle=7;next_name=0;return 0;}
int UFS_ReadDir(uint32_t handle,void *record,uint32_t *count){
 unsigned char *r=record;(void)handle;
 if(!names[next_name]){*count=0;return 0;}
 r[10]=8;strcpy((char *)r+11,names[next_name++]);*count=1;return 0;
}
int UFS_CloseDir(uint32_t handle){(void)handle;return 0;}
static void repeat(char *out,const char *prefix,const char *unit,unsigned count){
 strcpy(out,prefix);while(count--)strcat(out,unit);strcat(out,".srt");
}
static int valid_utf8(const char *s){
 const unsigned char *p=(const unsigned char *)s;
 while(*p){
  unsigned need=*p<0x80?0:*p>=0xf0?3:*p>=0xe0?2:*p>=0xc0?1:9;if(need==9)return 0;p++;
  while(need--){if((*p&0xc0)!=0x80)return 0;p++;}
 }
 return 1;
}
static const struct ub_item *find(const struct ub_catalogue *list,const char *full){
 for(uint32_t i=0;i<list->count;i++)if(!strcmp(list->items[i].path,full))return &list->items[i];
 return 0;
}
int main(void){
 static struct ub_catalogue list;char two[240],three[240],whole[240],split[240],ascii[240],invalid[240],full[300];
 const char *all[6]={two,three,whole,split,ascii,invalid};const size_t expected[6]={126,125,127,125,127,127};unsigned cases=0;
 repeat(two,"ab","\xc4\x85",70);             /* byte 126 is the first half of a 2-byte character */
 repeat(three,"ab","\xe2\x82\xac",50);       /* bytes 125..126 are 2/3 of a 3-byte character */
 repeat(whole,"abc","\xf0\x9f\x98\x80",35);  /* bytes 123..126 are a whole 4-byte character */
 repeat(split,"a","\xf0\x9f\x98\x80",35);    /* bytes 125..126 are 2/4 of a 4-byte character */
 repeat(ascii,"","x",140);
 strcpy(invalid,"zz");for(int i=0;i<130;i++)strcat(invalid,"\x85");strcat(invalid,".srt"); /* not UTF-8: untouched */
 for(int i=0;i<6;i++){names[i]=all[i];}
 names[6]=0;
 assert(scan_catalogue("/mnt/usb/sub",&list)==UB_OK&&list.count==7);
 for(int i=0;i<6;i++){
  const struct ub_item *item;snprintf(full,sizeof full,"/mnt/usb/sub/%s",all[i]);item=find(&list,full);
  assert(item&&item->kind==UB_KIND_FILE);                 /* full path is kept for selection */
  size_t n=strlen(item->name);assert(n<UB_NAME_BYTES&&!strncmp(item->name,all[i],n));
  if(i<5)assert(valid_utf8(item->name));
  assert(n==expected[i]);cases++;
 }
 /* Short names are copied unchanged. */
 names[0]="Movie \xc4\x85\xe2\x82\xac.srt";names[1]=0;
 assert(scan_catalogue("/mnt/usb/sub",&list)==UB_OK&&list.count==2);
 assert(find(&list,"/mnt/usb/sub/Movie \xc4\x85\xe2\x82\xac.srt")&&
        !strcmp(find(&list,"/mnt/usb/sub/Movie \xc4\x85\xe2\x82\xac.srt")->name,"Movie \xc4\x85\xe2\x82\xac.srt"));cases++;
 printf("subtitle_catalogue_utf8_test passed: %u cases; labels cut on UTF-8 boundaries, paths intact\n",cases);
 return 0;
}
