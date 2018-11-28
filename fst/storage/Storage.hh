//------------------------------------------------------------------------------
// File: Storage.hh
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

#ifndef __EOSFST_STORAGE_HH__
#define __EOSFST_STORAGE_HH__

#include "fst/Namespace.hh"
#include "common/Logging.hh"
#include "common/FileSystem.hh"
#include "common/RWMutex.hh"
#include "fst/Load.hh"
#include "fst/Health.hh"
#include "fst/txqueue/TransferMultiplexer.hh"
#include <vector>
#include <list>
#include <queue>
#include <map>

namespace eos
{
namespace common
{
class TransferQueue;
}
}

EOSFSTNAMESPACE_BEGIN

class Verify;
class Deletion;
class ImportScan;
class FileSystem;

//------------------------------------------------------------------------------
//! Class Storage
//------------------------------------------------------------------------------
class Storage: public eos::common::LogId
{
  friend class XrdFstOfsFile;
  friend class XrdFstOfs;
public:
  //----------------------------------------------------------------------------
  //! Create Storage object
  //!
  //! @param metadirectory path to meta dir
  //!
  //! @return pointer to newly created storage object
  //----------------------------------------------------------------------------
  static Storage* Create(const char* metadirectory);

  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  Storage(const char* metadirectory);

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~Storage();

  //----------------------------------------------------------------------------
  //! Shutdown all helper threads
  //----------------------------------------------------------------------------
  void ShutdownThreads();

  //----------------------------------------------------------------------------
  //! Add deletion object to the list of pending ones
  //!
  //! @param del deletion object
  //----------------------------------------------------------------------------
  void AddDeletion(std::unique_ptr<Deletion> del);

  //----------------------------------------------------------------------------
  //! Get deletion object removing it from the list
  //!
  //! @return get deletion object
  //----------------------------------------------------------------------------
  std::unique_ptr<Deletion> GetDeletion();

  //----------------------------------------------------------------------------
  //! Get number of pending deletions
  //!
  //! @return number of pending deletions
  //----------------------------------------------------------------------------
  size_t GetNumDeletions();

  //----------------------------------------------------------------------------
  //! Get the filesystem associated with the given filesystem id
  //! or NULL if none could be found
  //!
  //! @param fsid filesystem id
  //!
  //! @return associated filesystem object or NULL
  //----------------------------------------------------------------------------
  FileSystem* GetFileSystemById(eos::common::FileSystem::fsid_t fsid);

  //----------------------------------------------------------------------------
  //! Open transaction operation for file fid on filesystem fsid
  //!
  //! @param fsid filesystem id
  //! @param fid file id
  //!
  //! @return true if transaction opened successfully, otherwise false
  //----------------------------------------------------------------------------
  bool OpenTransaction(eos::common::FileSystem::fsid_t fsid,
                       unsigned long long fid);

  //----------------------------------------------------------------------------
  //! Close transaction operation for file fid on filesystem fsid
  //!
  //! @param fsid filesystem id
  //! @param fid file id
  //!
  //! @return true if transaction closed successfully, otherwise false
  //----------------------------------------------------------------------------
  bool CloseTransaction(eos::common::FileSystem::fsid_t fsid,
                        unsigned long long fid);

  //----------------------------------------------------------------------------
  //! Push new verification job to the queue if the maximum number of pending
  //! verifications is not exceeded.
  //!
  //! @param entry verification information about a file
  //----------------------------------------------------------------------------
  void PushVerification(eos::fst::Verify* entry);

  //----------------------------------------------------------------------------
  //! Push new import scan job to the queue.
  //!
  //! @param entry import scan information
  //----------------------------------------------------------------------------
  void PushImportScan(eos::fst::ImportScan* entry);

protected:
  eos::common::RWMutex mFsMutex; ///< Mutex protecting access to the fs map
  std::vector <FileSystem*> mFsVect; ///< Vector of filesystems
  //! Map of filesystem id to filesystem object
  std::map<eos::common::FileSystem::fsid_t, FileSystem*> mFileSystemsMap;
  //! Map of filesystem queue to filesystem object
  std::map<std::string, FileSystem*> mQueue2FsMap;

private:
  static constexpr std::chrono::seconds sConsistencyTimeout {300};
  bool mZombie; ///< State of the node
  XrdOucString mMetaDir; ///< Path to meta directory
  unsigned long long* mScrubPattern[2];
  unsigned long long* mScrubPatternVerify;
  //! Handle to the storage queue of gw transfers
  TransferQueue* mTxGwQueue;
  //! Handle to the low-level queue of gw transfers
  eos::common::TransferQueue* mGwQueue;
  //! Multiplexer for gw transfers
  TransferMultiplexer mGwMultiplexer;
  XrdSysMutex mBootingMutex; // Mutex protecting the boot set
  //! Set containing the filesystems currently booting
  std::set<eos::common::FileSystem::fsid_t> mBootingSet;
  eos::fst::Verify* mRunningVerify; ///< Currently running verification job
  XrdSysMutex mThreadsMutex; ///< Mutex protecting access to the set of threads
  std::set<pthread_t> mThreadSet; ///< Set of running helper threads
  XrdSysMutex mFsFullMapMutex; ///< Mutex protecting access to the fs full map
  //! Map indicating if a filesystem has less than  5 GB free
  std::map<eos::common::FileSystem::fsid_t, bool> mFsFullMap;
  //! Map indicating if a filesystem has less than (headroom) space free, which
  //! disables draining and balancing
  std::map<eos::common::FileSystem::fsid_t, bool> mFsFullWarnMap;
  XrdSysMutex mVerifyMutex; ///< Mutex protecting access to the verifications
  //! Queue of verification jobs pending
  std::queue <eos::fst::Verify*> mVerifications;
  XrdSysMutex mImportScanMutex; ///< Mutex protecting list of import scans
  //! Queue of import scan jobs pending
  std::queue <eos::fst::ImportScan*> mImportScans;
  XrdSysMutex mDeletionsMutex; ///< Mutex protecting the list of deletions
  std::list< std::unique_ptr<Deletion> > mListDeletions; ///< List of deletions
  Load mFstLoad; ///< Net/IO load monitor
  Health mFstHealth; ///< Local disk S.M.A.R.T monitor

  //! Struct BootThreadInfo
  struct BootThreadInfo {
    Storage* storage;
    FileSystem* filesystem;
  };

  //----------------------------------------------------------------------------
  //! Helper methods used for starting worker threads
  //----------------------------------------------------------------------------
  static void* StartVarPartitionMonitor(void* pp);
  static void* StartDaemonSupervisor(void* pp);
  static void* StartFsCommunicator(void* pp);
  static void* StartFsScrub(void* pp);
  static void* StartFsTrim(void* pp);
  static void* StartFsRemover(void* pp);
  static void* StartFsReport(void* pp);
  static void* StartFsErrorReport(void* pp);
  static void* StartFsVerify(void* pp);
  static void* StartFsImportScan(void* pp);
  static void* StartFsPublisher(void* pp);
  static void* StartFsBalancer(void* pp);
  static void* StartFsDrainer(void* pp);
  static void* StartFsCleaner(void* pp);
  static void* StartMgmSyncer(void* pp);
  static void* StartBoot(void* pp);

  //----------------------------------------------------------------------------
  //! Worker threads implementation
  //----------------------------------------------------------------------------
  void Supervisor();
  void Communicator();
  void Scrub();
  void Trim();
  void Remover();
  void Report();
  void ErrorReport();
  void Verify();
  void ImportScan();
  void Publish();
  void Balancer();
  void Drainer();
  void Cleaner();
  void MgmSyncer();
  void Boot(FileSystem* fs);

  //----------------------------------------------------------------------------
  //! Scrub filesystem
  //----------------------------------------------------------------------------
  int ScrubFs(const char* path, unsigned long long free,
              unsigned long long lbocks, unsigned long id, bool direct_io);

  //----------------------------------------------------------------------------
  //! Check if node is in zombie state i.e. true if any of the helper threads
  //! was not properly started.
  //----------------------------------------------------------------------------
  inline bool
  IsZombie()
  {
    return mZombie;
  }

  //----------------------------------------------------------------------------
  //! Run boot thread for specified filesystem
  //!
  //! @param fs filesystem object
  //!
  //! @return true if boot thread started successfully, otherwise false
  //----------------------------------------------------------------------------
  bool RunBootThread(FileSystem* fs);

  //----------------------------------------------------------------------------
  //! Write file system label files (.eosid and .eosuuid) according to the
  //! configuration if they don't exist already.
  //!
  //! @param path mount point of the file system
  //! @param fsid file system id
  //! @param uuid file system uuid
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool FsLabel(std::string path, eos::common::FileSystem::fsid_t fsid,
               std::string uuid);

  //----------------------------------------------------------------------------
  //! Check that the label on the file system matches the one in the
  //! configuration.
  //!
  //! @param path mount point of the file system
  //! @param fsid file system id
  //! @param uuid file system uuid
  //! @param fail_noid when true fail if there is no .eosfsid file present
  //! @param fail_nouuid when true fail if there is no .eosfsuuid file present
  //!
  //! @return true if labels match, otherwise false
  //----------------------------------------------------------------------------
  bool CheckLabel(std::string path, eos::common::FileSystem::fsid_t fsid,
                  std::string uuid, bool fail_noid = false,
                  bool fail_nouuid = false);

  //----------------------------------------------------------------------------
  //! Balancer related methods
  //----------------------------------------------------------------------------
  XrdSysCondVar balanceJobNotification;

  void GetBalanceSlotVariables(unsigned long long& nparalleltx,
                               unsigned long long& ratex,
                               std::string configqueue);


  unsigned long long GetScheduledBalanceJobs(unsigned long long totalscheduled,
      unsigned long long& totalexecuted);

  unsigned long long WaitFreeBalanceSlot(unsigned long long& nparalleltx,
                                         unsigned long long& totalscheduled,
                                         unsigned long long& totalexecuted);

  bool GetFileSystemInBalanceMode(std::vector<unsigned int>& balancefsvector,
                                  unsigned int& cycler,
                                  unsigned long long nparalleltx,
                                  unsigned long long ratetx);

  bool GetBalanceJob(unsigned int index);

  //----------------------------------------------------------------------------
  //! Drain related methods and attributes
  //----------------------------------------------------------------------------
  XrdSysCondVar drainJobNotification;

  //----------------------------------------------------------------------------
  //! Get the number of parallel transfers and transfer rate settings
  //!
  //! @param nparalleltx number of parallel transfers to run
  //! @param ratex rate per transfer
  //! @param nodeconfigqueue config queue to use
  //----------------------------------------------------------------------------
  void GetDrainSlotVariables(unsigned long long& nparalleltx,
                             unsigned long long& ratex,
                             std::string configqueue);

  //----------------------------------------------------------------------------
  //! Get the number of already scheduled jobs
  //!
  //! @param totalscheduled the total number of scheduled jobs
  //! @param totalexecuted the total number of executed jobs
  //! @return number of scheduled jobs
  //!
  //! The time delay from scheduling on MGM and appearing in the queue on the FST
  //! creates an accounting problem. The returned value is the currently known
  //! value on the FST which can be wrong e.g. too small!
  //----------------------------------------------------------------------------
  unsigned long long GetScheduledDrainJobs(unsigned long long totalscheduled,
      unsigned long long& totalexecuted);

  //----------------------------------------------------------------------------
  //! Wait that there is a free slot to schedule a new drain
  //!
  //! @param nparalleltx number of parallel transfers
  //! @param totalscheduled number of total scheduled transfers
  //! @param totalexecuted number of total executed transfers
  //!
  //! @return number of used drain slots
  //----------------------------------------------------------------------------
  unsigned long long WaitFreeDrainSlot(unsigned long long& nparalleltx,
                                       unsigned long long& totalscheduled,
                                       unsigned long long& totalexecuted);

  //----------------------------------------------------------------------------
  //! Get the list of filesystems which are in drain mode in current group
  //!
  //! @param drainfsvector result vector with the indices of draining filesystems
  //! @param cycler cyclic index guaranteeing round-robin selection
  //!
  //! @return true if there is any filesystem in drain mode
  //----------------------------------------------------------------------------
  bool GetFileSystemInDrainMode(std::vector<unsigned int>& drainfsvector,
                                unsigned int& cycler,
                                unsigned long long nparalleltx,
                                unsigned long long ratetx);

  //----------------------------------------------------------------------------
  //! Get drain job for the requested filesystem
  //!
  //! @param index index in the filesystem vector
  //!
  //! @return true if scheduled otherwise false
  //----------------------------------------------------------------------------
  bool GetDrainJob(unsigned int index);

  //----------------------------------------------------------------------------
  //! Check if node is active i.e. the stat.active
  //----------------------------------------------------------------------------
  bool IsNodeActive() const;
};

EOSFSTNAMESPACE_END
#endif
