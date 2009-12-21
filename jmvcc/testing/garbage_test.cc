/* garbage_test.cc
   Jeremy Barnes, 20 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Test for garbage collection.
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
#include "jmvcc/garbage.h"
#include "arch/demangle.h"

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
        var = val;
    }
};

BOOST_AUTO_TEST_CASE(test1)
{
    int v = 0;

    enter_critical();

    schedule_cleanup(Set_Var(v, 1));

    BOOST_CHECK_EQUAL(v, 0);

    leave_critical();

    BOOST_CHECK_EQUAL(v, 1);
}

size_t num_live = 0;
size_t max_num_live = 0;


struct Checked_Object {
    Checked_Object(int val)
        : val(val), magic(232910)
    {
        
    }

    ~Checked_Object()
    {
        if (magic != 232910)
            throw Exception("wrong magic");
        magic = 19283;
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

struct Garbage_Torture_Thread {

    boost::barrier & barrier;
    int niter;
    int thread;
    Checked_Object ** vals;

    Garbage_Torture_Thread(boost::barrier & barrier, int niter, int nthreads,
                           int thread, Checked_Object ** vals)
        : barrier(barrier), niter(niter), thread(thread), vals(vals)
    {
    }

    void operator () ()
    {
        // Wait for all threads to start up before we continue
        barrier.wait();
        
        for (unsigned iter = 0;  iter < niter;  ++iter) {
            enter_critical();
            leave_critical();
        }
    }
};

void run_garbage_test(int nthreads, int niter)
{
    cerr << "testing garbage with " << nthreads << " threads" << endl;
    boost::barrier barrier(nthreads);
    boost::thread_group tg;

    Checked_Object * vals[nthreads];

    Timer timer;
    for (unsigned i = 0;  i < nthreads;  ++i)
        tg.create_thread(Garbage_Torture_Thread(barrier, niter, nthreads, i,
                                                vals));
    
    tg.join_all();
    
    cerr << "elapsed: " << timer.elapsed() << endl;
}

BOOST_AUTO_TEST_CASE(garbage_torture)
{
    run_garbage_test(1, 10000);
    run_garbage_test(2,  5000);
    run_garbage_test(10, 1000);
}
