/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#define AESD_DEBUG 1  // Remove comment on this line to enable debug

#undef PDEBUG
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

#include <linux/cdev.h>
#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

struct aesd_dev
{
    struct aesd_circular_buffer buffer;
    struct cdev cdev;
    struct mutex lock;

    char *pending_write;
    size_t pending_write_size;
};

#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
