/** spinlock.h                                                     -*- C++ -*-
    Jeremy Barnes, 13 December 2009.  All rights reserved.
    Implementation of a spinlock.
*/

#ifndef __jmvcc__spinlock_h__
#define __jmvcc__spinlock_h__

namespace JMVCC {

struct Spinlock {
    Spinlock()
        : value(0)
    {
    }

    int acquire()
    {
        for (int tries = 0; true;  ++tries) {
            if (__sync_bool_compare_and_swap(&value, 0, 1))
                return 0;
            if (tries == 100) {
                tries = 0;
                sched_yield();
            }
        }
    }

    int release()
    {
        __sync_lock_release(&value);
        return 0;
    }

    volatile int value;
};

} // namespace JMVCC

#endif /* __jmvcc__spinlock_h__ */

