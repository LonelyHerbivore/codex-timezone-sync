// Persistent success/failure totals, independent of the optional activity logger.
// One short flock transaction per attempted Codex rewrite, mode 0600.
static const char *stats_path(void) {
  const char *p=getenv("CODEX_TZ_STATS_FILE");
  return p&&*p?p:"/CLIProxyAPI/data/codex-timezone-sync.stats";
}
static int stats_update(int success,int failure,int reset,
                        unsigned long long *s,unsigned long long *f) {
  int fd=open(stats_path(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);
  if(fd<0)return 0;
  struct stat st;
  if(flock(fd,LOCK_EX)!=0 || fstat(fd,&st)!=0 || !S_ISREG(st.st_mode)) {close(fd);return 0;}
  char line[96]={0};
  ssize_t n=read(fd,line,sizeof(line)-1);
  unsigned long long yes=0,no=0;
  if(n>0 && sscanf(line,"%llu %llu",&yes,&no)!=2)yes=no=0;
  int ok=1;
  if(reset){yes=0;no=0;}
  else {
    if(success && yes<ULLONG_MAX)yes++;
    if(failure && no<ULLONG_MAX)no++;
  }
  if(success || failure || reset) {
    int len=snprintf(line,sizeof(line),"%llu %llu\n",yes,no);
    ok=len>0 && len<(int)sizeof(line) && lseek(fd,0,SEEK_SET)==0 &&
      ftruncate(fd,0)==0 && write(fd,line,(size_t)len)==len && fsync(fd)==0;
  }
  if(s)*s=yes;
  if(f)*f=no;
  flock(fd,LOCK_UN);close(fd);
  return ok;
}
static void handle_stats(cliproxy_buffer *out) {
  unsigned long long yes=0,no=0;
  if(!stats_update(0,0,0,&yes,&no)) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"stats unavailable\"}");return;
  }
  char json[180];
  snprintf(json,sizeof(json),"{\"ok\":true,\"success\":%llu,\"failure\":%llu}",yes,no);
  http_response(out,200,"application/json",json);
}
static void handle_clear_stats(cliproxy_buffer *out) {
  if(!stats_update(0,0,1,NULL,NULL)) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"stats reset failed\"}");return;
  }
  http_response(out,200,"application/json","{\"ok\":true}");
}
