/* history_impl.h                                                  -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Implementation of methods for history.
*/

#ifndef __jmvcc__history_impl_h__
#define __jmvcc__history_impl_h__

#include "snapshot.h"
#include "transaction.h"

namespace JMVCC {

/*****************************************************************************/
/* HISTORY                                                                   */
/*****************************************************************************/

template<typename T>
History<T>::
History()
    : entries(0)
{
}

template<typename T>
History<T>::
History(T * initial)
    : entries(1)
{
    entries.push_back(Entry(get_current_epoch(), initial));
}

template<typename T>
History<T>::
History(const History & other)
{
    entries = other.entries;
}

template<typename T>
History<T>::
~History()
{
    for (typename Entries::iterator
             it = entries.begin(),
             end = entries.end();
         it != end;  ++it)
        cleanup_entry(*it);
}

template<typename T>
size_t
History<T>::
size() const
{
    return entries.size();
}

template<typename T>
const T &
History<T>::
most_recent_value(const Versioned_Object * obj) const
{
    return value_at_epoch(get_current_epoch(), obj);
    
    // TODO: optimization
    // If we're in a commit, we ignore the speculative new value until it's
    // committed and the epoch is incremented...
    if (JML_UNLIKELY(entries.back().epoch > get_current_epoch())) {
        if (entries.size() < 2)
            throw Exception("tried to read non-existent epoch");
        const Entry & entry = entries[-2];
        if (entry.epoch > get_current_epoch())
            throw Exception("wrong epoch");
        return *entry.value;
    }
    return *entries.back().value;
}

template<typename T>
bool
History<T>::
set_current_value(Epoch old_epoch, Epoch new_epoch,
                  const T & new_value)
{
    if (entries.empty())
        throw Exception("set_current_value with no entries");
    if (entries.back().epoch > old_epoch)
        return false;  // something updated before us
    
    entries.push_back(new_entry(new_epoch, new_value));
    
    return true;
}

template<typename T>
void
History<T>::
cleanup_old_value(Versioned_Object * obj)
{
    if (entries.size() < 2) return;  // nothing to clean up

    // Do the common case where the first entry is no longer needed
    // NOTE: dicey; needs to be properly analysed
    //while (entries.size() >= 2
    //       && entries[1]->epoch < get_earliest_epoch())
    //    entries.pop_front();
    //if (entries.size() < 2) return;

    // The second last entry needs to be cleaned up by the last snapshot
    Epoch epoch = entries[-2].epoch;

    snapshot_info.register_cleanup(obj, epoch);
}

/// Erase the entry that was speculatively added
template<typename T>
void
History<T>::
rollback(Epoch old_epoch)
{
    if (entries.empty())
        throw Exception("entries was empty");

    if (entries.back().epoch != old_epoch)
        throw Exception("erasing the wrong entry");

    cleanup_entry(entries.back());
    entries.pop_back();
}

/// Clean up the entry for an unneeded epoch
template<typename T>
void
History<T>::
cleanup(Epoch unneeded_epoch, const Versioned_Object * obj, Epoch trigger_epoch)
{
    if (entries.size() <= 1)
        throw Exception("cleaning up with < 2 values");

    // TODO: optimize
    int i = 0;
    for (typename Entries::iterator
             it = entries.begin(),
             end = entries.end();
         it != end;  ++it, ++i) {
            
        //if (i != std::distance(entries.begin(), it))
        //    throw Exception("distance is wrong");

        if (it->epoch == unneeded_epoch) {
                
            Epoch my_earliest_epoch = get_earliest_epoch();
            if (i == 0 && entries[1].epoch > my_earliest_epoch) {
                using namespace std;
                cerr << "*** DESTROYING EARLIEST EPOCH FOR OBJECT "
                     << obj << endl;
                backtrace();
                cerr << "  unneeded_epoch = " << unneeded_epoch << endl;
                cerr << "  trigger_epoch = " << trigger_epoch << endl;
                cerr << "  earliest_epoch = " << my_earliest_epoch << endl;
                cerr << "  OBJECT SHOULD BE DESTROYED AT EPOCH "
                     << my_earliest_epoch << endl;
                //cerr << "  current_trans = " << current_trans << endl;
                //cerr << "  current_trans epoch " << current_trans_epoch() << endl;
                snapshot_info.dump();
                obj->dump_unlocked();
                //throw Exception("destroying earliest epoch");
            }

            //validate();
            cleanup_entry(*it);
            entries.erase(it);
            //validate();

            if (i == 0 && entries.front().epoch > my_earliest_epoch)
                throw Exception("destroying earliest epoch");

            return;
        }
    }

    using namespace std;
    cerr << "----------- cleaning up didn't exist ---------" << endl;
    obj->dump_unlocked();
    cerr << "unneeded_epoch = " << unneeded_epoch << endl;
    cerr << "----------- end cleaning up didn't exist ---------" << endl;

    throw Exception("attempt to clean up something that didn't exist");
}

template<typename T>
void
History<T>::
dump(std::ostream & stream, int indent) const
{
    using namespace std;
    string s(indent, ' ');
    stream << s << "history with " << size()
           << " values" << endl;
    for (unsigned i = 0;  i < size();  ++i) {
        stream << s << "  " << i << ": epoch " << entries[i].epoch;
        stream << " addr " << entries[i].value;
        stream << " value " << *entries[i].value;
        stream << endl;
    }
}

template<typename T>
void
History<T>::
validate() const
{
    ssize_t e = 0;  // epoch we are up to

    for (unsigned i = 0;  i < entries.size();  ++i) {
        Epoch e2 = entries[i]->epoch;
        if (e2 > get_current_epoch() + 1) {
            using namespace std;
            cerr << "e = " << e << " e2 = " << e2 << endl;
            dump();
            cerr << "invalid current epoch" << endl;
            throw Exception("invalid current epoch");
        }
        if (e2 <= e) {
            using namespace std;
            cerr << "e = " << e << " e2 = " << e2 << endl;
            dump();
            cerr << "invalid epoch order" << endl;
            throw Exception("invalid epoch order");
        }
        e = e2;
    }
}

template<typename T>
struct History<T>::Entry_Holder {
    Entry_Holder(Epoch epoch, T * value)
        : entry(epoch, value), used(false)
    {
    }

    ~Entry_Holder()
    {
        if (!used) {
            entry.value->~T();
            allocator.deallocate(entry.value, 1);
        }
    }

    // Calling this will transfer ownership
    operator const Entry & () const
    {
        used = true;
        return entry;
    }

    Entry entry;
    mutable bool used;
};

template<typename T>
typename History<T>::Entry_Holder
History<T>::
new_entry(Epoch epoch, const T & initial)
{
    T * value = allocator.allocate(1);
    try {
        new (value) T(initial);
    }
    catch (...) {
        allocator.deallocate(value, 1);
        throw;
    }

    return Entry_Holder(epoch, value);
}

template<typename T>
void
History<T>::
cleanup_entry(const Entry & entry)
{
    if (JML_UNLIKELY(!entry.value)) return;
    entry.value->~T();
    allocator.deallocate(entry.value, 1);
}

template<typename T>
const T &
History<T>::
value_at_epoch(Epoch epoch, const Versioned_Object * obj) const
{
    if (entries.empty())
        throw Exception("attempt to obtain value for object that never "
                        "existed");
    
#if 0
    for (typename Entries::const_iterator
             it = entries.end(),
             beg = entries.begin();
         it != beg; /* no inc */) {
        --it;
        if (it->epoch <= epoch) return *it->value;
    }
#else
    for (int i = entries.size() - 1;  i >= 0;  --i)
        if (entries[i].epoch <= epoch) return *entries[i].value;
#endif    

    {
        using namespace std;
        cerr << "--------------- expired epoch -------------" << endl;
        cerr << "obj = " << obj << endl;
        cerr << "current_epoch = " << get_current_epoch() << endl;
        cerr << "earliest_epoch = " << get_earliest_epoch() << endl;
        cerr << "epoch = " << epoch << endl;
        dump();
        snapshot_info.dump();
        if (current_trans) current_trans->dump();
        obj->dump_unlocked();
        cerr << "--------------- end expired epoch" << endl;
    }    
    sleep(1);
    

    abort();
    
    
    throw Exception("attempt to obtain value for expired epoch");
}


} // namespace JMVCC

#endif /* __jmvcc__history_impl_h__ */
