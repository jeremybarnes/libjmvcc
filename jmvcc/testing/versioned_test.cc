/* versioned_test.cc
   Jeremy Barnes, 14 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.
   
   Test of versioned objects.
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
    current_epoch_ = 600;

    Versioned<int> var(0);

    BOOST_CHECK_EQUAL(var.history_size(), 0);
    BOOST_CHECK_EQUAL(var.read(), 0);

    auto_ptr<Transaction> t1(new Transaction());
    auto_ptr<Transaction> t2(new Transaction());
    auto_ptr<Transaction> t2a(new Transaction());

    BOOST_CHECK_EQUAL(get_current_epoch(), 600);

    {
        current_trans = t1.get();
        
        int & v = var.mutate();
        BOOST_CHECK_EQUAL(v, 0);
        v = 1;
        BOOST_CHECK_EQUAL(v, 1);
        BOOST_CHECK_EQUAL(var.read(), 1);
        BOOST_CHECK_EQUAL(var.history_size(), 0);

        BOOST_CHECK(t1->commit());

        BOOST_CHECK_EQUAL(var.read(), 1);
        BOOST_CHECK_EQUAL(var.history_size(), 1);

        current_trans = 0;
    }

    BOOST_CHECK_EQUAL(var.read(), 1);
    BOOST_CHECK_EQUAL(var.history_size(), 1);

    {
        current_trans = t2.get();

        BOOST_CHECK_EQUAL(var.read(), 0);

        current_trans = 0;
    }

    auto_ptr<Transaction> t3(new Transaction());

    {
        current_trans = t3.get();

        BOOST_CHECK_EQUAL(var.read(), 1);

        int & v = var.mutate();
        BOOST_CHECK_EQUAL(v, 1);
        v = 2;
        BOOST_CHECK_EQUAL(v, 2);
        BOOST_CHECK_EQUAL(var.read(), 2);
        BOOST_CHECK_EQUAL(var.history_size(), 1);

        BOOST_CHECK(t3->commit());

        BOOST_CHECK_EQUAL(var.read(), 2);
        BOOST_CHECK_EQUAL(var.history_size(), 2);

        current_trans = 0;
    }
    
    BOOST_CHECK_EQUAL(var.read(), 2);
    BOOST_CHECK_EQUAL(var.history_size(), 2);

    {
        current_trans = t1.get();
        BOOST_CHECK_EQUAL(var.read(), 1);
        current_trans = 0;
    }

    {
        current_trans = t2.get();
        snapshot_info.dump();
        var.dump();
        BOOST_CHECK_EQUAL(var.read(), 0);
        current_trans = 0;
    }

    // Deleting this transaction shouldn't cause anything to disappear as
    // there is another (t2) with the same epoch
    delete t2a.release();

    BOOST_CHECK_EQUAL(var.read(), 2);
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

    delete t1.release();

    BOOST_CHECK_EQUAL(var.read(), 2);
    BOOST_CHECK_EQUAL(var.history_size(), 1);

    {
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
}

