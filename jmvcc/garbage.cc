/* garbage.cc
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#include "garbage.h"
#include "jml/arch/exception.h"
#include "spinlock.h"
#include "jml/arch/cmp_xchg.h"
#include <ace/Synch.h>
#include <vector>
#include <iostream>
#include "jml/utils/hash_map.h"
#include <set>
#include "jml/utils/string_functions.h"
#include "jml/arch/backtrace.h"
#include "jml/arch/atomic_ops.h"


using namespace std;
using namespace ML;

namespace JMVCC {

/* The goal of this code is to enable multi-threading with no read-side locks
   (possibly only a memory barrier).

   The way that this works is that whenever a strucure is modified, the
   following model is used:

   1.  A copy of the old model is taken;
   2.  The updates are made on the copy;
   3.  The new copy is published in place;
   4.  We wait until everything that could have accessed the old copy cannot
       possibly need it any more;
   5.  We destroy the old copy.

   In order to read the structure, we simply access it.  The copy that we
   access will not be modified or destroyed while we are using it.

   Number 4 is the hard part.  We need to determine when nothing could have
   a reference to the old copy of the structure.

   In order to do this, we add the notion of a critical section.  In order to
   read a structure, the following must happen:

   1.  We enter into a "critical section"
   2.  We perform whatever reads we require
   3.  We exit from the critical section

   Note that the thread that is copying the structure also has to perform the
   same action as it copies.

   It is *forbidden* to access any structure that is protected this way from
   outside a critical section.

   
   Interface
   ---------

   * To enter into a critical section, call enter_critical()
   * To exit from a critical section, call exit_critical()
   * To start a new critical section (without explicitly exiting and then
     re-entering), call new_critical()
   * To read a protected value, call read_critical(value) which will return
     the current value after having done whatever memory barriers are
     necessary.
   * To publish a new value for a protected object, call
     publish_critical(new_value, cleanup_function).  The cleanup_function will
     be called on the old value once it can't possibly be valid anymore.
   * To schedule an arbitrary cleanup function to be run when nothing can
     access the object anymore, call schedule_cleanup().  Note that this
     function MAY be called outside a critical section, which is useful
     for things like destructors.  If there are no critical sections in
     process, then the cleanup will be run straight away.
   * To make an object that does all of this automatically, use the RCU<>
     template.


   Implementation
   --------------

   The important invariants are as follows:
   - No object is ever accessed outside a critical section
   - An object that is scheduled for cleanup with schedule_cleanup() will
     be cleaned up at the earliest when the last thread that was in a
     critical section when schedule_cleanup() was called has ended that
     critical section.

   We also want it to be as accurate as possible; ie that cleansups be called
   as soon as it is possible (rather than delayed) as delays will lead to
   objects accumulating in memory, leading to memory and cache pressure.

   When an thread enters into a critical section, it obtains a Critical_Info
   structure.  These structures (one per thread) are linked together in a
   doubly-linked list.  The oldest structure is pointed to by the oldest_ci
   pointer, and the newest one by the newest_ci pointer.  This means that
   traversing the list of critical_info structures traverses the active
   critical sections in order of creation: oldest to newest.

   INVARIANT: traversing the list of critical_info structures forward
   from oldest_ci traverses the active critical sections in creation order
   (oldest to newest).

   INVARIANT: traversing the list of critical_info structures from newest_ci
   to newest_ci traverses the active critical sections in creation order
   (oldest to newest).

   INVARIANT: the newest critical section is always pointed to by newest_ci.
   INVARIANT: the oldest critical section is always pointed to by oldest_ci.
   
   INVARIANT: the number of threads in critical sections is equal to the
   length of the critical_info list.


                  ci0       ci1       cin           

   oldest_ci ---> next ---> next ---> next --->  0
           0 <--- prev <--- prev <--- prev <---  newest_ci


   Each critical_info section has a list of cleanups to perform.  Cleanups
   are simply pushed on the cleanup list of the current critical seciton.

   Once a critical section ends, we need to do one of two things:
   1.  Perform all of the cleanups on its cleanup list;
   2.  Transfer all of the cleanups to the cleanup list of another thread.


   Subordinated Critical Sections

   One critical section is subordinated to another if its lifetime is entirely
   within the lifetime of the other:

   ----------------- CS A
         -----       CS B

   Here, critical section B is subordinated to critical section A.

   If critical section B is subordinated to critical section A, then its
   cleanups must be done by critical section A.  We can detect a subordinated
   critical section by noting that A will be before B in the creation time.


   Disjoint Critical Sections

   ---------              CS A
              ----------  CS B

   In this case, each critical section cleans up its own list, as neither one
   could be using objects that the other one wants to use.


   Overlapping Critical Sections
   
   --w----x----         CS A
        ----y-----z---  CS B
   
   This case is more complex.  Object w should be deleted when CS A disappears;
   objects x, y and z when CS B disappears.
   



   Thus,
   * Cleanups should be assigned to the NEWEST critical_info object no
     matter what critical section they were made in;
   * When a critical section finisheds:
     - If it's the oldest one, then run its cleanups
     - Otherwise, transfer its cleanups to the next oldest one


   --------------------d
       -----
         -x-------
   OK


   --------------------d
       -------------
           -x----
   OK
           

   --------------------
       -----------------------d
           -x----
   OK
*/

struct Critical_Info;


typedef ACE_Mutex Critical_Lock;
Critical_Lock critical_lock;

/// Global pointer to the latest critical info structure.  If this pointer is
/// null, then it means that there are no critical sections active and so
/// things can be deleted at will.
Critical_Info * newest_ci = 0;

typedef vector<boost::function<void ()> > Cleanups;

int num_in_critical = 0;
int num_cleanups_outstanding = 0;

bool debug_mode = false;


struct Critical_Info {
    bool live;

    Critical_Info()
        : live(false), prev(0), next(0)
    {
    }

    void insert()
    {
        if (live)
            throw Exception("insert on live Critical_Info");
        // Must be called with critical_lock held

        // This is the newest one.  Put it in the chain.
        prev = newest_ci;
        if (prev) {
            if (prev->next != 0)
                throw Exception("newest_ci->next != 0");
            prev->next = this;
        }
        newest_ci = this;
        next = 0;
        live = true;
    }

    ~Critical_Info()
    {
        if (!cleanups.empty())
            throw Exception("exited critical section with live cleanups");
    }

    void remove()
    {
        if (!live)
            throw Exception("remove on non-live Critical_Info");

        // Must be called with critical_lock held
        if (prev) {
            prev->next = next;
            prev->transfer_cleanups(cleanups);
        }

        if (next) next->prev = prev;
        else newest_ci = prev;

        prev = next = 0;

        live = false;
    }

    Critical_Info * prev;
    Critical_Info * next;
    
    Cleanups cleanups;

    void add_cleanup(Cleanup cleanup)
    {
        cleanups.push_back(cleanup);
        atomic_add(num_cleanups_outstanding, 1);
    }

    void transfer_cleanups(Cleanups & other_cleanups)
    {
        // TODO: swap or something smarter depending upon which is longer
        cleanups.insert(cleanups.end(),
                        other_cleanups.begin(),
                        other_cleanups.end());
        other_cleanups.clear();
    }

    void cleanup()
    {
        for (unsigned i = 0;  i != cleanups.size();  ++i)
            cleanups[i]();

        atomic_add(num_cleanups_outstanding, -cleanups.size());
        
        cleanups.clear();
    }
};

/// Thread-specific data: what is the thread's critical info structure.  Null
/// if the thread is not in a critical section.
__thread Critical_Info * t_critical = 0;


/// The structure that was allocated
__thread Critical_Info * t_critical_alloc = 0;


/// Thread-specific data: nesting level of the current thread.
__thread uint32_t t_nesting = 0;


void enter_critical()
{
    if (t_critical != 0) {
        ++t_nesting;
        return;
    }
    
    if (JML_UNLIKELY(!t_critical_alloc))
        t_critical_alloc = new Critical_Info();
    
    t_critical = t_critical_alloc;
    if (t_critical->live)
        throw Exception("entered critical section with live t_critical");

    ACE_Guard<Critical_Lock> guard(critical_lock);
    t_critical->insert();
    ++t_nesting;
    ++num_in_critical;
    check_invariants();
}

void leave_critical()
{
    if (t_nesting == 0 || !t_critical) {
        cerr << "badly nested critical sections" << endl;
        throw Exception("badly nested critical sections");
    }
    --t_nesting;
    if (t_nesting > 0) return;

    // We can't call cleanups with the lock held
    {
        ACE_Guard<Critical_Lock> guard(critical_lock);
        t_critical->remove();
        t_critical = 0;
        --num_in_critical;
        check_invariants();
    }

    t_critical_alloc->cleanup();

    if (debug_mode) {
        ACE_Guard<Critical_Lock> guard(critical_lock);
        check_invariants();
    }
}

void new_critical()
{
    leave_critical();
    enter_critical();
}

void schedule_cleanup(const Cleanup & cleanup)
{
    ACE_Guard<Critical_Lock> guard(critical_lock); // TO REMOVE
    if (newest_ci)
        newest_ci->add_cleanup(cleanup);
    else cleanup();
}

void check_invariants()
{
    if (!debug_mode) return;

    if (num_in_critical == 0) {
        if (t_critical)
            throw Exception("num_in_critical == 0 but t_critical != 0");
        if (newest_ci)
            throw Exception("num_in_critical == 0 but newest_ci != 0");
    }
    if (num_in_critical != 0) {
        if (t_critical && t_critical->live != true)
            throw Exception("t_critical->live != true");
        if (!newest_ci)
            throw Exception("num_in_critical != 0 but newest_ci == 0");
        if (newest_ci->live != true)
            throw Exception("newest_ci->live != true");
        if (newest_ci->next != 0)
            throw Exception("newest_ci->next != 0");
        if (num_in_critical == 1) {
            if (t_critical && t_critical != newest_ci)
                throw Exception("only one in critical and we're in critical but t_critical != newest_ci");
            if (newest_ci->prev != 0)
                throw Exception("one in critical but newest_ci->prev != 0");
        }
    }
}

int get_num_in_critical()
{
    return num_in_critical;
}

int get_num_cleanups_outstanding()
{
    return num_cleanups_outstanding;
}

void set_debug_mode(bool debug_mode_on)
{
    debug_mode = debug_mode_on;
}

} // namespace JMVCC
