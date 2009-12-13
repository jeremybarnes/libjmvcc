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


/// Current transaction for this thread
__thread Transaction * current_trans = 0;

size_t current_trans_epoch();

/// For the moment, only one commit can happen at a time
ACE_Mutex commit_lock;


/*****************************************************************************/
/* TRANSACTION                                                               */
/*****************************************************************************/

/// A transaction is both a snapshot and a sandbox.
struct Transaction : public Snapshot, public Sandbox {

    bool commit()
    {
        status = COMMITTING;
        bool result = Sandbox::commit(epoch());
        status = result ? COMMITTED : FAILED;
        if (!result) restart();
        return result;
    }

    void dump(std::ostream & stream = std::cerr, int indent = 0)
    {
        string s(indent, ' ');
        stream << s << "snapshot: epoch " << epoch() << " retries "
               << retries() << endl;
        stream << s << "sandbox" << endl;
        Sandbox::dump(stream, indent);
    }
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

void no_transaction_exception(const Versioned_Object * obj)
{
    throw Exception("not in a transaction");
}

size_t current_trans_epoch()
{
    return (current_trans ? current_trans->epoch() : 0);
}




} // namespace JMVCC

#endif /* __jmvcc__transaction_h__ */
