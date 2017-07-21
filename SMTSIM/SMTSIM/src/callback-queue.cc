//
// Callback queue: priority queue of future callbacks
//
// Jeff Brown
// $Id: callback-queue.cc,v 1.1.2.11.2.1.2.7 2009/12/04 21:03:02 jbrown Exp $
//

const char RCSid_1109058507[] =
"$Id: callback-queue.cc,v 1.1.2.11.2.1.2.7 2009/12/04 21:03:02 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "callback-queue.h"
#include "hash-map.h"
#include "utils.h"
#include "utils-cc.h"

using std::make_pair;
using std::pair;
using std::vector;


class C_Callback : public CBQ_Callback {
    CBQ_FuncPtr func_ptr_;
    void *data_ptr_;
    NoDefaultCopy nocopy;
public:
    C_Callback(CBQ_FuncPtr func_ptr, void *data_ptr) 
        : func_ptr_(func_ptr), data_ptr_(data_ptr) { }
    i64 invoke(CBQ_Args *args) { return func_ptr_(data_ptr_, args); }
    CBQ_FuncPtr get_func() const { return func_ptr_; }
    void *get_data() const { return data_ptr_; }
};


class CBQ_Entry {
    i64 time;
    unsigned order;     // Provides total ordering for requests with same times
    bool owned;         // Flag: callback is "owned" by this, delete when done
    CBQ_Callback *cb;   // NULL <=> canceled
    NoDefaultCopy nocopy;
public:
    CBQ_Entry(i64 time_, unsigned order_, bool owned_, CBQ_Callback *cb_)
        : time(time_), order(order_), owned(owned_), cb(cb_) { }
    ~CBQ_Entry() {
        if (owned) {
            delete cb;
        }
    }
    i64 get_time() const { return time; }
    bool have_cb() const { return (cb != NULL); }
    bool is_owned() const { return owned; }
    CBQ_Callback * get_cb() const { return cb; }
    void unlink_cb() { cb = NULL; }
    void set_order(unsigned order_) { order = order_; }

    void resched(i64 new_time, unsigned new_order) {
        time = new_time;
        order = new_order;
    }
    bool operator > (const CBQ_Entry& e2) const {
        return (time > e2.time) || 
            ((time == e2.time) && (order > e2.order));
    }

    std::string fmt() const {
        std::ostringstream out;
        out << "time " << time << " cb " << (void *) cb
            << " (order " << order << ")";
        return out.str();
    }
};


struct CBQ_EntryTimeComp {
    inline bool
    // Note: comparison is "greater-than" to get a min-heap from STL max-heap
    // routines
    operator() (const CBQ_Entry *e1, const CBQ_Entry *e2) const {
        return *e1 > *e2;
    }
};


struct CallbackPtrHash {
    inline size_t operator() (const CBQ_Callback *key) const {
        StlHashVoidPtr h;
        return h(static_cast<const void *>(key));
    }
};


#if 1 && HAVE_HASHMAP
    typedef hash_map<CBQ_Callback *, CBQ_Entry *,
                     CallbackPtrHash> CBQ_EntryMap;
#else
    typedef std::map<CBQ_Callback *, CBQ_Entry *> CBQ_EntryMap;
#endif


struct CallbackQueue {
private:
    // Time of earliest enqueued event, or I64_MAX if empty.  This is used for
    // very-low-cost testing for readiness, particularly if
    // CALLBACKQ_USE_NEUROTICALLY_OPTIMIZED_READY is active.
    i64 next_event_MUST_BE_FIRST;       // this field must come first (checked)

    unsigned next_order;
    typedef vector<CBQ_Entry *> CBQ_EntryVec;
    CBQ_EntryVec time_heap;     // CBQ_Entry pointer "owned" by this
    CBQ_EntryMap cb_to_ent;     // (CBQ_Callback pointers used as IDs.)
    NoDefaultCopy nocopy;

    void global_order_fixup();
    void re_enqueue(CBQ_Entry *ent, i64 new_time) {
        CBQ_EntryTimeComp comp_func;
        ent->resched(new_time, next_order);
        time_heap.push_back(ent);
        std::push_heap(time_heap.begin(), time_heap.end(),
                       comp_func);
        // Note: order update code copied from enqueue()
        next_order++;
        if (SP_F(next_order <= 0))      // Ordering tag overflow
            global_order_fixup();
        // no need to update next_event_MUST_BE_FIRST: parent service() call
        // will do so before returning
    }

public:
    CallbackQueue();
    ~CallbackQueue();

    void enqueue(i64 cb_time, CBQ_Callback *callback, bool owned) {
        sim_assert(cb_time >= 0);
        CBQ_EntryTimeComp comp_func;
        CBQ_Entry *new_ent = new CBQ_Entry(cb_time, next_order, owned,
                                           callback);
        time_heap.push_back(new_ent);
        std::push_heap(time_heap.begin(), time_heap.end(), comp_func);
        if (!cb_to_ent.insert(std::make_pair(callback, new_ent)).second) {
            abort_printf("callback enqueue(%s,%p) failed: dup callback\n",
                         fmt_i64(cb_time), (void *) callback);
        }
        next_order++;
        if (next_order <= 0)    // Ordering tag overflow
            global_order_fixup();
        if (cb_time < next_event_MUST_BE_FIRST)
            next_event_MUST_BE_FIRST = cb_time;
    }

    bool ready(i64 time_now) const {
        // return !time_heap.empty() && (time_heap.front()->time <= time_now);
        return next_event_MUST_BE_FIRST <= time_now;
    }

    void service(i64 time_now, CBQ_Args *cb_args) {
        CBQ_EntryTimeComp comp_func;
        while (!time_heap.empty() &&
               (time_heap.front()->get_time() <= time_now)) {
            std::pop_heap(time_heap.begin(), time_heap.end(), comp_func);
            CBQ_Entry *ent = time_heap.back();
            time_heap.pop_back();
            // "ent" now orphaned; we must either re_enqueue() or delete it
            i64 resched_time;
            if (ent->have_cb()) {
                // Warning: callback may invoke other CallbackQueue methods!
                // (but API 'contract' disallows "service" or "ready" methods)
                resched_time = ent->get_cb()->invoke(cb_args);
            } else {
                // callback pointer not present: callback was previously
                // canceled, so just silently delete this CBQ_Entry
                resched_time = -1;
            }
            if (resched_time >= 0) {
                // re-schedule this callback for later
                re_enqueue(ent, resched_time);
            } else {
                // destroy this callback.  "ent" is already out of the
                // time_heap, so remove it from the ID map, and delete it.
                if (ent->have_cb()) {
                    cb_to_ent.erase(ent->get_cb());
                }
                delete ent;             // NOTE: deletes ent->cb iff owned
            }
        }
        next_event_MUST_BE_FIRST = (time_heap.empty()) ? I64_MAX :
            time_heap.front()->get_time();
    }

    // true <=> is owned by CallbackQueue
    bool cancel(CBQ_Callback *callback);
    void dump(void *FILE_out, const char *prefix) const;
};


// variant of standard "offsetof" macro which ignores C++ restrictions
// data-types (C++ offsetof only works on POD-types, and CallbackQueue may
// not be Plain Old Data due to its inclusion of STL containers)
#define object_offsetof_dangerous(obj, field) \
    (reinterpret_cast<const char *>(&((obj).field)) - \
     reinterpret_cast<const char *>(&(obj)))

CallbackQueue::CallbackQueue()
    : next_event_MUST_BE_FIRST(I64_MAX), next_order(0)
{ 
    if (CALLBACKQ_USE_NEUROTICALLY_OPTIMIZED_READY) {
        // If this fails to compile or when run, you can safely just switch
        // off CALLBACKQ_USE_NEUROTICALLY_OPTIMIZED_READY in the header file
        int special_offset =
            object_offsetof_dangerous(*this,
                                      next_event_MUST_BE_FIRST);
        int special_size = sizeof(next_event_MUST_BE_FIRST);
        if ((special_offset != 0) || (special_size != sizeof(i64))) {
            abort_printf("CallbackQueue type failure: using neurotically-"
                         "optimized ready() implementation, but field "
                         "offset %d or size %d don't match required "
                         "offset %d, size %d\n", special_offset, special_size,
                         0, sizeof(i64));
        }
    }
}


CallbackQueue::~CallbackQueue()
{
    FOR_ITER(CBQ_EntryVec, time_heap, iter) {
        delete *iter;
    }
}


bool
CallbackQueue::cancel(CBQ_Callback *callback)
{
    CBQ_EntryMap::iterator found = cb_to_ent.find(callback);
    if (found == cb_to_ent.end()) {
        abort_printf("CallbackQueue::cancel: unknown callback %p\n",
                     (void *) callback);
    }
    CBQ_Entry *ent = found->second;
    cb_to_ent.erase(found);
    // Leave CBQ_Entry in heap structure, just mark it to be ignored (it will
    // be deleted in service() when its indicated time comes).
    sim_assert(ent->have_cb());
    ent->unlink_cb();
    // Callback object unlinked at this point: caller is now responsible for it
    return ent->is_owned();
}


void
CallbackQueue::dump(void *FILE_out, const char *prefix) const
{
    CBQ_EntryTimeComp comp_func;
    FILE *out = static_cast<FILE *>(FILE_out);
    vector<const CBQ_Entry *> sorted(time_heap.begin(), time_heap.end());
    // Comparison is reversed to get min-heap, so sort is descending
    std::sort_heap(sorted.begin(), sorted.end(), comp_func);
    for (int i = intsize(sorted) - 1; i >= 0; --i) {
        const CBQ_Entry *ent = sorted[i];
        fprintf(out, "%s%s\n", prefix, ent->fmt().c_str());
    }
}


void
CallbackQueue::global_order_fixup()
{
    // Oddball case: the order numbers we issue have overflowed.  Since
    // they are not exposed to the caller, we can just re-number all requests
    // as long as we don't change the ordering of any two with the same
    // request time.
    CBQ_EntryTimeComp comp_func;
    vector<CBQ_Entry *> sorted(time_heap.begin(), time_heap.end());
    // comp_func is reversed to get min-heap, so the sort is descending
    std::sort_heap(sorted.begin(), sorted.end(), comp_func);
    // Now "sorted[]" points to all entries, sorted in descending order of
    // (time,order) tags.  We'll walk through it backwards, assigning order
    // numbers from zero, leaving all order numbers >= sorted.size() available
    // for use until the next roll-over.
    next_order = 0;
    for (int i = intsize(sorted) - 1; i >= 0; --i) {
        // Does not affect global ordering
        sorted[i]->set_order(next_order);
        next_order++;
    }
}



//
// C interface
//

i64
callback_invoke(CBQ_Callback *callback, CBQ_Args *args)
{
    i64 retval = callback->invoke(args);
    return retval;
}

void
callback_destroy(CBQ_Callback *callback)
{
    delete callback;
}

void
callback_args_destroy(CBQ_Args *args)
{
    delete args;
}

CBQ_Callback *
callback_c_create(CBQ_FuncPtr cb_func_ptr, void *cb_data_ptr)
{
    return new C_Callback(cb_func_ptr, cb_data_ptr);
}

void
callback_c_query(const CBQ_Callback *cb, CBQ_FuncPtr *func_ptr_ret,
                 void **data_ptr_ret)
{
    const C_Callback *cancel_cast = dynamic_cast<const C_Callback *>(cb);
    if (!cancel_cast) {
        abort_printf("callback_c_query() invoked on cb %p, which "
                     "isn't a C_Callback!\n", (const void *) cb);
    }
    if (func_ptr_ret)
        *func_ptr_ret = cancel_cast->get_func();
    if (data_ptr_ret)
        *data_ptr_ret = cancel_cast->get_data();
}

CallbackQueue *
callbackq_create(void) {
    return new CallbackQueue();
}

void
callbackq_destroy(CallbackQueue *cbq)
{
    if (cbq)
        delete cbq;
}

void
callbackq_enqueue(CallbackQueue *cbq, i64 cb_time, CBQ_Callback *cb)
{
    cbq->enqueue(cb_time, cb, true);
}

void
callbackq_enqueue_unowned(CallbackQueue *cbq, i64 cb_time, CBQ_Callback *cb)
{
    cbq->enqueue(cb_time, cb, false);
}

void
callbackq_cancel(CallbackQueue *cbq, CBQ_Callback *cb)
{
    int is_owned = cbq->cancel(cb);
    if (!SP_T(is_owned)) {
        abort_printf("callbackq_cancel attempted on non-owned callback %p\n",
                     (const void *) cb);
    }
    delete cb;
}

void
callbackq_cancel_ret(CallbackQueue *cbq, CBQ_Callback *cb)
{
    cbq->cancel(cb);
}

void 
callbackq_service(CallbackQueue *cbq, i64 time_now, CBQ_Args *cb_args)
{
    cbq->service(time_now, cb_args);
}

#if !(CALLBACKQ_USE_NEUROTICALLY_OPTIMIZED_READY)
int
callbackq_ready(const CallbackQueue *cbq, i64 time_now)
{
    return cbq->ready(time_now);
}
#endif

void
callbackq_dump(const CallbackQueue *cbq, void *FILE_out, const char *prefix)
{
    cbq->dump(FILE_out, prefix);
}
