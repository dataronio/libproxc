
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ucontext.h>

#include "util/debug.h"
#include "internal.h"

void proc_mainfxn(Proc *proc)
{
    ASSERT_NOTNULL(proc);
    ASSERT_NOTNULL(proc->fxn);

    /* do scheduler stuff */
    /* call ctx->fxn() */
    /* when fxn returns, do */
        /* maybe clean up context? */
        /* remove context from scheduler */
        /* return control to scheduler */

    proc->fxn();

    PDEBUG("proc_mainfxn done\n");

    proc->state = PROC_ENDED;
    proc_yield(proc);
}

Proc* proc_self(void)
{
    Proc *proc = scheduler_self()->curr_proc;
    ASSERT_NOTNULL(proc);
    return proc;
}

int proc_create(Proc **new_proc, ProcFxn fxn)
{
    ASSERT_NOTNULL(new_proc);
    ASSERT_NOTNULL(fxn);

    Proc *proc;
    if (!(proc = malloc(sizeof(Proc)))) {
        PERROR("malloc failed for Proc\n");
        return errno;
    }

    Scheduler *sched = scheduler_self();
    if (posix_memalign(&proc->stack.ptr, (size_t)getpagesize(), sched->stack_size)) {
        free(proc);
        PERROR("posix_memalign failed\n");
        return errno;
    }

    /* set fxn and args */
    proc->fxn = fxn;
    proc->args.num = 0;
    proc->args.ptr = NULL;

    /* configure members */
    proc->stack.size = sched->stack_size;
    proc->stack.used = 0;
    proc->state = PROC_READY;
    proc->sched = sched;
    proc->proc_build = NULL;

    /* configure context */
    ctx_init(&proc->ctx, proc);

    *new_proc = proc;

    return 0;
}

void proc_free(Proc *proc)
{
    if (!proc) return;

    free(proc->args.ptr);
    free(proc->stack.ptr);
    free(proc);
}

int proc_setargs(Proc *proc, va_list args)
{
    /* FIXME for over 16 args */
    #define SMALL_SIZE_OPT 16
    void *tmp_args[SMALL_SIZE_OPT]; 
    void *xarg = va_arg(args, void *);
    while (xarg != PROXC_NULL) {
        ASSERT_TRUE(proc->args.num < SMALL_SIZE_OPT);
        tmp_args[proc->args.num++] = xarg;
        xarg = va_arg(args, void *);
    } 

    /* allocate args array for proc */
    if (!(proc->args.ptr = malloc(sizeof(void *) * proc->args.num))) {
        PERROR("malloc failed for Proc Args\n");
        return errno;
    }

    /* copy over args */
    memcpy(proc->args.ptr, tmp_args, sizeof(void *) * proc->args.num);

    return 0;
}

void proc_yield(Proc *proc)
{
    Scheduler *sched = (!proc)
                     ? scheduler_self()
                     : proc->sched;

    PDEBUG("yielding\n");
    ctx_switch(&sched->curr_proc->ctx, &sched->ctx);
}

