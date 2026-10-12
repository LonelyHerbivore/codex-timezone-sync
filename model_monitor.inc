// Read-only upstream model consistency monitor. Never stores prompts, bodies, headers or tokens.
#include <pthread.h>
#define MODEL_HISTORY 128
#define MODEL_PROBE_MAX (1024u * 1024u)
typedef struct {
  char id[100],requested[96],routed[96],upstream[96],channel[16],timezone_before[80],timezone_after[80];
  long long timestamp;
  int checked; // 0 unknown, 1 same, 2 mismatch
} model_record;
static model_record model_history[MODEL_HISTORY];
static int model_head=0,model_used=0;
static pthread_mutex_t model_mutex=PTHREAD_MUTEX_INITIALIZER;
static int safe_id(const char *s) {
  return safe_field(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:",99);
}
static int safe_model(const char *s) {
  return safe_field(s,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:-",95);
}
static char *field_string(const char *json,const tok *tokens,int count,int object,
                          const char *upper,const char *lower) {
  int i=get_field(json,tokens,count,object,upper);
  if(i<0 && lower)i=get_field(json,tokens,count,object,lower);
  return i>=0 && tokens[i].type==STRING?json_unescape(json,tokens[i]):NULL;
}
static int find_model_record(const char *id) {
  for(int i=0;i<model_used;i++) {
    int idx=(model_head+MODEL_HISTORY-1-i)%MODEL_HISTORY;
    if(strcmp(model_history[idx].id,id)==0)return idx;
  }
  return -1;
}
static void extract_original_zone(const char *payload,size_t len,char zone[80]) {
  zone[0]=0;
  if(!payload || len>MODEL_PROBE_MAX)return;
  tok *tokens=malloc(8192*sizeof(tok));if(!tokens)return;
  int n=parse_json(payload,len,tokens,8192);
  if(n>0)for(int i=0;i<n;i++) {
    if(tokens[i].type!=STRING)continue;
    char *str=json_unescape(payload,tokens[i]);if(!str)continue;
    const char *env=find_ci(str,"<environment_context");
    if(env) {
      const char *end=find_ci(env,"</environment_context");
      const char *tz=find_ci(env,"<timezone>");
      if(end && tz && tz<end) {
        tz+=strlen("<timezone>");const char *close=find_ci(tz,"</timezone>");
        if(close && close<end && (size_t)(close-tz)<80) {
          char val[80];memcpy(val,tz,(size_t)(close-tz));val[close-tz]=0;
          if(safe_field(val,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_+-",79)) {
            memcpy(zone,val,strlen(val)+1);free(str);break;
          }
        }
      }
    }
    free(str);
  }
  free(tokens);
}
static void monitor_new_request(const char *id,const char *requested,const char *routed,const char *before) {
  if(!id||!safe_id(id))return;
  if((!requested||!safe_model(requested))&&(!routed||!safe_model(routed)))return;
  pthread_mutex_lock(&model_mutex);
  int idx=find_model_record(id);
  if(idx<0){
    idx=model_head;
    model_head=(model_head+1)%MODEL_HISTORY;
    if(model_used<MODEL_HISTORY)model_used++;
  }
  model_record *r=&model_history[idx];
  memset(r,0,sizeof(*r));
  snprintf(r->id,sizeof(r->id),"%s",id);
  snprintf(r->requested,sizeof(r->requested),"%s",requested&&safe_model(requested)?requested:routed);
  snprintf(r->routed,sizeof(r->routed),"%s",routed&&safe_model(routed)?routed:"");
  r->timestamp=(long long)time(NULL);
  snprintf(r->timezone_before,sizeof(r->timezone_before),"%s",before&&*before?before:"unknown");
  sync_state state;if(load_state(&state))snprintf(r->timezone_after,sizeof(r->timezone_after),"%s",state.zone);
  pthread_mutex_unlock(&model_mutex);
}
static void monitor_request(const char *src,const tok *ts,int nt,int obj,const char *body,size_t len) {
  char *id=field_string(src,ts,nt,obj,"RequestID","request_id");
  if(!id)id=field_string(src,ts,nt,obj,"TraceID","trace_id");
  char *requested=field_string(src,ts,nt,obj,"RequestedModel","requested_model");
  char *routed=field_string(src,ts,nt,obj,"Model","model");
  if(!routed && body && len>0&&len<=MODEL_PROBE_MAX) {
    tok *t=malloc(2048*sizeof(tok));
    if(t) {
      int n=parse_json(body,len,t,2048);
      if(n>0&&t[0].type==OBJECT)
        routed=field_string(body,t,n,0,"model",NULL);
      free(t);
    }
  }
  char before[80]="";extract_original_zone(body,len,before);
  monitor_new_request(id,requested,routed,before);
  free(id);free(requested);free(routed);
}
static int model_from_json(const char *str,size_t len,char out[96]) {
  if(len==0||len>MODEL_PROBE_MAX)return 0;
  tok *ts=malloc(8192*sizeof(tok));if(!ts)return 0;
  int nt=parse_json(str,len,ts,8192),idx=-1;
  if(nt>0&&ts[0].type==OBJECT) {
    int nested=get_field(str,ts,nt,0,"response");
    if(nested>=0&&ts[nested].type==OBJECT)idx=get_field(str,ts,nt,nested,"model");
    if(idx<0){
      nested=get_field(str,ts,nt,0,"message");
      if(nested>=0&&ts[nested].type==OBJECT)idx=get_field(str,ts,nt,nested,"model");
    }
    if(idx<0) {
      int type=get_field(str,ts,nt,0,"type");
      int object=get_field(str,ts,nt,0,"object");
      if(type>=0||object>=0)idx=get_field(str,ts,nt,0,"model");
    }
  }
  int ok=0;
  if(idx>=0&&ts[idx].type==STRING){
    char *value=json_unescape(str,ts[idx]);
    if(value&&safe_model(value)){snprintf(out,96,"%s",value);ok=1;}
    free(value);
  }
  free(ts);return ok;
}
static int model_from_payload(const char *payload,size_t len,char out[96]) {
  if(len==0||len>MODEL_PROBE_MAX)return 0;
  while(len>0&&isspace((unsigned char)*payload)){payload++;len--;}
  while(len>0&&isspace((unsigned char)payload[len-1]))len--;
  if(len>1&&payload[0]=='{'&&model_from_json(payload,len,out))return 1;
  // Parse bounded SSE "data:" frames. No substrings from tool output are trusted.
  size_t pos=0,attempts=0;
  while(pos<len && attempts<32) {
    size_t end=pos;
    while(end<len&&payload[end]!='\n')end++;
    const char *line=payload+pos;
    size_t n=end-pos;
    while(n>0&&isspace((unsigned char)*line)){line++;n--;}
    if(n>=5&&!memcmp(line,"data:",5)) {
      line+=5;n-=5;
      while(n>0&&isspace((unsigned char)*line)){line++;n--;}
      if(n>0&&line[0]=='{'&&model_from_json(line,n,out))return 1;
      attempts++;
    }
    pos=end+1;
  }
  return 0;
}
static void monitor_observation(const char *id,const char *upstream,const char *channel) {
  if(!id||!safe_id(id)||!safe_model(upstream))return;
  int status=0;
  pthread_mutex_lock(&model_mutex);
  int idx=find_model_record(id);
  if(idx>=0) {
    model_record *r=&model_history[idx];
    // Once matched, later stream chunks without a model do not change it.
    snprintf(r->upstream,sizeof(r->upstream),"%s",upstream);
    snprintf(r->channel,sizeof(r->channel),"%s",channel);
    r->checked=(!strcasecmp(r->requested,upstream))?1:2;
    status=r->checked;
  }
  pthread_mutex_unlock(&model_mutex);
  if(status==1)log_event("model_match");
  if(status==2)log_event("model_mismatch");
}
static void monitor_response(const char *method,const uint8_t *bytes,size_t len) {
  if(!bytes||len==0||len>MAX_REQUEST)return;
  const char *s=(const char *)bytes;
  tok *ts=malloc(8192*sizeof(tok));if(!ts)return;
  int n=parse_json(s,len,ts,8192);
  if(n<=0||ts[0].type!=OBJECT){free(ts);return;}
  int obj=0,wrap=get_field(s,ts,n,obj,"request");
  if(wrap>=0&&ts[wrap].type==OBJECT)obj=wrap;
  int chunk=get_field(s,ts,n,obj,"ChunkIndex");
  if(chunk<0)chunk=get_field(s,ts,n,obj,"chunk_index");
  if(chunk>=0&&ts[chunk].type==PRIMITIVE &&
     (size_t)(ts[chunk].end-ts[chunk].start)==2 &&
     !memcmp(s+ts[chunk].start,"-1",2)){free(ts);return;}
  char *id=field_string(s,ts,n,obj,"RequestID","request_id");
  if(!id)id=field_string(s,ts,n,obj,"TraceID","trace_id");
  int b=get_field(s,ts,n,obj,!strcmp(method,"websocket.response_event")?"Payload":"Body");
  if(b<0)b=get_field(s,ts,n,obj,!strcmp(method,"websocket.response_event")?"payload":"body");
  if(id && b>=0&&ts[b].type==STRING){
    size_t byte_len=0;
    unsigned char *decoded=b64decode(s+ts[b].start,(size_t)(ts[b].end-ts[b].start),&byte_len);
    if(decoded && byte_len<=MODEL_PROBE_MAX) {
      char model[96]="";
      if(model_from_payload((char *)decoded,byte_len,model)) {
        const char *channel=!strcmp(method,"websocket.response_event")?"websocket":
               !strcmp(method,"response.intercept_stream_chunk")?"sse":"response";
        monitor_observation(id,model,channel);
      }
    }
    free(decoded);
  }
  free(id);free(ts);
}
static void handle_model_records(cliproxy_buffer *out) {
  char json[36000];size_t used=0;int matched=0,mismatched=0,unknown=0;
  pthread_mutex_lock(&model_mutex);
  for(int i=0;i<model_used;i++){
    const model_record *r=&model_history[(model_head+MODEL_HISTORY-1-i)%MODEL_HISTORY];
    if(r->checked==1)matched++;
    else if(r->checked==2)mismatched++;
    else unknown++;
  }
  used+=(size_t)snprintf(json,sizeof(json),
     "{\"ok\":true,\"matched\":%d,\"mismatched\":%d,\"unknown\":%d,\"records\":[",
     matched,mismatched,unknown);
  for(int i=0;i<model_used && i<60;i++) {
    const model_record *r=&model_history[(model_head+MODEL_HISTORY-1-i)%MODEL_HISTORY];
    int written=snprintf(json+used,sizeof(json)-used,
      "%s{\"timestamp\":%lld,\"request_id\":\"%s\",\"requested_model\":\"%s\","
      "\"routed_model\":\"%s\",\"upstream_model\":\"%s\",\"status\":\"%s\",\"channel\":\"%s\",\"timezone_before\":\"%s\",\"timezone_after\":\"%s\"}",
      i?",":"",r->timestamp,r->id,r->requested,r->routed,r->upstream,
      r->checked==1?"matched":r->checked==2?"mismatched":"unknown",r->channel,r->timezone_before,r->timezone_after);
    if(written<0||(size_t)written>=sizeof(json)-used-3)break;
    used+=(size_t)written;
  }
  pthread_mutex_unlock(&model_mutex);
  snprintf(json+used,sizeof(json)-used,"]}");
  http_response(out,200,"application/json",json);
}
