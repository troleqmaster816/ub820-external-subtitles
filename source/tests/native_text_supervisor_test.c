#define _GNU_SOURCE
#include "ub.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "../modules/external_subtitles/english_text.h"

const struct ub_payload ub_payload_external_subtitles_native_text={0};
static int settled=1,missing,bad_hash,bad_base,version185,installs,install_result,changes;
int ub_settled(const char *name){assert(!strcmp(name,"apl_dispsrv"));return settled;}
void ub_changed(struct ub_part *part){(void)part;changes++;}
void ub_log(const char *module,int level,const char *format,...){(void)module;(void)level;(void)format;}
const char *ub_install_stage(void){return "test";}
int ub_install_payload(int pid,const struct ub_payload *payload){assert(pid==42 && payload==&ub_payload_external_subtitles_native_text);installs++;return install_result;}
uint32_t ub_map_base(int pid,const char *suffix,char *path,size_t size){
 assert(pid==42);snprintf(path,size,"/known%s",suffix);
 if(missing)return 0;
 if(!strcmp(suffix,"/libctk.so"))return bad_base?0x10000000:0x29ad0000;
 if(!strcmp(suffix,"/libmulang.so"))return 0x29450000;
 assert(!strcmp(suffix,LANG_SUFFIX));return 0x41000000;
}
int ub_hash_file(const char *path,char result[65]){
 const char *hash;
 if(bad_hash)hash="0000000000000000000000000000000000000000000000000000000000000000";
 else if(strstr(path,"libctk.so"))hash=version185?"1ac31bbdde463ea755684d5f4995040ffa2e496accc3a6f6841b6005a34f25e5":"f4b0d75f37f49880c45a9bdd1be113d172c922df58d8b2e3c437abfc214fe991";
 else if(strstr(path,"libmulang.so"))hash=version185?"9856df99ec704146ecdd6019c8166b15c8a6b9550cf67624f429a56a363b1bb4":"dddd5af0fda3f8accde5a4006fe0160f9aa2292a2ae8fd973849af8071a8d8b5";
 else hash=version185?"f83f9367d53cfea7dda8cd1d0536c9fb1699466138c20499d0e3be06c102e44e":LANG_SHA256;
 strcpy(result,hash);return 0;
}
static struct ub_target target(void){struct ub_target t={0};t.pid=42;return t;}
int main(void){
 struct ub_part part={0};struct ub_target t=target();
 settled=0;english_text_step(&part,&t);assert(!installs && !t.state[1]);settled=1;
 missing=1;for(int i=0;i<121;i++)english_text_step(&part,&t);assert(t.state[0]==-1 && !installs);missing=0;
 t=target();bad_hash=1;english_text_step(&part,&t);assert(t.state[0]==-1 && !installs);bad_hash=0;
 t=target();bad_base=1;english_text_step(&part,&t);assert(t.state[0]==-1 && !installs);bad_base=0;
 for(version185=0;version185<=1;version185++){
  t=target();english_text_step(&part,&t);assert(t.installed && t.state[0]==1);int before=installs;
  english_text_step(&part,&t);assert(installs==before);
 }
 assert(changes==2 && installs==2);
 t=target();install_result=-1;for(int i=0;i<40;i++)english_text_step(&part,&t);
 assert(t.failed && t.state[0]==-1 && t.state[2]==LANG_ATTEMPTS && installs==14);
 t=target();install_result=-2;english_text_step(&part,&t);assert(t.failed && t.state[0]==-1 && installs==15);
 puts("native_text_supervisor_test passed: exact dependency pins/bases; both versions; bounded retry; quarantine propagation");
 return 0;
}
