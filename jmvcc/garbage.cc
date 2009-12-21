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

Spinlock critical_lock;

struct Critical_Info {
    Critical_Info()
        : current_critical(1), threads_in_curr(0), threads_in_prev(0)
    {
    }

    uint32_t current_critical;
    uint16_t threads_in_curr;
    uint16_t threads_in_prev;
};

Critical_Info critical_info;

__thread uint32_t t_current_critical = 0;
__thread uint32_t t_current_nesting = 0;

Spinlock cleanups_lock;
vector<boost::function<void ()> > cleanups_prev, cleanups_curr;


void enter_critical()
{
    if (t_current_critical != 0) {
        ++t_current_nesting;
        return;
    }

    ACE_Guard<Spinlock> guard(critical_lock);

    t_current_critical = critical_info.current_critical;
    t_current_nesting = 1;
    critical_info.threads_in_curr += 1;
}

void leave_critical()
{
    if (t_current_nesting == 0)
        throw Exception("badly nested critical sections");
    --t_current_nesting;
    if (t_current_nesting == 0) {
        vector<boost::function<void ()> > to_cleanup;

        ACE_Guard<Spinlock> guard(critical_lock);

        Critical_Info & ci = critical_info;

        if (ci.current_critical == t_current_critical) {
            --ci.threads_in_curr;
        }
        else {
            --ci.threads_in_prev;
            if (ci.threads_in_prev == 0) {
                // We are the last thread to exit the critical section.
                // F

                {
                    ACE_Guard<Spinlock> guard2(cleanups_lock);
                    cleanups_prev.swap(to_cleanup);
                    cleanups_curr.swap(cleanups_prev);
                }

                ++ci.current_critical;
                
            }

        }

        guard.release();

        t_current_critical = 0;

        // Now do the cleanups
        for (unsigned i = 0;  i < to_cleanup.size();  ++i)
            to_cleanup[i]();

        // We just left the critical section
        // do processing...
    }
}

void schedule_cleanup(const boost::function<void ()> & cleanup)
{
    if (t_current_critical == 0)
        throw Exception("cannot schedule cleanup when not in critical section");

    ACE_Guard<Spinlock> guard(cleanups_lock);
    if (t_current_critical == critical_info.current_critical)
        cleanups_curr.push_back(cleanup);
    else cleanups_prev.push_back(cleanup);
}

/// For testing: dump the status of the garbage collector to cerr
void dump_garbage_status()
{
    ACE_Guard<Spinlock> guard(critical_lock);
    ACE_Guard<Spinlock> guard2(cleanups_lock);

    cerr << "------------- garbage collector status" << endl;
    cerr << "current_critical = " << critical_info.current_critical << endl;
    cerr << "threads_in_curr = " << critical_info.threads_in_curr << endl;
    cerr << "threads_in_prev = " << critical_info.threads_in_prev << endl;
    cerr << "t_current_critical = " << t_current_critical << endl;
    cerr << "t_current_nesting = " << t_current_nesting << endl;
    cerr << "cleanups_prev.size() = " << cleanups_prev.size() << endl;
    cerr << "cleanups_curr.size() = " << cleanups_curr.size() << endl;
    cerr << endl;
}

} // namespace JMVCC
