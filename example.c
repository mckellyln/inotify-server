/*
 * example
 * mck - 12/20/13
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>

int main(int argc,char *argv[])
{
    char tfile[200] = { "testfile" };

    unlink(tfile);
    int fd = open(tfile, O_CREAT|O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr,"unable to open test file %s, errno = %d\n", tfile, errno);
        return 0;
    }

    char buf[200] = { "Now is the time for all good men ...\n" };
    int len = strlen(buf);
    if (write(fd, buf, len) != len) {
        fprintf(stderr,"unable to write out test file %s correctly\n", tfile);
        unlink(tfile);
        return 0;
    }

    int ret = close(fd);
    if (ret != 0)
        fprintf(stderr,"unable to close testfile, errno = %d\n", errno);

    // unlink(tfile);
 
    return 0;
}
