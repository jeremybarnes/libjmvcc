/* versioned_object.h                                              -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Abstract interface to a versioned object.
*/

#ifndef __jmvcc__versioned_object_h__
#define __jmvcc__versioned_object_h__

#include <iostream>
#include <string>
#include "utils/string_functions.h"

namespace JMVCC {


/*****************************************************************************/
/* VERSIONED_OBJECT                                                          */
/*****************************************************************************/

/// This is an actual object.  Contains metadata and value history of an
/// object.

struct Versioned_Object {

    Versioned_Object()
    {
    }

    // Lock the current value into memory, so that no other transaction is
    // allowed to modify it
    //virtual void lock_value() const = 0;

    // Get the commit ready and check that everything can go ahead, but
    // don't actually perform the commit
    virtual bool setup(size_t old_epoch, size_t new_epoch, void * data) = 0;

    // Confirm a setup commit, making it permanent
    virtual void commit(size_t new_epoch) throw () = 0;

    // Roll back a setup commit
    virtual void rollback(size_t new_epoch, void * data) throw () = 0;

    // Clean up information from an unused epoch
    virtual void cleanup(size_t unused_epoch, size_t trigger_epoch) = 0;
    
    virtual void dump(std::ostream & stream = std::cerr, int indent = 0) const
    {
    }

    virtual void dump_unlocked(std::ostream & stream = std::cerr, int indent = 0) const
    {
    }

    virtual std::string print_local_value(void * val) const
    {
        return ML::format("%08p", val);
    }
};



} // namespace JMVCC


#endif /* __jmvcc__versioned_object_h__ */
