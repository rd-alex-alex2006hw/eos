//------------------------------------------------------------------------------
// File: Messaging.cc
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

#include "XrdOuc/XrdOucEnv.hh"
#include "authz/XrdCapability.hh"
#include "fst/storage/Storage.hh"
#include "fst/Messaging.hh"
#include "fst/Deletion.hh"
#include "fst/Verify.hh"
#include "fst/ImportScan.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/FmdDbMap.hh"
#include "common/ShellCmd.hh"

EOSFSTNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Listen for incoming messages
//------------------------------------------------------------------------------
void
Messaging::Listen(ThreadAssistant& assistant) noexcept
{
  std::unique_ptr<XrdMqMessage> new_msg;

  while (!assistant.terminationRequested()) {
    new_msg.reset(XrdMqMessaging::gMessageClient.RecvMessage(&assistant));

    if (new_msg) {
      Process(new_msg.get());
    } else {
      assistant.wait_for(std::chrono::seconds(2));
    }
  }
}

//------------------------------------------------------------------------------
// Process incomming messages
//------------------------------------------------------------------------------
void
Messaging::Process(XrdMqMessage* newmessage)
{
  XrdOucString saction = newmessage->GetBody();
  XrdOucEnv action(saction.c_str());
  XrdOucString cmd = action.Get("mgm.cmd");
  XrdOucString subcmd = action.Get("mgm.subcmd");

  // Shared object communication point
  if (mSom) {
    XrdOucString error = "";
    bool result = mSom->ParseEnvMessage(newmessage, error);

    if (!result) {
      if (error != "no subject in message body") {
        eos_info("%s", error.c_str());
      } else {
        eos_debug("%s", error.c_str());
      }
    } else {
      return;
    }
  }

  if (cmd == "debug") {
    gOFS.SetDebug(action);
  }

  if (cmd == "register") {
    eos_notice("registering filesystems");
    XrdOucString manager = action.Get("mgm.manager");
    XrdOucString path2register = action.Get("mgm.path2register");
    XrdOucString space2register = action.Get("mgm.space2register");
    XrdOucString forceflag = action.Get("mgm.force");
    XrdOucString rootflag = action.Get("mgm.root");

    if (path2register.length() && space2register.length()) {
      XrdOucString sysline = "eosfstregister";

      if (rootflag == "true") {
        sysline += " -r ";
      }

      if (forceflag == "true") {
        sysline += " --force ";
      }

      sysline += manager;
      sysline += " ";
      sysline += path2register;
      sysline += " ";
      sysline += space2register;
      sysline += " >& /tmp/eosfstregister.out &";
      eos_notice("launched %s", sysline.c_str());
      eos::common::ShellCmd registercmd(sysline.c_str());
      eos::common::cmd_status rc = registercmd.wait(60);

      if (rc.exit_code) {
        eos_notice("cmd '%s' failed with rc=%d", sysline.c_str(), rc.exit_code);
      }
    }
  }

  if (cmd == "rtlog") {
    gOFS.SendRtLog(newmessage);
  }

  if (cmd == "fsck") {
    gOFS.SendFsck(newmessage);
  }

  if (cmd == "drop") {
    eos_info("drop");
    XrdOucEnv* capOpaque = NULL;
    int caprc = 0;

    if ((caprc = gCapabilityEngine.Extract(&action, capOpaque))) {
      // no capability - go away!
      if (capOpaque) {
        delete capOpaque;
      }

      eos_err("Cannot extract capability for deletion - errno=%d", caprc);
    } else {
      int envlen = 0;
      eos_debug("opaque is %s", capOpaque->Env(envlen));
      std::unique_ptr<Deletion> new_del;
      new_del.reset(Deletion::Create(capOpaque));
      delete capOpaque;

      if (new_del) {
        gOFS.Storage->AddDeletion(std::move(new_del));
      } else {
        eos_err("Cannot create a deletion entry - illegal opaque information");
      }
    }
  }

  if (cmd == "verify") {
    eos_info("verify");
    XrdOucEnv* capOpaque = &action;
    int envlen = 0;
    eos_debug("opaque is %s", capOpaque->Env(envlen));
    Verify* new_verify = Verify::Create(capOpaque);

    if (new_verify) {
      gOFS.Storage->PushVerification(new_verify);
    } else {
      eos_err("Cannot create a verify entry - illegal opaque information");
    }
  }

  if (cmd == "importscan") {
    eos_info("importscan");
    XrdOucEnv* capOpaque = &action;
    int envlen = 0;
    eos_debug("opaque is %s", capOpaque->Env(envlen));
    ImportScan* new_importScan = ImportScan::Create(capOpaque);

    if (new_importScan) {
      gOFS.Storage->PushImportScan(new_importScan);
    } else {
      eos_err("Cannot create an importScan entry - illegal opaque information");
    }
  }

  if (cmd == "resync") {
    eos::common::FileSystem::fsid_t fsid = (action.Get("mgm.fsid") ? strtoul(
        action.Get("mgm.fsid"), 0, 10) : 0);
    eos::common::FileId::fileid_t fid = (action.Get("mgm.fid") ? strtoull(
                                           action.Get("mgm.fid"), 0, 10) : 0);

    if ((!fsid)) {
      eos_err("dropping resync fsid=%lu fid=%llu", (unsigned long) fsid,
              (unsigned long long) fid);
    } else {
      if (!fid) {
        eos_warning("deleting fmd for fsid=%lu fid=%llu", (unsigned long) fsid,
                    (unsigned long long) fid);
        gFmdDbMapHandler.LocalDeleteFmd(fid, fsid);
      } else {
        FmdHelper* fMd = 0;
        fMd = gFmdDbMapHandler.LocalGetFmd(fid, fsid, 0, 0, 0, 0, true);

        if (fMd) {
          // force a resync of meta data from the MGM
          // e.g. store in the WrittenFilesQueue to have it done asynchronous
          gOFS.WrittenFilesQueueMutex.Lock();
          gOFS.WrittenFilesQueue.push(fMd->mProtoFmd);
          gOFS.WrittenFilesQueueMutex.UnLock();
          delete fMd;
        }
      }
    }
  }
}

EOSFSTNAMESPACE_END
