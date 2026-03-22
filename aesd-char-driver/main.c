/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 * Edits: Madhav Appanaboyina
 * AI Attribution: https://chatgpt.com/c/69bf6b9d-21b4-83e8-89fd-fc1dded31a01
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/mutex.h>

#include "aesdchar.h"
#include "aesd-circular-buffer.h"
#include "aesd_ioctl.h"

int aesd_major = 0; // use dynamic major
int aesd_minor = 0;

MODULE_AUTHOR("Madhav Appanaboyina");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static size_t aesd_circular_buffer_total_size(struct aesd_circular_buffer *buffer)
{
    size_t total = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, buffer, index) {
        if (entry->buffptr && entry->size) {
            total += entry->size;
        }
    }

    return total;
}

static uint8_t aesd_circular_buffer_valid_entries(struct aesd_circular_buffer *buffer)
{
    if (buffer->full) {
        return AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    return buffer->in_offs;
}

static long aesd_adjust_file_offset(struct file *filp, uint32_t write_cmd, uint32_t write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;
    uint8_t valid_entries;
    uint8_t idx;
    uint8_t i;
    loff_t newpos = 0;
    struct aesd_buffer_entry *entry;

    if (!dev) {
        return -EINVAL;
    }

    valid_entries = aesd_circular_buffer_valid_entries(&dev->buffer);
    if (write_cmd >= valid_entries) {
        return -EINVAL;
    }

    idx = dev->buffer.full ? dev->buffer.out_offs : 0;

    for (i = 0; i < valid_entries; i++) {
        entry = &dev->buffer.entry[idx];

        if (!entry->buffptr || entry->size == 0) {
            return -EINVAL;
        }

        if (i == write_cmd) {
            if (write_cmd_offset >= entry->size) {
                return -EINVAL;
            }

            newpos += write_cmd_offset;
            filp->f_pos = newpos;
            return 0;
        }

        newpos += entry->size;
        idx = (idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    return -EINVAL;
}

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    PDEBUG("open");

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t bytes_available;
    size_t bytes_to_copy;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (!dev) {
        return -EINVAL;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer,
                                                            *f_pos,
                                                            &entry_offset);
    if (!entry) {
        retval = 0;
        goto out;
    }

    bytes_available = entry->size - entry_offset;
    bytes_to_copy = (count < bytes_available) ? count : bytes_available;

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *newbuf;
    char *newline_ptr;
    struct aesd_buffer_entry new_entry;
    const char *old_buffptr = NULL;
    ssize_t retval = -ENOMEM;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    if (!dev) {
        return -EINVAL;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    newbuf = krealloc(dev->pending_write, dev->pending_write_size + count, GFP_KERNEL);
    if (!newbuf) {
        retval = -ENOMEM;
        goto out;
    }

    dev->pending_write = newbuf;

    if (copy_from_user(dev->pending_write + dev->pending_write_size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    dev->pending_write_size += count;
    retval = count;

    newline_ptr = memchr(dev->pending_write, '\n', dev->pending_write_size);
    if (newline_ptr) {
        new_entry.buffptr = dev->pending_write;
        new_entry.size = dev->pending_write_size;

        if (dev->buffer.full) {
            old_buffptr = dev->buffer.entry[dev->buffer.in_offs].buffptr;
        }

        aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);

        if (old_buffptr) {
            kfree(old_buffptr);
        }

        dev->pending_write = NULL;
        dev->pending_write_size = 0;
    }

out:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;
    size_t total_size;

    if (!dev) {
        return -EINVAL;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    total_size = aesd_circular_buffer_total_size(&dev->buffer);

    switch (whence) {
        case SEEK_SET:
            newpos = offset;
            break;
        case SEEK_CUR:
            newpos = filp->f_pos + offset;
            break;
        case SEEK_END:
            newpos = total_size + offset;
            break;
        default:
            mutex_unlock(&dev->lock);
            return -EINVAL;
    }

    if (newpos < 0 || newpos > total_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = newpos;
    mutex_unlock(&dev->lock);

    return newpos;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_seekto seekto;
    struct aesd_dev *dev = filp->private_data;
    long retval = 0;

    if (!dev) {
        return -EINVAL;
    }

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) {
        return -ENOTTY;
    }

    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) {
        return -ENOTTY;
    }

    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
                return -EFAULT;
            }

            if (mutex_lock_interruptible(&dev->lock)) {
                return -ERESTARTSYS;
            }

            retval = aesd_adjust_file_offset(filp,
                                             seekto.write_cmd,
                                             seekto.write_cmd_offset);

            mutex_unlock(&dev->lock);
            break;

        default:
            retval = -ENOTTY;
            break;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;

    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);

    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.pending_write = NULL;
    aesd_device.pending_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);
    if (result) {
        unregister_chrdev_region(dev, 1);
    }

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
            entry->buffptr = NULL;
            entry->size = 0;
        }
    }

    if (aesd_device.pending_write) {
        kfree(aesd_device.pending_write);
        aesd_device.pending_write = NULL;
        aesd_device.pending_write_size = 0;
    }

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
