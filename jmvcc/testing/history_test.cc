/* history_test.cc
   Jeremy Barnes, 13 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Test of the history class.
*/

#define BOOST_TEST_MAIN
#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>
#include <iostream>
#include <boost/thread.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/bind.hpp>
#include "arch/exception_handler.h"
#include "jmvcc/history.h"
#include "utils/testing/live_counting_obj.h"

using namespace ML;
using namespace JMVCC;
using namespace std;


using boost::unit_test::test_suite;


BOOST_AUTO_TEST_CASE( test0 )
{
    History<Obj> h;
    BOOST_CHECK_EQUAL(h.size(), 0);

}
