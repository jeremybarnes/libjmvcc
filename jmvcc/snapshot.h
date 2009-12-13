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

namespace JMVCC {

using namespace ML;

class Snapshot;
class Versioned_Object;

/// Global variable giving the number of committed transactions since the
/// beginning of the program
extern volatile size_t current_epoch_;

inline size_t get_current_epoch()
{
    return current_epoch_;
}

inline void set_current_epoch(size_t val)
{
    current_epoch_ = val;
}

/// Global variable giving the earliest epoch for which there is a snapshot
extern size_t earliest_epoch_;

void set_earliest_epoch(size_t val)
{
    if (val < earliest_epoch_) {
        using namespace std;
        cerr << "val = " << val << endl;
        cerr << "earliest_epoch = " << earliest_epoch_ << endl;
        throw Exception("earliest epoch was not increasing");
    }
    earliest_epoch_ = val;
}

size_t get_earliest_epoch()
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
    typedef ACE_Mutex Mutex;
    mutable Mutex lock;

    struct Entry {
        std::set<Snapshot *> snapshots;
        std::vector<std::pair<Versioned_Object *, size_t> > cleanups;
    };

    typedef std::map<size_t, Entry> Entries;
    Entries entries;

    // Register the snapshot for the current epoch.  Returns the number of
    // the epoch it was registered under.
    size_t register_snapshot(Snapshot * snapshot);

    void remove_snapshot(Snapshot * snapshot);

    void register_cleanup(Versioned_Object * obj, size_t epoch_to_cleanup);

    void
    perform_cleanup(Entries::iterator it, ACE_Guard<Mutex> & guard);

    void dump(std::ostream & stream = std::cerr);

    void dump_unlocked(std::ostream & stream = std::cerr);

    void validate() const
    {
        ACE_Guard<Mutex> guard(lock);
        validate_unlocked();
    }

    void validate_unlocked() const;

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

std::ostream & operator << (std::ostream & stream, const Status & status)
{
    switch (status) {
    case UNINITIALIZED: return stream << "UNINITIALIZED";
    case INITIALIZED:   return stream << "INITIALIZED";
    case RESTARTING:    return stream << "RESTARTING";
    case RESTARTING0:   return stream << "RESTARTING0";
    case RESTARTING0A:  return stream << "RESTARTING0A";
    case RESTARTING0B:  return stream << "RESTARTING0B";
    case RESTARTING2:   return stream << "RESTARTING2";
    case RESTARTED:     return stream << "RESTARTED";
    case COMMITTING:    return stream << "COMMITTING";
    case COMMITTED:     return stream << "COMMITTED";
    case FAILED:        return stream << "FAILED";
    default:            return stream << ML::format("Status(%d)", status);
    }
}

/*****************************************************************************/
/* SNAPSHOT                                                                  */
/*****************************************************************************/

/** Structure about a snapshot.  Mostly, this just holds information to be
    registered in the Snapshot_Info structure where most of the work is
    done.
*/
struct Snapshot : boost::noncopyable {
    Snapshot()
        : retries_(0), status(UNINITIALIZED)
    {
        register_me();
    }

    ~Snapshot()
    {
        snapshot_info.remove_snapshot(this);
    }

    void restart()
    {
        status = RESTARTING;
        ++retries_;
        if (get_current_epoch() != epoch_) {
            snapshot_info.remove_snapshot(this);
            register_me();
        }
    }

    void register_me()
    {
        snapshot_info.register_snapshot(this);

        if (status == UNINITIALIZED)
            status = INITIALIZED;
        else if (status == RESTARTING)
            status = RESTARTED;
    }

    size_t epoch() const { return epoch_; }

    int retries() const { return retries_; }

private:
    friend class Snapshot_Info;
    size_t epoch_;  ///< Epoch at which snapshot was taken
    int retries_;

public:
    Status status;
};



} // namespace JMVCC

#include "snapshot_impl.h"

#endif /* __jmvcc__snapshot_h__ */
