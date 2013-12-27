/*
 * server
 * mck - 12/20/13
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <limits.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <errno.h>
#include "uthash.h"

#define MMAX(a,b) (((a)>(b))?(a):(b))

#define MAX_WATCH_FDS      10000
#define MAX_EPOLL_EVENTS   100000
#define MAX_FILE_LEN       300

typedef struct{
    char filename[MAX_FILE_LEN];
    volatile time_t stime;
    UT_hash_handle hh;
}PURGE;
static PURGE *purge = NULL;

static int inot_fd = -1;
static char watchpath[MAX_FILE_LEN] = { "" };
static int lifespan = 60;
static int background = 0;
static char (*dirtbl)[MAX_FILE_LEN];
static int debug = 0;
static int dump_hash_info = 0;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;

static void *work_loop(void *bogus)
{
    struct timespec ts;

    PURGE *pp = NULL;
    PURGE *ptmp = NULL;
    while (1) {
        time_t time_now = time(NULL);
        pthread_rwlock_rdlock(&rwlock);
        HASH_ITER(hh, purge, pp, ptmp) {
            if ((pp->stime > 1) && ((time_now - pp->stime) >= lifespan)) {
                pp->stime = 1;
                if (debug > 1) {
                    pthread_mutex_lock(&lock);
                    fprintf(stderr,"debug: purging <%s> from page cache ...\n", pp->filename);
                    pthread_mutex_unlock(&lock);
                }
                int fd = open(pp->filename, O_RDONLY);
                if (fd >= 0) {
                    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
                    close(fd);
                }
            }
        }
        pthread_rwlock_unlock(&rwlock);
        ts.tv_sec = 0;
        ts.tv_nsec = 50000000;
        nanosleep(&ts, NULL);
    }

    return NULL;
}

static void sig_hdlr(int sig)
{
    if (sig == SIGUSR1) {
        dump_hash_info = 1;
    }else if (sig == SIGUSR2) {
        dump_hash_info = 2;
    } else {
        if (!background) {
            fflush(NULL);
            fprintf(stderr,"\nexiting on signal %d ...\n\n", sig);
        }
        exit(0);
    }
    return;
}

static void list_dir(char *dirpath)
{
    DIR *dirp = opendir(dirpath);
    if (dirp == NULL)
        return;

    int inot_wd = inotify_add_watch(inot_fd, dirpath, (IN_CREATE | IN_ISDIR | IN_CLOSE | IN_MOVED_TO | IN_DELETE));
    if (inot_wd < 0) {
        fprintf(stderr,"error: unable to add watchpath <%s>, errno = %d\n", dirpath, errno);
        exit(0);
    }

    if (inot_wd >= MAX_WATCH_FDS) {
        fprintf(stderr,"error: exceeded dirtbl capacity (%d)\n", MAX_WATCH_FDS);
        exit(0);
    }

    strcpy(dirtbl[inot_wd], dirpath);

    if (debug > 1) {
        pthread_mutex_lock(&lock);
        fprintf(stderr,"debug: %d adding dir <%s> to watchpath\n", inot_wd, dirpath);
        pthread_mutex_unlock(&lock);
    }

    struct dirent *dp = NULL;
    do {
        dp = readdir(dirp);
        if (dp == NULL)
            break;
        if ((strcmp(dp->d_name,".") != 0) && (strcmp(dp->d_name,"..") != 0) && (dp->d_ino != 0)) {
            if (dp->d_type & DT_DIR) {
                char dirpath2[MAX_FILE_LEN] = { "" };
                int dlen = snprintf (dirpath2, MAX_FILE_LEN, "%s/%s", dirpath, dp->d_name);
                if (dlen >= MAX_FILE_LEN-1) {
                    fprintf(stderr,"warning: dir skipped, path len (%d) exceeds limit (%d)\n", dlen, MAX_FILE_LEN);
                    break;
                }
                list_dir(dirpath2);
            }
        }
    } while (dp != NULL);

    closedir(dirp);
    return;
}

int main(int argc,char *argv[])
{
    struct sigaction sact;
    struct sigaction sign;

    if (argc > 1) {
        int i = 1;
        while(i < argc) {
            if ( (strncmp(argv[i], "-h", 2) == 0) || (strncmp(argv[i], "--h", 3) == 0) ) {
                fprintf(stderr,"\nusage: %s [-help] [-debug] [-bg (daemon)] [-life <> (60 sec)] watchpath\n\n", argv[0]);
                return 0;
            }
            if ( (strncmp(argv[i], "-b", 2) == 0) || (strncmp(argv[i], "--b", 3) == 0) ) {
                background = 1;
            } else if ( (strncmp(argv[i], "-d", 2) == 0) || (strncmp(argv[i], "--d", 3) == 0) ) {
                debug++;
            } else if ( (strncmp(argv[i], "-l", 2) == 0) || (strncmp(argv[i], "--l", 3) == 0) ) {
                if (++i < argc)
                    lifespan = atoi(argv[i]);
            } else {
                strcpy(watchpath, argv[i]);
            }
            ++i;
        }
    }

    if (watchpath[0] == '\0') {
        fprintf(stderr,"\nusage: need watchpath argument\n\n");
        return 0;
    }

    if (strncmp(watchpath,"..",2) == 0) {
        fprintf(stderr,"error: invalid watchpath <%s>, need absolute path\n", watchpath);
        return 0;
    }

    int len = strlen(watchpath);
    while ( (len > 1) && watchpath[len-1] == '/')
        len--;

    if (len <= 0) {
        fprintf(stderr,"error: invalid/empty watchpath\n" );
        return 0;
    }

    if ((len == 1) && (watchpath[0] == '/')) {
        fprintf(stderr,"error: invalid watchpath <%s>\n", watchpath);
        return 0;
    }

    watchpath[len] = '\0';

    DIR *dirp = opendir(watchpath);
    if (dirp == NULL) {
        fprintf(stderr,"error: unable to search watchpath <%s>, errno = %d\n", watchpath, errno);
        return 0;
    }
    closedir(dirp);

    if (lifespan < 0)
        lifespan = 0;

    if (debug) {
        fprintf(stderr,"debug     = %d\n", debug);
        fprintf(stderr,"lifespan  = %d\n", lifespan);
        fprintf(stderr,"watchpath = <%s>\n", watchpath);
    }

    int elen = (MAX_EPOLL_EVENTS * sizeof(struct inotify_event) + NAME_MAX + 1);
    char *ebuf = (char *)malloc(elen);
    if (ebuf == NULL) {
        fprintf(stderr,"error: unable to alloc mem for events (%d), errno = %d\n", MAX_EPOLL_EVENTS, errno);
        goto end;
    }

    dirtbl = (char (*)[MAX_FILE_LEN])malloc(MAX_WATCH_FDS * MAX_FILE_LEN);
    if (dirtbl == NULL){
        fprintf(stderr,"error: unable to alloc mem for dir table (%d), errno = %d\n", MAX_WATCH_FDS, errno);
        goto end;
    }

    inot_fd = inotify_init();
    if (inot_fd < 0) {
        fprintf(stderr,"error: unable to create inotify descriptor, errno = %d\n", errno);
        goto end;
    }

    list_dir(watchpath);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 262144);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t tid;
    int ret = pthread_create(&tid, &attr, (void *(*)(void *))work_loop, NULL);
    if (ret) {
        if (!background)
            fprintf(stderr,"error: unable to spawn worker thread, errno = %d\n", ret);
        goto end;
    }

    pthread_attr_destroy(&attr);

    if (background) {
        debug = 0;
        if (daemon(0, 0) < 0) {
            fprintf(stderr,"error: unable to become daemon, errno = %d\n", errno);
            goto end;
        }
    }

    sigfillset(&sact.sa_mask);
    sact.sa_handler = (void (*)(int))sig_hdlr;
    sact.sa_flags = 0;
    sigaction(SIGINT,&sact,NULL);
    sigaction(SIGQUIT,&sact,NULL);
    sigaction(SIGTERM,&sact,NULL);
    sigaction(SIGUSR1,&sact,NULL);
    sigaction(SIGUSR2,&sact,NULL);

    sigfillset(&sign.sa_mask);
    sign.sa_handler = SIG_IGN;
    sign.sa_flags = 0;
    sigaction(SIGHUP,&sign,NULL);

    int epoll_fd = epoll_create(1);

    struct epoll_event ev;

    ev.events = EPOLLIN;
    ev.data.fd = inot_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, inot_fd, &ev);

    time_t time_last = time(NULL);

    while(1) {

        PURGE *pp = NULL;
        struct epoll_event events[MAX_EPOLL_EVENTS];
        int nfds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, 10);
        for (int i=0;i<nfds;i++) {
            if (events[i].data.fd == inot_fd) {

                int ret = read(inot_fd, ebuf, elen);
                if (ret > 0) {
         
                    char wpath[MAX_FILE_LEN] = { "" };
                    for (char *p = ebuf;p<ebuf+ret;) {
                        struct inotify_event *event = (struct inotify_event *)p;
#if 0
                        if (event->mask & IN_ACCESS)        fprintf(stderr,"IN_ACCESS ");
                        if (event->mask & IN_ATTRIB)        fprintf(stderr,"IN_ATTRIB ");
                        if (event->mask & IN_CLOSE_NOWRITE) fprintf(stderr,"IN_CLOSE_NOWRITE ");
                        if (event->mask & IN_CLOSE_WRITE)   fprintf(stderr,"IN_CLOSE_WRITE ");
                        if (event->mask & IN_CREATE)        fprintf(stderr,"IN_CREATE ");
                        if (event->mask & IN_DELETE)        fprintf(stderr,"IN_DELETE ");
                        if (event->mask & IN_IGNORED)       fprintf(stderr,"IN_IGNORED ");
                        if (event->mask & IN_ISDIR)         fprintf(stderr,"IN_ISDIR ");
                        if (event->mask & IN_MODIFY)        fprintf(stderr,"IN_MODIFY ");
                        if (event->mask & IN_MOVED_FROM)    fprintf(stderr,"IN_MOVED_FROM ");
                        if (event->mask & IN_MOVED_TO)      fprintf(stderr,"IN_MOVED_TO ");
                        if (event->mask & IN_OPEN)          fprintf(stderr,"IN_OPEN ");
                        if (event->mask & IN_Q_OVERFLOW)    fprintf(stderr,"IN_Q_OVERFLOW ");
                        if (event->mask & IN_UNMOUNT)       fprintf(stderr,"IN_UNMOUNT ");
                        fprintf(stderr,"\n");
#endif
                        wpath[0] = '\0';

                        if ((event->len > 0) && 
                            (event->name != NULL) && 
                            (event->wd >= 0) && 
                            (event->wd < MAX_WATCH_FDS)) {
                            sprintf(wpath, "%s/%s", dirtbl[event->wd], event->name);
                            // fprintf(stderr,"%d path: %s\n", event->wd, wpath);
                        }

                        if ((event->mask & IN_CREATE) && 
                            (event->mask & IN_ISDIR) && 
                            (event->len > 0) && 
                            (event->name != NULL)) {
         
                            int inot_wd = -1;
                            inot_wd = inotify_add_watch(inot_fd, wpath, (IN_CREATE | IN_ISDIR | IN_CLOSE | IN_MOVED_TO));
                            if (inot_wd < 0) {
                                fprintf(stderr,"warning: unable to add new dir <%s>, errno = %d\n", wpath, errno);
                            } else {
                                if (inot_wd >= MAX_WATCH_FDS) {
                                    fprintf(stderr,"error: exceeded dirtbl capacity (%d)\n", MAX_WATCH_FDS);
                                } else {
                                    strcpy(dirtbl[inot_wd], wpath);
                                    if (debug > 1) {
                                        pthread_mutex_lock(&lock);
                                        fprintf(stderr,"debug: %d adding new dir <%s> to watchpath\n", inot_wd, wpath);
                                        pthread_mutex_unlock(&lock);
                                    }
                                }
                            }
         
                        } else if (!(event->mask & IN_ISDIR) &&
                                    (event->len > 0) &&
                                    ((event->mask & IN_CLOSE) ||
                                    (event->mask & IN_MOVED_TO))) {
         
                            pthread_rwlock_rdlock(&rwlock);
                            HASH_FIND_STR(purge, wpath, pp);
                            pthread_rwlock_unlock(&rwlock);
                            if (pp == NULL) {
                                pp = (PURGE *)malloc(sizeof(PURGE));
                                if (pp == NULL) {
                                    fprintf(stderr,"warning: unable to alloc mem for new hash entry, errno = %d\n", errno);
                                } else {
                                    strcpy(pp->filename, wpath);
                                    pthread_rwlock_wrlock(&rwlock);
                                    pp->stime = time(NULL);
                                    if (debug > 1) {
                                        pthread_mutex_lock(&lock);
                                        fprintf(stderr, "debug: adding file <%s>\n", pp->filename);
                                        pthread_mutex_unlock(&lock);
                                    }
                                    HASH_ADD_STR(purge, filename, pp);
                                    pthread_rwlock_unlock(&rwlock);
                                }
                            } else {
                                pthread_rwlock_wrlock(&rwlock);
                                if (pp->stime <= 1) {
                                    if (debug > 1) {
                                        pthread_mutex_lock(&lock);
                                        fprintf(stderr, "debug: deleting1 file <%s>\n", pp->filename);
                                        pthread_mutex_unlock(&lock);
                                    }
                                    HASH_DEL(purge, pp);
                                    free(pp);
                                } else {
                                    if (debug > 2) {
                                        pthread_mutex_lock(&lock);
                                        fprintf(stderr, "debug: updating1 file time <%s>\n", pp->filename);
                                        pthread_mutex_unlock(&lock);
                                    }
                                    pp->stime = time(NULL);
                                }
                                pthread_rwlock_unlock(&rwlock);
                            }

                        } else if (!(event->mask & IN_ISDIR) &&
                                    (event->len > 0) &&
                                    (event->mask & IN_DELETE)) {

                            pthread_rwlock_rdlock(&rwlock);
                            HASH_FIND_STR(purge, wpath, pp);
                            pthread_rwlock_unlock(&rwlock);
                            if (pp != NULL) {
                                pthread_rwlock_wrlock(&rwlock);
                                if (debug > 1) {
                                    pthread_mutex_lock(&lock);
                                    fprintf(stderr, "debug: deleting0 file <%s>\n", pp->filename);
                                    pthread_mutex_unlock(&lock);
                                }
                                HASH_DEL(purge, pp);
                                free(pp);
                                pthread_rwlock_unlock(&rwlock);
                            }
         
                        }
         
                        p += sizeof(struct inotify_event) + event->len;
                    }
         
                } else if (debug) {
                    pthread_mutex_lock(&lock);
                    fprintf(stderr,"warning: bad inotify event read, errno = %d\n", errno);
                    pthread_mutex_unlock(&lock);
                }

            }
        }

        time_t time_now = time(NULL);
        if ((time_now - time_last) >= lifespan*2) {

            // fprintf(stderr,"in clean-up loop\n");

            PURGE *ptmp = NULL;
            pthread_rwlock_wrlock(&rwlock);
            HASH_ITER(hh, purge, pp, ptmp) {
                if (pp->stime <= 1) {
                    if (debug > 1) {
                        pthread_mutex_lock(&lock);
                        fprintf(stderr, "debug: deleting2 file <%s>\n", pp->filename);
                        pthread_mutex_unlock(&lock);
                    }
                    HASH_DEL(purge, pp);
                    free(pp);
                }
            }
            pthread_rwlock_unlock(&rwlock);

            time_last = time_now;
        }

        if (dump_hash_info) {
            if (!background) {
                PURGE *pp = NULL;
                PURGE *ptmp = NULL;
                pthread_rwlock_rdlock(&rwlock);
                fflush(NULL);
                int n = 0;
                HASH_ITER(hh, purge, pp, ptmp) {
                    if (dump_hash_info == 2) {
                        pthread_mutex_lock(&lock);
                        fprintf(stderr,"debug: hash[%d].filename = <%s>  stime = %ld\n", n, pp->filename, pp->stime);
                        pthread_mutex_unlock(&lock);
                    }
                    n++;
                }
                pthread_rwlock_unlock(&rwlock);
                pthread_mutex_lock(&lock);
                fprintf(stderr,"debug: number of hash entries = %d time now = %ld\n", n, time(NULL));
                pthread_mutex_unlock(&lock);
            }
            dump_hash_info = 0;
        }

    }

end:
    return 0;
}
