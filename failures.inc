// Failed Codex rewrite payloads only. PRIVATE: full decoded request body may
// contain user text. Never serialize transport headers/OAuth credentials.
// Root-readable (0600), bounded to 48 MiB and never served by a static path.
#define FAILURE_FILE_MAX (48u * 1024u * 1024u)
#define FAILURE_ROWS 12
static const char *failure_path(void) {
  const char *p=getenv("CODEX_TZ_FAILURE_FILE");
  return p&&*p?p:"/CLIProxyAPI/data/codex-timezone-sync.failures";
}
static int persist_failure(const char *reason,const char *body,size_t len) {
  if(!body||!reason||len>MAX_REQUEST)return 0;
  char *encoded=b64encode((const unsigned char *)body,len);
  if(!encoded)return 0;
  int fd=open(failure_path(),O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  if(fd<0){free(encoded);return 0;}
  struct stat st;
  if(flock(fd,LOCK_EX)!=0 || fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) ||
     fchmod(fd,0600)!=0) {close(fd);free(encoded);return 0;}
  struct timespec now;
#ifdef _WIN32
  FILETIME ft;ULARGE_INTEGER ticks;
  GetSystemTimeAsFileTime(&ft);
  ticks.LowPart=ft.dwLowDateTime;ticks.HighPart=ft.dwHighDateTime;
  unsigned long long unix_ticks=ticks.QuadPart-116444736000000000ULL;
  now.tv_sec=(time_t)(unix_ticks/10000000ULL);
  now.tv_nsec=(long)((unix_ticks%10000000ULL)*100ULL);
#else
  clock_gettime(CLOCK_REALTIME,&now);
#endif
  static unsigned int nonce=0;
  unsigned long long id=(unsigned long long)now.tv_sec*1000000000ULL+
    (unsigned long long)now.tv_nsec+(unsigned long long)__sync_fetch_and_add(&nonce,1);
  char header[120];
  int prefix=snprintf(header,sizeof(header),"%llu\t%lld\t%s\t",
    id,(long long)now.tv_sec,reason);
  size_t size=strlen(encoded);
  int ok=prefix>0 && prefix<(int)sizeof(header) && size+prefix+1<FAILURE_FILE_MAX;
  if(ok && (size_t)st.st_size+size+(size_t)prefix+1>FAILURE_FILE_MAX)
    ok=ftruncate(fd,0)==0;
  if(ok) {
    // Append under lock; incomplete tail lines are ignored by readers.
    ok=write(fd,header,(size_t)prefix)==prefix;
    for(size_t pos=0;ok&&pos<size;) {
      ssize_t n=write(fd,encoded+pos,size-pos);
      if(n<=0){ok=0;break;}
      pos+=(size_t)n;
    }
    if(ok)ok=write(fd,"\n",1)==1 && fsync(fd)==0;
  }
  flock(fd,LOCK_UN);close(fd);free(encoded);
  return ok;
}
static FILE *open_failure_reader(void) {
  int fd=open(failure_path(),O_RDONLY|O_NOFOLLOW);
  if(fd<0)return NULL;
  struct stat st;
  if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || flock(fd,LOCK_SH)!=0){close(fd);return NULL;}
  FILE *stream=fdopen(fd,"r");
  if(!stream){flock(fd,LOCK_UN);close(fd);}
  return stream;
}
static int parse_failure_row(char *line,char **id,char **timestamp,char **reason,char **data) {
  char *t1=strchr(line,'\t');if(!t1)return 0;*t1++=0;
  char *t2=strchr(t1,'\t');if(!t2)return 0;*t2++=0;
  char *t3=strchr(t2,'\t');if(!t3)return 0;*t3++=0;
  size_t n=strlen(t3);
  if(n==0||t3[n-1]!='\n')return 0;
  t3[n-1]=0;
  if(!safe_field(line,"0123456789",22)||!safe_field(t1,"0123456789",20)||
     !safe_field(t2,"abcdefghijklmnopqrstuvwxyz0123456789_",24))return 0;
  *id=line;*timestamp=t1;*reason=t2;*data=t3;return 1;
}
static void handle_failures(cliproxy_buffer *out) {
  FILE *f=open_failure_reader();
  char ids[FAILURE_ROWS][24],times[FAILURE_ROWS][22],reasons[FAILURE_ROWS][26];
  size_t sizes[FAILURE_ROWS]={0},count=0,total=0;
  if(f){
    char *line=NULL;size_t cap=0;ssize_t read_count=0;
    while((read_count=getline(&line,&cap,f))>0) {
      if(read_count>(ssize_t)(MAX_REQUEST*4/3+512))continue;
      char *id=NULL,*ts=NULL,*reason=NULL,*data=NULL;
      if(!parse_failure_row(line,&id,&ts,&reason,&data))continue;
      int idx=(int)(total%FAILURE_ROWS);
      snprintf(ids[idx],sizeof(ids[idx]),"%s",id);
      snprintf(times[idx],sizeof(times[idx]),"%s",ts);
      snprintf(reasons[idx],sizeof(reasons[idx]),"%s",reason);
      sizes[idx]=strlen(data)*3/4;
      total++;if(count<FAILURE_ROWS)count++;
    }
    free(line);fclose(f);
  }
  char json[5000];size_t used=0;
  used+=(size_t)snprintf(json,sizeof(json),"{\"ok\":true,\"records\":[");
  for(size_t i=0;i<count;i++){
    size_t idx=(total-1-i)%FAILURE_ROWS;
    int n=snprintf(json+used,sizeof(json)-used,
      "%s{\"id\":\"%s\",\"timestamp\":%s,\"reason\":\"%s\",\"size\":%zu}",
      i?",":"",ids[idx],times[idx],reasons[idx],sizes[idx]);
    if(n<0||(size_t)n>=sizeof(json)-used-3)break;
    used+=(size_t)n;
  }
  snprintf(json+used,sizeof(json)-used,"]}");
  http_response(out,200,"application/json",json);
}
static void handle_failure_view(const char *raw,const tok *tokens,int body,cliproxy_buffer *out) {
  if(body<0 || tokens[body].type!=STRING) {
    http_response(out,400,"application/json","{\"ok\":false,\"reason\":\"missing id\"}");return;
  }
  size_t n=0;
  unsigned char *decoded=b64decode(raw+tokens[body].start,
    (size_t)(tokens[body].end-tokens[body].start),&n);
  tok t[24];int nt=decoded&&n<256?parse_json((char *)decoded,n,t,24):-1;
  int field=nt>0?get_field((char *)decoded,t,nt,0,"id"):-1;
  char *id=field>=0&&t[field].type==STRING?json_unescape((char *)decoded,t[field]):NULL;
  free(decoded);
  if(!id || !safe_field(id,"0123456789",22)) {
    free(id);http_response(out,400,"application/json","{\"ok\":false,\"reason\":\"invalid id\"}");return;
  }
  FILE *f=open_failure_reader();
  if(!f){free(id);http_response(out,404,"application/json","{\"ok\":false,\"reason\":\"record unavailable\"}");return;}
  char *line=NULL;size_t cap=0;ssize_t got;
  char *json=NULL;
  while((got=getline(&line,&cap,f))>0){
    if(got>(ssize_t)(MAX_REQUEST*4/3+512))continue;
    char *rid=NULL,*ts=NULL,*reason=NULL,*data=NULL;
    if(!parse_failure_row(line,&rid,&ts,&reason,&data)||strcmp(rid,id)!=0)continue;
    size_t length=strlen(data);
    if(length>MAX_REQUEST*4/3+8)break;
    json=malloc(length+220);
    if(json)snprintf(json,length+220,
      "{\"ok\":true,\"id\":\"%s\",\"reason\":\"%s\",\"kind\":\"%s\",\"body_base64\":\"%s\"}",
      rid,reason,!strcmp(reason,"invalid_base64")?"encoded-field":"decoded-body",data);
    break;
  }
  free(line);fclose(f);free(id);
  if(!json){http_response(out,404,"application/json","{\"ok\":false,\"reason\":\"record not found\"}");return;}
  http_response(out,200,"application/json",json);
  free(json);
}
static void handle_clear_failures(cliproxy_buffer *out) {
  int fd=open(failure_path(),O_WRONLY|O_CREAT|O_NOFOLLOW,0600);
  if(fd<0){http_response(out,500,"application/json","{\"ok\":false}");return;}
  struct stat st;
  int ok=flock(fd,LOCK_EX)==0&&fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&ftruncate(fd,0)==0;
  if(ok)ok=fsync(fd)==0;
  flock(fd,LOCK_UN);close(fd);
  http_response(out,ok?200:500,"application/json",ok?"{\"ok\":true}":"{\"ok\":false}");
}
