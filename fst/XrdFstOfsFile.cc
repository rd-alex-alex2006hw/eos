//------------------------------------------------------------------------------
// File: XrdFstOfsFile.cc
// Author: Elvin-Alin Sindrilaru / Andreas-Joachim Peters - CERN
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
 * but WITHOUT ANY WARRANTY; without even the implied waDon'trranty of  *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/
#define __STDC_FORMAT_MACROS
#include <cinttypes>
#include "common/Constants.hh"
#include "common/Path.hh"
#include "common/FileFsPath.hh"
#include "common/http/OwnCloud.hh"
#include "common/StringTokenizer.hh"
#include "common/SecEntity.hh"
#include "common/xrootd-ssi-protobuf-interface/eos_cta/include/CtaFrontendApi.hpp"
#include "fst/FmdDbMap.hh"
#include "fst/XrdFstOfsFile.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/layout/Layout.hh"
#include "fst/layout/LayoutPlugin.hh"
#include "fst/checksum/ChecksumPlugins.hh"
#include "fst/storage/FileSystem.hh"
#include "authz/XrdCapability.hh"
#include "XrdOss/XrdOssApi.hh"
#include "fst/io/FileIoPluginCommon.hh"

extern XrdOssSys* XrdOfsOss;

EOSFSTNAMESPACE_BEGIN

const uint16_t XrdFstOfsFile::msDefaultTimeout = 300; // default timeout value

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
XrdFstOfsFile::XrdFstOfsFile(const char* user, int MonID) :
  XrdOfsFile(user, MonID), eos::common::LogId(),
  mOpenOpaque(nullptr), mCapOpaque(nullptr), mFstPath(""), mBookingSize(0),
  mTargetSize(0), mMinSize(0), mMaxSize(0), viaDelete(false),
  writeDelete(false), mRainSize(0), mNsPath(""), mLocalPrefix(""),
  mRedirectManager(""), mSecString(""), mTpcKey(""), mEtag(""), mFileId(0),
  mFsId(0), mLid(0), mCid(0), mForcedMtime(1), mForcedMtime_ms(0), mFusex(false),
  mFusexIsUnlinked(false),
  closed(false), opened(false), mHasWrite(false), hasWriteError(false),
  hasReadError(false), isRW(false), mIsTpcDst(false), mIsDevNull(false),
  isCreation(false), isReplication(false), mIsInjection(false),
  mRainReconstruct(false), deleteOnClose(false), repairOnClose(false),
  commitReconstruction(false), mEventOnClose(false), mEventWorkflow(""),
  mSyncEventOnClose(false),
  mIsOCchunk(false), writeErrorFlag(false), mTpcFlag(kTpcNone),
  fMd(nullptr), mCheckSum(nullptr), layOut(nullptr), maxOffsetWritten(0),
  openSize(0), closeSize(0),
  mTpcThreadStatus(EINVAL), mTpcState(kTpcIdle), mTpcRetc(0)
{
  rBytes = wBytes = sFwdBytes = sBwdBytes = sXlFwdBytes
                                = sXlBwdBytes = rOffset = wOffset = 0;
  rTime.tv_sec = lrTime.tv_sec = rvTime.tv_sec = lrvTime.tv_sec = 0;
  rTime.tv_usec = lrTime.tv_usec = rvTime.tv_usec = lrvTime.tv_usec = 0;
  wTime.tv_sec = lwTime.tv_sec = cTime.tv_sec = 0;
  wTime.tv_usec = lwTime.tv_usec = cTime.tv_usec = 0;
  rCalls = wCalls = nFwdSeeks = nBwdSeeks = nXlFwdSeeks = nXlBwdSeeks = 0;
  closeTime.tv_sec = closeTime.tv_usec = 0;
  openTime.tv_sec = openTime.tv_usec = 0;
  tz.tz_dsttime = tz.tz_minuteswest = 0;
  mTimeout = getenv("EOS_FST_STREAM_TIMEOUT") ?
             strtoul(getenv("EOS_FST_STREAM_TIMEOUT"), 0, 10) : msDefaultTimeout;
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
XrdFstOfsFile::~XrdFstOfsFile()
{
  viaDelete = true;

  if (!closed) {
    close();
  }

  // Unmap the MD record
  if (fMd) {
    delete fMd;
    fMd = 0;
  }

  if (layOut) {
    delete layOut;
    layOut = 0;
  }
}

//------------------------------------------------------------------------------
// Open - - low level ofs method which is called from one of the layout plugins
//------------------------------------------------------------------------------
int
XrdFstOfsFile::openofs(const char* path,
                       XrdSfsFileOpenMode open_mode,
                       mode_t create_mode,
                       const XrdSecEntity* client,
                       const char* opaque)
{
  int retc = 0;

  while ((retc = XrdOfsFile::open(path, open_mode, create_mode, client,
                                  opaque)) > 0) {
    eos_static_notice("msg\"xrootd-lock-table busy - snoozing & retry\" "
                      "delay=%d errno=%d", retc, errno);
    std::this_thread::sleep_for(std::chrono::seconds(retc));
  }

  return retc;
}

//------------------------------------------------------------------------------
// Drop all replicas from the MGM
//------------------------------------------------------------------------------
int
XrdFstOfsFile::dropall(eos::common::FileId::fileid_t fileid, std::string path,
                       std::string manager)
{
  // If we committed the replica and an error happened remote, we have
  // to unlink it again
  XrdOucString hexstring = "";
  eos::common::FileId::Fid2Hex(fileid, hexstring);
  XrdOucErrInfo error;
  XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
  XrdOucString OpaqueString = "";
  OpaqueString += "&mgm.fid=";
  OpaqueString += hexstring;
  OpaqueString += "&mgm.fsid=anyway";
  OpaqueString += "&mgm.dropall=1";
  XrdOucEnv Opaque(OpaqueString.c_str());
  capOpaqueString += OpaqueString;
  // Delete the replica in the MGM
  int rcode = gOFS.CallManager(&error, path.c_str(), manager.c_str(),
                               capOpaqueString);

  if (rcode && (error.getErrInfo() != EIDRM)) {
    eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                hexstring.c_str(), fileid, manager.c_str());
  }

  eos_info("info=\"removing on manager\" manager=%s fid=%llu fsid= drop-allrc=%d",
           manager.c_str(), (unsigned long long) fileid, rcode);
  return rcode;
}

//------------------------------------------------------------------------------
// Open file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::open(const char* path, XrdSfsFileOpenMode open_mode,
                    mode_t create_mode, const XrdSecEntity* client,
                    const char* opaque)
{
  EPNAME("open");
  const char* tident = error.getErrUser();
  SetLogId(ExtractLogId(opaque).c_str(), client, tident);
  tIdent = error.getErrUser();
  char* val = 0;
  isRW = false;
  int retc = SFS_OK;
  int envlen = 0;
  mNsPath = path;
  gettimeofday(&openTime, &tz);
  bool hasCreationMode = (open_mode & SFS_O_CREAT);
  bool isRepairRead = false;
  // Mask some opaque parameters to shorten the logging
  XrdOucString maskOpaque = opaque ? opaque : "";
  eos::common::StringConversion::MaskTag(maskOpaque, "cap.sym");
  eos::common::StringConversion::MaskTag(maskOpaque, "cap.msg");
  eos::common::StringConversion::MaskTag(maskOpaque, "authz");
  eos_info("path=%s info=%s open_mode=%x", mNsPath.c_str(),
           maskOpaque.c_str(), open_mode);
  // Process and filter open opaque information
  std::string in_opaque = (opaque ? opaque : "");
  in_opaque += "&mgm.path=";
  in_opaque += mNsPath.c_str();
  //----------------------------------------------------------------------------
  // @todo (esindril): This should be dropped after Sept 2018 since it's
  // just a temporary fix for an issue on the eos fuse.
  //----------------------------------------------------------------------------
  FilterTagsInPlace(in_opaque, {"xrdcl.secuid", "xrdcl.secgid"});
  // Process TPC information - after this mOpenOpaque and mCapOpaque will be
  // properly populated and decrypted.
  int tpc_retc = ProcessTpcOpaque(in_opaque, client);

  if (tpc_retc == SFS_ERROR) {
    eos_err("%s", "msg=\"failed while processing TPC/open opaque\"");
    return SFS_ERROR;
  } else if (tpc_retc >= SFS_STALL) {
    return tpc_retc; // this is stall time in seconds
  }

  if (ProcessOpenOpaque()) {
    eos_err("%s", "msg=\"failed while processing open opaque info\"");
    return SFS_ERROR;
  }

  eos::common::Mapping::VirtualIdentity vid;

  if (ProcessCapOpaque(isRepairRead, vid)) {
    eos_err("%s", "msg=\"failed while processing cap opaque info\"");
    return SFS_ERROR;
  }

  if (ProcessMixedOpaque()) {
    eos_err("%s", "msg=\"failed while processing mixed opaque info\"");
    return SFS_ERROR;
  }

  // For RAIN layouts if the opaque information contains the tag mgm.rain.store=1
  // the corrupted files are recovered back on disk. There is no other way to make
  // the distinction between an open for write and open for recovery
  if (mCapOpaque && (val = mCapOpaque->Get("mgm.rain.store"))) {
    if (strncmp(val, "1", 1) == 0) {
      eos_info("%s", "msg=\"enabling RAIN store recovery\"");
      open_mode = SFS_O_RDWR;
      mRainReconstruct = true;
      mHasWrite = true;

      // Get logical file size
      if ((val = mCapOpaque->Get("mgm.rain.size"))) {
        try {
          mRainSize = std::stoull(val);
        } catch (...) {
          // ignore
        }
      } else {
        eos_warning("%s", "msg=\"unknown RAIN file size during reconstruction\"");
      }
    }
  }

  if ((mRainReconstruct && (mTpcFlag == kTpcSrcCanDo)) ||
      (mTpcFlag == kTpcSrcSetup)) {
    eos_info("kTpcSrcSetup return SFS_OK");
    return SFS_OK;
  }

  if ((open_mode & (SFS_O_WRONLY | SFS_O_RDWR | SFS_O_CREAT | SFS_O_TRUNC))) {
    isRW = true;
  }

  // File is supposed to act as a sink, used for draining
  if (mNsPath == "/replicate:0") {
    if (isRW) {
      eos_info("%s", "msg=\"file fid=0 acting as a sink i.e. /dev/null\"");
      mIsDevNull = true;
      return SFS_OK;
    } else {
      eos_info("%s", "msg=\"sink file i.e. /dev/null can only be opened for RW\"");
      return gOFS.Emsg(epname, error, EIO, "open - sink file can only be "
                       "opened RW mode", mNsPath.c_str());
    }
  }

  eos_info("ns_path=%s", mNsPath.c_str());

  if (mNsPath.beginswith("/replicate:")) {
    bool isopenforwrite = false;
    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WOpenFid[mFsId].count(mFileId)) {
        if (gOFS.WOpenFid[mFsId][mFileId] > 0) {
          isopenforwrite = true;
        }
      }
    }

    if (isopenforwrite) {
      eos_err("forbid to open replica - file %s is opened in RW mode",
              mNsPath.c_str());
      return gOFS.Emsg(epname, error, ENOENT, "open - cannot replicate: file "
                       "is opened in RW mode", mNsPath.c_str());
    }

    isReplication = true;
  }

  // Check if this is an open for HTTP
  if ((!isRW) && ((std::string(client->tident) == "http"))) {
    bool isopenforwrite = false;
    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WOpenFid[mFsId].count(mFileId)) {
        if (gOFS.WOpenFid[mFsId][mFileId] > 0) {
          isopenforwrite = true;
        }
      }
    }

    if (isopenforwrite) {
      eos_err("forbid to open replica for synchronization - file %s is opened "
              "in RW mode", mNsPath.c_str());
      return gOFS.Emsg(epname, error, ETXTBSY, "open - cannot synchronize this "
                       "file: file is opened in RW mode", mNsPath.c_str());
    }
  }

  eos_info("fstpath=%s", mFstPath.c_str());
  // Get the layout object
  layOut = eos::fst::LayoutPlugin::GetLayoutObject
           (this, mLid, client, &error, mFstPath.c_str(), msDefaultTimeout,
            mRainReconstruct);

  if (!layOut) {
    int envlen;
    eos_err("unable to handle layout for %s", mCapOpaque->Env(envlen));
    delete fMd;
    return gOFS.Emsg(epname, error, EINVAL, "open - illegal layout specified ",
                     mCapOpaque->Env(envlen));
  }

  layOut->SetLogId(logId, client, tident);
  errno = 0;

  if ((retc = layOut->GetFileIo()->fileExists())) {
    // We have to distinguish if an Exists call fails or return ENOENT, otherwise
    // we might trigger an automatic clean-up of a file !!!
    if (errno != ENOENT) {
      delete fMd;
      return gOFS.Emsg(epname, error, EIO, "open - unable to check for existance"
                       " of file ", mCapOpaque->Env(envlen));
    }

    if (isRW || (mCapOpaque->Get("mgm.zerosize"))) {
      // File does not exist, keep the create flag for writers and readers with 0-size at MGM
      isCreation = true;
      openSize = 0;
      // Used to indicate if a file was written in the meanwhile by someone else
      updateStat.st_mtime = 0;
      open_mode |= SFS_O_CREAT;
      create_mode |= SFS_O_MKPTH;
      eos_debug("adding creation flag because of %d %d", retc, errno);
    } else {
      // The open will fail but the client will get a recoverable error,
      // therefore it will try to read again from the other replicas.
      eos_warning("open for read, local file does not exists");
      return gOFS.Emsg(epname, error, ENOENT, "open, file does not exist ",
                       mCapOpaque->Env(envlen));
    }
  } else {
    eos_debug("removing creation flag because of %d %d", retc, errno);

    // Remove the creat flag
    if (open_mode & SFS_O_CREAT) {
      open_mode -= SFS_O_CREAT;
    }
  }

  // Capability access distinction
  if (isRW) {
    if (isCreation) {
      if (!mCapOpaque->Get("mgm.access")
          || ((strcmp(mCapOpaque->Get("mgm.access"), "create")) &&
              (strcmp(mCapOpaque->Get("mgm.access"), "write")) &&
              (strcmp(mCapOpaque->Get("mgm.access"), "update")))) {
        return gOFS.Emsg(epname, error, EPERM, "open - capability does not "
                         "allow to create/write/update this file", path);
      }
    } else {
      if (!mCapOpaque->Get("mgm.access")
          || ((strcmp(mCapOpaque->Get("mgm.access"), "create")) &&
              (strcmp(mCapOpaque->Get("mgm.access"), "write")) &&
              (strcmp(mCapOpaque->Get("mgm.access"), "update")))) {
        return gOFS.Emsg(epname, error, EPERM, "open - capability does not "
                         "allow to update/write/create this file", path);
      }
    }
  } else {
    if (!mCapOpaque->Get("mgm.access")
        || ((strcmp(mCapOpaque->Get("mgm.access"), "read")) &&
            (strcmp(mCapOpaque->Get("mgm.access"), "create")) &&
            (strcmp(mCapOpaque->Get("mgm.access"), "write")) &&
            (strcmp(mCapOpaque->Get("mgm.access"), "update")))) {
      return gOFS.Emsg(epname, error, EPERM, "open - capability does not allow "
                       "to read this file", path);
    }
  }

  // Bookingsize is only needed for file creation
  if (isRW && isCreation) {
    const char* sbookingsize = 0;
    const char* stargetsize = 0;

    if (!(sbookingsize = mCapOpaque->Get("mgm.bookingsize"))) {
      return gOFS.Emsg(epname, error, EINVAL, "open - no booking size in capability",
                       mNsPath.c_str());
    } else {
      mBookingSize = strtoull(mCapOpaque->Get("mgm.bookingsize"), 0, 10);

      if (errno == ERANGE) {
        eos_err("invalid bookingsize in capability bookingsize=%s", sbookingsize);
        return gOFS.Emsg(epname, error, EINVAL,
                         "open - invalid bookingsize in capability", mNsPath.c_str());
      }
    }

    if ((stargetsize = mCapOpaque->Get("mgm.targetsize"))) {
      mTargetSize = strtoull(mCapOpaque->Get("mgm.targetsize"), 0, 10);

      if (errno == ERANGE) {
        eos_err("invalid targetsize in capability targetsize=%s", stargetsize);
        return gOFS.Emsg(epname, error, EINVAL,
                         "open - invalid targetsize in capability", mNsPath.c_str());
      }
    }
  }

  // Check if the booking size violates the min/max-size criteria
  if (mBookingSize && mMaxSize) {
    if (mBookingSize > mMaxSize) {
      eos_err("invalid bookingsize specified - violates maximum file size criteria");
      return gOFS.Emsg(epname, error, ENOSPC, "open - bookingsize violates "
                       "maximum allowed filesize", mNsPath.c_str());
    }
  }

  if (mBookingSize && mMinSize) {
    if (mBookingSize < mMinSize) {
      eos_err("invalid bookingsize specified - violates minimum file size criteria");
      return gOFS.Emsg(epname, error, ENOSPC, "open - bookingsize violates "
                       "minimum allowed filesize", mNsPath.c_str());
    }
  }

  eos_info("fstpath=%s", mFstPath.c_str());
  fMd = gFmdDbMapHandler.LocalGetFmd(mFileId, mFsId, vid.uid, vid.gid, mLid, isRW,
                                     isRepairRead);

  if ((!fMd) || gOFS.Simulate_FMD_open_error) {
    if (!gOFS.Simulate_FMD_open_error) {
      // Get the layout object
      if (gFmdDbMapHandler.ResyncMgm(mFsId, mFileId, mRedirectManager.c_str())) {
        eos_info("msg=\"resync ok\" fsid=%lu fid=%llx", (unsigned long) mFsId, mFileId);
        fMd = gFmdDbMapHandler.LocalGetFmd(mFileId, mFsId, vid.uid, vid.gid, mLid,
                                           isRW);
      } else {
        eos_err("msg=\"resync failed\" fsid=%lu fid=%llx", (unsigned long) mFsId,
                mFileId);
      }
    }

    if ((!fMd) || gOFS.Simulate_FMD_open_error) {
      if ((!isRW) || (layOut->IsEntryServer() && (!isReplication))) {
        eos_crit("no fmd for fileid %llu on filesystem %lu", mFileId, mFsId);
        eos_warning("failed to get FMD record return recoverable error ENOENT(kXR_NotFound)");

        if (hasCreationMode) {
          // clean-up before re-bouncing
          dropall(mFileId, path, mRedirectManager.c_str());
        }

        // Return an error that can be recovered at the MGM
        return gOFS.Emsg(epname, error, ENOENT, "open - no FMD record found");
      } else {
        eos_crit("no fmd for fileid %llu on filesystem %d", mFileId, mFsId);
        return gOFS.Emsg(epname, error, ENOENT, "open - no FMD record found");
      }
    }
  }

  XrdOucString oss_opaque = "";
  oss_opaque += "&mgm.lid=";
  oss_opaque += std::to_string(mLid).c_str();
  oss_opaque += "&mgm.bookingsize=";
  oss_opaque += static_cast<int>(mBookingSize);
  // Open layout implementation
  eos_info("fstpath=%s open-mode=%x create-mode=%x layout-name=%s",
           mFstPath.c_str(), open_mode, create_mode, layOut->GetName());
  int rc = layOut->Open(open_mode, create_mode, oss_opaque.c_str());

  if (isReplication && !isCreation) {
    layOut->Stat(&updateStat);
  }

  if ((!rc) && isCreation && mBookingSize) {
    // check if the file system is full
    XrdSysMutexHelper lock(gOFS.Storage->mFsFullMapMutex);

    if (gOFS.Storage->mFsFullMap[mFsId]) {
      if (layOut->IsEntryServer() && (!isReplication)) {
        writeErrorFlag = kOfsDiskFullError;
        layOut->Remove();
        eos_warning("not enough space return recoverable error ENODEV(kXR_FSError)");

        if (hasCreationMode) {
          // clean-up before re-bouncing
          dropall(mFileId, path, mRedirectManager.c_str());
        }

        // Return an error that can be recovered at the MGM
        return gOFS.Emsg(epname, error, ENODEV, "open - not enough space");
      }

      writeErrorFlag = kOfsDiskFullError;
      return gOFS.Emsg("writeofs", error, ENOSPC,
                       "create file - disk space (headroom) exceeded fn=",
                       mCapOpaque ?
                       (mCapOpaque->Get("mgm.path") ? mCapOpaque->Get("mgm.path") : FName())
                       : FName());
    }

    rc = layOut->Fallocate(mBookingSize);

    if (rc) {
      eos_crit("file allocation gave return code %d errno=%d for allocation of size=%llu",
               rc, errno, mBookingSize);

      if (layOut->IsEntryServer() && (!isReplication)) {
        layOut->Remove();
        eos_warning("not enough space i.e file allocation failed, return "
                    "recoverable error ENODEV(kXR_FSError)");

        if (hasCreationMode) {
          // clean-up before re-bouncing
          dropall(mFileId, path, mRedirectManager.c_str());
        }

        // Return an error that can be recovered at the MGM
        return gOFS.Emsg(epname, error, ENODEV, "open - file allocation failed");
      } else {
        layOut->Remove();
        return gOFS.Emsg(epname, error, ENOSPC, "open - cannot allocate required space",
                         mNsPath.c_str());
      }
    }
  }

  eos_info("checksum_object=0x%llx entryserver=%d",
           (unsigned long long) mCheckSum.get(), layOut->IsEntryServer());

  if (!isCreation) {
    // Get the real size of the file, not the local stripe size!
    struct stat statinfo {};

    if ((retc = layOut->Stat(&statinfo))) {
      return gOFS.Emsg(epname, error, EIO,
                       "open - cannot stat layout to determine file size", mNsPath.c_str());
    }

    // We feed the layout size, not the physical on disk!
    eos_info("msg=\"layout size\": disk_size=%zu db_size= %llu",
             statinfo.st_size, fMd->mProtoFmd.size());

    if ((off_t) statinfo.st_size != (off_t) fMd->mProtoFmd.size()) {
      // In a RAID-like layout if the header is corrupted there is no way to know
      // the size of the initial file, therefore we take the value from the DB
      if (!mRainReconstruct) {
        openSize = fMd->mProtoFmd.size();
      } else {
        openSize = statinfo.st_size;
      }
    } else {
      openSize = statinfo.st_size;
    }

    // Preset with the last known checksum
    if (mCheckSum && isRW && !IsChunkedUpload()) {
      eos_info("msg=\"reset init\" file-xs=%s", fMd->mProtoFmd.checksum().c_str());
      mCheckSum->ResetInit(0, openSize, fMd->mProtoFmd.checksum().c_str());
    }
  }

  // If we are not the entry server for RAIN layouts we disable the checksum
  // object for write. If we read we don't check checksums at all since we
  // have block and parity checking.
  if (IsRainLayout(mLid) && ((!isRW) || (!layOut->IsEntryServer()))) {
    mCheckSum.reset(nullptr);
  }

  std::string filecxerror = "0";

  if (!rc) {
    // Set extended attributes
    std::unique_ptr<FileIo> io(FileIoPlugin::GetIoObject(
                                 layOut->GetLocalReplicaPath(), this));

    if (isRW) {
      // Set the eos lfn attribute
      if (mNsPath.beginswith("/replicate:") || mNsPath.beginswith("/fusex-open")) {
        if (mCapOpaque->Get("mgm.path")) {
          XrdOucString unsealedpath = mCapOpaque->Get("mgm.path");
          XrdOucString sealedpath = path;

          if (io->attrSet(std::string("user.eos.lfn"),
                          std::string(unsealedpath.c_str()))) {
            eos_err("unable to set extended attribute <eos.lfn> errno=%d", errno);
          }
        } else {
          eos_err("no lfn in replication capability");
        }
      } else {
        if (io->attrSet(std::string("user.eos.lfn"), std::string(mNsPath.c_str()))) {
          eos_err("unable to set extended attribute <eos.lfn> errno=%d", errno);
        }
      }

      // Set the eos creation time attribute
      if (mCapOpaque->Get("mgm.lpath") && mCapOpaque->Get("mgm.ctime")) {
        if (io->attrSet("user.eos.ctime", mCapOpaque->Get("mgm.ctime"))) {
          eos_err("unable to set extended attribute <eos.ctime> errno=%d", errno);
        }
      }
    }

    // Try to get error if the file has a scan error
    io->attrGet("user.eos.filecxerror", filecxerror);

    if ((!isRW) && (filecxerror == "1")) {
      if (eos::common::LayoutId::GetLayoutType(mLid) ==
          eos::common::LayoutId::kReplica) {
        eos_err("open of %s failed - replica has a checksum mismatch", mNsPath.c_str());
        return gOFS.Emsg(epname, error, EIO, "open - replica has a checksum mismatch",
                         mNsPath.c_str());
      }
    }
  }

  if (!rc) {
    opened = true;
    XrdSysMutexHelper scop_lock(gOFS.OpenFidMutex);

    if (isRW) {
      gOFS.WOpenFid[mFsId][mFileId]++;
    } else {
      gOFS.ROpenFid[mFsId][mFileId]++;
    }
  } else {
    // If we have local errors in open we don't disable the filesystem -
    // this is done by the Scrub thread if necessary!
    if (layOut->IsEntryServer() && (!isReplication)) {
      eos_warning("open error return recoverable error EIO(kXR_IOError)");

      // Clean-up before re-bouncing
      if (hasCreationMode) {
        dropall(mFileId, path, mRedirectManager.c_str());
      }
    }

    // Return an error that can be recovered at the MGM
    return gOFS.Emsg(epname, error, EIO, "open - failed open");
  }

  if (isRW) {
    if (!gOFS.Storage->OpenTransaction(mFsId, mFileId)) {
      eos_crit("cannot open transaction for fsid=%u fid=%llu", mFsId, mFileId);
    }
  }

  return rc;
}

//------------------------------------------------------------------------------
// Account for total read time
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddReadTime()
{
  unsigned long mus = (lrTime.tv_sec - cTime.tv_sec) * 1000000 +
                      (lrTime.tv_usec - cTime.tv_usec);
  rTime.tv_sec += (mus / 1000000);
  rTime.tv_usec += (mus % 1000000);
}

//------------------------------------------------------------------------------
// Account for total readv time
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddReadVTime()
{
  unsigned long mus = (lrvTime.tv_sec - cTime.tv_sec) * 1000000 +
                      (lrvTime.tv_usec - cTime.tv_usec);
  rvTime.tv_sec += (mus / 1000000);
  rvTime.tv_usec += (mus % 1000000);
}

//------------------------------------------------------------------------------
// Account for total write time
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddWriteTime()
{
  unsigned long mus = ((lwTime.tv_sec - cTime.tv_sec) * 1000000) +
                      lwTime.tv_usec - cTime.tv_usec;
  wTime.tv_sec += (mus / 1000000);
  wTime.tv_usec += (mus % 1000000);
}

//------------------------------------------------------------------------------
// Make report
//------------------------------------------------------------------------------
void
XrdFstOfsFile::MakeReportEnv(XrdOucString& reportString)
{
  // compute avg, min, max, sigma for read and written bytes
  unsigned long long rmin, rmax, rsum;
  unsigned long long rvmin, rvmax, rvsum; // readv bytes
  unsigned long long rsmin, rsmax, rssum; // read single bytes
  unsigned long rcmin, rcmax, rcsum;      // readv count
  unsigned long long wmin, wmax, wsum;
  double rsigma, rvsigma, rssigma, rcsigma, wsigma;
  {
    XrdSysMutexHelper vecLock(vecMutex);
    ComputeStatistics(rvec, rmin, rmax, rsum, rsigma);
    ComputeStatistics(wvec, wmin, wmax, wsum, wsigma);
    ComputeStatistics(monReadvBytes, rvmin, rvmax, rvsum, rvsigma);
    ComputeStatistics(monReadSingleBytes, rsmin, rsmax, rssum, rssigma);
    ComputeStatistics(monReadvCount, rcmin, rcmax, rcsum, rcsigma);
    char report[16384];

    if (rmin == 0xffffffff) {
      rmin = 0;
    }

    if (wmin == 0xffffffff) {
      wmin = 0;
    }

    snprintf(report, sizeof(report) - 1,
             "log=%s&path=%s&fstpath=%s&ruid=%u&rgid=%u&td=%s&"
             "host=%s&lid=%lu&fid=%llu&fsid=%lu&"
             "ots=%lu&otms=%lu&"
             "cts=%lu&ctms=%lu&"
             "nrc=%lu&nwc=%lu&"
             "rb=%llu&rb_min=%llu&rb_max=%llu&rb_sigma=%.02f&"
             "rv_op=%llu&rvb_min=%llu&rvb_max=%llu&rvb_sum=%llu&rvb_sigma=%.02f&"
             "rs_op=%llu&rsb_min=%llu&rsb_max=%llu&rsb_sum=%llu&rsb_sigma=%.02f&"
             "rc_min=%lu&rc_max=%lu&rc_sum=%lu&rc_sigma=%.02f&"
             "wb=%llu&wb_min=%llu&wb_max=%llu&wb_sigma=%.02f&"
             "sfwdb=%llu&sbwdb=%llu&sxlfwdb=%llu&sxlbwdb=%llu&"
             "nfwds=%lu&nbwds=%lu&nxlfwds=%lu&nxlbwds=%lu&"
             "rt=%.02f&rvt=%.02f&wt=%.02f&osize=%llu&csize=%llu&%s"
             , this->logId
             , mCapOpaque->Get("mgm.path") ? mCapOpaque->Get("mgm.path") : mNsPath.c_str()
             , mFstPath.c_str()
             , this->vid.uid, this->vid.gid, tIdent.c_str()
             , gOFS.mHostName, mLid, mFileId, mFsId
             , openTime.tv_sec, (unsigned long) openTime.tv_usec / 1000
             , closeTime.tv_sec, (unsigned long) closeTime.tv_usec / 1000
             , rCalls, wCalls
             , rsum, rmin, rmax, rsigma
             , (unsigned long long)monReadvBytes.size(), rvmin, rvmax, rvsum, rvsigma
             , (unsigned long long)monReadSingleBytes.size(), rsmin, rsmax, rssum, rssigma
             , rcmin, rcmax, rcsum, rcsigma
             , wsum
             , wmin
             , wmax
             , wsigma
             , sFwdBytes
             , sBwdBytes
             , sXlFwdBytes
             , sXlBwdBytes
             , nFwdSeeks
             , nBwdSeeks
             , nXlFwdSeeks
             , nXlBwdSeeks
             , ((rTime.tv_sec * 1000.0) + (rTime.tv_usec / 1000.0))
             , ((rvTime.tv_sec * 1000.0) + (rvTime.tv_usec / 1000.0))
             , ((wTime.tv_sec * 1000.0) + (wTime.tv_usec / 1000.0))
             , (unsigned long long) openSize
             , (unsigned long long) closeSize
             , eos::common::SecEntity::ToEnv(mSecString.c_str(),
                 ((mTpcFlag == kTpcDstSetup) ||
                  (mTpcFlag == kTpcSrcRead)) ? "tpc" : 0).c_str());
    reportString = report;
  }
}

//------------------------------------------------------------------------------
// Check if file has been modified while in use
//------------------------------------------------------------------------------
int
XrdFstOfsFile::modified()
{
  int rc = 0;
  bool fileExists = true;
  struct stat statinfo;

  if (layOut) {
    if ((layOut->Stat(&statinfo))) {
      fileExists = false;
    }
  } else {
    if ((XrdOfsOss->Stat(mFstPath.c_str(), &statinfo))) {
      fileExists = false;
    }
  }

  // Check if the file could have been changed in the meanwhile ...
  if (fileExists && isReplication && (!isRW)) {
    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WOpenFid[mFsId].count(mFileId)) {
        if (gOFS.WOpenFid[mFsId][mFileId] > 0) {
          eos_err("file is now open for writing - discarding replication "
                  "[wopen=%d]", gOFS.WOpenFid[mFsId][mFileId]);
          gOFS.Emsg("closeofs", error, EIO,
                    "guarantee correctness - "
                    "file has been opened for writing during replication",
                    mNsPath.c_str());
          rc = SFS_ERROR;
        }
      }
    }

    if ((statinfo.st_mtime != updateStat.st_mtime)) {
      eos_err("file has been modified during replication");
      rc = SFS_ERROR;
      gOFS.Emsg("closeofs", error, EIO, "guarantee correctness -"
                "file has been modified during replication", mNsPath.c_str());
    }
  }

  return rc;
}

//------------------------------------------------------------------------------
// Close - low level ofs method which is called from one of the layout plugins
//------------------------------------------------------------------------------
int
XrdFstOfsFile::closeofs()
{
  return XrdOfsFile::close();
}

//------------------------------------------------------------------------------
// Layout read callback
//------------------------------------------------------------------------------
int
XrdFstOfsFile::LayoutReadCB(eos::fst::CheckSum::ReadCallBack::callback_data_t*
                            cbd)
{
  return ((Layout*) cbd->caller)->Read(cbd->offset, cbd->buffer, cbd->size);
}

//------------------------------------------------------------------------------
// File read callback
//------------------------------------------------------------------------------
int
XrdFstOfsFile::FileIoReadCB(eos::fst::CheckSum::ReadCallBack::callback_data_t*
                            cbd)
{
  return ((FileIo*) cbd->caller)->fileRead(cbd->offset, cbd->buffer, cbd->size);
}

//------------------------------------------------------------------------------
// Verify checksum method
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::verifychecksum()
{
  bool checksumerror = false;
  int checksumlen = 0;

  // Deal with checksums
  if (mCheckSum) {
    mCheckSum->Finalize();

    if (mCheckSum->NeedsRecalculation()) {
      if ((!isRW) && ((sFwdBytes + sBwdBytes)
                      || (mCheckSum->GetMaxOffset() != openSize))) {
        // We don't rescan files if they are read non-sequential or only
        // partially
        eos_debug("info=\"skipping checksum (re-scan) for non-sequential "
                  "reading ...\"");
        mCheckSum.reset(nullptr);
        return false;
      }
    } else {
      eos_debug("isrw=%d max-offset=%lld opensize=%lld", isRW,
                mCheckSum->GetMaxOffset(), openSize);

      if (((!isRW) && ((mCheckSum->GetMaxOffset() != openSize) ||
                       (!mCheckSum->GetMaxOffset())))) {
        eos_debug("info=\"skipping checksum (re-scan) for access without any IO or "
                  "partial sequential read IO from the beginning...\"");
        mCheckSum.reset(nullptr);
        return false;
      }

      if ((isRW) && mCheckSum->GetMaxOffset() &&
          (mCheckSum->GetMaxOffset() < openSize)) {
        // If there was a write which was not extending the file the checksum
        // is dirty!
        mCheckSum->SetDirty();
      }
    }

    // If checksum is not completely computed
    if (mCheckSum->NeedsRecalculation()) {
      unsigned long long scansize = 0;
      float scantime = 0; // is ms

      if (!XrdOfsFile::fctl(SFS_FCTL_GETFD, 0, error)) {
        // Rescan the file
        eos::fst::CheckSum::ReadCallBack::callback_data_t cbd;
        cbd.caller = (void*) layOut;
        eos::fst::CheckSum::ReadCallBack cb(LayoutReadCB, cbd);

        if (mCheckSum->ScanFile(cb, scansize, scantime)) {
          XrdOucString sizestring;
          eos_info("info=\"rescanned checksum\" size=%s time=%.02f ms rate=%.02f MB/s %s",
                   eos::common::StringConversion::GetReadableSizeString(sizestring,
                       scansize, "B"),
                   scantime,
                   1.0 * scansize / 1000 / (scantime ? scantime : 99999999999999LL),
                   mCheckSum->GetHexChecksum());
        } else {
          eos_err("Rescanning of checksum failed");
          mCheckSum.reset(nullptr);
          return false;
        }
      } else {
        eos_err("Couldn't get file descriptor");
        mCheckSum.reset(nullptr);
        return false;
      }
    } else {
      // This was prefect streaming I/O
      if ((!isRW) && (mCheckSum->GetMaxOffset() != openSize)) {
        eos_info("info=\"skipping checksum (re-scan) since file was not read "
                 "completely %llu %llu...\"", mCheckSum->GetMaxOffset(), openSize);
        mCheckSum.reset(nullptr);
        return false;
      }
    }

    if (isRW) {
      eos_info("(write) checksum type: %s checksum hex: %s requested-checksum hex: %s",
               mCheckSum->GetName(), mCheckSum->GetHexChecksum(),
               mOpenOpaque->Get("mgm.checksum") ? mOpenOpaque->Get("mgm.checksum") : "-none-");

      // Check if the check sum for the file was given at upload time
      if (mOpenOpaque->Get("mgm.checksum")) {
        XrdOucString opaqueChecksum = mOpenOpaque->Get("mgm.checksum");
        XrdOucString hexChecksum = mCheckSum->GetHexChecksum();

        if ((opaqueChecksum != "disable") && (opaqueChecksum != hexChecksum)) {
          eos_err("requested checksum %s does not match checksum %s of uploaded"
                  " file", opaqueChecksum.c_str(), hexChecksum.c_str());
          mCheckSum.reset(nullptr);
          return true;
        }
      }

      mCheckSum->GetBinChecksum(checksumlen);
      // Copy checksum into meta data
      fMd->mProtoFmd.set_checksum(mCheckSum->GetHexChecksum());

      if (mHasWrite) {
        // If we have no write, we don't set this attributes (xrd3cp!)
        // set the eos checksum extended attributes
        std::unique_ptr<eos::fst::FileIo> io(eos::fst::FileIoPlugin::GetIoObject(
                                               mFstPath.c_str(), this));

        if (((eos::common::LayoutId::GetLayoutType(mLid) ==
              eos::common::LayoutId::kPlain) ||
             (eos::common::LayoutId::GetLayoutType(mLid) ==
              eos::common::LayoutId::kReplica))) {
          // Don't put file checksum tags for complex layouts like raid6,readdp, archive
          if (io->attrSet(std::string("user.eos.checksumtype"),
                          std::string(mCheckSum->GetName()))) {
            eos_err("unable to set extended attribute <eos.checksumtype> errno=%d", errno);
          }

          if (io->attrSet("user.eos.checksum", mCheckSum->GetBinChecksum(checksumlen),
                          checksumlen)) {
            eos_err("unable to set extended attribute <eos.checksum> errno=%d", errno);
          }
        }

        // Reset any tagged error
        if (io->attrSet("user.eos.filecxerror", "0")) {
          eos_err("unable to set extended attribute <eos.filecxerror> errno=%d", errno);
        }

        if (io->attrSet("user.eos.blockcxerror", "0")) {
          eos_err("unable to set extended attribute <eos.blockcxerror> errno=%d", errno);
        }
      }
    } else {
      // This is a read with checksum check, compare with fMD
      bool isopenforwrite = false;
      // If the file is currently opened for write we don't check checksums!
      {
        XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

        if (gOFS.WOpenFid[mFsId].count(mFileId)) {
          if (gOFS.WOpenFid[mFsId][mFileId] > 0) {
            isopenforwrite = true;
          }
        }
      }

      if (isopenforwrite) {
        eos_info("(read)  disabling checksum check: file is currently written");
        return false;
      }

      eos_info("(read)  checksum type: %s checksum hex: %s fmd-checksum: %s",
               mCheckSum->GetName(),
               mCheckSum->GetHexChecksum(),
               fMd->mProtoFmd.checksum().c_str());
      std::string calculatedchecksum = mCheckSum->GetHexChecksum();

      // We might fetch an unitialized value, so that is not to be considered
      // a checksum error yet.
      if (fMd->mProtoFmd.checksum() != "none") {
        if (calculatedchecksum != fMd->mProtoFmd.checksum().c_str()) {
          checksumerror = true;
        }
      }
    }
  }

  return checksumerror;
}

//------------------------------------------------------------------------------
// Close file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::close()
{
  EPNAME("close");
  int rc = 0; // return code
  int brc = 0; // return code before 'close' has been called
  bool checksumerror = false;
  bool targetsizeerror = false;
  bool committed = false;
  bool minimumsizeerror = false;
  bool consistencyerror = false;
  bool atomicoverlap = false;

  // Any close on a file opened in TPC mode invalidates tpc keys
  if (mTpcKey.length()) {
    {
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);

      if (gOFS.TpcMap[mIsTpcDst].count(mTpcKey.c_str())) {
        eos_info("msg=\"remove tpc key\" key=%s", mTpcKey.c_str());
        gOFS.TpcMap[mIsTpcDst].erase(mTpcKey.c_str());

        try {
          gOFS.TpcMap[mIsTpcDst].resize(0);
        } catch (const std::length_error& e) {}
      }
    }

    if (mTpcFlag == kTpcDstSetup) {
      if (!mTpcThreadStatus) {
        int retc = XrdSysThread::Join(mTpcThread, NULL);
        eos_debug("TPC job join returned %i", retc);
      } else {
        eos_warning("TPC job was never started successfully");
      }
    }
  }

  if (mIsDevNull) {
    eos_debug("msg=\"closing sink file i.e. /dev/null\"");
    closed = true;
    return SFS_OK;
  }

  // We enter the close logic only once since there can be an explicit close or
  // a close via the destructor
  if (opened && (!closed) && fMd) {
    // Check if the file close comes from a client disconnect e.g. the destructor
    XrdOucString hexstring = "";
    eos::common::FileId::Fid2Hex(fMd->mProtoFmd.fid(), hexstring);
    XrdOucErrInfo error;
    XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
    XrdOucString OpaqueString = "";
    OpaqueString += "&mgm.fsid=";
    OpaqueString += (int) fMd->mProtoFmd.fsid();
    OpaqueString += "&mgm.fid=";
    OpaqueString += hexstring;
    XrdOucEnv Opaque(OpaqueString.c_str());
    capOpaqueString += OpaqueString;
    eos_info("viaDelete=%d", viaDelete);

    if ((viaDelete || writeDelete) && ((isCreation ||
                                        IsChunkedUpload()) && (!mFusex))) {
      // It is closed by the destructor e.g. no proper close
      // or the specified checksum does not match the computed one
      if (viaDelete) {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"client disconnect\""
                 "  fsid=%u fxid=%08x on fsid=%u", fMd->mProtoFmd.fsid(), fMd->mProtoFmd.fid());
      }

      if (writeDelete) {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"write/policy error\""
                 " fsid=%u fxid=%08x on fsid=%u", fMd->mProtoFmd.fsid(), fMd->mProtoFmd.fid());
      }

      // Delete the file - set the file to be deleted
      deleteOnClose = true;
      layOut->Remove();

      if (layOut->IsEntryServer() && (!isReplication) && (!mIsInjection)) {
        capOpaqueString += "&mgm.dropall=1";
      }

      // Delete the replica in the MGM
      int rc = gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                                mCapOpaque->Get("mgm.manager"), capOpaqueString);

      if (rc) {
        eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                    hexstring.c_str(), fMd->mProtoFmd.fid(), mCapOpaque->Get("mgm.manager"));
      }
    } else {
      // Check if this was a newly created file
      if (isCreation) {
        // If we had space allocation we have to truncate the allocated space to
        // the real size of the file
        if ((strcmp(layOut->GetName(), "raiddp") == 0) ||
            (strcmp(layOut->GetName(), "raid6") == 0) ||
            (strcmp(layOut->GetName(), "archive") == 0)) {
          // the entry server has to truncate only if this is not a recovery action
          if (layOut->IsEntryServer() && !mRainReconstruct) {
            eos_info("msg=\"truncate RAIN layout\" truncate-offset=%llu",
                     maxOffsetWritten);
            layOut->Truncate(maxOffsetWritten);
          }
        } else {
          if ((long long) maxOffsetWritten > (long long) openSize) {
            // Check if we have to deallocate something for this file transaction
            if ((mBookingSize) && (mBookingSize > (long long) maxOffsetWritten)) {
              eos_info("deallocationg %llu bytes", mBookingSize - maxOffsetWritten);
              layOut->Truncate(maxOffsetWritten);
              // We have evt. to deallocate blocks which have not been written
              layOut->Fdeallocate(maxOffsetWritten, mBookingSize);
            }
          }
        }
      }

      eos_info("calling verifychecksum");
      checksumerror = verifychecksum();
      targetsizeerror = (mTargetSize) ? (mTargetSize != (off_t) maxOffsetWritten) :
                        false;

      if (isCreation) {
        // Check that the minimum file size policy is met!
        minimumsizeerror = (mMinSize) ? ((off_t) maxOffsetWritten < mMinSize) : false;

        if (minimumsizeerror) {
          eos_warning("written file %s is smaller than required minimum file "
                      "size=%llu written=%llu", mNsPath.c_str(), mMinSize,
                      maxOffsetWritten);
        }
      }

      if (IsRainLayout(layOut->GetLayoutId())) {
        // For RAID-like layouts don't do this check
        targetsizeerror = false;
        minimumsizeerror = false;
      }

      eos_debug("checksumerror = %i, targetsizerror= %i,"
                "maxOffsetWritten = %zu, targetsize = %lli",
                checksumerror, targetsizeerror, maxOffsetWritten, mTargetSize);

      // ---- add error simulation for checksum errors on read
      if ((!isRW) && gOFS.Simulate_XS_read_error) {
        checksumerror = true;
        eos_warning("simlating checksum errors on read");
      }

      // ---- add error simulation for checksum errors on write
      if (isRW && gOFS.Simulate_XS_write_error) {
        checksumerror = true;
        eos_warning("simlating checksum errors on write");
      }

      if (isRW && (checksumerror || targetsizeerror || minimumsizeerror)) {
        // We have a checksum error if the checksum was preset and does not match!
        // We have a target size error, if the target size was preset and does not match!
        // Set the file to be deleted
        deleteOnClose = true;
        layOut->Remove();

        if (layOut->IsEntryServer() && (!isReplication) && (!mIsInjection)) {
          capOpaqueString += "&mgm.dropall=1";
        }

        // Delete the replica in the MGM
        int rc = gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                                  mCapOpaque->Get("mgm.manager"), capOpaqueString);

        if (rc) {
          eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                      hexstring.c_str(), fMd->mProtoFmd.fid(), mCapOpaque->Get("mgm.manager"));
        }
      }

      // Store the entry server information before closing the layout
      bool isEntryServer = false;

      if (layOut->IsEntryServer()) {
        isEntryServer = true;
      }

      // First we assume that, if we have writes, we update it
      closeSize = openSize;

      if ((!checksumerror) && (mHasWrite || isCreation || commitReconstruction) &&
          (!minimumsizeerror) && (!mRainReconstruct || !hasReadError)) {
        // Commit meta data
        struct stat statinfo;

        if ((rc = layOut->Stat(&statinfo))) {
          rc = gOFS.Emsg(epname, this->error, EIO, "close - cannot stat closed layout"
                         " to determine file size", mNsPath.c_str());
        }

        if (!rc) {
          if ((statinfo.st_size == 0) || mHasWrite) {
            // Update size
            closeSize = statinfo.st_size;
            fMd->mProtoFmd.set_size(statinfo.st_size);
            fMd->mProtoFmd.set_disksize(statinfo.st_size);
            fMd->mProtoFmd.set_mgmsize(0xfffffffffff1ULL); // now again undefined
            fMd->mProtoFmd.set_mgmchecksum(""); // now again empty
            fMd->mProtoFmd.set_diskchecksum(""); // now again empty
            fMd->mProtoFmd.set_layouterror(0); // reset layout errors
            fMd->mProtoFmd.set_locations(""); // reset locations
            fMd->mProtoFmd.set_filecxerror(0);
            fMd->mProtoFmd.set_blockcxerror(0);
            fMd->mProtoFmd.set_locations(""); // reset locations
            fMd->mProtoFmd.set_filecxerror(0);
            fMd->mProtoFmd.set_blockcxerror(0);
            fMd->mProtoFmd.set_mtime(statinfo.st_mtime);
#ifdef __APPLE__
            fMd->mProtoFmd.set_mtime_ns(0);
#else
            fMd->mProtoFmd.set_mtime_ns(statinfo.st_mtim.tv_nsec);
#endif
            // Set the container id
            fMd->mProtoFmd.set_cid(mCid);

            // For replicat's set the original uid/gid/lid values
            if (mCapOpaque->Get("mgm.source.lid")) {
              fMd->mProtoFmd.set_lid(strtoul(mCapOpaque->Get("mgm.source.lid"), 0, 10));
            }

            if (mCapOpaque->Get("mgm.source.ruid")) {
              fMd->mProtoFmd.set_uid(atoi(mCapOpaque->Get("mgm.source.ruid")));
            }

            if (mCapOpaque->Get("mgm.source.rgid")) {
              fMd->mProtoFmd.set_gid(atoi(mCapOpaque->Get("mgm.source.rgid")));
            }

            // Commit local
            try {
              if (!gFmdDbMapHandler.Commit(fMd)) {
                eos_err("unabel to commit meta data to local database");
                (void) gOFS.Emsg(epname, this->error, EIO, "close - unable to "
                                 "commit meta data", mNsPath.c_str());
              }
            } catch (const std::length_error& e) {}

            // Commit to central mgm cache
            int envlen = 0;
            XrdOucString capOpaqueFile = "";
            XrdOucString mTimeString = "";
            capOpaqueFile += "/?";
            capOpaqueFile += mCapOpaque->Env(envlen);
            capOpaqueFile += "&mgm.pcmd=commit";
            capOpaqueFile += "&mgm.size=";
            char filesize[1024];
            sprintf(filesize, "%" PRIu64 "", fMd->mProtoFmd.size());
            capOpaqueFile += filesize;

            if (mCheckSum) {
              capOpaqueFile += "&mgm.checksum=";
              capOpaqueFile += mCheckSum->GetHexChecksum();
            }

            capOpaqueFile += "&mgm.mtime=";
            capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString,
                             (mForcedMtime != 1) ? mForcedMtime : (unsigned long long)
                             fMd->mProtoFmd.mtime());
            capOpaqueFile += "&mgm.mtime_ns=";
            capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString,
                             (mForcedMtime != 1) ? mForcedMtime_ms : (unsigned long long)
                             fMd->mProtoFmd.mtime_ns());

            if (mFusex) {
              capOpaqueFile += "&mgm.fusex=1";
            }

            if (mHasWrite) {
              capOpaqueFile += "&mgm.modified=1";
            }

            capOpaqueFile += "&mgm.add.fsid=";
            capOpaqueFile += (int) fMd->mProtoFmd.fsid();

            // If <drainfsid> is set, we can issue a drop replica
            if (mCapOpaque->Get("mgm.drainfsid")) {
              capOpaqueFile += "&mgm.drop.fsid=";
              capOpaqueFile += mCapOpaque->Get("mgm.drainfsid");
            }

            if (mRainReconstruct) {
              // Indicate that this is a commit of a RAIN reconstruction
              capOpaqueFile += "&mgm.reconstruction=1";

              if (!hasReadError && mOpenOpaque->Get("eos.pio.recfs")) {
                capOpaqueFile += "&mgm.drop.fsid=";
                capOpaqueFile += mOpenOpaque->Get("eos.pio.recfs");
                commitReconstruction = true;
              }
            } else {
              if (isEntryServer && !isReplication && !mIsInjection) {
                // The entry server commits size and checksum
                capOpaqueFile += "&mgm.commit.size=1&mgm.commit.checksum=1";
              } else {
                capOpaqueFile += "&mgm.replication=1";
              }
            }

            // The log ID to the commit
            capOpaqueFile += "&mgm.logid=";
            capOpaqueFile += logId;

            // Evt. tag as an OC-Chunk commit
            if (IsChunkedUpload()) {
              // Add the chunk information
              int envlen;
              capOpaqueFile += eos::common::OwnCloud::FilterOcQuery(mOpenOpaque->Env(envlen));
            }

            rc = gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                                  mCapOpaque->Get("mgm.manager"), capOpaqueFile);

            if (rc) {
              if ((error.getErrInfo() == EIDRM) || (error.getErrInfo() == EBADE) ||
                  (error.getErrInfo() == EBADR) || (error.getErrInfo() == EREMCHG)) {
                if (!gOFS.Storage->CloseTransaction(mFsId, mFileId)) {
                  eos_crit("cannot close transaction for fsid=%u fid=%llu", mFsId, mFileId);
                }

                if (error.getErrInfo() == EIDRM) {
                  // This file has been deleted in the meanwhile ... we can
                  // unlink that immediately
                  eos_info("info=\"unlinking fid=%08x path=%s - "
                           "file has been already unlinked from the namespace\"",
                           fMd->mProtoFmd.fid(), mNsPath.c_str());
                  mFusexIsUnlinked = true;
                }

                if (error.getErrInfo() == EBADE) {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "file size of replica does not match reference\"",
                          fMd->mProtoFmd.fid(), mNsPath.c_str());
                  consistencyerror = true;
                }

                if (error.getErrInfo() == EBADR) {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "checksum of replica does not match reference\"",
                          fMd->mProtoFmd.fid(), mNsPath.c_str());
                  consistencyerror = true;
                }

                if (error.getErrInfo() == EREMCHG) {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "overlapping atomic upload - discarding this one\"",
                          fMd->mProtoFmd.fid(), mNsPath.c_str());
                  atomicoverlap = true;
                }

                deleteOnClose = true;
              } else {
                eos_crit("commit returned an uncatched error msg=%s [probably timeout]"
                         " - closing transaction to keep the file save - rc = %d", error.getErrText(),
                         rc);

                if (isRW) {
                  gOFS.Storage->CloseTransaction(mFsId, mFileId);
                }
              }
            } else {
              committed = true;
            }
          }
        }
      }
    }

    if (isRW && (rc == SFS_OK)) {
      gOFS.Storage->CloseTransaction(mFsId, mFileId);
    }

    // Recompute our ETag
    {
      // If there is a checksum we use the checksum, otherwise we return inode+mtime
      if (mCheckSum) {
        if (strcmp(mCheckSum->GetName(), "md5")) {
          // use inode + checksum
          char setag[256];
          snprintf(setag, sizeof(setag) - 1, "\"%llu:%s\"",
                   eos::common::FileId::FidToInode((unsigned long long) fMd->mProtoFmd.fid()),
                   fMd->mProtoFmd.checksum().c_str());
          mEtag = setag;
        } else {
          // use checksum, S3 wants the pure MD5
          char setag[256];
          snprintf(setag, sizeof(setag) - 1, "\"%s\"", fMd->mProtoFmd.checksum().c_str());
          mEtag = setag;
        }
      } else {
        // use inode + mtime
        char setag[256];
        snprintf(setag, sizeof(setag) - 1, "\"%llu:%llu\"",
                 eos::common::FileId::FidToInode((unsigned long long) fMd->mProtoFmd.fid()),
                 (unsigned long long) fMd->mProtoFmd.mtime());
        mEtag = setag;
      }
    }
    int closerc = 0; // return of the close
    brc = rc; // return before the close
    rc |= modified();
    closerc = layOut->Close();
    rc |= closerc;
    closed = true;

    if (closerc || (mRainReconstruct && hasReadError)) {
      // For RAIN layouts if there is an error on close when writing then we
      // delete the whole file. If we do RAIN reconstruction we cleanup this
      // local replica which was not committed.
      if ((eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kRaidDP) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kRaid6) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) ==
           eos::common::LayoutId::kArchive)) {
        deleteOnClose = true;
      } else {
        // Some (remote) replica didn't make it through ... trigger an auto-repair
        if (!deleteOnClose) {
          repairOnClose = true;
        }
      }
    }

    {
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (isRW) {
        if ((mIsInjection || isCreation || IsChunkedUpload()) && (!rc) &&
            (gOFS.WOpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] > 1)) {
          // indicate that this file was closed properly and disable further delete on close
          gOFS.WNoDeleteOnCloseFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] = true;
        }

        gOFS.WOpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()]--;
      } else {
        gOFS.ROpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()]--;
      }

      if (gOFS.WOpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] <= 0) {
        gOFS.WOpenFid[fMd->mProtoFmd.fsid()].erase(fMd->mProtoFmd.fid());
        gOFS.WOpenFid[fMd->mProtoFmd.fsid()].resize(0);
        // When the last writer is gone we can remove the prohibiting entry
        gOFS.WNoDeleteOnCloseFid[fMd->mProtoFmd.fsid()].erase(fMd->mProtoFmd.fid());
        gOFS.WNoDeleteOnCloseFid[fMd->mProtoFmd.fsid()].resize(0);
      }

      if (gOFS.ROpenFid[fMd->mProtoFmd.fsid()][fMd->mProtoFmd.fid()] <= 0) {
        gOFS.ROpenFid[fMd->mProtoFmd.fsid()].erase(fMd->mProtoFmd.fid());
        gOFS.ROpenFid[fMd->mProtoFmd.fsid()].resize(0);
      }
    }

    gettimeofday(&closeTime, &tz);

    if (!deleteOnClose) {
      // Prepare a report and add to the report queue
      if (mTpcFlag != kTpcSrcCanDo) {
        // We don't want a report for the source tpc setup. The kTpcSrcRead
        // stage actually uses the opaque info from kTpcSrcSetup and that's
        // why we also generate a report at this stage.
        XrdOucString reportString = "";
        MakeReportEnv(reportString);
        gOFS.ReportQueueMutex.Lock();
        gOFS.ReportQueue.push(reportString);
        gOFS.ReportQueueMutex.UnLock();
      }

      if (isRW) {
        // Store in the WrittenFilesQueue
        gOFS.WrittenFilesQueueMutex.Lock();
        gOFS.WrittenFilesQueue.push(fMd->mProtoFmd);
        gOFS.WrittenFilesQueueMutex.UnLock();
      }
    }

    // Check if the target filesystem has been put into some non-operational mode
    // in the meanwhile, it makes no sense to try to commit in this case
    {
      eos::common::RWMutexReadLock lock(gOFS.Storage->mFsMutex);

      if (gOFS.Storage->mFileSystemsMap.count(mFsId) &&
          gOFS.Storage->mFileSystemsMap[mFsId]->GetConfigStatus() <
          eos::common::FileSystem::kDrain) {
        eos_notice("msg=\"failing transfer because filesystem has non-"
                   "operational state\" path=%s state=%s", mNsPath.c_str(),
                   eos::common::FileSystem::GetConfigStatusAsString
                   (gOFS.Storage->mFileSystemsMap[mFsId]->GetConfigStatus()));
        deleteOnClose = true;
      }
    }
    {
      // Check if the delete on close has been prohibited for this file id
      XrdSysMutexHelper scope_lock(gOFS.OpenFidMutex);

      if (gOFS.WNoDeleteOnCloseFid[mFsId].count(mFileId)) {
        eos_notice("msg=\"prohibiting delete on close since we had a "
                   "sussessfull put but still an unacknowledged open\" path=%s",
                   mNsPath.c_str());
        deleteOnClose = false;
      }
    }

    if (deleteOnClose && (mIsInjection || isCreation || IsChunkedUpload()) &&
        (!mFusex)) {
      rc = SFS_ERROR;
      eos_info("info=\"deleting on close\" fn=%s fstpath=%s",
               mCapOpaque->Get("mgm.path"), mFstPath.c_str());
      int retc = gOFS._rem(mNsPath.c_str(), error, 0, mCapOpaque.get(),
                           mFstPath.c_str(), mFileId, mFsId, true);

      if (retc) {
        eos_debug("<rem> returned retc=%d", retc);
      }

      if (committed) {
        // If we committed the replica and an error happened remote, we have
        // to unlink it again
        XrdOucString hexstring = "";
        eos::common::FileId::Fid2Hex(mFileId, hexstring);
        XrdOucErrInfo error;
        XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
        XrdOucString OpaqueString = "";
        OpaqueString += "&mgm.fsid=";
        OpaqueString += (int) mFsId;
        OpaqueString += "&mgm.fid=";
        OpaqueString += hexstring;

        // If deleteOnClose at the gateway then we drop all replicas
        if (layOut->IsEntryServer() && (!isReplication) && (!mIsInjection)) {
          OpaqueString += "&mgm.dropall=1";
        }

        XrdOucEnv Opaque(OpaqueString.c_str());
        capOpaqueString += OpaqueString;
        // Delete the replica in the MGM
        int rcode = gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                                     mCapOpaque->Get("mgm.manager"), capOpaqueString);

        if (rcode && (rcode != EIDRM)) {
          eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                      hexstring.c_str(), mFileId, mCapOpaque->Get("mgm.manager"));
        }

        eos_info("info=\"removing on manager\" manager=%s fid=%llu fsid=%d "
                 "fn=%s fstpath=%s rc=%d", mCapOpaque->Get("mgm.manager"),
                 mFileId, (int) mFsId,
                 mCapOpaque->Get("mgm.path"), mFstPath.c_str(), rcode);
      }

      if (minimumsizeerror) {
        // Minimum size criteria not fullfilled
        gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                  "because it is smaller than the required minimum file size"
                  " in that directory", mNsPath.c_str());
        eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                    "\"minimum file size criteria\"", mCapOpaque->Get("mgm.path"),
                    mFstPath.c_str());
      } else {
        if (checksumerror) {
          // Checksum error
          gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                    "because of a checksum error ", mNsPath.c_str());
          eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                      "\"checksum error\"", mCapOpaque->Get("mgm.path"), mFstPath.c_str());
        } else {
          if (writeErrorFlag == kOfsSimulatedIoError) {
            // Simulated write error
            gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                      "because of a simulated IO error ", mNsPath.c_str());
            eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                        "\"simulated IO error\"", mCapOpaque->Get("mgm.path"), mFstPath.c_str());
          } else {
            if (writeErrorFlag == kOfsMaxSizeError) {
              // Maximum size criteria not fullfilled
              gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned "
                        "because you exceeded the maximum file size settings for "
                        "this namespace branch", mNsPath.c_str());
              eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                          "\"maximum file size criteria\"", mCapOpaque->Get("mgm.path"),
                          mFstPath.c_str());
            } else {
              if (writeErrorFlag == kOfsDiskFullError) {
                // Disk full detected during write
                gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned"
                          " because the target disk filesystem got full and you "
                          "didn't use reservation", mNsPath.c_str());
                eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                            "\"filesystem full\"", mCapOpaque->Get("mgm.path"), mFstPath.c_str());
              } else {
                if (writeErrorFlag == kOfsIoError) {
                  // Generic IO error on the underlying device
                  gOFS.Emsg(epname, this->error, EIO, "store file - file has been cleaned because"
                            " of an IO error during a write operation", mNsPath.c_str());
                  eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason="
                           "\"write IO error\"", mCapOpaque->Get("mgm.path"), mFstPath.c_str());
                } else {
                  // Target size is different from the uploaded file size
                  if (targetsizeerror) {
                    gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                              "cleaned because the stored file does not match "
                              "the provided targetsize", mNsPath.c_str());
                    eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason="
                                "\"target size mismatch\"", mCapOpaque->Get("mgm.path"),
                                mFstPath.c_str());
                  } else {
                    if (consistencyerror) {
                      gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                                "cleaned because the stored file does not match "
                                "the reference meta-data size/checksum", mNsPath.c_str());
                      eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason="
                               "\"meta-data size/checksum mismatch\"", mCapOpaque->Get("mgm.path"),
                               mFstPath.c_str());
                    } else {
                      if (atomicoverlap) {
                        gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                                  "cleaned because of an overlapping atomic upload "
                                  "and we are not the last uploader", mNsPath.c_str());
                        eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason="
                                 "\"suppressed atomic uploadh\"", mCapOpaque->Get("mgm.path"),
                                 mFstPath.c_str());
                      } else {
                        // Client has disconnected and file is cleaned-up
                        gOFS.Emsg(epname, this->error, EIO, "store file - file has been "
                                  "cleaned because of a client disconnect", mNsPath.c_str());
                        eos_warning("info=\"deleting on close\" fn=%s fstpath=%s "
                                    "reason=\"client disconnect\"", mCapOpaque->Get("mgm.path"),
                                    mFstPath.c_str());
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    } else {
      if (checksumerror) {
        // Checksum error detected
        rc = SFS_ERROR;
        gOFS.Emsg(epname, this->error, EIO,
                  "verify checksum - checksum error for file fn=",
                  mCapOpaque->Get("mgm.path"));
        int envlen = 0;
        eos_crit("file-xs error file=%s", mCapOpaque->Env(envlen));
      }
    }

    if ((!IsChunkedUpload()) && repairOnClose) {
      // Do an upcall to the MGM and ask to adjust the replica of the uploaded file
      XrdOucString OpaqueString = "/?mgm.pcmd=adjustreplica&mgm.path=";
      OpaqueString += mCapOpaque->Get("mgm.path");
      eos_info("info=\"repair on close\" path=%s", mCapOpaque->Get("mgm.path"));

      if (gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                           mCapOpaque->Get("mgm.manager"), OpaqueString)) {
        eos_err("failed to execute 'adjustreplica' for path=%s",
                mCapOpaque->Get("mgm.path"));
        gOFS.Emsg(epname, error, EIO, "create all replicas - uploaded file is "
                  "at risk - only one replica has been successfully stored for fn=",
                  mCapOpaque->Get("mgm.path"));
      } else {
        if (!brc) {
          // Reset the return code and clean error message
          gOFS.Emsg(epname, error, 0, "no error");
          rc = 0;
        }
      }

      eos_warning("executed 'adjustreplica' for path=%s - file is at low risk "
                  "due to missing replicas", mCapOpaque->Get("mgm.path"));
    }

    if (!rc && (mEventOnClose || mSyncEventOnClose) && layOut->IsEntryServer()) {
      //trigger an MGM event if asked from the entry point
      XrdOucString capOpaqueFile = "";
      XrdOucString eventType = "";
      capOpaqueFile += "/?";
      int envlen = 0;
      capOpaqueFile += mCapOpaque->Env(envlen);
      capOpaqueFile += "&mgm.pcmd=event";

      // Set default workflow if nothing is specified
      if (mEventWorkflow.length() == 0) {
        mEventWorkflow = "default";
      }

      if (isRW) {
        eventType = mSyncEventOnClose ? "sync::closew" : "closew";
      } else {
        eventType = "closer";
      }

      if (mSyncEventOnClose && mEventWorkflow != common::RETRIEVE_WRITTEN_WORKFLOW_NAME) {
        std::string decodedAttributes;
        eos::common::SymKey::Base64Decode(mEventAttributes.c_str(), decodedAttributes);
        std::map<std::string, std::string> attributes;
        eos::common::StringConversion::GetKeyValueMap(decodedAttributes.c_str(),
            attributes,
            eos::common::WF_CUSTOM_ATTRIBUTES_TO_FST_EQUALS,
            eos::common::WF_CUSTOM_ATTRIBUTES_TO_FST_SEPARATOR, nullptr);
        std::string errMsgBackFromWfEndpoint;
        rc = NotifyProtoWfEndPointClosew(fMd->mProtoFmd, mEventOwner, mEventOwnerGroup,
                                         mEventRequestor, mEventRequestorGroup,
                                         mEventInstance, mCapOpaque->Get("mgm.path"),
                                         mCapOpaque->Get("mgm.manager"), attributes,
                                         errMsgBackFromWfEndpoint);

        if (rc == SFS_OK) {
          return rc;
        } else {
          if (SendArchiveFailedToManager(fMd->mProtoFmd.fid(),
                                         errMsgBackFromWfEndpoint)) {
            eos_crit("msg=\"Failed to send archive failed event to manager\" errMsgBackFromWfEndpoint=\"%s\"",
                     errMsgBackFromWfEndpoint.c_str());
          }

          return ECANCELED;
        }
      }

      capOpaqueFile += "&mgm.event=";
      capOpaqueFile += eventType;
      // The log ID to the commit
      capOpaqueFile += "&mgm.logid=";
      capOpaqueFile += logId;
      capOpaqueFile += "&mgm.ruid=";
      capOpaqueFile += mCapOpaque->Get("mgm.ruid");
      capOpaqueFile += "&mgm.rgid=";
      capOpaqueFile += mCapOpaque->Get("mgm.rgid");
      capOpaqueFile += "&mgm.sec=";
      capOpaqueFile += mCapOpaque->Get("mgm.sec");

      if (mEventWorkflow.length()) {
        capOpaqueFile += "&mgm.workflow=";
        capOpaqueFile += mEventWorkflow.c_str();
      }

      eos_info("msg=\"notify\" event=\"%s\" workflow=\"%s\"", eventType.c_str(),
               mEventWorkflow.c_str());
      rc = gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                            mCapOpaque->Get("mgm.manager"), capOpaqueFile, nullptr, 30, mSyncEventOnClose,
                            false);
    }
  }

  // Mask close error for fusex, if the file had been removed already
  if (mFusexIsUnlinked && mFusex) {
    rc = 0;
    error.setErrCode(0);
  }

  eos_info("Return code rc=%i errc=%d", rc, error.getErrInfo());
  return rc;
}

//------------------------------------------------------------------------------
// Read - low level ofs method which is called from one of the layout plugins
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readofs(XrdSfsFileOffset fileOffset, char* buffer,
                       XrdSfsXferSize buffer_size)
{
  gettimeofday(&cTime, &tz);
  rCalls++;
  int rc = XrdOfsFile::read(fileOffset, buffer, buffer_size);
  eos_debug("read %llu %llu %i rc=%d", this, fileOffset, buffer_size, rc);

  if (gOFS.Simulate_IO_read_error) {
    return gOFS.Emsg("readofs", error, EIO, "read file - simulated IO error fn=",
                     mCapOpaque ? (mCapOpaque->Get("mgm.path") ?
                                   mCapOpaque->Get("mgm.path") : FName()) : FName());
  }

  // Account seeks for monitoring
  if (rOffset != static_cast<unsigned long long>(fileOffset)) {
    if (rOffset < static_cast<unsigned long long>(fileOffset)) {
      nFwdSeeks++;
      sFwdBytes += (fileOffset - rOffset);
    } else {
      nBwdSeeks++;
      sBwdBytes += (rOffset - fileOffset);
    }

    if ((rOffset + (EOS_FSTOFS_LARGE_SEEKS)) < (static_cast<unsigned long long>
        (fileOffset))) {
      sXlFwdBytes += (fileOffset - rOffset);
      nXlFwdSeeks++;
    }

    if ((static_cast<unsigned long long>(rOffset) > (EOS_FSTOFS_LARGE_SEEKS)) &&
        (rOffset - (EOS_FSTOFS_LARGE_SEEKS)) > (static_cast<unsigned long long>
            (fileOffset))) {
      sXlBwdBytes += (rOffset - fileOffset);
      nXlBwdSeeks++;
    }
  }

  if (rc > 0) {
    if (layOut->IsEntryServer() || IsRainLayout(mLid)) {
      XrdSysMutexHelper vecLock(vecMutex);
      rvec.push_back(rc);
    }

    rOffset = fileOffset + rc;
  }

  gettimeofday(&lrTime, &tz);
  AddReadTime();
  return rc;
}

//------------------------------------------------------------------------------
// Read method
//------------------------------------------------------------------------------
int
XrdFstOfsFile::read(XrdSfsFileOffset fileOffset, XrdSfsXferSize amount)
{
  int rc = XrdOfsFile::read(fileOffset, amount);
  eos_debug("rc=%d offset=%lu size=%llu", rc, fileOffset, amount);
  return rc;
}

//------------------------------------------------------------------------------
// Read
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::read(XrdSfsFileOffset fileOffset, char* buffer,
                    XrdSfsXferSize buffer_size)
{
  eos_debug("fileOffset=%lli, buffer_size=%i", fileOffset, buffer_size);

  //  EPNAME("read");
  if (mTpcFlag == kTpcSrcRead) {
    if (!(rCalls % 10)) {
      if (!TpcValid()) {
        eos_err("msg=\"tcp interrupted by control-c - cancel tcp read\" key=%s",
                mTpcKey.c_str());
        return gOFS.Emsg("read", error, EINTR, "read - tpc transfer interrupted"
                         " by client disconnect", FName());
      }
    }
  }

  int rc = layOut->Read(fileOffset, buffer, buffer_size);
  eos_debug("layout read %d checkSum %d", rc, mCheckSum.get());

  if ((rc > 0) && (mCheckSum)) {
    XrdSysMutexHelper cLock(ChecksumMutex);
    mCheckSum->Add(buffer, static_cast<size_t>(rc),
                   static_cast<off_t>(fileOffset));
  }

  if (rc > 0) {
    rOffset = fileOffset + rc;
  }

  gettimeofday(&lrTime, &tz);
  AddReadTime();

  if (rc < 0) {
    // Here we might take some other action
    int envlen = 0;
    eos_crit("block-read error=%d offset=%llu len=%llu file=%s",
             error.getErrInfo(),
             static_cast<unsigned long long>(fileOffset),
             static_cast<unsigned long long>(buffer_size),
             FName(), mCapOpaque ? mCapOpaque->Env(envlen) : FName());
    hasReadError =
      true; // used to understand if a reconstruction of a RAIN file worked
  }

  eos_debug("rc=%d offset=%lu size=%llu", rc, fileOffset,
            static_cast<unsigned long long>(buffer_size));

  if ((fileOffset + buffer_size) >= openSize) {
    if (mCheckSum) {
      if (!mCheckSum->NeedsRecalculation()) {
        // If this is the last read of sequential reading, we can verify the checksum now
        if (verifychecksum()) {
          return gOFS.Emsg("read", error, EIO, "read file - wrong file checksum fn=",
                           FName());
        }
      }
    }
  }

  return rc;
}

//------------------------------------------------------------------------------
// Vector read - low level ofs method which is called from one of the
// layout plugins
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readvofs(XrdOucIOVec* readV, uint32_t readCount)
{
  eos_debug("read count=%i", readCount);
  gettimeofday(&cTime, &tz);
  XrdSfsXferSize sz = XrdOfsFile::readv(readV, readCount);
  gettimeofday(&lrvTime, &tz);
  AddReadVTime();
  // Collect monitoring info
  {
    XrdSysMutexHelper scope_lock(vecMutex);

    // If this is the last read of sequential reading, we can verify the checksum
    for (uint32_t i = 0; i < readCount; ++i) {
      monReadSingleBytes.push_back(readV[i].size);
    }

    monReadvBytes.push_back(sz);
    monReadvCount.push_back(readCount);
  }
  return sz;
}

//------------------------------------------------------------------------------
// Vector read - OFS interface method
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readv(XrdOucIOVec* readV, int readCount)
{
  eos_debug("read count=%i", readCount);
  // Copy the XrdOucIOVec structure to XrdCl::ChunkList
  uint32_t total_read = 0;
  XrdCl::ChunkList chunkList;
  chunkList.reserve(readCount);

  for (int i = 0; i < readCount; ++i) {
    total_read += (uint32_t)readV[i].size;
    chunkList.push_back(XrdCl::ChunkInfo((uint64_t)readV[i].offset,
                                         (uint32_t)readV[i].size,
                                         (void*)readV[i].data));
  }

  return layOut->ReadV(chunkList, total_read);
}

//------------------------------------------------------------------------------
// Read AIO
//------------------------------------------------------------------------------
int
XrdFstOfsFile::read(XrdSfsAio* aioparm)
{
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Write to OFS file
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::writeofs(XrdSfsFileOffset fileOffset, const char* buffer,
                        XrdSfsXferSize buffer_size)
{
  if (gOFS.Simulate_IO_write_error) {
    writeErrorFlag = kOfsSimulatedIoError;
    return gOFS.Emsg("writeofs", error, EIO, "write file - simulated IO error fn=",
                     mCapOpaque ? (mCapOpaque->Get("mgm.path") ?
                                   mCapOpaque->Get("mgm.path") : FName()) : FName());
  }

  if (mFsId) {
    if ((mTargetSize && (mTargetSize == mBookingSize)) ||
        (mBookingSize >= fileOffset + buffer_size)) {
      // Space has been successfully pre-allocated, let client write
    } else {
      // Check if the file system is full
      bool isfull = false;
      {
        XrdSysMutexHelper lock(gOFS.Storage->mFsFullMapMutex);
        isfull = gOFS.Storage->mFsFullMap[mFsId];
      }

      if (isfull) {
        writeErrorFlag = kOfsDiskFullError;
        return gOFS.Emsg("writeofs", error, ENOSPC, "write file - disk space "
                         "(headroom) exceeded fn=", mCapOpaque ?
                         (mCapOpaque->Get("mgm.path") ? mCapOpaque->Get("mgm.path") :
                          FName()) : FName());
      }
    }
  }

  if (mMaxSize) {
    // Check that the user didn't exceed the maximum file size policy
    if ((fileOffset + buffer_size) > mMaxSize) {
      writeErrorFlag = kOfsMaxSizeError;
      return gOFS.Emsg("writeofs", error, ENOSPC, "write file - your file "
                       "exceeds the maximum file size setting of bytes<=",
                       mCapOpaque ? (mCapOpaque->Get("mgm.maxsize") ?
                                     mCapOpaque->Get("mgm.maxsize") : "<undef>") : "undef");
    }
  }

  gettimeofday(&cTime, &tz);
  wCalls++;
  int rc = XrdOfsFile::write(fileOffset, buffer, buffer_size);

  if (rc != buffer_size) {
    // Tag an io error
    writeErrorFlag = kOfsIoError;
  }

  // Account seeks for monitoring
  if (wOffset != static_cast<unsigned long long>(fileOffset)) {
    if (wOffset < static_cast<unsigned long long>(fileOffset)) {
      nFwdSeeks++;
      sFwdBytes += (fileOffset - wOffset);
    } else {
      nBwdSeeks++;
      sBwdBytes += (wOffset - fileOffset);
    }

    if ((wOffset + (EOS_FSTOFS_LARGE_SEEKS)) < (static_cast<unsigned long long>
        (fileOffset))) {
      sXlFwdBytes += (fileOffset - wOffset);
      nXlFwdSeeks++;
    }

    if ((static_cast<unsigned long long>(wOffset) > (EOS_FSTOFS_LARGE_SEEKS)) &&
        (wOffset - (EOS_FSTOFS_LARGE_SEEKS)) > (static_cast<unsigned long long>
            (fileOffset))) {
      sXlBwdBytes += (wOffset - fileOffset);
      nXlBwdSeeks++;
    }
  }

  if (rc > 0) {
    if (layOut->IsEntryServer() || IsRainLayout(mLid)) {
      XrdSysMutexHelper lock(vecMutex);
      wvec.push_back(rc);
    }

    wOffset = fileOffset + rc;
  }

  gettimeofday(&lwTime, &tz);
  AddWriteTime();
  return rc;
}

//------------------------------------------------------------------------------
// Write
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::write(XrdSfsFileOffset fileOffset, const char* buffer,
                     XrdSfsXferSize buffer_size)
{
  if (mIsDevNull) {
    eos_debug("offset=%llu, length=%li discarded for sink file", fileOffset,
              buffer_size);
    maxOffsetWritten = fileOffset + buffer_size;
    return buffer_size;
  }

  int rc = layOut->Write(fileOffset, const_cast<char*>(buffer), buffer_size);

  //  EPNAME("write");
  if ((rc < 0) && isCreation && (error.getErrInfo() == EREMOTEIO)) {
    if (eos::common::LayoutId::GetLayoutType(mLid) ==
        eos::common::LayoutId::kReplica) {
      // If we see a remote IO error, we don't fail, we just call a repair
      // action afterwards (only for replica layouts!)
      repairOnClose = true;
      rc = buffer_size;
    }
  }

  // Evt. add checksum
  if ((rc > 0) && (mCheckSum)) {
    XrdSysMutexHelper cLock(ChecksumMutex);
    mCheckSum->Add(buffer,
                   static_cast<size_t>(rc),
                   static_cast<off_t>(fileOffset));
  }

  if (rc > 0) {
    if (static_cast<unsigned long long>(fileOffset + buffer_size) >
        static_cast<unsigned long long>(maxOffsetWritten)) {
      maxOffsetWritten = (fileOffset + buffer_size);
    }
  }

  mHasWrite = true;
  eos_debug("rc=%d offset=%lu size=%lu", rc, fileOffset,
            static_cast<unsigned long>(buffer_size));

  if (rc < 0) {
    int envlen = 0;

    if (!hasWriteError || EOS_LOGS_DEBUG) {
      eos_crit("block-write error=%d offset=%llu len=%llu file=%s",
               error.getErrInfo(),
               static_cast<unsigned long long>(fileOffset),
               static_cast<unsigned long long>(buffer_size),
               FName(), mCapOpaque ? mCapOpaque->Env(envlen) : FName());
    }

    hasWriteError = true;
  }

  if (rc < 0) {
    int envlen = 0;
    // Indicate the deletion flag for write errors
    writeDelete = true;
    XrdOucString errdetail;

    if (isCreation) {
      XrdOucString newerr;
      // Add to the error message that this file has been removed after the error,
      // which happens for creations
      newerr = error.getErrText();

      if (writeErrorFlag == kOfsSimulatedIoError) {
        // Simulated IO error
        errdetail += " => file has been removed because of a simulated IO error";
      } else {
        if (writeErrorFlag == kOfsDiskFullError) {
          // Disk full error
          errdetail +=
            " => file has been removed because the target filesystem  was full";
        } else {
          if (writeErrorFlag == kOfsMaxSizeError) {
            // Maximum file size error
            errdetail += " => file has been removed because the maximum target "
                         "filesize defined for that subtree was exceeded (maxsize=";
            char smaxsize[16];
            snprintf(smaxsize, sizeof(smaxsize) - 1, "%llu", (unsigned long long) mMaxSize);
            errdetail += smaxsize;
            errdetail += " bytes)";
          } else {
            if (writeErrorFlag == kOfsIoError) {
              // Generic IO error
              errdetail +=
                " => file has been removed due to an IO error on the target filesystem";
            } else {
              errdetail += " => file has been removed due to an IO error (unspecified)";
            }
          }
        }
      }

      newerr += errdetail.c_str();
      error.setErrInfo(error.getErrInfo(), newerr.c_str());
    }

    eos_err("block-write error=%d offset=%llu len=%llu file=%s error=\"%s\"",
            error.getErrInfo(),
            (unsigned long long) fileOffset,
            (unsigned long long) buffer_size, FName(),
            mCapOpaque ? mCapOpaque->Env(envlen) : FName(),
            errdetail.c_str());
  }

  return rc;
}

//------------------------------------------------------------------------------
// Write AIO
//------------------------------------------------------------------------------
int
XrdFstOfsFile::write(XrdSfsAio* aioparm)
{
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Sync OFS
//------------------------------------------------------------------------------
int
XrdFstOfsFile::syncofs()
{
  return XrdOfsFile::sync();
}

//------------------------------------------------------------------------------
// Verify if a TPC key is still valid
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::TpcValid()
{
  XrdSysMutexHelper scope_lock(gOFS.TpcMapMutex);

  if (mTpcKey.length() &&  gOFS.TpcMap[mIsTpcDst].count(mTpcKey.c_str())) {
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Sync file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::sync()
{
  static const int cbWaitTime = 1800;

  // TPC transfer
  if (mTpcFlag == kTpcDstSetup) {
    XrdSysMutexHelper scope_lock(&mTpcJobMutex);

    if (mTpcState == kTpcIdle) {
      eos_info("msg=\"tpc enabled -> 1st sync\"");
      mTpcThreadStatus = XrdSysThread::Run(&mTpcThread,
                                           XrdFstOfsFile::StartDoTpcTransfer,
                                           static_cast<void*>(this), XRDSYSTHREAD_HOLD,
                                           "TPC Transfer Thread");

      if (mTpcThreadStatus == 0) {
        mTpcState = kTpcRun;
        scope_lock.UnLock();
        return SFS_OK;
      } else {
        eos_err("msg=\"failed to start TPC job thread\"");
        mTpcState = kTpcDone;

        if (mTpcInfo.Key) {
          free(mTpcInfo.Key);
        }

        mTpcInfo.Key = strdup("Copy failed, could not start job");
        return mTpcInfo.Fail(&error, "could not start job", ECANCELED);
      }
    } else if (mTpcState == kTpcRun) {
      eos_info("msg=\"tpc running -> 2nd sync\"");

      if (mTpcInfo.SetCB(&error)) {
        return SFS_ERROR;
      }

      error.setErrCode(cbWaitTime);
      mTpcInfo.Engage();
      return SFS_STARTED;
    } else if (mTpcState == kTpcDone) {
      eos_info("msg=\"tpc already finished, retc=%i\"", mTpcRetc);

      if (mTpcRetc) {
        error.setErrInfo(mTpcRetc, (mTpcInfo.Key ? mTpcInfo.Key : "failed tpc"));
        return SFS_ERROR;
      } else {
        return SFS_OK;
      }
    } else {
      eos_err("msg=\"unknown tpc state\"");
      error.setErrInfo(EINVAL, "unknown TPC state");
      return SFS_ERROR;
    }
  } else {
    // Standard file sync
    return layOut->Sync();
  }
}

//------------------------------------------------------------------------------
// Sync
//------------------------------------------------------------------------------
int
XrdFstOfsFile::sync(XrdSfsAio* aiop)
{
  return layOut->Sync();
}

//----------------------------------------------------------------------------
// Static method used to start an asynchronous thread which is doing the TPC
// transfer
//----------------------------------------------------------------------------
void*
XrdFstOfsFile::StartDoTpcTransfer(void* arg)
{
  return reinterpret_cast<XrdFstOfsFile*>(arg)->DoTpcTransfer();
}

//------------------------------------------------------------------------------
// Run method for the thread doing the TPC transfer
//------------------------------------------------------------------------------
void*
XrdFstOfsFile::DoTpcTransfer()
{
  eos_info("msg=\"tpc now running - 1st sync\"");
  std::string src_url = "";
  std::string src_cgi = "";

  // The sync initiates the third party copy
  if (!TpcValid()) {
    eos_err("msg=\"tpc session invalidated during sync\"");
    XrdSysMutexHelper scope_lock(mTpcJobMutex);
    mTpcState = kTpcDone;
    mTpcRetc = ECONNABORTED;
    mTpcInfo.Reply(SFS_ERROR, ECONNABORTED, "sync TPC session closed by "
                   "disconnect");
    return 0;
  }

  {
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    // Construct the source URL
    src_url = "root://";
    src_url += gOFS.TpcMap[mIsTpcDst][mTpcKey.c_str()].src;
    src_url += "/";
    src_url += gOFS.TpcMap[mIsTpcDst][mTpcKey.c_str()].lfn;
    src_url += "?fst.readahead=true";
    src_cgi = "tpc.key=";
    src_cgi += mTpcKey.c_str();
    src_cgi += "&tpc.org=";
    src_cgi += gOFS.TpcMap[mIsTpcDst][mTpcKey.c_str()].org;
  }

  XrdIo tpcIO(src_url);
  tpcIO.SetLogId(logId);
  eos_info("sync-url=%s sync-cgi=%s", src_url.c_str(), src_cgi.c_str());

  if (tpcIO.fileOpen(0, 0, src_cgi)) {
    eos_err("msg=\"TPC open failed for url=%s cgi=%s\"", src_url.c_str(),
            src_cgi.c_str());
    XrdSysMutexHelper scope_lock(mTpcJobMutex);
    mTpcState = kTpcDone;
    mTpcRetc = EFAULT;
    mTpcInfo.Reply(SFS_ERROR, EFAULT, "sync - TPC open failed");
    return 0;
  }

  if (!TpcValid()) {
    tpcIO.fileClose();
    eos_err("msg=\"tpc session invalidated during sync\"");
    XrdSysMutexHelper scope_lock(mTpcJobMutex);
    mTpcState = kTpcDone;
    mTpcRetc = ECONNABORTED;
    mTpcInfo.Reply(SFS_ERROR, ECONNABORTED, "sync - TPC session closed by "
                   "disconnect");
    return 0;
  }

  int64_t rbytes = 0;
  int64_t wbytes = 0;
  off_t offset = 0;
  std::unique_ptr< std::vector<char> > buffer(
    new std::vector<char>(tpcIO.GetBlockSize()));
  eos_info("msg=\"tpc pull\" ");

  do {
    // Read the remote file in chunks and check after each chunk if the TPC
    // has been aborted already
    // @note this way of reading asynchronously in the buffer without waiting
    // for the async requests works properly only if readahead is enabled.
    // Otherwise, one must call fileWaitAsyncIO().
    rbytes = tpcIO.fileReadAsync(offset, &((*buffer)[0]),
                                 tpcIO.GetBlockSize(), true, 30);
    eos_debug("msg=\"tpc read\" rbytes=%lli request=%llu",
              rbytes, tpcIO.GetBlockSize());

    if (rbytes == -1) {
      (void) tpcIO.fileClose();
      eos_err("msg=\"tpc transfer terminated - remote read failed\"");
      XrdSysMutexHelper scope_lock(mTpcJobMutex);
      mTpcState = kTpcDone;
      mTpcRetc = EIO;
      mTpcInfo.Reply(SFS_ERROR, EIO, "sync - TPC remote read failed");
      return 0;
    }

    if (rbytes > 0) {
      // Write the buffer out through the local object
      wbytes = write(offset, &((*buffer)[0]), rbytes);
      eos_debug("msg=\"tpc write\" wbytes=%llu", wbytes);

      if (rbytes != wbytes) {
        (void) tpcIO.fileClose();
        eos_err("msg=\"tpc transfer terminated - local write failed\"");
        XrdSysMutexHelper scope_lock(mTpcJobMutex);
        mTpcState = kTpcDone;
        mTpcRetc = EIO;
        mTpcInfo.Reply(SFS_ERROR, EIO, "sync - TPC local write failed");
        return 0;
      }

      offset += rbytes;
    }

    // Check validity of the TPC key
    if (!TpcValid()) {
      (void) tpcIO.fileClose();
      eos_err("msg=\"tpc transfer invalidated during sync\"");
      XrdSysMutexHelper scope_lock(mTpcJobMutex);
      mTpcState = kTpcDone;
      mTpcRetc = ECONNABORTED;
      mTpcInfo.Reply(SFS_ERROR, ECONNABORTED, "sync - TPC session closed "
                     "by diconnect");
      return 0;
    }
  } while (rbytes > 0);

  // Close the remote file
  eos_info("done tpc transfer, close remote file and exit");
  XrdCl::XRootDStatus st = tpcIO.fileClose();
  XrdSysMutexHelper scope_lock(mTpcJobMutex);
  mTpcState = kTpcDone;
  mTpcInfo.Reply(SFS_OK, 0, "");
  return 0;
}

//------------------------------------------------------------------------------
// Truncate - low level ofs method which is called from one of the layout plugin
//------------------------------------------------------------------------------
int
XrdFstOfsFile::truncateofs(XrdSfsFileOffset fileOffset)
{
  // Truncation moves the max offset written
  eos_debug("value=%llu", (unsigned long long) fileOffset);
  maxOffsetWritten = fileOffset;
  struct stat buf;

  // stat the current file size
  // if the file has the proper size we don't truncate
  if (!::stat(mFstPath.c_str(), &buf)) {
    // if the file has the proper size we don't truncate
    if (buf.st_size == fileOffset) {
      return SFS_OK;
    }
  }

  return XrdOfsFile::truncate(fileOffset);
}

//------------------------------------------------------------------------------
// Truncate method
//------------------------------------------------------------------------------
int
XrdFstOfsFile::truncate(XrdSfsFileOffset fileOffset)
{
  eos_info("openSize=%llu fileOffset=%llu ", openSize, fileOffset);

  if (mIsDevNull) {
    return SFS_OK;
  }

  if (fileOffset != openSize) {
    mHasWrite = true;

    if (mCheckSum) {
      if (fileOffset != mCheckSum->GetMaxOffset()) {
        mCheckSum->Reset();
        mCheckSum->SetDirty();
      }
    }
  }

  return layOut->Truncate(fileOffset);
}

//------------------------------------------------------------------------------
// Stat file
//------------------------------------------------------------------------------
int
XrdFstOfsFile::stat(struct stat* buf)
{
  EPNAME("stat");
  int rc = SFS_OK;

  if (mIsDevNull) {
    buf->st_size = maxOffsetWritten;
    return rc;
  }

  if (mRainReconstruct) {
    buf->st_size = mRainSize;
    return rc;
  }

  if (layOut) {
    if ((rc = layOut->Stat(buf)))
      rc = gOFS.Emsg(epname, error, EIO, "stat - cannot stat layout to determine"
                     " file size ", mNsPath.c_str());
  } else {
    rc = gOFS.Emsg(epname, error, ENXIO, "stat - no layout to determine file size ",
                   mNsPath.c_str());
  }

  // store the file id as inode number
  if (!rc) {
    buf->st_ino = eos::common::FileId::FidToInode(mFileId);
  }

  // we store the mtime.ns time in st_dev ... sigh@Xrootd ...
#ifdef __APPLE__
  unsigned long nsec = buf->st_mtimespec.tv_nsec;
#else
  unsigned long nsec = buf->st_mtim.tv_nsec;
#endif
  // mask for 10^9
  nsec &= 0x7fffffff;
  // enable bit 32 as indicator
  nsec |= 0x80000000;
  // overwrite st_dev
  buf->st_dev = nsec;
#ifdef __APPLE__
  eos_info("path=%s inode=%lu size=%lu mtime=%lu.%lu", mNsPath.c_str(), mFileId,
           (unsigned long) buf->st_size, buf->st_mtimespec.tv_sec,
           buf->st_dev & 0x7ffffff);
#else
  eos_info("path=%s inode=%lu size=%lu mtime=%lu.%lu", mNsPath.c_str(), mFileId,
           (unsigned long) buf->st_size, buf->st_mtim.tv_sec, buf->st_dev & 0x7ffffff);
#endif
  return rc;
}

//------------------------------------------------------------------------------
// Implementation dependant commands
//------------------------------------------------------------------------------
int
XrdFstOfsFile::fctl(const int cmd, int alen, const char* args,
                    const XrdSecEntity* client)
{
  eos_debug("cmd=%i, args=%s", cmd, args);

  if (cmd == SFS_FCTL_SPEC1) {
    if (strncmp(args, "delete", alen) == 0) {
      eos_warning("Setting deletion flag for file %s", mFstPath.c_str());
      // This indicates to delete the file during the close operation
      viaDelete = true;
      return SFS_OK;
    } else if (strncmp(args, "nochecksum", alen) == 0) {
      int retc = SFS_OK;
      eos_warning("Setting nochecksum flag for file %s", mFstPath.c_str());
      mCheckSum.reset(nullptr);

      // Propagate command to all the replicas/stripes
      if (layOut) {
        retc = layOut->Fctl(std::string(args), client);
      }

      return retc;
    }
  }

  error.setErrInfo(ENOTSUP, "fctl command not supported");
  return SFS_ERROR;
}

//------------------------------------------------------------------------------
// Filter out particular tags from the opaque information
//------------------------------------------------------------------------------
void
XrdFstOfsFile::FilterTagsInPlace(std::string& opaque,
                                 const std::set<std::string> tags)
{
  bool found = false;
  std::ostringstream oss;
  std::list<std::string> tokens = eos::common::StringTokenizer::split
                                  <std::list<std::string>>(opaque, '&');

  for (const auto& token : tokens) {
    found = false;

    for (const auto& tag : tags) {
      if (token.find(tag) == 0) {
        found = true;
        break;
      }
    }

    if (!found && !token.empty()) {
      oss << token << "&";
    }
  }

  opaque = oss.str();

  if (!opaque.empty()) {
    opaque.pop_back();
  }
}

//------------------------------------------------------------------------------
// Return current mtime while open
//------------------------------------------------------------------------------
time_t
XrdFstOfsFile::GetMtime()
{
  if (!isRW) {
    // this is to report the MGM mtime to http get requests
    if (mForcedMtime != 1) {
      return mForcedMtime;
    }
  }

  if (fMd) {
    return fMd->mProtoFmd.mtime();
  } else {
    return 0;
  }
}

//------------------------------------------------------------------------------
// Return FMD checksum
//------------------------------------------------------------------------------
std::string
XrdFstOfsFile::GetFmdChecksum()
{
  return fMd->mProtoFmd.checksum();
}

//------------------------------------------------------------------------------
// Check if layout encoding indicates a RAIN layout
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::IsRainLayout(unsigned long long lid)
{
  using eos::common::LayoutId;
  unsigned long ltype = LayoutId::GetLayoutType(lid);

  if ((ltype == LayoutId::kArchive) ||
      (ltype == LayoutId::kRaidDP) ||
      (ltype == LayoutId::kRaid6)) {
    return true;
  } else {
    return false;
  }
}

//------------------------------------------------------------------------------
// Process open opaque information - this can come directly from the client
// or from the MGM redirection and it's not encrypted but sent in plain
// text in the URL
//------------------------------------------------------------------------------
int
XrdFstOfsFile::ProcessOpenOpaque()
{
  using namespace std::chrono;
  EPNAME("open");

  if (!mOpenOpaque) {
    eos_warning("msg=\"no open opaque info to process\"");
    return SFS_OK;
  }

  char* val = nullptr;

  // Handle various tags which are sent in plain text e.g. mgm.etag
  // Extract ETag from the redirection URL if available
  if ((val = mOpenOpaque->Get("mgm.etag"))) {
    mEtag = val;
  }

  // mgm.mtime=0 we set the mtime externaly. This indicates that during commit,
  // it should not update the mtime as in the case of FUSE clients which will
  // call utimes.
  if ((val = mOpenOpaque->Get("mgm.mtime"))) {
    time_t mtime = (time_t)strtoull(val, 0, 10);

    if (mtime == 0) {
      mForcedMtime = 0;
      mForcedMtime_ms = 0;
    } else {
      mForcedMtime = mtime;
      mForcedMtime_ms = 0;
    }
  }

  // mgm.fusex=1 - Suppress the file close broadcast to the fusex network
  // during the file close
  if ((val = mOpenOpaque->Get("mgm.fusex"))) {
    mFusex = true;
  }

  // Handle workflow events
  if ((val = mOpenOpaque->Get("mgm.event"))) {
    std::string event = val;

    if (event == "closew") {
      mEventOnClose = true;
    } else if (event == "sync::closew") {
      mSyncEventOnClose = true;
    }

    val = mOpenOpaque->Get("mgm.workflow");
    mEventWorkflow = (val ? val : "");
    val = mOpenOpaque->Get("mgm.instance");
    mEventInstance = val ? val : "";
    val = mOpenOpaque->Get("mgm.owner");
    mEventOwner = val ? val : "";
    val = mOpenOpaque->Get("mgm.ownergroup");
    mEventOwnerGroup = val ? val : "";
    val = mOpenOpaque->Get("mgm.requestor");
    mEventRequestor = val ? val : "";
    val = mOpenOpaque->Get("mgm.requestorgroup");
    mEventRequestorGroup = val ? val : "";
    val = mOpenOpaque->Get("mgm.attributes");
    mEventAttributes = val ? val : "";
  }

  if ((val = mOpenOpaque->Get("eos.injection"))) {
    mIsInjection = true;
  }

  // Tag as an OC chunk upload
  if (eos::common::OwnCloud::isChunkUpload(*mOpenOpaque.get())) {
    mIsOCchunk = true;
  }

  if ((val = mOpenOpaque->Get("x-upload-range"))) {
    // for partial range uploads via HTTP we run the same buisness logic like
    // for OC chunk uploads
    mIsOCchunk = true;
  }

  // Check if transfer is still valid to avoid any open replays
  if ((val = mOpenOpaque->Get("fst.valid"))) {
    try {
      std::string sval = val;
      int64_t valid_sec = std::stoll(sval);
      auto now = system_clock::now();
      auto now_sec = time_point_cast<seconds>(now).time_since_epoch().count();

      if (valid_sec < now_sec) {
        eos_err("msg=\"fst validity expired, avoid open replay\"");
        return gOFS.Emsg(epname, error, EINVAL, "open - fst validity expired",
                         mNsPath.c_str());
      }
    } catch (...) {
      // ignore
    }
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Process capability opaque information - this is encrypted information sent
// by the MGM to the FST
//------------------------------------------------------------------------------
int
XrdFstOfsFile::ProcessCapOpaque(bool& is_repair_read,
                                eos::common::Mapping::VirtualIdentity& vid)
{
  EPNAME("open");

  if (!mCapOpaque) {
    eos_warning("msg=\"no cap opaque info to process\"");
    return SFS_OK;
  }

  int envlen {0};
  eos_info("capability=%s", mCapOpaque->Env(envlen));
  char* val = nullptr;
  const char* hexfid = 0;
  const char* slid = 0;
  const char* secinfo = 0;
  const char* scid = 0;
  const char* smanager = 0;

  // Handle file id info
  if (!(hexfid = mCapOpaque->Get("mgm.fid"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no file id in capability",
                     mNsPath.c_str());
  }

  mFileId = eos::common::FileId::Hex2Fid(hexfid);

  // Handle security info
  if (!(secinfo = mCapOpaque->Get("mgm.sec"))) {
    return gOFS.Emsg(epname, error, EINVAL,
                     "open - no security information in capability", mNsPath.c_str());
  } else {
    mSecString = secinfo;
  }

  // Handle min size value
  if ((val = mCapOpaque->Get("mgm.minsize"))) {
    errno = 0;
    mMinSize = strtoull(val, 0, 10);

    if (errno) {
      eos_err("illegal minimum file size specified <%s>- restricting to 1 byte", val);
      mMinSize = 1;
    }
  } else {
    mMinSize = 0;
  }

  // Handle max size value
  if ((val = mCapOpaque->Get("mgm.maxsize"))) {
    errno = 0;
    mMaxSize = strtoull(val, 0, 10);

    if (errno) {
      eos_err("illegal maximum file size specified <%s>- restricting to 1 byte", val);
      mMaxSize = 1;
    }
  } else {
    mMaxSize = 0;
  }

  // Handle repair read flag
  if ((val = mCapOpaque->Get("mgm.repairread"))) {
    is_repair_read = true;
  }

  // Handle layout id
  if (!(slid = mCapOpaque->Get("mgm.lid"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no layout id in capability",
                     mNsPath.c_str());
  }

  mLid = atoi(slid);

  // Handle container id
  if (!(scid = mCapOpaque->Get("mgm.cid"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no container id in capability",
                     mNsPath.c_str());
  }

  mCid = strtoull(scid, 0, 10);

  // Handle the redirect manager
  if (!(smanager = mCapOpaque->Get("mgm.manager"))) {
    return gOFS.Emsg(epname, error, EINVAL, "open - no manager name in capability",
                     mNsPath.c_str());
  }

  mRedirectManager = smanager;
  {
    // evt. update the shared hash manager entry
    XrdSysMutexHelper lock(eos::fst::Config::gConfig.Mutex);
    XrdOucString ConfigManager = eos::fst::Config::gConfig.Manager;

    if (ConfigManager != mRedirectManager) {
      eos_warning("msg=\"MGM master seems to have changed - adjusting global "
                  "config\" old-manager=\"%s\" new-manager=\"%s\"",
                  ConfigManager.c_str(), mRedirectManager.c_str());
      eos::fst::Config::gConfig.Manager = mRedirectManager;
    }
  }
  // Handle virtual identity
  eos::common::Mapping::Nobody(vid);

  if ((val = mCapOpaque->Get("mgm.ruid"))) {
    vid.uid = atoi(val);
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec ruid missing",
                     mNsPath.c_str());
  }

  if ((val = mCapOpaque->Get("mgm.rgid"))) {
    vid.gid = atoi(val);
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec rgid missing",
                     mNsPath.c_str());
  }

  if ((val = mCapOpaque->Get("mgm.uid"))) {
    vid.uid_list.clear();
    vid.uid_list.push_back(atoi(val));
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec uid missing",
                     mNsPath.c_str());
  }

  if ((val = mCapOpaque->Get("mgm.gid"))) {
    vid.gid_list.clear();
    vid.gid_list.push_back(atoi(val));
  } else {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec gid missing",
                     mNsPath.c_str());
  }

  SetLogId(logId, vid, tIdent.c_str());
  return SFS_OK;
}

//----------------------------------------------------------------------------
// Process mixed opaque information - decisions that need to be taken based
// on both the ecrypted and un-encrypted opaque info
//----------------------------------------------------------------------------
int
XrdFstOfsFile::ProcessMixedOpaque()
{
  EPNAME("open");
  using eos::common::FileId;
  // Handle checksum request
  std::string opaqueCheckSum;
  char* val = nullptr;

  if (mOpenOpaque == nullptr || mCapOpaque == nullptr) {
    eos_warning("msg=\"open or cap opaque are empty\"");
    return SFS_OK;
  }

  if ((val = mOpenOpaque->Get("mgm.checksum"))) {
    opaqueCheckSum = val;
  }

  // Call the checksum factory function with the selected layout
  if (opaqueCheckSum != "ignore") {
    mCheckSum.reset(eos::fst::ChecksumPlugins::GetChecksumObject(mLid));
    eos_debug("checksum requested %d %u", mCheckSum.get(), mLid);
  }

  // Handle file system id and local prefix - If we open a replica we have to
  // take the right filesystem id and filesystem prefix for that replica
  const char* sfsid = 0;

  if (!(sfsid = mCapOpaque->Get("mgm.fsid"))) {
    return gOFS.Emsg(epname, error, EINVAL,
                     "open - no file system id in capability", mNsPath.c_str());
  }

  if (mOpenOpaque->Get("mgm.replicaindex")) {
    XrdOucString replicafsidtag = "mgm.fsid";
    replicafsidtag += (int) atoi(mOpenOpaque->Get("mgm.replicaindex"));

    if (mCapOpaque->Get(replicafsidtag.c_str())) {
      sfsid = mCapOpaque->Get(replicafsidtag.c_str());
    }
  }

  // Extract the local path prefix from the broadcasted configuration
  if (mOpenOpaque->Get("mgm.fsprefix")) {
    mLocalPrefix = mOpenOpaque->Get("mgm.fsprefix");
    mLocalPrefix.replace("#COL#", ":");
  } else {
    // Extract the local path prefix from the broadcasted configuration!
    mFsId = atoi(sfsid ? sfsid : "0");
    eos::common::RWMutexReadLock lock(gOFS.Storage->mFsMutex);

    if (mFsId && gOFS.Storage->mFileSystemsMap.count(mFsId)) {
      mLocalPrefix = gOFS.Storage->mFileSystemsMap[mFsId]->GetPath().c_str();
    }
  }

  // @note: the localprefix implementation does not work for gateway machines
  if (!mLocalPrefix.length()) {
    return gOFS.Emsg(epname, error, EINVAL, "open - cannot determine the prefix"
                     " path to use for the given filesystem id", mNsPath.c_str());
  }

  mFsId = atoi(sfsid);

  // Generate fst path
  if (mCapOpaque->Get("mgm.lpath")) {
    eos::common::FileFsPath::BuildPhysicalPath(mLocalPrefix.c_str(),
                             mCapOpaque->Get("mgm.lpath"), mFstPath);
  } else {
    FileId::FidPrefix2FullPath(FileId::Fid2Hex(mFileId).c_str(),
                               mLocalPrefix.c_str(), mFstPath);
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Process TPC (third-party copy) opaque information i.e handle tags like
// tpc.key, tpc.dst, tpc.stage etc.
//------------------------------------------------------------------------------
int
XrdFstOfsFile::ProcessTpcOpaque(std::string& opaque, const XrdSecEntity* client)
{
  EPNAME(__FUNCTION__);
  eos::common::StringConversion::ReplaceStringInPlace(opaque, "?", "&");
  eos::common::StringConversion::ReplaceStringInPlace(opaque, "&&", "&");
  XrdOucEnv env(opaque.c_str());
  std::string tpc_stage = env.Get("tpc.stage") ? env.Get("tpc.stage") : "";
  std::string tpc_key = env.Get("tpc.key") ? env.Get("tpc.key") : "";
  std::string tpc_src = env.Get("tpc.src") ? env.Get("tpc.src") : "";
  std::string tpc_dst = env.Get("tpc.dst") ? env.Get("tpc.dst") : "";
  std::string tpc_org = env.Get("tpc.org") ? env.Get("tpc.org") : "";
  std::string tpc_lfn = env.Get("tpc.lfn") ? env.Get("tpc.lfn") : "";
  // Remove any TPC flags from now on
  FilterTagsInPlace(opaque, {"tpc.stage", "tpc.key", "tpc.src", "tpc.dst",
                             "tpc.org", "tpc.lfn"
                            });

  // Determine the TPC step that we are in
  if (tpc_stage == "placement") {
    mTpcFlag = kTpcSrcCanDo;
    mIsTpcDst = false;
  } else if ((tpc_stage == "copy") && tpc_key.length() && tpc_dst.length()) {
    mTpcFlag = kTpcSrcSetup;
    mIsTpcDst = false;
  } else if ((tpc_stage == "copy") && tpc_key.length() && tpc_src.length()) {
    mTpcFlag = kTpcDstSetup;
    mIsTpcDst = true;
  } else if (tpc_key.length() && tpc_org.length()) {
    // Notice:
    // XRootD does not full follow the TPC specification and it doesn't set the
    // tpc.stage=copy in the TpcSrcRead step. The above condition should be:
    // else if ((tpc_stage == "copy") && tpc_key.length() && tpc_org.length()) {
    mTpcFlag = kTpcSrcRead;
    mIsTpcDst = false;
  }

  if ((mTpcFlag == kTpcSrcSetup) || (mTpcFlag == kTpcDstSetup)) {
    // Create a TPC entry in the TpcMap
    XrdSysMutexHelper tpc_lock(gOFS.TpcMapMutex);

    if (gOFS.TpcMap[mIsTpcDst].count(tpc_key)) {
      return gOFS.Emsg(epname, error, EPERM, "open - tpc key replayed",
                       mNsPath.c_str());
    }

    // Compute the tpc origin e.g. <name>:<pid>@<host.domain>
    // @todo(esindril) Xrootd 4.0
    // std::string origin_host = client->addrInfo->Name();
    std::string origin_host = client->host ? client->host : "<sss-auth>";
    std::string origin_tident = client->tident;
    origin_tident.erase(origin_tident.find(":"));
    tpc_org = origin_tident;
    tpc_org += "@";
    tpc_org += origin_host;
    // Store the TPC initialization
    gOFS.TpcMap[mIsTpcDst][tpc_key].key = tpc_key;
    gOFS.TpcMap[mIsTpcDst][tpc_key].org = tpc_org;
    gOFS.TpcMap[mIsTpcDst][tpc_key].src = tpc_src;
    gOFS.TpcMap[mIsTpcDst][tpc_key].dst = tpc_dst;
    gOFS.TpcMap[mIsTpcDst][tpc_key].path = mNsPath.c_str();
    gOFS.TpcMap[mIsTpcDst][tpc_key].lfn = tpc_lfn;
    // Set tpc key expiration time to 1 minute
    gOFS.TpcMap[mIsTpcDst][tpc_key].expires = time(NULL) + 60;
    mTpcKey = tpc_key.c_str();

    if (mTpcFlag == kTpcDstSetup) {
      if (!tpc_lfn.length()) {
        return gOFS.Emsg(epname, error, EINVAL, "open - tpc lfn missing",
                         mNsPath.c_str());
      }

      eos_info("msg=\"tpc dst session\" key=%s, org=%s, src=%s path=%s lfn=%s "
               "expires=%llu", gOFS.TpcMap[mIsTpcDst][tpc_key].key.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].org.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].src.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].path.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].lfn.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].expires);
    } else if (mTpcFlag == kTpcSrcSetup) {
      // Store the opaque info but without any tpc.* info
      gOFS.TpcMap[mIsTpcDst][tpc_key].opaque = opaque.c_str();
      // Store also the decoded capability info
      XrdOucEnv tmp_env(opaque.c_str());
      XrdOucEnv* cap_env {nullptr};
      int caprc = gCapabilityEngine.Extract(&tmp_env, cap_env);

      if (caprc == ENOKEY) {
        delete cap_env;
        return gOFS.Emsg(epname, error, caprc, "open - missing capability");
      } else if (caprc != 0) {
        delete cap_env;
        return gOFS.Emsg(epname, error, caprc, "open - capability illegal",
                         mNsPath.c_str());
      } else {
        int envlen = 0;
        gOFS.TpcMap[mIsTpcDst][tpc_key].capability = cap_env->Env(envlen);
        delete cap_env;
      }

      eos_info("msg=\"tpc src session\" key=%s, org=%s, dst=%s path=%s expires=%llu",
               gOFS.TpcMap[mIsTpcDst][tpc_key].key.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].org.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].dst.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].path.c_str(),
               gOFS.TpcMap[mIsTpcDst][tpc_key].expires);
    }
  } else if (mTpcFlag == kTpcSrcRead) {
    // Verify a TPC entry in the TpcMap since the destination's open can now
    // come before the transfer has been setup we have to give some time for
    // the TPC client to deposit the key the not so nice side effect is that
    // this thread stays busy during that time
    bool exists = false;

    for (size_t i = 0; i < 150; ++i) {
      {
        // Briefly take lock and release it
        XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);

        if (gOFS.TpcMap[mIsTpcDst].count(tpc_key)) {
          exists = true;
          break;
        }
      }

      if (!exists) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    XrdSysMutexHelper tpc_lock(gOFS.TpcMapMutex);
    time_t now = time(NULL);

    if (!gOFS.TpcMap[mIsTpcDst].count(tpc_key)) {
      eos_err("tpc key=%s not valid", tpc_key.c_str());
      return gOFS.Emsg(epname, error, EPERM, "open - tpc key not valid",
                       mNsPath.c_str());
    }

    if (gOFS.TpcMap[mIsTpcDst][tpc_key].expires < now) {
      eos_err("tpc key=%s expired", tpc_key.c_str());
      return gOFS.Emsg(epname, error, EPERM, "open - tpc key expired",
                       mNsPath.c_str());
    }

    // We trust 'sss' anyway and we miss the host name in the 'sss' entity
    std::string sec_prot = client->prot;

    if ((sec_prot != "sss")) {
      // Extract hostname from tident to avoid IPV4/6 fqdn mismatch errors
      std::string exp_org, cur_org;

      if (!GetHostFromTident(gOFS.TpcMap[mIsTpcDst][tpc_key].org, exp_org) ||
          !GetHostFromTident(tpc_org, cur_org)) {
        eos_err("failed to parse host from tpc_org=%s or cached_org=%s",
                tpc_org.c_str(), gOFS.TpcMap[mIsTpcDst][tpc_key].org.c_str());
        return gOFS.Emsg(epname, error, EPERM, "open - tpc origin parse error",
                         mNsPath.c_str());
      }

      if (exp_org != cur_org) {
        eos_err("tpc origin missmatch tpc_org=%s, cached_org=%s",
                tpc_org.c_str(), gOFS.TpcMap[mIsTpcDst][tpc_key].org.c_str());
        return gOFS.Emsg(epname, error, EPERM, "open - tpc origin mismatch",
                         mNsPath.c_str());
      }
    }

    eos_info("msg=\"tpc read\" key=%s, org=%s, path=%s expires=%llu",
             gOFS.TpcMap[mIsTpcDst][tpc_key].key.c_str(),
             gOFS.TpcMap[mIsTpcDst][tpc_key].org.c_str(),
             gOFS.TpcMap[mIsTpcDst][tpc_key].src.c_str(),
             gOFS.TpcMap[mIsTpcDst][tpc_key].path.c_str(),
             gOFS.TpcMap[mIsTpcDst][tpc_key].expires);
    // Grab the open information and expire entry
    mNsPath = gOFS.TpcMap[mIsTpcDst][tpc_key].path.c_str();
    opaque = gOFS.TpcMap[mIsTpcDst][tpc_key].opaque.c_str();
    SetLogId(ExtractLogId(opaque.c_str()).c_str());
    gOFS.TpcMap[mIsTpcDst][tpc_key].expires = (now - 10);
    // Store the provided origin to compare with our local connection
    // gOFS.TpcMap[mIsTpcDst][tpc_key].org = tpc_org;
    mTpcKey = tpc_key.c_str();
    // Save open opaque env
    mOpenOpaque.reset(new XrdOucEnv(opaque.c_str()));

    if (gOFS.TpcMap[mIsTpcDst][tpc_key].capability.length()) {
      mCapOpaque.reset(new XrdOucEnv(
                         gOFS.TpcMap[mIsTpcDst][tpc_key].capability.c_str()));
    } else {
      return gOFS.Emsg(epname, error, EINVAL, "open - capability not found "
                       "for tpc key %s", tpc_key.c_str());
    }
  }

  // Expire keys which are more than one 4 hours expired
  if (mTpcFlag > kTpcNone) {
    time_t now = time(NULL);
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    auto it = (gOFS.TpcMap[mIsTpcDst]).begin();
    auto del = (gOFS.TpcMap[mIsTpcDst]).begin();

    while (it != (gOFS.TpcMap[mIsTpcDst]).end()) {
      del = it;
      it++;

      if (now > (del->second.expires + (4 * 3600))) {
        eos_info("msg=\"expire tpc key\" key=%s", del->second.key.c_str());
        gOFS.TpcMap[mIsTpcDst].erase(del);
      }
    }
  }

  // For non-TPC transfer, src placement and destination TPCs we need to save
  // and decrypt the open and capability opaque info
  if ((mTpcFlag == kTpcNone) ||
      (mTpcFlag == kTpcDstSetup) ||
      (mTpcFlag == kTpcSrcCanDo)) {
    mOpenOpaque.reset(new XrdOucEnv(opaque.c_str()));
    XrdOucEnv* ptr_opaque {nullptr};
    int caprc = gCapabilityEngine.Extract(mOpenOpaque.get(), ptr_opaque);
    mCapOpaque.reset(ptr_opaque);

    if (caprc) {
      // If we just miss the key, better stall thec lient
      if (caprc == ENOKEY) {
        eos_err("msg=\"FST still misses the required capability key\"");
        return gOFS.Stall(error, 10, "FST still misses the required capability key");
      }

      return gOFS.Emsg(epname, error, caprc, "open - capability illegal",
                       mNsPath.c_str());
    }
  }

  return SFS_OK;
}

//------------------------------------------------------------------------------
// Extract logid from the opaque info
//------------------------------------------------------------------------------
std::string
XrdFstOfsFile::ExtractLogId(const char* opaque) const
{
  std::string log_id = "unknown";

  if (opaque == nullptr) {
    return log_id;
  }

  std::string sopaque = opaque;
  const std::string tag = "mgm.logid=";
  size_t pos_begin = sopaque.find(tag);

  if (pos_begin != std::string::npos) {
    pos_begin += tag.length();
    size_t pos_end = sopaque.find('&', pos_begin);

    if (pos_end != std::string::npos) {
      pos_end -= pos_begin;
    }

    log_id = sopaque.substr(pos_begin, pos_end);
  }

  return log_id;
}

//------------------------------------------------------------------------------
// Notify the workflow protobuf endpoint of closew event
//------------------------------------------------------------------------------
int
XrdFstOfsFile::NotifyProtoWfEndPointClosew(const Fmd& fmd,
    const string& ownerName,
    const string& ownerGroupName, const string& requestorName,
    const string& requestorGroupName, const string& instanceName,
    const string& fullPath, const string& managerName,
    const std::map<std::string, std::string>& xattrs, string& errMsgBack)
{
  using namespace eos::common;
  cta::xrd::Request request;
  auto notification = request.mutable_notification();
  notification->mutable_cli()->mutable_user()->set_username(requestorName);
  notification->mutable_cli()->mutable_user()->set_groupname(requestorGroupName);
  notification->mutable_file()->mutable_owner()->set_username(ownerName);
  notification->mutable_file()->mutable_owner()->set_groupname(ownerGroupName);
  notification->mutable_file()->set_size(fmd.size());
  notification->mutable_file()->mutable_cks()->set_type(
    eos::common::LayoutId::GetChecksumString(fmd.lid()));
  notification->mutable_file()->mutable_cks()->set_value(fmd.checksum());
  notification->mutable_wf()->set_event(cta::eos::Workflow::CLOSEW);
  notification->mutable_wf()->mutable_instance()->set_name(instanceName);
  notification->mutable_file()->set_lpath(fullPath);
  notification->mutable_file()->set_fid(fmd.fid());
  auto fxidString = eos::common::StringConversion::FastUnsignedToAsciiHex(
                      fmd.fid());

  std::string ctaArchiveFileId = "none";
  for (const auto& attrPair : xattrs) {
    google::protobuf::MapPair<std::string, std::string> attr(attrPair.first,
        attrPair.second);
    notification->mutable_file()->mutable_xattr()->insert(attr);

    if(attrPair.first == "CTA_ArchiveFileId") {
      ctaArchiveFileId = attrPair.second;
    }
  }

  std::ostringstream srcStream;
  srcStream << "root://" << managerName << "/" << fullPath << "?eos.lfn=fxid:"
            << fxidString;
  notification->mutable_wf()->mutable_instance()->set_url(srcStream.str());
  std::ostringstream reportStream;
  reportStream << "eosQuery://" << managerName
               << "//eos/wfe/passwd?mgm.pcmd=event&mgm.fid=" << fxidString
               << "&mgm.logid=cta&mgm.event=sync::archived&mgm.workflow=default&mgm.path=/dummy_path&mgm.ruid=0&mgm.rgid=0"
                  "&cta_archive_file_id=" << ctaArchiveFileId;
  notification->mutable_transport()->set_report_url(reportStream.str());
  std::ostringstream errorReportStream;
  errorReportStream << "eosQuery://" << managerName
                    << "//eos/wfe/passwd?mgm.pcmd=event&mgm.fid=" << fxidString
                    << "&mgm.logid=cta&mgm.event=" << ARCHIVE_FAILED_WORKFLOW_NAME
                    << "&mgm.workflow=default&mgm.path=/dummy_path&mgm.ruid=0&mgm.rgid=0"
                       "&cta_archive_file_id=" << ctaArchiveFileId
                    << "&mgm.errmsg=";
  notification->mutable_transport()->set_error_report_url(
    errorReportStream.str());

  // Communication with service
  std::string endPoint;
  std::string resource;
  {
    XrdSysMutexHelper lock(Config::gConfig.Mutex);
    endPoint = Config::gConfig.ProtoWFEndpoint;
    resource = Config::gConfig.ProtoWFResource;
  }

  if (endPoint.empty() || resource.empty()) {
    eos_static_err(
      "You are running proto wf jobs without specifying fstofs.protowfendpoint or fstofs.protowfresource in the FST config file."
    );
    return ENOTCONN;
  }

  XrdSsiPb::Config config;

  if (getenv("XRDDEBUG")) {
    config.set("log", "all");
  } else {
    config.set("log", "info");
  }

  config.set("request_timeout", "120");
  // Instantiate service object only once, static is also thread-safe
  static XrdSsiPbServiceType service(endPoint, resource, config);
  cta::xrd::Response response;

  try {
    auto sentAt = std::chrono::steady_clock::now();
    service.Send(request, response);
    auto receivedAt = std::chrono::steady_clock::now();
    auto timeSpent = std::chrono::duration_cast<std::chrono::milliseconds>
                     (receivedAt - sentAt);
    eos_static_info("SSI Protobuf time for sync::closew=%ld", timeSpent.count());
  } catch (std::runtime_error& error) {
    eos_static_err("Could not send request to outside service. Reason: %s",
                   error.what());
    return ENOTCONN;
  }

  static std::map<decltype(cta::xrd::Response::RSP_ERR_CTA), const char*>
  errorEnumMap;
  errorEnumMap[cta::xrd::Response::RSP_ERR_CTA] = "RSP_ERR_CTA";
  errorEnumMap[cta::xrd::Response::RSP_ERR_USER] = "RSP_ERR_USER";
  errorEnumMap[cta::xrd::Response::RSP_ERR_PROTOBUF] = "RSP_ERR_PROTOBUF";
  errorEnumMap[cta::xrd::Response::RSP_INVALID] = "RSP_INVALID";

  switch (response.type()) {
  case cta::xrd::Response::RSP_SUCCESS:
    return SFS_OK;

  case cta::xrd::Response::RSP_ERR_CTA:
  case cta::xrd::Response::RSP_ERR_USER:
  case cta::xrd::Response::RSP_ERR_PROTOBUF:
  case cta::xrd::Response::RSP_INVALID:
    errMsgBack = response.message_txt();
    eos_static_err("%s for file %s. Reason: %s", errorEnumMap[response.type()],
                   fullPath.c_str(), response.message_txt().c_str());
    return EPROTO;

  default:
    eos_static_err("Response:\n%s", response.DebugString().c_str());
    return EPROTO;
  }
}

//------------------------------------------------------------------------------
// Send archive failed event to the manager
//------------------------------------------------------------------------------
int XrdFstOfsFile::SendArchiveFailedToManager(const uint64_t fid,
    const std::string& errMsg)
{
  const auto fxidString = eos::common::StringConversion::FastUnsignedToAsciiHex(
                            fid);
  std::string encodedErrMsg;

  if (!common::SymKey::Base64Encode(errMsg.c_str(), errMsg.length(),
                                    encodedErrMsg)) {
    // "Failed to encode message using base64" in base64 is RmFpbGVkIHRvIGVuY29kZSBtZXNzYWdlIHVzaW5nIGJhc2U2NA==
    encodedErrMsg = "RmFpbGVkIHRvIGVuY29kZSBtZXNzYWdlIHVzaW5nIGJhc2U2NA==";
  }

  XrdOucString errorReportOpaque = "";
  errorReportOpaque += "/?";
  errorReportOpaque += "mgm.pcmd=event";
  errorReportOpaque += "&mgm.fid=";
  errorReportOpaque += fxidString.c_str();
  errorReportOpaque += "&mgm.logid=cta";
  errorReportOpaque += "&mgm.event=";
  errorReportOpaque += common::ARCHIVE_FAILED_WORKFLOW_NAME;
  errorReportOpaque += "&mgm.workflow=default";
  errorReportOpaque += "&mgm.path=/dummy_path";
  errorReportOpaque += "&mgm.ruid=0";
  errorReportOpaque += "&mgm.rgid=0";
  errorReportOpaque += "&mgm.errmsg=";
  errorReportOpaque += encodedErrMsg.c_str();
  eos_info("msg=\"sending error message to manager\" path=\"%s\" manager=\"%s\" errorReportOpaque=\"%s\"",
           mCapOpaque->Get("mgm.path"), mCapOpaque->Get("mgm.manager"),
           errorReportOpaque.c_str());
  return gOFS.CallManager(&error, mCapOpaque->Get("mgm.path"),
                          mCapOpaque->Get("mgm.manager"),
                          errorReportOpaque, nullptr, 30, mSyncEventOnClose, false);
}

//------------------------------------------------------------------------------
// Get hostname from tident
//------------------------------------------------------------------------------
bool
XrdFstOfsFile::GetHostFromTident(const std::string& tident,
                                 std::string& hostname)
{
  hostname.clear();
  size_t pos = tident.find('@');

  if ((pos == std::string::npos) || (pos + 1 == tident.length())) {
    return false;
  }

  size_t dot_pos = tident.find('.', pos + 1);
  hostname = tident.substr(pos + 1, dot_pos - pos - 1);
  return true;
}

EOSFSTNAMESPACE_END
