/* versioned.h                                                     -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Turns a normal object into a versioned one.
*/

#ifndef __jmvcc__versioned_h__
#define __jmvcc__versioned_h__

#include "jml/utils/circular_buffer.h"
#include "versioned_object.h"
#include <ace/Synch.h>


namespace JMVCC {


using namespace std;


/*****************************************************************************/
/* VERSIONED                                                                 */
/*****************************************************************************/

/** This template takes an underlying type and turns it into a versioned
    object.  It's used for simple objects where a new copy of the object
    can be stored for each version.

    For more complicated cases (for example, where a lot of the state
    can be shared between an old and a new version), the object should
    derive directly from Versioned_Object instead.
*/

template<typename T>
struct Versioned : public Versioned_Object {
    typedef ACE_Mutex Mutex;
    
    explicit Versioned(const T & val = T())
    {
        Entry entry = new_entry(0, val);
        current = entry.value;
        //valid_from = 0;
    }

    ~Versioned()
    {
        Entry entry(0, current);
        cleanup_entry(entry);
        for (typename History::iterator
                 it = history.begin(),
                 end = history.end();
             it != end;  ++it)
            cleanup_entry(*it);
    }

    // Client interface.  Just two methods to get at the current value.
    T & mutate()
    {
        if (!current_trans) no_transaction_exception(this);
        T * local = current_trans->local_value<T>(this);

        if (!local) {
            T value;
            {
                ACE_Guard<Mutex> guard(lock);
                //history.validate();
                value = value_at_epoch(current_trans->epoch());
            }
            local = current_trans->local_value<T>(this, value);

            if (!local)
                throw Exception("mutate(): no local was created");
        }
        
        return *local;
    }

    void write(const T & val)
    {
        mutate() = val;
    }
    
    const T read() const
    {
        if (!current_trans) {
            ACE_Guard<Mutex> guard(lock);
            return value_at_epoch(get_current_epoch());
        }
        
        const T * val = current_trans->local_value<T>(this);
        
        if (val) return *val;
     
        ACE_Guard<Mutex> guard(lock);
        return value_at_epoch(current_trans->epoch());
    }

    size_t history_size() const { return history.size(); }

private:
    // This structure provides a list of values.  Each one is tagged with the
    // earliest epoch in which it is valid.  The latest epoch in which it is
    // valid + 1 is that of the next entry in the list; that in current has no
    // latest epoch.

    struct Entry {
        explicit Entry(Epoch valid_to = 1, T * value = 0)
            : valid_to(valid_to), value(value)
        {
        }

        Epoch valid_to;
        T * value;
    };

    typedef ML::Circular_Buffer<Entry> History;

    T * current;         ///< Current value
    //Epoch valid_from;    ///< Equal to the valid_to of history.back()
    History history;     ///< History of older values with epoch
    mutable Mutex lock;

    Epoch valid_from() const { return (history.empty() ? 1 : history.back().valid_to); }

    /// Return the value for the given epoch
    const T & value_at_epoch(Epoch epoch) const
    {
        if (epoch >= valid_from())
            return *current;

        for (int i = history.size() - 1;  i > 0;  --i) {
            Epoch valid_from = history[i - 1].valid_to;
            if (epoch >= valid_from)
                return *history[i].value;
        }
        
        return *history.front().value;
    }
    
    struct Entry_Holder {
        Entry_Holder(Epoch valid_to, T * value)
            : entry(valid_to, value), used(false)
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

    Entry_Holder new_entry(Epoch valid_to, const T & initial)
    {
        T * value = allocator.allocate(1);
        try {
            new (value) T(initial);
        }
        catch (...) {
            allocator.deallocate(value, 1);
            throw;
        }

        return Entry_Holder(valid_to, value);
    }

    void cleanup_entry(const Entry & entry)
    {
        if (JML_UNLIKELY(!entry.value)) return;
        entry.value->~T();
        allocator.deallocate(entry.value, 1);
    }

    static std::allocator<T> allocator;

public:
    // Implement object interface

    virtual bool setup(Epoch old_epoch, Epoch new_epoch, void * data)
    {
        ACE_Guard<Mutex> guard(lock);

        if (new_epoch != get_current_epoch() + 1)
            throw Exception("epochs out of order");

        if (valid_from() > old_epoch)
            return false;  // something updated before us

        // We have to allocate the extra space in the history as nothing is
        // allowed to fail in the commit or rollback.  We won't read from this
        // entry as its epoch is higher than the current epoch.
        history.push_back(Entry(new_epoch, current));
        //valid_from = new_epoch;
        Entry entry = new_entry(0, *reinterpret_cast<T *>(data));
        current = entry.value;

        return true;
    }

    virtual void commit(Epoch new_epoch) throw ()
    {
        // Now that it's definitive, we perform the following:
        // 1.  We cleanup the first value on the history list
        ACE_Guard<Mutex> guard(lock);

        // Register the new history entry to be cleaned up
        Epoch valid_from = (history.size() > 1 ? history[-2].valid_to : 1);
        snapshot_info.register_cleanup(this, valid_from);
    }

    Epoch fake_commit(Epoch new_epoch) throw ()
    {
        // Now that it's definitive, we perform the following:
        // 1.  We cleanup the first value on the history list
        ACE_Guard<Mutex> guard(lock);

        // Register the new history entry to be cleaned up
        Epoch valid_from = (history.size() > 1 ? history[-2].valid_to : 1);
        return valid_from;
    }

    virtual void rollback(Epoch new_epoch, void * data) throw ()
    {
        // Reverse the setup
        ACE_Guard<Mutex> guard(lock);
        Entry entry(0, current);
        cleanup_entry(entry);
        current = history.back().value;
        history.pop_back();
        //valid_from = (history.empty() ? 0 : history.back().valid_to);
    }

    virtual void cleanup(Epoch unused_valid_from, Epoch trigger_epoch)
    {
        ACE_Guard<Mutex> guard(lock);

        if (history.empty())
            throw Exception("cleaning up with no values");

        if (unused_valid_from < history[0].valid_to) {
            history.pop_front();
            return;
        }

        // TODO: optimize
        Epoch valid_from = 1;
        for (typename History::iterator
                 it = history.begin(),
                 last,
                 end = history.end();
             it != end;  valid_from = it->valid_to, last = it, ++it) {

            if (valid_from == unused_valid_from) {
                if (valid_from != 1)
                    last->valid_to = it->valid_to;
                cleanup_entry(*it);
                history.erase(it);
                return;
            }
        }
    

        using namespace std;
        cerr << "----------- cleaning up didn't exist ---------" << endl;
        dump_unlocked();
        cerr << "unused_valid_from = " << unused_valid_from << endl;
        cerr << "trigger_epoch = " << trigger_epoch << endl;
        snapshot_info.dump();
        cerr << "----------- end cleaning up didn't exist ---------" << endl;
        
        throw Exception("attempt to clean up something that didn't exist");
    }
    
    virtual Epoch rename_epoch(Epoch old_valid_from,
                               Epoch new_valid_from) throw ()
    {
        ACE_Guard<Mutex> guard(lock);

        if (history.empty())
            throw Exception("renaming with no values");
        
#if 0
        if (old_valid_from < history[0].valid_to) {
            // The last one doesn't have a valid_from, so we assume that it's
            // ok and leave it.
            return;
        }
#endif

        // This is subtle.  Since we have valid_to values stored and not
        // valid_from values, we need to find the particular one and change
        // it.

        // TODO: optimize
        int i = 0;
        for (typename History::iterator
                 it = history.begin(),
                 end = history.end();
             it != end;  ++it, ++i) {
            
            if (it->valid_to == old_valid_from) {
                if (i != 0 && boost::prior(it)->valid_to >= new_valid_from)
                    throw Exception("new valid_from not ordered with respect to "
                                    "old");
                if (i != history.size() - 1
                    && boost::next(it)->valid_to <= new_valid_from)
                    throw Exception("new valid_from not ordered with respect to "
                                    "old 2");
                
                it->valid_to = new_valid_from;

                ++it;
                if (it == end) return 0;
                else return it->valid_to;
            }
        }

        using namespace std;
        cerr << "---------------------" << endl;
        cerr << "old_epoch = " << old_valid_from << endl;
        cerr << "new_epoch = " << new_valid_from << endl;
        dump_unlocked();
        throw Exception("attempt to rename something that didn't exist");
    }

    virtual void dump(std::ostream & stream = std::cerr, int indent = 0) const
    {
        ACE_Guard<Mutex> guard(lock);
        dump_itl(stream, indent);
    }

    virtual void dump_unlocked(std::ostream & stream = std::cerr,
                               int indent = 0) const
    {
        dump_itl(stream, indent);
    }

    void dump_itl(std::ostream & stream, int indent = 0) const
    {
        using namespace std;
        std::string s(indent, ' ');
        stream << s << "object at " << this << std::endl;
        stream << s << "history with " << history.size()
               << " values" << endl;
        for (unsigned i = 0;  i < history.size();  ++i) {
            stream << s << "  " << i << ": valid to " << history[i].valid_to;
            stream << " addr " << history[i].value;
            stream << " value " << *history[i].value;
            stream << endl;
        }
        stream << s << "  current: valid from " << valid_from()
               << " addr " << current << " value "
               << *current << endl;
    }

    virtual std::string print_local_value(void * val) const
    {
        return ostream_format(*reinterpret_cast<T *>(val));
    }

    virtual void validate() const
    {
        ssize_t e = 0;  // epoch we are up to
        
        for (unsigned i = 0;  i < history.size();  ++i) {
            Epoch e2 = history[i].valid_to;
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

    template<typename T2> friend class Versioned2;
};

template<typename T> std::allocator<T> Versioned<T>::allocator;

} // namespace JMVCC


#endif /* __jmvcc__versioned_h__ */
