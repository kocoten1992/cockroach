// Copyright 2014 The Cockroach Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied.  See the License for the specific language governing
// permissions and limitations under the License.

#include "db.h"
#include <algorithm>
#include <rocksdb/convenience.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/table.h>
#include <stdarg.h>
#include "batch.h"
#include "cache.h"
#include "comparator.h"
#include "defines.h"
#include "encoding.h"
#include "engine.h"
#include "env_manager.h"
#include "eventlistener.h"
#include "fmt.h"
#include "getter.h"
#include "godefs.h"
#include "iterator.h"
#include "merge.h"
#include "options.h"
#include "snapshot.h"
#include "status.h"

using namespace cockroach;

namespace cockroach {

// DBOpenHook in OSS mode only verifies that no extra options are specified.
__attribute__((weak)) rocksdb::Status DBOpenHook(std::shared_ptr<rocksdb::Logger> info_log,
                                                 const std::string& db_dir, const DBOptions opts,
                                                 EnvManager* env_mgr) {
  if (opts.extra_options.len != 0) {
    return rocksdb::Status::InvalidArgument(
        "DBOptions has extra_options, but OSS code cannot handle them");
  }
  return rocksdb::Status::OK();
}

DBKey ToDBKey(const rocksdb::Slice& s) {
  DBKey key;
  memset(&key, 0, sizeof(key));
  rocksdb::Slice tmp;
  if (DecodeKey(s, &tmp, &key.wall_time, &key.logical)) {
    key.key = ToDBSlice(tmp);
  }
  return key;
}

ScopedStats::ScopedStats(DBIterator* iter)
    : iter_(iter),
      internal_delete_skipped_count_base_(
          rocksdb::get_perf_context()->internal_delete_skipped_count) {
  if (iter_->stats != nullptr) {
    rocksdb::SetPerfLevel(rocksdb::PerfLevel::kEnableTimeExceptForMutex);
  }
}
ScopedStats::~ScopedStats() {
  if (iter_->stats != nullptr) {
    iter_->stats->internal_delete_skipped_count +=
        (rocksdb::get_perf_context()->internal_delete_skipped_count -
         internal_delete_skipped_count_base_);
    rocksdb::SetPerfLevel(rocksdb::PerfLevel::kDisable);
  }
}

void BatchSSTablesForCompaction(const std::vector<rocksdb::SstFileMetaData>& sst,
                                rocksdb::Slice start_key, rocksdb::Slice end_key,
                                uint64_t target_size, std::vector<rocksdb::Range>* ranges) {
  int prev = -1;  // index of the last compacted sst
  uint64_t size = 0;
  for (int i = 0; i < sst.size(); ++i) {
    size += sst[i].size;
    if (size < target_size && (i + 1) < sst.size()) {
      // We haven't reached the target size or the end of the sstables
      // to compact.
      continue;
    }

    rocksdb::Slice start;
    if (prev == -1) {
      // This is the first compaction.
      start = start_key;
    } else {
      // This is a compaction in the middle or end of the requested
      // key range. The start key for the compaction is the largest
      // key from the previous compacted.
      start = rocksdb::Slice(sst[prev].largestkey);
    }

    rocksdb::Slice end;
    if ((i + 1) == sst.size()) {
      // This is the last compaction.
      end = end_key;
    } else {
      // This is a compaction at the start or in the middle of the
      // requested key range. The end key is the largest key in the
      // current sstable.
      end = rocksdb::Slice(sst[i].largestkey);
    }

    ranges->emplace_back(rocksdb::Range(start, end));

    prev = i;
    size = 0;
  }
}

}  // namespace cockroach

namespace {

DBIterState DBIterGetState(DBIterator* iter) {
  DBIterState state = {};
  state.valid = iter->rep->Valid();
  state.status = ToDBStatus(iter->rep->status());

  if (state.valid) {
    rocksdb::Slice key;
    state.valid = DecodeKey(iter->rep->key(), &key, &state.key.wall_time, &state.key.logical);
    if (state.valid) {
      state.key.key = ToDBSlice(key);
      state.value = ToDBSlice(iter->rep->value());
    }
  }

  return state;
}

}  // namespace

DBStatus DBOpen(DBEngine** db, DBSlice dir, DBOptions db_opts) {
  rocksdb::Options options = DBMakeOptions(db_opts);

  const std::string additional_options = ToString(db_opts.rocksdb_options);
  if (!additional_options.empty()) {
    // TODO(peter): Investigate using rocksdb::LoadOptionsFromFile if
    // "additional_options" starts with "@". The challenge is that
    // LoadOptionsFromFile gives us a DBOptions and
    // ColumnFamilyOptions with no ability to supply "base" options
    // and no ability to determine what options were specified in the
    // file which could cause "defaults" to override the options
    // returned by DBMakeOptions. We might need to fix this upstream.
    rocksdb::Status status = rocksdb::GetOptionsFromString(options, additional_options, &options);
    if (!status.ok()) {
      return ToDBStatus(status);
    }
  }

  const std::string db_dir = ToString(dir);

  // Make the default options.env the default. It points to Env::Default which does not
  // need to be deleted.
  std::unique_ptr<cockroach::EnvManager> env_mgr(new cockroach::EnvManager(options.env));

  if (dir.len == 0) {
    // In-memory database: use a MemEnv as the base Env.
    auto memenv = rocksdb::NewMemEnv(rocksdb::Env::Default());
    // Register it for deletion.
    env_mgr->TakeEnvOwnership(memenv);
    // Make it the env that all other Envs must wrap.
    env_mgr->base_env = memenv;
    // Make it the env for rocksdb.
    env_mgr->db_env = memenv;
  }

  // Create the file registry. It uses the base_env to access the registry file.
  auto file_registry =
      std::unique_ptr<FileRegistry>(new FileRegistry(env_mgr->base_env, db_dir, db_opts.read_only));

  if (db_opts.use_file_registry) {
    // We're using the file registry.
    auto status = file_registry->Load();
    if (!status.ok()) {
      return ToDBStatus(status);
    }

    // EnvManager takes ownership of the file registry.
    env_mgr->file_registry.swap(file_registry);
  } else {
    // File registry format not enabled: check whether we have a registry file (we shouldn't).
    // The file_registry is not passed to anyone, it is deleted when it goes out of scope.
    auto status = file_registry->CheckNoRegistryFile();
    if (!status.ok()) {
      return ToDBStatus(status);
    }
  }

  // Call hooks to handle db_opts.extra_options.
  auto hook_status = DBOpenHook(options.info_log, db_dir, db_opts, env_mgr.get());
  if (!hook_status.ok()) {
    return ToDBStatus(hook_status);
  }

  // TODO(mberhault):
  // - check available ciphers somehow?
  //   We may have a encrypted files in the registry file but running without encryption flags.
  // - pass read-only flag though, we should not be modifying file/key registries (including key
  //   rotation) in read-only mode.

  // Register listener for tracking RocksDB stats.
  std::shared_ptr<DBEventListener> event_listener(new DBEventListener);
  options.listeners.emplace_back(event_listener);

  // Point rocksdb to the env to use.
  options.env = env_mgr->db_env;

  rocksdb::DB* db_ptr;

  rocksdb::Status status;
  if (db_opts.read_only) {
    status = rocksdb::DB::OpenForReadOnly(options, db_dir, &db_ptr);
  } else {
    status = rocksdb::DB::Open(options, db_dir, &db_ptr);
  }

  if (!status.ok()) {
    return ToDBStatus(status);
  }
  *db = new DBImpl(db_ptr, std::move(env_mgr),
                   db_opts.cache != nullptr ? db_opts.cache->rep : nullptr, event_listener);
  return kSuccess;
}

DBStatus DBDestroy(DBSlice dir) {
  rocksdb::Options options;
  return ToDBStatus(rocksdb::DestroyDB(ToString(dir), options));
}

DBStatus DBClose(DBEngine* db) {
  DBStatus status = db->AssertPreClose();
  if (status.data == nullptr) {
    delete db;
  }
  return status;
}

DBStatus DBFlush(DBEngine* db) {
  rocksdb::FlushOptions options;
  options.wait = true;
  return ToDBStatus(db->rep->Flush(options));
}

DBStatus DBSyncWAL(DBEngine* db) {
#ifdef _WIN32
  // On Windows, DB::SyncWAL() is not implemented due to fact that
  // `WinWritableFile` is not thread safe. To get around that, the only other
  // methods that can be used to ensure that a sync is triggered is to either
  // flush the memtables or perform a write with `WriteOptions.sync=true`. See
  // https://github.com/facebook/rocksdb/wiki/RocksDB-FAQ for more details.
  // Please also see #17442 for more discussion on the topic.

  // In order to force a sync we issue a write-batch containing
  // LogData with 'sync=true'. The LogData forces a write to the WAL
  // but otherwise doesn't add anything to the memtable or sstables.
  rocksdb::WriteBatch batch;
  batch.PutLogData("");
  rocksdb::WriteOptions options;
  options.sync = true;
  return ToDBStatus(db->rep->Write(options, &batch));
#else
  return ToDBStatus(db->rep->FlushWAL(true /* sync */));
#endif
}

DBStatus DBCompact(DBEngine* db) {
  return DBCompactRange(db, DBSlice(), DBSlice(), true /* force_bottommost */);
}

DBStatus DBCompactRange(DBEngine* db, DBSlice start, DBSlice end, bool force_bottommost) {
  rocksdb::CompactRangeOptions options;
  // By default, RocksDB doesn't recompact the bottom level (unless
  // there is a compaction filter, which we don't use). However,
  // recompacting the bottom layer is necessary to pick up changes to
  // settings like bloom filter configurations, and to fully reclaim
  // space after dropping, truncating, or migrating tables.
  if (force_bottommost) {
    options.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
  }
  // By default, RocksDB treats manual compaction requests as
  // operating exclusively, preventing normal automatic compactions
  // from running. This can block writes to the database, as L0
  // SSTables will become full without being allowed to compact to L1.
  options.exclusive_manual_compaction = false;

  // Compacting the entire database in a single-shot can use a
  // significant amount of additional (temporary) disk space. Instead,
  // we loop over the sstables in the lowest level and initiate
  // compactions on smaller ranges of keys. The resulting compacted
  // database is the same size, but the temporary disk space needed
  // for the compaction is dramatically reduced.
  std::vector<rocksdb::LiveFileMetaData> all_metadata;
  std::vector<rocksdb::LiveFileMetaData> metadata;
  db->rep->GetLiveFilesMetaData(&all_metadata);

  const std::string start_key(ToString(start));
  const std::string end_key(ToString(end));

  int max_level = 0;
  for (int i = 0; i < all_metadata.size(); i++) {
    // Skip any SSTables which fall outside the specified range, if a
    // range was specified.
    if ((!start_key.empty() && all_metadata[i].largestkey < start_key) ||
        (!end_key.empty() && all_metadata[i].smallestkey >= end_key)) {
      continue;
    }
    if (max_level < all_metadata[i].level) {
      max_level = all_metadata[i].level;
    }
    // Gather the set of SSTables to compact.
    metadata.push_back(all_metadata[i]);
  }
  all_metadata.clear();

  if (max_level != db->rep->NumberLevels() - 1) {
    // There are no sstables at the lowest level, so just compact the
    // specified key span, wholesale. Due to the
    // level_compaction_dynamic_level_bytes setting, this will only
    // happen on spans containing very little data.
    const rocksdb::Slice start_slice(start_key);
    const rocksdb::Slice end_slice(end_key);
    return ToDBStatus(db->rep->CompactRange(options, !start_key.empty() ? &start_slice : nullptr,
                                            !end_key.empty() ? &end_slice : nullptr));
  }

  // A naive approach to selecting ranges to compact would be to
  // compact the ranges specified by the smallest and largest key in
  // each sstable of the bottom-most level. Unfortunately, the
  // sstables in the bottom-most level have vastly different
  // sizes. For example, starting with the following set of bottom-most
  // sstables:
  //
  //   100M[16] 89M 70M 66M 56M 54M 38M[2] 36M 23M 20M 17M 8M 6M 5M 2M 2K[4]
  //
  // If we compact the entire database in one call we can end up with:
  //
  //   100M[22] 77M 76M 50M
  //
  // If we use the naive approach (compact the range specified by
  // the smallest and largest keys):
  //
  //   100M[18] 92M 68M 62M 61M 50M 45M 39M 31M 29M[2] 24M 23M 18M 9M 8M[2] 7M
  //   2K[4]
  //
  // With the approach below:
  //
  //   100M[19] 80M 68M[2] 62M 61M 53M 45M 36M 31M
  //
  // The approach below is to loop over the bottom-most sstables in
  // sorted order and initiate a compact range every 128MB of data.

  // Gather up the bottom-most sstable metadata.
  std::vector<rocksdb::SstFileMetaData> sst;
  for (int i = 0; i < metadata.size(); i++) {
    if (metadata[i].level != max_level) {
      continue;
    }
    sst.push_back(metadata[i]);
  }
  // Sort the metadata by smallest key.
  std::sort(sst.begin(), sst.end(),
            [](const rocksdb::SstFileMetaData& a, const rocksdb::SstFileMetaData& b) -> bool {
              return a.smallestkey < b.smallestkey;
            });

  // Batch the bottom-most sstables into compactions of ~128MB.
  const uint64_t target_size = 128 << 20;
  std::vector<rocksdb::Range> ranges;
  BatchSSTablesForCompaction(sst, start_key, end_key, target_size, &ranges);

  for (auto r : ranges) {
    rocksdb::Status status = db->rep->CompactRange(options, r.start.empty() ? nullptr : &r.start,
                                                   r.limit.empty() ? nullptr : &r.limit);
    if (!status.ok()) {
      return ToDBStatus(status);
    }
  }

  return kSuccess;
}

DBStatus DBApproximateDiskBytes(DBEngine* db, DBKey start, DBKey end, uint64_t* size) {
  const std::string start_key(EncodeKey(start));
  const std::string end_key(EncodeKey(end));
  const rocksdb::Range r(start_key, end_key);
  const uint8_t flags = rocksdb::DB::SizeApproximationFlags::INCLUDE_FILES;

  db->rep->GetApproximateSizes(&r, 1, size, flags);
  return kSuccess;
}

DBStatus DBPut(DBEngine* db, DBKey key, DBSlice value) { return db->Put(key, value); }

DBStatus DBMerge(DBEngine* db, DBKey key, DBSlice value) { return db->Merge(key, value); }

DBStatus DBGet(DBEngine* db, DBKey key, DBString* value) { return db->Get(key, value); }

DBStatus DBDelete(DBEngine* db, DBKey key) { return db->Delete(key); }

DBStatus DBDeleteRange(DBEngine* db, DBKey start, DBKey end) { return db->DeleteRange(start, end); }

DBStatus DBDeleteIterRange(DBEngine* db, DBIterator* iter, DBKey start, DBKey end) {
  rocksdb::Iterator* const iter_rep = iter->rep.get();
  iter_rep->Seek(EncodeKey(start));
  const std::string end_key = EncodeKey(end);
  for (; iter_rep->Valid() && kComparator.Compare(iter_rep->key(), end_key) < 0; iter_rep->Next()) {
    DBStatus status = db->Delete(ToDBKey(iter_rep->key()));
    if (status.data != NULL) {
      return status;
    }
  }
  return kSuccess;
}

DBStatus DBCommitAndCloseBatch(DBEngine* db, bool sync) {
  DBStatus status = db->CommitBatch(sync);
  if (status.data == NULL) {
    DBClose(db);
  }
  return status;
}

DBStatus DBApplyBatchRepr(DBEngine* db, DBSlice repr, bool sync) {
  return db->ApplyBatchRepr(repr, sync);
}

DBSlice DBBatchRepr(DBEngine* db) { return db->BatchRepr(); }

DBEngine* DBNewSnapshot(DBEngine* db) { return new DBSnapshot(db); }

DBEngine* DBNewBatch(DBEngine* db, bool writeOnly) {
  if (writeOnly) {
    return new DBWriteOnlyBatch(db);
  }
  return new DBBatch(db);
}

DBStatus DBEnvWriteFile(DBEngine* db, DBSlice path, DBSlice contents) {
  return db->EnvWriteFile(path, contents);
}

DBStatus DBEnvOpenFile(DBEngine* db, DBSlice path, DBWritableFile* file) {
  return db->EnvOpenFile(path, (rocksdb::WritableFile**)file);
}

DBStatus DBEnvReadFile(DBEngine* db, DBSlice path, DBSlice* contents) {
  return db->EnvReadFile(path, contents);
}

DBStatus DBEnvCloseFile(DBEngine* db, DBWritableFile file) {
  return db->EnvCloseFile((rocksdb::WritableFile*)file);
}

DBStatus DBEnvSyncFile(DBEngine* db, DBWritableFile file) {
  return db->EnvSyncFile((rocksdb::WritableFile*)file);
}

DBStatus DBEnvAppendFile(DBEngine* db, DBWritableFile file, DBSlice contents) {
  return db->EnvAppendFile((rocksdb::WritableFile*)file, contents);
}

DBStatus DBEnvDeleteFile(DBEngine* db, DBSlice path) { return db->EnvDeleteFile(path); }

DBStatus DBEnvDeleteDirAndFiles(DBEngine* db, DBSlice dir) { return db->EnvDeleteDirAndFiles(dir); }

DBIterator* DBNewIter(DBEngine* db, bool prefix, bool stats) {
  rocksdb::ReadOptions opts;
  opts.prefix_same_as_start = prefix;
  opts.total_order_seek = !prefix;
  auto db_iter = db->NewIter(&opts);

  if (stats) {
    db_iter->stats.reset(new IteratorStats);
    *db_iter->stats = {};
  }

  return db_iter;
}

DBIterator* DBNewTimeBoundIter(DBEngine* db, DBTimestamp min_ts, DBTimestamp max_ts,
                               bool with_stats) {
  IteratorStats* stats = nullptr;
  if (with_stats) {
    stats = new IteratorStats;
    *stats = {};
  }

  const std::string min = EncodeTimestamp(min_ts);
  const std::string max = EncodeTimestamp(max_ts);
  rocksdb::ReadOptions opts;
  opts.total_order_seek = true;
  opts.table_filter = [min, max, stats](const rocksdb::TableProperties& props) {
    auto userprops = props.user_collected_properties;
    auto tbl_min = userprops.find("crdb.ts.min");
    if (tbl_min == userprops.end() || tbl_min->second.empty()) {
      if (stats != nullptr) {
        ++stats->timebound_num_ssts;
      }
      return true;
    }
    auto tbl_max = userprops.find("crdb.ts.max");
    if (tbl_max == userprops.end() || tbl_max->second.empty()) {
      if (stats != nullptr) {
        ++stats->timebound_num_ssts;
      }
      return true;
    }
    // If the timestamp range of the table overlaps with the timestamp range we
    // want to iterate, the table might contain timestamps we care about.
    bool used = max.compare(tbl_min->second) >= 0 && min.compare(tbl_max->second) <= 0;
    if (used && stats != nullptr) {
      ++stats->timebound_num_ssts;
    }
    return used;
  };

  auto db_iter = db->NewIter(&opts);
  if (stats != nullptr) {
    db_iter->stats.reset(stats);
  }
  return db_iter;
}

void DBIterDestroy(DBIterator* iter) { delete iter; }

IteratorStats DBIterStats(DBIterator* iter) {
  IteratorStats stats = {};
  if (iter->stats != nullptr) {
    stats = *iter->stats;
  }
  return stats;
}

DBIterState DBIterSeek(DBIterator* iter, DBKey key) {
  ScopedStats stats(iter);
  iter->rep->Seek(EncodeKey(key));
  return DBIterGetState(iter);
}

DBIterState DBIterSeekToFirst(DBIterator* iter) {
  ScopedStats stats(iter);
  iter->rep->SeekToFirst();
  return DBIterGetState(iter);
}

DBIterState DBIterSeekToLast(DBIterator* iter) {
  ScopedStats stats(iter);
  iter->rep->SeekToLast();
  return DBIterGetState(iter);
}

DBIterState DBIterNext(DBIterator* iter, bool skip_current_key_versions) {
  ScopedStats stats(iter);
  // If we're skipping the current key versions, remember the key the
  // iterator was pointing out.
  std::string old_key;
  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (!SplitKey(iter->rep->key(), &key, &ts)) {
      DBIterState state = {0};
      state.valid = false;
      state.status = FmtStatus("failed to split mvcc key");
      return state;
    }
    old_key = key.ToString();
  }

  iter->rep->Next();

  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (!SplitKey(iter->rep->key(), &key, &ts)) {
      DBIterState state = {0};
      state.valid = false;
      state.status = FmtStatus("failed to split mvcc key");
      return state;
    }
    if (old_key == key) {
      // We're pointed at a different version of the same key. Fall
      // back to seeking to the next key.
      old_key.append("\0", 1);
      DBKey db_key;
      db_key.key = ToDBSlice(old_key);
      db_key.wall_time = 0;
      db_key.logical = 0;
      iter->rep->Seek(EncodeKey(db_key));
    }
  }

  return DBIterGetState(iter);
}

DBIterState DBIterPrev(DBIterator* iter, bool skip_current_key_versions) {
  ScopedStats stats(iter);
  // If we're skipping the current key versions, remember the key the
  // iterator was pointed out.
  std::string old_key;
  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (SplitKey(iter->rep->key(), &key, &ts)) {
      old_key = key.ToString();
    }
  }

  iter->rep->Prev();

  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (SplitKey(iter->rep->key(), &key, &ts)) {
      if (old_key == key) {
        // We're pointed at a different version of the same key. Fall
        // back to seeking to the prev key. In this case, we seek to
        // the "metadata" key and that back up the iterator.
        DBKey db_key;
        db_key.key = ToDBSlice(old_key);
        db_key.wall_time = 0;
        db_key.logical = 0;
        iter->rep->Seek(EncodeKey(db_key));
        if (iter->rep->Valid()) {
          iter->rep->Prev();
        }
      }
    }
  }

  return DBIterGetState(iter);
}

DBStatus DBMerge(DBSlice existing, DBSlice update, DBString* new_value, bool full_merge) {
  new_value->len = 0;

  cockroach::storage::engine::enginepb::MVCCMetadata meta;
  if (!meta.ParseFromArray(existing.data, existing.len)) {
    return ToDBString("corrupted existing value");
  }

  cockroach::storage::engine::enginepb::MVCCMetadata update_meta;
  if (!update_meta.ParseFromArray(update.data, update.len)) {
    return ToDBString("corrupted update value");
  }

  if (!MergeValues(&meta, update_meta, full_merge, NULL)) {
    return ToDBString("incompatible merge values");
  }
  return MergeResult(&meta, new_value);
}


DBStatus DBMergeOne(DBSlice existing, DBSlice update, DBString* new_value) {
   return DBMerge(existing, update, new_value, true);
}

DBStatus DBPartialMergeOne(DBSlice existing, DBSlice update, DBString* new_value) {
  return DBMerge(existing, update, new_value, false);
}


// DBGetStats queries the given DBEngine for various operational stats and
// write them to the provided DBStatsResult instance.
DBStatus DBGetStats(DBEngine* db, DBStatsResult* stats) { return db->GetStats(stats); }

DBString DBGetCompactionStats(DBEngine* db) { return db->GetCompactionStats(); }

DBStatus DBGetEnvStats(DBEngine* db, DBEnvStatsResult* stats) { return db->GetEnvStats(stats); }

DBSSTable* DBGetSSTables(DBEngine* db, int* n) { return db->GetSSTables(n); }

DBString DBGetUserProperties(DBEngine* db) { return db->GetUserProperties(); }

DBStatus DBIngestExternalFiles(DBEngine* db, char** paths, size_t len, bool move_files,
                               bool allow_file_modifications) {
  std::vector<std::string> paths_vec;
  for (size_t i = 0; i < len; i++) {
    paths_vec.push_back(paths[i]);
  }

  rocksdb::IngestExternalFileOptions ingest_options;
  // If move_files is true and the env supports it, RocksDB will hard link.
  // Otherwise, it will copy.
  ingest_options.move_files = move_files;
  // If snapshot_consistency is true and there is an outstanding RocksDB
  // snapshot, a global sequence number is forced (see the allow_global_seqno
  // option).
  ingest_options.snapshot_consistency = true;
  // If a file is ingested over existing data (including the range tombstones
  // used by range snapshots) or if a RocksDB snapshot is outstanding when this
  // ingest runs, then after moving/copying the file, RocksDB will edit it
  // (overwrite some of the bytes) to have a global sequence number. If this is
  // false, it will error in these cases instead.
  ingest_options.allow_global_seqno = allow_file_modifications;
  // If there are mutations in the memtable for the keyrange covered by the file
  // being ingested, this option is checked. If true, the memtable is flushed
  // and the ingest run. If false, an error is returned.
  ingest_options.allow_blocking_flush = true;
  rocksdb::Status status = db->rep->IngestExternalFile(paths_vec, ingest_options);
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  return kSuccess;
}

struct DBSstFileWriter {
  std::unique_ptr<rocksdb::Options> options;
  std::unique_ptr<rocksdb::Env> memenv;
  rocksdb::SstFileWriter rep;

  DBSstFileWriter(rocksdb::Options* o, rocksdb::Env* m)
      : options(o), memenv(m), rep(rocksdb::EnvOptions(), *o, o->comparator) {}
  virtual ~DBSstFileWriter() {}
};

DBSstFileWriter* DBSstFileWriterNew() {
  // TODO(dan): Right now, backup is the only user of this code, so that's what
  // the options are tuned for. If something else starts using it, we'll likely
  // have to add some configurability.

  rocksdb::BlockBasedTableOptions table_options;
  // Larger block size (4kb default) means smaller file at the expense of more
  // scanning during lookups.
  table_options.block_size = 64 * 1024;
  // The original LevelDB compatible format. We explicitly set the checksum too
  // to guard against the silent version upconversion. See
  // https://github.com/facebook/rocksdb/blob/972f96b3fbae1a4675043bdf4279c9072ad69645/include/rocksdb/table.h#L198
  table_options.format_version = 0;
  table_options.checksum = rocksdb::kCRC32c;

  rocksdb::Options* options = new rocksdb::Options();
  options->comparator = &kComparator;
  options->table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

  std::unique_ptr<rocksdb::Env> memenv;
  memenv.reset(rocksdb::NewMemEnv(rocksdb::Env::Default()));
  options->env = memenv.get();

  return new DBSstFileWriter(options, memenv.release());
}

DBStatus DBSstFileWriterOpen(DBSstFileWriter* fw) {
  rocksdb::Status status = fw->rep.Open("sst");
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  return kSuccess;
}

DBStatus DBSstFileWriterAdd(DBSstFileWriter* fw, DBKey key, DBSlice val) {
  rocksdb::Status status = fw->rep.Put(EncodeKey(key), ToSlice(val));
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  return kSuccess;
}

DBStatus DBSstFileWriterFinish(DBSstFileWriter* fw, DBString* data) {
  rocksdb::Status status = fw->rep.Finish();
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  uint64_t file_size;
  status = fw->memenv->GetFileSize("sst", &file_size);
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  const rocksdb::EnvOptions soptions;
  rocksdb::unique_ptr<rocksdb::SequentialFile> sst;
  status = fw->memenv->NewSequentialFile("sst", &sst, soptions);
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  // scratch is eventually returned as the array part of data and freed by the
  // caller.
  char* scratch = static_cast<char*>(malloc(file_size));

  rocksdb::Slice sst_contents;
  status = sst->Read(file_size, &sst_contents, scratch);
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  if (sst_contents.size() != file_size) {
    return FmtStatus("expected to read %" PRIu64 " bytes but got %zu", file_size,
                     sst_contents.size());
  }

  // The contract of the SequentialFile.Read call above is that it _might_ use
  // scratch as the backing data for sst_contents, but it also _might not_. If
  // it didn't, copy sst_contents into scratch, so we can unconditionally return
  // a DBString backed by scratch (which can then always be freed by the
  // caller). Note that this means the data is always copied exactly once,
  // either by Read or here.
  if (sst_contents.data() != scratch) {
    memcpy(scratch, sst_contents.data(), sst_contents.size());
  }
  data->data = scratch;
  data->len = sst_contents.size();

  return kSuccess;
}

void DBSstFileWriterClose(DBSstFileWriter* fw) { delete fw; }

DBStatus DBLockFile(DBSlice filename, DBFileLock* lock) {
  return ToDBStatus(
      rocksdb::Env::Default()->LockFile(ToString(filename), (rocksdb::FileLock**)lock));
}

DBStatus DBUnlockFile(DBFileLock lock) {
  return ToDBStatus(rocksdb::Env::Default()->UnlockFile((rocksdb::FileLock*)lock));
}
