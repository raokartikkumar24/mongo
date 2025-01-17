/**
 *    Copyright 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/topology_coordinator_impl.h"

#include <limits>

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

    const Seconds TopologyCoordinatorImpl::LastVote::leaseTime = Seconds(3);

namespace {

    // Interval between the time the last heartbeat from a node was received successfully, or
    // the time when we gave up retrying, and when the next heartbeat should be sent to a target.
    const Milliseconds kHeartbeatInterval(Seconds(2).total_milliseconds());

    // Maximum number of retries for a failed heartbeat.
    const int kMaxHeartbeatRetries = 2;

}  // namespace

    PingStats::PingStats() :
        count(0),
        value(std::numeric_limits<unsigned int>::max()),
        _lastHeartbeatStartDate(0),
        _numFailuresSinceLastStart(std::numeric_limits<int>::max()) {
    }

    void PingStats::start(Date_t now) {
        _lastHeartbeatStartDate = now;
        _numFailuresSinceLastStart = 0;
    }

    void PingStats::hit(int millis) {
        _numFailuresSinceLastStart = std::numeric_limits<int>::max();
        ++count;
        value = value == std::numeric_limits<unsigned int>::max() ? millis :
            static_cast<unsigned long>((value * .8) + (millis * .2));
    }

    void PingStats::miss() {
        ++_numFailuresSinceLastStart;
    }

    TopologyCoordinatorImpl::TopologyCoordinatorImpl(Seconds maxSyncSourceLagSecs) :
        _role(Role::follower),
        _currentPrimaryIndex(-1),
        _forceSyncSourceIndex(-1),
        _maxSyncSourceLagSecs(maxSyncSourceLagSecs),
        _selfIndex(-1),
        _stepDownUntil(0),
        _maintenanceModeCalls(0),
        _followerMode(MemberState::RS_STARTUP2)
    {
        invariant(getMemberState() == MemberState::RS_STARTUP);
    }

    TopologyCoordinator::Role TopologyCoordinatorImpl::getRole() const {
        return _role;
    }

    void TopologyCoordinatorImpl::setForceSyncSourceIndex(int index) {
        invariant(_forceSyncSourceIndex < _currentConfig.getNumMembers());
        _forceSyncSourceIndex = index;
    }

    HostAndPort TopologyCoordinatorImpl::getSyncSourceAddress() const {
        return _syncSource;
    }

    HostAndPort TopologyCoordinatorImpl::chooseNewSyncSource(Date_t now, 
                                                             const OpTime& lastOpApplied) {

        // if we have a target we've requested to sync from, use it
        if (_forceSyncSourceIndex != -1) {
            invariant(_forceSyncSourceIndex < _currentConfig.getNumMembers());
            _syncSource = _currentConfig.getMemberAt(_forceSyncSourceIndex).getHostAndPort();
            _forceSyncSourceIndex = -1;
            _sethbmsg(str::stream() << "syncing from: " << _syncSource.toString() << " by request",
                      0);
            return _syncSource;
        }

        // wait for 2N pings (not counting ourselves) before choosing a sync target
        int needMorePings = (_hbdata.size() - 1) * 2 - _getTotalPings();

        if (needMorePings > 0) {
            OCCASIONALLY log() << "waiting for " << needMorePings 
                               << " pings from other members before syncing";
            _syncSource = HostAndPort();
            return _syncSource;
        }

        // If we are only allowed to sync from the primary, set that
        if (!_currentConfig.isChainingAllowed()) {
            if (_currentPrimaryIndex == -1) {
                _syncSource = HostAndPort();
                return _syncSource;
            }
            else {
                _syncSource = _currentConfig.getMemberAt(_currentPrimaryIndex).getHostAndPort();
                return _syncSource;
            }
        }

        // find the member with the lowest ping time that is ahead of me

        // Find primary's oplog time. Reject sync candidates that are more than
        // maxSyncSourceLagSecs seconds behind.
        OpTime primaryOpTime;
        if (_currentPrimaryIndex != -1) {
            primaryOpTime = _hbdata[_currentPrimaryIndex].getOpTime();
        }
        else {
            // choose a time that will exclude no candidates, since we don't see a primary
            primaryOpTime = OpTime(_maxSyncSourceLagSecs.total_seconds(), 0);
        }

        if (primaryOpTime.getSecs() < 
            static_cast<unsigned int>(_maxSyncSourceLagSecs.total_seconds())) {
            // erh - I think this means there was just a new election
            // and we don't yet know the new primary's optime
            primaryOpTime = OpTime(_maxSyncSourceLagSecs.total_seconds(), 0);
        }

        OpTime oldestSyncOpTime(primaryOpTime.getSecs() - _maxSyncSourceLagSecs.total_seconds(), 0);

        int closestIndex = -1;

        // Make two attempts.  The first attempt, we ignore those nodes with
        // slave delay higher than our own, hidden nodes, and nodes that are excessively lagged.
        // The second attempt includes such nodes, in case those are the only ones we can reach.
        // This loop attempts to set 'closestIndex'.
        for (int attempts = 0; attempts < 2; ++attempts) {
            for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); 
                 it != _hbdata.end(); 
                 ++it) {
                // Don't consider ourselves.
                if (it->getConfigIndex() == _selfIndex) {
                    continue;
                }
                // Candidate must be up to be considered.
                if (!it->up()) {
                    continue;
                }
                // Candidate must be PRIMARY or SECONDARY state to be considered.
                if (!it->getState().readable()) {
                    continue;
                }
                // Candidate must build indexes if we build indexes, to be considered.
                if (_selfConfig().shouldBuildIndexes()) {
                    if (!_currentConfig.getMemberAt(it->getConfigIndex()).shouldBuildIndexes()) {
                        continue;
                    }
                }

                if (it->getState() == MemberState::RS_SECONDARY) {
                    // only consider secondaries that are ahead of where we are
                    if (it->getOpTime() <= lastOpApplied)
                        continue;
                    // omit secondaries that are excessively behind, on the first attempt at least.
                    if (attempts == 0 &&
                        it->getOpTime() < oldestSyncOpTime)
                        continue;
                }

                // omit nodes that are more latent than anything we've already considered
                if ((closestIndex != -1) &&
                    (_getPing(_currentConfig.getMemberAt(it->getConfigIndex()).getHostAndPort())
                     > _getPing(_currentConfig.getMemberAt(closestIndex).getHostAndPort()))) {
                    continue;
                }

                if (attempts == 0 &&
                    (_selfConfig().getSlaveDelay() < 
                     _currentConfig.getMemberAt(it->getConfigIndex()).getSlaveDelay()
                     || _currentConfig.getMemberAt(it->getConfigIndex()).isHidden())) {
                    continue; // skip this one in the first attempt
                }

                std::map<HostAndPort,Date_t>::iterator vetoed = 
                    _syncSourceBlacklist.find(
                        _currentConfig.getMemberAt(it->getConfigIndex()).getHostAndPort());
                if (vetoed != _syncSourceBlacklist.end()) {
                    // Do some veto housekeeping

                    // if this was on the veto list, check if it was vetoed in the last "while".
                    // if it was, skip.
                    if (vetoed->second > now) {
                        if (now % 5 == 0) {
                            log() << "replSet not trying to sync from " << vetoed->first
                                  << ", it is vetoed for " << (vetoed->second - now) 
                                  << " more seconds";
                        }
                        continue;
                    }
                    _syncSourceBlacklist.erase(vetoed);
                    // fall through, this is a valid candidate now
                }
                // This candidate has passed all tests; set 'closestIndex'
                closestIndex = it->getConfigIndex();
            }
            if (closestIndex != -1) break; // no need for second attempt
        }

        if (closestIndex == -1) {
            // Did not find any members to sync from
            _syncSource = HostAndPort();
            return _syncSource;
        }
        _syncSource = _currentConfig.getMemberAt(closestIndex).getHostAndPort();
        std::string msg(str::stream() << "syncing to: " << _syncSource.toString(), 0);
        _sethbmsg(msg);
        log() << msg;
        return _syncSource;
    }

    void TopologyCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
        _syncSourceBlacklist[host] = until;
    }

    void TopologyCoordinatorImpl::prepareSyncFromResponse(
            const ReplicationExecutor::CallbackData& data,
            const HostAndPort& target,
            const OpTime& lastOpApplied,
            BSONObjBuilder* response,
            Status* result) {
        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        response->append("syncFromRequested", target.toString());

        const MemberConfig& selfConfig = _selfConfig();
        if (selfConfig.isArbiter()) {
            *result = Status(ErrorCodes::NotSecondary, "arbiters don't sync");
            return;
        }
        if (_selfIndex == _currentPrimaryIndex) {
            *result = Status(ErrorCodes::NotSecondary, "primaries don't sync");
            return;
        }

        ReplicaSetConfig::MemberIterator targetConfig = _currentConfig.membersEnd();
        int targetIndex = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
                it != _currentConfig.membersEnd(); ++it) {
            if (it->getHostAndPort() == target) {
                targetConfig = it;
                break;
            }
            ++targetIndex;
        }
        if (targetConfig == _currentConfig.membersEnd()) {
            *result = Status(ErrorCodes::NodeNotFound,
                             str::stream() << "Could not find member \"" << target.toString() <<
                                     "\" in replica set");
            return;
        }
        if (targetIndex == _selfIndex) {
            *result = Status(ErrorCodes::InvalidOptions, "I cannot sync from myself");
            return;
        }
        if (targetConfig->isArbiter()) {
            *result = Status(ErrorCodes::InvalidOptions,
                             str::stream() << "Cannot sync from \"" << target.toString() <<
                                     "\" because it is an arbiter");
            return;
        }
        if (!targetConfig->shouldBuildIndexes() && selfConfig.shouldBuildIndexes()) {
            *result = Status(ErrorCodes::InvalidOptions,
                             str::stream() << "Cannot sync from \"" << target.toString() <<
                                     "\" because it does not build indexes");
            return;
        }

        const MemberHeartbeatData& hbdata = _hbdata[targetIndex];
        if (hbdata.hasAuthIssue()) {
            *result = Status(ErrorCodes::Unauthorized,
                             str::stream() << "not authorized to communicate with " <<
                                     target.toString());
            return;
        }
        if (hbdata.getHealth() == 0) {
            *result = Status(ErrorCodes::HostUnreachable,
                             str::stream() << "I cannot reach the requested member: " <<
                                     target.toString());
            return;
        }
        if (hbdata.getOpTime().getSecs()+10 < lastOpApplied.getSecs()) {
            warning() << "attempting to sync from " << target
                      << ", but its latest opTime is " << hbdata.getOpTime().getSecs()
                      << " and ours is " << lastOpApplied.getSecs() << " so this may not work"
                      << rsLog;
            response->append("warning",
                             str::stream() << "requested member \"" << target.toString() <<
                                     "\" is more than 10 seconds behind us");
            // not returning bad Status, just warning
        }

        HostAndPort prevSyncSource = getSyncSourceAddress();
        if (!prevSyncSource.empty()) {
            response->append("prevSyncTarget", prevSyncSource.toString());
        }

        setForceSyncSourceIndex(targetIndex);
        *result = Status::OK();
    }

    void TopologyCoordinatorImpl::prepareFreshResponse(
            const ReplicationExecutor::CallbackData& cbData,
            const ReplicationCoordinator::ReplSetFreshArgs& args,
            const OpTime& lastOpApplied,
            BSONObjBuilder* response,
            Status* result) {
        if (cbData.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        if (args.setName != _currentConfig.getReplSetName()) {
            *result = Status(ErrorCodes::ReplicaSetNotFound,
                             str::stream() << "Wrong repl set name. Expected: " <<
                                     _currentConfig.getReplSetName() <<
                                     ", received: " << args.setName);
            return;
        }

        bool weAreFresher = false;
        if( _currentConfig.getConfigVersion() > args.cfgver ) {
            log() << "replSet member " << args.who << " is not yet aware its cfg version "
                  << args.cfgver << " is stale";
            response->append("info", "config version stale");
            weAreFresher = true;
        }
        // check not only our own optime, but any other member we can reach
        else if( args.opTime < lastOpApplied ||
                 args.opTime < _latestKnownOpTime())  {
            weAreFresher = true;
        }
        response->appendDate("opTime", lastOpApplied.asDate());
        response->append("fresher", weAreFresher);

        std::string errmsg;
        bool doVeto = _shouldVetoMember(args.id, lastOpApplied, &errmsg);
        response->append("veto", doVeto);
        if (doVeto) {
            response->append("errmsg", errmsg);
        }
        *result = Status::OK();
    }

    bool TopologyCoordinatorImpl::_shouldVetoMember(unsigned int memberID,
                                                    const OpTime& lastOpApplied,
                                                    std::string* errmsg) const {
        const int hopefulIndex = _getMemberIndex(memberID);
        const int highestPriorityIndex = _getHighestPriorityElectableIndex();

        if (hopefulIndex == -1) {
            *errmsg = str::stream() << "replSet couldn't find member with id " << memberID;
            return true;
        }

        if (_iAmPrimary() && lastOpApplied >= _hbdata[hopefulIndex].getOpTime()) {
            // hbinfo is not updated for ourself, so if we are primary we have to check the
            // primary's last optime separately
            *errmsg = str::stream() << "I am already primary, " <<
                _currentConfig.getMemberAt(hopefulIndex).getHostAndPort().toString() << 
                " can try again once I've stepped down";
            return true;
        }

        if (_currentPrimaryIndex != -1 &&
                (hopefulIndex != _currentPrimaryIndex) &&
                (_hbdata[_currentPrimaryIndex].getOpTime() >=
                        _hbdata[hopefulIndex].getOpTime())) {
            // other members might be aware of more up-to-date nodes
            *errmsg = str::stream() <<
                _currentConfig.getMemberAt(hopefulIndex).getHostAndPort().toString() <<
                " is trying to elect itself but " << 
                _currentConfig.getMemberAt(_currentPrimaryIndex).getHostAndPort().toString() <<
                " is already primary and more up-to-date";
            return true;
        }

        if ((highestPriorityIndex != -1)) {
            const MemberConfig& hopefulMember = _currentConfig.getMemberAt(hopefulIndex);
            const MemberConfig& priorityMember = _currentConfig.getMemberAt(highestPriorityIndex);

            if (priorityMember.getPriority() > hopefulMember.getPriority()) {
                *errmsg = str::stream()
                            << hopefulMember.getHostAndPort().toString()
                            << " has lower priority of " << hopefulMember.getPriority() << " than "
                            << priorityMember.getHostAndPort().toString()
                            << " which has a priority of " << priorityMember.getPriority();
                return true;
            }
        }

        UnelectableReason reason = _getUnelectableReason(hopefulIndex);
        if (None != reason) {
            *errmsg = str::stream()
                         << "I don't think "
                         << _currentConfig.getMemberAt(hopefulIndex).getHostAndPort().toString()
                         << " is electable because the " << _getUnelectableReasonString(reason);
            return true;
        }

        return false;
    }

    // produce a reply to a received electCmd
    void TopologyCoordinatorImpl::prepareElectResponse(
            const ReplicationExecutor::CallbackData& data,
            const ReplicationCoordinator::ReplSetElectArgs& args,
            const Date_t now,
            BSONObjBuilder* response,
            Status* result) {

        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        const long long myver = _currentConfig.getConfigVersion();
        const int highestPriorityIndex = _getHighestPriorityElectableIndex();

        const MemberConfig* primary = _currentPrimaryMember();
        const MemberConfig* hopeful = _currentConfig.findMemberByID(args.whoid);
        const MemberConfig* highestPriority = highestPriorityIndex == -1 ? NULL :
                &_currentConfig.getMemberAt(highestPriorityIndex);

        int vote = 0;
        if (args.set != _currentConfig.getReplSetName()) {
            log() << "replSet error received an elect request for '" << args.set
                  << "' but our set name is '" <<
                _currentConfig.getReplSetName() << "'";
        }
        else if ( myver < args.cfgver ) {
            // we are stale.  don't vote
            log() << "replSetElect not voting because our config version is stale. Our version: " <<
                    myver << ", their version: " << args.cfgver;
        }
        else if ( myver > args.cfgver ) {
            // they are stale!
            log() << "replSetElect command received stale config version # during election. "
                    "Our version: " << myver << ", their version: " << args.cfgver;
            vote = -10000;
        }
        else if (!hopeful) {
            log() << "replSetElect couldn't find member with id " << args.whoid;
            vote = -10000;
        }
        else if (_iAmPrimary()) {
            log() << "I am already primary, "  << hopeful->getHostAndPort().toString()
                  << " can try again once I've stepped down";
            vote = -10000;
        }
        else if (primary) {
            log() << hopeful->getHostAndPort().toString() << " is trying to elect itself but "
                  << primary->getHostAndPort().toString() << " is already primary";
            vote = -10000;
        }
        else if (highestPriority && highestPriority->getPriority() > hopeful->getPriority()) {
            // TODO(spencer): What if the lower-priority member is more up-to-date?
            log() << hopeful->getHostAndPort().toString() << " has lower priority than "
                  << highestPriority->getHostAndPort().toString();
            vote = -10000;
        }
        else if (_lastVote.when.millis > 0 &&
                 _lastVote.when.millis + LastVote::leaseTime.total_milliseconds() >= now.millis &&
                 _lastVote.whoId != args.whoid) {
            log() << "replSet voting no for "
                  <<  hopeful->getHostAndPort().toString()
                  << "; voted for " << _lastVote.whoHostAndPort.toString() << ' '
                  << (now.millis - _lastVote.when.millis) / 1000 << " secs ago";
        }
        else {
            _lastVote.when = now;
            _lastVote.whoId = args.whoid;
            _lastVote.whoHostAndPort = hopeful->getHostAndPort();
            vote = _selfConfig().getNumVotes();
            invariant(hopeful->getId() == args.whoid);
            log() << "replSetElect voting yea for " << hopeful->getHostAndPort().toString()
                  << " (" << args.whoid << ')';
        }

        response->append("vote", vote);
        response->append("round", args.round);
        *result = Status::OK();
    }

    // produce a reply to a heartbeat
    void TopologyCoordinatorImpl::prepareHeartbeatResponse(
            const ReplicationExecutor::CallbackData& data,
            Date_t now,
            const ReplSetHeartbeatArgs& args,
            const std::string& ourSetName,
            const OpTime& lastOpApplied,
            ReplSetHeartbeatResponse* response,
            Status* result) {
        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        if (args.getProtocolVersion() != 1) {
            *result = Status(ErrorCodes::BadValue,
                             str::stream() << "replset: incompatible replset protocol version: "
                                           << args.getProtocolVersion());
            return;
        }

        // Verify that replica set names match
        std::string rshb = std::string(args.getSetName());
        if (ourSetName != rshb) {
            *result = Status(ErrorCodes::BadValue, "repl set names do not match");
            log() << "replSet set names do not match, ours: " << ourSetName <<
                "; remote node's: " << rshb;
            response->noteMismatched();
            return;
        }

        // This is a replica set
        response->noteReplSet();

/*
        if( cmdObj["checkEmpty"].trueValue() ) {
            // Eric: XXX takes read lock; only used for initial sync heartbeat
            resultObj->append("hasData", replHasDatabases());
        }
*/

        // Verify that the config's replset name matches
        if (_currentConfig.getReplSetName() != args.getSetName()) {
            *result = Status(ErrorCodes::BadValue, "repl set names do not match (2)");
            response->noteMismatched();
            return; 
        }
        response->setSetName(_currentConfig.getReplSetName());

        const MemberState myState = getMemberState();
        response->setState(myState.s);
        if (myState.primary()) {
            response->setElectionTime(_hbdata[_selfIndex].getElectionTime());
        }

        // Are we electable
        response->setElectable(None == _getMyUnelectableReason(now, lastOpApplied));

        // Heartbeat status message
        response->setHbMsg(_getHbmsg());
        response->setTime(Seconds(Milliseconds(now.asInt64()).total_seconds()));
        response->setOpTime(lastOpApplied.asDate());

        if (!_syncSource.empty()) {
            response->setSyncingTo(_syncSource.toString());
        }

        long long v = _currentConfig.getConfigVersion();
        response->setVersion(v);
        // Deliver new config if caller's version is older than ours
        if (v > args.getConfigVersion()) {
            response->setConfig(_currentConfig);
        }

        // Resolve the caller's id in our Member list
        int from = -1;
        if (v == args.getConfigVersion() && args.getSenderId() != -1) {
            from = _getMemberIndex(args.getSenderId());
        }
        if (from == -1) {
            // Can't find the member, so we leave out the stateDisagreement field
            *result = Status::OK();
            return;
        }

        // if we thought that this node is down, let it know
        if (!_hbdata[from].up()) {
            response->noteStateDisagreement();
        }

        // note that we got a heartbeat from this node
        _hbdata[from].setLastHeartbeatRecv(now);
        *result = Status::OK();
    }


    int TopologyCoordinatorImpl::_getMemberIndex(int id) const {
        int index = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
             it != _currentConfig.membersEnd();
             ++it, ++index) {
            if (it->getId() == id) {
                return index;
            }
        }
        return -1;
    }

    std::pair<ReplSetHeartbeatArgs, Milliseconds> TopologyCoordinatorImpl::prepareHeartbeatRequest(
                Date_t now,
                const std::string& ourSetName,
                const HostAndPort& target) {

        PingStats& hbStats = _pings[target];
        Milliseconds alreadyElapsed(now.asInt64() - hbStats.getLastHeartbeatStartDate().asInt64());
        if ((hbStats.getNumFailuresSinceLastStart() > kMaxHeartbeatRetries) ||
            (alreadyElapsed >= _currentConfig.getHeartbeatTimeoutPeriodMillis())) {

            // This is either the first request ever for "target", or the heartbeat timeout has
            // passed, so we're starting a "new" heartbeat.
            hbStats.start(now);
            alreadyElapsed = Milliseconds(0);
        }
        ReplSetHeartbeatArgs hbArgs;
        hbArgs.setProtocolVersion(1);
        hbArgs.setCheckEmpty(false);
        if (_currentConfig.isInitialized()) {
            hbArgs.setSetName(_currentConfig.getReplSetName());
            hbArgs.setConfigVersion(_currentConfig.getConfigVersion());
            if (_selfIndex >= 0) {
                const MemberConfig& me = _selfConfig();
                hbArgs.setSenderHost(me.getHostAndPort());
                hbArgs.setSenderId(me.getId());
            }
        }
        else {
            hbArgs.setSetName(ourSetName);
            hbArgs.setConfigVersion(0);
        }

        Milliseconds timeout(_currentConfig.getHeartbeatTimeoutPeriodMillis().total_milliseconds() -
                             alreadyElapsed.total_milliseconds()); 
        return std::make_pair(hbArgs, timeout);
    }

namespace {
    int findMemberIndexForHostAndPort(const ReplicaSetConfig& config, const HostAndPort& host) {
        for (int i = 0; i < config.getNumMembers(); ++i) {
            if (config.getMemberAt(i).getHostAndPort() == host) {
                return i;
            }
        }
        return -1;
    }
}  // namespace

    HeartbeatResponseAction TopologyCoordinatorImpl::processHeartbeatResponse(
            Date_t now,
            Milliseconds networkRoundTripTime,
            const HostAndPort& target,
            const StatusWith<ReplSetHeartbeatResponse>& hbResponse,
            OpTime myLastOpApplied) {

        PingStats& hbStats = _pings[target];
        invariant(hbStats.getLastHeartbeatStartDate() != Date_t(0));
        if (!hbResponse.isOK()) {
            hbStats.miss();
        }
        else {
            hbStats.hit(networkRoundTripTime.total_milliseconds());
            // Log diagnostics.
            if (hbResponse.getValue().isStateDisagreement()) {
                LOG(1) << target <<
                    " thinks that we are down because they cannot send us heartbeats.";
            }
        }

        Milliseconds alreadyElapsed(now.asInt64() - hbStats.getLastHeartbeatStartDate().asInt64());
        Date_t nextHeartbeatStartDate;
        if ((hbStats.getNumFailuresSinceLastStart() <= kMaxHeartbeatRetries) &&
            (alreadyElapsed < _currentConfig.getHeartbeatTimeoutPeriodMillis())) {

            if (!hbResponse.isOK()) {
                LOG(1) << "Bad heartbeat response from " << target <<
                    "; trying again; Retries left: " <<
                    (kMaxHeartbeatRetries - hbStats.getNumFailuresSinceLastStart()) <<
                    "; " << alreadyElapsed.total_milliseconds() << "ms have already elapsed";
            }
            nextHeartbeatStartDate = now;
        }
        else {
            nextHeartbeatStartDate = now + kHeartbeatInterval.total_milliseconds();
        }

        if (hbResponse.isOK() && hbResponse.getValue().hasConfig()) {
            const ReplicaSetConfig& newConfig = hbResponse.getValue().getConfig();
            if (newConfig.getConfigVersion() > _currentConfig.getConfigVersion()) {
                HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeReconfigAction();
                nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
                return nextAction;
            }
            else {
                // Could be we got the newer version before we got the response, or the
                // target erroneously sent us one, even through it isn't newer.
                if (newConfig.getConfigVersion() < _currentConfig.getConfigVersion()) {
                    LOG(1) << "Config version from heartbeat was older than ours.";
                }
                else {
                    LOG(2) << "Config from heartbeat response was same as ours.";
                }
                LOG(2) << "Current Config: " << _currentConfig.toBSON()
                       << " config in heartbeat: " << newConfig.toBSON();
            }
        }

        // Check if the heartbeat target is in our config.  If it isn't, there's nothing left to do,
        // so return early.
        const int memberIndex = findMemberIndexForHostAndPort(_currentConfig, target);
        if (memberIndex == -1) {
            LOG(1) << "replset: Could not find " << target  << " in current config so ignoring --"
                " current config: " << _currentConfig.toBSON();
            HeartbeatResponseAction nextAction = HeartbeatResponseAction::makeNoAction();
            nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
            return nextAction;
        }

        MemberHeartbeatData& hbData = _hbdata[memberIndex];
        if (!hbResponse.isOK()) {
            hbData.setDownValues(now, hbResponse.getStatus().reason());
        }
        else {
            const ReplSetHeartbeatResponse& hbr = hbResponse.getValue();
            hbData.setUpValues(
                    now,
                    hbr.hasState() ? hbr.getState() : MemberState::RS_UNKNOWN,
                    hbr.hasElectionTime() ? hbr.getElectionTime() : hbData.getElectionTime(),
                    hbr.hasOpTime() ? hbr.getOpTime() : hbData.getOpTime(),
                    hbr.getSyncingTo(),
                    hbr.getHbMsg());
        }
        HeartbeatResponseAction nextAction = _updateHeartbeatDataImpl(
                memberIndex,
                now,
                myLastOpApplied);

        nextAction.setNextHeartbeatStartDate(nextHeartbeatStartDate);
        return nextAction;
    }

    HeartbeatResponseAction TopologyCoordinatorImpl::_updateHeartbeatDataImpl(
            int updatedConfigIndex,
            Date_t now,
            const OpTime& lastOpApplied) {

        // This method has two interrelated responsibilities, performed in two phases.
        //
        // First, it updates the local notion of which remote node, if any is primary.  In the
        // process, it may request a remote primary to step down because there is a higher priority
        // node waiting, or because the local node thinks it is primary and that it has a more
        // recent electionTime.  It may instead decide that the local node should step down itself,
        // because a remote has a more recent election time.
        //
        // Second, if there is no remote primary, and the local node is not primary, it considers
        // whether or not to stand for election.
        invariant(updatedConfigIndex != _selfIndex);

        ////////////////////
        // Phase 1
        ////////////////////

        // If we believe the node whose data was just updated is primary, confirm that
        // the updated data supports that notion.  If not, erase our notion of who is primary.
        if (updatedConfigIndex == _currentPrimaryIndex) {
            const MemberHeartbeatData& updatedHBData = _hbdata[updatedConfigIndex];
            if (!updatedHBData.up() || !updatedHBData.getState().primary()) {
                _currentPrimaryIndex = -1;
            }
        }

        // If the current primary is not highest priority and up to date (within 10s),
        // have them/me stepdown.
        if (_currentPrimaryIndex != -1) {
            // check if we should ask the primary (possibly ourselves) to step down
            const int highestPriorityIndex = _getHighestPriorityElectableIndex();
            if (highestPriorityIndex != -1) {
                const MemberConfig& currentPrimaryMember =
                    _currentConfig.getMemberAt(_currentPrimaryIndex);
                const MemberConfig& highestPriorityMember =
                    _currentConfig.getMemberAt(highestPriorityIndex);
                const OpTime highestPriorityMemberOptime =
                    _hbdata[highestPriorityIndex].getOpTime();

                if ((highestPriorityMember.getPriority() > currentPrimaryMember.getPriority()) &&
                    _isOpTimeCloseEnoughToLatestToElect(highestPriorityMemberOptime)) {

                    log() << "stepping down "
                          << currentPrimaryMember.getHostAndPort().toString()
                          << " (priority " << currentPrimaryMember.getPriority() << "), "
                          << highestPriorityMember.getHostAndPort().toString()
                          << " is priority " << highestPriorityMember.getPriority()
                          << " and "
                          << (_latestKnownOpTime().getSecs() - highestPriorityMemberOptime.getSecs())
                          << " seconds behind";
                    if (_iAmPrimary()) {
                        return _stepDownSelf();
                    }
                    else {
                        _currentPrimaryIndex = -1;
                        return HeartbeatResponseAction::makeStepDownRemoteAction(
                                _currentPrimaryIndex);
                    }
                }
            }
        }

        // Scan the member list's heartbeat data for who is primary, and update
        // _currentPrimaryIndex and _role, or request a remote to step down, as necessary.
        {
            int remotePrimaryIndex = -1;
            for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin();
                 it != _hbdata.end(); 
                 ++it) {
                if (it->getConfigIndex() == _selfIndex) {
                    continue;
                }

                if( it->getState().primary() && it->up() ) {
                    if (remotePrimaryIndex != -1) {
                        // two other nodes think they are primary (asynchronously polled) 
                        // -- wait for things to settle down.
                        log() << "replSet info two remote primaries (transiently)";
                        return HeartbeatResponseAction::makeNoAction();
                    }
                    remotePrimaryIndex = it->getConfigIndex();
                }
            }

            if (remotePrimaryIndex != -1) {
                // If it's the same as last time, don't do anything further.
                if (_currentPrimaryIndex == remotePrimaryIndex) {
                    return HeartbeatResponseAction::makeNoAction();
                }
                // Clear last heartbeat message on ourselves (why?)
                _sethbmsg("");

                // If we are also primary, this is a problem.  Determine who should step down.
                if (_iAmPrimary()) {
                    OpTime remoteElectionTime = _hbdata[remotePrimaryIndex].getElectionTime();
                    log() << "replset: another primary seen with election time " 
                          << remoteElectionTime << " my elction time is " << _electionTime;

                    // Step down whomever has the older election time.
                    if (remoteElectionTime > _electionTime) {
                        log() << "stepping down; another primary was elected more recently";
                        return _stepDownSelfAndReplaceWith(remotePrimaryIndex);
                    }
                    else {
                        log() << "another PRIMARY detected and it should step down"
                                 " since it was elected earlier than me";
                        return HeartbeatResponseAction::makeStepDownRemoteAction(
                                remotePrimaryIndex);
                    }
                }

                _currentPrimaryIndex = remotePrimaryIndex;
                return HeartbeatResponseAction::makeNoAction();
            }
        }

        ////////////////////
        // Phase 2
        ////////////////////

        // We do not believe any remote to be primary.

        // If we are primary, check if we can still see majority of the set;
        // stepdown if we can't.
        if (_iAmPrimary()) {
            if (CannotSeeMajority == _getMyUnelectableReason(now, lastOpApplied)) {
                log() << "can't see a majority of the set, relinquishing primary";
                return _stepDownSelf();
            }

            return HeartbeatResponseAction::makeNoAction();
        }

        fassert(18505, _currentPrimaryIndex == -1);

        // At this point, there is no primary anywhere.  Check to see if we should become a
        // candidate.

        if (_role == Role::candidate) {
            return HeartbeatResponseAction::makeNoAction();
        }

        if (None != _getMyUnelectableReason(now, lastOpApplied)) {
            return HeartbeatResponseAction::makeNoAction();
        }

        // All checks passed, become a candidate and start election proceedings.
        _role = Role::candidate;
        return HeartbeatResponseAction::makeElectAction();
    }

    bool TopologyCoordinatorImpl::_aMajoritySeemsToBeUp() const {
        int vUp = 0;
        for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); 
             it != _hbdata.end(); 
             ++it) {
            if (it->up()) {
                vUp += _currentConfig.getMemberAt(it->getConfigIndex()).getNumVotes();
            }
        }

        return vUp * 2 > _totalVotes();
    }

    bool TopologyCoordinatorImpl::_isOpTimeCloseEnoughToLatestToElect(const OpTime lastApplied)
                                                                                            const {
        const unsigned int latestKnownOpTimeSecs = _latestKnownOpTime().getSecs();
        return latestKnownOpTimeSecs != 0 && lastApplied.getSecs() >= (latestKnownOpTimeSecs - 10);
    }

    bool TopologyCoordinatorImpl::_iAmPrimary() const {
        if (_role == Role::leader) {
            invariant(_currentPrimaryIndex == _selfIndex);
            return true;
        }
        return false;
    }

    int TopologyCoordinatorImpl::_totalVotes() const {
        static int complain = 0;
        int vTot = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
             it != _currentConfig.membersEnd();
             ++it) {
            vTot += it->getNumVotes();
        }
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning: even number of voting members in replica set config - "
                     "add an arbiter or set votes to 0 on one of the existing members";
        return vTot;
    }

    OpTime TopologyCoordinatorImpl::_latestKnownOpTime() const {
        OpTime latest(0,0);

        for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); 
             it != _hbdata.end(); 
             ++it) {

            if (!it->up()) {
                continue;
            }

            if (it->getOpTime() > latest) {
                latest = it->getOpTime();
            }
        }

        return latest;
    }

    bool TopologyCoordinatorImpl::_isMemberHigherPriority(int memberOneIndex,
                                                          int memberTwoIndex) const {
        if (memberOneIndex == -1)
            return false;

        if (memberTwoIndex == -1)
            return true;

        return _currentConfig.getMemberAt(memberOneIndex).getPriority() >
                _currentConfig.getMemberAt(memberTwoIndex).getPriority();
    }

    int TopologyCoordinatorImpl::_getHighestPriorityElectableIndex() const {
        int maxIndex = -1;
        for (int currentIndex = 0; currentIndex < _currentConfig.getNumMembers(); currentIndex++) {
            if (None == _getUnelectableReason(currentIndex) &&
                                                 _isMemberHigherPriority(currentIndex, maxIndex)) {
                maxIndex = currentIndex;
            }
        }

        return maxIndex;
    }

    void TopologyCoordinatorImpl::changeMemberState_forTest(const MemberState& newMemberState,
                                                            OpTime electionTime) {
        invariant(_selfIndex != -1);
        if (newMemberState == getMemberState())
            return;
        MemberHeartbeatData& hbData = _hbdata[_selfIndex];
        switch(newMemberState.s) {
        case MemberState::RS_PRIMARY:
            _role = Role::candidate;
            processWinElection(
                    hbData.getLastHeartbeat(),
                    OID(),
                    OpTime(0, 0),
                    electionTime);
            invariant(_role == Role::leader);
            break;
        case MemberState::RS_SECONDARY:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_RECOVERING:
        case MemberState::RS_STARTUP2:
            _role = Role::follower;
            _followerMode = newMemberState.s;
            if (_currentPrimaryIndex == _selfIndex) {
                _currentPrimaryIndex = -1;
            }
            break;
        case MemberState::RS_STARTUP:
            updateConfig(
                    ReplicaSetConfig(),
                    -1,
                    hbData.getLastHeartbeat(),
                    OpTime(0, 0));
            break;
        default:
            severe() << "Cannot switch to state " << newMemberState;
            invariant(false);
        }
        if (getMemberState() != newMemberState.s) {
            severe() << "Expected to enter state " << newMemberState << " but am now in " <<
                getMemberState();
            invariant(false);
        }
        log() << "replSet " << newMemberState;
    }

    void TopologyCoordinatorImpl::_setCurrentPrimaryForTest(int primaryIndex) {
        if (primaryIndex == _selfIndex) {
            changeMemberState_forTest(MemberState::RS_PRIMARY);
        }
        else {
            if (_iAmPrimary()) {
                changeMemberState_forTest(MemberState::RS_SECONDARY);
            }
            if (primaryIndex != -1) {
                _hbdata[primaryIndex].setUpValues( 
                        _hbdata[primaryIndex].getLastHeartbeat(),
                        MemberState::RS_PRIMARY,
                        OpTime(0, 0),
                        _hbdata[primaryIndex].getOpTime(),
                        "",
                        "");
            }
            _currentPrimaryIndex = primaryIndex;
        }
    }

    const MemberConfig* TopologyCoordinatorImpl::_currentPrimaryMember() const {
        if (_currentPrimaryIndex == -1)
            return NULL;

        return &(_currentConfig.getMemberAt(_currentPrimaryIndex));
    }

    void TopologyCoordinatorImpl::prepareStatusResponse(
            const ReplicationExecutor::CallbackData& data,
            Date_t now,
            unsigned selfUptime,
            const OpTime& lastOpApplied,
            BSONObjBuilder* response,
            Status* result) {
        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        // output for each member
        vector<BSONObj> membersOut;
        const MemberState myState = getMemberState();

        for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); 
             it != _hbdata.end(); 
             ++it) {
            if (it->getConfigIndex() == _selfIndex) {
                // add self
                BSONObjBuilder bb;
                bb.append("_id", _selfConfig().getId());
                bb.append("name", _selfConfig().getHostAndPort().toString());
                bb.append("health", 1.0);
                bb.append("state", static_cast<int>(it->getState().s));
                bb.append("stateStr", it->getState().toString());
                bb.append("uptime", selfUptime);
                if (!_selfConfig().isArbiter()) {
                    bb.append("optime", lastOpApplied);
                    bb.appendDate("optimeDate", lastOpApplied.asDate());
                }

                if (_maintenanceModeCalls) {
                    bb.append("maintenanceMode", _maintenanceModeCalls);
                }

                std::string s = _getHbmsg();
                if( !s.empty() )
                    bb.append("infoMessage", s);

                if (it->getState().primary()) {
                    bb.append("electionTime", it->getElectionTime());
                    bb.appendDate("electionDate", it->getElectionTime().asDate());
                }
                bb.append("self", true);
                membersOut.push_back(bb.obj());
            }
            else {
                // add non-self member
                BSONObjBuilder bb;
                bb.append("_id", _currentConfig.getMemberAt(it->getConfigIndex()).getId());
                bb.append("name", _currentConfig.getMemberAt(it->getConfigIndex())
                          .getHostAndPort().toString());
                double h = it->getHealth();
                bb.append("health", h);
                MemberState state = it->getState();
                bb.append("state", static_cast<int>(state.s));
                if( h == 0 ) {
                    // if we can't connect the state info is from the past
                    // and could be confusing to show
                    bb.append("stateStr", "(not reachable/healthy)");
                }
                else {
                    bb.append("stateStr", it->getState().toString());
                }

                if (state != MemberState::RS_UNKNOWN) {
                    // If state is UNKNOWN we haven't received any heartbeats and thus don't have
                    // meaningful values for these fields

                    unsigned int uptime = static_cast<unsigned int> ((it->getUpSince() ?
                            (now - it->getUpSince()) / 1000 /* convert millis to secs */ : 0));
                    bb.append("uptime", uptime);
                    if (!_currentConfig.getMemberAt(it->getConfigIndex()).isArbiter()) {
                        bb.append("optime", it->getOpTime());
                        bb.appendDate("optimeDate", it->getOpTime().asDate());
                    }
                    bb.appendDate("lastHeartbeat", it->getLastHeartbeat());
                    bb.appendDate("lastHeartbeatRecv", it->getLastHeartbeatRecv());
                    bb.append("pingMs",
                              _getPing(_currentConfig.getMemberAt(
                                      it->getConfigIndex()).getHostAndPort()));
                    std::string s = it->getLastHeartbeatMsg();
                    if( !s.empty() )
                        bb.append("lastHeartbeatMessage", s);

                    if (it->hasAuthIssue()) {
                        bb.append("authenticated", false);
                    }

                    std::string syncSource = it->getSyncSource();
                    if (!syncSource.empty()) {
                        bb.append("syncingTo", syncSource);
                    }

                    if (state == MemberState::RS_PRIMARY) {
                        bb.append("electionTime", it->getElectionTime());
                        bb.appendDate("electionDate", it->getElectionTime().asDate());
                    }
                }
                membersOut.push_back(bb.obj());
            }
        }

        // sort members bson
        sort(membersOut.begin(), membersOut.end());

        response->append("set", _currentConfig.getReplSetName());
        response->append("date", now);
        response->append("myState", myState.s);

        // Add sync source info
        if (!_syncSource.empty() && !myState.primary() && !myState.removed()) {
            response->append("syncingTo", _syncSource.toString());
        }

        response->append("members", membersOut);
        /* TODO: decide where this lands
        if( replSetBlind )
            result.append("blind",true); // to avoid confusion if set...
                                         // normally never set except for testing.
        */
        *result = Status::OK();
    }

    void TopologyCoordinatorImpl::prepareFreezeResponse(
            const ReplicationExecutor::CallbackData& data,
            Date_t now,
            int secs,
            BSONObjBuilder* response,
            Status* result) {
        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        if (secs == 0) {
            _stepDownUntil = now;
            log() << "replSet info 'unfreezing'";
            response->append("info", "unfreezing");
        }
        else {
            if ( secs == 1 )
                response->append("warning", "you really want to freeze for only 1 second?");

            if (!_iAmPrimary()) {
                setStepDownTime(now + (secs * 1000));
                log() << "replSet info 'freezing' for " << secs << " seconds";
            }
            else {
                log() << "replSet info received freeze command but we are primary";
            }
        }
        *result = Status::OK();
    }

    void TopologyCoordinatorImpl::setStepDownTime(Date_t newTime) {
        invariant(newTime > _stepDownUntil);
        _stepDownUntil = newTime;
    }

    int TopologyCoordinatorImpl::getCurrentPrimaryIndex() const {
        return _currentPrimaryIndex;
    }

    Date_t TopologyCoordinatorImpl::getStepDownTime() const {
        return _stepDownUntil;
    }

    // This function installs a new config object and recreates MemberHeartbeatData objects
    // that reflect the new config.
    void TopologyCoordinatorImpl::updateConfig(const ReplicaSetConfig& newConfig,
                                               int selfIndex,
                                               Date_t now,
                                               const OpTime& lastOpApplied) {
        invariant(_role != Role::candidate);
        invariant(selfIndex < newConfig.getNumMembers());

        _currentConfig = newConfig;

        _hbdata.clear();
        _role = Role::follower;
        _currentPrimaryIndex = -1;
        _forceSyncSourceIndex = -1;
        _selfIndex = selfIndex;

        int index = 0;
        for (ReplicaSetConfig::MemberIterator it = _currentConfig.membersBegin();
             it != _currentConfig.membersEnd();
             ++it, ++index) {
            // C++11: use emplace_back()
            if (index == selfIndex) {
                // special case for ourself since we need to be "up" (health > 0) for vote counting
                MemberHeartbeatData self = MemberHeartbeatData(index);
                self.setUpValues(
                        now,
                        getMemberState(),
                        OpTime(0, 0),
                        lastOpApplied,
                        "",
                        "");
                _hbdata.push_back(self);
            }
            else {
                _hbdata.push_back(MemberHeartbeatData(index));
            }
        }

        if (_currentConfig.getNumMembers() == 1 &&
            _selfIndex == 0 &&
            _currentConfig.getMemberAt(_selfIndex).isElectable()) {

            // If the new config describes a one-node replica set, we're the one member, and
            // we're electable, we must transition to candidate, in leiu of heartbeats.
            _role = Role::candidate;
        }

    }

    // TODO(emilkie): Better story for heartbeat message handling.
    void TopologyCoordinatorImpl::_sethbmsg(const std::string& s, int logLevel) {
        static time_t lastLogged;
        _hbmsgTime = time(0);

        if (s == _hbmsg) {
            // unchanged
            if (_hbmsgTime - lastLogged < 60)
                return;
        }

        unsigned sz = s.size();
        if (sz >= 256)
            memcpy(_hbmsg, s.c_str(), 255);
        else {
            _hbmsg[sz] = 0;
            memcpy(_hbmsg, s.c_str(), sz);
        }
        if (!s.empty()) {
            lastLogged = _hbmsgTime;
            LOG(logLevel) << "replSet " << s;
        }
    }

    const MemberConfig& TopologyCoordinatorImpl::_selfConfig() const {
        return _currentConfig.getMemberAt(_selfIndex);
    }

    TopologyCoordinatorImpl::UnelectableReason TopologyCoordinatorImpl::_getUnelectableReason(
                                                                                int index) const {
        const MemberConfig& memberConfig = _currentConfig.getMemberAt(index);
        const MemberHeartbeatData& hbData = _hbdata[index];
        if (memberConfig.isArbiter()) {
            return ArbiterIAm;
        }
        else if (memberConfig.getPriority() <= 0) {
            return NoPriority;
        }
        else if (hbData.getState() != MemberState::RS_SECONDARY) {
            return NotSecondary;
        }
        else if (!_isOpTimeCloseEnoughToLatestToElect(hbData.getOpTime())) {
            return NotCloseEnoughToLatestOptime;
        }
        else {
            invariant(memberConfig.isElectable());
            return None;
        }
    }

    TopologyCoordinatorImpl::UnelectableReason TopologyCoordinatorImpl::_getMyUnelectableReason(
                                                                const Date_t now,
                                                                const OpTime lastApplied) const {
        if (!_aMajoritySeemsToBeUp()) {
            return CannotSeeMajority;
        }
        else if (_selfConfig().isArbiter()) {
            return ArbiterIAm;
        }
        else if (_selfConfig().getPriority() <= 0) {
            return NoPriority;
        }
        else if (_stepDownUntil > now) {
            return StepDownPeriodActive;
        }
        else if (!getMemberState().secondary()) {
            return NotSecondary;
        }
        else if (_isOpTimeCloseEnoughToLatestToElect(lastApplied)) {
            return NotCloseEnoughToLatestOptime;
        }
        else {
            return None;
        }
    }

    std::string TopologyCoordinatorImpl::_getUnelectableReasonString(UnelectableReason ur) const {
        switch (ur) {
            case CannotSeeMajority: return "I cannot see a majority";
            case ArbiterIAm: return "member is an arbiter";
            case NoPriority: return "member has zero priority";
            case StepDownPeriodActive:
                return str::stream() << "I am still waiting for stepdown period to end at "
                                     << _stepDownUntil;
            case NotSecondary: return "member is not currently a secondary";
            case NotCloseEnoughToLatestOptime:
                return "member is more than 10 seconds behind the most up-to-date member";
            default:
                return "The MEMBER is electable! -- This should never be used! --";
        }
    }

    int TopologyCoordinatorImpl::_getPing(const HostAndPort& host) {
        return _pings[host].getMillis();
    }

    void TopologyCoordinatorImpl::_setElectionTime(const OpTime& newElectionTime) {
        _electionTime = newElectionTime;
    }

    int TopologyCoordinatorImpl::_getTotalPings() {
        PingMap::iterator it = _pings.begin();
        PingMap::iterator end = _pings.end();
        int totalPings = 0;
        while (it != end) {
            totalPings += it->second.getCount();
            it++;
        }
        return totalPings;
    }

    std::vector<HostAndPort> TopologyCoordinatorImpl::getMaybeUpHostAndPorts() const {
        std::vector<HostAndPort> upHosts;
        for (std::vector<MemberHeartbeatData>::const_iterator it = _hbdata.begin(); 
             it != _hbdata.end(); 
             ++it) {
            if (it->getConfigIndex() == _selfIndex) {
                continue;    // skip ourselves
            }
            if (!it->maybeUp()) {
                continue;    // skip DOWN nodes
            }

            upHosts.push_back(_currentConfig.getMemberAt(it->getConfigIndex()).getHostAndPort());
        }
        return upHosts;
    }

    bool TopologyCoordinatorImpl::voteForMyself(Date_t now) {
        // TODO(schwerin): We should refuse to vote for ourself if we do not think we're a
        // candidate, but repl_coordinator_impl_elect_test depends on being able to run an election
        // while not a candidate.  When that is fixed, we should reenable the following short
        // circuit condition:
        // if (_role != Role::candidate) {
        //     return false;
        // }
        int selfId = _currentConfig.getMemberAt(_selfIndex).getId();
        if ((_lastVote.when + LastVote::leaseTime.total_milliseconds() >= now) 
            && (_lastVote.whoId != selfId)) {
            log() << "replSet not voting yea for " << selfId <<
                " voted for " << _lastVote.whoHostAndPort.toString() << ' ' << 
                (now - _lastVote.when) / 1000 << " secs ago";
            return false;
        }
        _lastVote.when = now;
        _lastVote.whoId = selfId;
        return true;
    }

    MemberState TopologyCoordinatorImpl::getMemberState() const {
        if (_selfIndex == -1) {
            if (_currentConfig.isInitialized()) {
                return MemberState::RS_REMOVED;
            }
            return MemberState::RS_STARTUP;
        }
        if (_role == Role::leader) {
            return MemberState::RS_PRIMARY;
        }
        const MemberConfig& myConfig = _selfConfig();
        if (myConfig.isArbiter()) {
            return MemberState::RS_ARBITER;
        }
        if ((_maintenanceModeCalls > 0) && (_followerMode == MemberState::RS_SECONDARY)) {
            return MemberState::RS_RECOVERING;
        }
        return _followerMode;
    }

    void TopologyCoordinatorImpl::processWinElection(
            Date_t now,
            OID electionId,
            OpTime myLastOpApplied,
            OpTime electionOpTime) {
        invariant(_role == Role::candidate);
        _electionTime = electionOpTime;
        _electionId = electionId;
        _role = Role::leader;
        _currentPrimaryIndex = _selfIndex;
        _hbdata[_selfIndex].setUpValues(
                now,                      // now
                MemberState::RS_PRIMARY,  // state
                electionOpTime,           // electionTime
                myLastOpApplied,          // optime
                "",                       // syncingTo
                "");                      // heartbeatMessage
    }

    void TopologyCoordinatorImpl::processLoseElection(Date_t now, OpTime myLastOpApplied) {
        invariant(_role == Role::candidate);
        const HostAndPort syncSourceAddress = getSyncSourceAddress();
        _electionTime = OpTime(0, 0);
        _electionId = OID();
        _role = Role::follower;
        _hbdata[_selfIndex].setUpValues(
                now,
                getMemberState(),
                OpTime(0, 0),
                myLastOpApplied,
                syncSourceAddress.empty() ? "" : syncSourceAddress.toString(),
                "");
    }

    void TopologyCoordinatorImpl::stepDown() {
        _stepDownSelf();
    }

    void TopologyCoordinatorImpl::setFollowerMode(MemberState::MS newMode) {
        // TODO(emilkie): Uncomment once legacy StateBox is replaced with replcoord's MemberState.
        //invariant(_role == Role::follower);
        switch (newMode) {
        case MemberState::RS_RECOVERING:
        case MemberState::RS_ROLLBACK:
        case MemberState::RS_SECONDARY:
        case MemberState::RS_STARTUP2:
            _followerMode = newMode;
            break;
        default:
            invariant(false);
        }
    }

    HeartbeatResponseAction TopologyCoordinatorImpl::_stepDownSelf() {
        return _stepDownSelfAndReplaceWith(-1);
    }

    HeartbeatResponseAction TopologyCoordinatorImpl::_stepDownSelfAndReplaceWith(int newPrimary) {
        invariant(_role == Role::leader);
        invariant(_selfIndex != -1);
        invariant(_selfIndex != newPrimary);
        invariant(_selfIndex == _currentPrimaryIndex);
        _currentPrimaryIndex = newPrimary;
        _role = Role::follower;
        _hbdata[_selfIndex].setState(getMemberState());
        return HeartbeatResponseAction::makeStepDownSelfAction(_selfIndex);
    }

    void TopologyCoordinatorImpl::adjustMaintenanceCountBy(int inc) {
        invariant(_role == Role::follower);
        _maintenanceModeCalls += inc;
        invariant(_maintenanceModeCalls >= 0);
    }

    int TopologyCoordinatorImpl::getMaintenanceCount() const {
        return _maintenanceModeCalls;
    }

} // namespace repl
} // namespace mongo
