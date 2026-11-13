/* No test stubs in the preloaded initializer: it hashes this executable,
 * opens real proc/self/mem and patches these executable instruction sites. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern int region_source0(void),region_source1(void),region_source2(void);
static int checked;
static void fail(const char *s){if(write(2,s,strlen(s))<0)_exit(92);_exit(91);}
void fixture_entry_check(void){
 int (*sites[3])(void)={region_source0,region_source1,region_source2};
 const char *expected=getenv("FIXTURE_EXPECT"),*preload=getenv("LD_PRELOAD"),*old=getenv("FIXTURE_OLD");
 static const unsigned char stock[3]={1,2,4};
 unsigned i;int keep_stock=expected&&!strcmp(expected,"stock"),value=expected?atoi(expected):0;
 if(!keep_stock&&value!=1&&value!=2&&value!=4&&value!=7)fail("bad fixture expectation\n");
 for(i=0;i<3;i++){
  const volatile unsigned char *p=(const volatile unsigned char *)((unsigned long)sites[i]&~1UL);
  int want=keep_stock?stock[i]:value;
  if(p[0]!=(unsigned char)want||p[1]!=0x23)fail("source not patched before executable entry\n");
  if(sites[i]()!=want)fail("patched instruction execution mismatch\n");
 }
 if(getenv("UB820_BD_POLICY")||getenv("UB820_BD_PRELOAD_OLD"))fail("private environment leaked\n");
 if(old&&*old){
  if(!preload||strcmp(old,preload))fail("inherited LD_PRELOAD not restored\n");
  if(!getenv("FIXTURE_OLD_CTOR"))fail("inherited preload constructor not run\n");
 }
 else if(preload)fail("LD_PRELOAD not removed\n");
 checked=1;
}
int main(void){
 static const char ok[]="bd_preload_loader_test passed: entry instructions and environment verified\n";
 if(!checked)fail("entry check not run\n");
 return write(1,ok,sizeof ok-1)==sizeof ok-1?0:92;
}
