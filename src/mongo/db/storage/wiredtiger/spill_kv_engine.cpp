/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#ifdef _WIN32
#define NVALGRIND
#endif

#include "mongo/db/storage/wiredtiger/spill_kv_engine.h"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <valgrind/valgrind.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/wiredtiger/spill_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {

#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizerEnabled = true;
#else
constexpr bool kAddressSanitizerEnabled = false;
#endif

#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizerEnabled = true;
#else
constexpr bool kThreadSanitizerEnabled = false;
#endif

}  // namespace

SpillKVEngine::SpillKVEngine(const std::string& canonicalName,
                             const std::string& path,
                             ClockSource* clockSource,
                             WiredTigerConfig wtConfig)
    : WiredTigerKVEngineBase(canonicalName, path, clockSource, std::move(wtConfig)) {
    if (!_wtConfig.inMemory) {
        if (!boost::filesystem::exists(path)) {
            try {
                boost::filesystem::create_directories(path);
            } catch (std::exception& e) {
                LOGV2_ERROR(10380302,
                            "Error creating data directory",
                            "directory"_attr = path,
                            "error"_attr = e.what());
                throw;
            }
        }
    }

    std::string config = generateWTOpenConfigString(_wtConfig, true /* ephemeral */);
    LOGV2(10158000, "Opening spill WiredTiger", "config"_attr = config);

    auto startTime = Date_t::now();
    _openWiredTiger(path, config);
    LOGV2(10158001, "Spill WiredTiger opened", "duration"_attr = Date_t::now() - startTime);
    _eventHandler.setStartupSuccessful();
    _wtOpenConfig = config;

    // TODO(SERVER-103355): Disable session caching.
    _connection =
        std::make_unique<WiredTigerConnection>(_conn, clockSource, /*sessionCacheMax=*/33000, this);

    // TODO(SERVER-103209): Add support for configuring the spill WiredTiger instance at runtime.
}

SpillKVEngine::~SpillKVEngine() {
    cleanShutdown();
}

void SpillKVEngine::_openWiredTiger(const std::string& path, const std::string& wtOpenConfig) {
    auto wtEventHandler = _eventHandler.getWtEventHandler();

    int ret = wiredtiger_open(path.c_str(), wtEventHandler, wtOpenConfig.c_str(), &_conn);
    if (ret) {
        LOGV2_FATAL_NOTRACE(10158002,
                            "Failed to open the spill WiredTiger instance",
                            "details"_attr = wtRCToStatus(ret, nullptr).reason());
    }
}

std::unique_ptr<RecordStore> SpillKVEngine::getTemporaryRecordStore(OperationContext* opCtx,
                                                                    StringData ident,
                                                                    KeyFormat keyFormat) {
    SpillRecordStore::Params params;
    params.baseParams.uuid = boost::none;
    params.baseParams.ident = ident.toString();
    params.baseParams.engineName = _canonicalName;
    params.baseParams.keyFormat = keyFormat;
    params.baseParams.overwrite = true;
    // We don't log writes to spill tables.
    params.baseParams.isLogged = false;
    params.baseParams.forceUpdateWithFullDocument = false;
    return std::make_unique<SpillRecordStore>(this, std::move(params));
}

std::unique_ptr<RecordStore> SpillKVEngine::makeTemporaryRecordStore(OperationContext* opCtx,
                                                                     StringData ident,
                                                                     KeyFormat keyFormat) {
    WiredTigerSession session(_connection.get());

    WiredTigerRecordStoreBase::WiredTigerTableConfig wtTableConfig =
        getWiredTigerTableConfigFromStartupOptions(true /* usingSpillKVEngine */);
    wtTableConfig.keyFormat = keyFormat;
    // We don't log writes to spill tables.
    wtTableConfig.logEnabled = false;
    std::string config =
        WiredTigerRecordStoreBase::generateCreateString({} /* internal table */, wtTableConfig);

    std::string uri = WiredTigerUtil::buildTableUri(ident);
    LOGV2_DEBUG(10158008,
                2,
                "WiredTigerKVEngine::makeTemporaryRecordStore",
                "uri"_attr = uri,
                "config"_attr = config);
    uassertStatusOK(wtRCToStatus(session.create(uri.c_str(), config.c_str()), session));

    return getTemporaryRecordStore(opCtx, ident, keyFormat);
}

bool SpillKVEngine::hasIdent(RecoveryUnit& ru, StringData ident) const {
    return _wtHasUri(*SpillRecoveryUnit::get(ru).getSession(),
                     WiredTigerUtil::buildTableUri(ident));
}

std::vector<std::string> SpillKVEngine::getAllIdents(RecoveryUnit& ru) const {
    auto& wtRu = SpillRecoveryUnit::get(ru);
    return _wtGetAllIdents(wtRu);
}

Status SpillKVEngine::dropIdent(RecoveryUnit* ru,
                                StringData ident,
                                bool identHasSizeInfo,
                                const StorageEngine::DropIdentCallback& onDrop) {
    std::string uri = WiredTigerUtil::buildTableUri(ident);

    auto& wtRu = SpillRecoveryUnit::get(*ru);
    wtRu.getSessionNoTxn()->closeAllCursors(uri);

    WiredTigerSession session(_connection.get());

    int ret = session.drop(uri.c_str(), "checkpoint_wait=false");
    Status status = Status::OK();
    if (ret == 0 || ret == ENOENT) {
        // If ident doesn't exist, it is effectively dropped.
    } else {
        status = wtRCToStatus(ret, session);
    }
    LOGV2_DEBUG(10327200, 1, "WT drop", "uri"_attr = uri, "status"_attr = status);
    return status;
}

void SpillKVEngine::cleanShutdown() {
    LOGV2(10158003, "SpillKVEngine shutting down");

    if (!_conn) {
        return;
    }

    _connection->shuttingDown();
    _connection.reset();

    // We want WiredTiger to leak memory for faster shutdown except when we are running tools to
    // look for memory leaks.
    bool leak_memory = !kAddressSanitizerEnabled;
    std::string closeConfig = "";
    if (RUNNING_ON_VALGRIND) {  // NOLINT
        leak_memory = false;
    }
    if (leak_memory) {
        closeConfig = "leak_memory=true,";
    }

    auto startTime = Date_t::now();
    LOGV2(10158006, "Closing spill WiredTiger", "closeConfig"_attr = closeConfig);
    // WT_CONNECTION::close() takes a checkpoint. To ensure this is fast, we delete the tables
    // created by this KVEngine in SpillRecordStore destructor.
    invariantWTOK(_conn->close(_conn, closeConfig.c_str()), nullptr);
    LOGV2(10158007, "Closed spill WiredTiger ", "duration"_attr = Date_t::now() - startTime);
    _conn = nullptr;
}

}  // namespace mongo
