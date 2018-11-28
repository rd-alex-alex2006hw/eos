//------------------------------------------------------------------------------
// File: FileSystem.hh
// Author: Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#ifndef __EOSCOMMON_FILESYSTEM_HH__
#define __EOSCOMMON_FILESYSTEM_HH__

#include "common/Namespace.hh"
#include "mq/XrdMqSharedObject.hh"
#include <string>
#include <stdint.h>
#ifndef __APPLE__
#include <sys/vfs.h>
#else
#include <sys/param.h>
#include <sys/mount.h>
#endif
#include <atomic>

EOSCOMMONNAMESPACE_BEGIN;

#define EOS_TAPE_FSID 65535
#define EOS_TAPE_MODE_T (0x10000000ll)

class TransferQueue;

//------------------------------------------------------------------------------
//! Base Class abstracting the internal representation of a filesystem inside
//! the MGM and FST
//------------------------------------------------------------------------------
class FileSystem
{
protected:
  //! Queue Name/Path    = 'queue' + 'path' e.g. /eos/'host'/fst/data01
  std::string mQueuePath;

  //! Queue Name         = 'queue'          e.g. /eos/'host'/fst
  std::string mQueue;

  //! Filesystem Path e.g. /data01
  std::string mPath;

  //! Indicates that if the filesystem is deleted - the deletion should be
  //! broadcasted or not (only MGMs should broadcast deletion!)
  bool BroadCastDeletion;

  //! Handle to the shared object manager object
  //! Before usage mSom needs a read lock and mHash has to be validated
  //! to avoid race conditions in deletion.
  XrdMqSharedObjectManager* mSom;

  //! Mutex used in a file system constructor
  // TODO (esindril): This seems to be useless!!!
  XrdSysMutex mConstructorLock;

  //! Handle to the drain queue
  TransferQueue* mDrainQueue;

  //! Handle to the balance queue
  TransferQueue* mBalanceQueue;

  //! Handle to the extern queue
  TransferQueue* mExternQueue;

  // Counter for prebooked space on that filesystem
  unsigned long long PreBookedSpace;

  //! boot status stored inside the object not the hash
  int32_t mInternalBootStatus;

public:
  //------------------------------------------------------------------------------
  //! Struct & Type definitions
  //------------------------------------------------------------------------------

  //! File System ID type
  typedef uint32_t fsid_t;

  //! File System Status type
  typedef int32_t fsstatus_t;

  //! File System Activation Status Type
  typedef int32_t fsactive_t;

  //! Snapshot Structure of a filesystem

  typedef struct fs_snapshot {
    fsid_t mId;
    std::string mQueue;
    std::string mQueuePath;
    std::string mPath;
    std::string mErrMsg;
    std::string mGroup;
    std::string mUuid;
    std::string mHost;
    std::string mHostPort;
    std::string mProxyGroup;
    std::string mS3Credentials;
    std::string mLogicalPath;
    int8_t      mFileStickyProxyDepth;
    std::string mPort;
    std::string mGeoTag;
    size_t mPublishTimestamp;
    int mGroupIndex;
    std::string mSpace;
    fsstatus_t mStatus;
    fsstatus_t mConfigStatus;
    fsstatus_t mDrainStatus;
    fsactive_t mActiveStatus;
    double mBalThresh;
    long long mHeadRoom;
    unsigned int mErrCode;
    time_t mBootSentTime;
    time_t mBootDoneTime;
    time_t mHeartBeatTime;
    double mDiskUtilization;
    double mDiskWriteRateMb;
    double mDiskReadRateMb;
    double mNetEthRateMiB;
    double mNetInRateMiB;
    double mNetOutRateMiB;
    double mWeightRead;
    double mWeightWrite;
    double mNominalFilled;
    double mDiskFilled;
    long long mDiskCapacity;
    long long mDiskFreeBytes;
    long mDiskType;
    long mDiskBsize;
    long mDiskBlocks;
    long mDiskBused;
    long mDiskBfree;
    long mDiskBavail;
    long mDiskFiles;
    long mDiskFused;
    long mDiskFfree;
    long mFiles;
    long mDiskNameLen;
    long mDiskRopen;
    long mDiskWopen;
    long mScanRate; ///< Maximum scan rate in MB/s
    time_t mScanInterval;
    time_t mGracePeriod;
    time_t mDrainPeriod;
    bool mDrainerOn;
  } fs_snapshot_t;

  typedef struct host_snapshot {
    std::string mQueue;
    std::string mHost;
    std::string mHostPort;
    std::string mGeoTag;
    size_t mPublishTimestamp;
    fsactive_t mActiveStatus;
    time_t mHeartBeatTime;
    double mNetEthRateMiB;
    double mNetInRateMiB;
    double mNetOutRateMiB;
    long mGopen; // number of files open as data proxy
  } host_snapshot_t;

  //----------------------------------------------------------------------------
  //! Constructor
  //! @param queuepath Named Queue to specify the receiver filesystem of
  //!                  modifications e.g. /eos/<host:port>/fst/<path>
  //! @param queue     Named Queue to specify the receiver of modifications
  //!                  e.g. /eos/<host:port>/fst
  //! @param som       Handle to the shared object manager to store filesystem
  //!                  key-value pairs
  //! @param bc2mgm   If true we broad cast to the management server
  //----------------------------------------------------------------------------
  FileSystem(const char* queuepath, const char* queue,
             XrdMqSharedObjectManager* som, bool bc2mgm = false);

  //----------------------------------------------------------------------------
  // Destructor
  //----------------------------------------------------------------------------
  virtual ~FileSystem();

  //----------------------------------------------------------------------------
  // Enums
  //----------------------------------------------------------------------------

  //! Values for a boot status
  enum eBootStatus {
    kOpsError = -2,
    kBootFailure = -1,
    kDown = 0,
    kBootSent = 1,
    kBooting = 2,
    kBooted = 3
  };

  //! Values for a configuration status
  enum eConfigStatus {
    kUnknown = -1,
    kOff = 0,
    kEmpty,
    kDrainDead,
    kDrain,
    kRO,
    kWO,
    kRW
  };

  //! Values for a drain status
  enum eDrainStatus {
    kNoDrain = 0,
    kDrainPrepare = 1,
    kDrainWait = 2,
    kDraining = 3,
    kDrained = 4,
    kDrainStalling = 5,
    kDrainExpired = 6,
    kDrainLostFiles = 7
  };

  //! Values describing if a filesystem is online or offline
  //! (combination of multiple conditions)
  enum eActiveStatus {
    kUndefined = -1,
    kOffline = 0,
    kOnline = 1
  };

  //! Value indication the way a boot message should be executed on an FST node
  enum eBootConfig {
    kBootOptional = 0,
    kBootForced = 1,
    kBootResync = 2
  };

  //----------------------------------------------------------------------------
  // Get file system status as a string
  //----------------------------------------------------------------------------
  static const char* GetStatusAsString(int status);
  static const char* GetDrainStatusAsString(int status);
  static const char* GetConfigStatusAsString(int status);

  //----------------------------------------------------------------------------
  //! Parse a string status into the enum value
  //----------------------------------------------------------------------------
  static int GetStatusFromString(const char* ss);

  //----------------------------------------------------------------------------
  //! Parse a drain status into the enum value
  //----------------------------------------------------------------------------
  static int GetDrainStatusFromString(const char* ss);

  //----------------------------------------------------------------------------
  //! Parse a configuration status into the enum value
  //----------------------------------------------------------------------------
  static int GetConfigStatusFromString(const char*  ss);

  //----------------------------------------------------------------------------
  //! Parse an active status into an fsactive_t value
  //----------------------------------------------------------------------------
  static fsactive_t GetActiveStatusFromString(const char*  ss);

  //----------------------------------------------------------------------------
  //! Get the message string for an auto boot request
  //----------------------------------------------------------------------------
  static const char* GetAutoBootRequestString();

  //----------------------------------------------------------------------------
  //! Get the message string to register a filesystem
  //----------------------------------------------------------------------------
  static const char* GetRegisterRequestString();

  //----------------------------------------------------------------------------
  //! Cache Members
  //----------------------------------------------------------------------------
  fsactive_t cActive; ///< cache value of the active status
  XrdSysMutex cActiveLock; ///< lock protecting the cached active status
  time_t cActiveTime; ///< unix time stamp of last update of the active status
  fsstatus_t cStatus; ///< cache value of the status
  time_t cStatusTime; ///< unix time stamp of last update of the cached status
  XrdSysMutex cStatusLock; ///< lock protecting the cached status
  std::atomic<fsstatus_t> cConfigStatus; ///< cached value of the config status
  XrdSysMutex cConfigLock; ///< lock protecting the cached config status
  time_t cConfigTime; ///< unix time stamp of last update of the cached config status

  //----------------------------------------------------------------------------
  //! Open transaction to initiate bulk modifications on a file system
  //----------------------------------------------------------------------------
  bool
  OpenTransaction()
  {
    RWMutexReadLock lock(mSom->HashMutex);
    XrdMqSharedHash* hash = nullptr;

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->OpenTransaction();
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Close transaction to finish modifications on a file system
  //----------------------------------------------------------------------------
  bool
  CloseTransaction()
  {
    RWMutexReadLock lock(mSom->HashMutex);
    XrdMqSharedHash* hash = nullptr;

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->CloseTransaction();
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Set a filesystem ID.
  //----------------------------------------------------------------------------
  bool
  SetId(fsid_t fsid)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->Set("id", (long long) fsid);
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Set a key-value pair in a filesystem and evt. broadcast it.
  //----------------------------------------------------------------------------
  bool
  SetString(const char* key, const char* str, bool broadcast = true)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->Set(key, str, broadcast);
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Set a double value by name and evt. broadcast it.
  //----------------------------------------------------------------------------
  bool
  SetDouble(const char* key, double f, bool broadcast = true)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->Set(key, f, broadcast);
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Set a long long value and evt. broadcast it.
  //----------------------------------------------------------------------------
  bool
  SetLongLong(const char* key, long long l, bool broadcast = true)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->Set(key, l, broadcast);
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Remove a key from a filesystem and evt. broadcast it.
  //----------------------------------------------------------------------------
  bool
  RemoveKey(const char* key, bool broadcast = true)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      hash->Delete(key, broadcast);
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Set the filesystem status.
  //----------------------------------------------------------------------------
  bool
  SetStatus(fsstatus_t status, bool broadcast = true)
  {
    mInternalBootStatus = status;
    return SetString("stat.boot", GetStatusAsString(status), broadcast);
  }

  //----------------------------------------------------------------------------
  //! Set the activation status
  //----------------------------------------------------------------------------
  bool
  SetActiveStatus(fsactive_t active)
  {
    if (active == kOnline) {
      return SetString("stat.active", "online", false);
    } else {
      return SetString("stat.active", "offline", false);
    }
  }

  //----------------------------------------------------------------------------
  //! Set the draining status
  //----------------------------------------------------------------------------
  bool
  SetDrainStatus(fsstatus_t status, bool broadcast = true)
  {
    return SetString("drainstatus", GetDrainStatusAsString(status), broadcast);
  }

  //----------------------------------------------------------------------------
  //! Store a given statfs struct into the hash representation
  //!
  //! @param statfs struct to read
  //!
  //! @return true if successful otherwise false
  //----------------------------------------------------------------------------
  bool SetStatfs(struct statfs* statfs);

  //----------------------------------------------------------------------------
  //! Get the activation status via a cache.
  //! This can be used with a small cache which 1s expiration time to avoid too
  //! many lookup's in tight loops.
  //----------------------------------------------------------------------------
  fsactive_t
  GetActiveStatus(bool cached = false);

  //----------------------------------------------------------------------------
  //! Get the activation status from a snapshot
  //----------------------------------------------------------------------------
  fsactive_t
  GetActiveStatus(const fs_snapshot_t& snapshot)
  {
    return snapshot.mActiveStatus;
  }

  //----------------------------------------------------------------------------
  //! Get all keys in a vector of strings.
  //----------------------------------------------------------------------------
  bool
  GetKeys(std::vector<std::string>& keys)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      keys = hash->GetKeys();
      return true;
    } else {
      return false;
    }
  }

  //----------------------------------------------------------------------------
  //! Get the string value by key
  //----------------------------------------------------------------------------
  std::string
  GetString(const char* key)
  {
    std::string skey = key;

    if (skey == "<n>") {
      return "1";
    }

    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      return hash->Get(skey);
    } else {
      return "";
    }
  }

  //----------------------------------------------------------------------------
  //! Get the string value by key.
  //----------------------------------------------------------------------------
  double
  GetAge(const char* key)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      // avoid to return a string with a 0 pointer !
      return hash->GetAgeInSeconds(key);
    } else {
      return 0;
    }
  }

  //----------------------------------------------------------------------------
  //! Get a long long value by key
  //----------------------------------------------------------------------------
  long long
  GetLongLong(const char* key)
  {
    std::string skey = key;

    if (skey == "<n>") {
      return 1;
    }

    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      return hash->GetLongLong(key);
    } else {
      return 0;
    }
  }

  //----------------------------------------------------------------------------
  //! Get a double value by key
  //----------------------------------------------------------------------------
  double
  GetDouble(const char* key)
  {
    XrdMqSharedHash* hash = nullptr;
    RWMutexReadLock lock(mSom->HashMutex);

    if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      return hash->GetDouble(key);
    } else {
      return 0;
    }
  }

  //----------------------------------------------------------------------------
  //! Get the pre-booked space.
  //----------------------------------------------------------------------------
  long long
  GetPrebookedSpace()
  {
    // this is dummy for the moment, but will later return 'scheduled' used space
    return PreBookedSpace;
  }

  //----------------------------------------------------------------------------
  //! Do space pre-booking on the filesystem.
  //----------------------------------------------------------------------------
  void
  PreBookSpace(unsigned long long book)
  {
    PreBookedSpace += book;
  }

  //----------------------------------------------------------------------------
  //! Free the pro-booked space on the filesystem.
  //----------------------------------------------------------------------------
  void
  FreePreBookedSpace()
  {
    PreBookedSpace = 0;
  }

  //----------------------------------------------------------------------------
  //! Return handle to the drain queue.
  //----------------------------------------------------------------------------
  TransferQueue*
  GetDrainQueue()
  {
    return mDrainQueue;
  }

  //----------------------------------------------------------------------------
  //! Return handle to the balance queue.
  //----------------------------------------------------------------------------
  TransferQueue*
  GetBalanceQueue()
  {
    return mBalanceQueue;
  }

  //----------------------------------------------------------------------------
  //! Return handle to the external queue.
  //----------------------------------------------------------------------------
  TransferQueue*
  GetExternQueue()
  {
    return mExternQueue;
  }

  //----------------------------------------------------------------------------
  //! Check if the filesystem has a valid heartbeat
  //!
  //! @param fs snapshot of the filesystem
  //!
  //! @return true if filesystem got a heartbeat during the last 300 seconds,
  //! otherwise false
  //----------------------------------------------------------------------------
  bool HasHeartBeat(fs_snapshot_t& fs);

  //----------------------------------------------------------------------------
  //! Try to reserve <bookingspace> on the current filesystem
  //!
  //! @param fs Snapshot of the filesystem.
  //! @param bookingsize Size to be reserved
  //!
  //! @return true if there is enough space - false if not
  //----------------------------------------------------------------------------
  bool ReserveSpace(fs_snapshot_t& fs, unsigned long long bookingsize);

  //----------------------------------------------------------------------------
  //! Return the filesystem id.
  //----------------------------------------------------------------------------
  fsid_t
  GetId()
  {
    return (fsid_t) GetLongLong("id");
  }

  //----------------------------------------------------------------------------
  //! Return the filesystem queue path.
  //----------------------------------------------------------------------------
  std::string
  GetQueuePath()
  {
    return mQueuePath;
  }

  //----------------------------------------------------------------------------
  //! Return the filesystem queue name
  //----------------------------------------------------------------------------
  std::string
  GetQueue()
  {
    return mQueue;
  }

  //----------------------------------------------------------------------------
  //! Return the filesystem path
  //----------------------------------------------------------------------------
  std::string
  GetPath()
  {
    return mPath;
  }

  //----------------------------------------------------------------------------
  //! Return the filesystem status (via a cache)
  //----------------------------------------------------------------------------
  fsstatus_t
  GetStatus(bool cached = false);

  //----------------------------------------------------------------------------
  //! Get internal boot status
  //----------------------------------------------------------------------------
  fsstatus_t
  GetInternalBootStatus()
  {
    return mInternalBootStatus;
  }

  //----------------------------------------------------------------------------
  //! Return the drain status
  //----------------------------------------------------------------------------
  fsstatus_t
  GetDrainStatus()
  {
    return GetDrainStatusFromString(GetString("drainstatus").c_str());
  }

  //----------------------------------------------------------------------------
  //! Return the configuration status (via cache)
  //----------------------------------------------------------------------------
  fsstatus_t
  GetConfigStatus(bool cached = false);

  //----------------------------------------------------------------------------
  //! Return the error code variable of that filesystem
  //----------------------------------------------------------------------------
  int
  GetErrCode()
  {
    return atoi(GetString("stat.errc").c_str());
  }

  //------------------------------------------------------------------------------
  //! Snapshots all variables of a filesystem into a snapshot struct
  //!
  //! @param fs snapshot struct to be filled
  //! @param dolock indicates if the shared hash representing the filesystem has
  //!               to be locked or not
  //!
  //! @return true if successful, otherwise false
  //------------------------------------------------------------------------------
  bool SnapShotFileSystem(FileSystem::fs_snapshot_t& fs, bool dolock = true);

  //------------------------------------------------------------------------------
  //! Snapshots all variables of a filesystem into a snapshot struct
  //!
  //! @param fs snapshot struct to be filled
  //! @param dolock indicates if the shared hash representing the filesystem has
  //!               to be locked or not
  //!
  //! @return true if successful, otherwise false
  //------------------------------------------------------------------------------
  static bool SnapShotHost(XrdMqSharedObjectManager* som,
                           const std::string& queue,
                           FileSystem::host_snapshot_t& fs,
                           bool dolock = true);

  //----------------------------------------------------------------------------
  //! Function printing the file system info to the table
  //----------------------------------------------------------------------------
  void Print(TableHeader& table_mq_header, TableData& table_mq_data,
             std::string listformat, const std::string& filter = "");

  //----------------------------------------------------------------------------
  //! Store a configuration key-val pair.
  //! Internally, these keys are not prefixed with 'stat.'
  //!
  //! @param key key string
  //! @param val value string
  //----------------------------------------------------------------------------
  void CreateConfig(std::string& key, std::string& val);
};

EOSCOMMONNAMESPACE_END;

#endif
