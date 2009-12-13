/* sandbox.h                                                       -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   A sandbox for local changes.
*/

#ifndef __jmvcc__sandbox_h__
#define __jmvcc__sandbox_h__

namespace JMVCC {

/// A sandbox provides a place where writes don't affect the underlying
/// objects.  These writes can then be committed atomically.
struct Sandbox {
    struct Entry {
        Entry() : val(0), size(0)
        {
        }

        void * val;
        size_t size;

        std::string print() const
        {
            return format("val: %p size: %zd", val, size);
        }
    };

    typedef Lightweight_Hash<Versioned_Object *, Entry> Local_Values;
    Local_Values local_values;

    ~Sandbox()
    {
        clear();
    }

    void clear()
    {
        for (Local_Values::iterator
                 it = local_values.begin(),
                 end = local_values.end();
             it != end;  ++it) {
            free(it->second.val);
        }
        local_values.clear();
    }

    template<typename T>
    T * local_value(Versioned_Object * obj)
    {
        Local_Values::const_iterator it = local_values.find(obj);
        if (it == local_values.end()) return 0;
        return reinterpret_cast<T *>(it->second.val);
    }

    template<typename T>
    T * local_value(Versioned_Object * obj, const T & initial_value)
    {
        bool inserted;
        Local_Values::iterator it;
        boost::tie(it, inserted)
            = local_values.insert(make_pair(obj, Entry()));
        if (inserted) {
            it->second.val = malloc(sizeof(T));
            new (it->second.val) T(initial_value);
            it->second.size = sizeof(T);
        }
        return reinterpret_cast<T *>(it->second.val);
    }

    template<typename T>
    const T * local_value(const Versioned_Object * obj)
    {
        return local_value<T>(const_cast<Versioned_Object *>(obj));
    }

    template<typename T>
    const T * local_value(const Versioned_Object * obj, const T & initial_value)
    {
        return local_value(const_cast<Versioned_Object *>(obj), initial_value);
    }

    bool commit(size_t old_epoch)
    {
        ACE_Guard<ACE_Mutex> guard(commit_lock);

        size_t new_epoch = get_current_epoch() + 1;

        bool result = true;

        Local_Values::iterator
            it = local_values.begin(),
            end = local_values.end();

        // Commit everything
        for (; result && it != end;  ++it)
            result = it->first->setup(old_epoch, new_epoch, it->second.val);

        if (result) {
            // First we update the epoch.  This ensures that any new snapshot
            // created will see the correct epoch value, and won't look at
            // old values which might not have a list.
            //
            // IT IS REALLY IMPORTANT THAT THIS BE DONE IN THE GIVEN ORDER.
            // If we were to update the epoch afterwards, then new transactions
            // could be created with the old epoch.  These transactions might
            // need the values being cleaned up, racing with the creation
            // process.
            set_current_epoch(new_epoch);

            // Make sure these writes are seen before we clean up
            __sync_synchronize();

            // Success: we are in a new epoch
            for (it = local_values.begin(); it != end;  ++it)
                it->first->commit(new_epoch);
        }
        else {
            // Rollback any that were set up if there was a problem
            for (end = boost::prior(it), it = local_values.begin();
                 it != end;  ++it)
                it->first->rollback(new_epoch, it->second.val);
        }

        // TODO: for failed transactions, we'd do better to keep the
        // structure to avoid reallocations
        // TODO: clear as we go to better use cache
        clear();
        
        return result;
    }

    void dump(std::ostream & stream = std::cerr, int indent = 0) const
    {
        string s(indent, ' ');
        stream << "sandbox: " << local_values.size() << " local values"
             << endl;
        int i = 0;
        for (Local_Values::const_iterator
                 it = local_values.begin(), end = local_values.end();
             it != end;  ++it, ++i) {
            stream << s << "  " << i << " at " << it->first << ": value "
                 << it->first->print_local_value(it->second.val)
                 << endl;
        }
    }
};

inline std::ostream &
operator << (std::ostream & stream,
             const Sandbox::Entry & entry)
{
    return stream << entry.print();
}


} // namespace JMVCC

#endif /* __jmvcc__sandbox_h__ */
