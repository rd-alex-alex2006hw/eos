// ----------------------------------------------------------------------
// File: Schedule2Delete.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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

#include "common/Logging.hh"
#include "namespace/Prefetcher.hh"
#include "namespace/interface/IView.hh"
#include "namespace/interface/IFileMD.hh"
#include "namespace/interface/IFsView.hh"
#include "authz/XrdCapability.hh"
#include "mq/XrdMqMessaging.hh"
#include "mgm/Stat.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Macros.hh"
#include "mgm/FsView.hh"
#include "mgm/Messaging.hh"

#include <XrdOuc/XrdOucEnv.hh>

//----------------------------------------------------------------------------
// Utility function of sending message declared in anonymous namespace
//----------------------------------------------------------------------------
namespace {
  const char* delete_message = "msg.cmd=drop";

  XrdOucString constructCapability(int fsid, const char* localprefix) {
    XrdOucString capability = "&mgm.access=delete";
    capability += "&mgm.manager=";
    capability += gOFS->ManagerId.c_str();
    capability += "&mgm.fsid=";
    capability += fsid;
    capability += "&mgm.localprefix=";
    capability += localprefix;
    capability += "&mgm.fids=";

    return capability;
  }

  bool constructFileDeleteData(int fid,
                               XrdOucString& idData,
                               XrdOucErrInfo& error) {
    std::shared_ptr<eos::IFileMD> fmd;
    eos::common::RWMutexWriteLock nslock(gOFS->eosViewRWMutex);

    try {
      fmd = gOFS->eosFileService->getFileMD(fid);
    } catch(eos::MDException& e) {
      error.setErrInfo(e.getErrno(), e.getMessage().str().c_str());
      return false;
    }

    // IDs within the list follow the pattern -- hexfid[:lpath:ctime]
    idData = eos::common::FileId::Fid2Hex(fid).c_str();

    if (fmd->hasAttribute("logicalpath")) {
      eos::IFileMD::ctime_t ctime;
      char buff[64];

      std::string lpath = fmd->getAttribute("logicalpath");
      fmd->getCTime(ctime);
      sprintf(buff, "%ld", ctime.tv_sec);

      idData += ":";
      idData += lpath.c_str();
      idData += ":";
      idData += buff;
    }

    idData += ",";

    return true;
  }

  bool sendDeleteMessage(XrdOucString capability,
                         const char* idlist,
                         const char* receiver,
                         unsigned long long capValidity) {
    capability += idlist;

    XrdOucEnv incapenv(capability.c_str());
    XrdOucEnv* outcapenv = 0;
    eos::common::SymKey* symkey = eos::common::gSymKeyStore.GetCurrentKey();
    int rc = gCapabilityEngine.Create(&incapenv, outcapenv, symkey, capValidity);

    if (rc) {
      eos_static_err("unable to create capability - incap=%s errno=%u",
                     capability.c_str(), rc);
    } else {
      int caplen = 0;
      XrdOucString msgbody = delete_message;
      msgbody += outcapenv->Env(caplen);

      XrdMqMessage message("deletion");
      message.SetBody(msgbody.c_str());

      if (!Messaging::gMessageClient.SendMessage(message, receiver)) {
        eos_static_err("unable to send deletion message to %s", receiver);
        rc = -1;
      }
    }

    if (outcapenv) { delete outcapenv; }

    return (rc == 0);
  }
}

//----------------------------------------------------------------------------
// Schedule deletion for FSTs
//----------------------------------------------------------------------------
int
XrdMgmOfs::Schedule2Delete(const char* path,
                           const char* ininfo,
                           XrdOucEnv& env,
                           XrdOucErrInfo& error,
                           eos::common::LogId& ThreadLogId,
                           eos::common::Mapping::VirtualIdentity& vid,
                           const XrdSecEntity* client)
{
  static const char* epname = "Schedule2Delete";

  REQUIRE_SSS_OR_LOCAL_AUTH;
  ACCESSMODE_W;
  MAYSTALL;
  MAYREDIRECT;

  EXEC_TIMING_BEGIN("Scheduled2Delete");

  gOFS->MgmStats.Add("Schedule2Delete", 0, 0, 1);

  const char* anodename = env.Get("mgm.target.nodename");
  std::string nodename = (anodename) ? anodename : "-none-";
  eos_static_debug("nodename=%s", nodename.c_str());

  //--------------------------------------------------------------------------
  // Retrieve filesystem list of the current node
  //--------------------------------------------------------------------------

  std::vector<unsigned long> fslist;
  {
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

    if (!FsView::gFsView.mNodeView.count(nodename)) {
      eos_static_warning("msg=\"node is not configured\" name=%s",
                         nodename.c_str());
      return Emsg(epname, error, EINVAL,
                  "schedule deletes - inexistent node [EINVAL]",
                  nodename.c_str());
    }

    for (auto set_it = FsView::gFsView.mNodeView[nodename]->begin();
         set_it != FsView::gFsView.mNodeView[nodename]->end(); ++set_it) {
      fslist.push_back(*set_it);
    }
  }

  //--------------------------------------------------------------------------
  // Go through each filesystem, collect unlinked files and send list to FST
  //--------------------------------------------------------------------------

  size_t totaldeleted = 0;

  for (auto fsid : fslist) {
    eos::Prefetcher::prefetchFilesystemUnlinkedFileListAndWait(
                         gOFS->eosView, gOFS->eosFsView, fsid);

    std::unordered_set<eos::IFileMD::id_t> set_fids;
    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
    {
      eos::common::RWMutexReadLock vlock(gOFS->eosViewRWMutex);
      uint64_t num_files = eosFsView->getNumUnlinkedFilesOnFs(fsid);

      if (num_files == 0) {
        eos_static_debug("nothing to delete from fsid=%lu", fsid);
        continue;
      }

      set_fids.reserve(num_files);

      // Collect all the file ids to be deleted from the current filesystem
      for (auto it_fid = gOFS->eosFsView->getUnlinkedFileList(fsid);
           (it_fid && it_fid->valid()); it_fid->next()) {
        set_fids.insert(it_fid->getElement());
      }
    }

    eos::mgm::FileSystem* fs = 0;
    XrdOucString receiver = "";
    XrdOucString capability = "";
    XrdOucString idlist = "";
    int ndeleted = 0;
    int msgsize = 0;

    for (auto fid : set_fids) {
      // Loop over all files and emit a deletion message
      eos_static_info("msg=\"add to deletion message\" fxid=%08llx fsid=%lu",
                      fid, fsid);

      if (!fs) {
        // Grab filesystem object only once per fsid
        // to relax the mutex contention

        if (!fsid) {
          eos_static_err("no filesystem with fsid=0 in deletion list");
          continue;
        }

        if (FsView::gFsView.mIdView.count(fsid)) {
          fs = FsView::gFsView.mIdView[fsid];
        } else {
          fs = 0;
        }

        if (fs) {
          // Check the state of the filesystem to make sure it can delete
          if ((fs->GetActiveStatus() == eos::common::FileSystem::kOffline) ||
              (fs->GetConfigStatus() <= eos::common::FileSystem::kOff) ||
              (fs->GetStatus() != eos::common::FileSystem::kBooted)) {
            // Don't send messages as filesystem is down, booting or offline
            break;
          }

          capability = constructCapability(fs->GetId(), fs->GetPath().c_str());
          receiver = fs->GetQueue().c_str();
        }
      }

      XrdOucString idData = "";
      if (constructFileDeleteData(fid, idData, error)) {
        idlist += idData.c_str();

        ndeleted++;
        totaldeleted++;
      } else {
        eos_thread_err("could not process deletion of file fid=%llu. "
                       "Skipping deletion. ec=%d emsg=\"%s\"",
                       error.getErrInfo(), error.getErrText());
        continue;
      }

      // Compute message size
      msgsize = strlen(delete_message) + capability.length() + idlist.length();

      // Segment the message into chunks of 1024 files
      // or maximum 75% of MqMessage capacity
      if (ndeleted > 1024 ||
          msgsize > (0.75 * XrdMqClient::XrdMqMaxMessageLen)) {
        sendDeleteMessage(capability, idlist.c_str(),
                          receiver.c_str(), mCapabilityValidity);

        ndeleted = 0;
        idlist = "";
      }
    }

    // Send the remaining ids
    if (idlist.length()) {
      sendDeleteMessage(capability, idlist.c_str(),
                        receiver.c_str(), mCapabilityValidity);
    }
  }

  //--------------------------------------------------------------------------
  // End the operation
  //--------------------------------------------------------------------------

  if (totaldeleted) {
    error.setErrInfo(0, "submitted");

    gOFS->MgmStats.Add("Scheduled2Delete", 0, 0, totaldeleted);
    EXEC_TIMING_END("Scheduled2Delete");
  } else {
    error.setErrInfo(0, "");
  }

  return SFS_DATA;
}
