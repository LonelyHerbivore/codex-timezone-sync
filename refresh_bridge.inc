// The container has no Python/curl. A host-side Unix socket helper performs
// the real probe using the same Mihomo egress as the scheduled refresh.
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
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
  // Docker inspect (8s) plus the HTTPS probe (12s) can exceed 18s.
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
