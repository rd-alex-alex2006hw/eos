//------------------------------------------------------------------------------
//! @file GroupCmd.cc
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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

{
  //Group.cc
#include "mgm/proc/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/FsView.hh"

  //NsCmd.cc
#include "Cmd.hh"
#include "common/LinuxMemConsumption.hh"
#include "common/LinuxStat.hh"
#include "common/LinuxFds.hh"
#include "namespace/interface/IChLogFileMDSvc.hh"
#include "namespace/interface/IChLogContainerMDSvc.hh"
#include "namespace/interface/IContainerMDSvc.hh"
#include "namespace/interface/IFileMDSvc.hh"
#include "namespace/interface/IView.hh"
#include "namespace/interface/ContainerIterators.hh"
#include "namespace/ns_quarkdb/Constants.hh"
#include "namespace/ns_quarkdb_static/explorer/NamespaceExplorer.hh"
#include "namespace/ns_quarkdb/BackendClient.hh"
#include "namespace/Resolver.hh"
#include "namespace/Constants.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Quota.hh"
#include "mgm/Stat.hh"
#include "mgm/Master.hh"
#include "mgm/ZMQ.hh"
#include <sstream>
}

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Method implementing the specific behvior of the command executed by the
// asynchronous thread
//------------------------------------------------------------------------------
eos::console::ReplyProto
GroupCmd::ProcessRequest()
{
  eos::console::ReplyProto reply;
  eos::console::GroupProto group = mReqProto.group();
  eos::console::GroupProto::SubcmdCase subcmd = group.subcmd_case();

  if (subcmd == eos::console::GroupProto::kLs) {
    LsSubcmd(group.ls(), reply);
  } else if (subcmd == eos::console::GroupProto::kRm) {
    RmSubcmd(group.rm(), reply);
  } else if (subcmd == eos::console::GroupProto::kSet) {
    SetSubcmd(group.set(), reply);
  } else {
    reply.set_retc(EINVAL);
    reply.set_std_err("error: not supported");
  }

  return reply;
}

//------------------------------------------------------------------------------
// Execute ls subcommand
//------------------------------------------------------------------------------
int
GroupCmd::LsSubcmd(const eos::console::GroupProto_LsProto& ls,
                   eos::console::ReplyProto& reply)
{
  std::string output;
  std::string format;
  std::string mListFormat;
  std::string fqdn;

  switch (ls.outformat()) {
  case MONITORING:
    mOutFormat = "m"
                 break;

  case LONG:
    mOutFormat = "l"
                 break;

  case IOGROUP:
    mOutFormat = "io"
                 break;

  case IOFS:
    mOutFormat = "IO"
                 break;

  default :
    //
  }

  format = FsView::GetGroupFormat(std::string(mOutFormat.c_str()));

  if ((mOutFormat == "l")) {
    mListFormat = FsView::GetFileSystemFormat(std::string(mOutFormat.c_str()));
  }

  if ((mOutFormat == "IO")) {
    mListFormat = FsView::GetFileSystemFormat(std::string("io"));
    mOutFormat = "io";
  }

  // if -b || --brief
  if (ls.outhost()) {
    fqdn = ls.outhost()
  }

  if (!fqdn) {
    if (format.find("S") != std::string::npos) {
      format.replace(format.find("S"), 1, "s");
    }

    if (mListFormat.find("S") != std::string::npos) {
      mListFormat.replace(mListFormat.find("S"), 1, "s");
    }
  }

  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
  FsView::gFsView.PrintGroups(output, format, mListFormat, ls.outdepth(),
                              ls.selection());
  stdOut += output.c_str(); // #TOCK needed?
  reply.set_std_out(stdOut);
  reply.set_std_err(stdErr);
  reply.set_retc(retc);
  return SFS_OK; //TOCK is this needed?
}

//------------------------------------------------------------------------------
// Execute rm subcommand
//------------------------------------------------------------------------------
int
GroupCmd::RmSubcmd(const eos::console::GroupProto_RmProto& rm,
                   eos::console::ReplyProto& reply)
{
  if (pVid->uid == 0) {
    std::string groupname = (std::to_string(rm.group().length()) ? std::to_string(
                               rm.group()) : "");   // #TOCK is this variable needed or is rm.group() enough?

    if ((!groupname.length())) {
      stdErr = "error: illegal parameters";
      retc = EINVAL;
    } else {
      eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

      if (!FsView::gFsView.mGroupView.count(groupname)) {
        stdErr = "error: no such group '" + groupname.c_str() + "'";
        retc = ENOENT;
      } else {
        for (auto it = FsView::gFsView.mGroupView[groupname]->begin();
             it != FsView::gFsView.mGroupView[groupname]->end(); it++) {
          if (FsView::gFsView.mIdView.count(*it)) {
            FileSystem* fs = FsView::gFsView.mIdView[*it];

            if (fs) {
              // check that all filesystems are empty
              if ((fs->GetConfigStatus(false) != eos::common::FileSystem::kEmpty)) {
                stdErr = "error: unable to remove group '" + groupname.c_str() +
                         "' - filesystems are not all in empty state - try list the group and drain them or set: fs config <fsid> configstatus=empty\n";
                retc = EBUSY;
                reply.set_std_err(stdErr);
                reply.set_retc(retc);
                return SFS_OK;
              }
            }
          }
        }

        std::string groupconfigname =
          eos::common::GlobalConfig::gConfig.QueuePrefixName(
            FsGroup::sGetConfigQueuePrefix(), groupname.c_str());

        if (!eos::common::GlobalConfig::gConfig.SOM()->DeleteSharedHash(
              groupconfigname.c_str())) {
          stdErr = "error: unable to remove config of group '" + groupname.c_str() + "'";
          retc = EIO;
        } else {
          if (FsView::gFsView.UnRegisterGroup(groupname.c_str())) {
            //reply.set_std_out("success: removed group '" + groupname.c_str() + "'"); // #TOCK
            stdOut = "success: removed group '" + groupname.c_str() + "'";
          } else {
            stdErr = "error: unable to unregister group '" + groupname.c_str() + "'";
          }
        }
      }
    }
  } else {
    stdErr = "error: you have to take role 'root' to execute this command";
    retc = EPERM;
  }

  reply.set_std_out(stdOut);
  reply.set_std_err(stdErr);
  reply.set_retc(retc);
  return SFS_OK; //TOCK is this needed?
}

//------------------------------------------------------------------------------
// Execute set subcommand
//------------------------------------------------------------------------------
int
GroupCmd::SetSubcmd(const eos::console::GroupProto_SetProto& set,
                    eos::console::ReplyProto& reply)
{
  if (pVid->uid == 0) {
    std::string groupname = (std::to_string(set.group().length()) ? std::to_string(
                               set.group()) :
                             "");   // #TOCK is this variable needed or is using rm.group() enough?
    std::string status = std::to_string(
                           set.group_state()); // #TOCK how are proto bool type handled?
    std::string key = "status";

    if ((!groupname.length()) || (!status.length())) {
      stdErr = "error: illegal parameters";
      retc = EINVAL;
    } else {
      eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);

      if (!FsView::gFsView.mGroupView.count(groupname)) {
        stdOut = "info: creating group '" + groupname.c_str() + "'";

        if (!FsView::gFsView.RegisterGroup(groupname.c_str())) {
          std::string groupconfigname =
            eos::common::GlobalConfig::gConfig.QueuePrefixName(
              gOFS->GroupConfigQueuePrefix.c_str(), groupname.c_str());
          retc = EIO;
          stdErr = "error: cannot register group <" + groupname.c_str() + ">";
        }
      }

      if (!retc) {
        // Set this new group to offline
        if (!FsView::gFsView.mGroupView[groupname]->SetConfigMember
            (key, status, true, "/eos/*/mgm")) {
          stdErr = "error: cannot set config status";
          retc = EIO;
        }

        if (status == "on") {
          // Recompute the drain status in this group
          bool setactive = false;
          FileSystem* fs = 0;

          if (FsView::gFsView.mGroupView.count(groupname)) {
            for (auto git = FsView::gFsView.mGroupView[groupname]->begin();
                 git != FsView::gFsView.mGroupView[groupname]->end(); git++) {
              if (FsView::gFsView.mIdView.count(*git)) {
                int drainstatus = (eos::common::FileSystem::GetDrainStatusFromString(
                                     FsView::gFsView.mIdView[*git]->GetString("drainstatus").c_str()));

                if ((drainstatus == eos::common::FileSystem::kDraining) ||
                    (drainstatus == eos::common::FileSystem::kDrainStalling)) {
                  // If any group filesystem is draining, all the others have
                  // to enable the pull for draining!
                  setactive = true;
                }
              }
            }

            for (auto git = FsView::gFsView.mGroupView[groupname]->begin();
                 git != FsView::gFsView.mGroupView[groupname]->end(); git++) {
              fs = FsView::gFsView.mIdView[*git];

              if (fs) {
                if (setactive) {
                  if (fs->GetString("stat.drainer") != "on") {
                    fs->SetString("stat.drainer", "on");
                  }
                } else {
                  if (fs->GetString("stat.drainer") != "off") {
                    fs->SetString("stat.drainer", "off");
                  }
                }
              }
            }
          }
        }

        if (status == "off") {
          // Disable all draining in this group
          FileSystem* fs = 0;

          if (FsView::gFsView.mGroupView.count(groupname)) {
            for (auto git = FsView::gFsView.mGroupView[groupname]->begin();
                 git != FsView::gFsView.mGroupView[groupname]->end(); git++) {
              fs = FsView::gFsView.mIdView[*git];

              if (fs) {
                fs->SetString("stat.drainer", "off");
              }
            }
          }
        }
      }
    }
  } else {
    retc = EPERM;
    stdErr = "error: you have to take role 'root' to execute this command";
  }

  reply.set_std_err(stdErr);
  reply.set_retc(retc);
  return SFS_OK; //TOCK is this needed?
}