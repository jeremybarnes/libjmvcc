/* garbage_test.cc
   Jeremy Barnes, 20 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Test for garbage collection.
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
#include "jmvcc/garbage.h"
#include "jml/arch/demangle.h"
#include "jml/arch/atomic_ops.h"


using namespace ML;
using namespace JMVCC;
using namespace std;

struct Set_Var {
    Set_Var(int & var, int val)
        : var(var), val(val)
    {
    }

    int & var;
    int val;

    void operator () ()
    {
        cerr << "setting var at " << this << " from " << var
             << " to " << val << endl;
        var = val;
    }
};

BOOST_AUTO_TEST_CASE(test1)
{
    int v = 0;

    cerr << "before critical" << endl;

    enter_critical();

    cerr << "in critical" << endl;

    schedule_cleanup(Set_Var(v, 1));

    cerr << "after cleanup" << endl;

    BOOST_CHECK_EQUAL(v, 0);

    leave_critical();

    cerr << "out of critical" << endl;

    BOOST_CHECK_EQUAL(v, 1);
}

size_t num_live = 0;
size_t max_num_live = 0;

struct Checked_Object {
    Checked_Object(int val)
        : val(val), magic(232910)
    {
        atomic_add(num_live, 1);

        if (num_live > max_num_live)
            max_num_live = num_live;
    }

    ~Checked_Object()
    {
        if (magic != 232910)
            throw Exception("wrong magic");
        magic = 19283;

        atomic_add(num_live, -1);
    }

    int get() const
    {
        if (magic != 232910)
            throw Exception("wrong magic");
        return val;
    }

    int val;
    int magic;
};

void microsleep(double seconds)
{
    timespec ts = { trunc(seconds), (seconds - trunc(seconds)) * 1000000 };
    nanosleep(&ts, 0);
}

struct Garbage_Torture_Thread {

    boost::barrier & barrier;
    int niter;
    int nthreads;
    int thread;
    Checked_Object ** vals;
    int & errors;
    int mode;

    Garbage_Torture_Thread(boost::barrier & barrier, int niter, int nthreads,
                           int thread, Checked_Object ** vals,
                           int & errors,
                           int mode)
        : barrier(barrier), niter(niter), nthreads(nthreads),
          thread(thread), vals(vals),
          errors(errors), mode(mode)
    {
    }

    void operator () ()
    {
        int local_errors = 0;

        // Wait for all threads to start up before we continue
        barrier.wait();
        
        vector<int> old_values(nthreads);

        for (unsigned iter = 0;  iter < niter;  ++iter) {
            enter_critical();

            for (unsigned i = 0;  i < nthreads;  ++i) {
                try {
                    int new_value = vals[i]->get();
                    if (new_value < old_values[i]) {
                        cerr << "read an old value: " << new_value
                             << " should be >= " << old_values[i]
                             << " thread " << thread << " i " << i << endl;
                        ++local_errors;
                    }
                } catch (const std::exception & exc) {
                    cerr << "caught exception: " << exc.what() << endl;
                    ++local_errors;
                }
            }

            Checked_Object * old = vals[thread];
            vals[thread] = new Checked_Object(iter);

            if (mode == 1 && thread > 0)
                microsleep(0.001);

            schedule_cleanup(Delete_Object<Checked_Object>(old));

            if (mode == 2 && thread > 0)
                microsleep(0.001);

            leave_critical();

            if (mode == 3 && thread > 0)
                microsleep(0.001);
            
        }

        atomic_add(errors, local_errors);
    }
};

void run_garbage_test(int nthreads, int niter, int mode)
{
    cerr << endl << endl;
    cerr << "testing garbage with " << nthreads << " threads" << endl;
    boost::barrier barrier(nthreads);
    boost::thread_group tg;

    Checked_Object * vals[nthreads];
    for (unsigned i = 0;  i < nthreads;  ++i)
        vals[i] = new Checked_Object(0);

    int errors = 0;

    Timer timer;
    for (unsigned i = 0;  i < nthreads;  ++i)
        tg.create_thread(Garbage_Torture_Thread(barrier, niter, nthreads, i,
                                                vals, errors, mode));
    
    tg.join_all();

    cerr << "garbage collector status at end" << endl;

    BOOST_CHECK_EQUAL(errors, 0);
    BOOST_CHECK_EQUAL(num_live, nthreads);

    for (unsigned i = 0;  i < nthreads;  ++i) {
        BOOST_CHECK_EQUAL(vals[i]->get(), niter - 1);
        delete vals[i];
    }

    BOOST_CHECK_EQUAL(num_live, 0);

    cerr << "max_num_live = " << max_num_live << endl;
    cerr << "elapsed: " << timer.elapsed() << endl;
}

void run_garbage_test_mode(int mode)
{
    cerr << endl << endl << "mode = " << mode << endl;
    run_garbage_test(1, 10, mode);
    run_garbage_test(1, 10000, mode);
    run_garbage_test(2,  50000, mode);
    run_garbage_test(10, 10000, mode);
    run_garbage_test(100, 1000, mode);
}

BOOST_AUTO_TEST_CASE(garbage_torture)
{
    run_garbage_test_mode(0);
    run_garbage_test_mode(1);
    run_garbage_test_mode(2);
    run_garbage_test_mode(3);
}
