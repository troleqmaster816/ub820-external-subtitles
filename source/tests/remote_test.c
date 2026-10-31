/* Host test of the remote module: the markers the apl_mgr payload checks,
 * the overlay's key FIFO, the menu's request and status files, and the
 * choice saved to ub820_menu.cfg. */
#include "../modules/remote/module.c"
#include <stdlib.h>

static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)
struct ub_module *const ub_modules[]={&ub_module_remote,0};
/* config.c saves menu choices; on the host the stick is not involved */
const char *ub_stick_error(int code){(void)code;return "test";}
const struct ub_payload ub_payload_remote_keys;

static void read_text(const char *path,char *buf,size_t cap){FILE *f=fopen(path,"r");size_t n=f?fread(buf,1,cap-1,f):0;if(f)fclose(f);buf[n]=0;}
static void menu_set(const char *text){FILE *f=fopen(UB_REMOTE_REQUEST,"w");fputs(text,f);fclose(f);}
static void touch(const char *path){FILE *f=fopen(path,"w");fclose(f);}
static int present(const char *path){return access(path,F_OK)==0;}

int main(void){
 char buf[512];struct ub_stat a,b;
 unlink(UB_REMOTE_STATUS);unlink(UB_REMOTE_REQUEST);unlink(UB_NETFLIX_OFF);unlink(UB_NETFLIX_HELPER);unlink(UB_KEYS_FIFO);unlink(UB_CONFIG_DIR "/ub820_menu.cfg");
 /* default: the button keeps opening Netflix; the key FIFO is made */
 netflix_key=NETFLIX_STOCK;start(&ub_module_remote);
 CHECK(!keys_ok);menu_step(0,0);CHECK(!present(UB_REMOTE_STATUS)); /* no apl_mgr on the host: no menu row */
 keys_ok=1;menu_step(0,0);
 CHECK(!present(UB_NETFLIX_OFF));CHECK(!present(UB_NETFLIX_HELPER));
 read_text(UB_REMOTE_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"0 netflix \n"));
 CHECK(!ub_stat(UB_KEYS_FIFO,&a)&&S_ISFIFO(a.mode));
 /* stale markers from an earlier run are removed at start; the FIFO is kept
  * (the overlay in apl_dispsrv may hold it open) */
 touch(UB_NETFLIX_OFF);touch(UB_OVERLAY_OPEN);
 start(&ub_module_remote);keys_ok=1;
 CHECK(!present(UB_NETFLIX_OFF));CHECK(!present(UB_OVERLAY_OPEN));
 CHECK(!ub_stat(UB_KEYS_FIFO,&b)&&a.ino==b.ino);
 /* the menu switches it off: marker at once, saved, answered */
 menu_set("set nothing 3\n");menu_step(0,0);
 CHECK(netflix_key==NETFLIX_NOTHING);CHECK(present(UB_NETFLIX_OFF));CHECK(!present(UB_REMOTE_REQUEST));
 read_text(UB_REMOTE_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"3 nothing saved\n"));
 read_text(UB_CONFIG_DIR "/ub820_menu.cfg",buf,sizeof buf);CHECK(strstr(buf,"[remote]\nnetflix_key=nothing\n"));
 /* an unknown choice changes nothing */
 menu_set("set mute 4\n");menu_step(0,0);
 CHECK(netflix_key==NETFLIX_NOTHING);CHECK(present(UB_NETFLIX_OFF));
 read_text(UB_REMOTE_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"4 nothing unknown choice mute\n"));
 /* the overlay: its marker instead of "nothing"'s */
 menu_set("set helper 6\n");menu_step(0,0);
 CHECK(netflix_key==NETFLIX_HELPER);CHECK(!present(UB_NETFLIX_OFF));CHECK(present(UB_NETFLIX_HELPER));
 read_text(UB_REMOTE_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"6 helper saved\n"));
 /* back to Netflix */
 menu_set("set netflix 5\n");menu_step(0,0);
 CHECK(netflix_key==NETFLIX_STOCK);CHECK(!present(UB_NETFLIX_OFF));CHECK(!present(UB_NETFLIX_HELPER));
 read_text(UB_REMOTE_STATUS,buf,sizeof buf);CHECK(!strcmp(buf,"5 netflix saved\n"));
 /* read back at start: the menu file wins over modules.cfg */
 {FILE *f=fopen(UB_CONFIG_DIR "/modules.cfg","w");fputs("[remote]\nnetflix_key=helper\n",f);fclose(f);}
 netflix_key=NETFLIX_NOTHING;ub_config_load();CHECK(netflix_key==NETFLIX_STOCK);
 unlink(UB_NETFLIX_OFF);unlink(UB_NETFLIX_HELPER);unlink(UB_KEYS_FIFO);
 /* firmware: 1.82 exact, the 1.85 builds with the same code, nothing else
  * (apl_mgr changed in 1.85, so its 1.85 hash is not accepted) */
 CHECK(ub_firmware_match("a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3","a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3")==182);
 CHECK(ub_firmware_match("fefee7cc5b3a1e4d1ebed3248cb554c9ff434f1667d5e571d12b030aafe5d89e","a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3")==185);
 CHECK(ub_firmware_match("fefee7cc5b3a1e4d1ebed3248cb554c9ff434f1667d5e571d12b030aafe5d89e","3ca762ac044acba426c45be240075f2644fc9f645a0fbbb1aabc4c8656dcec1d")==0);
 CHECK(ub_firmware_match("eeb1b73b4c2b2857000000000000000000000000000000000000000000000000",UB_SHA256_APL_MGR)==0);
 /* apl_bdv 1.85: a data-only match, accepted for step() but NOT injectable */
 CHECK(ub_firmware_match("07f150cf640cf63a91a25994197ae26fb91f755e38b8a8313ee9c00a9280899f","0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d")==185);
 CHECK(ub_firmware_injectable("07f150cf640cf63a91a25994197ae26fb91f755e38b8a8313ee9c00a9280899f","0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d")==0);
 CHECK(ub_firmware_injectable("0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d","0eed5038db1c678b14cf666752c1c02db6c884ec1229329b9867f4342544714d")==1);
 /* payload variants: the exact hash picks its variant, a code-identical 1.85
  * build of the 1.82 pin takes the 1.82 one, anything else none */
 {static const struct ub_payload_variant vs[2]={
   {"1.82","a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3",0,0,0,0,0},
   {"1.85","eeb1b73b4c2b2857aae7699c8f74eaefdb77fd4aeaf79eea579b55aeb76a8ca0",0,0,0,0,0}};
  struct ub_payload pl={"x","a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3",0,0,vs,2},none={"x","",0,0,0,0};
  CHECK(ub_payload_variant(&pl,"a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3")==&vs[0]);
  CHECK(ub_payload_variant(&pl,"eeb1b73b4c2b2857aae7699c8f74eaefdb77fd4aeaf79eea579b55aeb76a8ca0")==&vs[1]);
  CHECK(ub_payload_variant(&pl,"fefee7cc5b3a1e4d1ebed3248cb554c9ff434f1667d5e571d12b030aafe5d89e")==&vs[0]);
  CHECK(ub_payload_variant(&pl,"07f150cf640cf63a91a25994197ae26fb91f755e38b8a8313ee9c00a9280899f")==0);
  CHECK(ub_payload_variant(&none,"a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3")==0);}
 /* a code-identical 1.85 file IS injectable */
 CHECK(ub_firmware_injectable("fefee7cc5b3a1e4d1ebed3248cb554c9ff434f1667d5e571d12b030aafe5d89e","a7692f0fa0e7e3dfb3cd90c69a7e9120204ab2c9b55ae3eea9b22e6425b6fbc3")==1);
 fflush(stdout); /* the runner shows stdout, then stderr (the log): the result goes last */
 fprintf(stderr,"%s (%d failures)\n",failures?"remote_test FAILED":"remote_test passed",failures);
 return failures!=0;
}
