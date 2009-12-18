/* garbage.cc
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#include "garbage.h"

using namespace std;

namespace JMVCC {

void enter_critical()
{
}

void leave_critical()
{
}

void schedule_cleanup(const boost::function<void ()> & cleanup)
{
}


} // namespace JMVCC
