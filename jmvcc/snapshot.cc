/* snapshot.cc
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.
*/

#include "snapshot.h"
#include "transaction.h"
#include "utils/pair_utils.h"


using namespace std;


namespace JMVCC {

volatile Epoch current_epoch_ = 1;
Epoch earliest_epoch_ = 1;

Snapshot_Info snapshot_info;


/*****************************************************************************/
/* SNAPSHOT_INFO                                                             */
/*****************************************************************************/

/* Obsolete Version Cleanups

   The goal of this code is to make sure that each version of each object
   gets cleaned up exactly once, at the point when the last snapshot that
   references the version is removed.

   One way to do this is to make sure that each version is either:
   a) the newest version of the object, or
   b) on a list of versions to clean up somewhere, or
   c) cleaned up

   Here, we describe how we maintain and shuffle these lists.

   Snapshot to Version Mapping
   ---------------------------

   Each version will have one or more snapshots that sees it (the exception is
   the newest version of an object, which may not have any snapshots that see
   it).

   versions    snapshots
   --------    ---------
        v0
                  s10
                  s15

       v20        s20
                  s30
                  s40

       v50
                  s70

       v80
                  s90
                  s600

   In this diagram, we have 4 versions of the object (v0, v20, v50 and v80)
   and 6 snapshots.  A version is visible to all snapshots that have an
   epoch >= the version number but < the next version number.  So v0 is
   visible to s10 and s15; v20 is visible to s20, s30 and s40; v40 is visible
   to s70 and v80 is visible to s90 and s600.

   We need to make sure that the version is cleaned up when the *last*
   snapshot that refers to it is destroyed.

   The way that we do this is as follows.  We assume that a later snapshot
   will live longer than an earlier one, and so we put the version to destroy
   on the list for the latest snapshot.  So we have the following lists of
   objects to clean up:

   versions    snapshots    tocleanup
   --------    ---------    ---------
        v0
                  s10
                  s15       v0

       v20        s20
                  s30
                  s40       v20

       v50
                  s70       v50

       v80
                  s90
                  s600
   
   Note that v80, as the most recent value, is not on any free list.
   When snapshot 20 is destroyed, there is nothing to clean up and so it
   simply is removed.  Same story for snapshot 30; now when snapshot 40 is
   destroyed it will clean up v20.

   However, there is no guarantee that the order of creation of the snapshots
   will be the reverse order of destruction.  Let's consider what happens
   if snapshot 40 finishes before snapshot 30 and snapshot 20.  In this case,
   it is not correct to clean up v20 as s20 and s30 still refer to it.  Instead,
   it needs to be moved to the cleanup list for s30.  We know that the version
   is still referenced because the epoch for the version (20) is less than or
   equal to the epoch for the previous snapshot (30).

   As a result, we simply move it to the cleanup list for s30.

   versions    snapshots    tocleanup      deleted
   --------    ---------    ---------      -------
        v0
                  s10
                  s15       v0

       v20        s20
                  s30       v20
                                           s40       

       v50
                  s70       v50

       v80
                  s90
                  s600
   
   Thus, the invariant is that a version will always be on the cleanup list of
   the latest snapshot that references it.
   
   When we cleanup, we look at the previous snapshot.  If the epoch of that
   snapshot is >= the epoch for our version, then we move it to the free
   list of that snapshot.  Otherwise, we clean it up.

   Finally, when we create a new version, we need to arrange for the previous
   most recent version to go onto a free list.  Consider a new version of the
   object on epoch 900:

   versions    snapshots    tocleanup      deleted
   --------    ---------    ---------      -------
        v0
                  s10
                  s15       v0

       v20        s20
                  s30       v20
                                           s40       

       v50
                  s70       v50

       v80
                  s90
                  s600      v80 <-- added
      v900
*/

Epoch
Snapshot_Info::
register_snapshot(Snapshot * snapshot)
{
    ACE_Guard<Mutex> guard(lock);
    snapshot->epoch_ = get_current_epoch();

    // TODO: since we know it will be inserted at the end, we can do a more
    // efficient lookup that only looks at the end.

    Entries::iterator previous_most_recent;
    if (entries.empty()) previous_most_recent = entries.end();
    else previous_most_recent = boost::prior(entries.end());

    entries[snapshot->epoch_].snapshots.insert(snapshot);

    /* INVARIANT: a registered snapshot should always go at the end of the
       list of snapshots; it is new and should therefore always be the last
       one.  We check it here. */
    Entries::iterator it = entries.find(snapshot->epoch_);
    if (it == entries.end())
        throw Exception("inserted but not found");
    if (it != boost::prior(entries.end())) {
        cerr << "stale snapshot" << endl;
        dump_unlocked();
        cerr << "snapshot->epoch_ = " << snapshot->epoch_ << endl;
        throw Exception("inserted stale snapshot");
    }

    /* Since we don't clean up anything based upon the most recent snapshot,
       we now need to look at what was the most recent snapshot and see if
       it needs to be cleaned up. */
    if (previous_most_recent != it && previous_most_recent != entries.end()) {
        /* Do we need to clean it up? */
        // NOTE: calling this function RELEASES the lock; we can't tough
        // entries after.
        if (previous_most_recent->second.snapshots.empty())
            perform_cleanup(previous_most_recent, guard);
    }

    return snapshot->epoch_;
}

void Snapshot_Info::Entry::
add_cleanup(const std::pair<Versioned_Object *, Epoch> & cleanup)
{
    ACE_Guard<Spinlock> guard(lock);
    cleanups.push_back(cleanup);
}

void
Snapshot_Info::
remove_snapshot(Snapshot * snapshot)
{
    snapshot->status = RESTARTING0;

    ACE_Guard<Mutex> guard(lock);

    if (entries.empty())
        throw Exception("remove_snapshot: empty entries");
    
    snapshot->status = RESTARTING0A;
    
    Entries::iterator it = entries.find(snapshot->epoch());
    if (it == entries.end()) {
        cerr << "-------- snapshot not found -----------" << endl;
        cerr << "snapshot = " << snapshot << endl;
        cerr << "current_trans = " << current_trans << endl;
        cerr << "snapshot->epoch() = " << snapshot->epoch() << endl;
        snapshot_info.dump_unlocked();
        //snapshot->dump();
        if (current_trans)
            current_trans->dump();
        cerr << "-------- end snapshot not found -----------" << endl;
            throw Exception("snapshot not found");
    }
    
    /* Is this the most recent snapshot?  If so, we can't clean up,
       even if we're removing the last entry, as there might be a
       new snapshot created with the same epoch. */
    //bool most_recent = (it == boost::prior(entries.end()));
    
    Entry & entry = it->second;
    
    if (!entry.snapshots.count(snapshot)) {
        cerr << "-------- snapshot out of sync -----------" << endl;
        snapshot_info.dump();
        //snapshot->dump();
        if (current_trans)
            current_trans->dump();
        cerr << "-------- end snapshot out of sync -----------" << endl;
        
        throw Exception("snapshots out of sync");
    }
    
    
    entry.snapshots.erase(snapshot);
    
    // NOTE: this must be last in the function; it causes the guard to be
    // released
    if (entry.snapshots.empty() /* && !most_recent*/)
        perform_cleanup(it, guard);
}

void
Snapshot_Info::
perform_cleanup(Entries::iterator it, ACE_Guard<Mutex> & guard)
{
    // TODO: try to hold the lock for less time here.  We only really need
    // the lock to add things to the previous snapshot.
    
    if (!it->second.snapshots.empty())
        throw Exception("perform_cleanup with snapshots");
    //if (it == boost::prior(entries.end()) && it->first == get_current_epoch())
    //    throw Exception("cleaning up most recent entry");
    if (it == entries.end())
        throw Exception("cleaning up invalid entry");

    /* Find where the previous snapshot is; any that can't be deleted
       here (due to being needed by a later snapshot) will need to be
       moved to that list */
    Entry * prev_snapshot = 0;
    Epoch prev_epoch = 0;
    
    Entries::iterator itnext = boost::next(it);

    if (it != entries.begin()) {
        Entries::iterator jt = boost::prior(it);
        
        prev_snapshot = &jt->second;
        prev_epoch = jt->first;
    }
    else {
        // Earliest epoch has changed, as this is the earliest known
        // and it just disappeared.
        try {
            if (itnext == entries.end())
                set_earliest_epoch(get_current_epoch());
            else set_earliest_epoch(itnext->first);
        } catch (const std::exception & exc) {
            cerr << "exception setting earliest epoch" << endl;
            dump_unlocked();
            cerr << "itnext == entries.end() = "
                 << (itnext == entries.end()) << endl;
            if (itnext != entries.end())
                cerr << "itnext->first = " << itnext->first
                     << endl;
            throw;
        }
    }
    
    //cerr << "prev_epoch = " << prev_epoch << endl;
    //cerr << "prev_snapshot = " << prev_snapshot << endl;
    
    int num_to_cleanup = 0;
    
    Entry & entry = it->second;

    // List of things to clean up once we release the guard
    vector<pair<Versioned_Object *, Epoch> > to_clean_up;
    
    for (unsigned i = 0;  i < entry.cleanups.size();  ++i) {
        Versioned_Object * obj = entry.cleanups[i].first;
        Epoch valid_from = entry.cleanups[i].second;
        
        //cerr << "epoch = " << epoch << endl;
        
        if (prev_epoch >= valid_from && prev_snapshot) {
            // still needed by prev snapshot
            //prev_snapshot->cleanups.push_back(make_pair(obj, valid_from));
            prev_snapshot->add_cleanup(make_pair(obj, valid_from));
        }
        else {
            // not needed anymore
            entry.cleanups[num_to_cleanup++] = entry.cleanups[i];
        }
    }
    
    entry.cleanups.resize(num_to_cleanup);
    
    to_clean_up.swap(entry.cleanups);

    Epoch snapshot_epoch = it->first;

    entries.erase(it);

    // Release the guard so that we can lock the objects
    guard.release();
    
    // Now do the actual cleanups with no lock held, to avoid deadlock (we can't
    // take the object lock with the snapshot_info lock held).
    for (unsigned i = 0;  i < to_clean_up.size();  ++i) {
        Versioned_Object * obj = to_clean_up[i].first;
        Epoch epoch = to_clean_up[i].second;
        
        try {
            obj->cleanup(epoch, snapshot_epoch);
        }
        catch (const std::exception & exc) {
            ostringstream obj_stream;
            obj->dump(obj_stream);
            cerr << "got exception: " << exc.what() << endl;
            cerr << "object after cleanup: " << endl;
            cerr << obj_stream.str();
            throw;
       }
    }
}

void
Snapshot_Info::
register_cleanup(Versioned_Object * obj, Epoch epoch_to_cleanup)
{
    // This is always called with the commit lock held, so:
    // 1.  The cleanups cannot happen at the same time;
    // 2.  There is only one thing here at a given time
    
    // NOTE: this is called with the object's lock held
    Entries::iterator it;

    {
        ACE_Guard<Mutex> guard(lock);

        if (entries.empty())
            throw Exception("register_cleanup with no snapshots");

        it = boost::prior(entries.end());
        it->second.add_cleanup(make_pair(obj, epoch_to_cleanup));
    }
}

void
Snapshot_Info::
compress_epochs()
{
    // We have to block any commits that are happening so that we can't get
    // any new epochs
    ACE_Guard<ACE_Mutex> commit_guard(commit_lock);

    ACE_Guard<Mutex> guard(lock);
    
    /* There could be any number of snapshots that are currently happening
       concurrently with us doing this.  We have to make sure that we don't
       modify their behaviour as we are renaming epochs.

       To analyse this, look at the following situation

       v16      s16
       v23
                s47

       There is a snapshot and a value at epoch 16, a new value at epoch
       23 and a snapshot at epoch 47.  What we want to do is transform it
       to this:

       v1       s1
       v2       s2

       In order to do this, we preceed as follows:

       1.  We change the epoch of the value at epoch 16 to that for its final
           epoch: 1.  Note that a read of this value by snapshot 16 will still
           return the same value.

       v1       s16
       v23
                s47

       2.  Now we can rename the epoch for snapshot 16:

       v1       s1
       v23
                s47

       3.  The value at epoch 23.  Again, changing its epoch will not change
           the value that is read by snapshot 47:

       v1       s1
       v2
                s47

       4.  Finally, we rename snapshot 47 to snapshot 1

       v1       s1
       v2       s2

       By doing these operations in this order, it is possible to compress
       the epochs without having to block all of the threads.

       Later on, it may be possible to avoid taking the commit lock.
    */

    if (entries.empty())
        return;

    // TODO: must have strong exception guarantee here, but it needs to be
    // implemented

    // TOOD: possible race condition with earliest_epoch(): when should it
    // be modified?

    cerr << "compress epochs: " << entries.size() << " entries" << endl;

    dump_unlocked();

    int i = 1; // starting epoch number
    for (Entries::iterator it = entries.begin(), end = entries.end();
         it != end;  /* no inc it */ ++i) {
        int old_epoch = it->first;
        int new_epoch = i;

        cerr << "renaming " << old_epoch << " to " << new_epoch << endl;

        if (old_epoch == new_epoch) {
            ++it;
            continue;  // nothing to do
        }

        if (new_epoch > old_epoch) {
            cerr << "new_epoch = " << new_epoch << endl;
            cerr << "old_epoch = " << old_epoch << endl;
            throw Exception("logic error in compress_epochs()");
        }
        
        Entry & entry = it->second;

        /* The cleanup list points to all things that need to be in this
           epoch. */
        for (Cleanups::iterator
                 jt = entry.cleanups.begin(),

                 jend = entry.cleanups.end();
             jt != jend;  ++jt) {
            Versioned_Object * obj = jt->first;
            size_t valid_from = jt->second;

            cerr << "  object " << obj << " before renaming "
                 << old_epoch << " to " << new_epoch << ":" << endl;
            obj->dump(cerr, 4);

            // TODO: if this throws? (not allowed to)
            obj->rename_epoch(valid_from, new_epoch);

            cerr << "  object " << obj << " after renaming "
                 << old_epoch << " to " << new_epoch << ":" << endl;
            obj->dump(cerr, 4);

            // Put it back in the cleanup list with the new epoch
            jt->second = new_epoch;
        }

        // Make sure writes are visible before we continue
        __sync_synchronize();

        cerr << "renaming epochs" << endl;
        dump_unlocked();

        for (set<Snapshot *>::iterator
                 jt = entry.snapshots.begin(),
                 jend = entry.snapshots.end();
             jt != jend;  ++jt)
            (*jt)->rename_epoch(old_epoch, new_epoch);

        // Make sure writes are visible before we continue
        __sync_synchronize();

        if (entries.count(new_epoch))
            throw Exception("new epoch already there");
        Entry & new_entry = entries[new_epoch];
        new_entry.snapshots.swap(entry.snapshots);
        new_entry.cleanups.swap(entry.cleanups);

        Entries::iterator new_it = boost::next(it);
        entries.erase(it);
        it = new_it;
    }

    cerr << "------------ finished renaming" << endl;
    dump_unlocked();

    current_epoch_ = i;
    earliest_epoch_ = 1;
}

void
Snapshot_Info::
dump_unlocked(std::ostream & stream)
{

    stream << "global state: " << endl;
    stream << "  current_epoch: " << get_current_epoch() << endl;
    stream << "  earliest_epoch: " << get_earliest_epoch() << endl;
    stream << "  current_trans: " << current_trans << " epoch "
           << (current_trans ? current_trans->epoch() : 0)
           << endl;
    stream << "  snapshot epochs: " << entries.size() << endl;
    int i = 0;
    for (map<Epoch, Entry>::const_iterator
             it = entries.begin(), end = entries.end();
         it != end;  ++it, ++i) {
        const Entry & entry = it->second;
        stream << "  " << i << " at epoch " << it->first << endl;
        stream << "    " << entry.snapshots.size() << " snapshots"
             << endl;
        int j = 0;
        for (set<Snapshot *>::const_iterator
                 jt = entry.snapshots.begin(), jend = entry.snapshots.end();
             jt != jend;  ++jt, ++j)
            stream << "      " << j << " " << *jt << " epoch "
                   << (*jt)->epoch() << " status " << (*jt)->status
                   << endl;
        stream << "    " << entry.cleanups.size() << " cleanups" << endl;
        for (unsigned j = 0;  j < entry.cleanups.size();  ++j)
            stream << "      " << j << ": object " << entry.cleanups[j].first
                 << " with version " << entry.cleanups[j].second << endl;
    }
}

void
Snapshot_Info::
dump(std::ostream & stream)
{
    ACE_Guard<Mutex> guard (lock);
    dump_unlocked(stream);
}

void
Snapshot_Info::
validate_unlocked() const
{
    
}

Epoch
Snapshot_Info::
has_cleanup(Epoch snapshot_epoch, const Versioned_Object * object) const
{
    ACE_Guard<Mutex> guard (lock);
    Entries::const_iterator it = entries.find(snapshot_epoch);

    if (it == entries.end()) return 0;

    for (Cleanups::const_iterator
             jt = it->second.cleanups.begin(),
             jend = it->second.cleanups.end();
         jt != jend;  ++jt)
        if (jt->first == object)
            return jt->second;

    return 0;
}


/*****************************************************************************/
/* SNAPSHOT                                                                  */
/*****************************************************************************/

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

void
Snapshot::
rename_epoch(Epoch old_epoch, Epoch new_epoch)
{
    cerr << "rename_epoch at " << this << endl;
    cerr << "epoch_ = " << epoch_ << endl;
    cerr << "old_epoch = " << old_epoch << endl;
    cerr << "new_epoch = " << new_epoch << endl;
    if (epoch_ != old_epoch)
        throw Exception("wrong old epoch");
    epoch_ = new_epoch;
}

} // namespace JMVCC
