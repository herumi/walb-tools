#pragma once
#include "walb_types.hpp"
#include "state_machine.hpp"

namespace walb {

/**
 * States.
 */
const char *const aClear = "Clear";
const char *const aSyncReady = "SyncReady";
const char *const aArchived = "Archived";
const char *const aStopped = "Stopped";

/**
 * Temporary states.
 */
const char *const atInitVol = "InitVol";
const char *const atClearVol = "ClearVol";
const char *const atResetVol = "ResetVol";
const char *const atFullSync = "FullSync";
const char *const atHashSync = "HashSync";
const char *const atWdiffRecv = "WdiffRecv";
const char *const atReplSync = "ReplSyncAsServer";
const char *const atStop = "Stop";
const char *const atStart = "Start";

const struct StateMachine::Pair statePairTbl[] = {
    { aClear, atInitVol },
    { atInitVol, aSyncReady },
    { aSyncReady, atClearVol },
    { atClearVol, aClear },

    { aSyncReady, atFullSync },
    { atFullSync, aArchived },

    { aSyncReady, atReplSync },
    { atReplSync, aArchived },

    { aArchived, atHashSync },
    { atHashSync, aArchived },
    { aArchived, atWdiffRecv },
    { atWdiffRecv, aArchived },
    { aArchived, atReplSync },
    { atReplSync, aArchived },

    { aArchived, atStop },
    { atStop, aStopped },

    { aStopped, atClearVol },
    { atClearVol, aClear },
    { aStopped, atStart },
    { atStart, aArchived },
    { aStopped, atResetVol },
    { atResetVol, aSyncReady },
};

/**
 * Actions.
 */
const char *const aMerge = "Merge";
const char *const aApply = "Apply";
const char *const aRestore = "Restore";
const char *const aReplSync = "ReplSyncAsClient";
const char *const aResize = "Resize";

const StrVec allActionVec = {aMerge, aApply, aRestore, aReplSync, aResize};


const std::string VOLUME_PREFIX = "i_";
const std::string RESTORE_PREFIX = "r_";

} // namespace walb