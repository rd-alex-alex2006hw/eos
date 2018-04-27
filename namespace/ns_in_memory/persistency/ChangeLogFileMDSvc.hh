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

//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   Change log based FileMD service
//------------------------------------------------------------------------------

#ifndef __EOS_NS_CHANGE_LOG_FILE_MD_SVC_HH__
#define __EOS_NS_CHANGE_LOG_FILE_MD_SVC_HH__

#include "namespace/MDException.hh"
#include "namespace/Namespace.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/interface/IChLogFileMDSvc.hh"
#include "namespace/interface/IQuota.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogFile.hh"
#include "common/Murmur3.hh"
#include "common/hopscotch_map.hh"
#include <google/sparse_hash_map>
#include <google/dense_hash_map>
#include <list>
#include <limits>
#include <functional>

EOSNSNAMESPACE_BEGIN

class LockHandler;
class ChangeLogContainerMDSvc;

//------------------------------------------------------------------------------
//! Change log based FileMD service
//------------------------------------------------------------------------------
class ChangeLogFileMDSvc: public IFileMDSvc, public IChLogFileMDSvc
{
  friend class FileMDFollower;
  friend class ConvertFileMDSvc;
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  ChangeLogFileMDSvc():
    pFirstFreeId(1), pChangeLog(0), pFollowerThread(0), pSlaveLock(0),
    pSlaveMode(false), pSlaveStarted(false), pSlavePoll(1000),
    pFollowStart(0), pFollowPending(0), pContSvc(0), pQuotaStats(0),
    pAutoRepair(0), pResSize(1000000)
  {
    pChangeLog = new ChangeLogFile;
    pthread_mutex_init(&pFollowStartMutex, 0);
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~ChangeLogFileMDSvc()
  {
    delete pChangeLog;
  }

  //----------------------------------------------------------------------------
  //! Initizlize the file service
  //----------------------------------------------------------------------------
  virtual void initialize() override;

  //----------------------------------------------------------------------------
  //! Make a transition from slave to master
  //----------------------------------------------------------------------------
  virtual void slave2Master(std::map<std::string, std::string>& config) override;

  //----------------------------------------------------------------------------
  //! Switch the namespace to read-only mode
  //----------------------------------------------------------------------------
  virtual void makeReadOnly() override;

  //----------------------------------------------------------------------------
  //! Configure the file service
  //----------------------------------------------------------------------------
  virtual void configure(const std::map<std::string, std::string>& config)
  override;

  //----------------------------------------------------------------------------
  //! Finalize the file service
  //----------------------------------------------------------------------------
  virtual void finalize() override;

  //----------------------------------------------------------------------------
  //! Get the file metadata information for the given file ID - asynchronous
  //! API, identical to synchronous one.
  //----------------------------------------------------------------------------
  virtual folly::Future<IFileMDPtr> getFileMDFut(IFileMD::id_t id) override
  {
    return folly::makeFuture(getFileMD(id, 0));
  }

  //----------------------------------------------------------------------------
  //! Get the file metadata information for the given file ID
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<IFileMD> getFileMD(IFileMD::id_t id) override
  {
    return getFileMD(id, 0);
  }

  //------------------------------------------------------------------------
  //! Get the file metadata information for the given file ID and clock
  //! value
  //------------------------------------------------------------------------
  virtual std::shared_ptr<IFileMD> getFileMD(IFileMD::id_t id,
      uint64_t* clock) override;

  //----------------------------------------------------------------------------
  //! Create new file metadata object with an assigned id
  //----------------------------------------------------------------------------
  virtual std::shared_ptr<IFileMD> createFile() override;

  //----------------------------------------------------------------------------
  //! Update the file metadata in the backing store after the FileMD object
  //! has been changed
  //----------------------------------------------------------------------------
  virtual void updateStore(IFileMD* obj) override;

  //----------------------------------------------------------------------------
  //! Remove object from the store
  //----------------------------------------------------------------------------
  virtual void removeFile(IFileMD* obj) override;

  //----------------------------------------------------------------------------
  //! Get number of files
  //----------------------------------------------------------------------------
  virtual uint64_t getNumFiles() override
  {
    return pIdMap.size();
  }

  //----------------------------------------------------------------------------
  //! Add file listener that will be notified about all of the changes in
  //! the store
  //----------------------------------------------------------------------------
  virtual void addChangeListener(IFileMDChangeListener* listener) override;

  //----------------------------------------------------------------------------
  //! Visit all the files
  //----------------------------------------------------------------------------
  virtual void visit(IFileVisitor* visitor) override;

  //----------------------------------------------------------------------------
  //! Notify the listeners about the change
  //----------------------------------------------------------------------------
  virtual void notifyListeners(IFileMDChangeListener::Event* event) override
  {
    ListenerList::iterator it;

    for (it = pListeners.begin(); it != pListeners.end(); ++it) {
      (*it)->fileMDChanged(event);
    }
  }

  //----------------------------------------------------------------------------
  //! Prepare for online compacting.
  //!
  //! No external file metadata mutation may occur while the method is
  //! running.
  //!
  //! @param  newLogFileName name for the compacted log file
  //! @return                compacting information that needs to be passed
  //!                        to other functions
  //----------------------------------------------------------------------------
  void* compactPrepare(const std::string& newLogFileName) override;

  //----------------------------------------------------------------------------
  //! Do the compacting.
  //!
  //! This does not access any of the in-memory structures so any external
  //! metadata operations (including mutations) may happen while it is
  //! running.
  //!
  //! @param  compactingData state information returned by compactPrepare
  //----------------------------------------------------------------------------
  void compact(void*& compactingData) override;

  //----------------------------------------------------------------------------
  //! Commit the compacting infomrmation.
  //!
  //! Updates the metadata structures. Needs an exclusive lock on the
  //! namespace. After successfull completion the new compacted
  //! log will be used for all the new data
  //!
  //! @param compactingData state information obtained from CompactPrepare
  //!                       and modified by Compact
  //! @param autorepair     indicate that broken records should be skipped
  //----------------------------------------------------------------------------
  void compactCommit(void* compactingData, bool autorepair = false) override;

  //----------------------------------------------------------------------------
  //! Register slave lock
  //----------------------------------------------------------------------------
  void setSlaveLock(LockHandler* slaveLock) override
  {
    pSlaveLock = slaveLock;
  }

  //----------------------------------------------------------------------------
  //! Get slave lock
  //----------------------------------------------------------------------------
  LockHandler* getSlaveLock()
  {
    return pSlaveLock;
  }

  //----------------------------------------------------------------------------
  //! Start the slave
  //----------------------------------------------------------------------------
  void startSlave() override;

  //----------------------------------------------------------------------------
  //! Stop the slave mode
  //----------------------------------------------------------------------------
  void stopSlave() override;

  //----------------------------------------------------------------------------
  //! Set container service
  //!
  //! @param cont_svc container service
  //----------------------------------------------------------------------------
  virtual void setContMDService(IContainerMDSvc* cont_svc) override;

  //----------------------------------------------------------------------------
  //! Get the change log
  //----------------------------------------------------------------------------
  ChangeLogFile* getChangeLog()
  {
    return pChangeLog;
  }

  //----------------------------------------------------------------------------
  //! Get the following offset
  //----------------------------------------------------------------------------
  uint64_t getFollowOffset() override
  {
    uint64_t lFollowStart;
    pthread_mutex_lock(&pFollowStartMutex);
    lFollowStart = pFollowStart;
    pthread_mutex_unlock(&pFollowStartMutex);
    return lFollowStart;
  }

  //----------------------------------------------------------------------------
  //! Set the following offset
  //----------------------------------------------------------------------------
  void setFollowOffset(uint64_t offset)
  {
    pthread_mutex_lock(&pFollowStartMutex);
    pFollowStart = offset;
    pthread_mutex_unlock(&pFollowStartMutex);
  }

  //----------------------------------------------------------------------------
  //! Get the following poll interval
  //----------------------------------------------------------------------------
  uint64_t getFollowPollInterval() const
  {
    return pSlavePoll;
  }

  //----------------------------------------------------------------------------
  //! Get the pending items
  //----------------------------------------------------------------------------
  uint64_t getFollowPending() override
  {
    uint64_t lFollowPending;
    pthread_mutex_lock(&pFollowStartMutex);
    lFollowPending = pFollowPending;
    pthread_mutex_unlock(&pFollowStartMutex);
    return lFollowPending;
  }

  //----------------------------------------------------------------------------
  //! Set the pending items
  //----------------------------------------------------------------------------
  void setFollowPending(uint64_t pending)
  {
    pthread_mutex_lock(&pFollowStartMutex);
    pFollowPending = pending;
    pthread_mutex_unlock(&pFollowStartMutex);
  }

  //----------------------------------------------------------------------------
  //! Set the QuotaStats object for the follower
  //!
  //! @param quota_stats object implementing the IQuotaStats interface
  //----------------------------------------------------------------------------
  void setQuotaStats(IQuotaStats* quota_stats) override;

  //----------------------------------------------------------------------------
  //! Get id map reservation size
  //----------------------------------------------------------------------------
  uint64_t getResSize() const
  {
    return pResSize;
  }

  //----------------------------------------------------------------------------
  //! Get changelog warning messages
  //!
  //! @return vector of warning messages
  //----------------------------------------------------------------------------
  std::vector<std::string> getWarningMessages() override;

  //----------------------------------------------------------------------------
  //! Clear changelog warning messages
  //----------------------------------------------------------------------------
  void clearWarningMessages() override;

  //------------------------------------------------------------------------
  //! Get first free file id
  //------------------------------------------------------------------------
  IFileMD::id_t getFirstFreeId() override
  {
    return pFirstFreeId;
  }

  //------------------------------------------------------------------------
  //! Resize container service map
  //------------------------------------------------------------------------
  void resize() override
  {
    return;
  }

  //----------------------------------------------------------------------------
  //! No caching for in-memory namespace.
  //----------------------------------------------------------------------------
  virtual CacheStatistics getCacheStatistics() override
  {
    return {};
  }

private:
  //----------------------------------------------------------------------------
  // Placeholder for the record info
  //----------------------------------------------------------------------------
  struct DataInfo {
    DataInfo(): logOffset(0), ptr((IFileMD*)0),
      buffer(0) {} // for some reason needed by sparse_hash_map::erase
    DataInfo(uint64_t logOffset, std::shared_ptr<IFileMD> ptr)
    {
      this->logOffset = logOffset;
      this->ptr = ptr;
      this->buffer = 0;
    }
    uint64_t logOffset;
    std::shared_ptr<IFileMD> ptr;
    Buffer*  buffer;
  };

  typedef tsl::hopscotch_map <
  IFileMD::id_t, DataInfo,
          Murmur3::MurmurHasher<uint64_t>, Murmur3::eqstr > IdMap;
  typedef std::list<IFileMDChangeListener*> ListenerList;

  //----------------------------------------------------------------------------
  // Changelog record scanner
  //----------------------------------------------------------------------------
  class FileMDScanner: public ILogRecordScanner
  {
  public:
    FileMDScanner(IdMap& idMap, bool slaveMode):
      pIdMap(idMap), pLargestId(0), pSlaveMode(slaveMode)
    {}
    virtual bool processRecord(uint64_t offset, char type,
                               const Buffer& buffer);
    uint64_t getLargestId() const
    {
      return pLargestId;
    }
  private:
    IdMap&    pIdMap;
    uint64_t  pLargestId;
    bool      pSlaveMode;
  };

  //----------------------------------------------------------------------------
  // Attach a broken file to lost+found
  //----------------------------------------------------------------------------
  void attachBroken(const std::string& parent, IFileMD* file);

  //----------------------------------------------------------------------------
  // Data
  //----------------------------------------------------------------------------
  IFileMD::id_t      pFirstFreeId;
  std::string        pChangeLogPath;
  ChangeLogFile*     pChangeLog;
  IdMap              pIdMap;
  ListenerList       pListeners;
  pthread_t          pFollowerThread;
  LockHandler*       pSlaveLock;
  bool               pSlaveMode;
  bool               pSlaveStarted;
  int32_t            pSlavePoll;
  pthread_mutex_t    pFollowStartMutex;
  uint64_t           pFollowStart;
  uint64_t           pFollowPending;
  ChangeLogContainerMDSvc* pContSvc;
  IQuotaStats*       pQuotaStats;
  bool               pAutoRepair;
  uint64_t           pResSize;
};

EOSNSNAMESPACE_END

#endif // __EOS_NS_CHANGE_LOG_FILE_MD_SVC_HH__
