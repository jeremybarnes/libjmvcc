/* epoch_compression_test.cc
   Jeremy Barnes, 21 September 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Test for the epoch compression functionality.
*/

#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include "jml/utils/string_functions.h"
#include "jml/utils/vector_utils.h"
#include "jml/utils/pair_utils.h"
#include <boost/test/unit_test.hpp>
#include <boost/bind.hpp>
#include <iostream>
#include <boost/thread.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/timer.hpp>
#include "jml/arch/exception_handler.h"
#include "jml/arch/threads.h"
#include <set>
#include "jml/arch/timers.h"
#include "jml/arch/backtrace.h"
#include <sched.h>
#include "jmvcc/transaction.h"
#include "jmvcc/versioned.h"

using namespace ML;
using namespace JMVCC;
using namespace std;

using boost::unit_test::test_suite;

BOOST_AUTO_TEST_CASE( test0 )
{
    BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);
    
    current_epoch_ = 600;
    earliest_epoch_ = 600;

    Versioned<int> var(0);

    BOOST_CHECK_EQUAL(var.history_size(), 0);
    BOOST_CHECK_EQUAL(var.read(), 0);

    auto_ptr<Transaction> t1(new Transaction());
    BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 1);

    BOOST_CHECK_EQUAL(get_current_epoch(), 600);
    BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);

    BOOST_CHECK_NO_THROW(snapshot_info.compress_epochs());

    BOOST_CHECK_EQUAL(var.read(), 0);
    BOOST_CHECK_EQUAL(t1->epoch(), 1);
    BOOST_CHECK_EQUAL(get_current_epoch(), 2);
    BOOST_CHECK_EQUAL(get_earliest_epoch(), 1);

    delete t1.release();

    BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);
}

struct In_Trans_Context : public In_Out_Critical {
    Transaction * old_trans;

    In_Trans_Context(Transaction * t)
    {
        old_trans = current_trans;
        current_trans = t;
    }

    In_Trans_Context(const auto_ptr<Transaction> & t)
    {
        old_trans = current_trans;
        current_trans = t.get();
    }

    ~In_Trans_Context()
    {
        current_trans = old_trans;
    }
    
};

void run_test(int test_num)
{
    cerr << "test_num = " << test_num << endl;

    BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);
    
    current_epoch_ = 600;
    earliest_epoch_ = 600;

    Versioned<int> var(0);

    BOOST_CHECK_EQUAL(var.history_size(), 0);
    {
        Local_Transaction trans;
        BOOST_CHECK_EQUAL(var.read(), 0);
    }

    auto_ptr<Transaction> t1(new Transaction());
    auto_ptr<Transaction> t2(new Transaction());
    auto_ptr<Transaction> t0(new Transaction());

    BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 1);

    BOOST_CHECK_EQUAL(get_current_epoch(), 600);
    BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);

    {
        In_Trans_Context trans(t1);

        for (unsigned i = 0;  i < 20;  ++i) {
            int & v = var.mutate();
            v += 1;
            BOOST_CHECK_EQUAL(t1->commit(), true);
        }
    }

    BOOST_CHECK_EQUAL(get_current_epoch(), 620);
    BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);

    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 2);

    {
        Local_Transaction trans;
        BOOST_CHECK_EQUAL(var.read(), 20);
    }
    BOOST_CHECK_EQUAL(var.history_size(), 1);

    {
        In_Trans_Context trans(t2);

        BOOST_CHECK_EQUAL(var.read(), 0);

        {
            int & v = var.mutate();
            v += 1;
            BOOST_CHECK_EQUAL(t2->commit(), false);
        }

        BOOST_CHECK_EQUAL(var.read(), 20);

        for (unsigned i = 0;  i < 20;  ++i) {
            int & v = var.mutate();
            v += 1;
            BOOST_CHECK_EQUAL(t2->commit(), true);
        }
        
        BOOST_CHECK_EQUAL(var.read(), 40);
    }

    {
        Local_Transaction trans;
        BOOST_CHECK_EQUAL(var.read(), 40);
    }
    BOOST_CHECK_EQUAL(var.history_size(), 2);

    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 3);

    BOOST_CHECK_EQUAL(get_current_epoch(), 640);
    BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);

    auto_ptr<Transaction> t3(new Transaction());

    {
        In_Trans_Context trans(t3);

        BOOST_CHECK_EQUAL(var.read(), 40);

        for (unsigned i = 0;  i < 20;  ++i) {
            int & v = var.mutate();
            v += 1;
            BOOST_CHECK_EQUAL(t3->commit(), true);
        }

        BOOST_CHECK_EQUAL(var.read(), 60);
    }
    
    {
        Local_Transaction trans;
        BOOST_CHECK_EQUAL(var.read(), 60);
    }
    BOOST_CHECK_EQUAL(var.history_size(), 3);

    BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 4);

    BOOST_CHECK_EQUAL(get_current_epoch(), 660);
    BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);

    {
        In_Trans_Context trans(t1);
        BOOST_CHECK_EQUAL(var.read(), 20);
    }

    {
        In_Trans_Context trans(t2);
        BOOST_CHECK_EQUAL(var.read(), 40);
    }

    {
        In_Trans_Context trans(t3);
        BOOST_CHECK_EQUAL(var.read(), 60);
    }

    {
        In_Trans_Context trans(t0);
        BOOST_CHECK_EQUAL(var.read(), 0);
    }

    if (test_num > 2) {
        BOOST_CHECK_NO_THROW(snapshot_info.compress_epochs());
        BOOST_CHECK_EQUAL(get_current_epoch(), 5);
        BOOST_CHECK_EQUAL(get_earliest_epoch(), 1);
        BOOST_CHECK_EQUAL(t0->epoch(), 1);
        BOOST_CHECK_EQUAL(t1->epoch(), 2);
        BOOST_CHECK_EQUAL(t2->epoch(), 3);
        BOOST_CHECK_EQUAL(t3->epoch(), 4);

        cerr << "------------ var after renaming" << endl;
        var.dump();
        cerr << "------------ end var after renaming" << endl;
    }

    {
        In_Trans_Context trans(t1);
        BOOST_CHECK_EQUAL(var.read(), 20);
    }

    {
        In_Trans_Context trans(t2);
        BOOST_CHECK_EQUAL(var.read(), 40);
    }

    {
        In_Trans_Context trans(t3);
        BOOST_CHECK_EQUAL(var.read(), 60);
    }

    {
        In_Trans_Context trans(t0);
        BOOST_CHECK_EQUAL(var.read(), 0);
    }

    if (test_num == 1) {
        delete t0.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 620);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 3);
        BOOST_CHECK_EQUAL(var.history_size(), 2);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }
        
        delete t1.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 640);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 2);
        BOOST_CHECK_EQUAL(var.history_size(), 1);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t2.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 660);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 1);
        BOOST_CHECK_EQUAL(var.history_size(), 0);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t3.release();

        BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        return;
    }

    if (test_num == 2) {
        delete t3.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 3);
        BOOST_CHECK_EQUAL(var.history_size(), 3);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t2.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 2);
        BOOST_CHECK_EQUAL(var.history_size(), 2);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t1.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 600);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 1);
        BOOST_CHECK_EQUAL(var.history_size(), 1);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t0.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 660);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 0);
        BOOST_CHECK_EQUAL(var.history_size(), 0);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);

        return;
    }

    if (test_num == 3) {
        delete t0.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 2);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 3);
        BOOST_CHECK_EQUAL(var.history_size(), 2);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }
        
        delete t1.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 3);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 2);
        BOOST_CHECK_EQUAL(var.history_size(), 1);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t2.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 4);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 1);
        BOOST_CHECK_EQUAL(var.history_size(), 0);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t3.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 5);
        BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        return;
    }

    if (test_num == 4) {
        delete t3.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 3);
        BOOST_CHECK_EQUAL(var.history_size(), 3);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t2.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 2);
        BOOST_CHECK_EQUAL(var.history_size(), 2);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t1.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 1);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 1);
        BOOST_CHECK_EQUAL(var.history_size(), 1);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        delete t0.release();

        BOOST_CHECK_EQUAL(get_earliest_epoch(), 5);
        BOOST_CHECK_EQUAL(snapshot_info.entry_count(), 0);
        BOOST_CHECK_EQUAL(var.history_size(), 0);
        {
            Local_Transaction trans;
            BOOST_CHECK_EQUAL(var.read(), 60);
        }

        BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);

        return;
    }


#if 0
    snapshot_info.compress_epochs();


    // Deleting this transaction shouldn't cause anything to disappear as
    // there is another (t2) with the same epoch
    delete t0.release();

    {
        Local_Transaction trans;
        BOOST_CHECK_EQUAL(var.read(), 2);
    }

    BOOST_CHECK_EQUAL(var.history_size(), 2);

    {
        current_trans = t1.get();
        BOOST_CHECK_EQUAL(var.read(), 1);
        current_trans = 0;
    }

    {
        current_trans = t2.get();
        BOOST_CHECK_EQUAL(var.read(), 0);
        current_trans = 0;
    }
    
    {
        current_trans = t3.get();
        BOOST_CHECK_EQUAL(var.read(), 2);
        current_trans = 0;
    }
    
    // Delete transaction t1.  This should cause the value 1 to disappear.

    cerr << "--------------------------------" << endl;
    snapshot_info.dump();
    var.dump();

    delete t1.release();

    BOOST_CHECK_EQUAL(var.read(), 2);
    BOOST_CHECK_EQUAL(var.history_size(), 1);

    {
        cerr << "--------------------------------" << endl;
        current_trans = t2.get();
        snapshot_info.dump();
        var.dump();
        BOOST_CHECK_EQUAL(var.read(), 0);
        current_trans = 0;
    }
    
    {
        current_trans = t3.get();
        BOOST_CHECK_EQUAL(var.read(), 2);
        current_trans = 0;
    }

    // Delete transaction t3.  This shouldn't cause anything to disappear.

    delete t3.release();

    BOOST_CHECK_EQUAL(var.read(), 2);
    BOOST_CHECK_EQUAL(var.history_size(), 1);

    {
        current_trans = t2.get();
        BOOST_CHECK_EQUAL(var.read(), 0);
        current_trans = 0;
    }

    delete t2.release();

    BOOST_CHECK_EQUAL(var.read(), 2);
    BOOST_CHECK_EQUAL(var.history_size(), 0);
#endif

    BOOST_REQUIRE_EQUAL(snapshot_info.entry_count(), 0);
}

BOOST_AUTO_TEST_CASE( test1 )
{
    run_test(1);
}

BOOST_AUTO_TEST_CASE( test2 )
{
    run_test(2);
}

BOOST_AUTO_TEST_CASE( test3 )
{
    run_test(3);
}

BOOST_AUTO_TEST_CASE( test4 )
{
    run_test(3);
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
                    ACE_Guard<ACE_Mutex> guard(commit_lock);
                    cerr << "--------------- total not zero" << endl;
                    snapshot_info.dump();
                    cerr << "total is " << total << endl;
                    cerr << "trans.epoch() = " << trans.epoch() << endl;
                    ++errors;
                    for (unsigned i = 0;  i < nvars;  ++i)
                        vars[i].dump();
                    cerr << "--------------- end total not zero" << endl;
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

void epoch_compression_thread(volatile bool & finished)
{
    while (!finished) snapshot_info.compress_epochs();
}

void run_epoch_compression_test(int nthreads, int niter, int nvals)
{
    cerr << "testing with " << nthreads << " threads and " << niter << " iter"
         << endl;
    Versioned<int> vals[nvals];
    boost::barrier barrier(nthreads);
    boost::thread_group tg;

    size_t failures = 0;

    volatile bool finished = false;
    boost::thread_group tg2;
    tg2.create_thread(boost::bind(epoch_compression_thread,
                                  boost::ref(finished)));


    Timer timer;
    for (unsigned i = 0;  i < nthreads;  ++i)
        tg.create_thread(Object_Test_Thread2(vals, nvals, niter,
                                             barrier, failures));
    
    tg.join_all();

    cerr << "elapsed: " << timer.elapsed() << endl;

    finished = true;
    
    tg2.join_all();

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

#if 0

BOOST_AUTO_TEST_CASE( stress_test_epoch_compression )
{
    //run_epoch_compression_test(1, 10, 1);
    //run_epoch_compression_test(2, 20, 10);
    run_epoch_compression_test(2,  5000, 2);
    run_epoch_compression_test(10, 1000, 100);
    run_epoch_compression_test(100, 100, 10);
    run_epoch_compression_test(1000, 10, 100);

    boost::timer t;
    run_epoch_compression_test(1, 10000, 1);
    cerr << "elapsed for 1000000 iterations: " << t.elapsed() << endl;
    cerr << "for 2^32 iterations: " << (1ULL << 32) / 1000000.0 * t.elapsed()
         << "s" << endl;
}

#endif
