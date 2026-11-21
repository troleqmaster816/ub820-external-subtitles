#ifndef UB_SUB_MEMORY_PROTOCOL_H
#define UB_SUB_MEMORY_PROTOCOL_H
/* Private RAM records between the helper and the apl_meda_bdplay payload.
 * Both are written atomically (temporary file + rename, 0600) and are only
 * trusted for the exact apl_meda_bdplay instance (PID and start time) they
 * name. Paths are always below /mnt/usb.
 *
 * hint      (helper -> payload): written by engine_prepare before the payload
 *           is installed; read once by the payload's initialize().
 * selection (payload -> helper): written after each successful Open. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef UB_SUB_HINT_FILE
#define UB_SUB_HINT_FILE "/tmp/.ub820_sub_hint"
#endif
#ifndef UB_SUB_SELECTION_FILE
#define UB_SUB_SELECTION_FILE "/tmp/.ub820_sub_selection"
#endif
#define UB_SUB_HINT_MAGIC 0x48534255u      /* "UBSH" */
#define UB_SUB_SELECTION_MAGIC 0x53534255u /* "UBSS" */
#define UB_SUB_RECORD_VERSION 1u
#define UB_SUB_PATH_BYTES 256u
#define UB_SUB_PATH_LIMIT 250u
#define UB_SUB_HINT_START_DIR 1u
#define UB_SUB_HINT_AUTOLOAD 2u
#define UB_SUB_KIND_DEFAULT 1u  /* the player's own /mnt/usb/sub/sub.srt */
#define UB_SUB_KIND_CHOSEN 2u   /* chosen in the file browser */
#define UB_SUB_KIND_REMEMBERED 3u  /* the disc memory's file, opened instead of sub.srt */
/* The only file the player's own autostart opens (thread 0x18a48). */
#define UB_DEFAULT_SRT "/mnt/usb/sub/sub.srt"

struct ub_sub_hint {
 uint32_t magic,version;
 uint32_t pid,birth_lo,birth_hi;
 uint32_t flags;
 char start_dir[UB_SUB_PATH_BYTES];
 char autoload[UB_SUB_PATH_BYTES];
 int8_t sync,charcode;
 uint8_t reserved[6];
};
struct ub_sub_selection {
 uint32_t magic,version,pid,birth_lo,birth_hi,seq,kind;
 char path[UB_SUB_PATH_BYTES];
};
_Static_assert(sizeof(struct ub_sub_hint)==544,"hint layout");
_Static_assert(offsetof(struct ub_sub_hint,start_dir)==24,"hint layout");
_Static_assert(offsetof(struct ub_sub_hint,autoload)==280,"hint layout");
_Static_assert(offsetof(struct ub_sub_hint,sync)==536,"hint layout");
_Static_assert(sizeof(struct ub_sub_selection)==284,"selection layout");
_Static_assert(offsetof(struct ub_sub_selection,path)==28,"selection layout");

/* A USB path in the form the payload's own browser produces: "/mnt/usb" or
 * below it, no empty, "." or ".." components, no control characters or
 * backslash, no trailing slash, NUL within the field, at most 250 bytes. */
static inline int ub_sub_path_ok(const char *s,size_t cap){
 static const char root[]="/mnt/usb";
 const char *end=memchr(s,0,cap);size_t n,begin=1;
 if(!end)return 0;
 n=(size_t)(end-s);
 if(n<8||n>UB_SUB_PATH_LIMIT||memcmp(s,root,8))return 0;
 if(n>8&&(s[8]!='/'||s[n-1]=='/'))return 0;
 for(size_t i=0;i<=n;i++){
  unsigned char c=(unsigned char)s[i];
  if(i<n&&(c<32||c==127||c=='\\'))return 0;
  if(i>0&&(c=='/'||c==0)){
   size_t size=i-begin;
   if(!size||(size==1&&s[begin]=='.')||(size==2&&s[begin]=='.'&&s[begin+1]=='.'))return 0;
   begin=i+1;
  }
 }
 return 1;
}
static inline int ub_sub_hint_valid(const struct ub_sub_hint *h,uint32_t pid,uint64_t born){
 for(unsigned i=0;i<sizeof h->reserved;i++)if(h->reserved[i])return 0;
 if(h->magic!=UB_SUB_HINT_MAGIC||h->version!=UB_SUB_RECORD_VERSION||!pid||h->pid!=pid||
    (((uint64_t)h->birth_hi<<32)|h->birth_lo)!=born||(h->flags&~(UB_SUB_HINT_START_DIR|UB_SUB_HINT_AUTOLOAD)))return 0;
 if(!memchr(h->start_dir,0,sizeof h->start_dir)||!memchr(h->autoload,0,sizeof h->autoload))return 0;
 if((h->flags&UB_SUB_HINT_START_DIR)&&!ub_sub_path_ok(h->start_dir,sizeof h->start_dir))return 0;
 if((h->flags&UB_SUB_HINT_AUTOLOAD)&&!ub_sub_path_ok(h->autoload,sizeof h->autoload))return 0;
 if(h->sync<-30||h->sync>30||h->charcode<0||h->charcode>1)return 0;
 return 1;
}
static inline int ub_sub_selection_valid(const struct ub_sub_selection *s,uint32_t pid,uint64_t born){
 return s->magic==UB_SUB_SELECTION_MAGIC&&s->version==UB_SUB_RECORD_VERSION&&pid&&s->pid==pid&&
  (((uint64_t)s->birth_hi<<32)|s->birth_lo)==born&&s->seq&&
  s->kind>=UB_SUB_KIND_DEFAULT&&s->kind<=UB_SUB_KIND_REMEMBERED&&ub_sub_path_ok(s->path,sizeof s->path);
}
#endif
