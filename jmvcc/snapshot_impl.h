/* snapshot_impl.h                                                 -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Implementation of functions in the snapshot.
*/

#ifndef __jmvcc__snapshot_impl_h__
#define __jmvcc__snapshot_impl_h__

namespace JMVCC {


/*****************************************************************************/
/* SNAPSHOT                                                                  */
/*****************************************************************************/

inline
Snapshot::
Snapshot()
    : retries_(0), status(UNINITIALIZED)
{
    register_me();
}

inline
Snapshot::
~Snapshot()
{
    snapshot_info.remove_snapshot(this);
}

inline
void
Snapshot::
restart()
{
    status = RESTARTING;
    ++retries_;
    if (get_current_epoch() != epoch_) {
        snapshot_info.remove_snapshot(this);
        register_me();
    }
}

inline
void
Snapshot::
register_me()
{
    snapshot_info.register_snapshot(this);

    if (status == UNINITIALIZED)
        status = INITIALIZED;
    else if (status == RESTARTING)
        status = RESTARTED;
}


} // namespace JMVCC


#endif /* __jmvcc__snapshot_impl_h__ */
