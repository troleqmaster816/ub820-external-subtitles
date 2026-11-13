/* Actual RAM preparation and atomic publication, with host-only asset stubs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ub.h"
#ifndef UB_SSH_DIR
#define UB_SSH_DIR "/tmp/ub820-ssh-runtime-test"
#endif
#define UB_SSH_KEYGEN_BIN UB_SSH_DIR "/keygen-fixture"
#define UB_TOOLS_H
const unsigned char ub_tool_busybox[]="#!/bin/sh\ntest \"$1\" = --install && test \"$2\" = -s || exit 8\nln -sf busybox \"$3/uname\"\n";
const size_t ub_tool_busybox_size=sizeof ub_tool_busybox-1;
const unsigned char ub_tool_sftp_server[]="sftp fixture\n";
const size_t ub_tool_sftp_server_size=sizeof ub_tool_sftp_server-1;
const unsigned char ub_tool_scp[]="scp fixture\n";
const size_t ub_tool_scp_size=sizeof ub_tool_scp-1;
const unsigned char ub_tool_dbclient[]="ssh fixture\n";
const size_t ub_tool_dbclient_size=sizeof ub_tool_dbclient-1;
const unsigned char ub_tool_dropbear[]="unused server fixture\n";
const size_t ub_tool_dropbear_size=sizeof ub_tool_dropbear-1;
const unsigned char ub_tool_dropbearkey[]="unused key generator fixture\n";
const size_t ub_tool_dropbearkey_size=sizeof ub_tool_dropbearkey-1;
static int publication_failure;
static ssize_t runtime_write(int fd,const void *data,size_t n){
 if(publication_failure==2){publication_failure=1;return write(fd,data,n>4?4:n);}
 if(publication_failure==1){publication_failure=0;errno=ENOSPC;return -1;}
 return write(fd,data,n);
}
#define write runtime_write
#include "../modules/ssh/module.c"
#undef write
static int failures;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);failures++;}}while(0)
static void put(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);if(fd<0)return;CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text));CHECK(!close(fd));}
static size_t read_text(const char *path,char *text,size_t cap){int fd=open(path,O_RDONLY);ssize_t n=fd>=0?read(fd,text,cap-1):0;if(fd>=0)close(fd);if(n<0)n=0;text[n]=0;return (size_t)n;}
static void expect_mode(const char *path,mode_t mode){struct stat st={0};CHECK(!stat(path,&st));CHECK((st.st_mode&0777)==mode);}
static void test_runtime(void){
 char err[200],text[256],target[80];ssize_t n;
 unlink(DIR "/profile");CHECK(!prepare_runtime(err,sizeof err));
 expect_mode(DIR,0700);expect_mode(BIN_DIR,0700);expect_mode(HOME_DIR,0700);expect_mode(HOME_DIR "/.ssh",0700);
 expect_mode(BUSYBOX_BIN,0700);expect_mode(SFTP_BIN,0700);expect_mode(SCP_BIN,0700);expect_mode(SSH_CLIENT_BIN,0700);
 n=readlink(SSH_SHELL,target,sizeof target-1);CHECK(n>0);if(n>0){target[n]=0;CHECK(!strcmp(target,"busybox"));}
 CHECK(!access(BIN_DIR "/uname",F_OK));
 put(DIR "/profile","# retained custom global RAM profile\n");put(HOME_DIR "/.profile","# retained user profile\n");
 CHECK(!prepare_runtime(err,sizeof err));
 read_text(DIR "/profile",text,sizeof text);CHECK(strstr(text,"retained custom global RAM profile"));
 read_text(HOME_DIR "/.profile",text,sizeof text);CHECK(strstr(text,"retained user profile"));
 CHECK(!runtime_directory(DIR "/outside"));unlink(DIR "/bad-directory");
 CHECK(!symlink("outside",DIR "/bad-directory"));CHECK(runtime_directory(DIR "/bad-directory")<0);unlink(DIR "/bad-directory");
 unlink(DIR "/profile");CHECK(!mkfifo(DIR "/profile",0600));CHECK(runtime_profile()<0);
 unlink(DIR "/profile");CHECK(!runtime_profile());
}
static void test_atomic_blob(void){
 static const char path[]=DIR "/atomic-blob";
 static const unsigned char old[]="old complete blob",newer[]="new complete replacement";
 char text[80];int fd;
 CHECK(!write_blob(path,old,sizeof old,0700));fd=open(path,O_RDONLY);CHECK(fd>=0);
 publication_failure=2;CHECK(write_blob(path,newer,sizeof newer,0700)<0);
 read_text(path,text,sizeof text);CHECK(!strcmp(text,(const char *)old));CHECK(access(DIR "/atomic-blob.new",F_OK));
 CHECK(!write_blob(path,newer,sizeof newer,0700));expect_mode(path,0700);
 read_text(path,text,sizeof text);CHECK(!strcmp(text,(const char *)newer));
 memset(text,0,sizeof text);CHECK(read(fd,text,sizeof old)==sizeof old);CHECK(!strcmp(text,(const char *)old));close(fd);
}
static void test_hostkey(void){
 static const unsigned char keygen[]="#!/bin/sh\nprintf x >> '" DIR "/keygen-calls'\nif test -f '" DIR "/keygen-fail'; then printf partial > \"$4\"; exit 9; fi\nprintf 'complete fixture host key' > \"$4\"\n";
 char err[200],text[80];
 unlink(HOSTKEY);unlink(HOSTKEY ".new");unlink(DIR "/keygen-fail");put(DIR "/keygen-calls","");
 CHECK(!write_blob(DROPBEARKEY_BIN,keygen,sizeof keygen-1,0700));
 CHECK(!prepare_hostkey(err,sizeof err));expect_mode(HOSTKEY,0600);CHECK(access(HOSTKEY ".new",F_OK));
 CHECK(read_text(DIR "/keygen-calls",text,sizeof text)==1);
 CHECK(!prepare_hostkey(err,sizeof err));CHECK(read_text(DIR "/keygen-calls",text,sizeof text)==1);
 unlink(HOSTKEY);put(DIR "/keygen-fail","fail");
 CHECK(prepare_hostkey(err,sizeof err)<0);CHECK(access(HOSTKEY,F_OK));CHECK(access(HOSTKEY ".new",F_OK));
 unlink(DIR "/keygen-fail");CHECK(!prepare_hostkey(err,sizeof err));CHECK(read_text(DIR "/keygen-calls",text,sizeof text)==3);
 unlink(HOSTKEY);CHECK(!mkfifo(HOSTKEY,0600));CHECK(prepare_hostkey(err,sizeof err)<0);unlink(HOSTKEY);
 CHECK(!symlink("profile",HOSTKEY));CHECK(prepare_hostkey(err,sizeof err)<0);unlink(HOSTKEY);
 put(HOSTKEY,"");CHECK(prepare_hostkey(err,sizeof err)<0);unlink(HOSTKEY);
}
int main(void){
 /* Fail a blocking-open regression instead of hanging the test runner. */
 alarm(10);umask(000);test_runtime();test_atomic_blob();test_hostkey();alarm(0);
 printf("ssh_runtime_test %s (%d failures): private directories; retained profiles; atomic blobs; host-key reuse/failure/type checks\n",failures?"FAILED":"passed",failures);
 return failures!=0;
}
