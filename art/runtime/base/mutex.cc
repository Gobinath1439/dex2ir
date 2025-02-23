/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mutex.h"

#include <errno.h>
#include <sys/time.h>

#include "atomic.h"
#include "base/logging.h"
#include "mutex-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {

Mutex* Locks::abort_lock_ = nullptr;
Mutex* Locks::breakpoint_lock_ = nullptr;
ReaderWriterMutex* Locks::classlinker_classes_lock_ = nullptr;
ReaderWriterMutex* Locks::heap_bitmap_lock_ = nullptr;
Mutex* Locks::logging_lock_ = nullptr;
ReaderWriterMutex* Locks::mutator_lock_ = nullptr;
Mutex* Locks::runtime_shutdown_lock_ = nullptr;
Mutex* Locks::thread_list_lock_ = nullptr;
Mutex* Locks::thread_suspend_count_lock_ = nullptr;
Mutex* Locks::trace_lock_ = nullptr;
Mutex* Locks::profiler_lock_ = nullptr;
Mutex* Locks::unexpected_signal_lock_ = nullptr;
Mutex* Locks::intern_table_lock_ = nullptr;

struct AllMutexData {
  // A guard for all_mutexes_ that's not a mutex (Mutexes must CAS to acquire and busy wait).
  Atomic<const BaseMutex*> all_mutexes_guard;
  // All created mutexes guarded by all_mutexes_guard_.
  std::set<BaseMutex*>* all_mutexes;
  AllMutexData() : all_mutexes(NULL) {}
};
static struct AllMutexData gAllMutexData[kAllMutexDataSize];

#if ART_USE_FUTEXES
static bool ComputeRelativeTimeSpec(timespec* result_ts, const timespec& lhs, const timespec& rhs) {
  const int32_t one_sec = 1000 * 1000 * 1000;  // one second in nanoseconds.
  result_ts->tv_sec = lhs.tv_sec - rhs.tv_sec;
  result_ts->tv_nsec = lhs.tv_nsec - rhs.tv_nsec;
  if (result_ts->tv_nsec < 0) {
    result_ts->tv_sec--;
    result_ts->tv_nsec += one_sec;
  } else if (result_ts->tv_nsec > one_sec) {
    result_ts->tv_sec++;
    result_ts->tv_nsec -= one_sec;
  }
  return result_ts->tv_sec < 0;
}
#endif

class ScopedAllMutexesLock {
 public:
  explicit ScopedAllMutexesLock(const BaseMutex* mutex) : mutex_(mutex) {
    while (!gAllMutexData->all_mutexes_guard.CompareAndSwap(0, mutex)) {
      NanoSleep(100);
    }
  }
  ~ScopedAllMutexesLock() {
    while (!gAllMutexData->all_mutexes_guard.CompareAndSwap(mutex_, 0)) {
      NanoSleep(100);
    }
  }
 private:
  const BaseMutex* const mutex_;
};

BaseMutex::BaseMutex(const char* name, LockLevel level) : level_(level), name_(name) {
  if (kLogLockContentions) {
    ScopedAllMutexesLock mu(this);
    std::set<BaseMutex*>** all_mutexes_ptr = &gAllMutexData->all_mutexes;
    if (*all_mutexes_ptr == NULL) {
      // We leak the global set of all mutexes to avoid ordering issues in global variable
      // construction/destruction.
      *all_mutexes_ptr = new std::set<BaseMutex*>();
    }
    (*all_mutexes_ptr)->insert(this);
  }
}

BaseMutex::~BaseMutex() {
  if (kLogLockContentions) {
    ScopedAllMutexesLock mu(this);
    gAllMutexData->all_mutexes->erase(this);
  }
}

void BaseMutex::DumpAll(std::ostream& os) {
  if (kLogLockContentions) {
    os << "Mutex logging:\n";
    ScopedAllMutexesLock mu(reinterpret_cast<const BaseMutex*>(-1));
    std::set<BaseMutex*>* all_mutexes = gAllMutexData->all_mutexes;
    if (all_mutexes == NULL) {
      // No mutexes have been created yet during at startup.
      return;
    }
    typedef std::set<BaseMutex*>::const_iterator It;
    os << "(Contended)\n";
    for (It it = all_mutexes->begin(); it != all_mutexes->end(); ++it) {
      BaseMutex* mutex = *it;
      if (mutex->HasEverContended()) {
        mutex->Dump(os);
        os << "\n";
      }
    }
    os << "(Never contented)\n";
    for (It it = all_mutexes->begin(); it != all_mutexes->end(); ++it) {
      BaseMutex* mutex = *it;
      if (!mutex->HasEverContended()) {
        mutex->Dump(os);
        os << "\n";
      }
    }
  }
}

void BaseMutex::CheckSafeToWait(Thread* self) {
  if (self == NULL) {
    CheckUnattachedThread(level_);
    return;
  }
  if (kDebugLocking) {
    CHECK(self->GetHeldMutex(level_) == this || level_ == kMonitorLock)
        << "Waiting on unacquired mutex: " << name_;
    bool bad_mutexes_held = false;
    for (int i = kLockLevelCount - 1; i >= 0; --i) {
      if (i != level_) {
        BaseMutex* held_mutex = self->GetHeldMutex(static_cast<LockLevel>(i));
        if (held_mutex != NULL) {
          LOG(ERROR) << "Holding \"" << held_mutex->name_ << "\" "
                     << "(level " << LockLevel(i) << ") while performing wait on "
                     << "\"" << name_ << "\" (level " << level_ << ")";
          bad_mutexes_held = true;
        }
      }
    }
    CHECK(!bad_mutexes_held);
  }
}

inline void BaseMutex::ContentionLogData::AddToWaitTime(uint64_t value) {
  if (kLogLockContentions) {
    // Atomically add value to wait_time.
    uint64_t new_val, old_val;
    volatile int64_t* addr = reinterpret_cast<volatile int64_t*>(&wait_time);
    volatile const int64_t* caddr = const_cast<volatile const int64_t*>(addr);
    do {
      old_val = static_cast<uint64_t>(QuasiAtomic::Read64(caddr));
      new_val = old_val + value;
    } while (!QuasiAtomic::Cas64(static_cast<int64_t>(old_val), static_cast<int64_t>(new_val), addr));
  }
}

void BaseMutex::RecordContention(uint64_t blocked_tid,
                                 uint64_t owner_tid,
                                 uint64_t nano_time_blocked) {
  if (kLogLockContentions) {
    ContentionLogData* data = contetion_log_data_;
    ++(data->contention_count);
    data->AddToWaitTime(nano_time_blocked);
    ContentionLogEntry* log = data->contention_log;
    // This code is intentionally racy as it is only used for diagnostics.
    uint32_t slot = data->cur_content_log_entry;
    if (log[slot].blocked_tid == blocked_tid &&
        log[slot].owner_tid == blocked_tid) {
      ++log[slot].count;
    } else {
      uint32_t new_slot;
      do {
        slot = data->cur_content_log_entry;
        new_slot = (slot + 1) % kContentionLogSize;
      } while (!data->cur_content_log_entry.CompareAndSwap(slot, new_slot));
      log[new_slot].blocked_tid = blocked_tid;
      log[new_slot].owner_tid = owner_tid;
      log[new_slot].count = 1;
    }
  }
}

void BaseMutex::DumpContention(std::ostream& os) const {
  if (kLogLockContentions) {
    const ContentionLogData* data = contetion_log_data_;
    const ContentionLogEntry* log = data->contention_log;
    uint64_t wait_time = data->wait_time;
    uint32_t contention_count = data->contention_count;
    if (contention_count == 0) {
      os << "never contended";
    } else {
      os << "contended " << contention_count
         << " total wait of contender " << PrettyDuration(wait_time)
         << " average " << PrettyDuration(wait_time / contention_count);
      SafeMap<uint64_t, size_t> most_common_blocker;
      SafeMap<uint64_t, size_t> most_common_blocked;
      for (size_t i = 0; i < kContentionLogSize; ++i) {
        uint64_t blocked_tid = log[i].blocked_tid;
        uint64_t owner_tid = log[i].owner_tid;
        uint32_t count = log[i].count;
        if (count > 0) {
          auto it = most_common_blocked.find(blocked_tid);
          if (it != most_common_blocked.end()) {
            most_common_blocked.Overwrite(blocked_tid, it->second + count);
          } else {
            most_common_blocked.Put(blocked_tid, count);
          }
          it = most_common_blocker.find(owner_tid);
          if (it != most_common_blocker.end()) {
            most_common_blocker.Overwrite(owner_tid, it->second + count);
          } else {
            most_common_blocker.Put(owner_tid, count);
          }
        }
      }
      uint64_t max_tid = 0;
      size_t max_tid_count = 0;
      for (const auto& pair : most_common_blocked) {
        if (pair.second > max_tid_count) {
          max_tid = pair.first;
          max_tid_count = pair.second;
        }
      }
      if (max_tid != 0) {
        os << " sample shows most blocked tid=" << max_tid;
      }
      max_tid = 0;
      max_tid_count = 0;
      for (const auto& pair : most_common_blocker) {
        if (pair.second > max_tid_count) {
          max_tid = pair.first;
          max_tid_count = pair.second;
        }
      }
      if (max_tid != 0) {
        os << " sample shows tid=" << max_tid << " owning during this time";
      }
    }
  }
}


Mutex::Mutex(const char* name, LockLevel level, bool recursive)
    : BaseMutex(name, level), recursive_(recursive), recursion_count_(0) {
#if ART_USE_FUTEXES
  state_ = 0;
  exclusive_owner_ = 0;
  num_contenders_ = 0;
#elif defined(__BIONIC__) || defined(__APPLE__)
  // Use recursive mutexes for bionic and Apple otherwise the
  // non-recursive mutexes don't have TIDs to check lock ownership of.
  pthread_mutexattr_t attributes;
  CHECK_MUTEX_CALL(pthread_mutexattr_init, (&attributes));
  CHECK_MUTEX_CALL(pthread_mutexattr_settype, (&attributes, PTHREAD_MUTEX_RECURSIVE));
  CHECK_MUTEX_CALL(pthread_mutex_init, (&mutex_, &attributes));
  CHECK_MUTEX_CALL(pthread_mutexattr_destroy, (&attributes));
#else
  CHECK_MUTEX_CALL(pthread_mutex_init, (&mutex_, NULL));
#endif
}

Mutex::~Mutex() {
#if ART_USE_FUTEXES
  if (state_ != 0) {
    Runtime* runtime = Runtime::Current();
    bool shutting_down = runtime == nullptr || runtime->IsShuttingDown(Thread::Current());
    LOG(shutting_down ? WARNING : FATAL) << "destroying mutex with owner: " << exclusive_owner_;
  } else {
    CHECK_EQ(exclusive_owner_, 0U)  << "unexpectedly found an owner on unlocked mutex " << name_;
    CHECK_EQ(num_contenders_, 0) << "unexpectedly found a contender on mutex " << name_;
  }
#else
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using locks.
  int rc = pthread_mutex_destroy(&mutex_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = (runtime == NULL) || runtime->IsShuttingDownLocked();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_mutex_destroy failed for " << name_;
  }
#endif
}

void Mutex::ExclusiveLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  if (kDebugLocking && !recursive_) {
    AssertNotHeld(self);
  }
  if (!recursive_ || !IsExclusiveHeld(self)) {
#if ART_USE_FUTEXES
    bool done = false;
    do {
      int32_t cur_state = state_;
      if (LIKELY(cur_state == 0)) {
        // Change state from 0 to 1.
        done = __sync_bool_compare_and_swap(&state_, 0 /* cur_state */, 1 /* new state */);
      } else {
        // Failed to acquire, hang up.
        ScopedContentionRecorder scr(this, SafeGetTid(self), GetExclusiveOwnerTid());
        num_contenders_++;
        if (futex(&state_, FUTEX_WAIT, 1, NULL, NULL, 0) != 0) {
          // EAGAIN and EINTR both indicate a spurious failure, try again from the beginning.
          // We don't use TEMP_FAILURE_RETRY so we can intentionally retry to acquire the lock.
          if ((errno != EAGAIN) && (errno != EINTR)) {
            PLOG(FATAL) << "futex wait failed for " << name_;
          }
        }
        num_contenders_--;
      }
    } while (!done);
    QuasiAtomic::MembarStoreLoad();
    DCHECK_EQ(state_, 1);
    exclusive_owner_ = SafeGetTid(self);
#else
    CHECK_MUTEX_CALL(pthread_mutex_lock, (&mutex_));
#endif
    RegisterAsLocked(self);
  }
  recursion_count_++;
  if (kDebugLocking) {
    CHECK(recursion_count_ == 1 || recursive_) << "Unexpected recursion count on mutex: "
        << name_ << " " << recursion_count_;
    AssertHeld(self);
  }
}

bool Mutex::ExclusiveTryLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  if (kDebugLocking && !recursive_) {
    AssertNotHeld(self);
  }
  if (!recursive_ || !IsExclusiveHeld(self)) {
#if ART_USE_FUTEXES
    bool done = false;
    do {
      int32_t cur_state = state_;
      if (cur_state == 0) {
        // Change state from 0 to 1.
        done = __sync_bool_compare_and_swap(&state_, 0 /* cur_state */, 1 /* new state */);
      } else {
        return false;
      }
    } while (!done);
    QuasiAtomic::MembarStoreLoad();
    DCHECK_EQ(state_, 1);
    exclusive_owner_ = SafeGetTid(self);
#else
    int result = pthread_mutex_trylock(&mutex_);
    if (result == EBUSY) {
      return false;
    }
    if (result != 0) {
      errno = result;
      PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
    }
#endif
    RegisterAsLocked(self);
  }
  recursion_count_++;
  if (kDebugLocking) {
    CHECK(recursion_count_ == 1 || recursive_) << "Unexpected recursion count on mutex: "
        << name_ << " " << recursion_count_;
    AssertHeld(self);
  }
  return true;
}

void Mutex::ExclusiveUnlock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertHeld(self);
  recursion_count_--;
  if (!recursive_ || recursion_count_ == 0) {
    if (kDebugLocking) {
      CHECK(recursion_count_ == 0 || recursive_) << "Unexpected recursion count on mutex: "
          << name_ << " " << recursion_count_;
    }
    RegisterAsUnlocked(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (LIKELY(cur_state == 1)) {
      QuasiAtomic::MembarStoreStore();
      // We're no longer the owner.
      exclusive_owner_ = 0;
      // Change state to 0.
      done =  __sync_bool_compare_and_swap(&state_, cur_state, 0 /* new state */);
      if (LIKELY(done)) {  // Spurious fail?
        // Wake a contender
        if (UNLIKELY(num_contenders_ > 0)) {
          futex(&state_, FUTEX_WAKE, 1, NULL, NULL, 0);
        }
      }
    } else {
      // Logging acquires the logging lock, avoid infinite recursion in that case.
      if (this != Locks::logging_lock_) {
        LOG(FATAL) << "Unexpected state_ in unlock " << cur_state << " for " << name_;
      } else {
        LogMessageData data(__FILE__, __LINE__, INTERNAL_FATAL, -1);
        LogMessage::LogLine(data, StringPrintf("Unexpected state_ %d in unlock for %s",
                                               cur_state, name_).c_str());
        _exit(1);
      }
    }
  } while (!done);
  QuasiAtomic::MembarStoreLoad();
#else
    CHECK_MUTEX_CALL(pthread_mutex_unlock, (&mutex_));
#endif
  }
}

void Mutex::Dump(std::ostream& os) const {
  os << (recursive_ ? "recursive " : "non-recursive ")
      << name_
      << " level=" << static_cast<int>(level_)
      << " rec=" << recursion_count_
      << " owner=" << GetExclusiveOwnerTid() << " ";
  DumpContention(os);
}

std::ostream& operator<<(std::ostream& os, const Mutex& mu) {
  mu.Dump(os);
  return os;
}

ReaderWriterMutex::ReaderWriterMutex(const char* name, LockLevel level)
    : BaseMutex(name, level)
#if ART_USE_FUTEXES
    , state_(0), exclusive_owner_(0), num_pending_readers_(0), num_pending_writers_(0)
#endif
{  // NOLINT(whitespace/braces)
#if !ART_USE_FUTEXES
  CHECK_MUTEX_CALL(pthread_rwlock_init, (&rwlock_, NULL));
#endif
}

ReaderWriterMutex::~ReaderWriterMutex() {
#if ART_USE_FUTEXES
  CHECK_EQ(state_, 0);
  CHECK_EQ(exclusive_owner_, 0U);
  CHECK_EQ(num_pending_readers_, 0);
  CHECK_EQ(num_pending_writers_, 0);
#else
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using locks.
  int rc = pthread_rwlock_destroy(&rwlock_);
  if (rc != 0) {
    errno = rc;
    // TODO: should we just not log at all if shutting down? this could be the logging mutex!
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = runtime == NULL || runtime->IsShuttingDownLocked();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_rwlock_destroy failed for " << name_;
  }
#endif
}

void ReaderWriterMutex::ExclusiveLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertNotExclusiveHeld(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (LIKELY(cur_state == 0)) {
      // Change state from 0 to -1.
      done =  __sync_bool_compare_and_swap(&state_, 0 /* cur_state*/, -1 /* new state */);
    } else {
      // Failed to acquire, hang up.
      ScopedContentionRecorder scr(this, SafeGetTid(self), GetExclusiveOwnerTid());
      num_pending_writers_++;
      if (futex(&state_, FUTEX_WAIT, cur_state, NULL, NULL, 0) != 0) {
        // EAGAIN and EINTR both indicate a spurious failure, try again from the beginning.
        // We don't use TEMP_FAILURE_RETRY so we can intentionally retry to acquire the lock.
        if ((errno != EAGAIN) && (errno != EINTR)) {
          PLOG(FATAL) << "futex wait failed for " << name_;
        }
      }
      num_pending_writers_--;
    }
  } while (!done);
  DCHECK_EQ(state_, -1);
  exclusive_owner_ = SafeGetTid(self);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_wrlock, (&rwlock_));
#endif
  RegisterAsLocked(self);
  AssertExclusiveHeld(self);
}

void ReaderWriterMutex::ExclusiveUnlock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  AssertExclusiveHeld(self);
  RegisterAsUnlocked(self);
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (LIKELY(cur_state == -1)) {
      // We're no longer the owner.
      exclusive_owner_ = 0;
      // Change state from -1 to 0.
      done =  __sync_bool_compare_and_swap(&state_, -1 /* cur_state*/, 0 /* new state */);
      if (LIKELY(done)) {  // cmpxchg may fail due to noise?
        // Wake any waiters.
        if (UNLIKELY(num_pending_readers_ > 0 || num_pending_writers_ > 0)) {
          futex(&state_, FUTEX_WAKE, -1, NULL, NULL, 0);
        }
      }
    } else {
      LOG(FATAL) << "Unexpected state_:" << cur_state << " for " << name_;
    }
  } while (!done);
#else
  CHECK_MUTEX_CALL(pthread_rwlock_unlock, (&rwlock_));
#endif
}

#if HAVE_TIMED_RWLOCK
bool ReaderWriterMutex::ExclusiveLockWithTimeout(Thread* self, int64_t ms, int32_t ns) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  timespec end_abs_ts;
  InitTimeSpec(true, CLOCK_REALTIME, ms, ns, &end_abs_ts);
  do {
    int32_t cur_state = state_;
    if (cur_state == 0) {
      // Change state from 0 to -1.
      done =  __sync_bool_compare_and_swap(&state_, 0 /* cur_state */, -1 /* new state */);
    } else {
      // Failed to acquire, hang up.
      timespec now_abs_ts;
      InitTimeSpec(true, CLOCK_REALTIME, 0, 0, &now_abs_ts);
      timespec rel_ts;
      if (ComputeRelativeTimeSpec(&rel_ts, end_abs_ts, now_abs_ts)) {
        return false;  // Timed out.
      }
      ScopedContentionRecorder scr(this, SafeGetTid(self), GetExclusiveOwnerTid());
      num_pending_writers_++;
      if (futex(&state_, FUTEX_WAIT, cur_state, &rel_ts, NULL, 0) != 0) {
        if (errno == ETIMEDOUT) {
          num_pending_writers_--;
          return false;  // Timed out.
        } else if ((errno != EAGAIN) && (errno != EINTR)) {
          // EAGAIN and EINTR both indicate a spurious failure,
          // recompute the relative time out from now and try again.
          // We don't use TEMP_FAILURE_RETRY so we can recompute rel_ts;
          PLOG(FATAL) << "timed futex wait failed for " << name_;
        }
      }
      num_pending_writers_--;
    }
  } while (!done);
  exclusive_owner_ = SafeGetTid(self);
#else
  timespec ts;
  InitTimeSpec(true, CLOCK_REALTIME, ms, ns, &ts);
  int result = pthread_rwlock_timedwrlock(&rwlock_, &ts);
  if (result == ETIMEDOUT) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_rwlock_timedwrlock failed for " << name_;
  }
#endif
  RegisterAsLocked(self);
  AssertSharedHeld(self);
  return true;
}
#endif

bool ReaderWriterMutex::SharedTryLock(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
#if ART_USE_FUTEXES
  bool done = false;
  do {
    int32_t cur_state = state_;
    if (cur_state >= 0) {
      // Add as an extra reader.
      done =  __sync_bool_compare_and_swap(&state_, cur_state, cur_state + 1);
    } else {
      // Owner holds it exclusively.
      return false;
    }
  } while (!done);
#else
  int result = pthread_rwlock_tryrdlock(&rwlock_);
  if (result == EBUSY) {
    return false;
  }
  if (result != 0) {
    errno = result;
    PLOG(FATAL) << "pthread_mutex_trylock failed for " << name_;
  }
#endif
  RegisterAsLocked(self);
  AssertSharedHeld(self);
  return true;
}

bool ReaderWriterMutex::IsSharedHeld(const Thread* self) const {
  DCHECK(self == NULL || self == Thread::Current());
  bool result;
  if (UNLIKELY(self == NULL)) {  // Handle unattached threads.
    result = IsExclusiveHeld(self);  // TODO: a better best effort here.
  } else {
    result = (self->GetHeldMutex(level_) == this);
  }
  return result;
}

void ReaderWriterMutex::Dump(std::ostream& os) const {
  os << name_
      << " level=" << static_cast<int>(level_)
      << " owner=" << GetExclusiveOwnerTid() << " ";
  DumpContention(os);
}

std::ostream& operator<<(std::ostream& os, const ReaderWriterMutex& mu) {
  mu.Dump(os);
  return os;
}

ConditionVariable::ConditionVariable(const char* name, Mutex& guard)
    : name_(name), guard_(guard) {
#if ART_USE_FUTEXES
  sequence_ = 0;
  num_waiters_ = 0;
#else
  CHECK_MUTEX_CALL(pthread_cond_init, (&cond_, NULL));
#endif
}

ConditionVariable::~ConditionVariable() {
#if ART_USE_FUTEXES
  if (num_waiters_!= 0) {
    Runtime* runtime = Runtime::Current();
    bool shutting_down = runtime == nullptr || runtime->IsShuttingDown(Thread::Current());
    LOG(shutting_down ? WARNING : FATAL) << "ConditionVariable::~ConditionVariable for " << name_
        << " called with " << num_waiters_ << " waiters.";
  }
#else
  // We can't use CHECK_MUTEX_CALL here because on shutdown a suspended daemon thread
  // may still be using condition variables.
  int rc = pthread_cond_destroy(&cond_);
  if (rc != 0) {
    errno = rc;
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    Runtime* runtime = Runtime::Current();
    bool shutting_down = (runtime == NULL) || runtime->IsShuttingDownLocked();
    PLOG(shutting_down ? WARNING : FATAL) << "pthread_cond_destroy failed for " << name_;
  }
#endif
}

void ConditionVariable::Broadcast(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  // TODO: enable below, there's a race in thread creation that causes false failures currently.
  // guard_.AssertExclusiveHeld(self);
  DCHECK_EQ(guard_.GetExclusiveOwnerTid(), SafeGetTid(self));
#if ART_USE_FUTEXES
  if (num_waiters_ > 0) {
    sequence_++;  // Indicate the broadcast occurred.
    bool done = false;
    do {
      int32_t cur_sequence = sequence_;
      // Requeue waiters onto mutex. The waiter holds the contender count on the mutex high ensuring
      // mutex unlocks will awaken the requeued waiter thread.
      done = futex(sequence_.Address(), FUTEX_CMP_REQUEUE, 0,
                   reinterpret_cast<const timespec*>(std::numeric_limits<int32_t>::max()),
                   &guard_.state_, cur_sequence) != -1;
      if (!done) {
        if (errno != EAGAIN) {
          PLOG(FATAL) << "futex cmp requeue failed for " << name_;
        }
      }
    } while (!done);
  }
#else
  CHECK_MUTEX_CALL(pthread_cond_broadcast, (&cond_));
#endif
}

void ConditionVariable::Signal(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  guard_.AssertExclusiveHeld(self);
#if ART_USE_FUTEXES
  if (num_waiters_ > 0) {
    sequence_++;  // Indicate a signal occurred.
    // Futex wake 1 waiter who will then come and in contend on mutex. It'd be nice to requeue them
    // to avoid this, however, requeueing can only move all waiters.
    int num_woken = futex(sequence_.Address(), FUTEX_WAKE, 1, NULL, NULL, 0);
    // Check something was woken or else we changed sequence_ before they had chance to wait.
    CHECK((num_woken == 0) || (num_woken == 1));
  }
#else
  CHECK_MUTEX_CALL(pthread_cond_signal, (&cond_));
#endif
}

void ConditionVariable::Wait(Thread* self) {
  guard_.CheckSafeToWait(self);
  WaitHoldingLocks(self);
}

void ConditionVariable::WaitHoldingLocks(Thread* self) {
  DCHECK(self == NULL || self == Thread::Current());
  guard_.AssertExclusiveHeld(self);
  unsigned int old_recursion_count = guard_.recursion_count_;
#if ART_USE_FUTEXES
  num_waiters_++;
  // Ensure the Mutex is contended so that requeued threads are awoken.
  guard_.num_contenders_++;
  guard_.recursion_count_ = 1;
  int32_t cur_sequence = sequence_;
  guard_.ExclusiveUnlock(self);
  if (futex(sequence_.Address(), FUTEX_WAIT, cur_sequence, NULL, NULL, 0) != 0) {
    // Futex failed, check it is an expected error.
    // EAGAIN == EWOULDBLK, so we let the caller try again.
    // EINTR implies a signal was sent to this thread.
    if ((errno != EINTR) && (errno != EAGAIN)) {
      PLOG(FATAL) << "futex wait failed for " << name_;
    }
  }
  guard_.ExclusiveLock(self);
  CHECK_GE(num_waiters_, 0);
  num_waiters_--;
  // We awoke and so no longer require awakes from the guard_'s unlock.
  CHECK_GE(guard_.num_contenders_, 0);
  guard_.num_contenders_--;
#else
  guard_.recursion_count_ = 0;
  CHECK_MUTEX_CALL(pthread_cond_wait, (&cond_, &guard_.mutex_));
#endif
  guard_.recursion_count_ = old_recursion_count;
}

void ConditionVariable::TimedWait(Thread* self, int64_t ms, int32_t ns) {
  DCHECK(self == NULL || self == Thread::Current());
  guard_.AssertExclusiveHeld(self);
  guard_.CheckSafeToWait(self);
  unsigned int old_recursion_count = guard_.recursion_count_;
#if ART_USE_FUTEXES
  timespec rel_ts;
  InitTimeSpec(false, CLOCK_REALTIME, ms, ns, &rel_ts);
  num_waiters_++;
  // Ensure the Mutex is contended so that requeued threads are awoken.
  guard_.num_contenders_++;
  guard_.recursion_count_ = 1;
  int32_t cur_sequence = sequence_;
  guard_.ExclusiveUnlock(self);
  if (futex(sequence_.Address(), FUTEX_WAIT, cur_sequence, &rel_ts, NULL, 0) != 0) {
    if (errno == ETIMEDOUT) {
      // Timed out we're done.
    } else if ((errno == EAGAIN) || (errno == EINTR)) {
      // A signal or ConditionVariable::Signal/Broadcast has come in.
    } else {
      PLOG(FATAL) << "timed futex wait failed for " << name_;
    }
  }
  guard_.ExclusiveLock(self);
  CHECK_GE(num_waiters_, 0);
  num_waiters_--;
  // We awoke and so no longer require awakes from the guard_'s unlock.
  CHECK_GE(guard_.num_contenders_, 0);
  guard_.num_contenders_--;
#else
#if !defined(__APPLE__)
  int clock = CLOCK_MONOTONIC;
#else
  int clock = CLOCK_REALTIME;
#endif
  guard_.recursion_count_ = 0;
  timespec ts;
  InitTimeSpec(true, clock, ms, ns, &ts);
  int rc = TEMP_FAILURE_RETRY(pthread_cond_timedwait(&cond_, &guard_.mutex_, &ts));
  if (rc != 0 && rc != ETIMEDOUT) {
    errno = rc;
    PLOG(FATAL) << "TimedWait failed for " << name_;
  }
#endif
  guard_.recursion_count_ = old_recursion_count;
}

void Locks::Init() {
  if (logging_lock_ != nullptr) {
    // Already initialized.
    DCHECK(abort_lock_ != nullptr);
    DCHECK(breakpoint_lock_ != nullptr);
    DCHECK(classlinker_classes_lock_ != nullptr);
    DCHECK(heap_bitmap_lock_ != nullptr);
    DCHECK(logging_lock_ != nullptr);
    DCHECK(mutator_lock_ != nullptr);
    DCHECK(thread_list_lock_ != nullptr);
    DCHECK(thread_suspend_count_lock_ != nullptr);
    DCHECK(trace_lock_ != nullptr);
    DCHECK(profiler_lock_ != nullptr);
    DCHECK(unexpected_signal_lock_ != nullptr);
    DCHECK(intern_table_lock_ != nullptr);
  } else {
    logging_lock_ = new Mutex("logging lock", kLoggingLock, true);
    abort_lock_ = new Mutex("abort lock", kAbortLock, true);

    DCHECK(breakpoint_lock_ == nullptr);
    breakpoint_lock_ = new Mutex("breakpoint lock", kBreakpointLock);
    DCHECK(classlinker_classes_lock_ == nullptr);
    classlinker_classes_lock_ = new ReaderWriterMutex("ClassLinker classes lock",
                                                      kClassLinkerClassesLock);
    DCHECK(heap_bitmap_lock_ == nullptr);
    heap_bitmap_lock_ = new ReaderWriterMutex("heap bitmap lock", kHeapBitmapLock);
    DCHECK(mutator_lock_ == nullptr);
    mutator_lock_ = new ReaderWriterMutex("mutator lock", kMutatorLock);
    DCHECK(runtime_shutdown_lock_ == nullptr);
    runtime_shutdown_lock_ = new Mutex("runtime shutdown lock", kRuntimeShutdownLock);
    DCHECK(thread_list_lock_ == nullptr);
    thread_list_lock_ = new Mutex("thread list lock", kThreadListLock);
    DCHECK(thread_suspend_count_lock_ == nullptr);
    thread_suspend_count_lock_ = new Mutex("thread suspend count lock", kThreadSuspendCountLock);
    DCHECK(trace_lock_ == nullptr);
    trace_lock_ = new Mutex("trace lock", kTraceLock);
    DCHECK(profiler_lock_ == nullptr);
    profiler_lock_ = new Mutex("profiler lock", kProfilerLock);
    DCHECK(unexpected_signal_lock_ == nullptr);
    unexpected_signal_lock_ = new Mutex("unexpected signal lock", kUnexpectedSignalLock, true);
    DCHECK(intern_table_lock_ == nullptr);
    intern_table_lock_ = new Mutex("InternTable lock", kInternTableLock);
  }
}


}  // namespace art
