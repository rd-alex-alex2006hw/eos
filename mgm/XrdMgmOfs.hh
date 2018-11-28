// ----------------------------------------------------------------------
// File: XrdMgmOfs.hh
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

/*----------------------------------------------------------------------------*/
/**
 * @file   XrdMgmOfs.hh
 *
 * @brief  XRootD OFS plugin implementing meta data handling of EOS
 *
 * This class is the OFS plugin which is implemented the meta data and
 * management server part. To understand the functionality of the MGM you start
 * here. The class implements three objects, the MgmOfs object for generic
 * meta data operations, configuration and EOS thread daemon startup,
 * the MgmOfsFile class which implements operations on files - in the case of
 * an MGM this is mainly 'open' for redirection to an FST. There is one exception
 * the so called EOS 'proc' commands. Every EOS shell command is implemented as
 * an 'open for read', 'read', 'close' sequence where the open actually executes
 * an EOS shell command and the read streams the result back. For details of
 * this REST like request/response format look at the ProcInterface class and
 * the implementations in the mgm/proc directory. The XrdMgmDirectory class
 * is provided to implement the POSIX like 'open', 'readdir', 'closedir' syntax.
 * The MGM code uses mainly three global mutexes given in the order they have
 * to be used:
 * - eos::common::RWMutexXXXLock lock(FsView::gFsView.ViewMutex)  : lock 1
 * - eos::common::RWMutexXXXLock lock(gOFS->eosViewRWMutex)       : lock 2
 * - eos::common::RWMutexXXXLock lock(Quota::pMapMutex)           : lock 3
 * The XXX is either Read or Write depending what has to be done on the
 * objects they are protecting. The first mutex is the file system view object
 * (FsView.cc) which contains the current state of the storage
 * filesystem/node/group/space configuration. The second mutex is protecting
 * the quota configuration and scheduling. The last mutex is protecting the
 * namespace.
 * The implementation uses a bunch of convenience macros to cut the code short.
 * These macro's filter/map path names, apply redirection, stalling rules and
 * require certain authentication credentials to be able to run some function.
 * The MgmOfs functions are always split into the main entry function e.g.
 * "::access" and an internal function "::_access". The main function applies
 * typically the mentioned macros and converts the XRootD client identity object
 * into an EOS virtual identity. The interal function requires an EOS virtual
 * identity and contains the full implementation. This allows to apply
 * mapping & stall/redirection rules once and use the interval function
 * implementation from other main functions e.g. the "rename" function can use
 * the "_access" internal function to check some permissions etc.
 * The MGM run's the following sub-services
 * (implemented by objects and threaded daemons):
 * - Fsck
 * - Balancer
 * - Iostat
 * - Messaging
 * - Vst
 * - Deletion
 * - Filesystem Listener
 * - Httpd
 * - Recycler
 * - LRU
 * - WFE
 *
 * Many functions in the MgmOfs interface take CGI parameters. The supported
 * CGI parameter are:
 * "eos.ruid" - uid role the client wants
 * "eos.rgid" - gid role the client wants
 * "eos.space" - space a user wants to use for scheduling a write
 * "eos.checksum" - checksum a file should have
 * "eos.lfn" - use this name as path name not the path parameter (used by prefix
 * redirector MGM's ...
 * "eos.bookingsize" - reserve the requested bytes in a file placement
 * "eos.cli.access=pio" - ask for a parallel open (changes the response of an open for RAIN layouts)
 * "eos.app" - set the application name reported by monitoring
 * "eos.targetsize" - expected size of a file to be uploaded
 * "eos.blockchecksum=ignore" - disable block checksum verification
 *
 * All path related functions take as parameters 'inpath' and 'ininfo'. These
 * parameters are remapped by the NAMESPACEMAP macro to path & info variables
 * which are not visible in the declaration of each function!
 */
/*----------------------------------------------------------------------------*/

#ifndef __EOSMGM_MGMOFS__HH__
#define __EOSMGM_MGMOFS__HH__

#include "auth_plugin/Request.pb.h"
#include "common/Mapping.hh"
#include "common/Logging.hh"
#include "common/LinuxStat.hh"
#include "common/FileId.hh"
#include "common/FileSystem.hh"
#include "common/AssistedThread.hh"
#include "mq/XrdMqMessaging.hh"
#include "mgm/proc/ProcCommand.hh"
#include "mgm/drain/Drainer.hh"
#include "mgm/TapeAwareGc.hh"
#include "namespace/interface/IContainerMD.hh"
#include "namespace/ns_quarkdb/QdbContactDetails.hh"
#include <google/sparse_hash_map>
#include <chrono>
#include <mutex>

USE_EOSMGMNAMESPACE

//! Forward declaration
class XrdMgmOfsFile;
class XrdMgmOfsDirectory;
class XrdCapability;
class XrdAccAuthorize;

namespace eos
{
class IFsView;
class IFileMDSvc;
class IContainerMDSvc;
class IView;
class IFileMDChangeListener;
class IContainerMDChangeListener;
}

namespace eos
{
namespace common
{
class CommentLog;
class JeMallocHandler;
}
}

namespace eos
{
namespace mgm
{
class MgmOfsVstMessaging;
class VstMessaging;
class IConfigEngine;
class HttpServer;
class Egroup;
class GeoTreeEngine;
class ZMQ;
class Recycle;
class Iostat;
class Stat;
class WFE;
class LRU;
class Fsck;
class IMaster;
class Messaging;
class PathRouting;
}
}

namespace eos
{
namespace auth
{
class RequestProto;
}
}

namespace zmq
{
class socket_t;
class context_t;
}

//------------------------------------------------------------------------------
//! Class implementing atomic meta data commands
/*----------------------------------------------------------------------------*/
class XrdMgmOfs : public XrdSfsFileSystem, public eos::common::LogId
{
  friend class XrdMgmOfsFile;
  friend class XrdMgmOfsDirectory;
  friend class ProcCommand;

public:

  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  XrdMgmOfs(XrdSysError* lp);

  //----------------------------------------------------------------------------
  //! Destructor
  //---------------------------------------------------------------------------
  virtual ~XrdMgmOfs();

  // ---------------------------------------------------------------------------
  // Object Allocation Functions
  // ---------------------------------------------------------------------------

  //----------------------------------------------------------------------------
  //! Return a MGM directory object
  //!
  //! @param user user-name
  //! @param MonID monitor ID
  //!
  //! @return MGM directory object
  //----------------------------------------------------------------------------
  XrdSfsDirectory* newDir(char* user = 0, int MonID = 0);

  //----------------------------------------------------------------------------
  //! Return a MGM file object
  //!
  //! @param user user-name
  //! @param MonID monitor ID
  //!
  //! @return MGM file object
  //----------------------------------------------------------------------------
  XrdSfsFile* newFile(char* user = 0, int MonID = 0);

  //----------------------------------------------------------------------------
  //! Meta data functions
  //! - the _XYZ functions are the internal version of XYZ when XrdSecEntity
  //! - objects have been mapped to VirtuIdentity's.
  //----------------------------------------------------------------------------

  //----------------------------------------------------------------------------
  //! Notify filesystem that a client has disconnected.
  //!
  //! @param client client's identify (see common description)
  //----------------------------------------------------------------------------
  virtual void Disc(const XrdSecEntity* client = 0);

  //----------------------------------------------------------------------------
  // Chmod by client
  //----------------------------------------------------------------------------
  int chmod(const char* Name,
            XrdSfsMode Mode,
            XrdOucErrInfo& out_error,
            const XrdSecEntity* client = 0,
            const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // chmod by vid
  // ---------------------------------------------------------------------------
  int _chmod(const char* Name,
             XrdSfsMode& Mode,
             XrdOucErrInfo& out_error,
             eos::common::Mapping::VirtualIdentity& vid,
             const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // chown by vid
  // ---------------------------------------------------------------------------
  int _chown(const char* Name,
             uid_t uid,
             gid_t gid,
             XrdOucErrInfo& out_error,
             eos::common::Mapping::VirtualIdentity& vid,
             const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // checksum by client
  // ---------------------------------------------------------------------------
  int chksum(XrdSfsFileSystem::csFunc Func,
             const char* csName,
             const char* Path,
             XrdOucErrInfo& out_error,
             const XrdSecEntity* client = 0,
             const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // check if file exists by client
  // ---------------------------------------------------------------------------
  int exists(const char* fileName,
             XrdSfsFileExistence& exists_flag,
             XrdOucErrInfo& out_error,
             const XrdSecEntity* client = 0,
             const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // check if file exists by client bypassing authorization/mapping/bouncing
  // ---------------------------------------------------------------------------
  int _exists(const char* fileName,
              XrdSfsFileExistence& exists_flag,
              XrdOucErrInfo& out_error,
              const XrdSecEntity* client = 0,
              const char* opaque = 0);

  // ---------------------------------------------------------------------------
  //! check if file exists by vid
  // ---------------------------------------------------------------------------
  int
  _exists(const char* fileName,
          XrdSfsFileExistence& exists_flag,
          XrdOucErrInfo& out_error,
          eos::common::Mapping::VirtualIdentity& vid,
          const char* opaque = 0, bool take_lock = true);

  // ---------------------------------------------------------------------------
  // EOS plugin call fan-out function
  // ---------------------------------------------------------------------------
  int FSctl(const int cmd,
            XrdSfsFSctl& args,
            XrdOucErrInfo& error,
            const XrdSecEntity* client);

  // ---------------------------------------------------------------------------
  // fsctl
  // ---------------------------------------------------------------------------
  int fsctl(const int cmd,
            const char* args,
            XrdOucErrInfo& out_error,
            const XrdSecEntity* client = 0);

  // ---------------------------------------------------------------------------
  //! get stats function (fake ok)
  // ---------------------------------------------------------------------------

  int
  getStats(char* buff, int blen)
  {
    return 0;
  }

  //----------------------------------------------------------------------------
  //! Return the version of the MGM software
  //!
  //! @return return a version string
  //----------------------------------------------------------------------------
  const char* getVersion();


  // ---------------------------------------------------------------------------
  // create directory
  // ---------------------------------------------------------------------------
  int mkdir(const char* dirName,
            XrdSfsMode Mode,
            XrdOucErrInfo& out_error,
            const XrdSecEntity* client = 0,
            const char* opaque = 0)
  {
    return mkdir(dirName, Mode, out_error, client, opaque, 0);
  }

  int mkdir(const char* dirName,
            XrdSfsMode Mode,
            XrdOucErrInfo& out_error,
            const XrdSecEntity* client = 0,
            const char* opaque = 0,
            ino_t* outino = 0);

  // ---------------------------------------------------------------------------
  // create directory by vid
  // ---------------------------------------------------------------------------
  int _mkdir(const char* dirName,
             XrdSfsMode Mode,
             XrdOucErrInfo& out_error,
             eos::common::Mapping::VirtualIdentity& vid,
             const char* opaque = 0,
             ino_t* outino = 0);

  //----------------------------------------------------------------------------
  //! Prepare a file (EOS does nothing, only stall/redirect if configured)
  //!
  //! @return always SFS_OK
  //----------------------------------------------------------------------------
  int prepare(XrdSfsPrep& pargs,
              XrdOucErrInfo& out_error,
              const XrdSecEntity* client = 0);

  // ---------------------------------------------------------------------------
  // delete file
  // ---------------------------------------------------------------------------
  int rem(const char* path,
          XrdOucErrInfo& out_error,
          const XrdSecEntity* client = 0,
          const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // delete file by vid
  // ---------------------------------------------------------------------------
  int _rem(const char* path,
           XrdOucErrInfo& out_error,
           eos::common::Mapping::VirtualIdentity& vid,
           const char* opaque = 0,
           bool simulate = false,
           bool keepversion = false,
           bool no_recycling = false,
           bool no_quota_enforcement = false);

  //----------------------------------------------------------------------------
  //! Low-level namespace find command
  //!
  //! @param path path to start the sub-tree find
  //! @param stdErr stderr output string
  //! @param vid virtual identity of the client
  //! @param found result map/set of the find
  //! @param key search for a certain key in the extended attributes
  //! @param val search for a certain value in the extended attributes
  //!        (requires key)
  //! @param no_files if true returns only directories, otherwise files and
  //!         directories
  //! @param millisleep milli seconds to sleep between each directory scan
  //! @param nscounter if true update ns counters, otherwise don't
  //! @param maxdepth is the maximum search depth
  //! @param filematch is a pattern match for file names
  //! @param take_lock if true then take namespace lock, otherwise don't
  //!
  //! @note The find command distinuishes 'power' and 'normal' users. If the
  //! virtual identity indicates the root or admin user queries are unlimited.
  //! For others queries are by dfeault limited to 50k directories and 100k
  //! files and an appropriate error/warning message is written to stdErr.
  //!
  //! @note Find limits can be (re-)defined in the access interface by using
  //! global rules:
  //! => access set limit 100000 rate:user:*:FindFiles
  //! => access set limit 50000 rate:user:*:FindDirs
  //! or individual rules
  //! => access set limit 100000000 rate:user:eosprod:FindFiles
  //! => access set limit 100000000 rate:user:eosprod:FindDirs
  //!
  //! @note If 'key' contains a wildcard character in the end find produces a
  //! list of directories containing an attribute starting with that key match
  //! like var=sys.policy.*
  //!
  //! @note The millisleep variable allows to slow down full scans to decrease
  //! the impact when doing large scans.
  // ---------------------------------------------------------------------------
  int _find(const char* path, XrdOucErrInfo& out_error, XrdOucString& stdErr,
            eos::common::Mapping::VirtualIdentity& vid,
            std::map<std::string, std::set<std::string> >& found,
            const char* key = 0, const char* val = 0, bool no_files = false,
            time_t millisleep = 0, bool nscounter = true, int maxdepth = 0,
            const char* filematch = 0, bool take_lock = true);

  // ---------------------------------------------------------------------------
  // delete dir
  // ---------------------------------------------------------------------------
  int remdir(const char* dirName,
             XrdOucErrInfo& out_error,
             const XrdSecEntity* client = 0,
             const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // delete dir by vid
  // ---------------------------------------------------------------------------
  int _remdir(const char* dirName,
              XrdOucErrInfo& out_error,
              eos::common::Mapping::VirtualIdentity& vid,
              const char* opaque = 0,
              bool simulate = false);

  // ---------------------------------------------------------------------------
  // rename file
  // ---------------------------------------------------------------------------
  int rename(const char* oldFileName,
             const char* newFileName,
             XrdOucErrInfo& out_error,
             const XrdSecEntity* client = 0,
             const char* opaqueO = 0,
             const char* opaqueN = 0);

  // ---------------------------------------------------------------------------
  // rename file by vid
  // ---------------------------------------------------------------------------
  int rename(const char* oldFileName,
             const char* newFileName,
             XrdOucErrInfo& out_error,
             eos::common::Mapping::VirtualIdentity& vid,
             const char* opaqueO = 0,
             const char* opaqueN = 0,
             bool overwrite = false);

  // ---------------------------------------------------------------------------
  // rename file by vid
  // ---------------------------------------------------------------------------
  int _rename(const char* oldFileName,
              const char* newFileName,
              XrdOucErrInfo& out_error,
              eos::common::Mapping::VirtualIdentity& vid,
              const char* opaqueO = 0,
              const char* opaqueN = 0,
              bool updateCTime = false,
              bool checkQuota = false,
              bool overwrite = false);

  // ---------------------------------------------------------------------------
  // symlink file/dir
  // ---------------------------------------------------------------------------
  int symlink(const char* sourceName,
              const char* targetName,
              XrdOucErrInfo& out_error,
              const XrdSecEntity* client = 0,
              const char* opaqueO = 0,
              const char* opaqueN = 0);

  // ---------------------------------------------------------------------------
  // symlink file/dir by vid
  // ---------------------------------------------------------------------------
  int symlink(const char* sourceName,
              const char* targetName,
              XrdOucErrInfo& out_error,
              eos::common::Mapping::VirtualIdentity& vid,
              const char* opaqueO = 0,
              const char* opaqueN = 0,
              bool overwrite = false);

  // ---------------------------------------------------------------------------
  // symlink file/dir by vid
  // ---------------------------------------------------------------------------
  int _symlink(const char* sourceName,
               const char* targetName,
               XrdOucErrInfo& out_error,
               eos::common::Mapping::VirtualIdentity& vid,
               const char* opaqueO = 0,
               const char* opaqueN = 0);

  // ---------------------------------------------------------------------------
  // read symbolic link
  // ---------------------------------------------------------------------------
  int readlink(const char* name,
               XrdOucErrInfo& out_error,
               XrdOucString& link,
               const XrdSecEntity* client = 0,
               const char* info = 0
              );

  // ---------------------------------------------------------------------------
  // read symbolic link
  // ---------------------------------------------------------------------------
  int _readlink(const char* name,
                XrdOucErrInfo& out_error,
                eos::common::Mapping::VirtualIdentity& vid,
                XrdOucString& link
               );


  // ---------------------------------------------------------------------------
  // stat file
  // ---------------------------------------------------------------------------
  int stat(const char* Name,
           struct stat* buf,
           XrdOucErrInfo& out_error,
           std::string* etag,
           const XrdSecEntity* client = 0,
           const char* opaque = 0,
           bool follow = true,
           std::string* uri = 0
          );

  int stat(const char* Name,
           struct stat* buf,
           XrdOucErrInfo& out_error,
           const XrdSecEntity* client = 0,
           const char* opaque = 0) override;

  // ---------------------------------------------------------------------------
  // stat file and get the checksum info
  // ---------------------------------------------------------------------------
  int _getchecksum(const char* Name,
                   XrdOucErrInfo& out_error,
                   std::string* xstype,
                   std::string* xs,
                   const XrdSecEntity* client = 0,
                   const char* opaque = 0,
                   bool follow = true
                  );
  // ---------------------------------------------------------------------------
  // stat file by vid
  // ---------------------------------------------------------------------------
  int _stat(const char* Name,
            struct stat* buf,
            XrdOucErrInfo& out_error,
            eos::common::Mapping::VirtualIdentity& vid,
            const char* opaque = 0,
            std::string* etag = 0,
            bool follow = true,
            std::string* uri = 0);


  // ---------------------------------------------------------------------------
  // stat file to retrieve mode
  // ---------------------------------------------------------------------------

  int
  stat(const char* Name,
       mode_t& mode,
       XrdOucErrInfo& out_error,
       const XrdSecEntity* client = nullptr,
       const char* opaque = nullptr) override
  {
    struct stat bfr;
    int rc = stat(Name, &bfr, out_error, client, opaque);

    if (!rc) {
      mode = bfr.st_mode;
    }

    return rc;
  }

  // ---------------------------------------------------------------------------
  // stat link
  // ---------------------------------------------------------------------------
  int lstat(const char* Name,
            struct stat* buf,
            XrdOucErrInfo& out_error,
            const XrdSecEntity* client = 0,
            const char* opaque = 0);

  //----------------------------------------------------------------------------
  //! Truncate a file (not supported in EOS, only via the file interface)
  //!
  //! @return SFS_ERROR and EOPNOTSUPP
  //----------------------------------------------------------------------------
  int truncate(const char*, XrdSfsFileOffset, XrdOucErrInfo&, const XrdSecEntity*,
               const char*);

  // ---------------------------------------------------------------------------
  // check access permissions
  // ---------------------------------------------------------------------------
  int access(const char*, int mode, XrdOucErrInfo&, const XrdSecEntity*,
             const char*);

  // ---------------------------------------------------------------------------
  // check access permissions by vid
  // ---------------------------------------------------------------------------
  int _access(const char*, int mode, XrdOucErrInfo&,
              eos::common::Mapping::VirtualIdentity& vid, const char*, bool lock = true);

  // ---------------------------------------------------------------------------
  // define access permissions by vid for a file/directory
  // ---------------------------------------------------------------------------
  int acc_access(const char*,
                 XrdOucErrInfo&,
                 eos::common::Mapping::VirtualIdentity& vid,
                 std::string& accperm);

  // ---------------------------------------------------------------------------
  // set utimes
  // ---------------------------------------------------------------------------
  int utimes(const char*, struct timespec* tvp, XrdOucErrInfo&,
             const XrdSecEntity*, const char*);
  // ---------------------------------------------------------------------------
  // set utimes by vid
  // ---------------------------------------------------------------------------
  int _utimes(const char*, struct timespec* tvp, XrdOucErrInfo&,
              eos::common::Mapping::VirtualIdentity& vid, const char* opaque = 0);

  // ---------------------------------------------------------------------------
  // touch a file
  // ---------------------------------------------------------------------------
  int _touch(const char* path,
             XrdOucErrInfo& error,
             eos::common::Mapping::VirtualIdentity& vid,
             const char* ininfo = 0);

  //----------------------------------------------------------------------------
  //! List extended attributes for a given file/directory - high-level API.
  //! See _attr_ls for details.
  //!
  //! @param path file/directory name to list attributes
  //! @param out_error error object
  //! @param client XRootD authentication object
  //! @param opaque CGI
  //! @param map return object with the extended attributes, key-value map
  //!
  //! @return SFS_OK if success otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int attr_ls(const char* path, XrdOucErrInfo& out_error,
              const XrdSecEntity* client, const char* opaque,
              eos::IContainerMD::XAttrMap& map);

  //----------------------------------------------------------------------------
  //! List extended attributes for a given file/directory - low-level API.
  //!
  //! @param path file/directory name to list attributes
  //! @param out_error error object
  //! @param vid virtual identity of the client
  //! @param client XRootD authentication object
  //! @param opaque CGI
  //! @param map return object with the extended attributes, key-value map
  //! @param lock if true take the namespace lock, otherwise don't
  //! @param link if true honour sys.link attirbute, otherwise don't
  //!
  //! @return SFS_OK if success otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int _attr_ls(const char* path, XrdOucErrInfo& out_error,
               eos::common::Mapping::VirtualIdentity& vid,
               const char* opaque, eos::IContainerMD::XAttrMap& map,
               bool take_lock = true, bool links = false);


  //----------------------------------------------------------------------------
  //! Set an extended attribute for a given file/directory - high-level API.
  //! See _attr_set for details.
  //!
  //! @param path file/directory name to set attribute
  //! @param out_error error object
  //! @param client XRootD authentication object
  //! @param opaque CGI
  //! @param key key to set
  //! @param value value to set for key
  //!
  //! @return SFS_OK if success otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int attr_set(const char* path, XrdOucErrInfo& out_error,
               const XrdSecEntity* client, const char* opaque,
               const char* key, const char* value);

  // ---------------------------------------------------------------------------
  //! Set an extended attribute for a given file/directory - low-level API.
  //!
  //! @param path file/directory name to set attribute
  //! @param out_error error object
  //! @param vid virtual identity of the client
  //! @param opaque CGI
  //! @param key key to set
  //! @param value value to set for key
  //! @param take_lock if true take namespace lock, otherwise don't
  //!
  //! @return SFS_OK if success otherwise SFS_ERROR
  // ---------------------------------------------------------------------------
  int _attr_set(const char* path, XrdOucErrInfo& out_error,
                eos::common::Mapping::VirtualIdentity& vid,
                const char* opaque, const char* key, const char* value,
                bool take_lock = true);

  //----------------------------------------------------------------------------
  //! Get an extended attribute for a given entry by key - high-level API.
  //! @note Normal POSIX R_OK & X_OK permissions are required to retrieve a key
  //!
  //! @param path directory name to get attribute
  //! @param out_error error object
  //! @param client XRootD authentication object
  //! @param opaque CGI
  //! @param key key to get
  //! @param value value returned
  //!
  //! @return SFS_OK if success otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int attr_get(const char* path, XrdOucErrInfo& out_error,
               const XrdSecEntity* client, const char* opaque,
               const char* key, XrdOucString& value);

  //----------------------------------------------------------------------------
  //! Get an extended attribute for a given ContainerMD - low-level API.
  //!
  //! @param cmd directory to get attribute
  //! @param key key to get
  //! @param value value returned
  //!
  //! @return true if attribute was found, false otherwise
  //----------------------------------------------------------------------------
  bool _attr_get(eos::IContainerMD& cmd, std::string key, std::string& rvalue);

  //----------------------------------------------------------------------------
  //! Get an extended attribute for a given FileMD - low-level API.
  //!
  //! @param fmd file to get attribute
  //! @param key key to get
  //! @param value value returned
  //!
  //! @return true if attribute was found, false otherwise
  //----------------------------------------------------------------------------
  bool _attr_get(eos::IFileMD& fmd, std::string key, std::string& rvalue);

  //----------------------------------------------------------------------------
  //! Get an extended attribute for a given entry by key - low-level API.
  //!
  //! @param path directory name to get attribute
  //! @param out_error error object
  //! @param vid virtual identity of the client
  //! @param opaque CGI
  //! @param key key to get
  //! @param value value returned
  //! @param take_lock if true take namespace lock, otherwise don't
  //!
  //! @return SFS_OK if success, otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int _attr_get(const char* path, XrdOucErrInfo& out_error,
                eos::common::Mapping::VirtualIdentity& vid, const char* opaque,
                const char* key, XrdOucString& value, bool take_lock = true);

  //----------------------------------------------------------------------------
  //! Remove an extended attribute for a given entry - high-level API.
  //! See _attr_rem for details.
  //!
  //! @param path file/directory name to delete attribute
  //! @param out_error error object
  //! @param client XRootD authentication object
  //! @param opauqe CGI
  //! @param key key to delete
  //!
  //! @return SFS_OK if success, otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int attr_rem(const char* path, XrdOucErrInfo& out_error,
               const XrdSecEntity* client, const char* opaque, const char* key);

  //----------------------------------------------------------------------------
  //! Remove an extended attribute for a given entry - low-level API.
  //!
  //! @param path file/directory name to delete attribute
  //! @param out_error error object
  //! @param vid virtual identity of the client
  //! @param opauqe CGI
  //! @param key key to delete
  //!
  //! @return SFS_OK if success, otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int _attr_rem(const char* path, XrdOucErrInfo& out_error,
                eos::common::Mapping::VirtualIdentity& vid,
                const char* opaque, const char* key);

  //----------------------------------------------------------------------------
  //! Remove all extended attributes for a given file/directory - low-level API.
  //! @note Only the owner of a directory can delete extended attributes with
  //! user prefix. sys prefix attributes can be deleted only by sudo'ers or root.
  //!
  //! @param path entry path
  //! @param out_error error object
  //! @param vid virtual identity of the client
  //! @param opaque CGI
  //!
  //! @return SFS_OK if success otherwise SFS_ERROR
  //----------------------------------------------------------------------------
  int _attr_clear(const char* path, XrdOucErrInfo& out_error,
                  eos::common::Mapping::VirtualIdentity& vid,
                  const char* opaque);

  // ---------------------------------------------------------------------------
  // drop stripe by vid
  // ---------------------------------------------------------------------------
  int _dropstripe(const char* path,
                  eos::common::FileId::fileid_t fid,
                  XrdOucErrInfo& error,
                  eos::common::Mapping::VirtualIdentity& vid,
                  unsigned long fsid,
                  bool forceRemove = false);

  // ---------------------------------------------------------------------------
  // drop all stripes of a file
  // ---------------------------------------------------------------------------
  int _dropallstripes(const char* path,
                      XrdOucErrInfo& error,
                      eos::common::Mapping::VirtualIdentity& vid,
                      bool forceRemove = false);

  // ---------------------------------------------------------------------------
  // verify stripe by vid
  // ---------------------------------------------------------------------------
  int _verifystripe(const char* path,
                    XrdOucErrInfo& error,
                    eos::common::Mapping::VirtualIdentity& vid,
                    unsigned long fsid,
                    XrdOucString options);

  // ---------------------------------------------------------------------------
  // move stripe by vid
  // ---------------------------------------------------------------------------
  int _movestripe(const char* path,
                  XrdOucErrInfo& error,
                  eos::common::Mapping::VirtualIdentity& vid,
                  unsigned long sourcefsid,
                  unsigned long targetfsid,
                  bool expressflag = false);

  // ---------------------------------------------------------------------------
  // copy stripe by vid
  // ---------------------------------------------------------------------------
  int _copystripe(const char* path,
                  XrdOucErrInfo& error,
                  eos::common::Mapping::VirtualIdentity& vid,
                  unsigned long sourcefsid,
                  unsigned long targetfsid,
                  bool expressflag = false);

  // ---------------------------------------------------------------------------
  // replicate stripe by vid
  // ---------------------------------------------------------------------------
  int _replicatestripe(const char* path,
                       XrdOucErrInfo& error,
                       eos::common::Mapping::VirtualIdentity& vid,
                       unsigned long sourcefsid,
                       unsigned long targetfsid,
                       bool dropstripe = false,
                       bool expressflag = false);

  // ---------------------------------------------------------------------------
  // replicate stripe providing file meta data by vid
  // ---------------------------------------------------------------------------
  int _replicatestripe(const std::shared_ptr<eos::IFileMD> &fmd,
                       const char* path,
                       XrdOucErrInfo& error,
                       eos::common::Mapping::VirtualIdentity& vid,
                       unsigned long sourcefsid,
                       unsigned long targetfsid,
                       bool dropstripe = false,
                       bool expressflag = false);

  //----------------------------------------------------------------------------
  //! @brief Merge one file into another one
  //!
  //! @param src_path to merge
  //! @param dst_path to merge into
  //! @param error XRootD error object
  //! @param vid virtual identity
  //!
  //! @return SFS_OK if success, otherwise SFS_ERROR
  //!
  //! @note This command acts like a rename and keeps the ownership and
  //! creation time of the target file.
  //----------------------------------------------------------------------------
  int merge(const char* src_path, const char* dst_path,
            XrdOucErrInfo& error, eos::common::Mapping::VirtualIdentity& vid);

  // ---------------------------------------------------------------------------
  // create a versioned file
  // ---------------------------------------------------------------------------

  int Version(eos::common::FileId::fileid_t fileid,
              XrdOucErrInfo& error,
              eos::common::Mapping::VirtualIdentity& vid,
              int max_versions,
              XrdOucString* versionedname = 0,
              bool simulate = false);

  // ---------------------------------------------------------------------------
  // purge versioned files to max_versions
  // ---------------------------------------------------------------------------

  int PurgeVersion(const char* versiondir,
                   XrdOucErrInfo& error,
                   int max_versions);

  // ---------------------------------------------------------------------------
  // send resync command to a file system
  // ---------------------------------------------------------------------------
  int SendResync(eos::common::FileId::fileid_t fid,
                 eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  // static Mkpath is not supported
  // ---------------------------------------------------------------------------

  static int
  Mkpath(const char* path,
         mode_t mode,
         const char* info = 0,
         XrdSecEntity* client = 0,
         XrdOucErrInfo* error = 0)
  {
    return SFS_ERROR;
  }

  // ---------------------------------------------------------------------------
  // make a file sharing path with signature
  // ---------------------------------------------------------------------------

  std::string CreateSharePath(const char* path,
                              const char* info,
                              time_t expires,
                              XrdOucErrInfo& error,
                              eos::common::Mapping::VirtualIdentity& vid);

  // ---------------------------------------------------------------------------
  // verify a file sharing path with signature
  // ---------------------------------------------------------------------------

  bool VerifySharePath(const char* path,
                       XrdOucEnv* opaque);


  // ---------------------------------------------------------------------------
  // create Ofs error messsage
  // ---------------------------------------------------------------------------
  int Emsg(const char*, XrdOucErrInfo&, int, const char* x,
           const char* y = "");

  // ---------------------------------------------------------------------------
  // Configuration routine
  // ---------------------------------------------------------------------------
  virtual int Configure(XrdSysError&);

  // ---------------------------------------------------------------------------
  // Namespace file view initialization thread start function
  // ---------------------------------------------------------------------------
  static void* StaticInitializeFileView(void* arg);

  // ---------------------------------------------------------------------------
  // Namepsace file view boot function
  // ---------------------------------------------------------------------------
  void* InitializeFileView();

  // ---------------------------------------------------------------------------
  // Signal handler thread start function
  // ---------------------------------------------------------------------------
  static void* StaticSignalHandlerThread(void* arg);

  // ---------------------------------------------------------------------------
  // Signal handler thread function
  // ---------------------------------------------------------------------------
  void* SignalHandlerThread();

  //----------------------------------------------------------------------------
  //! Init function
  //!
  //! This is just kept to be compatible with standard OFS plugins, but it is
  //! not used for the moment.
  //!
  //----------------------------------------------------------------------------
  virtual bool Init(XrdSysError&);

  //----------------------------------------------------------------------------
  // Create Stall response
  //!
  //! @param error error object with text/code
  //! @param stime seconds to stall
  //! @param msg message for the client
  //!
  //! @return number of seconds of stalling
  //----------------------------------------------------------------------------
  int Stall(XrdOucErrInfo& error, int stime, const char* msg);

  //----------------------------------------------------------------------------
  //! Create Redirection response
  //!
  //! @param error error object with text/code
  //! @param host redirection target host
  //! @param port redirection target port
  //!
  //!---------------------------------------------------------------------------
  int Redirect(XrdOucErrInfo& error, const char* host, int& port);

  //----------------------------------------------------------------------------
  //! Function to test if a client based on the called function and his
  //! identity should be stalled
  //!
  //! @param function name of the function to check
  //! @param accessmode macro generated parameter defining if this is a reading
  //! or writing (namespace modifying) function
  //! @param stalltime returns the time for a stall
  //! @param stallmsg returns the message to be displayed to the user
  //!
  //! @return true if client should get a stall, otherwise false
  //!
  //! @note  The stall rules are defined by globals in the Access object
  //! (see Access.cc)
  //----------------------------------------------------------------------------
  bool ShouldStall(const char* function, int accessmode,
                   eos::common::Mapping::VirtualIdentity& vid,
                   int& stalltime, XrdOucString& stallmsg);

  //----------------------------------------------------------------------------
  //! @brief Check if a client based on the called function and his
  //! identity should be redirected. The redirection rules are defined by
  //! globals in the Access object (see Access.cc)
  //!
  //! @param function name of the function to check
  //! @param __AccessMode__ macro generated parameter defining if this is a
  //!reading or writing (namespace modifying) function
  //! @param host returns the target host of a redirection
  //! @param port returns the target port of a redirection
  //!
  //! @return true if client should get a redirected otherwise false
  //!
  //----------------------------------------------------------------------------
  bool ShouldRedirect(const char* function, int accessmode,
                      eos::common::Mapping::VirtualIdentity& vid,
                      std::string& host, int& port);

  //----------------------------------------------------------------------------
  //! @brief Test if a client based on the called function and his identity
  //! should be re-routed.
  //!
  //! @param function name of the function to check
  //! @param accessmode macro generated parameter defining if this is a
  //!        reading or writing (namespace modifying) function
  //! @param vid virtual idenity
  //! @param host target host of a redirection
  //! @param port target port of a redirection
  //! @param stall_timeout timeout value in case stalling is required
  //!
  //! @return true if client should get a redirected otherwise false
  //----------------------------------------------------------------------------
  bool ShouldRoute(const char* function, int accessmode,
                   eos::common::Mapping::VirtualIdentity& vid,
                   const char* path, const char* info,
                   std::string& host, int& port, int& stall_timeout);

  //----------------------------------------------------------------------------
  //! Test if there is stall configured for the given rule
  //!
  //! @param path the path where the rule should be checked (currently unused)
  //! @param rule the rule to check e.g. rule = "ENOENT:*" meaning we send a
  //!         stall if an entry is missing
  //! @param stalltime returns the configured time to stall
  //! @param stallmsg returns the message displayed to the client during a stall
  //! @return true if there is a stall configured otherwise false
  //!
  //! The interface is generic to check for individual paths, but currently we
  //! just implemented global rules for any paths. See Access.cc for details.
  //----------------------------------------------------------------------------
  bool HasStall(const char* path,
                const char* rule,
                int& stalltime,
                XrdOucString& stallmsg);

  //----------------------------------------------------------------------------
  //!Test if there is redirect configured for a given rule
  //!
  //! @param path the path where the rule should be checked (currently unused)
  //! @param rule the rule to check e.g. rule = "ENOENT:*" meaning we send a
  //!        redirect if an entry is missing
  //! @param host returns the redirection target host
  //! @param port returns the redirection target port
  //! @return true if there is a redirection configured otherwise false
  //!
  //! The interface is generic to check for individual paths, but currently we
  //! just implemented global rules for any paths. See Access.cc for details.
  //----------------------------------------------------------------------------
  bool HasRedirect(const char* path, const char* rule, std::string& host,
                   int& port);

  //----------------------------------------------------------------------------
  //! Check if name space is booted
  //!
  //! @return true if booted, otherwise false
  //----------------------------------------------------------------------------
  bool IsNsBooted() const;

  // ---------------------------------------------------------------------------
  // Retrieve a mapping for a given path
  // ---------------------------------------------------------------------------
  void PathRemap(const char* inpath,
                 XrdOucString& outpath);  // global namespace remapping

  //----------------------------------------------------------------------------
  //! Allows to map paths like e.g. /store/ to /eos/instance/store/ to provide
  //! an unprefixed global namespace in a storage federation. It is used by
  //! the Configuration Engine to apply a mapping from a configuration file.
  //!
  //! @parma source mapping source
  //! @param target mapping target
  //! @param store_config if true also store mapping in the configuration,
  //!        otherwise don't
  //----------------------------------------------------------------------------
  bool AddPathMap(const char* source, const char* target,
                  bool store_config = true);

  // ---------------------------------------------------------------------------
  // Reset path mapping
  // ---------------------------------------------------------------------------
  void ResetPathMap();  // reset/empty the path map

  // ---------------------------------------------------------------------------
  // Send an explicit deletion message to any fsid/fid pair
  // ---------------------------------------------------------------------------
  bool DeleteExternal(eos::common::FileSystem::fsid_t fsid,
                      unsigned long long fid);

  // ---------------------------------------------------------------------------
  // Statistics circular buffer thread startup function
  // ---------------------------------------------------------------------------
  static void* StartMgmStats(void* pp);

  //----------------------------------------------------------------------------
  //! Authentication master thread startup static function
  //!
  //! @param pp pointer to the XrdMgmOfs class
  //!
  //----------------------------------------------------------------------------
  static void* StartAuthMasterThread(void* pp);

  //----------------------------------------------------------------------------
  //! Authentication master thread function - accepts requests from EOS AUTH
  //! plugins which he then forwards to worker threads.
  //!
  //! @param assistant thread doing the work
  //----------------------------------------------------------------------------
  void AuthMasterThread(ThreadAssistant& assistant) noexcept;

  //----------------------------------------------------------------------------
  //! Authentication worker thread startup static function
  //!
  //! @param pp pointer to the XrdMgmOfs class
  //!
  //----------------------------------------------------------------------------
  static void* StartAuthWorkerThread(void* pp);

  //----------------------------------------------------------------------------
  //! Authentication worker thread function - accepts requests from the master,
  //! executed the proper action and replies with the result.
  //----------------------------------------------------------------------------
  void AuthWorkerThread();

  //----------------------------------------------------------------------------
  //! Reconnect zmq::socket object
  //!
  //! @param socket zmq socket
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool ConnectToBackend(zmq::socket_t*& socket);

  // ---------------------------------------------------------------------------
  // Signal handler for signal 40 to start profiling the heap
  // ---------------------------------------------------------------------------
  static void StartHeapProfiling(int);

  // ---------------------------------------------------------------------------
  // Signal handler for signal 41 to stop profiling the heap
  // ---------------------------------------------------------------------------
  static void StopHeapProfiling(int);

  // ---------------------------------------------------------------------------
  // Signal handler for signal 42 to dump the heap profile
  // ---------------------------------------------------------------------------
  static void DumpHeapProfile(int);

  // ---------------------------------------------------------------------------
  // Filesystem error and configuration change listener thread function
  // ---------------------------------------------------------------------------
  void FsConfigListener(ThreadAssistant& assistant) noexcept;

  //------------------------------------------------------------------------------
  //! Add backup job to the queue to be picked up by the archive/backup submitter
  //! thread.
  //!
  //! @param job_opaque string representing the opaque information necessary for
  //!        the backup operation to be executed
  //! @return true if submission successful, otherwise false
  //------------------------------------------------------------------------------
  bool SubmitBackupJob(const std::string& job_opaque);

  //------------------------------------------------------------------------------
  //! Get set of pending backups i.e. return the path of the backup operations
  //! that are still pending at the MGM.
  //!
  //! @return vector of ArchDirStatus object representing the status of the
  //!         pending backup operations
  //------------------------------------------------------------------------------
  std::vector<ProcCommand::ArchDirStatus>
  GetPendingBkps();

  //------------------------------------------------------------------------------
  //! Discover/search for a service provided to the plugins by the platform
  //!
  //! @param svc_name name of the service the plugin wants to use
  //! @param opaque parameter for the service or reference to returned discvoery
  //!        service info
  //!
  //! @return 0 if successful, otherwise errno
  //------------------------------------------------------------------------------
  static int32_t DiscoverPlatformServices(const char* svc_name, void* opaque);

  //------------------------------------------------------------------------------
  //! Write an report log record about final deletion
  //!
  //! @param fmd meta data
  //------------------------------------------------------------------------------
  void WriteRmRecord(const std::shared_ptr<eos::IFileMD>& fmd);

  //------------------------------------------------------------------------------
  //! Write an report log record about deletion into recycle bin
  //!
  //! @param fmd meta data
  //------------------------------------------------------------------------------
  void WriteRecycleRecord(const std::shared_ptr<eos::IFileMD>& fmd);

  //----------------------------------------------------------------------------
  //! Wait until namespace is initialized - thread cancellation point
  //----------------------------------------------------------------------------
  void WaitUntilNamespaceIsBooted();

  //----------------------------------------------------------------------------
  //! Check if a host was tried in an URL already with the given error
  //----------------------------------------------------------------------------
  bool Tried(XrdCl::URL& url, std::string& host, const char* serr);

  //----------------------------------------------------------------------------
  //! Wait until namespace is initialized - thread cancellation point
  //!
  //! @param assistant reference to thread executing the job
  //----------------------------------------------------------------------------
  void WaitUntilNamespaceIsBooted(ThreadAssistant& assistant);

  //----------------------------------------------------------------------------
  // Configuration variables
  //----------------------------------------------------------------------------
  char* ConfigFN; ///< name of the configuration file
  IConfigEngine* ConfEngine; ///< storing/restoring configuration
  //! Authorization module for token encryption/decryption
  XrdCapability* CapabilityEngine;
  uint64_t mCapabilityValidity; ///< Time in seconds the capability is valid
  XrdOucString MgmOfsBroker; ///< Url of the message broker without MGM subject
  XrdOucString MgmOfsBrokerUrl; ///< Url of the message broker with MGM subject
  XrdOucString MgmOfsVstBrokerUrl; ///< Url of the message broker
  XrdOucString MgmArchiveDstUrl; ////< URL where all archives are saved
  XrdOucString MgmArchiveSvcClass; ////< CASTOR svcClass for archive transfers
  Messaging* MgmOfsMessaging; ///< messaging interface class
  VstMessaging* MgmOfsVstMessaging; ///< admin messaging interface class
  //! Queue where we are sending to by default
  XrdOucString MgmDefaultReceiverQueue;
  XrdOucString MgmOfsName; ///< mount point of the filesystem
  XrdOucString MgmOfsAlias; ///< alias of this MGM instance
  //! Xrootd port where redirections go on the FSTs -default is 1094
  XrdOucString MgmOfsTargetPort;
  XrdOucString MgmOfsQueue; ///< our mgm queue name
  XrdOucString MgmOfsInstanceName; ///< name of the EOS instance
  XrdOucString MgmConfigDir; ///< Directory where config files are stored
  ///< Name of the automatically loaded configuration file
  XrdOucString MgmConfigAutoLoad;
  //! Directory where tmp. archive transfer files are saved
  XrdOucString MgmArchiveDir;
  XrdOucString MgmProcPath; ///< Directory with proc files
  //! Directory with conversion files (used as temporary files when a layout
  //! is changed using third party copy)
  XrdOucString MgmProcConversionPath;
  XrdOucString MgmProcWorkflowPath; ///< Directory with worflows
  XrdOucString MgmProcLockPath; ///< Directory with client locks
  XrdOucString MgmProcDelegationPath; ///< Directory with client delegations
  //! Full path to the master indication proc file
  XrdOucString MgmProcMasterPath;
  XrdOucString MgmProcArchivePath; ///< EOS directory where archive dir inodes
  ///< are saved for fast find functionality
  //! Path to namespace changelog file for files
  XrdOucString MgmNsFileChangeLogFile;
  ///< Path to namespace changelog file for directories
  XrdOucString MgmNsDirChangeLogFile;
  XrdOucString MgmConfigQueue; ///< name of the mgm-wide broadcasted shared hash
  ///< Name of the cluster-wide broadcasted shared hash
  XrdOucString AllConfigQueue;
  XrdOucString FstConfigQueue; ///< name of the fst-wide broadcasted shared hash
  //! Name of the prefix for space configuration
  XrdOucString SpaceConfigQueuePrefix;
  //! Name of the prefix for node configuration
  XrdOucString NodeConfigQueuePrefix;
  ///< Name of the prefix for group configuration
  XrdOucString GroupConfigQueuePrefix;
  //!  Directory containing the transfer database and archive
  XrdOucString MgmTxDir;
  XrdOucString MgmAuthDir; ///< Directory containing exported authentication token
  XrdOucString ManagerId; ///< manager id in <host>:<port> format
  XrdOucString ManagerIp; ///< manager ip in <xxx.yyy.zzz.vvv> format
  int ManagerPort; ///< manager port as number e.g. 1094
  XrdOucString MgmOfsConfigEngineType; //type of ConfigEngine ( file or quarkdb)
  std::string
  ProtoWFEndPoint; ///< host and port of service to communicate with in case of proto workflows (typically CTA frontend)
  std::string
  ProtoWFResource; ///< endpoint of SSI service to communicate with in case of proto workflows (typically CTA frontend)
  //! Process state after namespace load time
  eos::common::LinuxStat::linux_stat_t LinuxStatsStartup;
  //! Map with scheduled fids for draining
  std::map<eos::common::FileId::fileid_t, time_t> ScheduledToDrainFid;
  XrdSysMutex ScheduledToDrainFidMutex; ///< mutex protecting ScheduledToDrainFid
  //! Map with scheduled fids for balancing
  std::map<eos::common::FileId::fileid_t, time_t> ScheduledToBalanceFid;
  //! Mutex protecting ScheduledToBalanceFid
  XrdSysMutex ScheduledToBalanceFidMutex;
  char* HostName; ///< our hostname as derived in XrdOfs
  char* HostPref; ///< our hostname as derived in XrdOfs without domain

  static XrdSysError* eDest; ///< error routing object

  //----------------------------------------------------------------------------
  // Namespace specific variables
  //----------------------------------------------------------------------------
  enum eNamespace {
    kDown = 0, kBooting = 1, kBooted = 2, kFailed = 3, kCompacting = 4
  };

  std::atomic<int> mInitialized; ///< Initialization state of the namespace
  std::atomic<time_t> mFileInitTime; ///< Time for the file initialization
  std::atomic<time_t> mTotalInitTime; ///< Time for entire initialization
  std::atomic<time_t> mStartTime; ///< Timestamp when daemon started
  bool Shutdown; ///< true if the shutdown function was called => avoid to join some threads
  //! Const strings to print the namespace boot state as in eNamespace
  static const char* gNameSpaceState[];

  //----------------------------------------------------------------------------
  // State variables
  //----------------------------------------------------------------------------
  //! Next free file id after namespace boot
  std::atomic<uint64_t> mBootFileId;
  ///< Next free container id after namespace boot
  std::atomic<uint64_t> mBootContainerId;
  bool IsRedirect; ///< true if the Redirect function should be called to redirect
  bool IsStall; ///< true if the Stall function should be called to send a wait
  bool mAuthorize; ///< Determine if the autorization should be applied or not
  std::string mAuthLib; ///< Path to authorization library
  bool IssueCapability; ///< defines if the Mgm issues capabilities
  //!  Acts only as a redirector, disables many components in the MGM
  bool MgmRedirector;
  //! Mgm writes error log with cluster collected file into
  //! /var/log/eos/error.log if <true>
  bool ErrorLog;
  bool NsInQDB = true; ///< True if we're using the QDB namespace.

  //----------------------------------------------------------------------------
  // Namespace variables
  //----------------------------------------------------------------------------
  eos::IContainerMDSvc* eosDirectoryService; ///< changelog for directories
  eos::IFileMDSvc* eosFileService; ///< changelog for files
  eos::IView* eosView; ///< hierarchical view of the namespace
  eos::IFsView* eosFsView; ///< filesystem view of the namespace
  eos::IFileMDChangeListener* eosContainerAccounting; ///< subtree accoutning
  //! Subtree mtime propagation
  eos::IContainerMDChangeListener* eosSyncTimeAccounting;
  eos::common::RWMutex eosViewRWMutex; ///< rw namespace mutex
  XrdOucString
  MgmMetaLogDir; //  Directory containing the meta data (change) log files

  // ---------------------------------------------------------------------------
  // thread variables
  // ---------------------------------------------------------------------------
  pthread_t mStatsTid; ///< Thread Id of the stats thread
  AssistedThread mFsConfigTid; ///< Fs listener/config change thread
  AssistedThread mAuthMasterTid; ///< Thread Id of the authentication thread
  std::vector<pthread_t> mVectTid; ///< vector of auth worker threads ids

  //----------------------------------------------------------------------------
  // Authentication plugin variables like the ZMQ front end port number and the
  // number of worker threads available at the MGM
  //----------------------------------------------------------------------------
  unsigned int mFrontendPort; ///< frontend port number for incoming requests
  unsigned int mNumAuthThreads; ///< max number of auth worker threads
  zmq::context_t* mZmqContext; ///< ZMQ context for all the sockets
  ZMQ* zMQ; ///< ZMQ processor

  //! Autentication response time statistics
  struct AuthStats {
    std::int64_t mNumSamples;
    std::int64_t mMax; ///< Max milliseconds
    std::int64_t mMin; ///< Min milliseconds
    double mVariance;
    double mMean;
  };

  std::mutex mAuthStatsMutex; ///< Mutex protecting authenticaton stats
  //! Map of operation types to duration
  std::map<eos::auth::RequestProto_OperationType,
      std::list<std::int64_t> > mAuthSamples;
  //! Map of operation types to aggregate info response times
  std::chrono::steady_clock::time_point mLastTimestamp;
  std::map<eos::auth::RequestProto_OperationType, AuthStats>
  mAuthAggregate;

  //----------------------------------------------------------------------------
  //! Collect statistics for authentication response times
  //!
  //! @param op operation type
  //! @param duration request duration
  //----------------------------------------------------------------------------
  void AuthCollectInfo(eos::auth::RequestProto_OperationType op,
                       std::int64_t ms_duration);

  //----------------------------------------------------------------------------
  //! Compute stats for the provided samples
  //!
  //! @param lst_samples list of samples
  //!
  //! @return statistics structure
  //----------------------------------------------------------------------------
  AuthStats AuthComputeStats(const std::list<std::int64_t>&
                             lst_samples) const;


  //----------------------------------------------------------------------------
  //! Update aggregate info with the latest samples
  //!
  //! @param stats statistics structure to be updated
  //! @param lst_samples list of samples
  //----------------------------------------------------------------------------
  void AuthUpdateAggregate(AuthStats& stats, const
                           std::list<std::int64_t>& lst_samples) const;

  //----------------------------------------------------------------------------
  //! Print statistics about authentication performance - needs to be called
  //! with the mutex lock
  //!
  //! @return statistics data
  //----------------------------------------------------------------------------
  std::string AuthPrintStatistics() const;

  //----------------------------------------------------------------------------
  //! Cast a change message to all fusex clients regarding a file change
  //!
  //! @param id file identifier
  //----------------------------------------------------------------------------
  void FuseXCastFile(eos::FileIdentifier id);

  //----------------------------------------------------------------------------
  //! Cast a change message to all fusex clients regarding a container change
  //!
  //! @param id container identifier
  //----------------------------------------------------------------------------
  void FuseXCastContainer(eos::ContainerIdentifier id);

  //----------------------------------------------------------------------------
  //! Cast a change message to all fusex clients regarding deletion of a name
  //!
  //! @param id container identifier
  //! @param name directory/file name to delete
  //!
  //----------------------------------------------------------------------------
  void FuseXCastDeletion(eos::ContainerIdentifier id,
                         const std::string& name);

  //----------------------------------------------------------------------------
  //! Setup /eos/<instance>/proc files
  //----------------------------------------------------------------------------
  void SetupProcFiles();

  //----------------------------------------------------------------------------
  //! Method called during shutdown to destroy the rest of the objects and
  //! clean up the threads.
  //----------------------------------------------------------------------------
  void OrderlyShutdown();

  //----------------------------------------------------------------------------
  // Class objects
  //----------------------------------------------------------------------------
  XrdAccAuthorize* Authorization = nullptr; ///< Authorization service

  //! Mgm Namespace Statistics
  std::unique_ptr<Stat> MgmStatsPtr;
  Stat& MgmStats;
  std::unique_ptr<Iostat> IoStats; ///<  Mgm IO Statistics

  //! Mgm IO Report store path by default is /var/tmp/eos/report
  XrdOucString IoReportStorePath;

  //! Class implementing comment log: mgm writes all proc commands with a
  //! comment into /var/log/eos/comments.log
  eos::common::CommentLog* mCommentLog;

  //! Class checking the filesystem
  std::unique_ptr<Fsck> FsckPtr;
  Fsck& FsCheck;

  //! Master/Slave configuration/failover class
  std::unique_ptr<IMaster> mMaster;

  //! Map storing the last time of a filesystem dump, this information is used
  //! to track filesystems which have not been checked decentral by an FST.
  //! It is filled in the 'dumpmd' function definde in Procinterface
  std::map<eos::common::FileSystem::fsid_t, time_t> DumpmdTimeMap;
  XrdSysMutex DumpmdTimeMapMutex; ///< mutex protecting the 'dumpmd' time

  //! Global path remapping
  std::map<std::string, std::string> PathMap;
  eos::common::RWMutex PathMapMutex; ///< mutex protecting the path map

  //! Global path routing
  std::unique_ptr<PathRouting> mRouting; ///< Path routing mechanism

  XrdMqSharedObjectManager ObjectManager; ///< Shared Hash/Queue ObjectManager
  //! Shared Hash/Queue Object Change Notifier
  XrdMqSharedObjectChangeNotifier ObjectNotifier;
  Drainer mDrainEngine; ///< Centralized draning
  bool mIsCentralDrain; ///< Flag to mark central draining
  std::unique_ptr<HttpServer> Httpd; ///<  Http daemon if available

  //! LRU object running the LRU policy engine
  std::unique_ptr<LRU> LRUPtr;
  LRU& LRUd;

  //! WFE object running the WFE engine
  std::unique_ptr<WFE> WFEPtr;
  WFE& WFEd;

  //!  Egroup refresh object running asynchronous Egroup fetch thread
  std::unique_ptr<Egroup> EgroupRefresh;
  //!  Recycle object running the recycle bin deletion thread
  std::unique_ptr<Recycle> Recycler;
  bool UTF8; ///< true if running in less restrictive character set mode

  std::string mArchiveEndpoint; ///< archive ZMQ connection endpoint
  std::string mFstGwHost; ///< FST gateway redirect fqdn host
  int mFstGwPort; ///< FST gateway redirect port, default 1094
  std::string mQdbCluster; ///< Quarkdb cluster info host1:port1 host2:port2 ..
  std::string mQdbPassword; ///< Quarkdb cluster password
  eos::QdbContactDetails mQdbContactDetails; ///< QuarkDB contact details
  int mHttpdPort; ///< port of the http server, default 8000
  int mFusexPort; ///< port of the FUSEX brocasz MQZ, default 1100
  bool mTapeAwareGcDefaultSpaceEnable; ///< Flag to mark if tape aware garbage collection should be enabled
  TapeAwareGc& mTapeAwareGc; ///< Tape aware garbage collector

private:
  std::map<std::string, XrdMgmOfsDirectory*>
  mMapDirs; ///< uuid to directory obj. mapping
  std::map<std::string, XrdMgmOfsFile*> mMapFiles; ///< uuid to file obj. mapping
  XrdSysMutex mMutexDirs; ///< mutex for protecting the access at the dirs map
  XrdSysMutex mMutexFiles; ///< mutex for protecting the access at the files map
  AssistedThread mSubmitterTid; ///< Archive submitter thread
  XrdSysMutex mJobsQMutex; ///< Mutex for archive/backup job queue
  std::list<std::string> mPendingBkps; ///< Backup jobs queueRequest
  //! Manage heap profiling
  std::unique_ptr<eos::common::JeMallocHandler> mJeMallocHandler;
  std::atomic<bool> mDoneOrderlyShutdown; ///< Mark for orderly shutdown

  //----------------------------------------------------------------------------
  //! Check that the auth ProtocolBuffer request has not been tampered with
  //!
  //! @param reqProto request ProtocolBuffer from an authentication plugin
  //!
  //! @return true if request is valid, otherwise false
  //----------------------------------------------------------------------------
  bool ValidAuthRequest(eos::auth::RequestProto* reqProto);

  //----------------------------------------------------------------------------
  //! Initialize MGM statistics to 0
  //----------------------------------------------------------------------------
  void InitStats();

  //----------------------------------------------------------------------------
  //! Start a thread that will queue, build and submit backup operations to
  //! the archiver daemon.
  //!
  //! @param arg mgm object
  //----------------------------------------------------------------------------
  void StartArchiveSubmitter(ThreadAssistant& assistant) noexcept;

  //----------------------------------------------------------------------------
  //! Setup MGM configuration directory
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool SetupConfigDir();

  static std::string MacroStringError(int errcode);

  //----------------------------------------------------------------------------
  // FS control functions
  // These functions are used in EOS to implement many stateless operations
  // such as commit/drop a replica, stat a file/directory,
  // create a directory listing for FUSE, chmod, chown, access, utimes
  // get checksum, schedule drain/balance/delete, etc.
  //
  // All of these functions will receive the following parameters:
  //
  // @param path   - path associated with this request
  // @param ininfo - opaque info associated with this request
  // @param env    - environment constructed from opaque info
  // @param error  - error reporting object
  // @param LogId  - thread logging object
  // @param vid    - mapped virtual identity
  // @param client - client security entity object
  //----------------------------------------------------------------------------

  //----------------------------------------------------------------------------
  //! Check access rights
  //----------------------------------------------------------------------------
  int Access(const char* path,
             const char* ininfo,
             XrdOucEnv& env,
             XrdOucErrInfo& error,
             eos::common::LogId& ThreadLogId,
             eos::common::Mapping::VirtualIdentity& vid,
             const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Adjust replica (repairOnClose from FST)
  //----------------------------------------------------------------------------
  int AdjustReplica(const char* path,
                    const char* ininfo,
                    XrdOucEnv& env,
                    XrdOucErrInfo& error,
                    eos::common::LogId& ThreadLogId,
                    eos::common::Mapping::VirtualIdentity& vid,
                    const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Get checksum of file
  //----------------------------------------------------------------------------
  int Checksum(const char* path,
               const char* ininfo,
               XrdOucEnv& env,
               XrdOucErrInfo& error,
               eos::common::LogId& ThreadLogId,
               eos::common::Mapping::VirtualIdentity& vid,
               const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Chmod of a directory
  //----------------------------------------------------------------------------
  int Chmod(const char* path,
            const char* ininfo,
            XrdOucEnv& env,
            XrdOucErrInfo& error,
            eos::common::LogId& ThreadLogId,
            eos::common::Mapping::VirtualIdentity& vid,
            const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Chown of a file or directory
  //----------------------------------------------------------------------------
  int Chown(const char* path,
            const char* ininfo,
            XrdOucEnv& env,
            XrdOucErrInfo& error,
            eos::common::LogId& ThreadLogId,
            eos::common::Mapping::VirtualIdentity& vid,
            const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Commit a replica
  //----------------------------------------------------------------------------
  int Commit(const char* path,
             const char* ininfo,
             XrdOucEnv& env,
             XrdOucErrInfo& error,
             eos::common::LogId& ThreadLogId,
             eos::common::Mapping::VirtualIdentity& vid,
             const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Drop a replica
  //----------------------------------------------------------------------------
  int Drop(const char* path,
           const char* ininfo,
           XrdOucEnv& env,
           XrdOucErrInfo& error,
           eos::common::LogId& ThreadLogId,
           eos::common::Mapping::VirtualIdentity& vid,
           const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Trigger an event
  //----------------------------------------------------------------------------
  int Event(const char* path,
            const char* ininfo,
            XrdOucEnv& env,
            XrdOucErrInfo& error,
            eos::common::LogId& ThreadLogId,
            eos::common::Mapping::VirtualIdentity& vid,
            const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Stat a file or directory.
  //! Will redirect to the RW master.
  //----------------------------------------------------------------------------
  int FuseStat(const char *path,
               const char *ininfo,
               XrdOucEnv &env,
               XrdOucErrInfo &error,
               eos::common::LogId &ThreadLogId,
               eos::common::Mapping::VirtualIdentity &vid,
               const XrdSecEntity *client);

  //----------------------------------------------------------------------------
  //! Fuse extension.
  //! Will redirect to the RW master.
  //----------------------------------------------------------------------------
  int Fusex(const char* path,
            const char* ininfo,
            std::string protobuf,
            XrdOucEnv& env,
            XrdOucErrInfo& error,
            eos::common::LogId& ThreadLogId,
            eos::common::Mapping::VirtualIdentity& vid,
            const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Return metadata in env representation
  //----------------------------------------------------------------------------
  int Getfmd(const char* path,
             const char* ininfo,
             XrdOucEnv& env,
             XrdOucErrInfo& error,
             eos::common::LogId& ThreadLogId,
             eos::common::Mapping::VirtualIdentity& vid,
             const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Handle file importation into the namespace operation
  //----------------------------------------------------------------------------
  int Import(const char* path,
             const char* ininfo,
             XrdOucEnv& env,
             XrdOucErrInfo& error,
             eos::common::LogId& ThreadLogId,
             eos::common::Mapping::VirtualIdentity& vid,
             const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Query to determine if current node is acting as master
  //----------------------------------------------------------------------------
  int IsMaster(const char* path,
               const char* ininfo,
               XrdOucEnv& env,
               XrdOucErrInfo& error,
               eos::common::LogId& ThreadLogId,
               eos::common::Mapping::VirtualIdentity& vid,
               const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Received signal to bounce everything to the remote master
  //----------------------------------------------------------------------------
  int MasterSignalBounce(const char* path,
                         const char* ininfo,
                         XrdOucEnv& env,
                         XrdOucErrInfo& error,
                         eos::common::LogId& ThreadLogId,
                         eos::common::Mapping::VirtualIdentity& vid,
                         const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Received signal from remote master to reload namespace
  //----------------------------------------------------------------------------
  int MasterSignalReload(const char* path,
                         const char* ininfo,
                         XrdOucEnv& env,
                         XrdOucErrInfo& error,
                         eos::common::LogId& ThreadLogId,
                         eos::common::Mapping::VirtualIdentity& vid,
                         const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Make a directory and return its inode
  //----------------------------------------------------------------------------
  int Mkdir(const char* path,
            const char* ininfo,
            XrdOucEnv& env,
            XrdOucErrInfo& error,
            eos::common::LogId& ThreadLogId,
            eos::common::Mapping::VirtualIdentity& vid,
            const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Parallel IO mode open
  //----------------------------------------------------------------------------
  int Open(const char* path,
           const char* ininfo,
           XrdOucEnv& env,
           XrdOucErrInfo& error,
           eos::common::LogId& ThreadLogId,
           eos::common::Mapping::VirtualIdentity& vid,
           const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Resolve symbolic link
  //----------------------------------------------------------------------------
  int Readlink(const char* path,
               const char* ininfo,
               XrdOucEnv& env,
               XrdOucErrInfo& error,
               eos::common::LogId& ThreadLogId,
               eos::common::Mapping::VirtualIdentity& vid,
               const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Get open redirect
  //----------------------------------------------------------------------------
  int Redirect(const char* path,
               const char* ininfo,
               XrdOucEnv& env,
               XrdOucErrInfo& error,
               eos::common::LogId& ThreadLogId,
               eos::common::Mapping::VirtualIdentity& vid,
               const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Repair file.
  //! Used to repair after scan error (E.g.: use the converter to rewrite)
  //----------------------------------------------------------------------------
  int Rewrite(const char* path,
              const char* ininfo,
              XrdOucEnv& env,
              XrdOucErrInfo& error,
              eos::common::LogId& ThreadLogId,
              eos::common::Mapping::VirtualIdentity& vid,
              const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Schedule a balance transfer
  //----------------------------------------------------------------------------
  int Schedule2Balance(const char* path,
                       const char* ininfo,
                       XrdOucEnv& env,
                       XrdOucErrInfo& error,
                       eos::common::LogId& ThreadLogId,
                       eos::common::Mapping::VirtualIdentity& vid,
                       const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Schedule deletion for FSTs
  //----------------------------------------------------------------------------
  int Schedule2Delete(const char* path,
                      const char* ininfo,
                      XrdOucEnv& env,
                      XrdOucErrInfo& error,
                      eos::common::LogId& ThreadLogId,
                      eos::common::Mapping::VirtualIdentity& vid,
                      const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Schedule a drain transfer
  //----------------------------------------------------------------------------
  int Schedule2Drain(const char* path,
                     const char* ininfo,
                     XrdOucEnv& env,
                     XrdOucErrInfo& error,
                     eos::common::LogId& ThreadLogId,
                     eos::common::Mapping::VirtualIdentity& vid,
                     const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Virtual filesystem stat
  //----------------------------------------------------------------------------
  int Statvfs(const char* path,
             const char* ininfo,
             XrdOucEnv& env,
             XrdOucErrInfo& error,
             eos::common::LogId& ThreadLogId,
             eos::common::Mapping::VirtualIdentity& vid,
             const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Create symbolic link
  //----------------------------------------------------------------------------
  int Symlink(const char* path,
              const char* ininfo,
              XrdOucEnv& env,
              XrdOucErrInfo& error,
              eos::common::LogId& ThreadLogId,
              eos::common::Mapping::VirtualIdentity& vid,
              const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Set transfer state and log
  //----------------------------------------------------------------------------
  int Txstate(const char* path,
              const char* ininfo,
              XrdOucEnv& env,
              XrdOucErrInfo& error,
              eos::common::LogId& ThreadLogId,
              eos::common::Mapping::VirtualIdentity& vid,
              const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Set utimes
  //----------------------------------------------------------------------------
  int Utimes(const char* path,
             const char* ininfo,
             XrdOucEnv& env,
             XrdOucErrInfo& error,
             eos::common::LogId& ThreadLogId,
             eos::common::Mapping::VirtualIdentity& vid,
             const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Get EOS version and features
  //----------------------------------------------------------------------------
  int Version(const char* path,
              const char* ininfo,
              XrdOucEnv& env,
              XrdOucErrInfo& error,
              eos::common::LogId& ThreadLogId,
              eos::common::Mapping::VirtualIdentity& vid,
              const XrdSecEntity* client);

  //----------------------------------------------------------------------------
  //! Extended attribute operations
  //----------------------------------------------------------------------------
  int Xattr(const char* path,
            const char* ininfo,
            XrdOucEnv& env,
            XrdOucErrInfo& error,
            eos::common::LogId& ThreadLogId,
            eos::common::Mapping::VirtualIdentity& vid,
            const XrdSecEntity* client);
};

extern XrdMgmOfs* gOFS; //< global handle to XrdMgmOfs object

#endif
