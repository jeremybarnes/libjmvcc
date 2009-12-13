/* transaction.h                                                   -*- C++ -*-
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Transactions.
*/

#ifndef __jmvcc__transaction_h__
#define __jmvcc__transaction_h__

#include "snapshot.h"
#include "sandbox.h"

namespace JMVCC {

class Transaction;

/// Current transaction for this thread
extern __thread Transaction * current_trans;

size_t current_trans_epoch();

/// For the moment, only one commit can happen at a time
extern ACE_Mutex commit_lock;

void no_transaction_exception(const Versioned_Object * obj) __attribute__((__noreturn__));
size_t current_trans_epoch();



/*****************************************************************************/
/* TRANSACTION                                                               */
/*****************************************************************************/

/// A transaction is both a snapshot and a sandbox.
struct Transaction : public Snapshot, public Sandbox {

    bool commit();

    void dump(std::ostream & stream = std::cerr, int indent = 0);
};

/*****************************************************************************/
/* LOCAL_TRANSACTION                                                         */
/*****************************************************************************/
struct Local_Transaction : public Transaction {
    Local_Transaction()
    {
        old_trans = current_trans;
        current_trans = this;
    }

    ~Local_Transaction()
    {
        current_trans = old_trans;
    }

    Transaction * old_trans;
};


} // namespace JMVCC

#endif /* __jmvcc__transaction_h__ */
