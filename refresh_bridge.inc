// Ask a host-side helper to refresh the actual Mihomo egress timezone.
// TCP/network credentials never cross the CPA public plugin resource route.
#ifdef _WIN32
// Windows' standard Python distribution has no AF_UNIX. Use a private,
// shared-directory request/reply mailbox consumed by refresh_egress.py --serve.
static plugin_mutex_t refresh_mutex=PLUGIN_MUTEX_INITIALIZER;
static void handle_probe_egress(cliproxy_buffer *out) {
  const char *path=getenv("CODEX_TZ_REFRESH_SOCKET");
  if(!path||!*path)path="C:\\CLIProxyAPI\\data\\codex-timezone-sync-refresh.sock";
  if(strlen(path)>800) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"invalid refresh path\"}");return;
  }
  char request[1024],temporary[1024],response[1024],ready[1024],nonce[32];
  snprintf(request,sizeof(request),"%s.request",path);
  snprintf(ready,sizeof(ready),"%s.ready",path);
  if(GetFileAttributesA(ready)==INVALID_FILE_ATTRIBUTES) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"refresh helper unavailable\"}");return;
  }
  plugin_mutex_lock(&refresh_mutex);
  static LONG seq=0;
  unsigned long long unique=(unsigned long long)GetTickCount() ^
    ((unsigned long long)GetCurrentProcessId()<<32) ^
    (unsigned long long)InterlockedIncrement(&seq);
  snprintf(nonce,sizeof(nonce),"%016llx",unique);
  snprintf(temporary,sizeof(temporary),"%s.request.%s.tmp",path,nonce);
  snprintf(response,sizeof(response),"%s.%s.reply",path,nonce);
  FILE *f=fopen(temporary,"wb");
  int published=0;
  if(f) {
    int written=fprintf(f,"%s\n",nonce);
    if(fclose(f)==0 && written==17)
      published=MoveFileExA(temporary,request,
        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
  }
  if(!published)DeleteFileA(temporary);
  int ok=0;
  if(published)for(int i=0;i<160;i++) {
    FILE *reply=fopen(response,"rb");
    if(reply) {
      char line[16]={0};
      ok=fgets(line,sizeof(line),reply)!=NULL && line[0]=='O' && line[1]=='K' &&
        (line[2]=='\n'||line[2]=='\r');
      fclose(reply);break;
    }
    Sleep(150);
  }
  if(!ok)DeleteFileA(request);
  DeleteFileA(response);
  plugin_mutex_unlock(&refresh_mutex);
  if(!ok) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"refresh helper unavailable or failed\"}");return;
  }
  sync_state fresh;
  if(!load_state(&fresh)) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"egress state unavailable\"}");return;
  }
  char json[320];
  snprintf(json,sizeof(json),"{\"ok\":true,\"timezone\":\"%s\",\"ip\":\"%s\",\"offset\":\"%s\"}",
    fresh.zone,fresh.ip,fresh.offset);
  http_response(out,200,"application/json",json);
}
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static void handle_probe_egress(cliproxy_buffer *out) {
  const char *path=getenv("CODEX_TZ_REFRESH_SOCKET");
  if(!path||!*path)path="/CLIProxyAPI/data/codex-timezone-sync-refresh.sock";
  struct sockaddr_un addr;
  memset(&addr,0,sizeof(addr));
  addr.sun_family=AF_UNIX;
  if(strlen(path)>=sizeof(addr.sun_path)) {
    http_response(out,500,"application/json","{\"ok\":false,\"reason\":\"invalid refresh socket path\"}");return;
  }
  memcpy(addr.sun_path,path,strlen(path)+1);
  int fd=socket(AF_UNIX,SOCK_STREAM,0);
  if(fd<0) {http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"refresh helper unavailable\"}");return;}
#ifdef SO_NOSIGPIPE
  int no_sigpipe=1;
  setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe));
#endif
  struct timeval tv={.tv_sec=25,.tv_usec=0};
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  int connected=connect(fd,(struct sockaddr *)&addr,sizeof(addr))==0;
  int sent=connected && send(fd,"REFRESH\n",8,MSG_NOSIGNAL)==8;
  char response[16]={0};
  ssize_t got=sent?recv(fd,response,sizeof(response)-1,0):-1;
  close(fd);
  if(got<3 || memcmp(response,"OK\n",3)!=0) {
    http_response(out,503,"application/json",
      "{\"ok\":false,\"reason\":\"出口检测失败；请检查主机刷新服务及 Mihomo 出口\"}");return;
  }
  sync_state fresh;
  if(!load_state(&fresh)) {
    http_response(out,503,"application/json","{\"ok\":false,\"reason\":\"出口检测后状态仍不可用\"}");return;
  }
  char json[320];
  snprintf(json,sizeof(json),
    "{\"ok\":true,\"timezone\":\"%s\",\"ip\":\"%s\",\"offset\":\"%s\"}",
    fresh.zone,fresh.ip,fresh.offset);
  http_response(out,200,"application/json",json);
}
#endif
