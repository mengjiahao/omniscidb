/*
 * Copyright 2019 MapD Technologies, Inc.
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

#ifndef RW_LOCKS_H
#define RW_LOCKS_H

#include "../Shared/mapd_shared_mutex.h"

namespace Catalog_Namespace {

/*
 *  The locking sequence for the locks below is as follows:
 *
 *  inter catalog / syscatalog it is always
 *
 *    read or write lock, then the sqlite_lock (if required)
 *
 *  intra catalog and syscatalog
 *
 *    always syscatalog locks (if required), then catalog locks
 */

template <typename T>
class read_lock {
  const T* catalog;
  mapd_shared_lock<mapd_shared_mutex> lock;
  bool holds_lock;

  template <typename inner_type>
  void lock_catalog(const inner_type* cat) {
    std::thread::id tid = std::this_thread::get_id();

    if (cat->thread_holding_write_lock != tid && !inner_type::thread_holds_read_lock) {
      // sharedMutex_上读锁。thread_holds_read_lock是个thread_local
      lock = mapd_shared_lock<mapd_shared_mutex>(cat->sharedMutex_);
      inner_type::thread_holds_read_lock = true;
      holds_lock = true;
    }
  }

 public:
  read_lock(const T* cat) : catalog(cat), holds_lock(false) { lock_catalog(cat); }

  ~read_lock() { unlock(); }

  void unlock() {
    if (holds_lock) {
      T::thread_holds_read_lock = false;
      lock.unlock();
      holds_lock = false;
    }
  }
};

/**
 * @brief 
 * 意思是 先 rlock sharedMutex_ 再 wlock sqliteMutex_. 
 * 读锁先加，如果同时有多个读锁成功，unlock后是其他有读锁的线程开始lock_catalog，这样比写锁优先级高。
 */
template <typename T>
class sqlite_lock {
  // always obtain a read lock on catalog first
  // to ensure correct locking order
  read_lock<T> cat_read_lock;
  const T* catalog;
  std::unique_lock<std::mutex> lock;
  bool holds_lock;

  template <typename inner_type>
  void lock_catalog(const inner_type* cat) {
    std::thread::id tid = std::this_thread::get_id();

    if (cat->thread_holding_sqlite_lock != tid) {
      // 最终是为了给sqliteMutex_上互斥锁。
      lock = std::unique_lock<std::mutex>(cat->sqliteMutex_);
      cat->thread_holding_sqlite_lock = tid;
      holds_lock = true;
    }
  }

 public:
  sqlite_lock(const T* cat) : cat_read_lock(cat), catalog(cat), holds_lock(false) {
    lock_catalog(cat);
  }

  ~sqlite_lock() { unlock(); }

  void unlock() {
    if (holds_lock) {
      std::thread::id no_thread;
      catalog->thread_holding_sqlite_lock = no_thread;
      lock.unlock();
      cat_read_lock.unlock();
      holds_lock = false;
    }
  }
};

template <typename T>
class write_lock {
  const T* catalog;
  mapd_unique_lock<mapd_shared_mutex> lock;
  bool holds_lock;

  template <typename inner_type>
  void lock_catalog(const inner_type* cat) {
    std::thread::id tid = std::this_thread::get_id();

    if (cat->thread_holding_write_lock != tid) {
      // 线程不同则给cat->sharedMutex_上写锁，进入写锁临界区的将线程id记录在cat->thread_holding_write_lock内。
      // 写锁可递归。
      lock = mapd_unique_lock<mapd_shared_mutex>(cat->sharedMutex_);
      cat->thread_holding_write_lock = tid;
      holds_lock = true;
    }
  }

 public:
  write_lock(const T* cat) : catalog(cat), holds_lock(false) { lock_catalog(cat); }

  ~write_lock() { unlock(); }

  void unlock() {
    if (holds_lock) {
      std::thread::id no_thread;
      catalog->thread_holding_write_lock = no_thread;
      lock.unlock();
      holds_lock = false;
    }
  }
};

}  // namespace Catalog_Namespace

#endif  // RW_LOCKS_H
