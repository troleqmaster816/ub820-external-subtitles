/* Only mocked byte arrays and temporary output streams; no process/device I/O. */
#include "../core/poke.h"
#include <assert.h>

static unsigned char memory_bytes[4];
static unsigned writes,reads,checks;
static size_t write_width,read_width;
static uint32_t last_address;
static int write_error,read_error,flip_byte;
static FILE *output;
static int mock_write(int pid,uint32_t address,const void *data,size_t length){
 assert(pid==123);assert(length>=1 && length<=4);
 writes++;write_width=length;last_address=address;
 if(write_error)return -1;
 memcpy(memory_bytes,data,length);return 0;
}
static int mock_read(int pid,uint32_t address,void *data,size_t length){
 assert(pid==123);assert(address==last_address);assert(length>=1 && length<=4);
 reads++;read_width=length;
 if(read_error)return -1;
 memcpy(data,memory_bytes,length);
 if(flip_byte>=0 && (unsigned)flip_byte<length)((unsigned char *)data)[flip_byte]^=0x80;
 return 0;
}
static void reset(void){
 memset(memory_bytes,0xa5,sizeof memory_bytes);writes=reads=0;
 write_width=read_width=0;write_error=read_error=0;flip_byte=-1;
}
static void check(int condition){checks++;assert(condition);}
static int value(uint32_t address,uint32_t number,uint32_t width){
 return ub_poke_value(123,address,number,width,mock_write,mock_read,output,output);
}
static int command(int argc,char **argv){
 return ub_poke_command(argc,argv,mock_write,mock_read,output,output);
}
int main(void){
 output=tmpfile();assert(output);
 for(unsigned width=1;width<=4;width++){
  reset();check(value(0x1000,0x89abcdef,width)==0);
  check(writes==1 && reads==1 && write_width==width && read_width==width);
  for(unsigned j=width;j<4;j++)check(memory_bytes[j]==0xa5);
  for(unsigned byte=0;byte<width;byte++){
   reset();flip_byte=(int)byte;check(value(0x1000,0x89abcdef,width)!=0);
  }
  reset();flip_byte=(int)width;check(value(0x1000,0x89abcdef,width)==0);
  reset();check(value(UINT32_MAX-width+1,0xffffffff,width)==0);
  if(width>1){reset();check(value(UINT32_MAX-width+2,1,width)!=0);check(!writes && !reads);}
 }
 reset();check(value(0,0,0)!=0);check(!writes && !reads);
 reset();check(value(0,0,5)!=0);check(!writes && !reads);
 reset();check(value(0,0,UINT32_MAX)!=0);check(!writes && !reads);
 reset();write_error=1;check(value(1,2,4)!=0);check(writes==1 && reads==0);
 reset();read_error=1;check(value(1,2,4)!=0);check(writes==1 && reads==1);
 char *argv[]={"test","--poke","123","0x1000","0x89abcdef","4","extra"};
 for(int argc=0;argc<5;argc++){reset();check(command(argc,argv)!=0);check(!writes && !reads);}
 reset();check(command(7,argv)!=0);check(!writes && !reads);
 reset();check(command(5,argv)==0);check(write_width==4 && read_width==4);
 for(unsigned width=1;width<=4;width++){
  char number[2]={(char)('0'+width),0};argv[5]=number;
  reset();check(command(6,argv)==0);check(write_width==width && read_width==width);
 }
 argv[5]="4";
 const char *invalid[]={"","-1","+1"," 1","1 ","1x","0x","0x100000000","4294967296","18446744073709551616"};
 for(unsigned field=2;field<=5;field++){
  char *saved=argv[field];
  for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){
   argv[field]=(char *)invalid[i];reset();check(command(6,argv)!=0);check(!writes && !reads);
  }
  argv[field]=saved;
 }
 argv[2]="0";reset();check(command(6,argv)!=0);check(!writes && !reads);argv[2]="123";
 argv[2]="2147483648";reset();check(command(6,argv)!=0);check(!writes && !reads);argv[2]="123";
 argv[5]="0";reset();check(command(6,argv)!=0);check(!writes && !reads);argv[5]="4";
 argv[3]="0xffffffff";reset();check(command(6,argv)!=0);check(!writes && !reads);
 argv[5]="1";reset();check(command(6,argv)==0);check(last_address==UINT32_MAX);
 argv[3]="0";argv[4]="0xffffffff";reset();check(command(6,argv)==0);
 check(fclose(output)==0);printf("poke_test passed: %u checks\n",checks);return 0;
}
