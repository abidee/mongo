/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/runner_yield_policy.h"
#include "mongo/db/pdfile.h"

#pragma once

namespace mongo {

    /**
     * A PlanExecutor is the abstraction that knows how to crank a tree of stages into execution.
     * The executor is usually part of a larger abstraction that is interacting with the cache
     * and/or the query optimizer.
     *
     * Executes a plan.  Used by a runner.  Calls work() on a plan until a result is produced.
     * Stops when the plan is EOF or if the plan errors.
     */
    class PlanExecutor {
    public:
        PlanExecutor(WorkingSet* ws, PlanStage* rt) : _workingSet(ws),
                                                      _root(rt),
                                                      _killed(false) { }

        WorkingSet* getWorkingSet() { return _workingSet.get(); }

        /**
         * Methods that just pass down to the PlanStage tree.
         */
        void saveState() {
            if (!_killed) { _root->prepareToYield(); }
        }

        bool restoreState() {
            if (!_killed) {
                _root->recoverFromYield();
            }
            return !_killed;
        }

        void invalidate(const DiskLoc& dl) {
            if (!_killed) { _root->invalidate(dl); }
        }

        /**
         * During the yield, the database we're operating over or any collection we're relying on
         * may be dropped.  When this happens all cursors and runners on that database and
         * collection are killed or deleted in some fashion. (This is how the _killed gets set.)
         */
        void kill() { _killed = true; }

        // This is OK even if we were killed.
        PlanStageStats* getStats() { return _root->getStats(); }

        void setYieldPolicy(Runner::YieldPolicy policy) {
            if (Runner::YIELD_MANUAL == policy) {
                _yieldPolicy.reset();
            }
            else {
                _yieldPolicy.reset(new RunnerYieldPolicy());
            }
        }

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
            if (_killed) { return Runner::RUNNER_DEAD; }

            for (;;) {
                WorkingSetID id;
                PlanStage::StageState code = _root->work(&id);

                if (PlanStage::ADVANCED == code) {
                    WorkingSetMember* member = _workingSet->get(id);

                    if (NULL != objOut) {
                        if (member->hasObj()) {
                            *objOut = member->obj;
                        }
                        else {
                            // TODO: When we deal with projection and inclusion/exclusion, we'd fill
                            // out 'objOut' from the index data here.
                            _workingSet->free(id);
                            return Runner::RUNNER_ERROR;
                        }
                    }

                    if (NULL != dlOut) {
                        if (member->hasLoc()) {
                            *dlOut = member->loc;
                        }
                        else {
                            _workingSet->free(id);
                            return Runner::RUNNER_ERROR;
                        }
                    }
                    _workingSet->free(id);
                    return Runner::RUNNER_ADVANCED;
                }
                else if (PlanStage::NEED_TIME == code) {
                    // Fall through to yield check at end of large conditional.
                }
                else if (PlanStage::NEED_FETCH == code) {
                    // id has a loc and refers to an obj we need to fetch.
                    WorkingSetMember* member = _workingSet->get(id);

                    // This must be true for somebody to request a fetch and can only change when an
                    // invalidation happens, which is when we give up a lock.  Don't give up the
                    // lock between receiving the NEED_FETCH and actually fetching(?).
                    verify(member->hasLoc());

                    // Actually bring record into memory.
                    Record* record = member->loc.rec();

                    // If we're allowed to, go to disk outside of the lock.
                    if (NULL != _yieldPolicy.get()) {
                        saveState();
                        _yieldPolicy->yield(record);
                        if (_killed) { return Runner::RUNNER_DEAD; }
                        restoreState();
                    }
                    else {
                        // We're set to manually yield.  We go to disk in the lock.
                        record->touch();
                    }

                    // Record should be in memory now.  Log if it's not.
                    if (!Record::likelyInPhysicalMemory(record->dataNoThrowing())) {
                        OCCASIONALLY {
                            warning() << "Record wasn't in memory immediately after fetch: "
                                      << member->loc.toString() << endl;
                        }
                    }

                    // Note that we're not freeing id.  Fetch semantics say that we shouldn't.
                }
                else if (PlanStage::IS_EOF == code) {
                    return Runner::RUNNER_EOF;
                }
                else {
                    verify(PlanStage::FAILURE == code);
                    return Runner::RUNNER_DEAD;
                }

                // Yield, if we can yield ourselves.
                if (NULL != _yieldPolicy.get() && _yieldPolicy->shouldYield()) {
                    saveState();
                    _yieldPolicy->yield();
                    if (_killed) { return Runner::RUNNER_DEAD; }
                    restoreState();
                }
            }
        }

    private:
        scoped_ptr<WorkingSet> _workingSet;
        scoped_ptr<PlanStage> _root;
        scoped_ptr<RunnerYieldPolicy> _yieldPolicy;

        // Did somebody drop an index we care about or the namespace we're looking at?  If so, we'll
        // be killed.
        bool _killed;
    };

}  // namespace mongo
