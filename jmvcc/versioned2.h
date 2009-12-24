/* versioned2.h                                                    -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Turns a normal object into a versioned one.  This is a lock-free version.
*/

#ifndef __jmvcc__versioned2_h__
#define __jmvcc__versioned2_h__

#include <iostream> // debug
#include "versioned.h"
#include "utils/circular_buffer.h"
#include "arch/cmp_xchg.h"
#include "arch/atomic_ops.h"
#include "garbage.h"


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
struct Versioned2 : public Versioned_Object {

    explicit Versioned2(const T & val = T())
    {
        //static Info info;
        data = new_data(val, 1);
    }

    ~Versioned2()
    {
        delete_data(const_cast<Data *>(get_data()));
    }

    // Client interface.  Just two methods to get at the current value.
    T & mutate()
    {
        if (!current_trans) no_transaction_exception(this);
        T * local = current_trans->local_value<T>(this);

        if (!local) {
            T value;
            {
                value = get_data()->value_at_epoch(current_trans->epoch());
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
        const Data * d = get_data();

        if (!current_trans) {
            throw Exception("reading outside a transaction");
            //T result = d->value_at_epoch(get_current_epoch());
            //return result;
        }
        const T * val = current_trans->local_value<T>(this);
        
        if (val) return *val;
        
        T result = d->value_at_epoch(current_trans->epoch());
        return result;
    }

    size_t history_size() const
    {
        size_t result = get_data()->size() - 1;
        return result;
    }

private:
    // This structure provides a list of values.  Each one is tagged with the
    // earliest epoch in which it is valid.  The latest epoch in which it is
    // valid + 1 is that of the next entry in the list; that in current has no
    // latest epoch.

    struct Entry {
        explicit Entry(Epoch valid_to = 1, const T & value = T(),
                       Epoch valid_from = 1)
            : valid_from(valid_from), valid_to(valid_to), value(value)
        {
        }

        Epoch valid_from;
        Epoch valid_to;
        T value;
    };

    // Internal data object allocated
    struct Data {
        Data(size_t capacity)
            : capacity(capacity), first(0), last(0), magic(18273)
        {
            created_at = get_current_epoch();
            created_for = (current_trans ? current_trans->epoch() : 0);
            zombied_at = 0;
            zombied_for = 0;
            destroyed_at = 0;
            destroyed_for = 0;
        }

        Data(size_t capacity, const Data & old_data)
            : capacity(capacity), first(0), last(0), magic(18273)
        {
            created_at = get_current_epoch();
            created_for = (current_trans ? current_trans->epoch() : 0);
            zombied_at = 0;
            zombied_for = 0;
            destroyed_at = 0;
            destroyed_for = 0;

            for (unsigned i = 0;  i < old_data.size();  ++i)
                push_back(old_data.element(i));
        }

        uint32_t capacity;   // Number allocated
        uint32_t first;      // Index of first valid entry
        uint32_t last;       // Index of last valid entry
        uint32_t magic;
        Epoch created_at;
        Epoch created_for;
        mutable Epoch zombied_at;
        mutable Epoch zombied_for;
        Epoch destroyed_at;
        Epoch destroyed_for;
        Entry history[1];  // real ones are allocated after

        uint32_t size() const { cm();  return last - first; }

        ~Data()
        {
            cm();
            size_t sz = size();
            magic = 0;
            for (unsigned i = 0;  i < sz;  ++i)
                history[i].value.~T();
        }

        /// Return the value for the given epoch
        const T & value_at_epoch(Epoch epoch) const
        {
            cm();
            for (int i = last - 1;  i > first;  --i) {
                Epoch valid_from = history[i - 1].valid_to;
                if (epoch >= valid_from)
                    return history[i].value;
            }
            
            return history[first].value;
        }
        
        Data * copy(size_t new_capacity) const
        {
            cm();
            if (new_capacity < size())
                throw Exception("new capacity is wrong");

            return new_data(*this, new_capacity);
        }

        Entry & front()
        {
            cm();
            return history[first];
        }

        const Entry & front() const
        {
            cm();
            return history[first];
        }

        void pop_front()
        {
            cm();
            /* Need to:
               1.  Increment first
               2.  Set up the destructor for that element to be run for
                   garbage collection
            */
            if (size() < 2)
                throw Exception("attempt to pop last valid value off");
            first += 1;
        }

        void pop_back()
        {
            cm();
            if (size() < 2)
                throw Exception("popping back last element");
            --last;
            // Need to: make sure that garbage collection runs its destructor
        }

        void push_back(const Entry & entry)
        {
            cm();
            if (last == capacity) {
                using namespace std;
                cerr << "last = " << last << endl;
                cerr << "capacity = " << capacity << endl;
                throw Exception("can't push back");
            }
            new (&history[last].value) T(entry.value);
            history[last].valid_to = entry.valid_to;
            
            memory_barrier();

            ++last;
        }
        
        const Entry & back() const
        {
            cm();
            return history[last - 1];
        }

        Entry & back()
        {
            cm();
            return history[last - 1];
        }

        Entry & element(int index)
        {
            cm();
            if (index < 0 || index >= size())
                throw Exception("invalid element");
            return history[first + index];
        }

        const Entry & element(int index) const
        {
            cm();
            if (index < 0 || index >= size())
                throw Exception("invalid element");
            return history[first + index];
        }

        size_t checksum() const
        {
            cm();
            const unsigned * vals = reinterpret_cast<const unsigned *>(this);
            const unsigned * vals2 
                = reinterpret_cast<const unsigned *>(&history[capacity]);

            size_t total = 0;
            for (; vals != vals2;  ++vals)
                total = total * 5 + (*vals);

            return total;
        }

        void cm() const
        {
            if (magic != 18273) {
                using namespace std;
                cerr << "current_epoch = " << get_current_epoch() << endl;
                cerr << "earliest_epoch = " << get_earliest_epoch() << endl;
                cerr << "created_at     = " << created_at << endl;
                cerr << "created_for    = " << created_for << endl;
                cerr << "zombied_at     = " << zombied_at << endl;
                cerr << "destroyed_at   = " << destroyed_at << endl;
                cerr << "destroyed_for  = " << destroyed_for << endl;
                cerr << "magic = " << magic << endl;
                throw Exception("wrong magic");
            }
        }
    };

    // The single internal data member.  Updated atomically.
    mutable Data * data;

    const Data * get_data() const
    {
        return reinterpret_cast<const Data *>(data);
    }

    struct Delete_Data {
        Delete_Data(Data * data)
            : data(data)
        {
        }

        void operator () ()
        {
            data->~Data();
            data->destroyed_at = get_current_epoch();
            data->destroyed_for = (current_trans ? current_trans->epoch() : 0);
            //free(data);  // DEBUG
        }

        Data * data;
    };

    static void delete_data(Data * data)
    {
        schedule_cleanup(Delete_Data(data));
    }

    static void delete_data_now(Data * data)
    {
        Delete_Data do_it(data);
    }

    static Data * new_data(size_t capacity)
    {
        // TODO: exception safety...
        void * d = malloc(sizeof(Data) + capacity * sizeof(Entry));
        Data * d2 = new (d) Data(capacity);
        return d2;
    }

    static Data * new_data(const T & val, size_t capacity)
    {
        // TODO: exception safety...
        void * d = malloc(sizeof(Data) + capacity * sizeof(Entry));
        Data * d2 = new (d) Data(capacity);
        d2->push_back(Entry(1,  val, get_current_epoch()));
        return d2;
    }

    static Data * new_data(const Data & old, size_t capacity)
    {
        // TODO: exception safety...
        void * d = malloc(sizeof(Data) + capacity * sizeof(Entry));
        Data * d2 = new (d) Data(capacity, old);
        old.cm();
        return d2;
    }

    bool set_data(const Data * & old_data, Data * new_data)
    {
        // For the moment, the commit lock is held when we update this, so
        // there is no possibility of conflict.  But if ever we decide to
        // allow for parallel commits, then we need to be more careful here
        // to do it atomically.
        memory_barrier();

        bool result = cmp_xchg(reinterpret_cast<Data * &>(data),
                               const_cast<Data * &>(old_data),
                               new_data);

        if (!result) delete_data_now(new_data);
        else {
            old_data->zombied_at = get_current_epoch();
            old_data->zombied_for = (current_trans ? current_trans->epoch() : 0);
            delete_data(const_cast<Data *>(old_data));
        }

        return result;
    }
        
public:
    // Implement object interface

    virtual bool setup(Epoch old_epoch, Epoch new_epoch, void * new_value)
    {
        for (;;) {
            const Data * d = get_data();

            if (new_epoch != get_current_epoch() + 1)
                throw Exception("epochs out of order");
            
            Epoch valid_from = 1;
            if (d->size() > 1)
                valid_from = d->element(d->size() - 2).valid_to;
            
            if (valid_from > old_epoch)
                return false;  // something updated before us
            
            Data * new_data = d->copy(d->size() + 1);
            new_data->back().valid_to = new_epoch;
            new_data->push_back(Entry(1 /* valid_to */,
                                      *reinterpret_cast<T *>(new_value),
                                      new_epoch));
            
            if (set_data(d, new_data)) return true;
        }
    }

    virtual void commit(Epoch new_epoch) throw ()
    {
        const Data * d = get_data();

        // Now that it's definitive, we have an older entry to clean up
        Epoch valid_from = 1;
        if (d->size() > 2)
            valid_from = d->element(d->size() - 3).valid_to;

        snapshot_info.register_cleanup(this, valid_from);
    }

    virtual void rollback(Epoch new_epoch, void * local_data) throw ()
    {
#if 1
        const Data * d = get_data();

        for (;;) {
            Data * d2 = d->copy(d->size());
            d2->pop_back();
            if (set_data(d, d2)) return;
        }
#else
        Data * d;
        do {
            d = get_data();
            d->pop_back();
            d->back().valid_to = 1;  // probably unnecessary...
            memory_barrier();
        } while (d != data);
#endif
    }

    virtual void cleanup(Epoch unused_epoch, Epoch trigger_epoch)
    {
        const Data * d = get_data();

        for (;;) {

            if (d->size() < 2) {
                using namespace std;
                cerr << "cleaning up: unused_epoch = " << unused_epoch
                     << " trigger_epoch = " << trigger_epoch << endl;
                cerr << "current_epoch = " << get_current_epoch() << endl;
                throw Exception("cleaning up with no values to clean up");
            }
            
            using namespace std;
            //cerr << "cleaning up: unused_epoch = " << unused_epoch
            //     << " trigger_epoch = " << trigger_epoch << endl;
            
            //dump_unlocked();
            
#if 0
            if (unused_epoch < d->front().valid_to) {
                // Can be done atomically
                d->pop_front();
                return;
            }
#endif
            
            //cerr << "not in first one" << endl;
            
            Data * data2 = new_data(d->size());
            
            // Copy them, skipping the one that matched
            
            // TODO: optimize
            Epoch valid_from = 1;
            bool found = false;
            for (unsigned i = d->first, e = d->last, j = 0; i != e;  ++i) {
                //cerr << "i = " << i << " e = " << e << " j = " << j
                //     << " element = " << d->history[i].value << " valid to "
                //     << d->history[i].valid_to << " found = "
                //     << found << " valid_from = " << valid_from << endl;
                //cerr << "data2->size() = " << data2->size() << endl;
                
                if (valid_from == unused_epoch
                    || (i == d->first
                        && unused_epoch < d->front().valid_to)) {
                    //cerr << "  removing" << endl;
                    if (found)
                        throw Exception("two with the same valid_from value");
                    found = true;
                    if (j != 0)
                        data2->history[j - 1].valid_to = d->history[i].valid_to;
                }
                else {
                    // Copy element i to element j
                    new (&data2->history[j].value) T(d->history[i].value);
                    data2->history[j].valid_to = d->history[i].valid_to;
                    ++j;
                    ++data2->last;
                }
                
                valid_from = d->history[i].valid_to;
            }
            
            if (found) {
                if (d->size() != data2->size() + 1) {
                    cerr << "d->size() = " << d->size() << endl;
                    cerr << "data2->size() = " << data2->size() << endl;
                    dump_unlocked();
                    throw Exception("sizes were wrong");
                }
                
                if (set_data(d, data2)) return;
                continue;
            }
            
            static Lock lock;
            Guard guard2(lock);
            cerr << "----------- cleaning up didn't exist ---------" << endl;
            dump_unlocked();
            cerr << "unused_epoch = " << unused_epoch << endl;
            cerr << "trigger_epoch = " << trigger_epoch << endl;
            snapshot_info.dump();
            cerr << "----------- end cleaning up didn't exist ---------" << endl;
            
            throw Exception("attempt to clean up something that didn't exist");
        }
    }
    
    virtual void rename_epoch(Epoch old_epoch, Epoch new_epoch) throw ()
    {
        throw Exception("versioned2: no renaming");
#if 0
        if (history.empty())
            throw Exception("renaming up with no values");
        
        if (old_epoch < history[0].valid_to) {
            // The last one doesn't have a valid_from, so we assume that it's
            // ok and leave it.
            return;
        }

        // TODO: optimize
        int i = 0;
        for (typename History::iterator
                 it = history.begin(),
                 end = history.end();
             it != end;  ++it, ++i) {
            
            if (it->valid_to == old_epoch) {
                if (i != 0 && boost::prior(it)->valid_to >= new_epoch)
                    throw Exception("new epoch not ordered with respect to "
                                    "old");
                if (i != history.size() - 1
                    && boost::next(it)->valid_to <= new_epoch)
                    throw Exception("new epoch not ordered with respect to "
                                    "old 2");
                
                it->valid_to = new_epoch;
                return;
            }
        }

        using namespace std;
        cerr << "---------------------" << endl;
        cerr << "old_epoch = " << old_epoch << endl;
        cerr << "new_epoch = " << new_epoch << endl;
        dump_unlocked();
        throw Exception("attempt to rename something that didn't exist");
#endif
    }

    virtual void dump(std::ostream & stream = std::cerr, int indent = 0) const
    {
        dump_itl(stream, indent);
    }

    virtual void dump_unlocked(std::ostream & stream = std::cerr,
                               int indent = 0) const
    {
        dump_itl(stream, indent);
    }

    void dump_itl(std::ostream & stream, int indent = 0) const
    {
        const Data * d = get_data();

        using namespace std;
        std::string s(indent, ' ');
        stream << s << "object at " << this << std::endl;
        stream << s << "history with " << d->size()
               << " values" << endl;
        for (unsigned i = 0;  i < d->size();  ++i) {
            const Entry & entry = d->element(i);
            stream << s << "  " << i << ": valid from "
                   << entry.valid_from << " valid_to " << entry.valid_to;
            stream << " addr " << &entry.value;
            stream << " value " << entry.value;
            stream << endl;
        }
    }

    virtual std::string print_local_value(void * val) const
    {
        return ostream_format(*reinterpret_cast<T *>(val));
    }

    virtual void validate() const
    {
#if 0
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
#endif
    }
};

} // namespace JMVCC


#endif /* __jmvcc__versioned2_h__ */
