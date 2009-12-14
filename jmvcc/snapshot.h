/* snapshot.h                                                      -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Snapshot functionality.
*/

#ifndef __jmvcc__snapshot_h__
#define __jmvcc__snapshot_h__

#include "arch/exception.h"
#include <map>
#include <set>
#include <vector>
#include <iostream>
#include "ace/Mutex.h"
#include "ace/Synch.h"
#include "utils/string_functions.h"
#include <boost/utility.hpp>
#include "jmvcc_defs.h"

class test0;   // for testing code

namespace JMVCC {

using namespace ML;

/// Global variable giving the number of committed transactions since the
/// beginning of the program
extern volatile Epoch current_epoch_;

inline Epoch get_current_epoch()
{
    return current_epoch_;
}

inline void set_current_epoch(Epoch val)
{
    current_epoch_ = val;
}

/// Global variable giving the earliest epoch for which there is a snapshot
extern Epoch earliest_epoch_;

inline void set_earliest_epoch(Epoch val)
{
    if (val < earliest_epoch_) {
        using namespace std;
        cerr << "val = " << val << endl;
        cerr << "earliest_epoch = " << earliest_epoch_ << endl;
        throw Exception("earliest epoch was not increasing");
    }
    earliest_epoch_ = val;
}

inline Epoch get_earliest_epoch()
{
    return earliest_epoch_;
}


/*****************************************************************************/
/* SNAPSHOT_INFO                                                             */
/*****************************************************************************/

/* WHEN WE DESTROY A SNAPSHOT, we can clean up the entries upon which only
   this snapshot depends.

   How to keep track of which snapshot a history enrtry depends upon:
   - With no transaction active, there should only be one single entry in
     each history
   - Each snapshot has a list of entries that can be cleaned up after it
     dies

 */

/// Information about transactions in progress
struct Snapshot_Info {
    // Register the snapshot for the current epoch.  Returns the number of
    // the epoch it was registered under.
    Epoch register_snapshot(Snapshot * snapshot);

    void remove_snapshot(Snapshot * snapshot);

    void register_cleanup(Versioned_Object * obj, Epoch epoch_to_cleanup);

    void dump(std::ostream & stream = std::cerr);

    void validate() const
    {
        ACE_Guard<Mutex> guard(lock);
        validate_unlocked();
    }

    size_t entry_count() const { return entries.size(); }

private:
    typedef ACE_Mutex Mutex;
    mutable Mutex lock;

    typedef std::vector<std::pair<Versioned_Object *, Epoch> > Cleanups;

    struct Entry {
        std::set<Snapshot *> snapshots;
        Cleanups cleanups;
    };

    typedef std::map<Epoch, Entry> Entries;
    Entries entries;

    /** Compress a range of epochs to remove holes from the epoch space and
        start back at zero.  Used once the epochs start to get too high:
        we can't allow a wrap around, and we would prefer not to use
        64 bits.
    */
    void compress_epochs();

    void dump_unlocked(std::ostream & stream = std::cerr);

    void validate_unlocked() const;

    void perform_cleanup(Entries::iterator it, ACE_Guard<Mutex> & guard);
    
    friend class ::test0;
};

extern Snapshot_Info snapshot_info;

/// A snapshot provides a view of all objects that is frozen at the moment
/// the shapshot was created.  Provides a read-only view.
///
/// Note that long-lived snapshots might be created (in order to take
/// hot backups or for replication).  We need to be efficient in order to
/// do so.

enum Status {
    UNINITIALIZED,
    INITIALIZED,
    RESTARTING,
    RESTARTING0,
    RESTARTING0A,
    RESTARTING0B,
    RESTARTING2,
    RESTARTED,
    COMMITTING,
    COMMITTED,
    FAILED
};

std::ostream & operator << (std::ostream & stream, const Status & status);

/*****************************************************************************/
/* SNAPSHOT                                                                  */
/*****************************************************************************/

/** Structure about a snapshot.  Mostly, this just holds information to be
    registered in the Snapshot_Info structure where most of the work is
    done.
*/
struct Snapshot : boost::noncopyable {
    Snapshot();

    ~Snapshot();

    void restart();

    Epoch epoch() const { return epoch_; }

    int retries() const { return retries_; }

    void rename_epoch(Epoch old_epoch, Epoch new_epoch);

private:
    friend class Snapshot_Info;
    Epoch epoch_;  ///< Epoch at which snapshot was taken
    int retries_;

    void register_me();

public:
    Status status;
};



} // namespace JMVCC

#include "snapshot_impl.h"

#endif /* __jmvcc__snapshot_h__ */
