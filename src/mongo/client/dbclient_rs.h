/** @file dbclient_rs.h Connect to a Replica Set, from C++ */

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/shared_ptr.hpp>
#include <utility>

#include "mongo/client/dbclientinterface.h"
#include "mongo/client/export_macros.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class ReplicaSetMonitor;
class TagSet;
struct ReadPreferenceSetting;
typedef boost::shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;

/** Use this class to connect to a replica set of servers.  The class will manage
   checking for which server in a replica set is master, and do failover automatically.

   This can also be used to connect to replica pairs since pairs are a subset of sets

   On a failover situation, expect at least one operation to return an error (throw
   an exception) before the failover is complete.  Operations are not retried.
*/
class MONGO_CLIENT_API DBClientReplicaSet : public DBClientBase {
public:
    using DBClientBase::query;
    using DBClientBase::update;
    using DBClientBase::remove;

    /** Call connect() after constructing. autoReconnect is always on for DBClientReplicaSet
     * connections. */
    DBClientReplicaSet(const std::string& name,
                       const std::vector<HostAndPort>& servers,
                       double so_timeout = 0);
    virtual ~DBClientReplicaSet();

    /**
     * Returns false if no member of the set were reachable. This object
     * can still be used even when false was returned as it will try to
     * reconnect when you use it later.
     */
    bool connect();

    /**
     * Logs out the connection for the given database.
     *
     * @param dbname the database to logout from.
     * @param info the result object for the logout command (provided for backwards
     *     compatibility with mongo shell)
     */
    virtual void logout(const std::string& dbname, BSONObj& info);

    // ----------- simple functions --------------

    /** throws userassertion "no master found" */
    virtual std::auto_ptr<DBClientCursor> query(const std::string& ns,
                                                Query query,
                                                int nToReturn = 0,
                                                int nToSkip = 0,
                                                const BSONObj* fieldsToReturn = 0,
                                                int queryOptions = 0,
                                                int batchSize = 0);

    /** throws userassertion "no master found" */
    virtual BSONObj findOne(const std::string& ns,
                            const Query& query,
                            const BSONObj* fieldsToReturn = 0,
                            int queryOptions = 0);

    virtual void insert(const std::string& ns,
                        BSONObj obj,
                        int flags = 0,
                        const WriteConcern* wc = NULL);

    virtual void insert(const std::string& ns,
                        const std::vector<BSONObj>& v,
                        int flags = 0,
                        const WriteConcern* wc = NULL);

    virtual void remove(const std::string& ns, Query obj, int flags, const WriteConcern* wc = NULL);

    virtual void update(
        const std::string& ns, Query query, BSONObj obj, int flags, const WriteConcern* wc = NULL);

    virtual void killCursor(long long cursorID);

    // ---- access raw connections ----

    /**
     * WARNING: this method is very dangerous - this object can decide to free the
     *     returned master connection any time.
     *
     * @return the reference to the address that points to the master connection.
     */
    DBClientConnection& masterConn();

    /**
     * WARNING: this method is very dangerous - this object can decide to free the
     *     returned master connection any time. This can also unpin the cached
     *     slaveOk/read preference connection.
     *
     * @return the reference to the address that points to a secondary connection.
     */
    DBClientConnection& slaveConn();

    // ---- callback pieces -------

    virtual void say(Message& toSend, bool isRetry = false, std::string* actualServer = 0);
    virtual bool recv(Message& toRecv);
    virtual void checkResponse(const char* data,
                               int nReturned,
                               bool* retry = NULL,
                               std::string* targetHost = NULL);

    /* this is the callback from our underlying connections to notify us that we got a "not master"
     * error.
     */
    void isntMaster();

    /* this is used to indicate we got a "not master or secondary" error from a secondary.
     */
    void isntSecondary();

    // ----- status ------

    virtual bool isFailed() const {
        return !_master || _master->isFailed();
    }
    bool isStillConnected();

    // ----- informational ----

    double getSoTimeout() const {
        return _so_timeout;
    }

    std::string toString() const {
        return getServerAddress();
    }

    std::string getServerAddress() const;

    virtual ConnectionString::ConnectionType type() const {
        return ConnectionString::SET;
    }
    virtual bool lazySupported() const {
        return true;
    }

    // ---- low level ------

    virtual bool call(Message& toSend,
                      Message& response,
                      bool assertOk = true,
                      std::string* actualServer = 0);
    virtual bool callRead(Message& toSend, Message& response) {
        return checkMaster()->callRead(toSend, response);
    }

    /**
     * Returns whether a query or command can be sent to secondaries based on the query object
     * and options.
     *
     * @param ns the namespace of the query.
     * @param queryObj the query object to check.
     * @param queryOptions the query options
     *
     * @return true if the query/cmd could potentially be sent to a secondary, false otherwise
     */
    static bool MONGO_CLIENT_FUNC
    isSecondaryQuery(const std::string& ns, const BSONObj& queryObj, int queryOptions);

    virtual void setRunCommandHook(DBClientWithCommands::RunCommandHookFunc func);
    virtual void setPostRunCommandHook(DBClientWithCommands::PostRunCommandHookFunc func);

    /**
     * Performs a "soft reset" by clearing all states relating to secondary nodes and
     * returning secondary connections to the pool.
     */
    virtual void reset();

    /**
     * @bool setting if true, DBClientReplicaSet connections will make sure that secondary
     *    connections are authenticated and log them before returning them to the pool.
     */
    static void setAuthPooledSecondaryConn(bool setting);

    virtual int getMaxWireVersion() {
        return checkMaster()->getMaxWireVersion();
    }

    virtual int getMinWireVersion() {
        return checkMaster()->getMinWireVersion();
    }

protected:
    /** Authorize.  Authorizes all nodes as needed
    */
    virtual void _auth(const BSONObj& params);

    virtual void sayPiggyBack(Message& toSend) {
        checkMaster()->say(toSend);
    }

private:
    /**
     * Used to simplify slave-handling logic on errors
     *
     * @return back the passed cursor
     * @throws DBException if the directed node cannot accept the query because it
     *     is not a master
     */
    std::auto_ptr<DBClientCursor> checkSlaveQueryResult(std::auto_ptr<DBClientCursor> result);

    DBClientConnection* checkMaster();

    /**
     * Helper method for selecting a node based on the read preference. Will advance
     * the tag tags object if it cannot find a node that matches the current tag.
     *
     * @param readPref the preference to use for selecting a node.
     *
     * @return a pointer to the new connection object if it can find a good connection.
     *     Otherwise it returns NULL.
     *
     * @throws DBException when an error occurred either when trying to connect to
     *     a node that was thought to be ok or when an assertion happened.
     */
    DBClientConnection* selectNodeUsingTags(boost::shared_ptr<ReadPreferenceSetting> readPref);

    /**
     * @return true if the last host used in the last slaveOk query is still in the
     * set and can be used for the given read preference.
     */
    bool checkLastHost(const ReadPreferenceSetting* readPref);

    /**
     * Destroys all cached information about the last slaveOk operation.
     */
    void invalidateLastSlaveOkCache();

    void _auth(DBClientConnection* conn);

    /**
     * Calls logout on the connection for all known database this DBClientRS instance has
     * logged in.
     */
    void logoutAll(DBClientConnection* conn);

    /**
     * Clears the master connection.
     */
    void resetMaster();

    /**
     * Clears the slaveOk connection and returns it to the pool if not the same as _master.
     */
    void resetSlaveOkConn();

    /**
     * Maximum number of retries to make for auto-retry logic when performing a slave ok
     * operation.
     */
    static const size_t MAX_RETRY;

    // TODO: remove this when processes other than mongos uses the driver version.
    static bool _authPooledSecondaryConn;

    // Throws a DBException if the monitor doesn't exist and there isn't a cached seed to use.
    ReplicaSetMonitorPtr _getMonitor() const;

    std::string _setName;

    HostAndPort _masterHost;
    boost::scoped_ptr<DBClientConnection> _master;

    // Last used host in a slaveOk query (can be a primary).
    HostAndPort _lastSlaveOkHost;
    // Last used connection in a slaveOk query (can be a primary).
    // Connection can either be owned here or returned to the connection pool. Note that
    // if connection is primary, it is owned by _master so it is incorrect to return
    // it to the pool.
    std::auto_ptr<DBClientConnection> _lastSlaveOkConn;
    boost::shared_ptr<ReadPreferenceSetting> _lastReadPref;

    double _so_timeout;

    // we need to store so that when we connect to a new node on failure
    // we can re-auth
    // this could be a security issue, as the password is stored in memory
    // not sure if/how we should handle
    std::map<std::string, BSONObj> _auths;  // dbName -> auth parameters

protected:
    /**
     * for storing (non-threadsafe) information between lazy calls
     */
    class LazyState {
    public:
        LazyState() : _lastClient(NULL), _lastOp(-1), _secondaryQueryOk(false), _retries(0) {}
        DBClientConnection* _lastClient;
        int _lastOp;
        bool _secondaryQueryOk;
        int _retries;

    } _lazyState;
};

/**
 * A simple object for representing the list of tags requested by a $readPreference.
 */
class MONGO_CLIENT_API TagSet {
public:
    /**
     * Creates a TagSet that matches any nodes.
     *
     * Do not call during static init.
     */
    TagSet();

    /**
     * Creates a TagSet from a BSONArray of tags.
     *
     * @param tags the list of tags associated with this option. This object
     *     will get a shared copy of the list. Therefore, it is important
     *     for the the given tag to live longer than the created tag set.
     */
    explicit TagSet(const BSONArray& tags) : _tags(tags) {}

    /**
     * Returns the BSONArray listing all tags that should be accepted.
     */
    const BSONArray& getTagBSON() const {
        return _tags;
    }

    bool operator==(const TagSet& other) const {
        return _tags == other._tags;
    }

private:
    BSONArray _tags;
};

struct MONGO_CLIENT_API ReadPreferenceSetting {
    /**
     * @parm pref the read preference mode.
     * @param tag the tag set. Note that this object will have the
     *     tag set will have this in a reset state (meaning, this
     *     object's copy of tag will have the iterator in the initial
     *     position).
     */
    ReadPreferenceSetting(ReadPreference pref, const TagSet& tag) : pref(pref), tags(tag) {}

    inline bool equals(const ReadPreferenceSetting& other) const {
        return pref == other.pref && tags == other.tags;
    }

    BSONObj toBSON() const;

    const ReadPreference pref;
    TagSet tags;
};
}
