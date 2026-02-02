#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
    openlog("writer",0 , LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Usage: %s <writefile> <writestr>", argv[0]);
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    syslog(LOG_DEBUG, "Writing text from %s to %s", writestr, writefile);

    int file = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0) {
        syslog(LOG_ERR, "open failed: %s", strerror(errno));
        closelog();
        return 1;
    }

    if (write(file, writestr, strlen(writestr)) < 0) {
        syslog(LOG_ERR, "write failed: %s", strerror(errno));
        close(file);
        closelog();
        return 1;
    }

    if (close(file) < 0) {
        syslog(LOG_ERR, "close failed: %s", strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}