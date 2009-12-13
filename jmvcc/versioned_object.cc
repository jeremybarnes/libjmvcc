/* versioned_object.cc
   Jeremy Barnes, 13 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Implementation of default Versioned_Object methods.
*/

#include "versioned_object.h"
#include "utils/string_functions.h"

using namespace std;

/*****************************************************************************/
/* VERSIONED_OBJECT                                                          */
/*****************************************************************************/

namespace JMVCC {

void
Versioned_Object::
dump(std::ostream & stream, int indent) const
{
}

void
Versioned_Object::
dump_unlocked(std::ostream & stream,
              int indent) const
{
}

std::string
Versioned_Object::
print_local_value(void * val) const
{
    return ML::format("%08p", val);
}

} // namespace JMVCC
