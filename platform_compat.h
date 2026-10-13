// Windows portability shims for the CPA C ABI plugin. Linux/macOS use POSIX APIs.
#ifndef CODEX_TZ_PLATFORM_COMPAT_H
#define CODEX_TZ_PLATFORM_COMPAT_H

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
// The AF_UNIX Windows sockaddr layout is available on Windows 10 build 17063+.
// Define the ABI here for older MinGW SDKs without afunix.h.
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct sockaddr_un { ADDRESS_FAMILY sun_family; char sun_path[108]; };
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
// MinGW's C runtime can differ from the host process CRT (Python/Go).
// Read the current Windows process environment directly instead of a CRT copy.
static const char *plugin_getenv(const char *name) {
  static const char *names[]={"CODEX_TZ_LOG_FILE","CODEX_TZ_CONFIG_FILE",
    "CODEX_TZ_STATE_FILE","CODEX_TZ_STATS_FILE","CODEX_TZ_FAILURE_FILE",
    "CODEX_TZ_REFRESH_SOCKET"};
  static char values[6][2048];
  for(unsigned i=0;i<6;i++)if(strcmp(name,names[i])==0){
    DWORD len=GetEnvironmentVariableA(name,values[i],sizeof(values[i]));
    return len>0&&len<sizeof(values[i])?values[i]:NULL;
  }
  return NULL;
}
#define getenv plugin_getenv

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode)&_S_IFMT)==_S_IFREG)
#endif
#define strcasecmp _stricmp
#define gmtime_r plugin_gmtime_r
static struct tm *plugin_gmtime_r(const time_t *value, struct tm *out) {
  return gmtime_s(out,value)==0 ? out : NULL;
}
#define strndup plugin_strndup
static char *plugin_strndup(const char *s,size_t size) {
  size_t n=strnlen(s,size);
  char *result=(char *)malloc(n+1);
  if(result){memcpy(result,s,n);result[n]=0;}
  return result;
}
#define mkstemp plugin_mkstemp
static int plugin_mkstemp(char *path) {
  size_t n=strlen(path);
  if(n<6||strcmp(path+n-6,"XXXXXX")){errno=EINVAL;return -1;}
  for(int i=0;i<16;i++){
    memcpy(path+n-6,"XXXXXX",6);
    if(_mktemp_s(path,n+1)!=0)return -1;
    int fd=_open(path,_O_CREAT|_O_EXCL|_O_RDWR|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(fd>=0)return fd;
    if(errno!=EEXIST)return -1;
  }
  errno=EEXIST;return -1;
}
#define open plugin_open
static int plugin_open(const char *path,int flags,...) {
  int mode=0;
  if(flags&_O_CREAT){
    va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);
  }
  DWORD attr=GetFileAttributesA(path);
  // Windows does not implement O_NOFOLLOW; reject existing reparse points.
  if(attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_REPARSE_POINT)){
    errno=ELOOP;return -1;
  }
  return _open(path,flags|_O_BINARY,mode);
}
#define close _close
#define read _read
#define write _write
#define lseek _lseek
#define fdopen _fdopen
#define unlink _unlink
#define fsync _commit
#define ftruncate plugin_ftruncate
static int plugin_ftruncate(int fd,long long length) {
  return _chsize_s(fd,(unsigned long long)length)==0 ? 0 : -1;
}
#define fchmod plugin_fchmod
static int plugin_fchmod(int fd,int mode){
  // Windows has ACLs, not POSIX mode bits. The containing data directory
  // must be ACL-restricted by its administrator.
  (void)fd;(void)mode;return 0;
}

#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
#endif
static int flock(int fd,int operation) {
  HANDLE handle=(HANDLE)_get_osfhandle(fd);
  if(handle==INVALID_HANDLE_VALUE){errno=EBADF;return -1;}
  OVERLAPPED ov={0};
  BOOL ok;
  if(operation==LOCK_UN)
    ok=UnlockFileEx(handle,0,MAXDWORD,MAXDWORD,&ov);
  else
    ok=LockFileEx(handle,operation==LOCK_EX?LOCKFILE_EXCLUSIVE_LOCK:0,
                  0,MAXDWORD,MAXDWORD,&ov);
  if(!ok){errno=EACCES;return -1;}
  return 0;
}
static ssize_t plugin_getline(char **line,size_t *capacity,FILE *stream) {
  if(!line||!capacity||!stream){errno=EINVAL;return -1;}
  if(!*line||!*capacity){
    *capacity=256;*line=(char *)malloc(*capacity);
    if(!*line)return -1;
  }
  size_t count=0;int c;
  while((c=fgetc(stream))!=EOF){
    if(count+1>=*capacity){
      size_t next=*capacity*2;
      if(next<*capacity||next>20u*1024u*1024u){errno=EOVERFLOW;return -1;}
      char *tmp=(char *)realloc(*line,next);
      if(!tmp)return -1;
      *line=tmp;*capacity=next;
    }
    (*line)[count++]=(char)c;
    if(c=='\n')break;
  }
  if(count==0)return -1;
  (*line)[count]=0;
  return (ssize_t)count;
}
#define getline plugin_getline

typedef volatile LONG plugin_mutex_t;
#define PLUGIN_MUTEX_INITIALIZER 0
static void plugin_mutex_lock(plugin_mutex_t *mutex){
  while(InterlockedCompareExchange(mutex,1,0)!=0)Sleep(0);
}
static void plugin_mutex_unlock(plugin_mutex_t *mutex){InterlockedExchange(mutex,0);}

#else
#include <pthread.h>
typedef pthread_mutex_t plugin_mutex_t;
#define PLUGIN_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static void plugin_mutex_lock(plugin_mutex_t *mutex){pthread_mutex_lock(mutex);}
static void plugin_mutex_unlock(plugin_mutex_t *mutex){pthread_mutex_unlock(mutex);}
#endif
#endif
