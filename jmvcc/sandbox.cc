/* sandbox.cc
   Jeremy Barnes, 12 December 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Implementation of the sandbox.
*/

#include "sandbox.h"
#include "transaction.h"
#include "jml/arch/atomic_ops.h"


using namespace std;

namespace JMVCC {


/*****************************************************************************/
/* SANDBOX                                                                   */
/*****************************************************************************/

Sandbox::
~Sandbox()
{
    clear();
}

void
Sandbox::
clear()
{
    for (Local_Values::iterator
             it = local_values.begin(),
             end = local_values.end();
         it != end;  ++it) {
        free(it->second.val);
    }
    local_values.clear();
}

Epoch
Sandbox::
commit(Epoch old_epoch)
{
    ACE_Guard<ACE_Mutex> guard(commit_lock);

    Epoch new_epoch = get_current_epoch() + 1;

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
        memory_barrier();

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
        
    return (result ? new_epoch : 0);
}

void
Sandbox::
dump(std::ostream & stream, int indent) const
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

std::ostream &
operator << (std::ostream & stream,
             const Sandbox::Entry & entry)
{
    return stream << entry.print();
}

} // namespace JMVCC
