// ----------------------------------------------------------------------
// File: ProcCache.hh
// Author: Geoffray Adde - CERN
// ----------------------------------------------------------------------

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

#ifndef __PROCCACHE__HH__
#define __PROCCACHE__HH__

#include <common/RWMutex.hh>
#include <fstream>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <krb5.h>
#include "common/Logging.hh"

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
class ProcCache;

/*----------------------------------------------------------------------------*/
/**
 * @brief Class to read the command line of a pid through proc files
 *
 */
/*----------------------------------------------------------------------------*/
class ProcReaderCmdLine
{
  std::string pFileName;
public:
  ProcReaderCmdLine(const std::string& filename) :
    pFileName(filename)
  {
  }
  ~ProcReaderCmdLine()
  {
  }
  int ReadContent(std::vector<std::string>& cmdLine);
};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class to read the fsuid and the fsgid of a pid through proc files
 *
 */
/*----------------------------------------------------------------------------*/
class ProcReaderFsUid
{
  std::string pFileName;
public:
  ProcReaderFsUid(const std::string& filename) :
    pFileName(filename)
  {
  }
  ~ProcReaderFsUid()
  {
  }
  int Read();
  int ReadContent(uid_t& fsUid, gid_t& fsGid);
};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class to read /proc/<pid>/stat file starting time , ppid and sid
 *
 */
/*----------------------------------------------------------------------------*/
class ProcReaderPsStat
{
  std::string pFileName;
  int fd;
  FILE* file;

public:
  ProcReaderPsStat() : fd(-1), file(NULL) {}
  ProcReaderPsStat(const std::string& filename)
  {
    fd = -1;
    file = NULL;
    SetFilename(filename);
  }
  ~ProcReaderPsStat()
  {
    Close();
  }
  void SetFilename(const std::string& filename);
  void Close();
  int ReadContent(long long unsigned& startTime, pid_t& ppid, pid_t& sid);
};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class to read the Krb5 login in a credential cache file
 *
 */
/*----------------------------------------------------------------------------*/
class ProcReaderKrb5UserName
{
  std::string pKrb5CcFile;

  static eos::common::RWMutex sMutex;
  static bool sMutexOk;
  static krb5_context sKcontext;
  static bool sKcontextOk;

public:
  ProcReaderKrb5UserName(const std::string& krb5ccfile) :
    pKrb5CcFile(krb5ccfile)  //, pKcontext(), pKcontextOk(true)
  {
    eos::common::RWMutexWriteLock lock(sMutex);

    if (!sMutexOk) {
      ProcReaderKrb5UserName::sMutex.SetBlocking(true);
      ProcReaderKrb5UserName::sMutex.SetBlockedStackTracing(false);
      sMutexOk = true;
    }
  }
  ~ProcReaderKrb5UserName()
  {
  }
  bool ReadUserName(std::string& userName);
  time_t GetModifTime();
  static void StaticDestroy();
};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class to read the GSI identity in a GSI proxy file
 *
 */
/*----------------------------------------------------------------------------*/
class ProcReaderGsiIdentity
{
  std::string pGsiProxyFile;
  static bool sInitOk;
public:
  ProcReaderGsiIdentity(const std::string& gsiproxyfile) :
    pGsiProxyFile(gsiproxyfile)
  {
  }
  ~ProcReaderGsiIdentity()
  {
  }
  bool ReadIdentity(std::string& sidentity);
  time_t GetModifTime();
  static void StaticDestroy();
};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class representing a Proc File information cache entry for one pid.
 *
 */
/*----------------------------------------------------------------------------*/
class ProcCacheEntry
{
  friend class ProcCache;
  // RWMutex to protect entry
  mutable eos::common::RWMutex pMutex;

  // internal values
  ProcReaderPsStat pciPsStat;

  // internal values
  pid_t pPid;
  pid_t pPPid;
  pid_t pSid;
  uid_t pFsUid;
  gid_t pFsGid;
  unsigned long long pStartTime;
  std::string pProcPrefix;
  std::string pCmdLineStr;
  std::vector<std::string> pCmdLineVect;
  std::string pAuthMethod;
  mutable int pError;
  mutable std::string pErrMessage;

  //! return true fs success, false if failure
  int
  ReadContentFromFiles();
  //! return true if the information is up-to-date after the call, false else
  int
  UpdateIfPsChanged();

public:
  ProcCacheEntry(unsigned int pid, const char* procpath = 0) :
    pPid(pid), pPPid(), pSid(), pFsUid(-1), pFsGid(-1), pStartTime(0), pError(0)
  {
    std::stringstream ss;
    ss << (procpath ? procpath : "/proc/") << pPid;
    pProcPrefix = ss.str();
    pMutex.SetBlocking(true);
    pMutex.SetBlockedStackTracing(false);
  }

  ~ProcCacheEntry()
  {
  }

  //
  bool GetAuthMethod(std::string& value) const
  {
    eos::common::RWMutexReadLock lock(pMutex);

    if (pAuthMethod.empty() || pAuthMethod == "none") {
      return false;
    }

    value = pAuthMethod;
    return true;
  }

  bool SetAuthMethod(const std::string& value)
  {
    eos::common::RWMutexWriteLock lock(pMutex);
    pAuthMethod = value;
    return true;
  }

  bool GetFsUidGid(uid_t& uid, gid_t& gid) const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    uid = pFsUid;
    gid = pFsGid;
    return true;
  }

  bool GetSid(pid_t& sid) const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    sid = pSid;
    return true;
  }

  bool GetStartupTime(time_t& sut) const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    sut = pStartTime / sysconf(_SC_CLK_TCK);
    return true;
  }

  const std::vector<std::string>&
  GetArgsVec() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pCmdLineVect;
  }

  const std::string&
  GetArgsStr() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pCmdLineStr;
  }

  bool HasError() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pError;
  }

  const std::string&
  GetErrorMessage() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pErrMessage;
  }

  time_t
  GetProcessStartTime() const
  {
    eos::common::RWMutexReadLock lock(pMutex);
    return pStartTime;
  }

};

/*----------------------------------------------------------------------------*/
/**
 * @brief Class representing a Proc File information cache catalog.
 *
 */
/*----------------------------------------------------------------------------*/
class ProcCache
{
  // RWMUtex; Mutex to protect catalog
  std::map<int, ProcCacheEntry*> pCatalog;
  // RWMutex to protect entry
  eos::common::RWMutex pMutex;
  // path od the proc filesystem
  std::string pProcPath;

public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  ProcCache()
  {
    pMutex.SetBlocking(true);
    pProcPath = "/proc/";
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~ProcCache()
  {
    eos::common::RWMutexWriteLock lock(pMutex);

    for (auto it = pCatalog.begin(); it != pCatalog.end(); it++) {
      delete it->second;
    }
  }

  //----------------------------------------------------------------------------
  //! Move constructor
  //----------------------------------------------------------------------------
  ProcCache(ProcCache&& other);

  //----------------------------------------------------------------------------
  //! Move assignment operator
  //----------------------------------------------------------------------------
  ProcCache& operator=(ProcCache&& other);

  //----------------------------------------------------------------------------
  //! Copy constructor
  //----------------------------------------------------------------------------
  ProcCache(const ProcCache&) = delete;

  //----------------------------------------------------------------------------
  //! Copy assignment operator
  //----------------------------------------------------------------------------
  ProcCache& operator=(const ProcCache&) = delete;


  //! returns true if the cache has an entry for the given pid, false else
  //! regardless of the fact it's up-to-date or not
  bool HasEntry(int pid)
  {
    return static_cast<bool>(pCatalog.count(pid));
  }

  void
  SetProcPath(const char* procpath)
  {
    pProcPath = procpath;
  }

  const std::string&
  GetProcPath() const
  {
    return pProcPath;
  }

  //! returns true if the cache has an up-to-date entry after the call
  int
  InsertEntry(int pid)
  {
    int errCode;
    eos::common::RWMutexWriteLock lock(pMutex);

    // if there is no such process return an error and remove the entry from the cache
    if (getpgid(pid) < 0) {
      RemoveEntry(pid);
      return ESRCH;
    }

    if (!HasEntry(pid)) {
      //eos_static_debug("There and pid is %d",pid);
      pCatalog[pid] = new ProcCacheEntry(pid, pProcPath.c_str());
    }

    auto entry = GetEntry(pid);

    if ((errCode = entry->UpdateIfPsChanged())) {
      eos_static_err("something wrong happened in reading proc stuff %d : %s", pid,
                     pCatalog[pid]->pErrMessage.c_str());
      delete pCatalog[pid];
      pCatalog.erase(pid);
      return errCode;
    }

    return 0;
  }

  //! returns true if the entry is removed after the call
  bool RemoveEntry(int pid)
  {
    if (!HasEntry(pid)) {
      return true;
    } else {
      delete pCatalog[pid];
      pCatalog.erase(pid);
      return true;
    }
  }

  //! returns true if the entry is removed after the call
  int RemoveEntries(const std::set<pid_t>& protect)
  {
    int count = 0;
    eos::common::RWMutexWriteLock lock(pMutex);

    for (auto it = pCatalog.begin(); it != pCatalog.end();) {
      if (protect.count(it->first)) {
        ++it;
      } else {
        delete it->second;
        pCatalog.erase(it++);
        ++count;
      }
    }

    return count;
  }

  //! get the entry associated to the pid if it exists
  //! gets NULL if the the cache does not have such an entry
  ProcCacheEntry* GetEntry(int pid)
  {
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return NULL;
    } else {
      return entry->second;
    }
  }

  bool GetAuthMethod(int pid, std::string& value)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetAuthMethod(value);
  }

  bool GetStartupTime(int pid, time_t& sut)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetStartupTime(sut);
  }

  bool GetFsUidGid(int pid, uid_t& uid, gid_t& gid)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetFsUidGid(uid, gid);
  }

  const std::vector<std::string>&
  GetArgsVec(int pid)
  {
    static std::vector<std::string> dummy;
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return dummy;
    }

    return entry->second->GetArgsVec();
  }

  const std::string&
  GetArgsStr(int pid)
  {
    static std::string dummy;
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return dummy;
    }

    return entry->second->GetArgsStr();
  }

  bool GetSid(int pid, pid_t& sid)
  {
    eos::common::RWMutexReadLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->GetSid(sid);
  }

  bool SetAuthMethod(int pid, const std::string& value)
  {
    eos::common::RWMutexWriteLock lock(pMutex);
    auto entry = pCatalog.find(pid);

    if (entry == pCatalog.end()) {
      return false;
    }

    return entry->second->SetAuthMethod(value);
  }
};

#ifndef __PROCCACHE__NOGPROCCACHE__
//extern ProcCache gProcCache;
extern std::vector<ProcCache> gProcCacheV;
extern int gProcCacheShardSize;
inline ProcCache& gProcCache(int i)
{
  return gProcCacheV[i % gProcCacheShardSize];
}
#endif

#endif
