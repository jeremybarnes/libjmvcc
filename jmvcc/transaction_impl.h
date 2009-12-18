/* transaction_impl.h                                              -*- C++ -*-
   Jeremy Barnes, 13 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Implementation of transactions.
*/

#ifndef __jmvcc__transaction_impl_h__
#define __jmvcc__transaction_impl_h__


#include "garbage.h"


namespace JMVCC {


/*****************************************************************************/
/* LOCAL_TRANSACTION                                                         */
/*****************************************************************************/

inline
Local_Transaction::
Local_Transaction()
{
    enter_critical();
    old_trans = current_trans;
    current_trans = this;
}

inline
Local_Transaction::
~Local_Transaction()
{
    current_trans = old_trans;
    leave_critical();
}

} // namespace JMVCC

#endif /*  __jmvcc__transaction_impl_h__ */
