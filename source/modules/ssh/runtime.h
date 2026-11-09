/* Managed SSH files live in RAM. The home and user startup files are retained
 * across server restarts; only our binaries and applet links are refreshed.
 * No mounts, chroot, forced command, whitelist or firmware passwd edits. */
#ifndef UB_SSH_RUNTIME_H
#define UB_SSH_RUNTIME_H
static int runtime_directory(const char *path){
 int fd,rc;
 if(mkdir(path,0700)&&errno!=EEXIST)return -1;
 fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 rc=fchmod(fd,0700);if(close(fd))rc=-1;return rc;
}
static int runtime_profile(void){
 static const char text[]="# UB820 SSH session profile (RAM only). User settings: $HOME/.profile\n";
 int fd=open(DIR "/profile",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600),rc=0;
 if(fd<0){
  struct ub_stat st;
  if(errno!=EEXIST)return -1;
  fd=open(DIR "/profile",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
  rc=ub_fstat(fd,&st)||!S_ISREG(st.mode)?-1:0;
  if(close(fd))rc=-1;
  return rc;
 }
 size_t done=0;
 while(done<sizeof text-1){ssize_t n=write(fd,text+done,sizeof text-1-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-1;break;}done+=(size_t)n;}
 if(close(fd))rc=-1;
 if(rc)unlink(DIR "/profile");
 return rc;
}
static int prepare_runtime(char *err,size_t cap){
 static const struct {const char *path;const unsigned char *data;const size_t *size;} files[]={
  {BUSYBOX_BIN,ub_tool_busybox,&ub_tool_busybox_size},
  {SFTP_BIN,ub_tool_sftp_server,&ub_tool_sftp_server_size},
  {SCP_BIN,ub_tool_scp,&ub_tool_scp_size},
  {SSH_CLIENT_BIN,ub_tool_dbclient,&ub_tool_dbclient_size},
 };
 const char *dirs[]={DIR,BIN_DIR,HOME_DIR,HOME_DIR "/.ssh"};
 for(unsigned i=0;i<sizeof dirs/sizeof *dirs;i++)if(runtime_directory(dirs[i])){
  snprintf(err,cap,"cannot prepare SSH directory %s (%s)",dirs[i],strerror(errno));return -1;
 }
 for(unsigned i=0;i<sizeof files/sizeof *files;i++)if(write_blob(files[i].path,files[i].data,*files[i].size,0700)){
  snprintf(err,cap,"cannot publish SSH tool %s (%s)",files[i].path,strerror(errno));return -1;
 }
 {char *argv[]={(char *)BUSYBOX_BIN,(char *)"--install",(char *)"-s",(char *)BIN_DIR,0};
  if(run(argv,10)){snprintf(err,cap,"cannot install the SSH shell/tool links");return -1;}
 }
 /* Publish a known managed shell link even if a previous RAM install had
  * another shell at that name. Other user startup files remain untouched. */
 unlink(SSH_SHELL ".new");
 if(symlink("busybox",SSH_SHELL ".new")||rename(SSH_SHELL ".new",SSH_SHELL)){
  unlink(SSH_SHELL ".new");snprintf(err,cap,"cannot publish the SSH shell link");return -1;
 }
 {int fd=open(SSH_SHELL,O_RDONLY|O_CLOEXEC);
  if(fd<0){snprintf(err,cap,"SSH shell is missing after tool installation");return -1;}close(fd);
 }
 if(runtime_profile()){snprintf(err,cap,"cannot prepare SSH login profile");return -1;}
 return 0;
}
/* Keep the host identity across Stop/Start. A failed generation never publishes
 * a partial final key; a user-supplied invalid key is reported, not overwritten. */
static int prepare_hostkey(char *err,size_t cap){
 struct ub_stat st;int fd,rc;
 fd=open(HOSTKEY,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
 if(fd>=0){
  rc=ub_fstat(fd,&st)||!S_ISREG(st.mode)||st.size<=0?-1:0;
  if(!rc&&fchmod(fd,0600))rc=-1;
  if(close(fd))rc=-1;
  if(rc){snprintf(err,cap,"RAM SSH host key is not a regular nonempty file");return -1;}
  return 0;
 }
 if(errno!=ENOENT){snprintf(err,cap,"cannot open RAM SSH host key (%s)",strerror(errno));return -1;}
 unlink(HOSTKEY ".new");
 {char *argv[]={(char *)DROPBEARKEY_BIN,(char *)"-t",(char *)"ed25519",(char *)"-f",(char *)HOSTKEY ".new",0};
  if(run(argv,20)){unlink(HOSTKEY ".new");snprintf(err,cap,"SSH host key generation failed");return -1;}
 }
 fd=open(HOSTKEY ".new",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
 if(fd<0){unlink(HOSTKEY ".new");snprintf(err,cap,"SSH host key generation produced no key");return -1;}
 rc=ub_fstat(fd,&st)||!S_ISREG(st.mode)||st.size<=0?-1:0;
 if(!rc&&fchmod(fd,0600))rc=-1;
 if(close(fd))rc=-1;
 if(rc||rename(HOSTKEY ".new",HOSTKEY)){
  unlink(HOSTKEY ".new");snprintf(err,cap,"cannot publish the SSH host key");return -1;
 }
 return 0;
}
#endif
