/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_coordinator.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/util/concurrency/list.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class OperationContext;

namespace repl {

    /**
     * Represents a latency measurement for each replica set member based on heartbeat requests.
     * The measurement is an average weighted 80% to the old value, and 20% to the new value.
     *
     * Also stores information about heartbeat progress and retries.
     */
    class PingStats {
    public:
        PingStats();

        /**
         * Records that a new heartbeat request started at "now".
         *
         * This resets the failure count used in determining whether the next request to a target
         * should be a retry or a regularly scheduled heartbeat message.
         */
        void start(Date_t now);

        /**
         * Records that a heartbeat request completed successfully, and that "millis" milliseconds
         * were spent for a single network roundtrip plus remote processing time.
         */
        void hit(int millis);

        /**
         * Records that a heartbeat request failed.
         */
        void miss();

        /**
         * Gets the number of hit() calls.
         */
        unsigned int getCount() const { return count; }

        /**
         * Gets the weighted average round trip time for heartbeat messages to the target.
         */
        unsigned int getMillis() const { return value; }

        /**
         * Gets the date at which start() was last called, which is used to determine if
         * a heartbeat should be retried or if the time limit has expired.
         */
        Date_t getLastHeartbeatStartDate() const { return _lastHeartbeatStartDate; }

        /**
         * Gets the number of failures since start() was last called.
         *
         * This value is incremented by calls to miss(), cleared by calls to start() and
         * set to the maximum possible value by calls to hit().
         */
        int getNumFailuresSinceLastStart() const { return _numFailuresSinceLastStart; }

    private:
        unsigned int count;
        unsigned int value;
        Date_t _lastHeartbeatStartDate;
        int _numFailuresSinceLastStart;
    };

    class TopologyCoordinatorImpl : public TopologyCoordinator {
    public:
        /**
         * Constructs a Topology Coordinator object.
         * @param maxSyncSourceLagSecs a sync source is re-evaluated after it lags behind further
         *                             than this amount.
         **/
        TopologyCoordinatorImpl(Seconds maxSyncSourceLagSecs);

        ////////////////////////////////////////////////////////////
        //
        // Implementation of TopologyCoordinator interface
        //
        ////////////////////////////////////////////////////////////

        virtual Role getRole() const;
        virtual MemberState getMemberState() const;
        virtual HostAndPort getSyncSourceAddress() const;
        virtual std::vector<HostAndPort> getMaybeUpHostAndPorts() const;
        virtual int getMaintenanceCount() const;
        virtual void setForceSyncSourceIndex(int index);
        virtual HostAndPort chooseNewSyncSource(Date_t now, 
                                                const OpTime& lastOpApplied);
        virtual void blacklistSyncSource(const HostAndPort& host, Date_t until);
        virtual void setStepDownTime(Date_t newTime);
        virtual void setFollowerMode(MemberState::MS newMode);
        virtual void adjustMaintenanceCountBy(int inc);
        virtual void prepareSyncFromResponse(const ReplicationExecutor::CallbackData& data,
                                             const HostAndPort& target,
                                             const OpTime& lastOpApplied,
                                             BSONObjBuilder* response,
                                             Status* result);
        virtual void prepareFreshResponse(const ReplicationExecutor::CallbackData& data,
                                          const ReplicationCoordinator::ReplSetFreshArgs& args,
                                          const OpTime& lastOpApplied,
                                          BSONObjBuilder* response,
                                          Status* result);
        virtual void prepareElectResponse(const ReplicationExecutor::CallbackData& data,
                                          const ReplicationCoordinator::ReplSetElectArgs& args,
                                          const Date_t now,
                                          BSONObjBuilder* response,
                                          Status* result);
        virtual void prepareHeartbeatResponse(const ReplicationExecutor::CallbackData& data,
                                              Date_t now,
                                              const ReplSetHeartbeatArgs& args,
                                              const std::string& ourSetName,
                                              const OpTime& lastOpApplied,
                                              ReplSetHeartbeatResponse* response,
                                              Status* result);
        virtual void prepareStatusResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           unsigned uptime,
                                           const OpTime& lastOpApplied,
                                           BSONObjBuilder* response,
                                           Status* result);
        virtual void prepareFreezeResponse(const ReplicationExecutor::CallbackData& data,
                                           Date_t now,
                                           int secs,
                                           BSONObjBuilder* response,
                                           Status* result);
        virtual void updateConfig(const ReplicaSetConfig& newConfig,
                                  int selfIndex,
                                  Date_t now,
                                  const OpTime& lastOpApplied);
        virtual std::pair<ReplSetHeartbeatArgs, Milliseconds> prepareHeartbeatRequest(
                Date_t now,
                const std::string& ourSetName,
                const HostAndPort& target);
        virtual HeartbeatResponseAction processHeartbeatResponse(
                Date_t now,
                Milliseconds networkRoundTripTime,
                const HostAndPort& target,
                const StatusWith<ReplSetHeartbeatResponse>& hbResponse,
                OpTime myLastOpApplied);
        virtual bool voteForMyself(Date_t now);
        virtual void processWinElection(
                Date_t now,
                OID electionId,
                OpTime myLastOpApplied,
                OpTime electionOpTime);
        virtual void processLoseElection(Date_t now, OpTime myLastOpApplied);
        virtual void stepDown();
        virtual Date_t getStepDownTime() const;

        ////////////////////////////////////////////////////////////
        //
        // Test support methods
        //
        ////////////////////////////////////////////////////////////

        // Changes _memberState to newMemberState.  Only for testing.
        void changeMemberState_forTest(const MemberState& newMemberState,
                                       OpTime electionTime = OpTime(0,0));

        // Sets "_electionTime" to "newElectionTime".  Only for testing.
        void _setElectionTime(const OpTime& newElectionTime);

        // Sets _currentPrimaryIndex to the given index.  Should only be used in unit tests!
        // TODO(spencer): Remove this once we can easily call for an election in unit tests to
        // set the current primary.
        void _setCurrentPrimaryForTest(int primaryIndex);

        // Returns _currentPrimaryIndex.  Only used in unittests.
        int getCurrentPrimaryIndex() const;

    private:

        enum UnelectableReason {
            None,
            CannotSeeMajority,
            NotCloseEnoughToLatestOptime,
            ArbiterIAm,
            NotSecondary,
            NoPriority,
            StepDownPeriodActive
        };

        // Returns the number of heartbeat pings which have occurred.
        int _getTotalPings();

        // Returns the current "ping" value for the given member by their address
        int _getPing(const HostAndPort& host);

        // Determines if we will veto the member specified by "memberID", given that the last op
        // we have applied locally is "lastOpApplied".
        // If we veto, the errmsg will be filled in with a reason
        bool _shouldVetoMember(unsigned int memberID,
                               const OpTime& lastOpApplied,
                               std::string* errmsg) const;

        // Returns the index of the member with the matching id, or -1 if none match.
        int _getMemberIndex(int id) const;

        // Sees if a majority number of votes are held by members who are currently "up"
        bool _aMajoritySeemsToBeUp() const;

        // Is optime close enough to the latest known optime to qualify for an election
        bool _isOpTimeCloseEnoughToLatestToElect(const OpTime lastApplied) const;

        // Returns reason why "self" member is unelectable
        UnelectableReason _getMyUnelectableReason(const Date_t now,
                                                  const OpTime lastOpApplied) const;

        // Returns reason why memberIndex is unelectable
        UnelectableReason _getUnelectableReason(int memberIndex) const;

        // Returns the nice text of why the node is unelectable
        std::string _getUnelectableReasonString(UnelectableReason ur) const;

        // Return true if we are currently primary
        bool _iAmPrimary() const;

        // Returns the total number of votes in the current config
        int _totalVotes() const;

        // Scans through all members that are 'up' and return the latest known optime
        OpTime _latestKnownOpTime() const;

        // Scans the electable set and returns the highest priority member index
        int _getHighestPriorityElectableIndex() const;

        // Returns true if "one" member is higher priority than "two" member
        bool _isMemberHigherPriority(int memberOneIndex, int memberTwoIndex) const;

        // Helper shortcut to self config
        const MemberConfig& _selfConfig() const;

        // Returns NULL if there is no primary, or the MemberConfig* for the current primary
        const MemberConfig* _currentPrimaryMember() const;

        /**
         * Performs updating "_hbdata" and "_currentPrimaryIndex" for processHeartbeatResponse().
         */
        HeartbeatResponseAction _updateHeartbeatDataImpl(
                int updatedConfigIndex,
                Date_t now,
                const OpTime& lastOpApplied);

        HeartbeatResponseAction _stepDownSelf();
        HeartbeatResponseAction _stepDownSelfAndReplaceWith(int newPrimary);

        MemberState _getMyState() const;

        // This node's role in the replication protocol.
        Role _role;

        // This is a unique id that is generated and set each time we transition to PRIMARY, as the
        // result of an election.
        OID _electionId;
        // The time at which the current PRIMARY was elected.
        OpTime _electionTime;

        // the index of the member we currently believe is primary, if one exists, otherwise -1
        int _currentPrimaryIndex;
        // the hostandport we are currently syncing from
        // empty if no sync source (we are primary, or we cannot connect to anyone yet)
        HostAndPort _syncSource;
        // These members are not chosen as sync sources for a period of time, due to connection
        // issues with them
        std::map<HostAndPort, Date_t> _syncSourceBlacklist;
        // The next sync source to be chosen, requested via a replSetSyncFrom command
        int _forceSyncSourceIndex;
        // How far this node must fall behind before considering switching sync sources
        Seconds _maxSyncSourceLagSecs;

        // insanity follows

        // "heartbeat message"
        // sent in requestHeartbeat respond in field "hbm"
        char _hbmsg[256]; // we change this unlocked, thus not a std::string
        Date_t _hbmsgTime; // when it was logged
        void _sethbmsg(const std::string& s, int logLevel = 0);
        // heartbeat msg to send to others; descriptive diagnostic info
        std::string _getHbmsg() const {
            if ( time(0)-_hbmsgTime > 120 ) return "";
            return _hbmsg;
        }

        int _selfIndex; // this node's index in _members and _currentConfig

        ReplicaSetConfig _currentConfig; // The current config, including a vector of MemberConfigs
        // heartbeat data for each member.  It is guaranteed that this vector will be maintained
        // in the same order as the MemberConfigs in _currentConfig, therefore the member config
        // index can be used to index into this vector as well.
        std::vector<MemberHeartbeatData> _hbdata;

        // Time when stepDown command expires
        Date_t _stepDownUntil;

        // The number of calls we have had to enter maintenance mode
        int _maintenanceModeCalls;

        // The sub-mode of follower that we are in.  Legal values are RS_SECONDARY, RS_RECOVERING,
        // RS_STARTUP2 (initial sync) and RS_ROLLBACK.  Only meaningful if _role == Role::follower.
        // Configured via setFollowerMode().  If the sub-mode is RS_SECONDARY, then the effective
        // sub-mode is either RS_SECONDARY or RS_RECOVERING, depending on _maintenanceModeCalls.
        // Rather than accesing this variable direclty, one should use the getMemberState() method,
        // which computes the replica set node state on the fly.
        MemberState::MS _followerMode;

        typedef std::map<HostAndPort, PingStats> PingMap;
        // Ping stats for each member by HostAndPort;
        PingMap _pings;

        // Last vote info from the election
        struct LastVote {

            static const Seconds leaseTime;

            LastVote() : when(0), whoId(-1) { }
            Date_t when;
            int whoId;
            HostAndPort whoHostAndPort;
        } _lastVote;

    };

} // namespace repl
} // namespace mongo
