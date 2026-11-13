/* Native OPTIONS pages must retain stock routing and context.  This fixture
 * does not model CTK drawing or claim that a rendered popup contains text. */
#define main power_ui_fixture_main
#include "power_ui_test.c"
#undef main

static unsigned char native_context[0x3600], expected_context[0x3600];
static unsigned native_key, native_calls, refresh_calls;
static void *get_native_context(int resource){assert(resource==0x27);return native_context;}
static unsigned get_native_key(void *event){(void)event;return native_key;}
static int stock_key(void){native_calls++;return 37;}
static void stock_refresh(void){refresh_calls++;}
static void prepare_native(unsigned page){
 memset(&m,0,sizeof m);memset(native_context,0,sizeof native_context);
 *field(native_context,PAGE_OFF)=page;
 *field(native_context,ROOT_WINDOW_OFF)=0x1234;
 *field(native_context,SELECT_OFF)=1;
 *field(native_context,0x3380)=(uint32_t)-8; /* subtitle position */
 *field(native_context,0x3384)=0;   /* luminance: Original */
 *field(native_context,0x3388)=1;   /* size: Normal */
 *field(native_context,0x338c)=3;
 *field(native_context,0x3390)=0;   /* character code: UTF-8 */
 *field(native_context,0x3394)=1;
 ((struct descriptor *)(native_context+ROW_OFF))[1].child=1;
 m.ctx=native_context;m.owner=(Widget)(uintptr_t)0x1234;m.base=4;m.count=2;
 ub_get_ctx=get_native_context;ub_get_key=get_native_key;
 ub_original_key_router=stock_key;ub_original_refresh=stock_refresh;
 ub_lookup=0;native_calls=refresh_calls=0;
 memcpy(expected_context,native_context,sizeof native_context);
}
int main(void){
 static const unsigned pages[]={0,1,0x14,0x16};
 static const unsigned keys[]={KEY_OK,KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_BACK};
 unsigned i,j,checks=0;
 for(i=0;i<sizeof pages/sizeof pages[0];i++){
  prepare_native(pages[i]);
  for(j=0;j<sizeof keys/sizeof keys[0];j++){
   native_key=keys[j];assert(ub_hook_key_router()==37);
   assert(native_calls==j+1&&!m.active);
   assert(!memcmp(native_context,expected_context,sizeof native_context));checks++;
  }
  ub_hook_refresh();assert(refresh_calls==1&&!m.active);
  assert(!memcmp(native_context,expected_context,sizeof native_context));checks++;
 }
 /* An old private preview must be destroyed when stock opens a native page,
  * rather than redrawn over its popup or consuming its direction keys. */
 prepare_native(0x16);m.active=m.preview=1;m.window=(Widget)(uintptr_t)2;
 native_key=KEY_UP;assert(ub_hook_key_router()==37&&native_calls==1);
 assert(!m.active&&!m.window&&!memcmp(native_context,expected_context,sizeof native_context));checks++;
 prepare_native(0x16);m.active=m.preview=1;m.window=(Widget)(uintptr_t)2;
 ub_hook_refresh();assert(refresh_calls==1&&!m.active&&!m.window);
 assert(!memcmp(native_context,expected_context,sizeof native_context));checks++;
 /* Likewise, a non-preview private page invalidated by the native owner/page
  * transition must relinquish the key before handling its own controls. */
 prepare_native(0x16);m.active=1;m.window=(Widget)(uintptr_t)2;
 native_key=KEY_DOWN;assert(ub_hook_key_router()==37&&native_calls==1&&!m.active);
 assert(!memcmp(native_context,expected_context,sizeof native_context));checks++;
 printf("native_popup_test passed (%u scenarios)\n",checks);return 0;
}
