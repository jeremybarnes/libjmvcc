/* snapshot.h                                                      -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Snapshot functionality.
*/

#ifndef __jmvcc__snapshot_h__
#define __jmvcc__snapshot_h__

#include "jml/arch/exception.h"
#include <map>
#include <set>
#include <vector>
#include <iostream>
#include <ace/Mutex.h>
#include <ace/Synch.h>
#include "jml/utils/string_functions.h"
#include <boost/utility.hpp>
#include "jmvcc_defs.h"
#include "spinlock.h"

class test0;   // for testing code

namespace JMVCC {

template<class Var> void test0_type();  // testing code

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
    if (val < current_epoch_)
        throw Exception("current_epoch_ is decreasing");
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
    if (val > current_epoch_) {
        throw Exception("earliest epoch after current epoch");
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

    void register_cleanup(Versioned_Object * obj,
                          Epoch valid_from_to_cleanup);

    void dump(std::ostream & stream = std::cerr);

    void validate() const
    {
        ACE_Guard<Mutex> guard(lock);
        validate_unlocked();
    }

    size_t entry_count() const { return entries.size(); }

    /** Compress a range of epochs to remove holes from the epoch space and
        start back at zero.  Used once the epochs start to get too high:
        we can't allow a wrap around, and we would prefer not to use
        64 bits.
    */
    void compress_epochs();

    /** For testing.  Check if the given epoch has the given object in it,
        and returns the valid_from of that object.  Slow and inefficient. */
    Epoch has_cleanup(Epoch snapshot_epoch,
                      const Versioned_Object * object) const;

private:
    typedef ACE_Mutex Mutex;
    mutable Mutex lock;

    struct Cleanup_Entry {
        Cleanup_Entry(Versioned_Object * object = 0,
                      Epoch valid_from = 0)
            : object(object), valid_from(valid_from)
        {
        }

        Versioned_Object * object;
        Epoch valid_from;
    };

    typedef std::vector<Cleanup_Entry> Cleanups;

    struct Entry {
        std::set<Snapshot *> snapshots;
        Cleanups cleanups;

        void add_cleanup(const Cleanup_Entry & cleanup);
        mutable Spinlock lock;
    };

    typedef std::map<Epoch, Entry> Entries;
    Entries entries;

    void dump_unlocked(std::ostream & stream = std::cerr);

    void validate_unlocked() const;

    void perform_cleanup(Entries::iterator it, ACE_Guard<Mutex> & guard);
    
    friend class ::test0;
    template<class Var> friend void test0_type();
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

    void set_epoch(Epoch new_epoch);

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
