// Durable settings. Read per request so the native interceptor picks up saves immediately.
// The host is responsible for authenticating /v0/management/plugin routes.
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef struct { int enabled, logging, debug; } sync_settings;
static const char *config_path(void) {
  const char *p=getenv("CODEX_TZ_CONFIG_FILE");
  return p && *p ? p : "/CLIProxyAPI/data/codex-timezone-sync.config";
}
static sync_settings load_settings(void) {
  sync_settings cfg={1,1,0};
  FILE *f=fopen(config_path(),"r");
  if (!f)return cfg;
  char line[96];
  while(fgets(line,sizeof(line),f)) {
    int value=-1;
    if(sscanf(line,"enabled=%d",&value)==1 && (value==0||value==1))cfg.enabled=value;
    else if(sscanf(line,"logging=%d",&value)==1 && (value==0||value==1))cfg.logging=value;
    else if(sscanf(line,"debug=%d",&value)==1 && (value==0||value==1))cfg.debug=value;
  }
  fclose(f);
  return cfg;
}
static int save_settings(sync_settings cfg) {
  const char *path=config_path();
  size_t n=strlen(path);
  if(n>1024)return 0;
  char temp[1100];snprintf(temp,sizeof(temp),"%s.XXXXXX",path);
  int fd=mkstemp(temp);
  if(fd<0)return 0;
  char data[70];
  int m=snprintf(data,sizeof(data),"enabled=%d\nlogging=%d\ndebug=%d\n",cfg.enabled,cfg.logging,cfg.debug);
  int ok=fchmod(fd,0600)==0 && write(fd,data,(size_t)m)==m && fsync(fd)==0;
  if(close(fd)!=0)ok=0;
  if(ok)ok=rename(temp,path)==0;
  if(!ok)unlink(temp);
  return ok;
}
static int json_boolean(const char *raw,tok v,int *out) {
  if(v.type!=PRIMITIVE)return 0;
  size_t n=(size_t)(v.end-v.start);
  if(n==4 && !memcmp(raw+v.start,"true",4)){*out=1;return 1;}
  if(n==5 && !memcmp(raw+v.start,"false",5)){*out=0;return 1;}
  return 0;
}
static int parse_settings_body(const char *raw,size_t len,sync_settings *cfg) {
  tok t[40];int n=parse_json(raw,len,t,40);
  if(n<1||t[0].type!=OBJECT)return 0;
  // Reject unknown keys to avoid silent misconfiguration.
  for(int i=1;i<n && t[i].start<t[0].end;) {
    if(t[i].parent!=0||t[i].type!=STRING||i+1>=n)return 0;
    if(!eq(raw,t[i],"enabled") && !eq(raw,t[i],"logging") && !eq(raw,t[i],"debug"))return 0;
    i=next_index(t,n,i+1);
  }
  const char *names[]={"enabled","logging","debug"};
  int *flags[]={&cfg->enabled,&cfg->logging,&cfg->debug};
  for(int j=0;j<3;j++) {
    int id=get_field(raw,t,n,0,names[j]);
    if(id>=0 && !json_boolean(raw,t[id],flags[j]))return 0;
  }
  return 1;
}

typedef struct { char zone[80],date[18],abbr[18],offset[12],ip[64]; long age; } sync_state;
static int load_state(sync_state *state) {
  memset(state,0,sizeof(*state));
  const char *path=getenv("CODEX_TZ_STATE_FILE");
  if(!path||!*path)path=DEFAULT_STATE;
  struct stat st;
  if(stat(path,&st)!=0)return 0;
  state->age=(long)difftime(time(NULL),st.st_mtime);
  if(state->age<0 || state->age>7200)return 0;
  FILE *f=fopen(path,"r");if(!f)return 0;
  char line[256];
  int ok=fgets(line,sizeof(line),f) &&
    sscanf(line,"%79[^|]|%17[^|]|%17[^|]|%11[^\n]",state->zone,state->date,state->abbr,state->offset)==4;
  if(ok && fgets(line,sizeof(line),f)) {
    line[strcspn(line,"\r\n")]=0;
    if(safe_field(line,"0123456789abcdefABCDEF:.",63))strcpy(state->ip,line);
  }
  fclose(f);
  return ok && safe_field(state->zone,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-",79) &&
    safe_field(state->date,"0123456789-",17) && strlen(state->date)==10 &&
    safe_field(state->abbr,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-",17) &&
    safe_field(state->offset,"0123456789:+-",11);
}

// Calculate today using the observed egress UTC offset, even after a date boundary
// between periodic IP-geolocation observations. No process-global TZ mutation.
static void derive_local_date(char *date,size_t cap,const char *offset) {
  int hour=0,minute=0;char sign=0,extra=0;
  if(sscanf(offset,"%c%2d:%2d%c",&sign,&hour,&minute,&extra)!=3||
     (sign!='+'&&sign!='-')||hour>23||minute>59)return;
  long seconds=(long)(hour*3600+minute*60)*(sign=='-'?-1:1);
  time_t epoch=time(NULL)+seconds;
  struct tm utc;
  if(gmtime_r(&epoch,&utc))strftime(date,cap,"%Y-%m-%d",&utc);
}
