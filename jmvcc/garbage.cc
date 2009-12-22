/* garbage.cc
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#include "garbage.h"
#include "arch/exception.h"
#include "spinlock.h"
#include "arch/cmp_xchg.h"
#include <ace/Synch.h>
#include <vector>
#include <iostream>


using namespace std;
using namespace ML;

namespace JMVCC {

/** A thread can be in one of three states:
    - Not in a critical section
    - In the previous critical section
    - In the current critical section

    The previous critical section finishes when all threads have transitioned
    out of it.

    In this case, the critical sectino number increases, and those that were
    in the new critical section are now considered to be in the old one.
*/

typedef vector<boost::function<void ()> > Cleanups;

/* Each thread has a critical_info structure associated with it.  The
   cleanups go in that structure.  When a thread exits a critical section,
   we look at the parent field of the structure.  If it's */
struct Critical_Info {
    Critical_Info()
        : parent(0), child(0), finished_child(0), dead(false)
    {
    }

    ~Critical_Info()
    {
        dead = true;
    }

    // ONLY the parent is allowed to manipulate this pointer.  Points to the
    // current parent.
    Critical_Info * parent;

    // Points to the child 
    Critical_Info * child;

    // Points to a child that is finished, for which the parent has the
    // responsibility to clean up.
    Critical_Info * finished_child;
    
    // The thread itself manipulates this list; no locks
    Cleanups cleanups;

    bool dead;
};

Spinlock critical_lock;

/// Global pointer to the latest critical info structure.
Critical_Info * last_info = 0;

/// Thread-specific data: what is the thread's critical info structure.  Null
/// if the thread is not in a critical section.
__thread Critical_Info * t_critical = 0;


/// Thread-specific data: nesting level of the current thread.
__thread uint32_t t_nesting = 0;

void enter_critical()
{
    //cerr << "enter_critical: t_critical = " << t_critical << " t_nesting = "
    //     << t_nesting << endl;

    if (t_critical != 0) {
        ++t_nesting;
        return;
    }

    t_critical = new Critical_Info();
    t_nesting = 1;
    t_critical->child = 0;
    
    ACE_Guard<Spinlock> guard(critical_lock);
    // TODO: get rid of this lock

    // Insert it into the back of the list of critical sections
    for (;;) {
        t_critical->parent = last_info;
        if (cmp_xchg(last_info, t_critical->parent, t_critical)) break;
    }

    // We have a problem here: the parent has been updated, but the parent's
    // child has not.  This might cause a race if we didn't have a lock
    // above.
    if (t_critical->parent)
        t_critical->parent->child = t_critical;
}

void leave_critical()
{
    //cerr << "leaving_critical: t_critical = " << t_critical << " t_nesting = "
    //     << t_nesting << endl;

    if (t_nesting == 0) {
        dump_garbage_status();
        throw Exception("badly nested critical sections");
    }
    --t_nesting;
    if (t_nesting > 0) return;

    /* If we have no parents, that means that we're the thread that everyone
       is waiting for, and we can clean up.  Otherwise, we need to add
       ourself to the list of things for our parent to clean up. */
    ACE_Guard<Spinlock> guard(critical_lock);

    if (t_critical->parent == 0) {
        /* We are ready to clean up everything.  First, detach us from the
           list, with the lock held. */
        if (t_critical->child) {
            //ACE_Guard<Spinlock> guard(critical_lock);
            if (t_critical->child)
                t_critical->child->parent = 0;
            t_critical->child = 0;
        } else last_info = 0;

        // Nothing points to us anymore (our parent is null and our child now
        // has a different parent) so we don't need to lock the pointers
        // anymore.
        guard.release();

        /* Now go through and clean up. */
        Critical_Info * current = t_critical, * next;
        while (current) {
            next = current->finished_child;
            for (unsigned i = 0;  i < current->cleanups.size();  ++i)
                current->cleanups[i]();
            delete current;
            current = next;
        }
    }
    else {
        /* Our child now has our parent as a parent */
        if (t_critical->child)
            t_critical->child->parent = t_critical->parent;
        else last_info = t_critical->parent;
        t_critical->finished_child = t_critical->parent->finished_child;
        t_critical->parent->finished_child = t_critical;
    }

    t_critical = 0;
}

void new_critical()
{
    leave_critical();
    enter_critical();
}

void schedule_cleanup(const boost::function<void ()> & cleanup)
{
    if (t_critical == 0) {
        /* We're not in a critical section, so we don't offer any
           guarantees about this data staying around.  Mostly, this code
           path will just be used for testing. */
        cleanup();
        return;
    }

    /* TODO: if we're the one and only thread, then we can just call cleanup
       here. */
    t_critical->cleanups.push_back(cleanup);
}

/// For testing: dump the status of the garbage collector to cerr
void dump_garbage_status()
{
    ACE_Guard<Spinlock> guard(critical_lock);
    size_t num_live_threads = 0;
    size_t num_zombie_threads = 0;
    size_t num_zombie_objects = 0;

    cerr << "------------- garbage collector status" << endl;
    cerr << "t_critical = " << t_critical << endl;
    cerr << "t_nesting = " << t_nesting << endl;
    
    int n = 0;
    for (Critical_Info * info = last_info;  info;  info=info->parent, ++n) {
        cerr << "critical info number " << n << " at " << info
             << " dead " << info->dead << endl;
        cerr << "  " << info->cleanups.size() << " cleanups" << endl;
        num_live_threads += 1;
        num_zombie_objects += info->cleanups.size();
        cerr << "  zombies:" << endl;
        int nz = 0;
        for (Critical_Info * zombie = info->finished_child;  zombie;
             zombie = zombie->finished_child, ++nz) {
            cerr << "    zombie " << nz << " at " << zombie
                 << " with " << zombie->cleanups.size() << " cleanups"
                 << " dead " << info->dead << endl;
            num_zombie_objects += zombie->cleanups.size();
        }
    }

    cerr << "num_live_threads = " << num_live_threads << endl;
    cerr << "num_zombie_threads = " << num_zombie_threads << endl;
    cerr << "num_zombie_objects = " << num_zombie_objects << endl;

    cerr << endl;
}

} // namespace JMVCC
