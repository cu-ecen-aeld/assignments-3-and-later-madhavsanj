/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer implementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 * Edits: Madhav Appanaboyina
 * AI Attribution: https://chatgpt.com/share/69a255e9-89e4-8012-b865-b3526345d3b9
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset. Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset. This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(
            struct aesd_circular_buffer *buffer,
            size_t char_offset,
            size_t *entry_offset_byte_rtn )
{
    size_t entries_to_scan;
    size_t idx;
    size_t running_size = 0;

    if (!buffer || !entry_offset_byte_rtn) {
        return NULL;
    }

    /* Determine number of valid entries */
    entries_to_scan = buffer->full ?
        AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED :
        buffer->in_offs;

    /* Oldest entry index */
    idx = buffer->full ? buffer->out_offs : 0;

    for (size_t i = 0; i < entries_to_scan; i++) {

        struct aesd_buffer_entry *entry = &buffer->entry[idx];

        if (entry->buffptr && entry->size > 0) {

            if (char_offset < (running_size + entry->size)) {
                *entry_offset_byte_rtn = char_offset - running_size;
                return entry;
            }

            running_size += entry->size;
        }

        idx = (idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer,
                                    const struct aesd_buffer_entry *add_entry)
{
    if (!buffer || !add_entry) {
        return;
    }

    /*
     * If buffer is full, we overwrite the oldest entry.
     * Move out_offs forward.
     */
    if (buffer->full) {
        buffer->out_offs =
            (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    /* Copy new entry into current in_offs */
    buffer->entry[buffer->in_offs] = *add_entry;

    /* Advance in_offs */
    buffer->in_offs =
        (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    /* Buffer becomes full when in_offs catches up to out_offs */
    buffer->full = (buffer->in_offs == buffer->out_offs);
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer, 0, sizeof(struct aesd_circular_buffer));
}