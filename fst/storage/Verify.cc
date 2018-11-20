// ----------------------------------------------------------------------
// File: Verify.cc
// Author: Andreas-Joachim Peters - CERN
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

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

/*----------------------------------------------------------------------------*/
#include "fst/storage/Storage.hh"
#include "fst/storage/FileSystem.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/XrdFstOss.hh"
#include "fst/io/FileIoPluginCommon.hh"
#include "fst/Verify.hh"
#include "fst/checksum/ChecksumPlugins.hh"
#include "fst/FmdDbMap.hh"
#include "common/Path.hh"
#include "common/FsFilePath.hh"
/*----------------------------------------------------------------------------*/

extern eos::fst::XrdFstOss* XrdOfsOss;

EOSFSTNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
void
Storage::Verify()
{
  std::map<uint64_t, time_t> open_w_out;

  // Thread that verifies stored files
  while (1) {
    mVerifyMutex.Lock();

    if (!mVerifications.size()) {
      mVerifyMutex.UnLock();
      sleep(1);
      continue;
    }

    eos::fst::Verify* verifyfile = mVerifications.front();

    if (verifyfile) {
      eos_static_debug("got %llu\n", (unsigned long long) verifyfile);
      mVerifications.pop();
      mRunningVerify = verifyfile;
      {
        XrdSysMutexHelper wLock(gOFS.OpenFidMutex);

        if (gOFS.WOpenFid[verifyfile->fsId].count(verifyfile->fId)) {
          if (gOFS.WOpenFid[verifyfile->fsId][verifyfile->fId] > 0) {
            time_t now = time(NULL);

            if (open_w_out[verifyfile->fId] < now) {
              eos_static_warning("file is currently opened for writing id=%x on "
                                 "fs=%u - skipping verification", verifyfile->fId,
                                 verifyfile->fsId);
              // Spit this message out only once pre minute
              open_w_out[verifyfile->fId] = now + 60;
            }

            mVerifications.push(verifyfile);
            mVerifyMutex.UnLock();
            continue;
          }
        }
      }
    } else {
      eos_static_debug("got nothing");
      mVerifyMutex.UnLock();
      mRunningVerify = 0;
      continue;
    }

    mVerifyMutex.UnLock();
    eos_static_debug("verifying File Id=%x on Fs=%u", verifyfile->fId,
                     verifyfile->fsId);
    // verify the file
    XrdOucString hexfid = "";
    eos::common::FileId::Fid2Hex(verifyfile->fId, hexfid);
    XrdOucErrInfo error;
    XrdOucString fstPath = "";

    if (verifyfile->lPath.length()) {

      eos::common::FsFilePath::BuildPhysicalPath(verifyfile->localPrefix.c_str(),
                                                 verifyfile->lPath.c_str(),
                                                 fstPath);
    } else {
      eos::common::FileId::FidPrefix2FullPath(hexfid.c_str(),
                                              verifyfile->localPrefix.c_str(),
                                              fstPath);
    }

    {
      FmdHelper* fMd = 0;
      fMd = gFmdDbMapHandler.LocalGetFmd(verifyfile->fId, verifyfile->fsId, 0, 0, 0,
                                         0,
                                         true);

      if (fMd) {
        // force a resync of meta data from the MGM
        // e.g. store in the WrittenFilesQueue to have it done asynchronous
        gOFS.WrittenFilesQueueMutex.Lock();
        gOFS.WrittenFilesQueue.push(fMd->mProtoFmd);
        gOFS.WrittenFilesQueueMutex.UnLock();
        delete fMd;
      }
    }

    // get current size on disk
    struct stat statinfo;
    int open_rc = -1;

    std::string s3credentials = "";
    std::string sFstPath = fstPath.c_str();
    if ((fstPath.beginswith("s3:")) || (fstPath.beginswith("s3s:"))) {
      s3credentials = gOFS.Storage->
          GetFileSystemById(verifyfile->fsId)->GetString("s3credentials");
      sFstPath += "?s3credentials=" + s3credentials;
    }

    std::unique_ptr<FileIo> io (eos::fst::FileIoPluginHelper::GetIoObject(
                                  sFstPath.c_str()));

    if (!io || (open_rc = io->fileOpen(0, 0)) || io->fileStat(&statinfo)) {
      eos_static_err("unable to verify file id=%x on fs=%u path=%s - stat on "
                     "local disk failed", verifyfile->fId, verifyfile->fsId,
                     fstPath.c_str());
      // If there is no file, we should not commit anything to the MGM
      verifyfile->commitSize = 0;
      verifyfile->commitChecksum = 0;
      statinfo.st_size = 0; // indicates the missing file - not perfect though
    }

    // even if the stat failed, we run this code to tag the file as is ...
    // attach meta data
    FmdHelper* fMd = 0;
    fMd = gFmdDbMapHandler.LocalGetFmd(verifyfile->fId, verifyfile->fsId, 0, 0, 0,
                                       verifyfile->commitFmd, true);
    bool localUpdate = false;

    if (!fMd) {
      eos_static_err("unable to verify id=%x on fs=%u path=%s - no local MD stored",
                     verifyfile->fId, verifyfile->fsId, fstPath.c_str());
    } else {
      if ((fMd->mProtoFmd.size() != (unsigned long long) statinfo.st_size)  ||
          (fMd->mProtoFmd.disksize() != (unsigned long long) statinfo.st_size)) {
        eos_static_err("updating file size: path=%s fid=%s fs value %llu - changelog value %llu",
                       verifyfile->path.c_str(), hexfid.c_str(), statinfo.st_size,
                       fMd->mProtoFmd.size());
        fMd->mProtoFmd.set_disksize(statinfo.st_size);
        localUpdate = true;
      }

      if (fMd->mProtoFmd.lid() != verifyfile->lId) {
        eos_static_err("updating layout id: path=%s fid=%s central value %u - changelog value %u",
                       verifyfile->path.c_str(), hexfid.c_str(), verifyfile->lId,
                       fMd->mProtoFmd.lid());
        localUpdate = true;
      }

      if (fMd->mProtoFmd.cid() != verifyfile->cId) {
        eos_static_err("updating container: path=%s fid=%s central value %llu - changelog value %llu",
                       verifyfile->path.c_str(), hexfid.c_str(), verifyfile->cId,
                       fMd->mProtoFmd.cid());
        localUpdate = true;
      }

      // update size
      fMd->mProtoFmd.set_size(statinfo.st_size);
      fMd->mProtoFmd.set_lid(verifyfile->lId);
      fMd->mProtoFmd.set_cid(verifyfile->cId);
      CheckSum* checksummer = ChecksumPlugins::GetChecksumObject(
                                fMd->mProtoFmd.lid());
      unsigned long long scansize = 0;
      float scantime = 0; // is ms
      eos::fst::CheckSum::ReadCallBack::callback_data_t cbd;
      cbd.caller = (void*) io.get();
      eos::fst::CheckSum::ReadCallBack cb(eos::fst::XrdFstOfsFile::FileIoReadCB, cbd);

      if ((checksummer) && verifyfile->computeChecksum &&
          (!checksummer->ScanFile(cb, scansize, scantime, verifyfile->verifyRate))) {
        eos_static_crit("cannot scan file to recalculate the checksum id=%llu on fs=%u path=%s",
                        verifyfile->fId, verifyfile->fsId, fstPath.c_str());
      } else {
        XrdOucString sizestring;

        if (checksummer && verifyfile->computeChecksum) {
          eos_static_info("rescanned checksum - size=%s time=%.02fms rate=%.02f "
                          "MB/s limit=%d MB/s", eos::common::StringConversion::GetReadableSizeString(
                            sizestring, scansize, "B"),
                          scantime, 1.0 * scansize / 1000 / (scantime ? scantime : 99999999999999LL),
                          verifyfile->verifyRate);
        }

        if (checksummer && verifyfile->computeChecksum) {
          int checksumlen = 0;
          checksummer->GetBinChecksum(checksumlen);
          bool cxError = false;
          std::string computedchecksum = checksummer->GetHexChecksum();

          if (fMd->mProtoFmd.checksum() != computedchecksum) {
            cxError = true;
          }

          // commit the disk checksum in case of differences between the in-memory value
          if (fMd->mProtoFmd.diskchecksum() != computedchecksum) {
            cxError = true;
            localUpdate = true;
          }

          if (cxError) {
            eos_static_err("checksum invalid   : path=%s fid=%s checksum=%s stored-checksum=%s",
                           verifyfile->path.c_str(), hexfid.c_str(), checksummer->GetHexChecksum(),
                           fMd->mProtoFmd.checksum().c_str());
            fMd->mProtoFmd.set_checksum(computedchecksum);
            fMd->mProtoFmd.set_diskchecksum(computedchecksum);
            fMd->mProtoFmd.set_disksize(fMd->mProtoFmd.size());

            if (verifyfile->commitSize) {
              fMd->mProtoFmd.set_mgmsize(fMd->mProtoFmd.size());
            }

            if (verifyfile->commitChecksum) {
              fMd->mProtoFmd.set_mgmchecksum(computedchecksum);
              fMd->mProtoFmd.set_blockcxerror(0);
              fMd->mProtoFmd.set_filecxerror(0);
            }

            localUpdate = true;
          } else {
            eos_static_info("checksum OK        : path=%s fid=%s checksum=%s",
                            verifyfile->path.c_str(), hexfid.c_str(),
                            checksummer->GetHexChecksum());

            // Reset error flags if needed
            if (fMd->mProtoFmd.blockcxerror() || fMd->mProtoFmd.filecxerror()) {
              fMd->mProtoFmd.set_blockcxerror(0);
              fMd->mProtoFmd.set_filecxerror(0);
              localUpdate = true;
            }
          }

          // Update the extended attributes
          if (io) {
            (void)io->attrSet("user.eos.checksum", checksummer->GetBinChecksum(checksumlen),
                              checksumlen);
            (void)io->attrSet("user.eos.checksumtype", checksummer->GetName(),
                              strlen(checksummer->GetName()));
            (void)io->attrSet("user.eos.filecxerror", "0", 1);
            (void)io->attrSet("user.eos.blockcxerror", "0");
          }
        }

        eos::common::Path cPath(verifyfile->path.c_str());

        // commit local
        if (localUpdate && (!gFmdDbMapHandler.Commit(fMd))) {
          eos_static_err("unable to verify file id=%llu on fs=%u path=%s - commit "
                         "to local MD storage failed", verifyfile->fId,
                         verifyfile->fsId, fstPath.c_str());
        } else {
          if (localUpdate) {
            eos_static_info("committed verified meta data locally id=%llu on fs=%u path=%s",
                            verifyfile->fId, verifyfile->fsId, fstPath.c_str());
          }

          // commit to central mgm cache, only if commitSize or commitChecksum is set
          XrdOucString capOpaqueFile = "";
          XrdOucString mTimeString = "";
          capOpaqueFile += "/?";
          capOpaqueFile += "&mgm.pcmd=commit";
          capOpaqueFile += "&mgm.verify.checksum=1";
          capOpaqueFile += "&mgm.size=";
          char filesize[1024];
          sprintf(filesize, "%" PRIu64 "", fMd->mProtoFmd.size());
          capOpaqueFile += filesize;
          capOpaqueFile += "&mgm.fid=";
          capOpaqueFile += hexfid;
          capOpaqueFile += "&mgm.path=";
          capOpaqueFile += verifyfile->path.c_str();

          if (checksummer && verifyfile->computeChecksum) {
            capOpaqueFile += "&mgm.checksum=";
            capOpaqueFile += checksummer->GetHexChecksum();

            if (verifyfile->commitChecksum) {
              capOpaqueFile += "&mgm.commit.checksum=1";
            }
          }

          if (verifyfile->commitSize) {
            capOpaqueFile += "&mgm.commit.size=1";
          }

          capOpaqueFile += "&mgm.mtime=";
          capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString,
                           (unsigned long long) fMd->mProtoFmd.mtime());
          capOpaqueFile += "&mgm.mtime_ns=";
          capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString,
                           (unsigned long long) fMd->mProtoFmd.mtime_ns());
          capOpaqueFile += "&mgm.add.fsid=";
          capOpaqueFile += (int) fMd->mProtoFmd.fsid();

          if (verifyfile->commitSize || verifyfile->commitChecksum) {
            if (localUpdate) {
              eos_static_info("committed verified meta data centrally id=%llu on fs=%u path=%s",
                              verifyfile->fId, verifyfile->fsId, fstPath.c_str());
            }

            int rc = gOFS.CallManager(&error, verifyfile->path.c_str(), 0, capOpaqueFile);

            if (rc) {
              eos_static_err("unable to verify file id=%s fs=%u at manager %s",
                             hexfid.c_str(), verifyfile->fsId, verifyfile->managerId.c_str());
            }
          }
        }
      }

      if (checksummer) {
        delete checksummer;
      }

      if (fMd) {
        delete fMd;
      }
    }

    if (!open_rc) {
      io->fileClose();
    }

    mRunningVerify = 0;

    if (verifyfile) {
      delete verifyfile;
    }
  }
}

EOSFSTNAMESPACE_END
