/* object_test.cc
   Jeremy Barnes, 21 September 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Test for the set functionality.
*/

#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include "utils/string_functions.h"
#include "utils/vector_utils.h"
#include "utils/pair_utils.h"
#include <boost/test/unit_test.hpp>
#include <boost/bind.hpp>
#include <iostream>
#include <boost/thread.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include "arch/cmp_xchg.h"
#include "arch/threads.h"
#include "arch/exception_handler.h"
#include <set>
#include "utils/circular_buffer.h"
#include "utils/lightweight_hash.h"
#include "arch/timers.h"
#include "ace/Mutex.h"
#include "arch/backtrace.h"
#include <pthread.h>
#include <sched.h>
#include "jmvcc/history.h"

using namespace ML;
using namespace JMVCC;
using namespace std;

using boost::unit_test::test_suite;

struct Spinlock {
    Spinlock()
        : value(0)
    {
    }

    int acquire()
    {
        for (int tries = 0; true;  ++tries) {
            if (__sync_bool_compare_and_swap(&value, 0, 1))
                return 0;
            if (tries == 100) {
                tries = 0;
                sched_yield();
            }
        }
    }

    int release()
    {
        __sync_lock_release(&value);
        return 0;
    }

    volatile int value;
};

typedef ACE_Mutex Mutex;
//typedef Spinlock Mutex;

struct Transaction;



/* Obsolete Version Cleanups

   The goal of this code is to make sure that each version of each object
   gets cleaned up exactly once, at the point when the last snapshot that
   references the version is removed.

   One way to do this is to make sure that each version is either:
   a) the newest version of the object, or
   b) on a list of versions to clean up somewhere, or
   c) cleaned up

   Here, we describe how we maintain and shuffle these lists.

   Snapshot to Version Mapping
   ---------------------------

   Each version will have one or more snapshots that sees it (the exception is
   the newest version of an object, which may not have any snapshots that see
   it).

   versions    snapshots
   --------    ---------
        v0
                  s10
                  s15

       v20        s20
                  s30
                  s40

       v50
                  s70

       v80
                  s90
                  s600

   In this diagram, we have 4 versions of the object (v0, v20, v50 and v80)
   and 6 snapshots.  A version is visible to all snapshots that have an
   epoch >= the version number but < the next version number.  So v0 is
   visible to s10 and s15; v20 is visible to s20, s30 and s40; v40 is visible
   to s70 and v80 is visible to s90 and s600.

   We need to make sure that the version is cleaned up when the *last*
   snapshot that refers to it is destroyed.

   The way that we do this is as follows.  We assume that a later snapshot
   will live longer than an earlier one, and so we put the version to destroy
   on the list for the latest snapshot.  So we have the following lists of
   objects to clean up:

   versions    snapshots    tocleanup
   --------    ---------    ---------
        v0
                  s10
                  s15       v0

       v20        s20
                  s30
                  s40       v20

       v50
                  s70       v50

       v80
                  s90
                  s600
   
   Note that v80, as the most recent value, is not on any free list.
   When snapshot 20 is destroyed, there is nothing to clean up and so it
   simply is removed.  Same story for snapshot 30; now when snapshot 40 is
   destroyed it will clean up v20.

   However, there is no guarantee that the order of creation of the snapshots
   will be the reverse order of destruction.  Let's consider what happens
   if snapshot 40 finishes before snapshot 30 and snapshot 20.  In this case,
   it is not correct to clean up v20 as s20 and s30 still refer to it.  Instead,
   it needs to be moved to the cleanup list for s30.  We know that the version
   is still referenced because the epoch for the version (20) is less than or
   equal to the epoch for the previous snapshot (30).

   As a result, we simply move it to the cleanup list for s30.

   versions    snapshots    tocleanup      deleted
   --------    ---------    ---------      -------
        v0
                  s10
                  s15       v0

       v20        s20
                  s30       v20
                                           s40       

       v50
                  s70       v50

       v80
                  s90
                  s600
   
   Thus, the invariant is that a version will always be on the cleanup list of
   the latest snapshot that references it.
   
   When we cleanup, we look at the previous snapshot.  If the epoch of that
   snapshot is >= the epoch for our version, then we move it to the free
   list of that snapshot.  Otherwise, we clean it up.

   Finally, when we create a new version, we need to arrange for the previous
   most recent version to go onto a free list.  Consider a new version of the
   object on epoch 900:

   versions    snapshots    tocleanup      deleted
   --------    ---------    ---------      -------
        v0
                  s10
                  s15       v0

       v20        s20
                  s30       v20
                                           s40       

       v50
                  s70       v50

       v80
                  s90
                  s600      v80 <-- added
      v900
*/


BOOST_AUTO_TEST_CASE( test0 )
{
    // Check basic invariants
    BOOST_CHECK_EQUAL(current_trans, (Transaction *)0);
    BOOST_CHECK_EQUAL(snapshot_info.entries.size(), 0);

    size_t starting_epoch = get_current_epoch();

    Versioned<int> myval(6);

    BOOST_CHECK_EQUAL(snapshot_info.entries.size(), 0);
    BOOST_CHECK_EQUAL(myval.history.size(), 1);
    BOOST_CHECK_EQUAL(myval.read(), 6);
    
    {
        // Should throw an exception when we mutate out of a transaction
        JML_TRACE_EXCEPTIONS(false);
        BOOST_CHECK_THROW(myval.mutate(), Exception);
    }

    // Check strong exception safety
    BOOST_CHECK_EQUAL(myval.history.size(), 1);
    BOOST_CHECK_EQUAL(myval.read(), 6);

    cerr << "------------------ at start" << endl;
    snapshot_info.dump();
    cerr << "------------------ end at start" << endl;
    
    // Create a transaction
    {
        Local_Transaction trans1;
        
        cerr << "&trans1 = " << &trans1 << endl;

        BOOST_CHECK_EQUAL(myval.history.size(), 1);
        BOOST_CHECK_EQUAL(myval.read(), 6);
        
        // Check that the snapshot is properly there
        BOOST_REQUIRE_EQUAL(snapshot_info.entries.size(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entries.begin()->first, get_current_epoch());
        BOOST_REQUIRE_EQUAL(snapshot_info.entries.begin()->second.snapshots.size(), 1);
        BOOST_CHECK_EQUAL(*snapshot_info.entries.begin()->second.snapshots.begin(), &trans1);
        
        // Check that the correct value is copied over
        BOOST_CHECK_EQUAL(myval.mutate(), 6);

        // Check that we can increment it
        BOOST_CHECK_EQUAL(++myval.mutate(), 7);

        // Check that it was recorded
        BOOST_CHECK_EQUAL(trans1.local_values.size(), 1);

        // FOR TESTING, increment the current epoch
        set_current_epoch(get_current_epoch() + 1);

        // Restart the transaction; check that it was properly recorded by the
        // snapshot info
        trans1.restart();

        // Check that the snapshot is properly there
        BOOST_REQUIRE_EQUAL(snapshot_info.entries.size(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entries.begin()->first,
                          get_current_epoch());
        BOOST_REQUIRE_EQUAL(snapshot_info.entries.begin()->second.snapshots.size(), 1);
        BOOST_CHECK_EQUAL(*snapshot_info.entries.begin()->second.snapshots.begin(), &trans1);
        
        // Finish the transaction without committing it
    }

    cerr << "------------------ at end" << endl;
    snapshot_info.dump();
    cerr << "------------------ end at end" << endl;

    BOOST_CHECK_EQUAL(myval.history.size(), 1);
    BOOST_CHECK_EQUAL(myval.read(), 6);
    BOOST_CHECK_EQUAL(snapshot_info.entries.size(), 0);
    BOOST_CHECK_EQUAL(get_current_epoch(), starting_epoch + 1);

    current_epoch_ = 1;
    earliest_epoch_ = 1;
}

void object_test_thread(Versioned<int> & var, int iter,
                        boost::barrier & barrier,
                        size_t & failures)
{
    // Wait for all threads to start up before we continue
    barrier.wait();

    int errors = 0;
    int local_failures = 0;

    for (unsigned i = 0;  i < iter;  ++i) {
        //static Lock lock;
        //Guard guard(lock);

        // Keep going until we succeed
        int old_val = var.read();
#if 0
        cerr << endl << "=======================" << endl;
        cerr << "i = " << i << " old_val = " << old_val << endl;
#endif
        {
            {
#if 0
                cerr << "-------------" << endl << "state before trans"
                     << endl;
                snapshot_info.dump();
                var.dump();
                cerr << "-------------" << endl;
#endif

                Local_Transaction trans;

#if 0
                cerr << "-------------" << endl << "state after trans"
                     << endl;
                snapshot_info.dump();
                var.dump();
                trans.dump();
                cerr << "-------------" << endl;
                int old_val2 = var.read();
#endif

                //cerr << "transaction at epoch " << trans.epoch << endl;

#if 0
                cerr << "-------------" << endl << "state before read"
                     << endl;
                snapshot_info.dump();
                var.dump();
                trans.dump();
                cerr << "-------------" << endl;
#endif


                int tries = 0;
                do {
                    ++tries;
                    int & val = var.mutate();
                    

#if 0
                    cerr << "old_val2 = " << old_val2 << endl;
                    cerr << "&val = " << &val << " val = " << val << endl;
                    cerr << "&var.read() = " << &var.read() << endl;
                    cerr << "&var.mutate() = " << &var.mutate() << endl;

                    cerr << "-------------" << endl << "state after read"
                         << endl;
                    snapshot_info.dump();
                    var.dump();
                    trans.dump();
                    cerr << "-------------" << endl;
#endif
                    
                    if (val % 2 != 0) {
                        cerr << "val should be even: " << val << endl;
                        ++errors;
                    }
                    
                    //BOOST_CHECK_EQUAL(val % 2, 0);
                    
                    val += 1;
                    if (val % 2 != 1) {
                        cerr << "val should be odd: " << val << endl;
                        ++errors;
                    }
                    
                    //BOOST_CHECK_EQUAL(val % 2, 1);
                    
                    val += 1;
                    if (val % 2 != 0) {
                        cerr << "val should be even 2: " << val << endl;
                        ++errors;
                    }
                    //BOOST_CHECK_EQUAL(val % 2, 0);
                    
                    //cerr << "trying commit iter " << i << " val = "
                    //     << val << endl;

#if 0
                    cerr << "-------------" << endl << "state before commit"
                         << endl;
                    snapshot_info.dump();
                    var.dump();
                    trans.dump();
                    cerr << "-------------" << endl;
#endif

                } while (!trans.commit());

                local_failures += tries - 1;

#if 0
                cerr << "-------------" << endl << "state after commit"
                     << endl;
                snapshot_info.dump();
                var.dump();
                trans.dump();
                cerr << "-------------" << endl;
#endif

                //cerr << "var.history.size() = " << var.history.entries.size()
                //     << endl;
            
                if (var.read() % 2 != 0) {
                    ++errors;
                    cerr << "val should be even after trans: " << var.read()
                         << endl;
                }
            }

#if 0
            cerr << "-------------" << endl << "state after trans destroyed"
                 << endl;
            snapshot_info.dump();
            var.dump();
            cerr << "-------------" << endl;
#endif
            
            if (var.read() % 2 != 0) {
                ++errors;
                cerr << "val should be even after trans: " << var.read()
                     << endl;
            }
            
            //BOOST_CHECK_EQUAL(var.read() % 2, 0);
        }

        int new_val = var.read();
        if (new_val <= old_val) {
            ++errors;
            cerr << "no progress made: " << new_val << " <= " << old_val
                 << endl;
        }
        //BOOST_CHECK(var.read() > old_val);
    }

    static Lock lock;
    Guard guard(lock);

    BOOST_CHECK_EQUAL(errors, 0);

    failures += local_failures;
}

void run_object_test(int nthreads, int niter)
{
    cerr << "testing with " << nthreads << " threads and " << niter << " iter"
         << endl;
    Versioned<int> val(0);
    boost::barrier barrier(nthreads);
    boost::thread_group tg;

    size_t failures = 0;

    Timer timer;
    for (unsigned i = 0;  i < nthreads;  ++i)
        tg.create_thread(boost::bind(&object_test_thread, boost::ref(val),
                                     niter,
                                     boost::ref(barrier),
                                     boost::ref(failures)));
    
    tg.join_all();

    cerr << "elapsed: " << timer.elapsed() << endl;

    cerr << "val.history.entries.size() = " << val.history.size()
         << endl;

    cerr << "current_epoch = " << get_current_epoch() << endl;
    cerr << "failures: " << failures << endl;

#if 0
    cerr << "current_epoch: " << current_epoch << endl;
    for (unsigned i = 0;  i < val.history.entries.size();  ++i)
        cerr << "value at epoch " << val.history.entries[i].epoch << ": "
             << val.history.entries[i].value << endl;
#endif

    BOOST_CHECK_EQUAL(val.history.size(), 1);
    BOOST_CHECK_EQUAL(val.read(), niter * nthreads * 2);
}


BOOST_AUTO_TEST_CASE( test1 )
{
    //run_object_test(1, 10000);
    //run_object_test(10, 1000);
    run_object_test(1, 100000);
    run_object_test(10, 10000);
    run_object_test(100, 1000);
    run_object_test(1000, 100);
}

struct Object_Test_Thread2 {
    Versioned<int> * vars;
    int nvars;
    int iter;
    boost::barrier & barrier;
    size_t & failures;

    Object_Test_Thread2(Versioned<int> * vars,
                        int nvars,
                        int iter, boost::barrier & barrier,
                        size_t & failures)
        : vars(vars), nvars(nvars), iter(iter), barrier(barrier),
          failures(failures)
    {
    }

    void operator () ()
    {
        // Wait for all threads to start up before we continue
        barrier.wait();
        
        int errors = 0;
        int local_failures = 0;
        
        for (unsigned i = 0;  i < iter;  ++i) {
            // Keep going until we succeed
            int var1 = random() % nvars, var2 = random() % nvars;
            
            bool succeeded = false;

            while (!succeeded) {
                Local_Transaction trans;
                
                // Now that we're inside, the total should be zero
                ssize_t total = 0;
                for (unsigned i = 0;  i < nvars;  ++i)
                    total += vars[i].read();
                if (total != 0) {
                    cerr << "total is " << total << endl;
                    ++errors;
                }
                
                int & val1 = vars[var1].mutate();
                int & val2 = vars[var2].mutate();
                    
                val1 -= 1;
                val2 += 1;
                
                succeeded = trans.commit();
                local_failures += !succeeded;
            }
        }

        static Lock lock;
        Guard guard(lock);
        
        BOOST_CHECK_EQUAL(errors, 0);
        
        failures += local_failures;
    }
};

void run_object_test2(int nthreads, int niter, int nvals)
{
    cerr << "testing with " << nthreads << " threads and " << niter << " iter"
         << endl;
    Versioned<int> vals[nvals];
    boost::barrier barrier(nthreads);
    boost::thread_group tg;

    size_t failures = 0;

    Timer timer;
    for (unsigned i = 0;  i < nthreads;  ++i)
        tg.create_thread(Object_Test_Thread2(vals, nvals, niter,
                                             barrier, failures));
    
    tg.join_all();

    cerr << "elapsed: " << timer.elapsed() << endl;

    ssize_t total = 0;
    for (unsigned i = 0;  i < nvals;  ++i)
        total += vals[i].read();

    BOOST_CHECK_EQUAL(snapshot_info.entries.size(), 0);

    BOOST_CHECK_EQUAL(total, 0);
    for (unsigned i = 0;  i < nvals;  ++i) {
        if (vals[i].history.size() != 1)
            vals[i].dump();
        BOOST_CHECK_EQUAL(vals[i].history.size(), 1);
    }
}


BOOST_AUTO_TEST_CASE( test2 )
{
    cerr << endl << endl << "========= test 2: multiple variables" << endl;
    
    run_object_test2(1, 10, 1);
    //run_object_test2(2, 20, 10);
    run_object_test2(2,  50000, 2);
    run_object_test2(10, 10000, 100);
    run_object_test2(100, 1000, 10);
    run_object_test2(1000, 100, 100);
}


