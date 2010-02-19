/* garbage.h                                                       -*- C++ -*-
   Jeremy Barnes, 17 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Garbage collection functionality.
*/

#ifndef __jmvcc__garbage_h__
#define __jmvcc__garbage_h__


#include <boost/function.hpp>
#include <boost/bind.hpp>
#include "jml/arch/atomic_ops.h"
#include "jml/arch/cmp_xchg.h"


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

template<typename X>
struct Delete_Object {
    Delete_Object(X * x)
        : x(x)
    {
    }

    X * x;

    void operator () ()
    {
        delete x;
    }
};

typedef boost::function<void ()> Cleanup;

/// Schedule a cleanup.  Has to be called when in a critical section.
void schedule_cleanup(const Cleanup & cleanup);


// Debug only
void set_debug_mode(bool debug_mode_on);
int get_num_in_critical();
int get_num_cleanups_outstanding();
void check_invariants();



template<class Data, class Deleter>
struct RCU {

    RCU(Data * data = 0)
        : data(data)
    {
    }

    ~RCU()
    {
        Deleter d;
        
        if (data != 0)
            schedule_cleanup(boost::bind(d, data));
    }

    const Data * read() const
    {
        // TODO: memory barriers?
        // TODO: check in critical section?
        return reinterpret_cast<const Data *>(data);
    }

    const Data * operator -> () const
    {
        return *read();
    }

    const Data * operator * () const
    {
        return read();
    }

    bool publish(const Data * old_data, Data * new_data)
    {
        // For the moment, the commit lock is held when we update this, so
        // there is no possibility of conflict.  But if ever we decide to
        // allow for parallel commits, then we need to be more careful here
        // to do it atomically.
        ML::memory_barrier();

        bool result = cmp_xchg(reinterpret_cast<Data * &>(data),
                               const_cast<Data * &>(old_data),
                               new_data);

        if (!result) {
            Deleter d;
            d(new_data);
        }
        else schedule_cleanup(boost::bind(Deleter(),
                                          const_cast<Data *>(old_data)));
        
        return result;
    }


private:
    mutable Data * data;
};

} // namespace JMVCC

#endif /* __jmvcc__garbage_h__ */
