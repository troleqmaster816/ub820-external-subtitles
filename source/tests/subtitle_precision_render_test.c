/* Controller tests use synthetic cue metadata and native-operation mocks. */
#define UB_HOST_TEST 1
#include "../modules/external_subtitles/precision_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t enabled,seek_flag,cue[2],mapped,stored,search_time[4];
static int32_t offset;
static int search_result,next_result,render_result,next_count,render_count,needs_result;
static uint32_t next_begin,next_end;
static char trace[128];static unsigned trace_size;
static void event(char c){assert(trace_size+1<sizeof trace);trace[trace_size++]=c;trace[trace_size]=0;}
static uint32_t get_state(void){event('G');return enabled;}
static int32_t sync_seconds(void){event('O');return offset;}
static uint32_t get_seek(void){event('K');return seek_flag;}
static void set_seek(uint32_t v){event('S');seek_flag=v;}
static int search(void *record,uint32_t h,uint32_t m,uint32_t s,uint32_t cc){
 assert(record==(void *)cue);event('F');search_time[0]=h;search_time[1]=m;search_time[2]=s;search_time[3]=cc;return search_result;
}
static int next(void *record){
 assert(record==(void *)cue);event('N');next_count++;
 if(!next_result){cue[0]=next_begin;cue[1]=next_end;}
 return next_result;
}
static int prepare(void){event('P');return 0;}
static int render(void){event('R');render_count++;return render_result;}
static int present(void){event('M');mapped=1;return 0;}
static int unmap(void){event('U');mapped=0;return 0;}
static void store_display(uint32_t value){event('D');stored=value;}
static int needs_update(void){event('Q');return needs_result;}
static uint32_t pack(uint32_t seconds,uint32_t cc){return ((seconds/3600)<<24)|(((seconds%3600)/60)<<16)|((seconds%60)<<8)|cc;}
static void reset(void){
 enabled=1;seek_flag=1;offset=0;search_result=0;next_result=4;render_result=0;
 mapped=0;stored=99;next_count=0;render_count=0;needs_result=1;trace_size=0;trace[0]=0;
 memset(search_time,0,sizeof search_time);
 cue[0]=pack(1027,64);cue[1]=pack(1029,59);
 ub_precision_renderer=(struct ub_precision_render_api){get_state,sync_seconds,get_seek,set_seek,search,next,prepare,render,present,unmap,store_display,cue,&mapped,needs_update};
}
static void check_trace(const char *expected){if(strcmp(trace,expected)){fprintf(stderr,"trace %s expected %s\n",trace,expected);assert(0);}}

int main(void){
 reset();assert(!ub_precision_render_update(1,1027,63));check_trace("GOKSFUD");assert(!mapped && !seek_flag && search_time[3]==63);
 reset();assert(!ub_precision_render_update(1,1027,64));check_trace("GOKSFPRMD");assert(mapped && search_time[1]==17 && search_time[2]==7 && search_time[3]==64);
 reset();assert(!ub_precision_render_update(1,1029,58));check_trace("GOKSFPRMD");
 reset();assert(!ub_precision_render_update(1,1029,59));check_trace("GOKSFNUD");assert(next_count==1 && !render_count);
 /* Both subsecond cues can be presented during the SAME integer second. */
 reset();cue[0]=pack(1027,14);cue[1]=pack(1027,59);
 assert(!ub_precision_render_update(1,1027,14));assert(render_count==1);
 next_result=0;next_begin=pack(1027,59);next_end=pack(1027,99);
 assert(!ub_precision_render_update(1,1027,59));assert(render_count==2 && next_count==1 && mapped);
 reset();offset=1;assert(!ub_precision_render_update(1,1026,64));assert(search_time[2]==7 && search_time[3]==64 && mapped);
 reset();offset=-1;assert(!ub_precision_render_update(1,1028,64));assert(search_time[2]==7 && search_time[3]==64 && mapped);
 reset();offset=-1;assert(!ub_precision_render_update(1,0,99));check_trace("GOUD");assert(seek_flag==1);
 reset();assert(!ub_precision_render_update(1,0x7fffffe2u,0));check_trace("GUD");
 reset();offset=0x7fffffff;assert(!ub_precision_render_update(1,1027,64));check_trace("GOUD");
 reset();assert(ub_precision_render_update(0,1027,64)==1);check_trace("GUD");assert(stored==0);
 reset();enabled=0;assert(!ub_precision_render_update(1,1027,64));check_trace("G");assert(stored==99);
 reset();seek_flag=0;assert(!ub_precision_render_update(1,1027,64));check_trace("GOKPRMD");
 reset();search_result=4;assert(!ub_precision_render_update(1,1027,64));check_trace("GOKSFUD");assert(stored==1);
 reset();search_result=5;assert(ub_precision_render_update(1,1027,64)==7);check_trace("GOKSF");assert(stored==99 && !seek_flag);
 reset();render_result=3;assert(ub_precision_render_update(1,1027,64)==3);check_trace("GOKSFPRU");assert(stored==99 && !mapped);
 reset();assert(ub_precision_render_update(1,1027,100)==7);check_trace("G");
 /* Advancing into a gap must clear instead of rendering a future cue. */
 reset();cue[0]=pack(1027,14);cue[1]=pack(1027,59);
 next_result=0;next_begin=pack(1028,14);next_end=pack(1028,59);
 assert(!ub_precision_render_update(1,1027,90));check_trace("GOKSFNUD");assert(!mapped && !render_count);
 /* Constant-division arithmetic must retain second/minute/hour rollovers
  * and the native byte-sized hour field, without ARM division imports. */
 const uint32_t boundaries[]={0,59,60,3599,3600,86399,86400,0x7fffffe1u};
 for(unsigned i=0;i<sizeof boundaries/sizeof boundaries[0];i++){
  uint32_t seconds=boundaries[i];reset();cue[0]=pack(seconds,0);cue[1]=pack(seconds,99);
  assert(!ub_precision_render_update(1,seconds,37));
  assert(search_time[0]==(uint8_t)(seconds/3600) && search_time[1]==(seconds%3600)/60 && search_time[2]==seconds%60 && search_time[3]==37 && mapped);
 }
 /* Fast mode omits only unchanged preparation/render/presentation. It still
  * consumes seek and stores display; expired/gap/disabled cues clear first. */
 reset();mapped=1;needs_result=0;
 assert(!ub_precision_render_update_fast(1,1027,64));check_trace("GOKSFQD");assert(mapped && !render_count && stored==1 && !seek_flag);
 reset();mapped=1;needs_result=1;
 assert(!ub_precision_render_update_fast(1,1027,64));check_trace("GOKSFQPRMD");assert(render_count==1);
 reset();mapped=0;needs_result=0;
 assert(!ub_precision_render_update_fast(1,1027,64));check_trace("GOKSFPRMD");assert(mapped && render_count==1);
 reset();mapped=1;needs_result=0;
 assert(!ub_precision_render_update_fast(1,1029,59));check_trace("GOKSFNUD");assert(!mapped && next_count==1);
 reset();mapped=1;needs_result=0;
 assert(!ub_precision_render_update_fast(1,1027,63));check_trace("GOKSFUD");assert(!mapped);
 reset();mapped=1;needs_result=0;
 assert(ub_precision_render_update_fast(0,1027,64)==1);check_trace("GUD");assert(!mapped && stored==0);
 reset();mapped=1;needs_result=1;render_result=3;
 assert(ub_precision_render_update_fast(1,1027,64)==3);check_trace("GOKSFQPRU");assert(!mapped && stored==99);
 reset();mapped=1;needs_result=0;ub_precision_renderer.needs_update=0;
 assert(!ub_precision_render_update_fast(1,1027,64));check_trace("GOKSFPRMD");assert(render_count==1);
 puts("subtitle_precision_render_test passed: subsecond begin/end, adjacent cues, signed sync, seek, clear, errors and native state order");
 return 0;
}
