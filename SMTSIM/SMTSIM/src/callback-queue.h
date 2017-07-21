// -*- C++ -*-
//
// Callback queue: stand-alone abstractions for C and C++ callbacks, along
// with a priority-queue to invoke them.
//
// Jeff Brown
// $Id: callback-queue.h,v 1.1.2.8.2.1.2.3 2009/12/04 21:03:02 jbrown Exp $
//

#ifndef CALLBACK_QUEUE_H
#define CALLBACK_QUEUE_H 

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CallbackQueue CallbackQueue;
typedef struct CBQ_Callback CBQ_Callback;
typedef struct CBQ_Args CBQ_Args;


#ifdef __cplusplus
    // A basic type-safe C++ callback/closure abstraction.  The return
    // type is fixed for simplicity's sake, at the cost of full generality.
    // (A derived class could of course hold a reference of arbitrary type,
    // where it stores a result value upon invocation.)
    //
    // When invoked from within a CallbackQueue, if invoke() returns >= 0, the
    // value is taken as a time and the callback is rescheduled for that time.
    struct CBQ_Callback {       // fixed-type struct: ease passage through C
        virtual ~CBQ_Callback() { }
        virtual i64 invoke(CBQ_Args *args) = 0;
    };

    // CBQ_Args instances supply additional callback arguments at invocation
    // time (beyond those bound in at creation time).  These are not deleted
    // automatically by CallbackQueue infrastructure, to allow for nested or
    // multiple invocations.  The caller+callee must ensure they're deleted at
    // an appropriate time.  (Some ref-counting pointers sure would be nice,
    // here.)
    struct CBQ_Args {
        virtual ~CBQ_Args() { }
    };
#endif  // __cplusplus


// Directly invoke/destroy callbacks, outside of a CallbackQueue.
// (These are for C-accessiblity)
i64 callback_invoke(CBQ_Callback *callback, CBQ_Args *args);
void callback_destroy(CBQ_Callback *callback);
void callback_args_destroy(CBQ_Args *args);


// C callback abstraction (not type safe): a function pointer which takes a
// void-pointer parameter that's set when the callback is created, and
// a CBQ_Args pointer that's given when the callback is invoked.
typedef i64 (*CBQ_FuncPtr)(void *, CBQ_Args *);

// Create a callback from a C function pointer and opaque argument pointer.
// This creates a new object which must eventually be destroyed via
// callback_destroy(), or implicitly by a CallbackQueue.
CBQ_Callback *callback_c_create(CBQ_FuncPtr cb_func_ptr, void *cb_data_ptr);

// Query a C callback, returning (through pointers) the arguments it was
// created with.  The callback must have been created via callback_create_c().
void callback_c_query(const CBQ_Callback *cb, CBQ_FuncPtr *func_ptr_ret,
                      void **data_ptr_ret);


// Create or destroy a callback-queue.
//
// Note that the CallbackQueue interface uses CBQ_Callback pointers for two
// distinct purposes: first, as object handles for callback
// invocation/deletion, and second, as unique identifiers for cancelling
// scheduled callbacks.  (There used to be seperate integer ID numbers for
// callbacks which, while a little clearer regarding callback object
// ownership, tended to just add more bookkeeping work for consumers.)

CallbackQueue *callbackq_create(void);
void callbackq_destroy(CallbackQueue *cbq);

// Enqueue a callback for later service.  The CBQ_Callback object is "owned",
// and later deleted by, the CallbackQueue instance (unless canceled
// specifically with cancel_ret).  Does not return on failure.
//
// See callbackq_service() for notes regarding the meaning of time values.
void callbackq_enqueue(CallbackQueue *cbq, i64 cb_time,
                       CBQ_Callback *cb);

// Like callbackq_enqueue(), except that the CallbackQueue does NOT take
// ownership of the callback object, and will not delete it under any
// circumstance.
void callbackq_enqueue_unowned(CallbackQueue *cbq, i64 cb_time,
                               CBQ_Callback *cb);

// Cancel and delete a pending callback.  A callback must not call this on
// itself.  This must not be used on callbacks submitted via
// callbackq_enqueue_unowned().
void callbackq_cancel(CallbackQueue *cbq, CBQ_Callback *cb);

// Cancel a pending callback invocation, preserving the callback instance
// itself.  Callback object "ownership", if held, is transferred to the
// caller; the callback is never destroyed.  A callback may call this on
// itself, but then must NOT request automatic rescheduling via its return
// value.
void callbackq_cancel_ret(CallbackQueue *cbq, CBQ_Callback *cb);


// Service all pending callbacks scheduled with cb_time <= time_now.  Passes
// the given "cb_args" object pointer to each callback, without any other
// processing; the semantics of the cb_args pointer (whether it can be NULL,
// who frees that object, etc.) are determined by the consumers.
//
// Triggered callbacks may enqueue() other callbacks, but should not lead to
// nested calls to service() on the same object.
//
// NOTE: the "time" values used here do not necessarily correspond with any
// others in the system, or with any other CallbackQueue instance.  They are
// only used in the context of this CallbackQueue object, between
// callbackq_enqueue() and callbackq_service() calls.  The only requirement is
// that the sequence of time values be non-decreasing across calls to
// callbackq_service().  "time" could be "simulation cycles", "thread #2
// complete instructions", etc.
void callbackq_service(CallbackQueue *cbq, i64 time_now, CBQ_Args *cb_args);


// Non-modifying test: does the given CallbackQueue have any work to do right
// now, where "now" is time_now?  Returns false iff a call to
// callbackq_service(...time_now...) will have no observable effect.  This
// can be used e.g. to avoid needlessly constructing a CBQ_Args argument
// needlessly.
//
// Should not be used while a service() is in-progress on the same object.
//
// This has a sane implemntation, and a neurotically-pre-optimized version,
// selected at compile-time by the following macro definition.
#define CALLBACKQ_USE_NEUROTICALLY_OPTIMIZED_READY 1
#if (!(CALLBACKQ_USE_NEUROTICALLY_OPTIMIZED_READY))
    int callbackq_ready(const CallbackQueue *cbq, i64 time_now);
#else
    // Icky but performant hack: test whether a CallbackQueue is ready, with
    // no function-call overhead.  Relies on a field of specific width and
    // offset, within the otherwise-opaque CallbackQueue object.  (These
    // conditions are verified by the object constructor.)  The idea is to
    // make it really cheap to have CallbackQueue-s which are mostly idle,
    // especially when they're used for more than just having a single global
    // event queue with a shared clock.  (This can safely be disabled by
    // switching off the controlling macro, which reverts to an actual
    // type-safe implementation.)
    #define callbackq_ready(cbq, time_now) \
        (((const i64 *)(cbq))[0] <= (time_now))
#endif

// Print the current callback queue, for debugging and such
void callbackq_dump(const CallbackQueue *cbq, void *FILE_out,
                    const char *prefix);

#ifdef __cplusplus
}
#endif

#endif  // CALLBACK_QUEUE_H
