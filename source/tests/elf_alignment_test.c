/* Parse the embedded ELF images without starting or modifying another process.
 * Host CPUs often tolerate the unaligned reads that fault on ARM LDRD. */
#include "../core/injector.c"
#include "payloads.h"
#include <assert.h>

static const struct ub_payload *const payloads[] = {
 &ub_payload_ui_menu,
 &ub_payload_ui_settings,
 &ub_payload_external_subtitles_precision_source,
 &ub_payload_external_subtitles_engine,
 &ub_payload_external_subtitles_native_text,
 &ub_payload_power_policy,
 &ub_payload_native_events_launch,
 &ub_payload_native_events_mgr_launch,
 &ub_payload_remote_keys,
};

static void check_payload(const struct ub_payload *payload){
 struct elf parsed;
 size_t size=payload->elf_size;
 unsigned char *copy=malloc(size+8);
 assert(copy);

 /* The declaration must guarantee alignment in the generated build. */
 assert((uintptr_t)payload->elf%8==0);
 assert(!view(&parsed,payload->elf,size));
 assert((uintptr_t)parsed.sh%_Alignof(Elf32_Shdr)==0);
 for(unsigned shift=1;shift<=3;shift++){
  memcpy(copy+shift,payload->elf,size);
  assert(view(&parsed,copy+shift,size)==-1);
 }

 memcpy(copy,payload->elf,size);
 Elf32_Ehdr *header=(Elf32_Ehdr *)copy;
 header->e_shoff++;
 assert(view(&parsed,copy,size)==-1);

 memcpy(copy,payload->elf,size);
 header->e_shoff=(Elf32_Off)((size+3)&~(size_t)3);
 assert(view(&parsed,copy,size)==-1);
 assert(view(&parsed,payload->elf,sizeof(Elf32_Ehdr)-1)==-1);
 memcpy(copy,payload->elf,size);header->e_phoff+=2;
 assert(view(&parsed,copy,size)==-1);
 for(unsigned i=0;i<((const Elf32_Ehdr *)payload->elf)->e_shnum;i++){
  memcpy(copy,payload->elf,size);
  Elf32_Shdr *sections=(Elf32_Shdr *)(copy+header->e_shoff);
  unsigned type=sections[i].sh_type;
  if(type==SHT_DYNSYM||type==SHT_REL||type==SHT_DYNAMIC||type==SHT_HASH){
   sections[i].sh_offset+=2;assert(view(&parsed,copy,size)==-1);
   memcpy(copy,payload->elf,size);sections[i].sh_size--;assert(view(&parsed,copy,size)==-1);
  }
 }
 memcpy(copy,payload->elf,size);
 Elf32_Shdr *sections=(Elf32_Shdr *)(copy+header->e_shoff);
 if(header->e_shstrndx){
  Elf32_Shdr *strings=sections+header->e_shstrndx;
  memset(copy+strings->sh_offset,'x',strings->sh_size);
  assert(view(&parsed,copy,size)==-1);
 }
 assert(!view(&parsed,payload->elf,size));
 assert(!findsym(&parsed,"ub_nonexistent_test_symbol"));
 free(copy);
}

int main(void){
 for(unsigned i=0;i<sizeof payloads/sizeof *payloads;i++)check_payload(payloads[i]);
 printf("elf_alignment_test passed (%zu embedded ELF images)\n",sizeof payloads/sizeof *payloads);
 return 0;
}
