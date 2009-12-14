/* versioned.h                                                     -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Turns a normal object into a versioned one.
*/

#ifndef __jmvcc__versioned_h__
#define __jmvcc__versioned_h__

#include "history.h"
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
        : history(new T())
    {
    }

    Versioned(const T & val)
        : history(new T(val))
    {
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
                value = history.value_at_epoch(current_trans->epoch(), this);
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
            //history.validate();
            return history.most_recent_value(this);
        }
        
        const T * val = current_trans->local_value<T>(this);
        
        if (val) return *val;
     
        ACE_Guard<Mutex> guard(lock);
        return history.value_at_epoch(current_trans->epoch(), this);
    }

    size_t history_size() const { return history.size(); }

private:
    // Implement object interface

    T * current;         ///< Current value
    History<T> history;  ///< History of older values with epoch
    mutable Mutex lock;

public:
    virtual bool setup(Epoch old_epoch, Epoch new_epoch, void * data)
    {
        ACE_Guard<Mutex> guard(lock);
        bool result = history.set_current_value(old_epoch, new_epoch,
                                                *reinterpret_cast<T *>(data));
        return result;
    }

    virtual void commit(Epoch new_epoch) throw ()
    {
        // Now that it's definitive, we can clean up any old values
        ACE_Guard<Mutex> guard(lock);
        history.cleanup_old_value(this);
    }

    virtual void rollback(Epoch new_epoch, void * data) throw ()
    {
        ACE_Guard<Mutex> guard(lock);
        history.rollback(new_epoch);
    }

    virtual void cleanup(Epoch unused_epoch, Epoch trigger_epoch)
    {
        ACE_Guard<Mutex> guard(lock);
        history.cleanup(unused_epoch, this, trigger_epoch);
    }

    virtual void rename_epoch(Epoch old_epoch, Epoch new_epoch)
    {
        ACE_Guard<Mutex> guard(lock);
        history.rename(old_epoch, new_epoch);
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
        std::string s(indent, ' ');
        stream << s << "object at " << this << std::endl;
        history.dump(stream, indent + 2);
    }

    virtual std::string print_local_value(void * val) const
    {
        return ostream_format(*reinterpret_cast<T *>(val));
    }
};

} // namespace JMVCC


#endif /* __jmvcc__versioned_h__ */
