// codex-timezone-sync: CLIProxyAPI C ABI v1 request interceptor.
// No third-party C libraries; fail-open. Only failed Codex API bodies may be archived.
// Transport credentials and surrounding management/interceptor headers are excluded.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
static void derive_local_date(char *date,size_t cap,const char *offset);
#include "dashboard_bundle.h"

#define ABI_VERSION 1
#define MAX_REQUEST (12u * 1024u * 1024u)
#define MAX_TOKENS 150000
#define TZ_MARKER "[CPAMC_TIMEZONE_V1]"
#define DEFAULT_STATE "/CLIProxyAPI/data/codex-timezone-sync.state"

typedef struct { void *ptr; size_t len; } cliproxy_buffer;
typedef int (*host_call_fn)(void *, const char *, const uint8_t *, size_t, cliproxy_buffer *);
typedef void (*host_free_fn)(void *, size_t);
typedef struct { uint32_t abi_version; void *host_ctx; host_call_fn call; host_free_fn free_buffer; } cliproxy_host_api;
typedef int (*plugin_call_fn)(const char *, const uint8_t *, size_t, cliproxy_buffer *);
typedef void (*plugin_free_fn)(void *, size_t);
typedef void (*plugin_shutdown_fn)(void);
typedef struct { uint32_t abi_version; plugin_call_fn call; plugin_free_fn free_buffer; plugin_shutdown_fn shutdown; } cliproxy_plugin_api;

typedef struct { int start, end, parent, type; } tok;
enum { OBJECT=1, ARRAY=2, STRING=3, PRIMITIVE=4 };

static void reply(cliproxy_buffer *res, const char *msg) {
  if (!res || !msg) return;
  size_t n = strlen(msg);
  res->ptr = malloc(n ? n : 1);
  if (res->ptr) { memcpy(res->ptr, msg, n); res->len = n; }
}

// Parse only enough of JSON to identify top-level fields while preserving original bytes.
static int parse_json(const char *s, size_t n, tok *ts, int capacity) {
  int count=0, parent=-1;
  for (size_t i=0; i<n;) {
    unsigned char c=(unsigned char)s[i];
    if (isspace(c) || c==':' || c==',') { i++; continue; }
    if (c=='{' || c=='[') {
      if (count>=capacity) return -1;
      int id=count++; ts[id]=(tok){(int)i,-1,parent,c=='{'?OBJECT:ARRAY};
      parent=id; i++; continue;
    }
    if (c=='}' || c==']') {
      if (parent<0 || ts[parent].type!=(c=='}'?OBJECT:ARRAY)) return -1;
      ts[parent].end=(int)i+1; parent=ts[parent].parent; i++; continue;
    }
    if (c=='"') {
      size_t start=++i;
      for (;i<n;i++) {
        if (s[i]=='\\') { if (++i>=n) return -1; continue; }
        if (s[i]=='"') break;
        if ((unsigned char)s[i]<32) return -1;
      }
      if (i>=n || count>=capacity) return -1;
      ts[count++]=(tok){(int)start,(int)i,parent,STRING}; i++; continue;
    }
    if (c=='-' || isdigit(c) || c=='t' || c=='f' || c=='n') {
      size_t start=i;
      while (i<n && s[i]!=',' && s[i]!=']' && s[i]!='}' && !isspace((unsigned char)s[i])) i++;
      if (start==i || count>=capacity) return -1;
      ts[count++]=(tok){(int)start,(int)i,parent,PRIMITIVE}; continue;
    }
    return -1;
  }
  return parent<0 && count>0 ? count : -1;
}

static int next_index(const tok *ts, int nt, int id) {
  int end=ts[id].end; id++;
  while (id<nt && ts[id].start<end) id++;
  return id;
}
static int eq(const char *s, tok v, const char *name) {
  return v.type==STRING && (size_t)(v.end-v.start)==strlen(name) &&
         memcmp(s+v.start,name,strlen(name))==0;
}
static int get_field(const char *s, const tok *ts, int nt, int obj, const char *name) {
  if (obj<0 || ts[obj].type!=OBJECT) return -1;
  for (int id=obj+1;id<nt && ts[id].start<ts[obj].end;) {
    if (ts[id].parent!=obj || ts[id].type!=STRING) return -1;
    int val=id+1;
    if (val>=nt || ts[val].parent!=obj) return -1;
    if (eq(s,ts[id],name)) return val;
    id=next_index(ts,nt,val);
  }
  return -1;
}
static int string_is(const char *s, tok t, const char *expected) {
  size_t len=strlen(expected);
  return t.type==STRING && (size_t)(t.end-t.start)==len && !memcmp(s+t.start,expected,len);
}
static int contains(const char *s, size_t n, const char *needle) {
  size_t m=strlen(needle);
  if (m>n) return 0;
  for (size_t i=0;i+m<=n;i++) if (!memcmp(s+i,needle,m)) return 1;
  return 0;
}

static int b64val(unsigned char c) {
  if (c>='A'&&c<='Z') return c-'A';
  if (c>='a'&&c<='z') return c-'a'+26;
  if (c>='0'&&c<='9') return c-'0'+52;
  if (c=='+') return 62;
  if (c=='/') return 63;
  return -1;
}
static unsigned char *b64decode(const char *s, size_t n, size_t *out_n) {
  if (n%4 || n/4*3 > MAX_REQUEST) return NULL;
  size_t pad=(n && s[n-1]=='=')+(n>1 && s[n-2]=='=');
  size_t size=n/4*3-pad;
  unsigned char *out=malloc(size+1); if (!out) return NULL;
  size_t j=0;
  for (size_t i=0;i<n;i+=4) {
    int a=b64val(s[i]),b=b64val(s[i+1]);
    int c=s[i+2]=='='?0:b64val(s[i+2]);
    int d=s[i+3]=='='?0:b64val(s[i+3]);
    if (a<0||b<0||c<0||d<0 || (i+4<n && (s[i+2]=='='||s[i+3]=='=')) ||
       (s[i+2]=='='&&s[i+3]!='=')) {free(out);return NULL;}
    uint32_t x=((uint32_t)a<<18)|((uint32_t)b<<12)|((uint32_t)c<<6)|(uint32_t)d;
    if(j<size) out[j++]=(unsigned char)(x>>16);
    if(j<size) out[j++]=(unsigned char)(x>>8);
    if(j<size) out[j++]=(unsigned char)x;
  }
  out[size]=0; *out_n=size;return out;
}
static char *b64encode(const unsigned char *s, size_t n) {
  static const char abc[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if(n>(SIZE_MAX-4)/4*3) return NULL;
  size_t size=((n+2)/3)*4;
  char *out=malloc(size+1);if(!out)return NULL;
  size_t j=0;
  for(size_t i=0;i<n;i+=3) {
    size_t left=n-i;uint32_t a=s[i],b=left>1?s[i+1]:0,c=left>2?s[i+2]:0;
    out[j++]=abc[a>>2];out[j++]=abc[((a&3)<<4)|(b>>4)];
    out[j++]=left>1?abc[((b&15)<<2)|(c>>6)]:'=';
    out[j++]=left>2?abc[c&63]:'=';
  }
  out[j]=0;return out;
}

static int safe_field(const char *s, const char *allowed, size_t maxlen) {
  size_t n=strlen(s);if(n==0||n>maxlen)return 0;
  for(size_t i=0;i<n;i++)if(!strchr(allowed,s[i]))return 0;
  return 1;
}
static int timezone_context(char *dst,size_t cap) {
  char zone[80]="",date[18]="",abbr[18]="",offset[12]="";
  const char *allowed_zone="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-";
  const char *allowed_abbr="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-";
  const char *state=getenv("CODEX_TZ_STATE_FILE");
  if(!state || !*state)state=DEFAULT_STATE;
  struct stat st;
  // Fail open if the egress observation is missing or older than two hours.
  // This avoids incorrectly asserting a timezone after a proxy node switch.
  if(stat(state,&st)!=0 || time(NULL)-st.st_mtime>7200 || st.st_mtime>time(NULL)+60) return 0;
  FILE *fp=fopen(state,"r");
  if(!fp)return 0;
  char line[256];
  int loaded=0;
  if(fgets(line,sizeof(line),fp)) {
    char tmpz[80],tmpd[18],tmpa[18],tmpo[12];
    if(sscanf(line,"%79[^|]|%17[^|]|%17[^|]|%11[^\n]",tmpz,tmpd,tmpa,tmpo)==4 &&
      safe_field(tmpz,allowed_zone,79) &&
      safe_field(tmpd,"0123456789-",17) && strlen(tmpd)==10 &&
      safe_field(tmpa,allowed_abbr,17) && safe_field(tmpo,"0123456789:+-",11)) {
      strcpy(zone,tmpz);strcpy(date,tmpd);strcpy(abbr,tmpa);strcpy(offset,tmpo);
      loaded=1;
    }
  }
  fclose(fp);
  if(!loaded)return 0;
  derive_local_date(date,sizeof(date),offset);
  snprintf(dst,cap,TZ_MARKER "\nCurrent date: %s\nCurrent timezone: %s (%s, UTC%s).\nUse this date/timezone for environment context unless the user explicitly specifies another timezone.",date,zone,abbr,offset);
  return 1;
}

// splice a literal ASCII addition, preserving all other JSON bytes.
static char *splice(const char *src,size_t n,size_t where,size_t remove,const char *add,size_t m,size_t *out_n) {
  if(where>n||remove>n-where||n-remove>MAX_REQUEST-m)return NULL;
  size_t size=n-remove+m;
  char *dst=malloc(size+1);if(!dst)return NULL;
  memcpy(dst,src,where);memcpy(dst+where,add,m);
  memcpy(dst+where+m,src+where+remove,n-where-remove);
  dst[size]=0;*out_n=size;return dst;
}
static int normalize_existing_environments(const char *,size_t,char **,size_t *);
static char *rewrite_payload(const char *payload,size_t len,size_t *new_len) {
  if(len==0||len>MAX_REQUEST)return NULL;
  tok *ts=malloc(MAX_TOKENS*sizeof(tok));if(!ts)return NULL;
  int nt=parse_json(payload,len,ts,MAX_TOKENS);
  if(nt<1||ts[0].type!=OBJECT){free(ts);return NULL;}
  char context[280];if(!timezone_context(context,sizeof(context))){free(ts);return NULL;}
  char *normalized=NULL;int env_status=normalize_existing_environments(payload,len,&normalized,new_len);
  if(env_status!=0){free(ts);return env_status==1?normalized:NULL;}
  char *answer=NULL;
  int instructions=get_field(payload,ts,nt,0,"instructions");
  int input=get_field(payload,ts,nt,0,"input");
  int messages=get_field(payload,ts,nt,0,"messages");
  if(instructions>=0&&ts[instructions].type==STRING) {
    int start=ts[instructions].start,end=ts[instructions].end;
    if(!contains(payload+start,(size_t)(end-start),TZ_MARKER)) {
      size_t n=strlen(context)+10;char *add=malloc(n);
      if(add){snprintf(add,n,"\n\n%s",context);
        // context has literal newlines; convert them to JSON escapes.
        size_t m=strlen(add), outcap=m*2+1;char *escaped=malloc(outcap);
        if(escaped){size_t k=0;for(size_t i=0;i<m;i++){
          if(add[i]=='\n'){escaped[k++]='\\';escaped[k++]='n';}
          else if(add[i]=='"'||add[i]=='\\'){escaped[k++]='\\';escaped[k++]=add[i];}
          else escaped[k++]=add[i];}
          answer=splice(payload,len,(size_t)end,0,escaped,k,new_len);free(escaped);
        }free(add);
      }
    }
  } else if(instructions>=0 && ts[instructions].type==PRIMITIVE &&
            (size_t)(ts[instructions].end-ts[instructions].start)==4 &&
            !memcmp(payload+ts[instructions].start,"null",4)) {
    char *escaped=malloc(strlen(context)*2+4);
    if(escaped){size_t k=0;escaped[k++]='"';
      for(const char *c=context;*c;c++){if(*c=='\n'){escaped[k++]='\\';escaped[k++]='n';}
      else if(*c=='"'||*c=='\\'){escaped[k++]='\\';escaped[k++]=*c;}
      else escaped[k++]=*c;}
      escaped[k++]='"';
      answer=splice(payload,len,(size_t)ts[instructions].start,4,escaped,k,new_len);free(escaped);
    }
  } else if(instructions>=0) {
    // Do not insert duplicate instructions when an unexpected type exists.
  } else if(input>=0 && ts[input].type==ARRAY) {
    // Native Responses input; insert top-level instructions without modifying messages.
    char *escaped=malloc(strlen(context)*2+32);
    if(escaped){size_t k=0;memcpy(escaped,"\"instructions\":\"",16);k=16;
      for(const char *c=context;*c;c++){if(*c=='\n'){escaped[k++]='\\';escaped[k++]='n';}
      else if(*c=='"'||*c=='\\'){escaped[k++]='\\';escaped[k++]=*c;}
      else escaped[k++]=*c;}
      escaped[k++]='"';escaped[k++]=',';
      answer=splice(payload,len,(size_t)ts[0].start+1,0,escaped,k,new_len);free(escaped);
    }
  } else if(messages>=0 && ts[messages].type==ARRAY) {
    // Chat-completions routed to Codex: prepend one developer message.
    char *escaped=malloc(strlen(context)*2+100);
    if(escaped){size_t k=0;const char *prefix="{\"role\":\"developer\",\"content\":\"";
      memcpy(escaped,prefix,strlen(prefix));k=strlen(prefix);
      for(const char *c=context;*c;c++){if(*c=='\n'){escaped[k++]='\\';escaped[k++]='n';}
      else if(*c=='"'||*c=='\\'){escaped[k++]='\\';escaped[k++]=*c;}
      else escaped[k++]=*c;}
      escaped[k++]='"';escaped[k++]='}';
      // Check whether the messages array is empty; comma only when nonempty.
      size_t at=(size_t)ts[messages].start+1;
      size_t check=at;while(check<len&&isspace((unsigned char)payload[check]))check++;
      if(check<len&&payload[check]!=']')escaped[k++]=',';
      answer=splice(payload,len,at,0,escaped,k,new_len);free(escaped);
    }
  }
  free(ts);return answer;
}

#include "settings.inc"
#include "timezone_normalize.inc"

static void http_response(cliproxy_buffer *out,int code,const char *kind,const char *body) {
  char *b64=b64encode((const unsigned char *)body,strlen(body));
  if(!b64)return;
  size_t cap=strlen(b64)+220;
  char *msg=malloc(cap);
  if(msg) {
    snprintf(msg,cap,"{\"ok\":true,\"result\":{\"StatusCode\":%d,\"Headers\":{\"Content-Type\":[\"%s; charset=utf-8\"],\"Cache-Control\":[\"no-store\"]},\"Body\":\"%s\"}}",code,kind,b64);
    reply(out,msg);free(msg);
  }
  free(b64);
}

#include "events.inc"
#include "model_monitor.inc"
#include "stats.inc"
#include "refresh_bridge.inc"
#include "failures.inc"

static void handle_clear_activity(cliproxy_buffer *out) {
  // Clear only the activity stream and volatile model observations.
  int fd=open(log_path(),O_WRONLY|O_CREAT|O_NOFOLLOW,0600);
  if(fd<0) {http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"unable to open events\"}");return;}
  struct stat st;
  int ok=flock(fd,LOCK_EX)==0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && ftruncate(fd,0)==0;
  if(ok)fsync(fd);
  flock(fd,LOCK_UN);close(fd);
  if(!ok) {http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"unable to clear events\"}");return;}
  pthread_mutex_lock(&model_mutex);
  memset(model_history,0,sizeof(model_history));
  model_head=0;model_used=0;
  pthread_mutex_unlock(&model_mutex);
  http_response(out,200,"application/json","{\"ok\":true}");
}

static void intercept(const uint8_t *request,size_t len,cliproxy_buffer *response) {
  if(!request||!len||len>MAX_REQUEST){reply(response,"{\"ok\":true,\"result\":{}}");return;}
  const char *src=(const char *)request;
  tok *ts=malloc(MAX_TOKENS*sizeof(tok));if(!ts){reply(response,"{\"ok\":true,\"result\":{}}");return;}
  int nt=parse_json(src,len,ts,MAX_TOKENS);
  if(nt<1||ts[0].type!=OBJECT){free(ts);reply(response,"{\"ok\":true,\"result\":{}}");return;}
  int obj=0;
  int wrapped=get_field(src,ts,nt,obj,"request");
  if(wrapped>=0&&ts[wrapped].type==OBJECT)obj=wrapped;
  int to=get_field(src,ts,nt,obj,"ToFormat");
  if(to<0)to=get_field(src,ts,nt,obj,"to_format");
  int body=get_field(src,ts,nt,obj,"Body");
  if(body<0)body=get_field(src,ts,nt,obj,"body");
  if(to<0||!string_is(src,ts[to],"codex")) {
    free(ts);reply(response,"{\"ok\":true,\"result\":{}}");return;
  }
  if(body<0||ts[body].type!=STRING) {
    if(load_settings().enabled) {
      stats_update(0,1,0,NULL,NULL);
      persist_failure("invalid_body","",0);
    }
    free(ts);reply(response,"{\"ok\":true,\"result\":{}}");return;
  }
  int sync_enabled=load_settings().enabled;
  const char *encoded=src+ts[body].start;size_t encoded_len=(size_t)(ts[body].end-ts[body].start);
  size_t payload_len=0;unsigned char *payload=b64decode(encoded,encoded_len,&payload_len);
  char before_zone[80]="";
  if(payload){extract_original_zone((const char *)payload,payload_len,before_zone);monitor_request(src,ts,nt,obj,(const char *)payload,payload_len);}
  free(ts);
  if(!sync_enabled || !payload) {
    if(sync_enabled) {
      stats_update(0,1,0,NULL,NULL);
      persist_failure("invalid_base64",encoded,encoded_len);
    }
    free(payload);reply(response,"{\"ok\":true,\"result\":{}}");return;
  }
  int already=contains((char *)payload,payload_len,TZ_MARKER);
  size_t next_len=0;char *rewritten=rewrite_payload((char *)payload,payload_len,&next_len);
  if(!rewritten) {
    if(!already) {
      sync_state current;
      stats_update(0,1,0,NULL,NULL);
      persist_failure(load_state(&current)?"rewrite_failed":"state_unavailable",
        (const char *)payload,payload_len);
    }
    free(payload);log_event_detail("skipped",before_zone);
    reply(response,"{\"ok\":true,\"result\":{}}");return;
  }
  char *next_b64=b64encode((unsigned char *)rewritten,next_len);free(rewritten);
  if(!next_b64){
    stats_update(0,1,0,NULL,NULL);persist_failure("encode_failed",(const char *)payload,payload_len);
    free(payload);reply(response,"{\"ok\":true,\"result\":{}}");return;
  }
  size_t n=strlen(next_b64)+64;char *result=malloc(n);
  if(result) {
    snprintf(result,n,"{\"ok\":true,\"result\":{\"Body\":\"%s\"}}",next_b64);
    reply(response,result);
    if(response->ptr){stats_update(1,0,0,NULL,NULL);log_event_detail("rewritten",before_zone);}
    else {
      stats_update(0,1,0,NULL,NULL);persist_failure("response_failed",(const char *)payload,payload_len);
    }
    free(result);
  } else {
    stats_update(0,1,0,NULL,NULL);persist_failure("response_failed",(const char *)payload,payload_len);
  }
  free(payload);
  free(next_b64);
}

// CPA exposes menu resources without auth. Never place credentials in this page.
static void handle_status(cliproxy_buffer *out) {
  sync_settings cfg=load_settings();sync_state state;
  int available=load_state(&state);
  char json[768];
  snprintf(json,sizeof(json),
    "{\"ok\":true,\"available\":%s,\"timezone\":\"%s\",\"date\":\"%s\",\"abbreviation\":\"%s\",\"offset\":\"%s\",\"ip\":\"%s\",\"age_seconds\":%ld,\"settings\":{\"enabled\":%s,\"logging\":%s,\"debug\":%s}}",
    available?"true":"false",available?state.zone:"",available?state.date:"",available?state.abbr:"",available?state.offset:"",available?state.ip:"",available?state.age:-1L,
    cfg.enabled?"true":"false",cfg.logging?"true":"false",cfg.debug?"true":"false");
  http_response(out,200,"application/json",json);
}
static void handle_selftest(cliproxy_buffer *out) {
  if(!load_settings().enabled) {
    http_response(out,409,"application/json","{\"ok\":false,\"reason\":\"同步功能已关闭\"}");return;
  }
  sync_state state;
  if(!load_state(&state)) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"出口时区状态缺失或已过期\"}");return;
  }
  const char *sample="{\"model\":\"codex-selftest\",\"input\":[]}";
  size_t out_len=0;
  char *converted=rewrite_payload(sample,strlen(sample),&out_len);
  int ok=converted && contains(converted,out_len,TZ_MARKER) && contains(converted,out_len,state.zone);
  free(converted);
  if(!ok) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"请求重写自检失败\"}");return;
  }
  char json[160];
  snprintf(json,sizeof(json),"{\"ok\":true,\"timezone\":\"%s\",\"mode\":\"dry-run\"}",state.zone);
  http_response(out,200,"application/json",json);
}
static void handle_settings(const char *raw,const tok *ts,int body,cliproxy_buffer *out) {
  if(body<0 || ts[body].type!=STRING) {
    http_response(out,400,"application/json","{\"ok\":false,\"error\":\"missing JSON body\"}");return;
  }
  size_t n=0;
  const char *data=raw+ts[body].start;
  size_t encoded_len=(size_t)(ts[body].end-ts[body].start);
  unsigned char *decoded=b64decode(data,encoded_len,&n);
  sync_settings cfg=load_settings();
  if(!decoded || n>2048 || !parse_settings_body((char *)decoded,n,&cfg)) {
    free(decoded);
    http_response(out,400,"application/json","{\"ok\":false,\"error\":\"invalid settings\"}");return;
  }
  free(decoded);
  if(!save_settings(cfg)) {
    http_response(out,500,"application/json","{\"ok\":false,\"error\":\"unable to save configuration\"}");return;
  }
  handle_status(out);
}
static void management_page(const uint8_t *request,size_t request_len,cliproxy_buffer *response) {
  if (!request || !request_len || request_len>MAX_REQUEST) {
    reply(response,"{\"ok\":false,\"error\":{\"code\":\"invalid_request\"}}");return;
  }
  const char *raw=(const char *)request;
  tok *ts=malloc(MAX_TOKENS*sizeof(tok));
  if (!ts) return;
  int nt=parse_json(raw,request_len,ts,MAX_TOKENS);
  int obj=0;
  if (nt<1 || ts[0].type!=OBJECT) {free(ts);return;}
  int wrap=get_field(raw,ts,nt,obj,"request");
  if (wrap>=0 && ts[wrap].type==OBJECT)obj=wrap;
  int path=get_field(raw,ts,nt,obj,"Path");
  if (path<0)path=get_field(raw,ts,nt,obj,"path");
  int verb=get_field(raw,ts,nt,obj,"Method");
  if (verb<0)verb=get_field(raw,ts,nt,obj,"method");
  int body=get_field(raw,ts,nt,obj,"Body");
  if(body<0)body=get_field(raw,ts,nt,obj,"body");
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"GET") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/status")) {
    free(ts);handle_status(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"GET") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/models")) {
    free(ts);handle_model_records(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"GET") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/logs")) {
    free(ts);handle_logs(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"GET") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/stats")) {
    free(ts);handle_stats(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"POST") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/stats/clear")) {
    free(ts);handle_clear_stats(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"POST") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/activity/clear")) {
    free(ts);handle_clear_activity(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"POST") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/egress/refresh")) {
    free(ts);handle_probe_egress(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"GET") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/failures")) {
    free(ts);handle_failures(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"POST") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/failures/view")) {
    handle_failure_view(raw,ts,body,response);free(ts);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"POST") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/failures/clear")) {
    free(ts);handle_clear_failures(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"POST") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/selftest")) {
    free(ts);handle_selftest(response);return;
  }
  if(path>=0 && verb>=0 && string_is(raw,ts[verb],"PUT") &&
    string_is(raw,ts[path],"/v0/management/plugins/codex-timezone-sync/settings")) {
    handle_settings(raw,ts,body,response);free(ts);return;
  }
  int valid=(path>=0 && verb>=0 && string_is(raw,ts[verb],"GET") &&
             string_is(raw,ts[path],"/v0/resource/plugins/codex-timezone-sync/panel"));
  free(ts);
  if (!valid) {
    reply(response,"{\"ok\":true,\"result\":{\"StatusCode\":404,\"Headers\":{\"Content-Type\":[\"text/plain; charset=utf-8\"]},\"Body\":\"Tm90IGZvdW5k\"}}");
    return;
  }
  char *encoded=b64encode(dashboard_html,dashboard_html_size);
  if (!encoded)return;
  size_t cap=strlen(encoded)+200;
  char *json=malloc(cap);
  if(json) {
    snprintf(json,cap,"{\"ok\":true,\"result\":{\"StatusCode\":200,\"Headers\":{\"Content-Type\":[\"text/html; charset=utf-8\"],\"Cache-Control\":[\"no-store\"],\"X-Content-Type-Options\":[\"nosniff\"]},\"Body\":\"%s\"}}",encoded);
    reply(response,json);
    free(json);
  }
  free(encoded);
}

static int plugin_call(const char *method,const uint8_t *request,size_t request_len,cliproxy_buffer *response) {
  if(response){response->ptr=NULL;response->len=0;}
  if(!method||!response)return 1;
  if(!strcmp(method,"plugin.register")||!strcmp(method,"plugin.reconfigure")) {
    reply(response,"{\"ok\":true,\"result\":{\"schema_version\":1,\"metadata\":{\"Name\":\"Codex Timezone Sync\",\"Version\":\"0.5.0\",\"Author\":\"LonelyHerbivore\",\"GitHubRepository\":\"https://github.com/LonelyHerbivore/codex-timezone-sync\",\"ConfigFields\":[]},\"capabilities\":{\"request_interceptor\":true,\"management_api\":true,\"response_interceptor\":true,\"response_stream_interceptor\":true,\"websocket_response_observer\":true}}}");
    return 0;
  }
  if(!strcmp(method,"management.register")) {
    reply(response,
      "{\"ok\":true,\"result\":{\"Routes\":["
      "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync/status\"},"
      "{\"Method\":\"PUT\",\"Path\":\"/plugins/codex-timezone-sync/settings\"},"
      "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync/logs\"},"
      "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync/models\"},"
      "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync/stats\"},"
      "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync/stats/clear\"},"
      "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync/activity/clear\"},"
      "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync/egress/refresh\"},"
      "{\"Method\":\"GET\",\"Path\":\"/plugins/codex-timezone-sync/failures\"},"
      "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync/failures/view\"},"
      "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync/failures/clear\"},"
      "{\"Method\":\"POST\",\"Path\":\"/plugins/codex-timezone-sync/selftest\"}],"
      "\"Resources\":[{\"Path\":\"/panel\",\"Menu\":\"Codex Timezone Sync\",\"Description\":\"Codex 请求时区同步状态与控制\"}]}}");
    return 0;
  }
  if(!strcmp(method,"management.handle")) {management_page(request,request_len,response);return 0;}
  if(!strcmp(method,"response.intercept_after") ||
     !strcmp(method,"response.intercept_stream_chunk") ||
     !strcmp(method,"websocket.response_event")) {
    monitor_response(method,request,request_len);
    reply(response,"{\"ok\":true,\"result\":{}}");return 0;
  }
  if(!strcmp(method,"request.intercept_after")) {intercept(request,request_len,response);return 0;}
  if(!strcmp(method,"request.intercept_before")) {reply(response,"{\"ok\":true,\"result\":{}}");return 0;}
  reply(response,"{\"ok\":false,\"error\":{\"code\":\"unknown_method\",\"message\":\"unsupported method\"}}");return 0;
}
static void plugin_free(void *p,size_t n){(void)n;free(p);}
static void plugin_shutdown(void){}
__attribute__((visibility("default"))) int cliproxy_plugin_init(const cliproxy_host_api *host,cliproxy_plugin_api *plugin){
  (void)host;
  if(!plugin)return 1;
  plugin->abi_version=ABI_VERSION;
  plugin->call=plugin_call;plugin->free_buffer=plugin_free;plugin->shutdown=plugin_shutdown;
  return 0;
}
