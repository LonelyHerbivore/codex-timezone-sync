// Only rewrite semantic environment context fields. All other JSON bytes are preserved.
#include <strings.h>
#define ENV_FIELDS_LIMIT 512
static const char *find_ci(const char *hay,const char *needle) {
  size_t n=strlen(needle);
  for(const char *p=hay;*p;p++)if(!strncasecmp(p,needle,n))return p;
  return NULL;
}
static char *text_replace(char *old,size_t pos,size_t remove,const char *add,size_t addlen) {
  size_t n=strlen(old),newlen=0;
  char *next=splice(old,n,pos,remove,add,addlen,&newlen);
  if(next)free(old);
  return next;
}
static int hexval(char c) {
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  if(c>='A'&&c<='F')return c-'A'+10;
  return -1;
}
static int read_u16(const char *s,uint32_t *v) {
  *v=0;
  for(int j=0;j<4;j++){int x=hexval(s[j]);if(x<0)return 0;*v=(*v<<4)|(uint32_t)x;}
  return 1;
}
static char *json_unescape(const char *raw,tok t) {
  size_t n=(size_t)(t.end-t.start);
  char *buf=malloc(n+1);if(!buf)return NULL;
  size_t w=0;
  const char *s=raw+t.start;
  for(size_t i=0;i<n;i++) {
    unsigned char x=(unsigned char)s[i];
    if(x!='\\'){if(x==0){free(buf);return NULL;}buf[w++]=(char)x;continue;}
    if(++i>=n){free(buf);return NULL;}
    char a=s[i];const char *simple="\"\\/bfnrt";
    const char *mapping="\"\\/\b\f\n\r\t";
    const char *hit=strchr(simple,a);
    if(hit){buf[w++]=mapping[hit-simple];continue;}
    if(a!='u'||i+4>=n){free(buf);return NULL;}
    uint32_t cp=0;
    if(!read_u16(s+i+1,&cp)){free(buf);return NULL;}i+=4;
    if(cp>=0xD800&&cp<=0xDBFF){
      uint32_t lo=0;
      if(i+6>=n||s[i+1]!='\\'||s[i+2]!='u'||!read_u16(s+i+3,&lo)||
         lo<0xDC00||lo>0xDFFF){free(buf);return NULL;}
      cp=0x10000+((cp-0xD800)<<10)+(lo-0xDC00);i+=6;
    }else if(cp>=0xDC00&&cp<=0xDFFF){free(buf);return NULL;}
    if(cp==0){free(buf);return NULL;}
    if(cp<0x80)buf[w++]=(char)cp;
    else if(cp<0x800){buf[w++]=(char)(0xC0|(cp>>6));buf[w++]=(char)(0x80|(cp&63));}
    else if(cp<0x10000){buf[w++]=(char)(0xE0|(cp>>12));buf[w++]=(char)(0x80|((cp>>6)&63));buf[w++]=(char)(0x80|(cp&63));}
    else {buf[w++]=(char)(0xF0|(cp>>18));buf[w++]=(char)(0x80|((cp>>12)&63));buf[w++]=(char)(0x80|((cp>>6)&63));buf[w++]=(char)(0x80|(cp&63));}
  }
  buf[w]=0;return buf;
}
static char *json_escape_text(const char *s) {
  size_t n=strlen(s);if(n>(MAX_REQUEST-3)/6)return NULL;
  char *out=malloc(n*6+3);if(!out)return NULL;
  size_t w=0;out[w++]='"';
  const char hex[]="0123456789abcdef";
  for(size_t i=0;i<n;i++){
    unsigned char x=(unsigned char)s[i];
    if(x=='"'||x=='\\'){out[w++]='\\';out[w++]=(char)x;}
    else if(x=='\n'){out[w++]='\\';out[w++]='n';}
    else if(x=='\r'){out[w++]='\\';out[w++]='r';}
    else if(x=='\t'){out[w++]='\\';out[w++]='t';}
    else if(x<32){out[w++]='\\';out[w++]='u';out[w++]='0';out[w++]='0';out[w++]=hex[x>>4];out[w++]=hex[x&15];}
    else out[w++]=(char)x;
  }
  out[w++]='"';out[w]=0;return out;
}
static char *patch_env_field(char *block,const char *field,const char *value) {
  char start[60],end[65],insertion[150];
  snprintf(start,sizeof(start),"<%s>",field);
  snprintf(end,sizeof(end),"</%s>",field);
  size_t cursor=0;int found=0;
  while(block){
    const char *open=find_ci(block+cursor,start);
    if(!open)break;
    const char *close=find_ci(open+strlen(start),end);
    if(!close)break;
    found=1;
    size_t at=(size_t)(open-block)+strlen(start);
    size_t length=(size_t)(close-(block+at));
    if(length!=strlen(value)||memcmp(block+at,value,length)!=0){
      block=text_replace(block,at,length,value,strlen(value));
      if(!block)return NULL;
    }
    cursor=at+strlen(value)+strlen(end);
  }
  if(!block)return NULL;
  if(!found){
    char self[60];snprintf(self,sizeof(self),"<%s/>",field);
    const char *selfpos=find_ci(block,self);
    if(selfpos){
      snprintf(insertion,sizeof(insertion),"<%s>%s</%s>",field,value,field);
      block=text_replace(block,(size_t)(selfpos-block),strlen(self),insertion,strlen(insertion));
    }else{
      const char *closing=find_ci(block,"</environment_context");
      if(!closing)return block;
      snprintf(insertion,sizeof(insertion),"<%s>%s</%s>\n",field,value,field);
      block=text_replace(block,(size_t)(closing-block),0,insertion,strlen(insertion));
    }
  }
  return block;
}
static char *patch_environment_text(char *text,const char *zone,const char *date,int *found) {
  size_t cursor=0;
  *found=0;
  while(text){
    const char *opening=find_ci(text+cursor,"<environment_context");
    if(!opening)break;
    const char *openend=strchr(opening,'>');
    if(!openend)break;
    const char *ending=find_ci(openend+1,"</environment_context");
    if(!ending)break;
    const char *endend=strchr(ending,'>');
    if(!endend)break;
    *found=1;
    size_t offset=(size_t)(opening-text),count=(size_t)(endend+1-opening);
    char *block=strndup(opening,count);
    if(!block){free(text);return NULL;}
    block=patch_env_field(block,"timezone",zone);
    if(block)block=patch_env_field(block,"current_date",date);
    if(!block){free(text);return NULL;}
    if(strlen(block)!=count||memcmp(opening,block,count)!=0){
      char *next=text_replace(text,offset,count,block,strlen(block));
      if(!next){free(block);free(text);return NULL;}
      text=next;
    }
    cursor=offset+strlen(block);
    free(block);
  }
  return text;
}
static void push_field(const tok *ts,int index,int *positions,int *count) {
  if(index<0||ts[index].type!=STRING||*count>=ENV_FIELDS_LIMIT)return;
  for(int j=0;j<*count;j++)if(positions[j]==index)return;
  positions[(*count)++]=index;
}
static void collect_contents(const char *s,const tok *ts,int nt,int v,int *ids,int *count) {
  if(v<0)return;
  if(ts[v].type==STRING){push_field(ts,v,ids,count);return;}
  if(ts[v].type!=ARRAY)return;
  for(int i=v+1;i<nt&&ts[i].start<ts[v].end;i=next_index(ts,nt,i)){
    if(ts[i].type!=OBJECT)continue;
    int type=get_field(s,ts,nt,i,"type");
    if(type<0||(!string_is(s,ts[type],"text")&&!string_is(s,ts[type],"input_text")))continue;
    push_field(ts,get_field(s,ts,nt,i,"text"),ids,count);
  }
}
static void collect_messages(const char *s,const tok *ts,int nt,int list,int *ids,int *count) {
  if(list<0||ts[list].type!=ARRAY)return;
  for(int i=list+1;i<nt&&ts[i].start<ts[list].end;i=next_index(ts,nt,i)){
    if(ts[i].type!=OBJECT)continue;
    int role=get_field(s,ts,nt,i,"role");
    if(role<0||(!string_is(s,ts[role],"system")&&!string_is(s,ts[role],"developer")&&!string_is(s,ts[role],"user")))continue;
    collect_contents(s,ts,nt,get_field(s,ts,nt,i,"content"),ids,count);
  }
}
// 0: no environment block; 1: changed, out owned by caller; 2: already normalized; -1: fail open.
static int normalize_existing_environments(const char *payload,size_t len,char **out,size_t *out_len) {
  sync_state st;if(!load_state(&st))return -1;
  char date[18];snprintf(date,sizeof(date),"%s",st.date);
  derive_local_date(date,sizeof(date),st.offset);
  tok *ts=malloc(MAX_TOKENS*sizeof(tok));if(!ts)return -1;
  int nt=parse_json(payload,len,ts,MAX_TOKENS);
  if(nt<1||ts[0].type!=OBJECT){free(ts);return -1;}
  int positions[ENV_FIELDS_LIMIT],count=0,field;
  collect_contents(payload,ts,nt,get_field(payload,ts,nt,0,"instructions"),positions,&count);
  collect_contents(payload,ts,nt,get_field(payload,ts,nt,0,"system"),positions,&count);
  field=get_field(payload,ts,nt,0,"input");
  if(field>=0&&ts[field].type==STRING)push_field(ts,field,positions,&count);
  else collect_messages(payload,ts,nt,field,positions,&count);
  collect_messages(payload,ts,nt,get_field(payload,ts,nt,0,"messages"),positions,&count);
  // Reverse source order, avoiding offsets invalidated by previous edits.
  for(int a=0;a<count;a++)for(int b=a+1;b<count;b++)if(ts[positions[a]].start<ts[positions[b]].start){
    int tmp=positions[a];positions[a]=positions[b];positions[b]=tmp;
  }
  char *current=strndup(payload,len);if(!current){free(ts);return -1;}
  size_t length=len;
  int saw_env=0,changed=0;
  for(int j=0;j<count;j++){
    tok t=ts[positions[j]];
    char *decoded=json_unescape(payload,t);
    if(!decoded){free(current);free(ts);return -1;}
    int found=0;
    char *normalized=patch_environment_text(decoded,st.zone,date,&found);
    if(!normalized){free(current);free(ts);return -1;}
    if(found){
      saw_env=1;
      char *encoded=json_escape_text(normalized);
      free(normalized);
      if(!encoded){free(current);free(ts);return -1;}
      size_t at=(size_t)t.start-1,oldlen=(size_t)(t.end-t.start)+2,enc_len=strlen(encoded);
      if(oldlen!=enc_len||memcmp(current+at,encoded,oldlen)!=0){
        size_t next_len=0;
        char *next=splice(current,length,at,oldlen,encoded,enc_len,&next_len);
        if(!next){free(encoded);free(current);free(ts);return -1;}
        free(current);current=next;length=next_len;changed=1;
      }
      free(encoded);
    }else free(normalized);
  }
  free(ts);
  if(!saw_env||!changed){free(current);return saw_env?2:0;}
  *out=current;*out_len=length;return 1;
}
