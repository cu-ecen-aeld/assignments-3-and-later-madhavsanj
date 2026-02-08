#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n", ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n", ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data *data = (struct thread_data *)thread_param;

    if (data == NULL) {
        return NULL;
    }

    data->thread_complete_success = false;

    if (data->wait_to_obtain_ms > 0) {
        usleep((useconds_t)data->wait_to_obtain_ms * 1000);
    }

    if (pthread_mutex_lock(data->mutex) != 0) {
        return thread_param;
    }

    if (data->wait_to_release_ms > 0) {
        usleep((useconds_t)data->wait_to_release_ms * 1000);
    }

    if (pthread_mutex_unlock(data->mutex) != 0) {
        return thread_param;
    }

    data->thread_complete_success = true;
    return thread_param;
}

bool start_thread_obtaining_mutex(pthread_t *thread,
                                 pthread_mutex_t *mutex,
                                 int wait_to_obtain_ms,
                                 int wait_to_release_ms)
{
    struct thread_data *data;

    if (thread == NULL || mutex == NULL) {
        return false;
    }

    data = (struct thread_data *)malloc(sizeof(struct thread_data));
    if (data == NULL) {
        return false;
    }

    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;
    data->thread_complete_success = false;

    if (pthread_create(thread, NULL, threadfunc, data) != 0) {
        free(data);
        return false;
    }

    return true;
}
