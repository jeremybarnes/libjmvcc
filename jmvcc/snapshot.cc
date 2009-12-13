/* snapshot.cc
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.
*/

#include "snapshot.h"
#include "transaction.h"

using namespace std;


namespace JMVCC {

volatile size_t current_epoch_ = 1;
size_t earliest_epoch_ = 1;

Snapshot_Info snapshot_info;


/*****************************************************************************/
/* SNAPSHOT_INFO                                                             */
/*****************************************************************************/

size_t
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
        snapshot_info.dump();
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
    size_t prev_epoch = 0;
    
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
    vector<pair<Versioned_Object *, size_t> > to_clean_up;
    
    for (unsigned i = 0;  i < entry.cleanups.size();  ++i) {
        Versioned_Object * obj = entry.cleanups[i].first;
        size_t epoch = entry.cleanups[i].second;
        
        //cerr << "epoch = " << epoch << endl;
        
        if (prev_epoch >= epoch && prev_snapshot) {
            // still needed by prev snapshot
            prev_snapshot->cleanups.push_back(make_pair(obj, epoch));
        }
        else entry.cleanups[num_to_cleanup++] = entry.cleanups[i]; // not needed anymore
    }
    
    //debug << "num_to_cleanup = " << num_to_cleanup << endl;
    
    entry.cleanups.resize(num_to_cleanup);
    
    to_clean_up.swap(entry.cleanups);

    size_t snapshot_epoch = it->first;

    entries.erase(it);

    // Release the guard so that we can lock the objects
    guard.release();
    
    // Now do the actual cleanups with no lock held, to avoid deadlock (we can't
    // take the object lock with the snapshot_info lock held).
    for (unsigned i = 0;  i < to_clean_up.size();  ++i) {
        Versioned_Object * obj = to_clean_up[i].first;
        size_t epoch = to_clean_up[i].second;
        
        //debug << "cleaning up object " << obj << " with unneeded epoch "
        //      << epoch << endl;
        
        //ostringstream obj_stream_before;
        //obj->dump(obj_stream_before);
        
        try {
            obj->cleanup(epoch, snapshot_epoch);
        }
        catch (const std::exception & exc) {
            ostringstream obj_stream;
            obj->dump(obj_stream);
            cerr << "got exception: " << exc.what() << endl;
            //cerr << debug.str();
            //cerr << "object before cleanup: " << endl;
            //cerr << obj_stream_before.str();
            cerr << "object after cleanup: " << endl;
            cerr << obj_stream.str();
            //abort();  // let execution continus; see if this causes an error
        }
    }
}

void
Snapshot_Info::
register_cleanup(Versioned_Object * obj, size_t epoch_to_cleanup)
{
    // NOTE: this is called with the object's lock held
    ACE_Guard<Mutex> guard(lock);

    if (entries.empty())
        throw Exception("register_cleanup with no snapshots");

    Entries::iterator it = boost::prior(entries.end());
    it->second.cleanups.push_back(make_pair(obj, epoch_to_cleanup));
}

void
Snapshot_Info::
dump_unlocked(std::ostream & stream)
{

    stream << "global state: " << endl;
    stream << "  current_epoch: " << get_current_epoch() << endl;
    stream << "  earliest_epoch: " << get_earliest_epoch() << endl;
    stream << "  current_trans: " << current_trans << endl;
    stream << "  snapshot epochs: " << entries.size() << endl;
    int i = 0;
    for (map<size_t, Entry>::const_iterator
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

} // namespace JMVCC
