/* history.h                                                       -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Data structure maintaining a list of versioned values.
*/

#ifndef __jmvcc__history_h__
#define __jmvcc__history_h__

#include "utils/circular_buffer.h"
#include "versioned_object.h"

namespace JMVCC {

using namespace ML;

/*****************************************************************************/
/* HISTORY                                                                   */
/*****************************************************************************/

template<typename T>
struct History {
    History();

    History(T * initial);

    History(const History & other);

    ~History();

    size_t size() const;

    const T & most_recent_value(const Versioned_Object * obj) const;

    /// Return the value for the given epoch
    const T & value_at_epoch(size_t epoch, const Versioned_Object * obj) const;

    /// Update the current value at a new epoch.  Returns true if it
    /// succeeded.  If the value has changed since the old epoch, it will
    /// not succeed.
    bool set_current_value(size_t old_epoch, size_t new_epoch,
                           const T & new_value);

    void cleanup_old_value(Versioned_Object * obj);

    /// Erase the entry that was speculatively added
    void rollback(size_t old_epoch);

    /// Clean up the entry for an unneeded epoch
    void cleanup(size_t unneeded_epoch, const Versioned_Object * obj, size_t trigger_epoch);
    void dump(std::ostream & stream = std::cerr, int indent = 0) const;

private:
    struct Entry {
        Entry()
            : epoch(0), value(0)
        {
        }

        Entry(size_t epoch, T * value)
            : epoch(epoch), value(value)
        {
        }

        size_t epoch;
        T * value;
    };

    typedef ML::Circular_Buffer<Entry> Entries;
    Entries entries;

    void validate() const;

    template<typename TT> friend class Versioned;

    static std::allocator<T> allocator;

    // Little object to return the result of new_entry for exception
    // safety
    struct Entry_Holder;

    Entry_Holder new_entry(size_t epoch, const T & initial);

    void cleanup_entry(const Entry & entry);
};

template<typename T> std::allocator<T> History<T>::allocator;


} // namespace JMVCC

#include "history_impl.h"

#endif /* __jmvcc__history_h__ */
