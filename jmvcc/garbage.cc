/* garbage.cc
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#include "garbage.h"

using namespace std;

namespace JMVCC {

volatile size_t critical_counter = 1;

__thread size_t current_critical = 0;
__thread size_t current_nesting = 0;


void enter_critical()
{
    if (current_critical != 0) {
        ++current_nesting;
        return;
    }
}

void leave_critical()
{
    if (current_nesting == 0)
        throw Exception("badly nested critical sections");
    --current_nesting;
    if (current_nesting == 0) {
        // We just left the critical section
        // do processing...
    }
}



void schedule_cleanup(const boost::function<void ()> & cleanup)
{
}


} // namespace JMVCC
