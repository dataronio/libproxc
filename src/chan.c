
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "internal.h"

Chan* chan_create(size_t data_size)
{
    Chan *chan;

    /* alloc CHAN struct */
    if (!(chan = malloc(sizeof(Chan)))) {
        PERROR("malloc failed for Chan\n");
        return NULL;
    }

    PDEBUG("CHAN of type size %zu created\n", data_size);

    /* set CHAN members */
    chan->data_size = data_size;
    TAILQ_INIT(&chan->endQ);
    TAILQ_INIT(&chan->altQ);

    return chan;
}

void chan_free(Chan *chan)
{
    if (!chan) return;

    PDEBUG("CHAN closed\n");
    free(chan);
}

static inline
void _chan_copydata(void *dst, void *src, size_t size)
{
    switch (size) {
    case 0:    /* do nothing */                   break;
    case 1:   *(uint8_t *)dst =  *(uint8_t *)src; break;
    case 2:  *(uint16_t *)dst = *(uint16_t *)src; break;
    case 4:  *(uint32_t *)dst = *(uint32_t *)src; break;
    case 8:  *(uint64_t *)dst = *(uint64_t *)src; break;
    default: memcpy(dst, src, size); break;
    }
}

int chan_write(Chan *chan, void *data, size_t size)
{
    ASSERT_NOTNULL(chan);
    ASSERT_EQ(size, chan->data_size);

    //Proc *proc = proc_self();

    // << acquire lock <<
    
    ChanEnd *first;
    
    first = TAILQ_FIRST(&chan->altQ);
    if (first) {
        if (alt_accept(first->guard)) {
            
            // >> release lock >>

            //memcpy(first->data, data, size);
            _chan_copydata(first->data, data, size);
            scheduler_addready(first->proc);
            return 1;
        }
    }

    first = TAILQ_FIRST(&chan->endQ);
    /* if chanQ not empty and containts readers */
    if (first && first->type == CHAN_READER) {
        TAILQ_REMOVE(&chan->endQ, first, node);

        // >> release lock >>

        PDEBUG("CHAN write, reader found\n");
        
        /* copy over data */
        //memcpy(first->data, data, size);
        _chan_copydata(first->data, data, size);

        /* resume reader */
        scheduler_addready(first->proc);
        //proc_yield(proc);
        return 1;
    }

    /* if not, chanQ is empty or contains writers, enqueue self */
    Proc *proc = proc_self();
    struct ChanEnd reader_end = {
        .type  = CHAN_WRITER,
        .data  = data,
        .chan  = chan,
        .proc  = proc,
        .guard = NULL
    };
    TAILQ_INSERT_TAIL(&chan->endQ, &reader_end, node);

    // >> release lock >>
    
    PDEBUG("CHAN write, no readers, enqueue\n");

    /* yield until reader reschedules this end */
    proc->state = PROC_CHANWAIT;
    proc_yield(proc);
    /* here, chan operation is complete */
    return 1;
}

int chan_read(Chan *chan, void *data, size_t size)
{
    ASSERT_NOTNULL(chan);
    ASSERT_EQ(size, chan->data_size); 

    //Proc *proc = proc_self();

    // << acquire lock <<

    ChanEnd *first = TAILQ_FIRST(&chan->endQ);

    /* if chanQ not empty and contains writers */
    if (first && first->type == CHAN_WRITER) {
        TAILQ_REMOVE(&chan->endQ, first, node);

        // >> release lock >>
        
        PDEBUG("CHAN read, writer found\n");
        
        /* copy over data */
        //memcpy(data, first->data, size);
        _chan_copydata(data, first->data, size);

        /* resume writer */
        scheduler_addready(first->proc);
        //proc_yield(proc);
        return 1;
    }
    
    /* if not, chanQ is empty or contains readers, enqueue self */
    Proc *proc = proc_self();
    struct ChanEnd writer_end = {
        .type  = CHAN_READER,
        .data  = data,
        .chan  = chan,
        .proc  = proc,
        .guard = NULL
    };
    TAILQ_INSERT_TAIL(&chan->endQ, &writer_end, node);
    
    // >> release lock >>

    PDEBUG("CHAN read, no writers, enqueue\n");

    /* yield until writer reschedules this end */
    proc->state = PROC_CHANWAIT;
    proc_yield(proc);
    /* here, chan operation is complete */
    return 1;
}

int chan_altenable(Chan *chan, Guard *guard)
{
    ASSERT_NOTNULL(chan);
    ASSERT_NOTNULL(guard);

    ChanEnd *ch_end = TAILQ_FIRST(&chan->endQ);
    if (ch_end && ch_end->type == CHAN_WRITER) {
        return 1;
    }

    TAILQ_INSERT_TAIL(&chan->altQ, &guard->ch_end, node);

    return 0;
}

void chan_altdisable(Chan *chan, Guard *guard)
{
    ASSERT_NOTNULL(chan);
    ASSERT_NOTNULL(guard);
    ASSERT_EQ(chan, guard->chan);

    TAILQ_REMOVE(&chan->altQ, &guard->ch_end, node);
}

void chan_altread(Chan *chan, Guard *guard, size_t size)
{
    ASSERT_NOTNULL(chan);
    ASSERT_NOTNULL(guard);
    ASSERT_EQ(size, chan->data_size);

    // << acquire lock <<
    
    ChanEnd *first = TAILQ_FIRST(&chan->endQ);
    if (UNLIKELY(first->type != CHAN_WRITER)) {
        PANIC("Altread called on chan with no writers\n");
    }

    TAILQ_REMOVE(&chan->endQ, first, node);

    // >> release lock >>

    //memcpy(guard->data.ptr, first->data, chan->data_size);
    _chan_copydata(guard->data.ptr, first->data, chan->data_size);

    scheduler_addready(first->proc);
}

