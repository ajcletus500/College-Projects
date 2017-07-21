/*
 * Inter-stage queues for SMTSIM
 *
 * Jeff Brown
 * $Id: stage-queue.h,v 1.3.18.1 2008/04/30 22:17:58 jbrown Exp $
 */

#ifndef STAGE_QUEUE_H
#define STAGE_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

struct activelist;

typedef struct StageQueue {
    struct activelist *head, *tail;
    int count;
} StageQueue;


#define stageq_clear(queue) do { \
    (queue).head = (queue).tail = NULL; \
    (queue).count = 0; \
} while(0)


#define stageq_count(queue) ((queue).count)


#define stageq_enqueue(queue, inst) do { \
    (inst)->next = NULL; \
    if ((queue).head == NULL) \
        (queue).head = (inst); \
    else \
        (queue).tail->next = (inst); \
    (queue).tail = (inst); \
    (queue).count++; \
} while(0)


#define stageq_enqueue_head(queue, inst) do { \
    (inst)->next = (queue).head; \
    (queue).head = (inst); \
    if ((queue).tail == NULL) \
        (queue).tail = (inst); \
    (queue).count++; \
} while(0)


#define stageq_head(queue) ((queue).head)


#define stageq_dequeue(queue) do { \
    (queue).head = (queue).head->next; \
    if (!(queue).head) \
        (queue).tail = NULL; \
    (queue).count--; \
} while(0)


#define stageq_assign(dst_queue, src_queue) do { \
    (dst_queue).head = (src_queue).head; \
    (dst_queue).tail = (src_queue).tail; \
    (dst_queue).count = (src_queue).count; \
    (src_queue).head = (src_queue).tail = NULL; \
    (src_queue).count = 0; \
} while(0)


#define stageq_append_all(dst_queue, src_queue) do { \
    if ((src_queue).count) { \
        if ((dst_queue).tail) \
            (dst_queue).tail->next = (src_queue).head; \
        else \
            (dst_queue).head = (src_queue).head; \
        (dst_queue).tail = (src_queue).tail; \
        (dst_queue).count += (src_queue).count; \
        (src_queue).head = (src_queue).tail = NULL; \
        (src_queue).count = 0; \
    } \
} while(0)


/*
 * prev_inst should be NULL to delete the first element; prev_inst must NOT
 * point to the last element.
 */
#define stageq_delete(queue, prev_inst) do { \
    activelist *deleted = NULL; \
    if (prev_inst) { \
        deleted = (prev_inst)->next; \
        (prev_inst)->next = (prev_inst)->next->next; \
        if (!(prev_inst)->next) \
            (queue).tail = (prev_inst); \
    } else if ((queue).head) { \
        deleted = (queue).head; \
        (queue).head = (queue).head->next; \
        if (!(queue).head) \
            (queue).tail = NULL; \
    } \
    if (deleted) { \
        deleted->next = NULL; \
        (queue).count--; \
    } \
} while(0)


#ifdef __cplusplus
}
#endif

#endif  /* STAGE_QUEUE_H */
