/* versioned.h                                                     -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Turns a normal object into a versioned one.
*/

#ifndef __jmvcc__versioned_h__
#define __jmvcc__versioned_h__

#include "utils/circular_buffer.h"
#include "versioned_object.h"
#include <ace/Synch.h>

namespace JMVCC {


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
    
    Versioned()
    {
        //current = new_entry(get_current_epoch(), T());
        history.push_back(new_entry(get_current_epoch(), T()));
    }
    
    explicit Versioned(const T & val)
    {
        //current = new_entry(get_current_epoch(), val);
        history.push_back(new_entry(get_current_epoch(), val));
    }

    ~Versioned()
    {
        //cleanup_entry(current);
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
            local = current_trans->local_value<int>(this, value);

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
            //return *current.value;
            return *history.back().value;
        }
        
        const T * val = current_trans->local_value<T>(this);
        
        if (val) return *val;
     
        ACE_Guard<Mutex> guard(lock);
        return value_at_epoch(current_trans->epoch());
    }

    //size_t history_size() const { return history.size(); }
    size_t history_size() const { return history.size() - 1; }

private:
    // This structure provides a list of values.  Each one is tagged with the
    // earliest epoch in which it is valid.  The latest epoch in which it is
    // valid + 1 is that of the next entry in the list; that in current has no
    // latest epoch.

    struct Entry {
        Entry()
            : valid_from(0), value(0)
        {
        }

        Entry(Epoch valid_from, T * value)
            : valid_from(valid_from), value(value)
        {
        }

        Epoch valid_from;
        T * value;
    };

    typedef ML::Circular_Buffer<Entry> History;

    //Entry current;       ///< Current value
    History history;     ///< History of older values with epoch
    mutable Mutex lock;

    /// Return the value for the given epoch
    const T & value_at_epoch(Epoch epoch) const
    {
        //if (epoch >= current.valid_from)
        //    return *current.value;

        for (int i = history.size() - 1;  i >= 0;  --i)
            if (epoch >= history[i].valid_from)
                return *history[i].value;

        using namespace std;
        cerr << "--------------- expired epoch -------------" << endl;
        cerr << "obj = " << this << endl;
        cerr << "current_epoch = " << get_current_epoch() << endl;
        cerr << "earliest_epoch = " << get_earliest_epoch() << endl;
        cerr << "epoch = " << epoch << endl;
        dump_unlocked();
        snapshot_info.dump();
        if (current_trans) current_trans->dump();
        cerr << "--------------- end expired epoch" << endl;

        throw Exception("attempt to obtain value for expired epoch");
    }

    struct Entry_Holder {
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

    Entry_Holder new_entry(Epoch epoch, const T & initial)
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

        if (new_epoch <= get_current_epoch())
            throw Exception("epochs out of order");

        //if (current.valid_from > old_epoch)
        //    return false;  // something updated before us

        if (history.back().valid_from > old_epoch)
            return false;

        // We have to allocate the extra space in the history as nothing is
        // allowed to fail in the commit or rollback.  We won't read from this
        // entry as its epoch is higher than the current epoch.
        history.push_back(new_entry(new_epoch, *reinterpret_cast<T *>(data)));
    
        return true;
    }

    virtual void commit(Epoch new_epoch) throw ()
    {
        // Now that it's definitive, we perform the following:
        // 1.  We transfer the new value we added to the current value;
        // 2.  We cleanup the first value on the history list
        ACE_Guard<Mutex> guard(lock);
        //std::swap(history.back(), current);

        // Register the new history entry to be cleaned up
        //snapshot_info.register_cleanup(this, history.back().valid_from);
        snapshot_info.register_cleanup(this, history[-2].valid_from);
    }

    virtual void rollback(Epoch new_epoch, void * data) throw ()
    {
        // Reverse the setup
        ACE_Guard<Mutex> guard(lock);
        cleanup_entry(history.back());
        history.pop_back();
    }

    virtual void cleanup(Epoch unused_epoch, Epoch trigger_epoch)
    {
        ACE_Guard<Mutex> guard(lock);

        if (history.empty())
            throw Exception("cleaning up with no values");

        // TODO: optimize
        int i = 0;
        for (typename History::iterator
                 it = history.begin(),
                 end = history.end();
             it != end;  ++it, ++i) {
            
            //if (i != std::distance(history.begin(), it))
            //    throw Exception("distance is wrong");

            if (it->valid_from == unused_epoch) {
                
#if 0
                Epoch my_earliest_epoch = get_earliest_epoch();
                if (i == 0 && history[1].valid_from > my_earliest_epoch) {
                    using namespace std;
                    cerr << "*** DESTROYING EARLIEST EPOCH FOR OBJECT "
                         << this << endl;
                    //backtrace();
                    cerr << "  unused_epoch = " << unused_epoch << endl;
                    cerr << "  trigger_epoch = " << trigger_epoch << endl;
                    cerr << "  earliest_epoch = " << my_earliest_epoch << endl;
                    cerr << "  OBJECT SHOULD BE DESTROYED AT EPOCH "
                         << my_earliest_epoch << endl;
                    //cerr << "  current_trans = " << current_trans << endl;
                    //cerr << "  current_trans epoch " << current_trans_epoch() << endl;
                    snapshot_info.dump();
                    dump_unlocked();
                    //throw Exception("destroying earliest epoch");
                }
#endif

                //validate();
                cleanup_entry(*it);
                history.erase(it);
                //validate();

#if 0
                if (i == 0 && history.front().valid_from > my_earliest_epoch)
                    throw Exception("destroying earliest epoch");
#endif

                return;
            }
        }
    

        using namespace std;
        cerr << "----------- cleaning up didn't exist ---------" << endl;
        dump_unlocked();
        cerr << "unused_epoch = " << unused_epoch << endl;
        cerr << "----------- end cleaning up didn't exist ---------" << endl;
        
        throw Exception("attempt to clean up something that didn't exist");
    }
    
    virtual void rename_epoch(Epoch old_epoch, Epoch new_epoch)
    {
        ACE_Guard<Mutex> guard(lock);

        if (history.empty())
            throw Exception("renaming empty epoch");

        // TODO: optimize
        int i = 0;
        for (typename History::iterator
                 it = history.begin(),
                 end = history.end();
             it != end;  ++it, ++i) {
            
            if (it->valid_from == old_epoch) {
                if (i != 0 && boost::prior(it)->valid_from >= new_epoch)
                    throw Exception("new epoch not ordered with respect to old");
                if (i != history.size() - 1
                    && boost::next(it)->valid_from <= new_epoch)
                    throw Exception("new epoch not ordered with respect to old 2");
                
                it->valid_from = new_epoch;
                return;
            }
        }
        
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
        //stream << s << "  current: valid from " << current.valid_from
        //       << " addr " << current.value << " value "
        //       << *current.value << endl;
        for (unsigned i = 0;  i < history.size();  ++i) {
            stream << s << "  " << i << ": valid from " << history[i].valid_from;
            stream << " addr " << history[i].value;
            stream << " value " << *history[i].value;
            stream << endl;
        }
    }

    virtual std::string print_local_value(void * val) const
    {
        return ostream_format(*reinterpret_cast<T *>(val));
    }

    virtual void validate() const
    {
        ssize_t e = 0;  // epoch we are up to
        
        for (unsigned i = 0;  i < history.size();  ++i) {
            Epoch e2 = history[i].valid_from;
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
};

template<typename T> std::allocator<T> Versioned<T>::allocator;

} // namespace JMVCC


#endif /* __jmvcc__versioned_h__ */
