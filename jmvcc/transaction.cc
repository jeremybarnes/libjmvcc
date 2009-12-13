/* transaction.cc
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Implementation of transactions.
*/

#include "transaction.h"


using namespace std;


namespace JMVCC {

/// Current transaction for this thread
__thread Transaction * current_trans = 0;

/// For the moment, only one commit can happen at a time
ACE_Mutex commit_lock;


void no_transaction_exception(const Versioned_Object * obj)
{
    throw Exception("not in a transaction");
}

size_t current_trans_epoch()
{
    return (current_trans ? current_trans->epoch() : 0);
}



/*****************************************************************************/
/* TRANSACTION                                                               */
/*****************************************************************************/

bool
Transaction::
commit()
{
    status = COMMITTING;
    bool result = Sandbox::commit(epoch());
    status = result ? COMMITTED : FAILED;
    if (!result) restart();
    return result;
}

void
Transaction::
dump(std::ostream & stream, int indent)
{
    string s(indent, ' ');
    stream << s << "snapshot: epoch " << epoch() << " retries "
           << retries() << endl;
    stream << s << "sandbox" << endl;
    Sandbox::dump(stream, indent);
}

} // namespace JMVCC

