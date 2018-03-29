/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

// How many times to try refreshing the routing info if the set of chunks loaded from the config
// server is found to be inconsistent.
const int kMaxInconsistentRoutingInfoRefreshAttempts = 3;

/**
 * Given an (optional) initial routing table and a set of changed chunks returned by the catalog
 * cache loader, produces a new routing table with the changes applied.
 *
 * If the collection is no longer sharded returns nullptr. If the epoch has changed, expects that
 * the 'collectionChunksList' contains the full contents of the chunks collection for that namespace
 * so that the routing table can be built from scratch.
 *
 * Throws ConflictingOperationInProgress if the chunk metadata was found to be inconsistent (not
 * containing all the necessary chunks, contains overlaps or chunks' epoch values are not the same
 * as that of the collection). Since this situation may be transient, due to the collection being
 * dropped or recreated concurrently, the caller must retry the reload up to some configurable
 * number of attempts.
 */
std::shared_ptr<RoutingTableHistory> refreshCollectionRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<RoutingTableHistory> existingRoutingInfo,
    StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollectionAndChangedChunks) {
    if (swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound) {
        return nullptr;
    }

    const auto collectionAndChunks = uassertStatusOK(std::move(swCollectionAndChangedChunks));

    auto chunkManager = [&] {
        // If we have routing info already and it's for the same collection epoch, we're updating.
        // Otherwise, we're making a whole new routing table.
        if (existingRoutingInfo &&
            existingRoutingInfo->getVersion().epoch() == collectionAndChunks.epoch) {

            return existingRoutingInfo->makeUpdated(collectionAndChunks.changedChunks);
        }
        auto defaultCollator = [&]() -> std::unique_ptr<CollatorInterface> {
            if (!collectionAndChunks.defaultCollation.isEmpty()) {
                // The collation should have been validated upon collection creation
                return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                           ->makeFromBSON(collectionAndChunks.defaultCollation));
            }
            return nullptr;
        }();
        return RoutingTableHistory::makeNew(nss,
                                            collectionAndChunks.uuid,
                                            KeyPattern(collectionAndChunks.shardKeyPattern),
                                            std::move(defaultCollator),
                                            collectionAndChunks.shardKeyIsUnique,
                                            collectionAndChunks.epoch,
                                            collectionAndChunks.changedChunks);
    }();

    std::set<ShardId> shardIds;
    chunkManager->getAllShardIds(&shardIds);
    for (const auto& shardId : shardIds) {
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    }
    return chunkManager;
}

}  // namespace

CatalogCache::CatalogCache(CatalogCacheLoader& cacheLoader) : _cacheLoader(cacheLoader) {}

CatalogCache::~CatalogCache() = default;

StatusWith<CachedDatabaseInfo> CatalogCache::getDatabase(OperationContext* opCtx,
                                                         StringData dbName) {
    try {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        auto itDbEntry = _databases.find(dbName);
        if (itDbEntry != _databases.end()) {
            const auto& dbEntry = itDbEntry->second;
            auto primaryShard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->dbt.getPrimary()));
            return {CachedDatabaseInfo(dbEntry->dbt, std::move(primaryShard))};
        }

        const auto catalogClient = Grid::get(opCtx)->catalogClient();

        const auto dbNameCopy = dbName.toString();

        // Load the database entry
        const auto opTimeWithDb = uassertStatusOK(catalogClient->getDatabase(
            opCtx, dbNameCopy, repl::ReadConcernLevel::kMajorityReadConcern));
        const auto& dbDesc = opTimeWithDb.value;
        const auto& dbEntry = (_databases[dbName] = std::make_shared<DatabaseInfoEntry>(
                                   DatabaseInfoEntry{std::move(dbDesc)}));

        // Load the sharded collections entries
        // TODO (SERVER-34061): Stop loading sharded collections when loading a database entry.
        repl::OpTime collLoadConfigOptime;
        const std::vector<CollectionType> collections = uassertStatusOK(
            catalogClient->getCollections(opCtx, &dbNameCopy, &collLoadConfigOptime));

        CollectionInfoMap collectionEntries;
        for (const auto& coll : collections) {
            if (coll.getDropped()) {
                continue;
            }
            collectionEntries[coll.getNs().ns()] = std::make_shared<CollectionRoutingInfoEntry>();
        }
        _collectionsByDb[dbName] = std::move(collectionEntries);

        auto primaryShard = uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbEntry->dbt.getPrimary()));
        return {CachedDatabaseInfo(dbEntry->dbt, std::move(primaryShard))};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) {
    return _getCollectionRoutingInfoAt(opCtx, nss, boost::none);
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, Timestamp atClusterTime) {
    return _getCollectionRoutingInfoAt(opCtx, nss, atClusterTime);
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::_getCollectionRoutingInfoAt(
    OperationContext* opCtx, const NamespaceString& nss, boost::optional<Timestamp> atClusterTime) {
    while (true) {
        const auto swDbInfo = getDatabase(opCtx, nss.db());
        if (!swDbInfo.isOK()) {
            return swDbInfo.getStatus();
        }
        const auto dbInfo = std::move(swDbInfo.getValue());

        stdx::unique_lock<stdx::mutex> ul(_mutex);

        const auto itDb = _collectionsByDb.find(nss.db());
        if (itDb == _collectionsByDb.end()) {
            return {CachedCollectionRoutingInfo(nss, dbInfo, nullptr)};
        }
        const auto itColl = itDb->second.find(nss.ns());
        if (itColl == itDb->second.end()) {
            return {CachedCollectionRoutingInfo(nss, dbInfo, nullptr)};
        }
        auto& collEntry = itColl->second;

        if (collEntry->needsRefresh) {
            auto refreshNotification = collEntry->refreshCompletionNotification;
            if (!refreshNotification) {
                refreshNotification = (collEntry->refreshCompletionNotification =
                                           std::make_shared<Notification<Status>>());
                _scheduleCollectionRefresh(ul, collEntry, nss, 1);
            }

            // Wait on the notification outside of the mutex
            ul.unlock();

            auto refreshStatus = [&]() {
                Timer t;
                ON_BLOCK_EXIT([&] { _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros()); });

                try {
                    const Milliseconds kReportingInterval{250};
                    while (!refreshNotification->waitFor(opCtx, kReportingInterval)) {
                        _stats.totalRefreshWaitTimeMicros.addAndFetch(t.micros());
                        t.reset();
                    }

                    return refreshNotification->get(opCtx);
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }
            }();

            if (!refreshStatus.isOK()) {
                return refreshStatus;
            }

            // Once the refresh is complete, loop around to get the latest value
            continue;
        }

        auto cm = std::make_shared<ChunkManager>(collEntry->routingInfo, atClusterTime);

        return {CachedCollectionRoutingInfo(nss, dbInfo, std::move(cm))};
    }
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    invalidateShardedCollection(nss);
    return getCollectionRoutingInfo(opCtx, nss);
}

StatusWith<CachedCollectionRoutingInfo> CatalogCache::getShardedCollectionRoutingInfoWithRefresh(
    OperationContext* opCtx, const NamespaceString& nss) {
    invalidateShardedCollection(nss);

    auto routingInfoStatus = getCollectionRoutingInfo(opCtx, nss);
    if (routingInfoStatus.isOK() && !routingInfoStatus.getValue().cm()) {
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection " << nss.ns() << " is not sharded."};
    }

    return routingInfoStatus;
}

void CatalogCache::onStaleConfigError(CachedCollectionRoutingInfo&& ccriToInvalidate) {
    _stats.countStaleConfigErrors.addAndFetch(1);

    // Ensure the move constructor of CachedCollectionRoutingInfo is invoked in order to clear the
    // input argument so it can't be used anymore
    auto ccri(ccriToInvalidate);

    if (!ccri._cm) {
        // We received StaleShardVersion for a collection we thought was unsharded. The collection
        // must have become sharded.
        invalidateShardedCollection(ccri._nss);
        return;
    }

    // We received StaleShardVersion for a collection we thought was sharded. Either a migration
    // occurred to or from a shard we contacted, or the collection was dropped.
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    const auto nss = ccri._cm->getns();
    const auto itDb = _collectionsByDb.find(nss.db());
    if (itDb == _collectionsByDb.end()) {
        // The database was dropped.
        return;
    }

    auto itColl = itDb->second.find(nss.ns());
    if (itColl == itDb->second.end()) {
        // The collection was dropped.
    } else if (itColl->second->needsRefresh) {
        // Refresh has been scheduled for the collection already
        return;
    } else if (itColl->second->routingInfo->getVersion() == ccri._cm->getVersion()) {
        // If the versions match, the last version of the routing information that we used is no
        // longer valid, so trigger a refresh.
        itColl->second->needsRefresh = true;
    }
}

void CatalogCache::invalidateShardedCollection(const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto itDb = _collectionsByDb.find(nss.db());
    if (itDb == _collectionsByDb.end()) {
        return;
    }

    if (itDb->second.find(nss.ns()) == itDb->second.end()) {
        itDb->second[nss.ns()] = std::make_shared<CollectionRoutingInfoEntry>();
    }
    itDb->second[nss.ns()]->needsRefresh = true;
}

void CatalogCache::purgeDatabase(StringData dbName) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _databases.erase(dbName);
    _collectionsByDb.erase(dbName);
}

void CatalogCache::purgeAllDatabases() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _databases.clear();
    _collectionsByDb.clear();
}

void CatalogCache::report(BSONObjBuilder* builder) const {
    BSONObjBuilder cacheStatsBuilder(builder->subobjStart("catalogCache"));

    size_t numDatabaseEntries;
    size_t numCollectionEntries{0};
    {
        stdx::lock_guard<stdx::mutex> ul(_mutex);
        numDatabaseEntries = _databases.size();
        for (const auto& entry : _collectionsByDb) {
            numCollectionEntries += entry.second.size();
        }
    }

    cacheStatsBuilder.append("numDatabaseEntries", static_cast<long long>(numDatabaseEntries));
    cacheStatsBuilder.append("numCollectionEntries", static_cast<long long>(numCollectionEntries));

    _stats.report(&cacheStatsBuilder);
}

void CatalogCache::_scheduleCollectionRefresh(WithLock lk,
                                              std::shared_ptr<CollectionRoutingInfoEntry> collEntry,
                                              NamespaceString const& nss,
                                              int refreshAttempt) {
    const auto existingRoutingInfo = std::move(collEntry->routingInfo);

    // If we have an existing chunk manager, the refresh is considered "incremental", regardless of
    // how many chunks are in the differential
    const bool isIncremental(existingRoutingInfo);

    if (isIncremental) {
        _stats.numActiveIncrementalRefreshes.addAndFetch(1);
        _stats.countIncrementalRefreshesStarted.addAndFetch(1);
    } else {
        _stats.numActiveFullRefreshes.addAndFetch(1);
        _stats.countFullRefreshesStarted.addAndFetch(1);
    }

    // Invoked when one iteration of getChunksSince has completed, whether with success or error
    const auto onRefreshCompleted = [ this, t = Timer(), nss, isIncremental ](
        const Status& status, RoutingTableHistory* routingInfoAfterRefresh) {
        if (isIncremental) {
            _stats.numActiveIncrementalRefreshes.subtractAndFetch(1);
        } else {
            _stats.numActiveFullRefreshes.subtractAndFetch(1);
        }

        if (!status.isOK()) {
            _stats.countFailedRefreshes.addAndFetch(1);

            log() << "Refresh for collection " << nss << " took " << t.millis() << " ms and failed"
                  << causedBy(redact(status));
        } else if (routingInfoAfterRefresh) {
            log() << "Refresh for collection " << nss << " took " << t.millis()
                  << " ms and found version " << routingInfoAfterRefresh->getVersion();
        } else {
            log() << "Refresh for collection " << nss << " took " << t.millis()
                  << " ms and found the collection is not sharded";
        }
    };

    // Invoked if getChunksSince resulted in error
    const auto onRefreshFailed = [ this, collEntry, nss, refreshAttempt, onRefreshCompleted ](
        WithLock lk, const Status& status) noexcept {
        onRefreshCompleted(status, nullptr);

        // It is possible that the metadata is being changed concurrently, so retry the
        // refresh again
        if (status == ErrorCodes::ConflictingOperationInProgress &&
            refreshAttempt < kMaxInconsistentRoutingInfoRefreshAttempts) {
            _scheduleCollectionRefresh(lk, collEntry, nss, refreshAttempt + 1);
        } else {
            // Leave needsRefresh to true so that any subsequent get attempts will kick off
            // another round of refresh
            collEntry->refreshCompletionNotification->set(status);
            collEntry->refreshCompletionNotification = nullptr;
        }
    };

    const auto refreshCallback =
        [ this, collEntry, nss, existingRoutingInfo, onRefreshFailed, onRefreshCompleted ](
            OperationContext * opCtx,
            StatusWith<CatalogCacheLoader::CollectionAndChangedChunks> swCollAndChunks) noexcept {
        std::shared_ptr<RoutingTableHistory> newRoutingInfo;
        try {
            newRoutingInfo = refreshCollectionRoutingInfo(
                opCtx, nss, std::move(existingRoutingInfo), std::move(swCollAndChunks));

            onRefreshCompleted(Status::OK(), newRoutingInfo.get());
        } catch (const DBException& ex) {
            stdx::lock_guard<stdx::mutex> lg(_mutex);
            onRefreshFailed(lg, ex.toStatus());
            return;
        }

        stdx::lock_guard<stdx::mutex> lg(_mutex);

        collEntry->needsRefresh = false;
        collEntry->refreshCompletionNotification->set(Status::OK());
        collEntry->refreshCompletionNotification = nullptr;

        if (!newRoutingInfo) {
            // The refresh found that collection was dropped, so remove it from our cache.
            auto itDb = _collectionsByDb.find(nss.db());
            if (itDb == _collectionsByDb.end()) {
                // The entire database was dropped.
                return;
            }
            itDb->second.erase(nss.ns());
            return;
        }
        collEntry->routingInfo = std::move(newRoutingInfo);
    };

    const ChunkVersion startingCollectionVersion =
        (existingRoutingInfo ? existingRoutingInfo->getVersion() : ChunkVersion::UNSHARDED());

    log() << "Refreshing chunks for collection " << nss << " based on version "
          << startingCollectionVersion;

    try {
        _cacheLoader.getChunksSince(nss, startingCollectionVersion, refreshCallback);
    } catch (const DBException& ex) {
        const auto status = ex.toStatus();

        // ConflictingOperationInProgress errors trigger retry of the catalog cache reload logic. If
        // we failed to schedule the asynchronous reload, there is no point in doing another
        // attempt.
        invariant(status != ErrorCodes::ConflictingOperationInProgress);

        stdx::lock_guard<stdx::mutex> lg(_mutex);
        onRefreshFailed(lg, status);
    }
}

void CatalogCache::Stats::report(BSONObjBuilder* builder) const {
    builder->append("countStaleConfigErrors", countStaleConfigErrors.load());

    builder->append("totalRefreshWaitTimeMicros", totalRefreshWaitTimeMicros.load());

    builder->append("numActiveIncrementalRefreshes", numActiveIncrementalRefreshes.load());
    builder->append("countIncrementalRefreshesStarted", countIncrementalRefreshesStarted.load());

    builder->append("numActiveFullRefreshes", numActiveFullRefreshes.load());
    builder->append("countFullRefreshesStarted", countFullRefreshesStarted.load());

    builder->append("countFailedRefreshes", countFailedRefreshes.load());
}

CachedDatabaseInfo::CachedDatabaseInfo(DatabaseType dbt, std::shared_ptr<Shard> primaryShard)
    : _dbt(std::move(dbt)), _primaryShard(std::move(primaryShard)) {}

const ShardId& CachedDatabaseInfo::primaryId() const {
    return _dbt.getPrimary();
}

bool CachedDatabaseInfo::shardingEnabled() const {
    return _dbt.getSharded();
}

boost::optional<DatabaseVersion> CachedDatabaseInfo::databaseVersion() const {
    return _dbt.getVersion();
}

CachedCollectionRoutingInfo::CachedCollectionRoutingInfo(NamespaceString nss,
                                                         CachedDatabaseInfo db,
                                                         std::shared_ptr<ChunkManager> cm)
    : _nss(std::move(nss)), _db(std::move(db)), _cm(std::move(cm)) {}

}  // namespace mongo