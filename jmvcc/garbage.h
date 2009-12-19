/* garbage.h                                                       -*- C++ -*-
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#ifndef __jmvcc__garbage_h__
#define __jmvcc__garbage_h__


#include <boost/function.hpp>


namespace JMVCC {

void enter_critical();

void leave_critical();


// Same as enter_critical() then leave_critical()
void new_critical();

void schedule_cleanup(const boost::function<void ()> & cleanup);

} // namespace JMVCC

#endif /* __jmvcc__garbage_h__ */
