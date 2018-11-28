//------------------------------------------------------------------------------
// File: FileSystem.cc
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

#include "common/Namespace.hh"
#include "common/FileSystem.hh"
#include "common/Logging.hh"
#include "common/TransferQueue.hh"

EOSCOMMONNAMESPACE_BEGIN;

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
FileSystem::FileSystem(const char* queuepath, const char* queue,
                       XrdMqSharedObjectManager* som, bool bc2mgm)
{
  XrdSysMutexHelper cLock(mConstructorLock);
  mQueuePath = queuepath;
  mQueue = queue;
  mPath = queuepath;
  mPath.erase(0, mQueue.length());
  mSom = som;
  mInternalBootStatus = kDown;
  PreBookedSpace = 0;
  cActive = 0;
  cStatus = 0;
  cConfigStatus = 0;
  cActiveTime = 0;
  cStatusTime = 0;
  cConfigTime = 0;
  std::string broadcast = queue;

  if (bc2mgm) {
    broadcast = "/eos/*/mgm";
  }

  if (mSom) {
    mSom->HashMutex.LockRead();
    XrdMqSharedHash* hash = nullptr;

    if (!(hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
      mSom->HashMutex.UnLockRead();
      // create the hash object
      mSom->CreateSharedHash(mQueuePath.c_str(), broadcast.c_str(), som);
      mSom->HashMutex.LockRead();
      hash = mSom->GetObject(mQueuePath.c_str(), "hash");

      if (hash) {
        hash->OpenTransaction();
        hash->Set("queue", mQueue.c_str());
        hash->Set("queuepath", mQueuePath.c_str());
        hash->Set("path", mPath.c_str());
        std::string hostport =
          eos::common::StringConversion::GetStringHostPortFromQueue(mQueue.c_str());

        if (hostport.length()) {
          size_t ppos = hostport.find(":");
          std::string host = hostport;
          std::string port = hostport;

          if (ppos != std::string::npos) {
            host.erase(ppos);
            port.erase(0, ppos + 1);
          } else {
            port = "1094";
          }

          hash->Set("hostport", hostport.c_str());
          hash->Set("host", host.c_str());
          hash->Set("port", port.c_str());
          hash->Set("configstatus", "down");
          hash->Set("drainstatus", "nodrain");
        } else {
          eos_static_crit("there is no hostport defined for queue %s\n", mQueue.c_str());
        }

        hash->CloseTransaction();
      }

      mSom->HashMutex.UnLockRead();
    } else {
      hash->SetBroadCastQueue(broadcast.c_str());
      hash->OpenTransaction();
      hash->Set("queue", mQueue.c_str());
      hash->Set("queuepath", mQueuePath.c_str());
      hash->Set("path", mPath.c_str());
      std::string hostport =
        eos::common::StringConversion::GetStringHostPortFromQueue(mQueue.c_str());

      if (hostport.length()) {
        size_t ppos = hostport.find(":");
        std::string host = hostport;
        std::string port = hostport;

        if (ppos != std::string::npos) {
          host.erase(ppos);
          port.erase(0, ppos + 1);
        } else {
          port = "1094";
        }

        hash->Set("hostport", hostport.c_str());
        hash->Set("host", host.c_str());
        hash->Set("port", port.c_str());
      } else {
        eos_static_crit("there is no hostport defined for queue %s\n", mQueue.c_str());
      }

      hash->CloseTransaction();
      mSom->HashMutex.UnLockRead();
    }

    mDrainQueue = new TransferQueue(mQueue.c_str(), mQueuePath.c_str(), "drainq",
                                    this, mSom, bc2mgm);
    mBalanceQueue = new TransferQueue(mQueue.c_str(), mQueuePath.c_str(),
                                      "balanceq", this, mSom, bc2mgm);
    mExternQueue = new TransferQueue(mQueue.c_str(), mQueuePath.c_str(), "externq",
                                     this, mSom, bc2mgm);
  } else {
    mDrainQueue = 0;
    mBalanceQueue = 0;
    mExternQueue = 0;
  }

  if (bc2mgm) {
    BroadCastDeletion = false;
  } else {
    BroadCastDeletion = true;
  }
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
FileSystem::~FileSystem()
{
  XrdSysMutexHelper cLock(mConstructorLock);

  // remove the shared hash of this file system
  if (mSom) {
    mSom->DeleteSharedHash(mQueuePath.c_str(), BroadCastDeletion);
  }

  if (mDrainQueue) {
    delete mDrainQueue;
  }

  if (mBalanceQueue) {
    delete mBalanceQueue;
  }

  if (mExternQueue) {
    delete mExternQueue;
  }
}

//------------------------------------------------------------------------------
// Return the given status as a string
//------------------------------------------------------------------------------
const char*
FileSystem::GetStatusAsString(int status)
{
  if (status == kDown) {
    return "down";
  }

  if (status == kOpsError) {
    return "opserror";
  }

  if (status == kBootFailure) {
    return "bootfailure";
  }

  if (status == kBootSent) {
    return "bootsent";
  }

  if (status == kBooting) {
    return "booting";
  }

  if (status == kBooted) {
    return "booted";
  }

  return "unknown";
}

//------------------------------------------------------------------------------
// Return given drain status as a string
//------------------------------------------------------------------------------
const char*
FileSystem::GetDrainStatusAsString(int status)
{
  if (status == kNoDrain) {
    return "nodrain";
  }

  if (status == kDrainPrepare) {
    return "prepare";
  }

  if (status == kDrainWait) {
    return "waiting";
  }

  if (status == kDraining) {
    return "draining";
  }

  if (status == kDrained) {
    return "drained";
  }

  if (status == kDrainStalling) {
    return "stalling";
  }

  if (status == kDrainExpired) {
    return "expired";
  }

  if (status == kDrainLostFiles) {
    return "lostfiles";
  }

  return "unknown";
}

//------------------------------------------------------------------------------
// Return given configuration status as a string
//------------------------------------------------------------------------------
const char*
FileSystem::GetConfigStatusAsString(int status)
{
  if (status == kUnknown) {
    return "unknown";
  }

  if (status == kOff) {
    return "off";
  }

  if (status == kEmpty) {
    return "empty";
  }

  if (status == kDrainDead) {
    return "draindead";
  }

  if (status == kDrain) {
    return "drain";
  }

  if (status == kRO) {
    return "ro";
  }

  if (status == kWO) {
    return "wo";
  }

  if (status == kRW) {
    return "rw";
  }

  return "unknown";
}

//------------------------------------------------------------------------------
// Get the status from a string representation
//------------------------------------------------------------------------------
int
FileSystem::GetStatusFromString(const char* ss)
{
  if (!ss) {
    return kDown;
  }

  if (!strcmp(ss, "down")) {
    return kDown;
  }

  if (!strcmp(ss, "opserror")) {
    return kOpsError;
  }

  if (!strcmp(ss, "bootfailure")) {
    return kBootFailure;
  }

  if (!strcmp(ss, "bootsent")) {
    return kBootSent;
  }

  if (!strcmp(ss, "booting")) {
    return kBooting;
  }

  if (!strcmp(ss, "booted")) {
    return kBooted;
  }

  return kDown;
}


//------------------------------------------------------------------------------
// Return configuration status from a string representation
//------------------------------------------------------------------------------
int
FileSystem::GetConfigStatusFromString(const char* ss)
{
  if (!ss) {
    return kDown;
  }

  if (!strcmp(ss, "unknown")) {
    return kUnknown;
  }

  if (!strcmp(ss, "off")) {
    return kOff;
  }

  if (!strcmp(ss, "empty")) {
    return kEmpty;
  }

  if (!strcmp(ss, "draindead")) {
    return kDrainDead;
  }

  if (!strcmp(ss, "drain")) {
    return kDrain;
  }

  if (!strcmp(ss, "ro")) {
    return kRO;
  }

  if (!strcmp(ss, "wo")) {
    return kWO;
  }

  if (!strcmp(ss, "rw")) {
    return kRW;
  }

  return kUnknown;
}

//------------------------------------------------------------------------------
// Return drains status from string representation
//------------------------------------------------------------------------------
int
FileSystem::GetDrainStatusFromString(const char* ss)
{
  if (!ss) {
    return kNoDrain;
  }

  if (!strcmp(ss, "nodrain")) {
    return kNoDrain;
  }

  if (!strcmp(ss, "prepare")) {
    return kDrainPrepare;
  }

  if (!strcmp(ss, "wait")) {
    return kDrainWait;
  }

  if (!strcmp(ss, "draining")) {
    return kDraining;
  }

  if (!strcmp(ss, "stalling")) {
    return kDrainStalling;
  }

  if (!strcmp(ss, "drained")) {
    return kDrained;
  }

  if (!strcmp(ss, "expired")) {
    return kDrainExpired;
  }

  if (!strcmp(ss, "lostfiles")) {
    return kDrainLostFiles;
  }

  return kNoDrain;
}

//------------------------------------------------------------------------------
// Return active status from a string representation
//------------------------------------------------------------------------------
FileSystem::fsactive_t
FileSystem::GetActiveStatusFromString(const char* ss)
{
  if (!ss) {
    return kOffline;
  }

  if (!strcmp(ss, "online")) {
    return kOnline;
  }

  if (!strcmp(ss, "offline")) {
    return kOffline;
  }

  return kOffline;
}

//------------------------------------------------------------------------------
// Return boot request string
//------------------------------------------------------------------------------
const char*
FileSystem::GetAutoBootRequestString()
{
  return "mgm.cmd=bootreq";
}

//------------------------------------------------------------------------------
// Return register request string
//------------------------------------------------------------------------------
const char*
FileSystem::GetRegisterRequestString()
{
  return "mgm.cmd=register";
}

//------------------------------------------------------------------------------
// Store a configuration key-val pair.
// Internally, these keys are not prefixed with 'stat.'
//------------------------------------------------------------------------------
void
FileSystem::CreateConfig(std::string& key, std::string& val)
{
  key = val = "";
  RWMutexReadLock lock(mSom->HashMutex);
  key = mQueuePath;
  XrdMqSharedHash* hash = mSom->GetObject(mQueuePath.c_str(), "hash");
  val = hash->SerializeWithFilter("stat.", true);
}

//------------------------------------------------------------------------------
// Snapshots all variables of a filesystem into a snapshot struct
//------------------------------------------------------------------------------
bool
FileSystem::SnapShotFileSystem(FileSystem::fs_snapshot_t& fs, bool dolock)
{
  if (dolock) {
    mSom->HashMutex.LockRead();
  }

  XrdMqSharedHash* hash = nullptr;

  if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
    fs.mId = (fsid_t) hash->GetUInt("id");
    fs.mQueue = mQueue;
    fs.mQueuePath = mQueuePath;
    fs.mGroup = hash->Get("schedgroup");
    fs.mUuid = hash->Get("uuid");
    fs.mHost = hash->Get("host");
    fs.mHostPort = hash->Get("hostport");
    fs.mProxyGroup = hash->Get("proxygroup");
    fs.mS3Credentials = hash->Get("s3credentials");
    fs.mLogicalPath = hash->Get("logicalpath");
    fs.mFileStickyProxyDepth = -1;

    if (hash->Get("filestickyproxydepth").size()) {
      fs.mFileStickyProxyDepth = hash->GetLongLong("filestickyproxydepth");
    }

    fs.mPort = hash->Get("port");
    std::string::size_type dpos = 0;

    if ((dpos = fs.mGroup.find(".")) != std::string::npos) {
      std::string s = fs.mGroup;
      s.erase(0, dpos + 1);
      fs.mGroupIndex = atoi(s.c_str());
    } else {
      fs.mGroupIndex = 0;
    }

    fs.mSpace = fs.mGroup;

    if (dpos != std::string::npos) {
      fs.mSpace.erase(dpos);
    }

    fs.mPath = mPath;
    fs.mErrMsg = hash->Get("stat.errmsg");
    fs.mGeoTag.clear();

    if (hash->Get("forcegeotag").size()) {
      fs.mGeoTag = hash->Get("forcegeotag");
    }

    if (fs.mGeoTag == "<none>") {
      fs.mGeoTag.clear();
    }

    if (fs.mGeoTag.empty()) {
      fs.mGeoTag = hash->Get("stat.geotag");
    }

    fs.mPublishTimestamp = (size_t)hash->GetLongLong("stat.publishtimestamp");
    fs.mStatus = GetStatusFromString(hash->Get("stat.boot").c_str());
    fs.mConfigStatus = GetConfigStatusFromString(
                         hash->Get("configstatus").c_str());
    fs.mDrainStatus = GetDrainStatusFromString(hash->Get("drainstatus").c_str());
    fs.mActiveStatus = GetActiveStatusFromString(hash->Get("stat.active").c_str());
    //headroom can be configured as KMGTP so the string should be properly converted
    fs.mHeadRoom = StringConversion::GetSizeFromString(hash->Get("headroom"));
    fs.mErrCode = (unsigned int) hash->GetLongLong("stat.errc");
    fs.mBootSentTime = (time_t) hash->GetLongLong("stat.bootsenttime");
    fs.mBootDoneTime = (time_t) hash->GetLongLong("stat.bootdonetime");
    fs.mHeartBeatTime = (time_t) hash->GetLongLong("stat.heartbeattime");
    fs.mDiskUtilization = hash->GetDouble("stat.disk.load");
    fs.mNetEthRateMiB = hash->GetDouble("stat.net.ethratemib");
    fs.mNetInRateMiB = hash->GetDouble("stat.net.inratemib");
    fs.mNetOutRateMiB = hash->GetDouble("stat.net.outratemib");
    fs.mDiskWriteRateMb = hash->GetDouble("stat.disk.writeratemb");
    fs.mDiskReadRateMb = hash->GetDouble("stat.disk.readratemb");
    fs.mDiskType = (long) hash->GetLongLong("stat.statfs.type");
    fs.mDiskFreeBytes = hash->GetLongLong("stat.statfs.freebytes");
    fs.mDiskCapacity = hash->GetLongLong("stat.statfs.capacity");
    fs.mDiskBsize = (long) hash->GetLongLong("stat.statfs.bsize");
    fs.mDiskBlocks = (long) hash->GetLongLong("stat.statfs.blocks");
    fs.mDiskBfree = (long) hash->GetLongLong("stat.statfs.bfree");
    fs.mDiskBused = (long) hash->GetLongLong("stat.statfs.bused");
    fs.mDiskBavail = (long) hash->GetLongLong("stat.statfs.bavail");
    fs.mDiskFiles = (long) hash->GetLongLong("stat.statfs.files");
    fs.mDiskFfree = (long) hash->GetLongLong("stat.statfs.ffree");
    fs.mDiskFused = (long) hash->GetLongLong("stat.statfs.fused");
    fs.mDiskFilled = (double) hash->GetDouble("stat.statfs.filled");
    fs.mNominalFilled = (double) hash->GetDouble("stat.nominal.filled");
    fs.mFiles = (long) hash->GetLongLong("stat.usedfiles");
    fs.mDiskNameLen = (long) hash->GetLongLong("stat.statfs.namelen");
    fs.mDiskRopen = (long) hash->GetLongLong("stat.ropen");
    fs.mDiskWopen = (long) hash->GetLongLong("stat.wopen");
    fs.mWeightRead = 1.0;
    fs.mWeightWrite = 1.0;
    fs.mScanRate = (time_t) hash->GetLongLong("scanrate");
    fs.mScanInterval = (time_t) hash->GetLongLong("scaninterval");
    fs.mGracePeriod = (time_t) hash->GetLongLong("graceperiod");
    fs.mDrainPeriod = (time_t) hash->GetLongLong("drainperiod");
    fs.mDrainerOn   = (hash->Get("stat.drainer") == "on");
    fs.mBalThresh   = hash->GetDouble("stat.balance.threshold");

    if (dolock) {
      mSom->HashMutex.UnLockRead();
    }

    return true;
  } else {
    if (dolock) {
      mSom->HashMutex.UnLockRead();
    }

    fs.mId = 0;
    fs.mQueue = "";
    fs.mQueuePath = "";
    fs.mGroup = "";
    fs.mPath = "";
    fs.mUuid = "";
    fs.mHost = "";
    fs.mHostPort = "";
    fs.mProxyGroup = "";
    fs.mS3Credentials = "";
    fs.mLogicalPath = "";
    fs.mFileStickyProxyDepth = -1;
    fs.mPort = "";
    fs.mErrMsg = "";
    fs.mGeoTag = "";
    fs.mPublishTimestamp = 0;
    fs.mStatus = 0;
    fs.mConfigStatus = 0;
    fs.mDrainStatus = 0;
    fs.mHeadRoom = 0;
    fs.mErrCode = 0;
    fs.mBootSentTime = 0;
    fs.mBootDoneTime = 0;
    fs.mHeartBeatTime = 0;
    fs.mDiskUtilization = 0;
    fs.mNetEthRateMiB = 0;
    fs.mNetInRateMiB = 0;
    fs.mNetOutRateMiB = 0;
    fs.mDiskWriteRateMb = 0;
    fs.mDiskReadRateMb = 0;
    fs.mDiskType = 0;
    fs.mDiskBsize = 0;
    fs.mDiskBlocks = 0;
    fs.mDiskBfree = 0;
    fs.mDiskBused = 0;
    fs.mDiskBavail = 0;
    fs.mDiskFiles = 0;
    fs.mDiskFfree = 0;
    fs.mDiskFused = 0;
    fs.mFiles = 0;
    fs.mDiskNameLen = 0;
    fs.mDiskRopen = 0;
    fs.mDiskWopen = 0;
    fs.mScanRate = 0;
    fs.mDrainerOn = false;
    fs.mBalThresh = 0.0;
    return false;
  }
}

//------------------------------------------------------------------------------
// Snapshots all variables of a filesystem into a snapshot struct
//------------------------------------------------------------------------------
bool
FileSystem::SnapShotHost(XrdMqSharedObjectManager* som,
                         const std::string& queue,
                         FileSystem::host_snapshot_t& host, bool dolock)
{
  if (dolock) {
    som->HashMutex.LockRead();
  }

  XrdMqSharedHash* hash = NULL;

  if ((hash = som->GetObject(queue.c_str(), "hash"))) {
    host.mQueue = queue;
    host.mHost        = hash->Get("stat.host");
    host.mHostPort      = hash->Get("stat.hostport");
    host.mGeoTag        = hash->Get("stat.geotag");
    host.mPublishTimestamp = hash->GetLongLong("stat.publishtimestamp");
    host.mActiveStatus = GetActiveStatusFromString(
                           hash->Get("stat.active").c_str());
    host.mNetEthRateMiB = hash->GetDouble("stat.net.ethratemib");
    host.mNetInRateMiB  = hash->GetDouble("stat.net.inratemib");
    host.mNetOutRateMiB = hash->GetDouble("stat.net.outratemib");
    host.mGopen = hash->GetLongLong("stat.dataproxy.gopen");

    if (dolock) {
      som->HashMutex.UnLockRead();
    }

    return true;
  } else {
    if (dolock) {
      som->HashMutex.UnLockRead();
    }

    host.mQueue = queue;
    host.mHost = "";
    host.mHostPort = "";
    host.mGeoTag        = "";
    host.mPublishTimestamp = 0;
    host.mActiveStatus = kOffline;
    host.mNetEthRateMiB = 0;
    host.mNetInRateMiB  = 0;
    host.mNetOutRateMiB = 0;
    host.mGopen = 0;
    return false;
  }
}

//------------------------------------------------------------------------------
// Store a given statfs struct into the hash representation
//------------------------------------------------------------------------------
bool
FileSystem::SetStatfs(struct statfs* statfs)
{
  if (!statfs) {
    return false;
  }

  bool success = true;
  success &= SetLongLong("stat.statfs.type", statfs->f_type);
  success &= SetLongLong("stat.statfs.bsize", statfs->f_bsize);
  success &= SetLongLong("stat.statfs.blocks", statfs->f_blocks);
  success &= SetLongLong("stat.statfs.bfree", statfs->f_bfree);
  success &= SetLongLong("stat.statfs.bavail", statfs->f_bavail);
  success &= SetLongLong("stat.statfs.files", statfs->f_files);
  success &= SetLongLong("stat.statfs.ffree", statfs->f_ffree);
#ifdef __APPLE__
  success &= SetLongLong("stat.statfs.namelen", MNAMELEN);
#else
  success &= SetLongLong("stat.statfs.namelen", statfs->f_namelen);
#endif
  return success;
}

//------------------------------------------------------------------------------
// Try to reserve <bookingspace> on the current filesystem
//------------------------------------------------------------------------------
bool
FileSystem::ReserveSpace(fs_snapshot_t& fs, unsigned long long bookingsize)
{
  long long headroom = fs.mHeadRoom;
  long long freebytes = fs.mDiskFreeBytes;
  long long prebooked = GetPrebookedSpace();

  // guarantee that we don't overbook the filesystem and we keep <headroom> free
  if ((unsigned long long)(freebytes - prebooked) > ((unsigned long long) headroom
      + bookingsize)) {
    // there is enough space
    return true;
  } else {
    return false;
  }
}

//------------------------------------------------------------------------------
// Check if the filesystem has a valid heartbeat
//------------------------------------------------------------------------------
bool
FileSystem::HasHeartBeat(fs_snapshot_t& fs)
{
  time_t now = time(NULL);
  time_t hb = fs.mHeartBeatTime;

  if ((now - hb) < 60) {
    // we allow some time drift plus overload delay of 60 seconds
    return true;
  }

  return false;
}

//----------------------------------------------------------------------------
// Return the configuration status (via cache)
//----------------------------------------------------------------------------
FileSystem::fsstatus_t
FileSystem::GetConfigStatus(bool cached)
{
  fsstatus_t rConfigStatus = 0;
  XrdSysMutexHelper lock(cConfigLock);

  if (cached) {
    time_t now = time(NULL);

    if (now - cConfigTime) {
      cConfigTime = now;
    } else {
      rConfigStatus = cConfigStatus;
      return rConfigStatus;
    }
  }

  cConfigStatus = GetConfigStatusFromString(GetString("configstatus").c_str());
  rConfigStatus = cConfigStatus;
  return rConfigStatus;
}

//----------------------------------------------------------------------------
// Return the filesystem status (via a cache)
//----------------------------------------------------------------------------
FileSystem::fsstatus_t
FileSystem::GetStatus(bool cached)
{
  fsstatus_t rStatus = 0;
  XrdSysMutexHelper lock(cStatusLock);

  if (cached) {
    time_t now = time(NULL);

    if (now - cStatusTime) {
      cStatusTime = now;
    } else {
      rStatus = cStatus;
      return rStatus;
    }
  }

  cStatus = GetStatusFromString(GetString("stat.boot").c_str());
  rStatus = cStatus;
  return rStatus;
}

//----------------------------------------------------------------------------
// Function printing the file system info to the table
//----------------------------------------------------------------------------
void
FileSystem::Print(TableHeader& table_mq_header, TableData& table_mq_data,
                  std::string listformat, const std::string& filter)
{
  RWMutexReadLock lock(mSom->HashMutex);
  XrdMqSharedHash* hash = nullptr;

  if ((hash = mSom->GetObject(mQueuePath.c_str(), "hash"))) {
    hash->Print(table_mq_header, table_mq_data, listformat, filter);
  }
}

//----------------------------------------------------------------------------
// Get the activation status via a cache.
// This can be used with a small cache which 1s expiration time to avoid too
// many lookup's in tight loops.
//----------------------------------------------------------------------------
FileSystem::fsactive_t
FileSystem::GetActiveStatus(bool cached)
{
  fsactive_t rActive = 0;
  XrdSysMutexHelper lock(cActiveLock);

  if (cached) {
    time_t now = time(NULL);

    if (now - cActiveTime) {
      cActiveTime = now;
    } else {
      rActive = cActive;
      return rActive;
    }
  }

  std::string active = GetString("stat.active");

  if (active == "online") {
    cActive = kOnline;
    return kOnline;
  } else if (active == "offline") {
    cActive = kOffline;
    return kOffline;
  } else {
    cActive = kUndefined;
    return kUndefined;
  }
}

EOSCOMMONNAMESPACE_END;
