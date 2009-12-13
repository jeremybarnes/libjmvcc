/* sandbox.h                                                       -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   A sandbox for local changes.
*/

#ifndef __jmvcc__sandbox_h__
#define __jmvcc__sandbox_h__


#include "utils/lightweight_hash.h"
#include "utils/string_functions.h"
#include "versioned_object.h"
#include <boost/tuple/tuple.hpp>

namespace JMVCC {


/*****************************************************************************/
/* SANDBOX                                                                   */
/*****************************************************************************/

/// A sandbox provides a place where writes don't affect the underlying
/// objects.  These writes can then be committed atomically.

class Sandbox {
    struct Entry {
        Entry() : val(0), size(0)
        {
        }

        void * val;
        size_t size;

        std::string print() const
        {
            return ML::format("val: %p size: %zd", val, size);
        }
    };

    typedef ML::Lightweight_Hash<Versioned_Object *, Entry> Local_Values;
    Local_Values local_values;

public:
    ~Sandbox();

    void clear();

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
            = local_values.insert(std::make_pair(obj, Entry()));
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

    bool commit(size_t old_epoch);

    void dump(std::ostream & stream = std::cerr, int indent = 0) const;

    size_t num_local_values() const { return local_values.size(); }

    friend std::ostream & operator << (std::ostream&, const Sandbox::Entry&);
};

std::ostream &
operator << (std::ostream & stream, const Sandbox::Entry & entry);


} // namespace JMVCC

#endif /* __jmvcc__sandbox_h__ */
