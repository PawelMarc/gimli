/*
 * Copyright (c) 2008-2009 Message Systems, Inc. All rights reserved
 * For licensing information, see:
 * https://labs.omniti.com/gimli/trunk/LICENSE
 */
#include "impl.h"

int respawn = 1;
int should_exit = 0;
int debug = 0;
int watchdog_interval = 60;
int watchdog_start_interval = 200;
int watchdog_stop_interval = 60;
int respawn_frequency = 15;
int detach = 1;
int do_setsid = 1;
int child_argc;
char **child_argv;
char *glider_path = GIMLI_GLIDER_PATH;
char *trace_dir = "/tmp";
volatile struct gimli_heartbeat *heartbeat = NULL;
static char hb_file[256] = "";
static time_t last_spawn = 0;

#define TRACE_NONE 0
#define TRACE_ME   1
#define TRACE_DONE 2

struct kid_proc {
  pid_t pid;
  pid_t tracer_for;
  struct kid_proc *next, *prev;
  int exit_status;
  int out_fd;
  int should_trace;
  int running;
  int kills;
  int watchdog;
};

struct kid_proc *procs = NULL;
static void setup_signal_handlers(int is_child);
void wait_for_exit(struct kid_proc *p, int timeout);
void wait_for_child(struct kid_proc *p);

static void catch_sigchld(int sig_num)
{
  pid_t dead_pid;
  int status;
  struct kid_proc *p;

  signal(SIGCHLD, catch_sigchld);
  do {
    dead_pid = waitpid(-1, &status, WNOHANG|WUNTRACED);
    if (dead_pid == 0) {
      break;
    }
    if (dead_pid == -1 && errno == EINTR) {
      continue;
    }
    if (dead_pid == -1) {
      break;
    }
    for (p = procs; p; p = p->next) {
      if (dead_pid == p->pid) {
        p->exit_status = status;
        if (WIFSTOPPED(status)) {
          gimli_set_proctitle("child pid %d stopped", dead_pid);
          if (!p->should_trace) {
            p->should_trace = TRACE_ME;
          }
        } else {
          gimli_set_proctitle("child pid %d exited", dead_pid);
          p->running = 0;
        }
        break;
      }
    }
  } while (1);
}

/* the child sends us SIGUSR1 as an alternative heartbeat indicator */
static void catch_usr1(int sig_num)
{
  signal(SIGUSR1, catch_usr1);
  if (heartbeat) {
    heartbeat->state = GIMLI_HB_RUNNING;
    heartbeat->ticks++;
  }
}

/* any other signals, we're going to exit, but we want to make
 * sure that our cleanup bits are invoked */
static void catch_other(int sig_num)
{
  struct kid_proc *p;

  fprintf(stderr, "monitor: caught signal %d, terminating.\n", sig_num);
  should_exit = 1;
  respawn = 0;
  for (p = procs; p; p = p->next) {
    if (p->running && p->tracer_for == 0) {
      kill(p->pid, SIGTERM);
    }
  }
}

static void cleanup(void)
{
  if (hb_file[0]) {
    if (debug) {
      fprintf(stderr, "unlinking %s\n", hb_file);
    }
    unlink(hb_file);
  }
}

static int prep(void)
{
  char var[1024];
  char *v;
  int fd;
  void *addr;
  struct gimli_heartbeat template;

  atexit(cleanup);

  /* mmap a temporary file; we do this so that we can pass on the file
   * descriptor to the child, so that it can have a handle on something to map.
   * We unlink the file from disk as soon as we've opened it */

  snprintf(hb_file, sizeof(hb_file)-1, "/tmp/gimlihbXXXXXX");
  fd = mkstemp(hb_file);
  if (fd == -1) {
    fprintf(stderr, 
      "monitor: failed to open heartbeat file: %s\n", hb_file);
    hb_file[0] = '\0';
    return 0;
  }
  if (debug) {
    fprintf(stderr, "monitor: opened hearbeat file: %s\n", hb_file);
  }

  /* make sure the file is sized big enough for the heartbeat, otherwise
   * we'll SIGBUS when trying to access it */
  memset(&template, 0, sizeof(template));
  write(fd, &template, sizeof(template));

  errno = 0;
  addr = mmap(NULL, sizeof(*heartbeat),
                PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (debug) {
    fprintf(stderr, "monitor: mmap fd=%d -> addr %p (%s)\n",
      fd, addr, strerror(errno));
  }
  unlink(hb_file);

  /* we never close the fd, because we want it to be inherited by the child */
  /* close(fd); */

  if (addr == MAP_FAILED) {
    perror("monitor: failed to map heartbeat memory");
    return 0;
  }

  memset(addr, 0, sizeof(*heartbeat));
  heartbeat = addr;

  /* allow child to find the segment */
  snprintf(var, sizeof(var), "GIMLI_HB_FD=%d", fd);
  putenv(strdup(var));

  /* allow child to signal us */
  snprintf(var, sizeof(var), "GIMLI_MONITOR_PID=%d", getpid());
  putenv(strdup(var));

  return 1;
}

static struct kid_proc *spawn_child(void)
{
  struct kid_proc *p;
  
  p = calloc(1, sizeof(*p));
  if (!p) {
    fprintf(stderr, "calloc(): %s\n", strerror(errno));
    return NULL;
  }

  heartbeat->state = GIMLI_HB_NOT_SUPP;
  heartbeat->ticks = 0;

  p->pid = fork();
  if (p->pid == 0) {
    if (do_setsid) {
      setsid();
    }
    setup_signal_handlers(1);
    _exit(execvp(child_argv[0], child_argv));
  }
  if (p->pid == -1) {
    fprintf(stderr, "fork() failed: %s\n", strerror(errno));
    free(p);
    return NULL;
  }
  gimli_set_proctitle("monitoring child %d", p->pid);
  p->running = 1;
  p->next = procs;
  if (procs) {
    procs->prev = p;
  }
  procs = p;
  return p;
}

static void trace_child(struct kid_proc *p)
{
  char pidbuf[32];
  char cmdbuf[1024];
  char tracefile[1024];
  char childname[256];
  struct kid_proc *trc;
  int tracefd;

  if (p->watchdog) {
    gimli_set_proctitle("watchdog triggered: tracing %d", p->pid);
  } else {
    gimli_set_proctitle("fault detected: tracing %d", p->pid);
  }
  p->should_trace = TRACE_DONE;

  trc = calloc(1, sizeof(*trc));
  trc->tracer_for = p->pid;

  snprintf(childname, sizeof(childname)-1, "%s", child_argv[0]);

  snprintf(tracefile, sizeof(tracefile)-1, "%s/%s.%d.trc",
    trace_dir, basename(childname), p->pid);
  tracefd = open(tracefile, O_WRONLY|O_CREAT|O_TRUNC|O_APPEND, 0600);
  if (tracefd == -1) {
    fprintf(stderr, "Unable to open trace file %s: %s\n",
      tracefile, strerror(errno));
  } else {
    char buf[2048];
    time_t now;

    fprintf(stderr, "Tracing to file: %s\n", tracefile);

    time(&now);

    snprintf(cmdbuf, sizeof(cmdbuf)-1, "%s", glider_path);
    snprintf(pidbuf, sizeof(pidbuf)-1, "%d", p->pid);

    snprintf(buf, sizeof(buf)-1,
      "This is a trace file generated by Gimli.\n"
      "Process: %s (pid=%d)\n"
      "Traced because: %s\n"
      "Time of trace: (%ld) %s\n"
      "Invoking trace program: %s\n",
      childname, p->pid,
      p->watchdog ? "watchdog triggered" : "fault detected",
      (long)now, ctime(&now),
      cmdbuf
      );
    write(tracefd, buf, strlen(buf));

    trc->pid = fork();
    if (trc->pid == 0) {
      setup_signal_handlers(1);
      close(1);
      close(2);
      dup2(tracefd, 1);
      dup2(tracefd, 2);
      close(tracefd);
      _exit(execlp(cmdbuf, cmdbuf, pidbuf, (char*)NULL));
    }
    if (trc->pid == -1) {
      int err = errno;
      fprintf(stderr, "fork() failed while tracing child %d: %s\n",
          p->pid, strerror(err));
      snprintf(buf, sizeof(buf)-1,
          "fork() failed while launching tracer: %s\n",
          strerror(err));
      write(tracefd, buf, strlen(buf));
      free(trc);
      trc = NULL;
    } else {
      trc->next = procs;
      if (procs) {
        procs->prev = trc;
      }
      procs = trc;
      /* force a context switch to allow enough time for the child to run
       * so that we can wait for it */
      sleep(2);
      wait_for_child(trc);
    }

    close(tracefd);
  }

  if (p->watchdog) {
    kill(p->pid, SIGABRT);
  } else {
    /* allow the child opportunity to clean up */
    kill(p->pid, SIGCONT);
  }
}

static void setup_signal_handlers(int is_child)
{
  void (*handler)(int) = is_child ? SIG_DFL : catch_other;

  signal(SIGTERM, handler);
  signal(SIGINT, handler);
  signal(SIGQUIT, handler);
  signal(SIGCHLD, is_child ? SIG_DFL : catch_sigchld);
  signal(SIGUSR1, is_child ? SIG_DFL : catch_usr1);
}

static int did_hb_state_change(struct gimli_heartbeat *ref)
{
  /* if the child transitioned state, we need to recalculate
   * our sleep interval */
  struct gimli_heartbeat sample;
  sample = *heartbeat;
  if (sample.state != ref->state) {
    return 1;
  }
  return 0;
}

void wait_for_exit(struct kid_proc *p, int timeout)
{
  struct timespec ts, rem;
  int ticks = timeout;
  int nticks;
  int max_sleep;
  struct gimli_heartbeat hb;

  hb = *heartbeat;

  max_sleep = watchdog_interval;
  if (max_sleep > watchdog_start_interval) {
    max_sleep = watchdog_start_interval;
  }
  if (max_sleep > watchdog_stop_interval) {
    max_sleep = watchdog_stop_interval;
  }
  max_sleep /= 2;
  if (max_sleep > 10 || max_sleep == 0) {
    max_sleep = 10;
  }

  while (ticks > 0 && p->running && p->exit_status == 0) {
    nticks = ticks;
    if (nticks > max_sleep) {
      nticks = max_sleep;
    }
    ts.tv_nsec = 0;
    ts.tv_sec = nticks;
    memset(&rem, 0, sizeof(rem));

    while (nanosleep(&ts, &rem) == -1) {
      /* interrupted */
      if (p->exit_status || p->should_trace == TRACE_ME) {
        return;
      }
      if (p->tracer_for == 0 && did_hb_state_change(&hb)) {
        return;
      }
      if (errno != EINTR) {
        break;
      }
      memcpy(&ts, &rem, sizeof(rem));
    }
    if (p->exit_status || p->should_trace == TRACE_ME) {
      return;
    }
    if (p->tracer_for == 0 && did_hb_state_change(&hb)) {
      return;
    }
    ticks -= nticks;
  }
}

void wait_for_child(struct kid_proc *p)
{
  int use_heartbeat = p->tracer_for == 0;
  struct gimli_heartbeat hb;

  memset(&hb, 0, sizeof(hb));

  while (p->running) {
    int ticks;
    struct timespec ts, rem;

    hb = *heartbeat;

    if (use_heartbeat && heartbeat->state != GIMLI_HB_NOT_SUPP) {
      ticks = watchdog_interval;

      if (hb.state == GIMLI_HB_STARTING) {
        ticks = watchdog_start_interval;
      } else if (hb.state == GIMLI_HB_STOPPING) {
        ticks = watchdog_stop_interval;
      }

    } else {
      ticks = 60;
    }

    wait_for_exit(p, ticks);

    if (p->exit_status || p->should_trace == TRACE_ME) {
      goto trace;
    }

    if (use_heartbeat && did_hb_state_change(&hb)) {
      continue;
    } else {
      goto trace;
    }

    if (p->should_trace ||
        (p->running && hb.state != GIMLI_HB_NOT_SUPP &&
        heartbeat->state == hb.state &&
        heartbeat->ticks == hb.ticks)) {
      goto trace;
    }
  }

trace:
  /* if we get here, the child needs to be traced, then terminated */
  if (p->running && hb.state != GIMLI_HB_NOT_SUPP &&
      heartbeat->state == hb.state &&
      heartbeat->ticks == hb.ticks) {
    p->watchdog = 1;
    if (p->should_trace == TRACE_NONE) {
      p->should_trace = TRACE_ME;
    }
  }

  if (p->running) {

    p->exit_status = 0;
    if (p->should_trace == TRACE_ME) {
      trace_child(p);
    }

    wait_for_exit(p, watchdog_stop_interval);

    while (p->running) {
      kill(p->pid, SIGKILL);
      wait_for_exit(p, 2);
    }
  }

  /* process is done; unlink from the list */
  if (procs == p) {
    procs = p->next;
  }
  if (p->next) {
    p->next->prev = p->prev;
  }
  if (p->prev) {
    p->prev->next = p->next;
  }
}

int main(int argc, char *argv[])
{
  int i;
  struct kid_proc *p;

  if (argc < 2) {
    fprintf(stderr, "not enough arguments\n");
    return 1;
  }
  argv = gimli_init_proctitle(argc, argv);
  child_argc = argc;
  child_argv = argv;
  if (!process_args(&child_argc, &child_argv)) {
    return 1;
  }
  if (debug) {
    fprintf(stderr, "Child to monitor: (argc=%d) ", child_argc);
    for (i = 0; i < child_argc; i++) {
      if (i) fprintf(stderr, " ");
      fprintf(stderr, "%s", child_argv[i]);
    }
    fprintf(stderr, "\n");
  }

  if (!prep()) {
    return 1;
  }

  if (detach) {
    if (debug) {
      fprintf(stderr, "detaching to spawn %s\n", child_argv[0]);
    }
    if (fork()) {
      exit(0);
    }
    if (do_setsid) {
      setsid();
      if (fork()) {
        exit(0);
      }
    }
  } else {
    if (debug) {
      fprintf(stderr, "starting new session for %s\n", child_argv[0]);
    }
    setsid();
  }

  setup_signal_handlers(0);

  while (respawn) {
    int diff;

    diff = time(NULL) - last_spawn;
    if (diff < respawn_frequency) {
      /* throttle back on the spawns */
      gimli_set_proctitle("respawn in %d seconds", respawn_frequency - diff);
      sleep(1);
      continue;
    }

    p = spawn_child();
    if (p == NULL) {
      gimli_set_proctitle("delaying spawn: resource limitations prevent fork");
      sleep(60);
      continue;
    }
    time(&last_spawn);
    wait_for_child(p);
    if (WIFSIGNALED(p->exit_status) && (
#ifdef SIGSEGV
          WTERMSIG(p->exit_status) == SIGSEGV ||
#endif
#ifdef SIGABRT
          WTERMSIG(p->exit_status) == SIGABRT ||
#endif
#ifdef SIGBUS
          WTERMSIG(p->exit_status) == SIGBUS ||
#endif
#ifdef SIGILL
          WTERMSIG(p->exit_status) == SIGILL ||
#endif
#ifdef SIGFPE
          WTERMSIG(p->exit_status) == SIGFPE ||
#endif
#ifdef SIGKILL
          WTERMSIG(p->exit_status) == SIGKILL ||
#endif
          0
          )) {
      respawn = 1;
      free(p);
    } else {
      /* not an abnormal termination */
      int ret = WIFEXITED(p->exit_status) ?
        WEXITSTATUS(p->exit_status) : 0;
      fprintf(stderr, "child exited with return %d\n", ret);
      exit(ret);
    }
  }
  while (1) {
    int running = 0;
    for (p = procs; p; p = p->next) {
      if (p->running) {
        running++;
      }
    }
    if (!running) break;
    fprintf(stderr, "waiting for %d processes to terminate\n", running);
    wait_for_child(procs);
  }

  return 0;
}


/* vim:ts=2:sw=2:et:
 */

