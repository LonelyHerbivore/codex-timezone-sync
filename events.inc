// Lightweight, bounded telemetry: never persist model input/output, headers, tokens or auth data.
#ifndef _WIN32
#include <sys/file.h>
#endif
#define EVENT_LIMIT 131072
#define EVENT_ROWS 80
static const char *log_path(void) {
  const char *p=getenv("CODEX_TZ_LOG_FILE");
  return p && *p ? p : "/CLIProxyAPI/data/codex-timezone-sync.events";
}
static void log_event_detail(const char *kind,const char *original) {
  sync_settings cfg=load_settings();
  if(!cfg.logging)return;
  sync_state state;
  int available=load_state(&state);
  const char *zone=available?state.zone:"unknown";
  const char *path=log_path();
  int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
  if(fd<0)return;
  struct stat st;
  if(flock(fd,LOCK_EX)!=0 || fstat(fd,&st)!=0 || !S_ISREG(st.st_mode)) {close(fd);return;}
  // Rotate on size only; this small diagnostic log is not an audit archive.
  if(st.st_size>EVENT_LIMIT && ftruncate(fd,0)!=0) {flock(fd,LOCK_UN);close(fd);return;}
  char line[250];
  const char *before=original && safe_field(original,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-",79)?original:"unknown";
  int n=snprintf(line,sizeof(line),"%lld\t%s\t%s\t%s\n",(long long)time(NULL),kind,zone,before);
  if(n>0 && n<(int)sizeof(line)) {ssize_t written=write(fd,line,(size_t)n); (void)written;}
  flock(fd,LOCK_UN);close(fd);
}
static void log_event(const char *kind){log_event_detail(kind,NULL);}
static void handle_logs(cliproxy_buffer *out) {
  // Event files must never be symlinks, including when read through the UI.
  int fd=open(log_path(),O_RDONLY|O_NOFOLLOW);
  FILE *f=fd>=0?fdopen(fd,"r"):NULL;
  if(fd>=0 && !f)close(fd);
  char rows[EVENT_ROWS][250];size_t stored=0,total=0;
  if(f) {
    char line[250];
    while(fgets(line,sizeof(line),f)) {
      if(!strchr(line,'\n')) {int c;while((c=fgetc(f))!='\n'&&c!=EOF){}continue;}
      strcpy(rows[total%EVENT_ROWS],line);
      total++;if(stored<EVENT_ROWS)stored++;
    }
    fclose(f);
  }
  char json[18000];size_t used=0;
  used+=(size_t)snprintf(json,sizeof(json),"{\"ok\":true,\"events\":[");
  size_t valid=0;
  for(size_t i=0;i<stored;i++) {
    const char *line=rows[(total-1-i)%EVENT_ROWS];
    long long timestamp=0;char kind[25]="",zone[80]="",before[80]="unknown";
    if(sscanf(line,"%lld\t%24[a-z_]\t%79[A-Za-z0-9_+/-]\t%79[A-Za-z0-9_+/-]",&timestamp,kind,zone,before)<3)continue;
    int safe_kind=!strcmp(kind,"rewritten")||!strcmp(kind,"skipped")||!strcmp(kind,"no_state")||!strcmp(kind,"model_match")||!strcmp(kind,"model_mismatch");
    if(!safe_kind)continue;
    int n=snprintf(json+used,sizeof(json)-used,"%s{\"timestamp\":%lld,\"result\":\"%s\",\"timezone\":\"%s\",\"timezone_before\":\"%s\"}",
      valid?",":"",timestamp,kind,zone,before);
    if(n<0 || (size_t)n>=sizeof(json)-used)break;
    used+=(size_t)n;valid++;
  }
  snprintf(json+used,sizeof(json)-used,"]}");
  http_response(out,200,"application/json",json);
}
