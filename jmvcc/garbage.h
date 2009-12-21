/* garbage.h                                                       -*- C++ -*-
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#ifndef __jmvcc__garbage_h__
#define __jmvcc__garbage_h__


#include <boost/function.hpp>


namespace JMVCC {

/* Garbage Collection

   This is a mechanism to defer cleanups of some memory until nothing can
   possibly be accessing it any more.  We try to do the cleanup as soon as
   possible in order to ...
*/

void enter_critical();

void leave_critical();

/// For testing; what is the number of the critical section?
size_t current_critical_section();

// Same as enter_critical() then leave_critical()
void new_critical();

void schedule_cleanup(const boost::function<void ()> & cleanup);

/// For testing; count how many cleanups need to be performed
void num_outstanding_cleanups();

} // namespace JMVCC

#endif /* __jmvcc__garbage_h__ */
