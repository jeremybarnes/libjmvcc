/* object_test.cc Jeremy Barnes, 21 September 2009 Copyright (c) 2009
   Jeremy Barnes.  All rights reserved.

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
#include <boost/timer.hpp>
#include "arch/exception_handler.h"
#include "arch/threads.h"
#include <set>
#include "arch/timers.h"
#include "arch/backtrace.h"
#include <sched.h>
#include "jmvcc/transaction.h"
#include "jmvcc/versioned.h"

using namespace ML;
using namespace JMVCC;
using namespace std;

using boost::unit_test::test_suite;

BOOST_AUTO_TEST_CASE( test0 )
{
    // Check basic invariants
    BOOST_CHECK_EQUAL(current_trans, (Transaction *)0);
    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 0);

    size_t starting_epoch = get_current_epoch();

    Versioned<int> myval(6);

    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 0);
    BOOST_CHECK_EQUAL(myval.history_size(), 0);
    BOOST_CHECK_EQUAL(myval.read(), 6);
    
    {
        // Should throw an exception when we mutate out of a transaction
        JML_TRACE_EXCEPTIONS(false);
        BOOST_CHECK_THROW(myval.mutate(), Exception);
    }

    // Check strong exception safety
    BOOST_CHECK_EQUAL(myval.history_size(), 0);
    BOOST_CHECK_EQUAL(myval.read(), 6);

    cerr << "------------------ at start" << endl;
    snapshot_info.dump();
    cerr << "------------------ end at start" << endl;
    
    // Create a transaction
    {
        Local_Transaction trans1;
        
        cerr << "&trans1 = " << &trans1 << endl;

        BOOST_CHECK_EQUAL(myval.history_size(), 0);
        BOOST_CHECK_EQUAL(myval.read(), 6);
        
        // Check that the snapshot is properly there
        BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entries.begin()->first, get_current_epoch());
        BOOST_REQUIRE_EQUAL(snapshot_info.entries.begin()->second.snapshots.size(), 1);
        BOOST_CHECK_EQUAL(*snapshot_info.entries.begin()->second.snapshots.begin(), &trans1);
        
        // Check that the correct value is copied over
        BOOST_CHECK_EQUAL(myval.mutate(), 6);

        // Check that we can increment it
        BOOST_CHECK_EQUAL(++myval.mutate(), 7);

        // Check that it was recorded
        BOOST_CHECK_EQUAL(trans1.num_local_values(), 1);

        // FOR TESTING, increment the current epoch
        set_current_epoch(get_current_epoch() + 1);

        // Restart the transaction; check that it was properly recorded by the
        // snapshot info
        trans1.restart();

        // Check that the snapshot is properly there
        BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entries.begin()->first,
                          get_current_epoch());
        BOOST_REQUIRE_EQUAL(snapshot_info.entries.begin()->second.snapshots.size(), 1);
        BOOST_CHECK_EQUAL(*snapshot_info.entries.begin()->second.snapshots.begin(), &trans1);
        
        // Finish the transaction without committing it
    }

    cerr << "------------------ at end" << endl;
    snapshot_info.dump();
    cerr << "------------------ end at end" << endl;

    BOOST_CHECK_EQUAL(myval.history_size(), 0);
    BOOST_CHECK_EQUAL(myval.read(), 6);
    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 0);
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

                //cerr << "var.history_size() = " << var.history.entries.size()
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
            var.dump();
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

    cerr << "val.history.entries.size() = " << val.history_size()
         << endl;

    cerr << "current_epoch = " << get_current_epoch() << endl;
    cerr << "failures: " << failures << endl;

#if 0
    cerr << "current_epoch: " << current_epoch << endl;
    for (unsigned i = 0;  i < val.history.entries.size();  ++i)
        cerr << "value at epoch " << val.history.entries[i].epoch << ": "
             << val.history.entries[i].value << endl;
#endif

    BOOST_CHECK_EQUAL(val.history_size(), 0);
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

    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 0);

    BOOST_CHECK_EQUAL(total, 0);
    for (unsigned i = 0;  i < nvals;  ++i) {
        if (vals[i].history_size() != 0)
            vals[i].dump();
        BOOST_CHECK_EQUAL(vals[i].history_size(), 0);
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

    boost::timer t;
    run_object_test2(1, 1000000, 1);
    cerr << "elapsed for 1000000 iterations: " << t.elapsed() << endl;
    cerr << "for 2^32 iterations: " << (1ULL << 32) / 1000000.0 * t.elapsed()
         << "s" << endl;
}



