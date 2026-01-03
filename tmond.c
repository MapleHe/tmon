/****************************************************************************
forked by thorfinn thorfinn@binf.ku.dk
                   tMon - Distributed Resource Monitor

                   Copyright (C) 1999 Jaco Breitenbach

    This code is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this package; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    Jaco Breitenbach can be contacted at <jjb@dsp.sun.ac.za> 

****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <poll.h>
#define MSGLEN 128
#define BAD_OPEN_MESSAGE "Error: /proc filesystem not mounted.\n"
#define STAT_FILE "/proc/stat"
#define MEMINFO_FILE "/proc/meminfo"
#define VERSION "0.3"
#define SLEEPTIME 250000
#define DISKWAIT 20 //This means that the disk will only be check every 5th sec.
#define NOBODY "nobody"
#define PIDFILE "/var/run/tmond.pid"
#define PROC_INTERVAL 12
#define PROC_POLL_INTERVAL 3

pthread_attr_t attr;

static volatile sig_atomic_t stop_requested = 0;
static int wakeup_fd = -1;

struct shared_topusers {
  volatile unsigned int seq;
  char max_cpu_user[16];
  char max_mem_user[16];
  volatile int running;
};

#define FILE_TO_BUF(filename, fd) do	{						\
    static int n;							\
    if (fd == -1) {							\
      fd = open(filename, O_RDONLY);					\
      if (fd == -1) {							\
	log_err("%s", BAD_OPEN_MESSAGE);				\
	exit(1);							\
      }									\
    }									\
    if (lseek(fd, 0L, SEEK_SET) == -1) {				\
      log_err("lseek failed: %s", strerror(errno));			\
      close(fd);							\
      fd = -1;								\
      exit(1);								\
    }									\
    n = read(fd, buf, sizeof buf - 1);					\
    if (n < 0) {							\
      log_err("read failed: %s", strerror(errno));			\
      close(fd);							\
      fd = -1;								\
      exit(1);								\
    }									\
    buf[n] = '\0';							\
  }while(0)


static char message[MSGLEN];
pthread_mutex_t	mut = PTHREAD_MUTEX_INITIALIZER;
static char buf[1024];
static char *appname;
int sock;
int ncores_val;
float mems_val;
void log_init (void)	{
  closelog();
  openlog(appname, LOG_PID, LOG_DAEMON);
}


void log_msg (const char *fmt, ...)	{
  char msg[1024];
  va_list args;
  
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  syslog(LOG_INFO, "%.500s", msg);
}

int ncores(){
  FILE * fp;
  char res[128];
  memset(res,0,128);
  fp = popen("/bin/cat /proc/cpuinfo |grep -c '^processor'","r");
  if (!fp) return 1;  /* Return default value 1 on failure */
  fread(res, 1, sizeof(res)-1, fp);
  pclose(fp);  /* Use pclose instead of fclose */
  return atoi(res);
}

float nmem(){
  FILE * fp;
  char res[128];
  float val = 0.0;
  memset(res,0,128);
  fp = popen("/usr/bin/head -n1 /proc/meminfo","r");
  if (!fp) return 1.0;  /* Return default value 1.0 GB on failure */
  fread(res, 1, sizeof(res)-1, fp);
  sscanf(res,"MemTotal: %f kB\n",&val);
  // fprintf(stderr,"d:%f\n",val);
  pclose(fp);  /* Use pclose instead of fclose */
  return val/1024.0/1024.0;
}

void log_err (const char *fmt, ...)	{
  char msg[1024];
  va_list args;
  
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  syslog(LOG_ERR, "%.500s", msg);
}


void* sigwait_thread(void *arg) {
  sigset_t *set = (sigset_t *)arg;
  int sig;
  
  while (sigwait(set, &sig) == 0) {
    log_msg("Signal %d received, initiating shutdown", sig);
    stop_requested = 1;
    if (sock >= 0) {
      shutdown(sock, SHUT_RDWR);
    }
    if (wakeup_fd >= 0) {
      char c = 'x';
      write(wakeup_fd, &c, 1);
    }
    break;
  }
  return NULL;
}


int send_to_client (int sock_fd)	{
  int		result; 
  
  if (sock_fd >= 0)	{
    /* Lock and unlock the shared message, just to be safe. */
    pthread_mutex_lock(&mut);
    result = send(sock_fd, message, MSGLEN, 0);
    pthread_mutex_unlock(&mut);
    if (result < MSGLEN) return 1;
  }
  return 0;
}


void* spawn (void *arg)	{
  int *client = (int *)arg;
  int						sock_fd;
  struct sockaddr_in		addr;
  socklen_t				addrlen = sizeof(addr);
  
  /* Set signal SIGPIPE to be ignored. */
  signal(SIGPIPE, SIG_IGN);
  sock_fd = *client;
  free(client);  /* Free allocated memory immediately */
  if (getsockname(sock_fd, (struct sockaddr *) &addr, &addrlen))	{
    close(sock_fd);
    return NULL;
  }
  log_msg("Connection from %s on port %hd", 
	  inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
  /* Upon error, this thread will self-destruct. */
  while (1)	{
    if (send_to_client(sock_fd))	{
      log_msg("Closing connection to %s", inet_ntoa(addr.sin_addr));
      close(sock_fd);
      return NULL;
    }
    usleep(SLEEPTIME);
  }
  return NULL;
}


int make_socket (unsigned short int port)	{
  int yes  = 1;
  int sock;
  struct sockaddr_in	addr;
  
  /* The initial server side socket is created and initialized. */
  if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0)	{
    log_err("Socket: %s", strerror(errno));
    exit(1);
  }
  /* Allow the port to be reused if we die */
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
    log_err("SocketOpt: %s", strerror(errno));
    exit(1);
  }
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)))	{
    log_err("Bind: %s", strerror(errno));
    exit(1);
  }
  listen(sock, SOMAXCONN);
  return sock;
}


void* get_statistics (void *ptr) {
  struct shared_topusers *shared = (struct shared_topusers *)ptr;
  static int		fd_stat=-1, fd_meminfo=-1;
  long			cpu_used, idle;
  static long		cpu_old, idle_old;
  long			rt_cpu, rt_idle, cpu_total;
  float			cpu, mem, swp;
  int				i;
  struct statvfs diskinfo;
  int runTimes = 0; 
  char *filename = "/";
  char cpu_user[16], mem_user[16];
  unsigned int seq1, seq2;
  /* A dedicated thread is spawned to update the statistics. It is noted
   * that it is necessary to read the whole /proc file in order to force
   * an update. To restrict CPU usage, memory and swap info is updated
   * only once for every 8 updates in CPU info.
   */
  while (!stop_requested && __atomic_load_n(&shared->running, __ATOMIC_ACQUIRE))	{
    FILE_TO_BUF(MEMINFO_FILE, fd_meminfo);
    {
      long long total=1, memfree=0, buffers=0, cached=0;
      sscanf(buf, "%*[^T]Total: %lld", &total);
      sscanf(buf, "%*[^F]Free: %lld", &memfree);
      sscanf(buf, "%*[^\n]%*[^u]uffers: %lld", &buffers);
      sscanf(buf, "%*[^C]Cached: %lld", &cached);
      mem = 100.*(total - memfree - buffers - cached)/total;
    }
    {
      if (!statvfs(filename, &diskinfo)) 
	swp = (100* (1-(float)diskinfo.f_bavail/diskinfo.f_blocks));
      else
	swp = 100;
      runTimes = 0;
    }
    //swp=runTimes;
    for (i=0; i<8; i++)	{
      FILE_TO_BUF(STAT_FILE, fd_stat);
      {
	long a, b, c;
	sscanf(buf,"cpu %ld %ld %ld %ld",&a,&b,&c,&idle);
	cpu_used = a + b + c;
      }
      
      rt_cpu = cpu_used - cpu_old;
      rt_idle = idle - idle_old;
      cpu_total = rt_cpu+rt_idle;
      if (cpu_total > 0) { cpu = 100.*rt_cpu/cpu_total; }
      else { cpu = 0.; }
      cpu_old = cpu_used;
      idle_old = idle;
      
      /* Lock and unlock the shared message space. */
      pthread_mutex_lock(&mut);
      
      if (shared) {
        do {
          seq1 = __atomic_load_n(&shared->seq, __ATOMIC_ACQUIRE);
          snprintf(cpu_user, sizeof(cpu_user), "%.6s", shared->max_cpu_user);
          snprintf(mem_user, sizeof(mem_user), "%.6s", shared->max_mem_user);
          seq2 = __atomic_load_n(&shared->seq, __ATOMIC_ACQUIRE);
        } while (seq1 != seq2 || (seq1 & 1));
        snprintf(message, MSGLEN,"cores:%d mem:%.1f cpu:%.1f mem:%.1f swp:%.1f uC:%.6s uM:%.6s",
                 ncores_val,mems_val, cpu, mem, swp, cpu_user, mem_user);
      } else {
        snprintf(message, MSGLEN,"cores:%d mem:%.1f cpu:%.1f mem:%.1f swp:%.1f",
                 ncores_val,mems_val, cpu, mem, swp);
      }
      //fprintf(stderr,"%s\n",message);
      pthread_mutex_unlock(&mut);
      usleep(SLEEPTIME);
    }
    runTimes++;
  }
  return NULL;
}


void set_nobody (void)	{
  struct passwd *pw;
  
  /* If the daemon was started as root, reset the effective uid and gid
   * to belong to user nobody. This is to prevent ANY security leaks.
   */
  if (!geteuid())	{
    /* We are running as root, so continue to set uid and gid to nobody.
     * First we search the password file for nobody's uid and gid.
     */
    pw = getpwent();
    while ((pw != NULL) && strncmp(pw->pw_name, NOBODY, sizeof(NOBODY)))
      pw = getpwent();
    endpwent();
    if (pw)	{
      /* Located nobody's password file entry. Proceed to set uid and gid
       * and additional groups. Do uid last, because we need that to 
       * change our groups too */
      if (setgid(pw->pw_gid))
	log_err("WARNING - Unable to set gid to nobody.");
      if (initgroups(NOBODY, pw->pw_gid))
	log_err("WARNING - Unable to set additional groups to nobody.");
      if (setuid(pw->pw_uid)) /* we aren't root after this... */
	log_err("WARNING - Unable to set uid to nobody.");
    }
    else	{
      log_err("WARNING - Unable to set effective uid to nobody.");
    }
  }
}


static int is_numeric(const char *s) {
  if (!s || !*s) return 0;
  while (*s) {
    if (*s < '0' || *s > '9') return 0;
    s++;
  }
  return 1;
}

struct pid_tick_entry {
  int pid;
  unsigned long long ticks;
};

struct pid_tick_map {
  struct pid_tick_entry *entries;
  int capacity;
  int count;
};

static int init_pid_map(struct pid_tick_map *map, int capacity) {
  map->capacity = capacity;
  map->count = 0;
  map->entries = (struct pid_tick_entry *)calloc(capacity, sizeof(struct pid_tick_entry));
  if (!map->entries) {
    map->capacity = 0;
    return -1;
  }
  return 0;
}

static void free_pid_map(struct pid_tick_map *map) {
  if (map->entries) {
    free(map->entries);
    map->entries = NULL;
  }
  map->count = 0;
}

static void put_pid_map(struct pid_tick_map *map, int pid, unsigned long long ticks) {
  unsigned int hash = pid % map->capacity;
  int i;
  for (i = 0; i < map->capacity; i++) {
    int idx = (hash + i) % map->capacity;
    if (map->entries[idx].pid == 0) {
      /* Empty slot, insert new entry */
      map->entries[idx].pid = pid;
      map->entries[idx].ticks = ticks;
      map->count++;
      return;
    }
    if (map->entries[idx].pid == pid) {
      /* Entry exists, only update ticks */
      map->entries[idx].ticks = ticks;
      return;
    }
  }
  /* Hash table is full, cannot insert (should not happen) */
}

static unsigned long long get_pid_map(struct pid_tick_map *map, int pid) {
  unsigned int hash = pid % map->capacity;
  int i;
  for (i = 0; i < map->capacity; i++) {
    int idx = (hash + i) % map->capacity;
    if (map->entries[idx].pid == pid) return map->entries[idx].ticks;
    if (map->entries[idx].pid == 0) return 0;
  }
  return 0;
}

struct uid_stat {
  uid_t uid;
  unsigned long long cpu_ticks;
  unsigned long mem_kb;
};

struct proc_record {
  int pid;
  uid_t uid;
  unsigned long long ticks;
  unsigned long rss_kb;
};

struct proc_snapshot {
  struct proc_record *records;
  int count;
  int capacity;
};

static int init_snapshot(struct proc_snapshot *snap, int capacity) {
  snap->capacity = capacity;
  snap->count = 0;
  snap->records = (struct proc_record *)malloc(capacity * sizeof(struct proc_record));
  if (!snap->records) {
    snap->capacity = 0;
    return -1;
  }
  return 0;
}

static void free_snapshot(struct proc_snapshot *snap) {
  if (snap->records) {
    free(snap->records);
    snap->records = NULL;
  }
  snap->count = 0;
}

static void add_record(struct proc_snapshot *snap, int pid, uid_t uid, 
                       unsigned long long ticks, unsigned long rss_kb) {
  if (snap->count >= snap->capacity) {
    int new_capacity = snap->capacity * 2;
    struct proc_record *new_records;
    new_records = (struct proc_record *)realloc(snap->records, 
                  new_capacity * sizeof(struct proc_record));
    if (!new_records) {
      /* realloc failed, keep original data, do not add new record */
      return;
    }
    snap->records = new_records;
    snap->capacity = new_capacity;
  }
  snap->records[snap->count].pid = pid;
  snap->records[snap->count].uid = uid;
  snap->records[snap->count].ticks = ticks;
  snap->records[snap->count].rss_kb = rss_kb;
  snap->count++;
}

static int collect_proc_snapshot(struct pid_tick_map *tick_map, 
                                  struct proc_snapshot *snap, int full) {
  DIR *dir = opendir("/proc");
  struct dirent *ent;
  char path[256];
  FILE *fp;
  int pid;
  unsigned long long utime, stime, ticks;
  uid_t uid;
  unsigned long rss_kb;
  
  if (!dir) return -1;
  
  while ((ent = readdir(dir)) != NULL) {
    if (!is_numeric(ent->d_name)) continue;
    pid = atoi(ent->d_name);
    
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (!fp) continue;
    
    utime = stime = 0;
    if (fscanf(fp, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", 
               &utime, &stime) == 2) {
      ticks = utime + stime;
      if (!full && tick_map) {
        put_pid_map(tick_map, pid, ticks);
      }
    }
    fclose(fp);
    
    if (full && snap) {
      uid = 0;
      rss_kb = 0;
      snprintf(path, sizeof(path), "/proc/%d/status", pid);
      fp = fopen(path, "r");
      if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
          if (sscanf(line, "Uid:\t%u", &uid) == 1) continue;
          if (sscanf(line, "VmRSS:\t%lu", &rss_kb) == 1) continue;
        }
        fclose(fp);
      }
      add_record(snap, pid, uid, ticks, rss_kb);
    }
  }
  closedir(dir);
  return 0;
}

static void parse_and_update_topusers(struct shared_topusers *shared, 
                                       struct pid_tick_map *tick0,
                                       struct proc_snapshot *snap) {
  struct uid_stat uid_stats[64];
  int uid_count = 0;
  int i, j;
  uid_t max_cpu_uid = 0, max_mem_uid = 0;
  unsigned long long max_cpu = 0;
  unsigned long max_mem = 0;
  struct passwd pwbuf, *pw;
  char buf[1024];
  
  for (i = 0; i < snap->count; i++) {
    struct proc_record *rec = &snap->records[i];
    unsigned long long ticks0 = get_pid_map(tick0, rec->pid);
    unsigned long long delta_ticks = 0;
    if (ticks0 > 0 && rec->ticks > ticks0) {
      delta_ticks = rec->ticks - ticks0;
    }
    
    int found = 0;
    for (j = 0; j < uid_count; j++) {
      if (uid_stats[j].uid == rec->uid) {
        uid_stats[j].cpu_ticks += delta_ticks;
        uid_stats[j].mem_kb += rec->rss_kb;
        found = 1;
        break;
      }
    }
    if (!found && uid_count < 64) {
      uid_stats[uid_count].uid = rec->uid;
      uid_stats[uid_count].cpu_ticks = delta_ticks;
      uid_stats[uid_count].mem_kb = rec->rss_kb;
      uid_count++;
    }
  }
  
  for (i = 0; i < uid_count; i++) {
    if (uid_stats[i].cpu_ticks > max_cpu) {
      max_cpu = uid_stats[i].cpu_ticks;
      max_cpu_uid = uid_stats[i].uid;
    }
    if (uid_stats[i].mem_kb > max_mem) {
      max_mem = uid_stats[i].mem_kb;
      max_mem_uid = uid_stats[i].uid;
    }
  }
  
  __atomic_add_fetch(&shared->seq, 1, __ATOMIC_RELEASE);
  if (getpwuid_r(max_cpu_uid, &pwbuf, buf, sizeof(buf), &pw) == 0 && pw) {
    snprintf(shared->max_cpu_user, sizeof(shared->max_cpu_user), "%s", pw->pw_name);
  } else {
    snprintf(shared->max_cpu_user, sizeof(shared->max_cpu_user), "uid%u", max_cpu_uid);
  }
  if (getpwuid_r(max_mem_uid, &pwbuf, buf, sizeof(buf), &pw) == 0 && pw) {
    snprintf(shared->max_mem_user, sizeof(shared->max_mem_user), "%s", pw->pw_name);
  } else {
    snprintf(shared->max_mem_user, sizeof(shared->max_mem_user), "uid%u", max_mem_uid);
  }
  __atomic_add_fetch(&shared->seq, 1, __ATOMIC_RELEASE);
}

static void scanner_supervisor(struct shared_topusers *shared, int wake_fd) {
  struct pid_tick_map tick0;
  struct proc_snapshot snap;
  pid_t parser_pid;
  int status;
  int elapsed = 0;
  struct pollfd pfd;
  
  if (init_pid_map(&tick0, 2048) < 0) {
    log_err("Failed to initialize pid map");
    _exit(1);
  }
  if (init_snapshot(&snap, 512) < 0) {
    log_err("Failed to initialize snapshot");
    free_pid_map(&tick0);
    _exit(1);
  }
  
  pfd.fd = wake_fd;
  pfd.events = POLLIN;
  
  while (__atomic_load_n(&shared->running, __ATOMIC_ACQUIRE)) {
    int rc = poll(&pfd, 1, PROC_POLL_INTERVAL * 1000);
    
    if (rc > 0 && (pfd.revents & POLLIN)) {
      break;
    }
    
    if (!__atomic_load_n(&shared->running, __ATOMIC_ACQUIRE)) {
      break;
    }
    
    collect_proc_snapshot(&tick0, NULL, 0);
    elapsed += PROC_POLL_INTERVAL;
    
    if (elapsed >= PROC_INTERVAL) {
      elapsed = 0;
      snap.count = 0;
      collect_proc_snapshot(NULL, &snap, 1);
      
      parser_pid = fork();
      if (parser_pid == 0) {
        set_nobody();
        parse_and_update_topusers(shared, &tick0, &snap);
        _exit(0);
      } else if (parser_pid > 0) {
        waitpid(parser_pid, &status, 0);
      }
      
      free_pid_map(&tick0);
      if (init_pid_map(&tick0, 2048) < 0) {
	log_err("Failed to reinitialize pid map");
	free_snapshot(&snap);
	_exit(1);
      }
    }
  }
  
  free_pid_map(&tick0);
  free_snapshot(&snap);
  _exit(0);
}

void Usage (void)	{
  printf("\nUsage:\ttmond [-p port]\nThe default port number is 7777.\n\n");
  exit(1);
}


int main (int argc, char *argv[])	{
  struct shared_topusers *shared;
  pid_t scanner_pid;
  sigset_t sigset;
  pthread_t sig_thread, stat_thread;
  int wakeup_fds[2];
  
  ncores_val = ncores();
  mems_val = nmem();
#if 0
  fprintf(stderr,"cnores:%d\n",ncores);
  fprintf(stderr,"mems:%f\n",mems);
  return 0;
#endif

  sigemptyset(&sigset);
  sigaddset(&sigset, SIGTERM);
  sigaddset(&sigset, SIGINT);
  pthread_sigmask(SIG_BLOCK, &sigset, NULL);

  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int	new_sock, pidfile;
  FILE	*fhandle;
  int 	result;
  struct sockaddr_in 	clientname;
  socklen_t 	size = sizeof(struct sockaddr_in);
  unsigned short int	port=7777;
  pthread_t thread;
  int opt;
  /* Check commandline parameters. */
  while ((opt=getopt(argc,argv,"p:h")) != EOF)	{
    switch(opt)	{
    case 'p':	{
      port = (unsigned short) atoi(optarg);
      break;
    }
    default:	Usage();
    }	
  }
  
	/* Extract and save calling application name for syslog. */
  if (strchr(argv[0], '/'))
    appname = strrchr(argv[0], '/') + 1;
  else
    appname = argv[0];
  
  /* Fork, and have the parent exit. The child becomes the server. */
  if (fork())
    exit(0);
  
  /* Create shared memory for top users */
  shared = (struct shared_topusers *)mmap(NULL, sizeof(struct shared_topusers),
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  memset(shared, 0, sizeof(struct shared_topusers));
  __atomic_store_n(&shared->running, 1, __ATOMIC_RELEASE);
  snprintf(shared->max_cpu_user, sizeof(shared->max_cpu_user), "none");
  snprintf(shared->max_mem_user, sizeof(shared->max_mem_user), "none");
  
  if (pipe(wakeup_fds) < 0) {
    perror("pipe");
    exit(1);
  }
  
  scanner_pid = fork();
  if (scanner_pid == 0) {
    close(wakeup_fds[1]);
    scanner_supervisor(shared, wakeup_fds[0]);
    _exit(0);
  } else if (scanner_pid < 0) {
    perror("fork scanner");
    exit(1);
  }
  close(wakeup_fds[0]);
  wakeup_fd = wakeup_fds[1];
  
  result = pthread_create(&sig_thread, NULL, sigwait_thread, &sigset);
  if (result) {
    log_err("pthread_create sigwait_thread: %s", strerror(errno));
    exit(1);
  }
  
  /* Initialize syslog. */
  log_init();
  
  /* Redirect stdin, stdout, and stderr to /dev/null. */
  freopen("/dev/null", "r", stdin);
  freopen("/dev/null", "w", stdout);
  freopen("/dev/null", "w", stderr);
  
  /* Chdir to the root directory so that the current disk can be unmounted
   * if desired. 
   */
  if(chdir("/")){
    fprintf(stderr,"Problem chdir\n");
    return 0;
  }
  
  /* Check whether PID file already exists. If not, and we have the
   * necessary access permissions, write the current PID to the file.
   */
  if ((pidfile = open(PIDFILE, O_WRONLY | O_CREAT | O_EXCL,		\
		      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1)	{
    if (errno == EEXIST)
      log_err("%s already exists.", PIDFILE);
    else
      log_err("Error opening %s.", PIDFILE);
  }
  else if ((fhandle = fdopen(pidfile, "w")) != NULL)	{
    fprintf(fhandle, "%u\n", (unsigned int)getpid());
    fclose(fhandle);
  }

  /* Set uid and gid to nobody (if possible and required). */
  set_nobody();
  
  /* Spawn a dedicated thread to update resource statistics. */
  result = pthread_create(&stat_thread, NULL, get_statistics, (void *)shared);
  if (result)	{
    log_err("pthread_create: %s", strerror(errno));
    exit(1);
  }
  
  /* Create and bind socket. */
  sock = make_socket(port);
  
  log_msg("Daemon started on port %d", port);
  
  while (1)	{

    /* Block until new connection received. */
    new_sock = accept(sock, (struct sockaddr *) &clientname, &size);
    if (new_sock < 0)	{
      if (stop_requested) {
        break;
      }
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      log_err("accept: %s", strerror(errno));
      break;
    }
    else	{
      /* Spawn a new thread to handle the new connection. */
      int *client_sock = (int *) malloc(sizeof(int));
      if (!client_sock) {
	log_err("malloc failed for client socket");
	close(new_sock);
      }
      else {
	*client_sock = new_sock;
	result = pthread_create(&thread, &attr, spawn, (void *) client_sock);
	if (result)	{
	  log_err("pthread_create: %s", strerror(errno));
	  free(client_sock);
	  close(new_sock);
	}
      }
    }
  }
  
  log_msg("Shutting down gracefully");
  __atomic_store_n(&shared->running, 0, __ATOMIC_RELEASE);
  
  pthread_join(stat_thread, NULL);
  
  waitpid(scanner_pid, NULL, 0);
  
  if (shared != MAP_FAILED) {
    munmap(shared, sizeof(struct shared_topusers));
  }
  
  close(sock);
  if (wakeup_fd >= 0) {
    close(wakeup_fd);
  }
  
  unlink(PIDFILE);
  
  return 0;
}



