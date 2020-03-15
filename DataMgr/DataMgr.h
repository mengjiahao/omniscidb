/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file    DataMgr.h
 * @author Todd Mostak <todd@map-d.com>
 */
#ifndef DATAMGR_H
#define DATAMGR_H

#include "../Shared/MapDParameters.h"
#include "../Shared/mapd_shared_mutex.h"
#include "AbstractBuffer.h"
#include "AbstractBufferMgr.h"
#include "BufferMgr/Buffer.h"
#include "BufferMgr/BufferMgr.h"
#include "MemoryLevel.h"

#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace File_Namespace {
class FileBuffer;
class GlobalFileMgr;
}  // namespace File_Namespace

namespace CudaMgr_Namespace {
class CudaMgr;
}

namespace Data_Namespace {

struct MemoryData {
  size_t slabNum;
  int32_t startPage;
  size_t numPages;
  u_int32_t touch;
  std::vector<int32_t> chunk_key;
  Buffer_Namespace::MemStatus memStatus;
};

struct MemoryInfo {
  size_t pageSize;
  size_t maxNumPages;
  size_t numPageAllocated;
  bool isAllocationCapped;
  std::vector<MemoryData> nodeMemoryData;
};

//! Parse /proc/meminfo into key/value pairs.
class ProcMeminfoParser {
  std::unordered_map<std::string, size_t> items_;

 public:
  ProcMeminfoParser() {
    std::ifstream f("/proc/meminfo");
    std::stringstream ss;
    ss << f.rdbuf();
    for (const std::string& line : split(ss.str(), "\n")) {
      if (line.empty()) {
        continue;
      }
      const auto nv = split(line, ":", 1);
      CHECK(nv.size() == 2) << "unexpected line format in /proc/meminfo: " << line;
      const auto name = strip(nv[0]), value = to_lower(strip(nv[1]));
      auto v = split(value);
      CHECK(v.size() == 1 || v.size() == 2)
          << "unexpected line format in /proc/meminfo: " << line;
      items_[name] = std::atoll(v[0].c_str());
      if (v.size() == 2) {
        CHECK(v[1] == "kb") << "unexpected unit suffix in /proc/meminfo: " << line;
        items_[name] *= 1024;
      }
    }
  }
  auto operator[](const std::string& name) { return items_[name]; }
  auto begin() { return items_.begin(); }
  auto end() { return items_.end(); }
};

class DataMgr {
  friend class GlobalFileMgr;

 public:
  DataMgr(const std::string& dataDir,
          const MapDParameters& mapd_parameters,
          const bool useGpus,
          const int numGpus,
          const int startGpu = 0,
          const size_t reservedGpuMem = (1 << 27),
          const size_t numReaderThreads =
              0); /* 0 means use default for # of reader threads */
  ~DataMgr();
  AbstractBuffer* createChunkBuffer(const ChunkKey& key,
                                    const MemoryLevel memoryLevel,
                                    const int deviceId = 0,
                                    const size_t page_size = 0);
  AbstractBuffer* getChunkBuffer(const ChunkKey& key,
                                 const MemoryLevel memoryLevel,
                                 const int deviceId = 0,
                                 const size_t numBytes = 0);
  void deleteChunksWithPrefix(const ChunkKey& keyPrefix);
  void deleteChunksWithPrefix(const ChunkKey& keyPrefix, const MemoryLevel memLevel);
  AbstractBuffer* alloc(const MemoryLevel memoryLevel,
                        const int deviceId,
                        const size_t numBytes);
  void free(AbstractBuffer* buffer);
  void freeAllBuffers();
  // copies one buffer to another
  void copy(AbstractBuffer* destBuffer, AbstractBuffer* srcBuffer);
  bool isBufferOnDevice(const ChunkKey& key,
                        const MemoryLevel memLevel,
                        const int deviceId);
  std::vector<MemoryInfo> getMemoryInfo(const MemoryLevel memLevel);
  std::string dumpLevel(const MemoryLevel memLevel);
  void clearMemory(const MemoryLevel memLevel);

  // const std::map<ChunkKey, File_Namespace::FileBuffer *> & getChunkMap();
  const std::map<ChunkKey, File_Namespace::FileBuffer*>& getChunkMap();
  void checkpoint(const int db_id,
                  const int tb_id);  // checkpoint for individual table of DB
  void getChunkMetadataVec(
      std::vector<std::pair<ChunkKey, ChunkMetadata>>& chunkMetadataVec);
  void getChunkMetadataVecForKeyPrefix(
      std::vector<std::pair<ChunkKey, ChunkMetadata>>& chunkMetadataVec,
      const ChunkKey& keyPrefix);
  inline bool gpusPresent() { return hasGpus_; }
  void removeTableRelatedDS(const int db_id, const int tb_id);
  void setTableEpoch(const int db_id, const int tb_id, const int start_epoch);
  size_t getTableEpoch(const int db_id, const int tb_id);

  CudaMgr_Namespace::CudaMgr* getCudaMgr() const { return cudaMgr_.get(); }
  File_Namespace::GlobalFileMgr* getGlobalFileMgr() const;

  // database_id, table_id, column_id, fragment_id
  std::vector<int> levelSizes_;

  struct SystemMemoryUsage {
    int64_t free;      // available CPU RAM memory in bytes
    int64_t total;     // total CPU RAM memory in bytes
    int64_t resident;  // resident process memory in bytes
    int64_t vtotal;    // total process virtual memory in bytes
    int64_t regular;   // process bytes non-shared
    int64_t shared;    // process bytes shared (file maps + shmem)
  };

  SystemMemoryUsage getSystemMemoryUsage() const;

 private:
  size_t getTotalSystemMemory() const;
  void populateMgrs(const MapDParameters& mapd_parameters,
                    const size_t userSpecifiedNumReaderThreads);
  void convertDB(const std::string basePath);
  void checkpoint();  // checkpoint for whole DB, called from convertDB proc only
  void createTopLevelMetadata() const;

  std::vector<std::vector<AbstractBufferMgr*>> bufferMgrs_;
  std::unique_ptr<CudaMgr_Namespace::CudaMgr> cudaMgr_;
  std::string dataDir_;
  bool hasGpus_;
  size_t reservedGpuMem_;
  std::map<ChunkKey, std::shared_ptr<mapd_shared_mutex>> chunkMutexMap_;
  mapd_shared_mutex chunkMutexMapMutex_;
};
}  // namespace Data_Namespace

#endif  // DATAMGR_H