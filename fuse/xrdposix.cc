//------------------------------------------------------------------------------
// File: xrdposix.cc
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
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/************************************************************************/
/* Based on 'xrdposix.cc' XRootD Software                               */
/* xrdposix.cc                                                          */
/*                                                                      */
/* Author: Wei Yang (Stanford Linear Accelerator Center, 2007)          */
/*                                                                      */
/* C wrapper to some of the Xrootd Posix library functions              */
/*                                                                      */
/* Modified: Andreas-Joachim Peters (CERN,2008) XCFS                    */
/* Modified: Andreas-Joachim Peters (CERN,2010) EOS                     */
/************************************************************************/

#define _FILE_OFFSET_BITS 64

/*----------------------------------------------------------------------------*/
#include "xrdposix.hh"
#include "FuseCacheEntry.hh"
#include "../fst/layout/LayoutPlugin.hh"
#include "../fst/layout/PlainLayout.hh"
#include "../fst/layout/RaidDpLayout.hh"
#include "../fst/layout/ReedSLayout.hh"
/*----------------------------------------------------------------------------*/
#include <climits>
#include <stdint.h>
#include <iostream>
#include <libgen.h>
#include <pwd.h>
/*----------------------------------------------------------------------------*/
#include "XrdCache/XrdFileCache.hh"
#include "XrdCache/FileAbstraction.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucHash.hh"
#include "XrdOuc/XrdOucTable.hh"
#include "XrdOuc/XrdOucString.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSfs/XrdSfsInterface.hh"
/*----------------------------------------------------------------------------*/
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
/*----------------------------------------------------------------------------*/
#include "common/Logging.hh"
#include "common/Path.hh"
#include "common/RWMutex.hh"
/*----------------------------------------------------------------------------*/
#include <google/dense_hash_map>
#include <google/sparse_hash_map>
#include <google/sparsehash/densehashtable.h>
/*----------------------------------------------------------------------------*/

#ifndef __macos__
#define OSPAGESIZE 4096
#else
#define OSPAGESIZE 65536
#endif

static XrdCl::FileSystem* fs;
static XrdFileCache* XFC;

static XrdOucHash<XrdOucString>* passwdstore;
static XrdOucHash<XrdOucString>* stringstore;

XrdSysMutex passwdstoremutex;
XrdSysMutex stringstoremutex;
XrdSysMutex environmentmutex;
XrdSysMutex connectionIdMutex;
int connectionId;

int fuse_cache_read;
int fuse_cache_write;

bool fuse_exec = false; // indicates if files should be make exectuble

bool fuse_shared = false; // inidicated if this is eosd = true or eosfsd = false

bool fuse_environment_lock = false; // indicating if commands have to run in a locked environment to guarantee private authentication
XrdOucString MgmHost; // host name of our FUSE contact point

using eos::common::LayoutId;

//------------------------------------------------------------------------------
// String store
//------------------------------------------------------------------------------

char*
STRINGSTORE (const char* __charptr__)
{
  XrdOucString* yourstring;

  if (!__charptr__) return (char*) "";

  if ((yourstring = stringstore->Find(__charptr__)))
  {
    return ( (char*) yourstring->c_str());
  }
  else
  {
    XrdOucString* newstring = new XrdOucString(__charptr__);
    stringstoremutex.Lock();
    stringstore->Add(__charptr__, newstring);
    stringstoremutex.UnLock();
    return (char*) newstring->c_str();
  }
}



//------------------------------------------------------------------------------
//             ******* Implementation Translations *******
//------------------------------------------------------------------------------

// Protecting the path/inode translation table
eos::common::RWMutex mutex_inode_path;

// Mapping path name to inode
google::dense_hash_map<std::string, unsigned long long> path2inode;

// Mapping inode to path name
google::dense_hash_map<unsigned long long, std::string> inode2path;


//------------------------------------------------------------------------------
// Lock for environment variables 
//------------------------------------------------------------------------------

void
xrd_lock_environment ()
{
  if (fuse_environment_lock)
  {
    eos_static_debug("env-locked");
    unsetenv("KRB5CCNAME");
    unsetenv("X509_USER_PROXY");
    environmentmutex.Lock();
  }
}

//------------------------------------------------------------------------------
// Unlock for environment variables 
//------------------------------------------------------------------------------

void
xrd_unlock_environment ()
{
  if (fuse_environment_lock)
  {
    eos_static_debug("env-unlocked");
    unsetenv("KRB5CCNAME");
    unsetenv("X509_USER_PROXY");
    environmentmutex.UnLock();
  }
}


//------------------------------------------------------------------------------
// Lock read
//------------------------------------------------------------------------------

void
xrd_lock_r_p2i ()
{
  mutex_inode_path.LockRead();
}


//------------------------------------------------------------------------------
// Unlock read
//------------------------------------------------------------------------------

void
xrd_unlock_r_p2i ()
{
  mutex_inode_path.UnLockRead();
}


//------------------------------------------------------------------------------
// Drop the basename and return only the last level path name
//------------------------------------------------------------------------------

char*
xrd_basename (unsigned long long inode)
{
  eos::common::RWMutexReadLock vLock(mutex_inode_path);
  const char* fname = xrd_path(inode);

  if (fname)
  {
    std::string spath = fname;
    size_t len = spath.length();

    if (len)
    {
      if (spath[len - 1] == '/')
      {
        spath.erase(len - 1);
      }
    }

    size_t spos = spath.rfind("/");

    if (spos != std::string::npos)
    {
      spath.erase(0, spos + 1);
    }

    return static_cast<char*> (STRINGSTORE(spath.c_str()));
  }

  return 0;
}


//------------------------------------------------------------------------------
// Translate from inode to path
//------------------------------------------------------------------------------

const char*
xrd_path (unsigned long long inode)
{
  // Obs: use xrd_lock_r_p2i/xrd_unlock_r_p2i in the scope of the returned string
  if (inode2path.count(inode))
    return inode2path[inode].c_str();
  else
    return 0;
}


//------------------------------------------------------------------------------
// Translate from path to inode
//------------------------------------------------------------------------------

unsigned long long
xrd_inode (const char* path)
{
  eos::common::RWMutexReadLock rd_lock(mutex_inode_path);
  unsigned long long ret = 0;

  if (path2inode.count(path))
    ret = path2inode[path];

  return ret;
}


//------------------------------------------------------------------------------
// Store an inode <-> path mapping
//------------------------------------------------------------------------------

void
xrd_store_p2i (unsigned long long inode, const char* path)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_inode_path);
  path2inode[path] = inode;
  inode2path[inode] = path;
}


//------------------------------------------------------------------------------
// Store an inode <-> path mapping given the parent inode
//------------------------------------------------------------------------------

void
xrd_store_child_p2i (unsigned long long inode,
                     unsigned long long childinode,
                     const char* name)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_inode_path);
  std::string fullpath = inode2path[inode];
  std::string sname = name;

  eos_static_debug("parent_inode=%llu, child_inode=%llu, name=%s, fullpath=%s",
                   inode, childinode, name, fullpath.c_str());

  if (sname != ".")
  {
    // we don't need to store this one
    if (sname == "..")
    {
      if (inode == 1)
      {
        fullpath = "/";
      }
      else
      {
        size_t spos = fullpath.find("/");
        size_t bpos = fullpath.rfind("/");

        if ((spos != std::string::npos) && (spos != bpos))
        {
          fullpath.erase(bpos);
        }
      }
    }
    else
    {
      fullpath += "/";
      size_t spos = fullpath.find("//");

      while (spos != std::string::npos)
      {
        fullpath.replace(spos, 2, "/");
        spos = fullpath.find("//");
      }

      fullpath += name;
    }

    eos_static_debug("sname=%s fullpath=%s inode=%llu childinode=%llu ",
                     sname.c_str(), fullpath.c_str(), inode, childinode);
    path2inode[fullpath] = childinode;
    inode2path[childinode] = fullpath;
  }
}


//------------------------------------------------------------------------------
// Delete an inode <-> path mapping given the inode
//------------------------------------------------------------------------------

void
xrd_forget_p2i (unsigned long long inode)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_inode_path);

  if (inode2path.count(inode))
  {
    std::string path = inode2path[inode];
    path2inode.erase(path);
    inode2path.erase(inode);
  }
}



//------------------------------------------------------------------------------
//      ******* Implementation of the directory listing table *******
//------------------------------------------------------------------------------

// Protecting the directory listing table
eos::common::RWMutex mutex_dir2inodelist;

// Dir listing map
google::dense_hash_map<unsigned long long, std::vector<unsigned long long> > dir2inodelist;
google::dense_hash_map<unsigned long long, struct dirbuf> dir2dirbuf;


//------------------------------------------------------------------------------
// Lock read
//------------------------------------------------------------------------------

void
xrd_lock_r_dirview ()
{
  mutex_dir2inodelist.LockRead();
}

//------------------------------------------------------------------------------
// Unlock read
//------------------------------------------------------------------------------

void
xrd_unlock_r_dirview ()
{
  mutex_dir2inodelist.UnLockRead();
}


//------------------------------------------------------------------------------
// Lock write
//------------------------------------------------------------------------------

void
xrd_lock_w_dirview ()
{
  mutex_dir2inodelist.LockWrite();
}


//------------------------------------------------------------------------------
// Unlock write
//------------------------------------------------------------------------------

void
xrd_unlock_w_dirview ()
{
  mutex_dir2inodelist.UnLockWrite();
}


//------------------------------------------------------------------------------
// Create a new entry in the maps for the current inode (directory)
//------------------------------------------------------------------------------

void
xrd_dirview_create (unsigned long long inode)
{
  eos_static_debug("inode=%llu", inode);
  //............................................................................
  //Obs: path should be attached beforehand into path translation
  //............................................................................
  eos::common::RWMutexWriteLock vLock(mutex_dir2inodelist);
  dir2inodelist[inode].clear();
  dir2dirbuf[inode].p = 0;
  dir2dirbuf[inode].size = 0;
}


//------------------------------------------------------------------------------
// Delete entry from maps for current inode (directory)
//------------------------------------------------------------------------------

void
xrd_dirview_delete (unsigned long long inode)
{
  eos_static_debug("inode=%llu", inode);
  eos::common::RWMutexWriteLock wr_lock(mutex_dir2inodelist);

  if (dir2inodelist.count(inode))
  {
    if (dir2dirbuf[inode].p)
    {
      free(dir2dirbuf[inode].p);
    }

    dir2dirbuf.erase(inode);
    dir2inodelist[inode].clear();
    dir2inodelist.erase(inode);
  }
}


//------------------------------------------------------------------------------
// Get entry's inode with index 'index' from directory
//------------------------------------------------------------------------------

unsigned long long
xrd_dirview_entry (unsigned long long dirinode,
                   size_t index,
                   int get_lock)
{
  eos_static_debug("dirinode=%llu, index=%zu", dirinode, index);

  if (get_lock) eos::common::RWMutexReadLock rd_lock(mutex_dir2inodelist);

  if ((dir2inodelist.count(dirinode)) &&
      (dir2inodelist[dirinode].size() > index))
  {
    return dir2inodelist[dirinode][index];
  }

  return 0;
}


//------------------------------------------------------------------------------
// Get dirbuf corresponding to inode
//------------------------------------------------------------------------------

struct dirbuf*
xrd_dirview_getbuffer (unsigned long long inode, int get_lock)
{
  if (get_lock) eos::common::RWMutexReadLock rd_lock(mutex_dir2inodelist);

  if (dir2dirbuf.count(inode))
    return &dir2dirbuf[inode];
  else
    return 0;
}



//------------------------------------------------------------------------------
//      ******* Implementation of the FUSE directory cache *******
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Get maximum number of directories in cache
//------------------------------------------------------------------------------

static const unsigned long long
GetMaxCacheSize ()
{
  return 1024;
}

// Protecting the cache entry map
eos::common::RWMutex mutex_fuse_cache;

// Directory cache
google::dense_hash_map<unsigned long long, FuseCacheEntry*> inode2cache;


//------------------------------------------------------------------------------
// Get a cached directory
//------------------------------------------------------------------------------

int
xrd_dir_cache_get (unsigned long long inode,
                   struct timespec mtime,
                   struct dirbuf** b)
{
  int retc = 0;
  FuseCacheEntry* dir = 0;
  eos::common::RWMutexReadLock rd_lock(mutex_fuse_cache);

  if (inode2cache.count(inode) && (dir = inode2cache[inode]))
  {
    struct timespec oldtime = dir->GetModifTime();

    if ((oldtime.tv_sec == mtime.tv_sec) &&
        (oldtime.tv_nsec == mtime.tv_nsec))
    {
      //........................................................................
      // Dir in cache and valid
      //........................................................................
      *b = static_cast<struct dirbuf*> (calloc(1, sizeof ( dirbuf)));
      dir->GetDirbuf(*b);
      retc = 1; // found
    }
  }

  return retc;
}


//------------------------------------------------------------------------------
// Add or update a cache directory entry
//------------------------------------------------------------------------------

void
xrd_dir_cache_sync (unsigned long long inode,
                    int nentries,
                    struct timespec mtime,
                    struct dirbuf* b)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_fuse_cache);
  FuseCacheEntry* dir = 0;

  if ((inode2cache.count(inode)) && (dir = inode2cache[inode]))
  {
    dir->Update(nentries, mtime, b);
  }
  else
  {
    //..........................................................................
    // Add new entry
    //..........................................................................
    if (inode2cache.size() >= GetMaxCacheSize())
    {
      //........................................................................
      // Size control of the cache
      //........................................................................
      unsigned long long indx = 0;
      unsigned long long entries_del =
        static_cast<unsigned long long> (0.25 * GetMaxCacheSize());
      google::dense_hash_map<unsigned long long, FuseCacheEntry*>::iterator iter;
      iter = inode2cache.begin();

      while ((indx <= entries_del) && (iter != inode2cache.end()))
      {
        dir = (FuseCacheEntry*) iter->second;
        inode2cache.erase(iter++);
        delete dir;
        indx++;
      }
    }

    dir = new FuseCacheEntry(nentries, mtime, b);
    inode2cache[inode] = dir;
  }

  return;
}


//------------------------------------------------------------------------------
// Get a subentry from a cached directory
//------------------------------------------------------------------------------

int
xrd_dir_cache_get_entry (fuse_req_t req,
                         unsigned long long inode,
                         unsigned long long entry_inode,
                         const char* efullpath)
{
  int retc = 0;
  eos::common::RWMutexReadLock rd_lock(mutex_fuse_cache);
  FuseCacheEntry* dir;

  if ((inode2cache.count(inode)) && (dir = inode2cache[inode]))
  {
    if (dir->IsFilled())
    {
      struct fuse_entry_param e;
      if (dir->GetEntry(entry_inode, e))
      {
        xrd_store_p2i(entry_inode, efullpath);
        fuse_reply_entry(req, &e);
        retc = 1; // found
      }
    }
  }

  return retc;
}


//------------------------------------------------------------------------------
// Add new subentry to a cached directory
//------------------------------------------------------------------------------

void
xrd_dir_cache_add_entry (unsigned long long inode,
                         unsigned long long entry_inode,
                         struct fuse_entry_param* e)
{
  eos::common::RWMutexReadLock rd_lock(mutex_fuse_cache);
  FuseCacheEntry* dir = 0;

  if ((inode2cache.count(inode)) && (dir = inode2cache[inode]))
  {
    dir->AddEntry(entry_inode, e);
  }

  return;
}



//------------------------------------------------------------------------------
//      ******* Implementation of the open File Descriptor map *******
//------------------------------------------------------------------------------

// Map used for associating file descriptors with XrdCl::File objects
eos::common::RWMutex rwmutex_fd2fobj;
google::dense_hash_map<int, eos::fst::Layout*> fd2fobj;

// Pool of available file descriptors
unsigned int base_fd = 1;
std::queue<int> pool_fd;


//------------------------------------------------------------------------------
// Generate string by concatenating the inode and uid information
//------------------------------------------------------------------------------

static std::string
generate_index (unsigned long long inode, uid_t uid)
{
  char index[256];
  snprintf(index, sizeof ( index) - 1, "%llu-%u", inode, uid);
  return index;
}


//------------------------------------------------------------------------------
// Create a simulated file descriptor
//------------------------------------------------------------------------------

int
xrd_generate_fd ()
{
  int retc = -1;

  if (!pool_fd.empty())
  {
    retc = pool_fd.front();
    pool_fd.pop();
  }
  else if (base_fd < UINT_MAX)
  {
    base_fd++;
    retc = base_fd;
  }
  else
  {
    eos_static_err("error=no more file descirptors available.");
    retc = -1;
  }

  return retc;
}


//------------------------------------------------------------------------------
// Return the fd value to the pool
//------------------------------------------------------------------------------

void
xrd_release_fd (int fd)
{
  eos_static_debug("Calling function.");
  pool_fd.push(fd);
}


//------------------------------------------------------------------------------
// Add new mapping between fd and XrdCl::File object
//------------------------------------------------------------------------------

int
xrd_add_fd2file (eos::fst::Layout* obj)
{
  eos_static_debug("Calling function.");
  int fd = -1;
  eos::common::RWMutexWriteLock wr_lock(rwmutex_fd2fobj);
  fd = xrd_generate_fd();

  if (fd > 0)
  {
    fd2fobj[fd] = obj;
  }
  else
  {
    eos_static_err("error=error while getting file descriptor");
  }

  return fd;
}


//------------------------------------------------------------------------------
// Get the XrdCl::File object corresponding to the fd
//------------------------------------------------------------------------------

eos::fst::Layout*
xrd_get_file (int fd)
{
  eos::common::RWMutexReadLock rd_lock(rwmutex_fd2fobj);

  if (fd2fobj.count(fd))
  {
    return fd2fobj[fd];
  }
  else
  {
    return 0;
  }
}


//------------------------------------------------------------------------------
// Remove entry from mapping
//------------------------------------------------------------------------------

void
xrd_remove_fd2file (int fd)
{
  eos_static_debug("Calling function.");
  eos::common::RWMutexWriteLock wr_lock(rwmutex_fd2fobj);

  if (fd2fobj.count(fd))
  {
    google::dense_hash_map<int, eos::fst::Layout*>::iterator iter = fd2fobj.find(fd);
    eos::fst::Layout* fobj = static_cast<eos::fst::Layout*> (iter->second);
    delete fobj;
    fobj = 0;
    fd2fobj.erase(iter);
    //..........................................................................
    // Return fd to the pool
    //..........................................................................
    xrd_release_fd(fd);
  }
}


// Map <inode, user> to a file descriptor
google::dense_hash_map<std::string, unsigned long long> inodeuser2fd;

// Mutex to protecte the map
XrdSysMutex mutex_inodeuser2fd;


//------------------------------------------------------------------------------
// Add fd as an open file descriptor to speed-up mknod
//------------------------------------------------------------------------------

void
xrd_add_open_fd (int fd, unsigned long long inode, uid_t uid)
{
  eos_static_debug("Calling function inode = %llu, uid = %lu.",
                   inode, (unsigned long) uid);

  XrdSysMutexHelper lock(mutex_inodeuser2fd);
  inodeuser2fd[generate_index(inode, uid)] = fd;
}


//------------------------------------------------------------------------------
// Return file descriptor held by a user for a file
//------------------------------------------------------------------------------

unsigned long long
xrd_get_open_fd (unsigned long long inode, uid_t uid)
{
  eos_static_debug("Calling function inode = %llu, uid = %lu.",
                   inode, (unsigned long) uid);

  XrdSysMutexHelper lock(mutex_inodeuser2fd);
  std::string index = generate_index(inode, uid);

  if (inodeuser2fd.count(index))
  {
    return inodeuser2fd[index];
  }

  return 0;
}


//------------------------------------------------------------------------------
// Relelase file descriptor
//------------------------------------------------------------------------------

void
xrd_release_open_fd (unsigned long long inode, uid_t uid)
{
  eos_static_debug("Calling function inode = %llu, uid = %lu.",
                   inode, (unsigned long) uid);

  XrdSysMutexHelper lock(mutex_inodeuser2fd);
  std::string index = generate_index(inode, uid);

  if (inodeuser2fd.count(index))
  {
    inodeuser2fd.erase(index);
  }
}



//------------------------------------------------------------------------------
//        ******* Implementation IO Buffer Management *******
//------------------------------------------------------------------------------

// Forward declaration
class IoBuf;

// Protecting the IO buffer map
XrdSysMutex IoBufferLock;

// IO buffer table
std::map<int, IoBuf> IoBufferMap;


//------------------------------------------------------------------------------
//! Class IoBuf 
//------------------------------------------------------------------------------

class IoBuf
{
private:
  void* buffer;
  size_t size;

public:

  //..........................................................................
  //! Constructor
  //..........................................................................

  IoBuf ()
  {
    buffer = 0;
    size = 0;
  }

  //..........................................................................
  //! Destructor 
  //..........................................................................

  virtual
  ~IoBuf ()
  {
    if (buffer && size) free(buffer);
  }

  //..........................................................................
  //! Get buffer
  //..........................................................................

  char*
  GetBuffer ()
  {
    return (char*) buffer;
  }

  //..........................................................................
  //! Get size of buffer
  //..........................................................................

  size_t
  GetSize ()
  {
    return size;
  }

  //..........................................................................
  //! Resize buffer
  //..........................................................................

  void
  Resize (size_t newsize)
  {
    if (newsize > size)
    {
      size = (newsize < (128 * 1024)) ? 128 * 1024 : newsize;
      buffer = realloc(buffer, size);
    }
  }
};


//------------------------------------------------------------------------------
// Guarantee a buffer for reading of at least 'size' for the specified fd
//------------------------------------------------------------------------------

char*
xrd_attach_read_buffer (int fd, size_t size)
{
  XrdSysMutexHelper lock(IoBufferLock);
  IoBufferMap[fd].Resize(size);
  return (char*) IoBufferMap[fd].GetBuffer();
}


//------------------------------------------------------------------------------
// Release read buffer corresponding to the file
//------------------------------------------------------------------------------

void
xrd_release_read_buffer (int fd)
{
  XrdSysMutexHelper lock(IoBufferLock);
  IoBufferMap.erase(fd);
  return;
}


//------------------------------------------------------------------------------
//             ******* XROOTD interface functions *******
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Remove extended attribute
//------------------------------------------------------------------------------

int
xrd_rmxattr (const char* path,
             const char* xattr_name,
             uid_t uid,
             pid_t pid)
{
  eos_static_info("path=%s xattr_name=%s uid=%u pid=%u", path, xattr_name, uid, pid);
  eos::common::Timing rmxattrtiming("rmxattr");
  COMMONTIMING("START", &rmxattrtiming);
  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=rm&";
  request += "mgm.xattrname=";
  request += xattr_name;
  arg.FromString(request);

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);

  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  xrd_unlock_environment();
  COMMONTIMING("GETPLUGIN", &rmxattrtiming);

  errno = 0;

  if (status.IsOK())
  {
    int items = 0;
    char tag[1024];
    //..........................................................................
    // Parse output
    //..........................................................................
    items = sscanf(response->GetBuffer(), "%sp retc=%i", tag, &retc);

    if ((items != 2) || (strcmp(tag, "rmxattr:")))
    {
      errno = ENOENT;
      retc = -1;
    }
    else
    {
      errno = retc;
      if (errno) retc = -1;
    }
  }
  else
  {
    errno = EFAULT;
    retc = errno;
  }

  COMMONTIMING("END", &rmxattrtiming);

  if (EOS_LOGS_DEBUG)
  {
    rmxattrtiming.Print();
  }

  delete response;
  return retc;
}


//------------------------------------------------------------------------------
// Set extended attribute
//------------------------------------------------------------------------------

int
xrd_setxattr (const char* path,
              const char* xattr_name,
              const char* xattr_value,
              size_t size,
              uid_t uid,
              pid_t pid)
{
  eos_static_info("path=%s xattr_name=%s xattr_value=%s uid=%u pid=%u", path, xattr_name, xattr_value, uid, pid);
  eos::common::Timing setxattrtiming("setxattr");
  COMMONTIMING("START", &setxattrtiming);
  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=set&";
  request += "mgm.xattrname=";
  request += xattr_name;
  request += "&";
  request += "mgm.xattrvalue=";
  request += xattr_value;
  arg.FromString(request);

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  xrd_unlock_environment();
  COMMONTIMING("GETPLUGIN", &setxattrtiming);

  errno = 0;

  if (status.IsOK())
  {
    int items = 0;
    char tag[1024];
    //..........................................................................
    // Parse output
    //..........................................................................
    items = sscanf(response->GetBuffer(), "%s retc=%i", tag, &retc);

    if ((items != 2) || (strcmp(tag, "setxattr:")))
    {
      errno = ENOENT;
    }
    else
    {
      errno = retc;
    }
  }
  else
  {
    errno = EFAULT;
  }
  COMMONTIMING("END", &setxattrtiming);

  if (EOS_LOGS_DEBUG)
  {
    setxattrtiming.Print();
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Read an extended attribute
//------------------------------------------------------------------------------

int
xrd_getxattr (const char* path,
              const char* xattr_name,
              char** xattr_value,
              size_t* size,
              uid_t uid,
              pid_t pid)
{
  eos_static_info("path=%s xattr_name=%s uid=%u pid=%u", path, xattr_name, uid, pid);
  eos::common::Timing getxattrtiming("getxattr");

  COMMONTIMING("START", &getxattrtiming);
  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=get&";
  request += "mgm.xattrname=";
  request += xattr_name;
  arg.FromString(request);

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  xrd_unlock_environment();
  COMMONTIMING("GETPLUGIN", &getxattrtiming);

  errno = 0;

  if (status.IsOK())
  {
    int items = 0;
    char tag[1024];
    char rval[4096];
    //..........................................................................
    // Parse output
    //..........................................................................
    items = sscanf(response->GetBuffer(), "%s retc=%i value=%s", tag, &retc, rval);

    if ((items != 3) || (strcmp(tag, "getxattr:")))
    {
      errno = EFAULT;
    }
    else
    {
      if (strcmp(xattr_name, "user.eos.XS") == 0)
      {
        char* ptr = rval;

        for (unsigned int i = 0; i < strlen(rval); i++, ptr++)
        {
          if (*ptr == '_')
            *ptr = ' ';
        }
      }

      *size = strlen(rval);
      *xattr_value = (char*) calloc((*size) + 1, sizeof ( char));
      *xattr_value = strncpy(*xattr_value, rval, *size);
      errno = retc;
    }
  }
  else
  {
    errno = EFAULT;
  }

  COMMONTIMING("END", &getxattrtiming);

  if (EOS_LOGS_DEBUG)
  {
    getxattrtiming.Print();
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// List extended attributes
//------------------------------------------------------------------------------

int
xrd_listxattr (const char* path,
               char** xattr_list,
               size_t* size,
               uid_t uid,
               pid_t pid)
{
  eos_static_info("path=%s uid=%u pid=%u", path, uid, pid);
  eos::common::Timing listxattrtiming("listxattr");

  COMMONTIMING("START", &listxattrtiming);

  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=ls";
  arg.FromString(request);

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  xrd_unlock_environment();
  COMMONTIMING("GETPLUGIN", &listxattrtiming);

  errno = 0;
  if (status.IsOK())
  {
    int items = 0;
    char tag[1024];
    char rval[16384];
    //..........................................................................
    // Parse output
    //..........................................................................
    items = sscanf(response->GetBuffer(), "%s retc=%i %s", tag, &retc, rval);

    if ((items != 3) || (strcmp(tag, "lsxattr:")))
    {
      errno = ENOENT;
    }
    else
    {
      *size = strlen(rval);
      char* ptr = rval;

      for (unsigned int i = 0; i < (*size); i++, ptr++)
      {
        if (*ptr == '&')
          *ptr = '\0';
      }

      *xattr_list = (char*) calloc((*size) + 1, sizeof ( char));
      *xattr_list = (char*) memcpy(*xattr_list, rval, *size);
      errno = retc;
    }
  }
  else
  {
    errno = EFAULT;
  }

  COMMONTIMING("END", &listxattrtiming);

  if (EOS_LOGS_DEBUG)
  {
    listxattrtiming.Print();
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Return file attributes. If a field is meaningless or semi-meaningless
// (e.g., st_ino) then it should be set to 0 or given a "reasonable" value.
//------------------------------------------------------------------------------

int
xrd_stat (const char* path, struct stat* buf)
{
  eos_static_info("path=%s", path);
  eos::common::Timing stattiming("xrd_stat");

  COMMONTIMING("START", &stattiming);

  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=stat";
  arg.FromString(request);
  XrdCl::XRootDStatus status = fs->Query(XrdCl::QueryCode::OpaqueFile,
                                         arg, response);
  COMMONTIMING("GETPLUGIN", &stattiming);

  errno = 0;

  if (status.IsOK())
  {
    unsigned long long sval[10];
    unsigned long long ival[6];
    char tag[1024];
    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(response->GetBuffer(),
                       "%s %llu %llu %llu %llu %llu %llu %llu %llu "
                       "%llu %llu %llu %llu %llu %llu %llu %llu",
                       tag, (unsigned long long*) &sval[0],
                       (unsigned long long*) &sval[1],
                       (unsigned long long*) &sval[2],
                       (unsigned long long*) &sval[3],
                       (unsigned long long*) &sval[4],
                       (unsigned long long*) &sval[5],
                       (unsigned long long*) &sval[6],
                       (unsigned long long*) &sval[7],
                       (unsigned long long*) &sval[8],
                       (unsigned long long*) &sval[9],
                       (unsigned long long*) &ival[0],
                       (unsigned long long*) &ival[1],
                       (unsigned long long*) &ival[2],
                       (unsigned long long*) &ival[3],
                       (unsigned long long*) &ival[4],
                       (unsigned long long*) &ival[5]);

    if ((items != 17) || (strcmp(tag, "stat:")))
    {
      errno = ENOENT;
      delete response;
      return errno;
    }
    else
    {
      buf->st_dev = (dev_t) sval[0];
      buf->st_ino = (ino_t) sval[1];
      buf->st_mode = (mode_t) sval[2];
      buf->st_nlink = (nlink_t) sval[3];
      buf->st_uid = (uid_t) sval[4];
      buf->st_gid = (gid_t) sval[5];
      buf->st_rdev = (dev_t) sval[6];
      buf->st_size = (off_t) sval[7];
      buf->st_blksize = (blksize_t) sval[8];
      buf->st_blocks = (blkcnt_t) sval[9];
#ifdef __APPLE__
      buf->st_atimespec.tv_sec = (time_t) ival[0];
      buf->st_mtimespec.tv_sec = (time_t) ival[1];
      buf->st_ctimespec.tv_sec = (time_t) ival[2];
      buf->st_atimespec.tv_nsec = (time_t) ival[3];
      buf->st_mtimespec.tv_nsec = (time_t) ival[4];
      buf->st_ctimespec.tv_nsec = (time_t) ival[5];
#else
      buf->st_atime = (time_t) ival[0];
      buf->st_mtime = (time_t) ival[1];
      buf->st_ctime = (time_t) ival[2];
      buf->st_atim.tv_sec = (time_t) ival[0];
      buf->st_mtim.tv_sec = (time_t) ival[1];
      buf->st_ctim.tv_sec = (time_t) ival[2];
      buf->st_atim.tv_nsec = (time_t) ival[3];
      buf->st_mtim.tv_nsec = (time_t) ival[4];
      buf->st_ctim.tv_nsec = (time_t) ival[5];
#endif

      if (S_ISREG(buf->st_mode) && fuse_exec)
      {
        buf->st_mode |= (S_IXUSR | S_IXGRP | S_IXOTH);
      }

      buf->st_mode &= (~S_ISVTX); // clear the vxt bit
      buf->st_mode &= (~S_ISUID); // clear suid
      buf->st_mode &= (~S_ISGID); // clear sgid
      retc = 0;
    }
  }
  else
  {
    eos_static_err("error=status is NOT ok.");
    errno = EFAULT;
  }

  COMMONTIMING("END", &stattiming);

  if (EOS_LOGS_DEBUG)
  {
    stattiming.Print();
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Return statistics about the filesystem
//------------------------------------------------------------------------------

int
xrd_statfs (const char* path, struct statvfs* stbuf)
{
  eos_static_info("path=%s", path);
  static unsigned long long a1 = 0;
  static unsigned long long a2 = 0;
  static unsigned long long a3 = 0;
  static unsigned long long a4 = 0;
  static XrdSysMutex statmutex;
  static time_t laststat = 0;
  statmutex.Lock();

  errno = 0;
  if ((time(NULL) - laststat) < ((15 + (int) 5.0 * rand() / RAND_MAX)))
  {
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = a3 / 4096;
    stbuf->f_bfree = a1 / 4096;
    stbuf->f_bavail = a1 / 4096;
    stbuf->f_files = a4;
    stbuf->f_ffree = a2;
    stbuf->f_fsid = 0xcafe;
    stbuf->f_namemax = 256;
    statmutex.UnLock();
    return 0;
  }

  eos::common::Timing statfstiming("xrd_statfs");
  COMMONTIMING("START", &statfstiming);

  int retc;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=statvfs&";
  request += "path=";
  request += path;
  arg.FromString(request);
  XrdCl::XRootDStatus status = fs->Query(XrdCl::QueryCode::OpaqueFile,
                                         arg, response);
  COMMONTIMING("END", &statfstiming);

  errno = 0;

  if (EOS_LOGS_DEBUG)
  {
    statfstiming.Print();
  }

  if (status.IsOK())
  {
    char tag[1024];

    if (!response->GetBuffer())
    {
      statmutex.UnLock();
      delete response;\
      errno = EFAULT;
      return errno;
    }

    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(response->GetBuffer(),
                       "%s retc=%d f_avail_bytes=%llu f_avail_files=%llu "
                       "f_max_bytes=%llu f_max_files=%llu",
                       tag, &retc, &a1, &a2, &a3, &a4);

    if ((items != 6) || (strcmp(tag, "statvfs:")))
    {
      statmutex.UnLock();
      delete response;
      errno = EFAULT;
      return errno;
    }

    retc = -retc;
    laststat = time(NULL);
    statmutex.UnLock();
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = a3 / 4096;
    stbuf->f_bfree = a1 / 4096;
    stbuf->f_bavail = a1 / 4096;
    stbuf->f_files = a4;
    stbuf->f_ffree = a2;
  }
  else
  {
    statmutex.UnLock();
    errno = EFAULT;
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Change permissions for the file
//------------------------------------------------------------------------------

int
xrd_chmod (const char* path,
           mode_t mode,
           uid_t uid,
           pid_t pid)
{
  eos_static_info("path=%s mode=%x uid=%u pid=%u", path, mode, uid, pid);
  eos::common::Timing chmodtiming("xrd_chmod");
  COMMONTIMING("START", &chmodtiming);
  int retc = 0;
  XrdOucString smode;
  smode += (int) mode;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=chmod&mode=";
  request += smode.c_str();
  arg.FromString(request);

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  xrd_unlock_environment();
  COMMONTIMING("END", &chmodtiming);

  errno = 0;

  if (EOS_LOGS_DEBUG)
  {
    chmodtiming.Print();
  }

  if (status.IsOK())
  {
    char tag[1024];

    if (!response->GetBuffer())
    {
      delete response;
      errno = EFAULT;
      return errno;
    }

    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(response->GetBuffer(), "%s retc=%d", tag, &retc);

    if ((items != 2) || (strcmp(tag, "chmod:")))
    {
      errno = EFAULT;
    }
    else
    {
      errno = retc;
    }
  }
  else
  {
    errno = EFAULT;
  }

  delete response;
  return errno;
}

//------------------------------------------------------------------------------
// Update the last access time and last modification time
//------------------------------------------------------------------------------

int
xrd_utimes (const char* path,
            struct timespec* tvp,
            uid_t uid,
            pid_t pid)
{
  eos_static_info("path=%s uid=%u pid=%u", path, uid, pid);
  eos::common::Timing utimestiming("xrd_utimes");

  COMMONTIMING("START", &utimestiming);
  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=utimes&tv1_sec=";
  char lltime[1024];
  sprintf(lltime, "%llu", (unsigned long long) tvp[0].tv_sec);
  request += lltime;
  request += "&tv1_nsec=";
  sprintf(lltime, "%llu", (unsigned long long) tvp[0].tv_nsec);
  request += lltime;
  request += "&tv2_sec=";
  sprintf(lltime, "%llu", (unsigned long long) tvp[1].tv_sec);
  request += lltime;
  request += "&tv2_nsec=";
  sprintf(lltime, "%llu", (unsigned long long) tvp[1].tv_nsec);
  request += lltime;
  arg.FromString(request);

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  xrd_unlock_environment();
  COMMONTIMING("END", &utimestiming);

  errno = 0;

  if (EOS_LOGS_DEBUG)
  {
    utimestiming.Print();
  }

  if (status.IsOK())
  {
    char tag[1024];
    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(response->GetBuffer(), "%s retc=%d", tag, &retc);

    if ((items != 2) || (strcmp(tag, "utimes:")))
    {
      errno = EFAULT;
    }
    else
    {
      errno = retc;
    }
  }
  else
  {
    errno = EFAULT;
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// It returns -ENOENT if the path doesn't exist, -EACCESS if the requested
// permission isn't available, or 0 for success. Note that it can be called
// on files, directories, or any other object that appears in the filesystem.
//------------------------------------------------------------------------------

int
xrd_access (const char* path,
            int mode,
            uid_t uid,
            pid_t pid
            )
{
  eos_static_info("path=%s mode=%d uid=%u pid=%u", path, mode, uid, pid);
  eos::common::Timing accesstiming("xrd_access");
  COMMONTIMING("START", &accesstiming);

  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  char smode[16];
  snprintf(smode, sizeof (smode) - 1, "%d", mode);

  request = path;
  request += "?";
  request += "mgm.pcmd=access&mode=";
  request += smode;

  arg.FromString(request);

  xrd_lock_environment();

  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);


  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);

  xrd_unlock_environment();

  COMMONTIMING("STOP", &accesstiming);

  errno = 0;

  if (EOS_LOGS_DEBUG)
  {
    accesstiming.Print();
  }

  if (status.IsOK())
  {
    char tag[1024];
    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(response->GetBuffer(), "%s retc=%d", tag, &retc);

    fprintf(stderr, "access-retc=%d", retc);
    if ((items != 2) || (strcmp(tag, "access:")))
    {
      errno = EFAULT;
    }
    else
    {
      errno = retc;
    }
  }
  else
  {
    errno = EFAULT;
  }

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Get list of entries in directory
//------------------------------------------------------------------------------

int
xrd_inodirlist (unsigned long long dirinode, const char* path, uid_t uid, pid_t pid)
{
  eos_static_info("inode=%llu path=%s", dirinode, path);
  eos::common::Timing inodirtiming("xrd_inodirlist");
  COMMONTIMING("START", &inodirtiming);

  int retc = 0;
  char* ptr = 0;
  char* value = 0;
  int doinodirlist = -1;
  std::string request = path;

  COMMONTIMING("GETSTSTREAM", &inodirtiming);


  xrd_lock_environment();
  request.insert(0, xrd_user_url(uid, pid));
  XrdCl::File* file = new XrdCl::File();
  XrdCl::XRootDStatus status = file->Open(request.c_str(),
                                          XrdCl::OpenFlags::Flags::Read);
  xrd_unlock_environment();

  errno = 0;

  if (!status.IsOK())
  {
    eos_static_err("error=got an error to request.");
    delete file;
    errno = ENOENT;
    return errno;
  }

  //............................................................................
  // Start to read
  //............................................................................
  int npages = 1;
  off_t offset = 0;
  unsigned int nbytes = 0;
  value = (char*) malloc(PAGESIZE + 1);

  COMMONTIMING("READSTSTREAM", &inodirtiming);

  status = file->Read(offset, PAGESIZE, value + offset, nbytes);

  while ((status.IsOK()) && (nbytes == PAGESIZE))
  {
    npages++;
    value = (char*) realloc(value, npages * PAGESIZE + 1);
    offset += PAGESIZE;
    status = file->Read(offset, PAGESIZE, value + offset, nbytes);
  }

  if (status.IsOK()) offset += nbytes;

  value[offset] = 0;
  delete file;
  xrd_dirview_create((unsigned long long) dirinode);

  COMMONTIMING("PARSESTSTREAM", &inodirtiming);

  xrd_lock_w_dirview(); // =>

  if (status.IsOK())
  {
    char dirpath[4096];
    unsigned long long inode;
    char tag[128];
    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(value, "%s retc=%d", tag, &retc);

    if (retc)
    {
      free(value);
      xrd_unlock_w_dirview(); // <=
      xrd_dirview_delete((unsigned long long) dirinode);
      errno = EFAULT;
      return errno;
    }

    if ((items != 2) || (strcmp(tag, "inodirlist:")))
    {
      eos_static_err("error=got an error(1).");
      free(value);
      xrd_unlock_w_dirview(); // <=
      xrd_dirview_delete((unsigned long long) dirinode);
      errno = EFAULT;
      return errno;
    }

    ptr = strchr(value, ' ');
    if (ptr) ptr = strchr(ptr + 1, ' ');

    char* endptr = value + strlen(value) - 1;

    while ((ptr) && (ptr < endptr))
    {
      int items = sscanf(ptr, "%s %llu", dirpath, &inode);

      if (items != 2)
      {
        eos_static_err("error=got an error(2).");
        free(value);
        xrd_unlock_w_dirview(); // <=
        xrd_dirview_delete((unsigned long long) dirinode);
        errno = EFAULT;
        return errno;
      }

      XrdOucString whitespacedirpath = dirpath;
      whitespacedirpath.replace("%20", " ");

      xrd_store_child_p2i(dirinode, inode, whitespacedirpath.c_str());
      dir2inodelist[dirinode].push_back(inode);

      // to the next entries
      if (ptr) ptr = strchr(ptr + 1, ' ');
      if (ptr) ptr = strchr(ptr + 1, ' ');
    }

    doinodirlist = 0;
  }

  xrd_unlock_w_dirview(); // <=
  COMMONTIMING("END", &inodirtiming);

  if (EOS_LOGS_DEBUG)
  {
    //inodirtiming.Print();
  }

  free(value);
  return doinodirlist;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

struct dirent*
xrd_readdir (const char* path_dir, size_t *size, uid_t uid, pid_t pid)
{
  eos_static_info("path=%s", path_dir);

  struct dirent* dirs = NULL;
  XrdCl::DirectoryList* response = 0;
  XrdCl::DirListFlags::Flags flags = XrdCl::DirListFlags::None;
  string path_str = path_dir;

  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.DirList(path_str, flags, response);
  xrd_unlock_environment();

  if (status.IsOK())
  {
    *size = response->GetSize();

    dirs = static_cast<struct dirent*> (calloc(*size, sizeof ( struct dirent)));
    int i = 0;
    for (XrdCl::DirectoryList::ConstIterator iter = response->Begin();
      iter != response->End();
      ++iter)
    {
      XrdCl::DirectoryList::ListEntry* list_entry =
        static_cast<XrdCl::DirectoryList::ListEntry*> (*iter);
      size_t len = list_entry->GetName().length();
      const char* cp = list_entry->GetName().c_str();
      const int dirhdrln = dirs[i].d_name - (char *) &dirs[i];
#if defined(__macos__) || defined(__FreeBSD__)
      dirs[i].d_fileno = i;
      dirs[i].d_type = DT_UNKNOWN;
      dirs[i].d_namlen = len;
#else
      dirs[i].d_ino = i;
      dirs[i].d_off = i*NAME_MAX;
#endif
      dirs[i].d_reclen = len + dirhdrln;
      dirs[i].d_type = DT_UNKNOWN;
      strncpy(dirs[i].d_name, cp, len);
      dirs[i].d_name[len] = '\0';
      i++;
    }

    return dirs;
  }

  *size = 0;
  return NULL;
}


//------------------------------------------------------------------------------
// Create a directory with the given name
//------------------------------------------------------------------------------

int
xrd_mkdir (const char* path,
           mode_t mode,
           uid_t uid,
           pid_t pid)
{
  eos_static_info("path=%s mode=%d uid=%u pid=%u", path, mode, uid, pid);
  XrdCl::Access::Mode mode_XrdCl = eos::common::LayoutId::MapModeSfs2XrdCl(mode);
  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.MkDir(path,
                                        XrdCl::MkDirFlags::MakePath,
                                        mode_XrdCl);
  xrd_unlock_environment();
  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}


//------------------------------------------------------------------------------
// Remove the given directory
//------------------------------------------------------------------------------

int
xrd_rmdir (const char* path, uid_t uid, pid_t pid)
{
  eos_static_info("path=%s uid=%u pid=%u", path, uid, pid);
  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.RmDir(path);
  xrd_unlock_environment();

  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}

//------------------------------------------------------------------------------
// Map open return codes to errno's
//------------------------------------------------------------------------------

int
xrd_error_retc_map (int retc)
{
  if (retc) errno = retc;
  if (retc == kXR_ArgInvalid)
  {
    errno = EINVAL;
  }

  if (retc == kXR_ArgMissing)
  {
    errno = EINVAL;
  }

  if (retc == kXR_ArgTooLong)
  {
    errno = E2BIG;
  }

  if (retc == kXR_FileNotOpen)
  {
    errno = EBADF;
  }

  if (retc == kXR_FSError)
  {
    errno = EIO;
  }

  if (retc == kXR_InvalidRequest)
  {
    errno = EINVAL;
  }

  if (retc == kXR_IOError)
  {
    errno = EIO;
  }

  if (retc == kXR_NoMemory)
  {
    errno = ENOMEM;
  }

  if (retc == kXR_NoSpace)
  {
    errno = ENOSPC;
  }

  if (retc == kXR_ServerError)
  {
    errno = EIO;
  }

  if (retc == kXR_NotAuthorized)
  {
    errno = EPERM;
  }

  if (retc == kXR_NotFound)
  {
    errno = ENOENT;
  }

  if (retc == kXR_Unsupported)
  {
    errno = ENOTSUP;
  }

  if (retc == kXR_NotFile)
  {
    errno = EISDIR;
  }

  if (retc == kXR_isDirectory)
  {
    errno = EISDIR;
  }

  if (retc == kXR_Cancelled)
  {
    errno = ECANCELED;
  }

  if (retc == kXR_ChkLenErr)
  {
    errno = ERANGE;
  }

  if (retc == kXR_ChkSumErr)
  {
    errno = ERANGE;
  }

  if (retc == kXR_inProgress)
  {
    errno = EAGAIN;
  }

  if (retc)
  {
    return -1;
  }

  return 0;
}

//------------------------------------------------------------------------------
// Open a file
//------------------------------------------------------------------------------

int
xrd_open (const char* path, int oflags, mode_t mode, uid_t uid, pid_t pid)
{
  eos_static_info("path=%s flags=%d mode=%d uid=%u pid=%u", path, oflags, mode, uid, pid);
  int t0;
  int retc = -1;
  XrdOucString spath = xrd_user_url(uid, pid);
  spath += path;
  mode_t mode_sfs = 0;
  errno = 0;

  XrdSfsFileOpenMode flags_sfs = SFS_O_RDONLY; // open for read by default

  if (oflags & (O_CREAT | O_EXCL | O_RDWR | O_WRONLY))
  {
    flags_sfs = SFS_O_CREAT | SFS_O_RDWR;
  }

  if (mode & S_IRUSR) mode_sfs |= XrdCl::Access::UR;

  if (mode & S_IWUSR) mode_sfs |= XrdCl::Access::UW;

  if (mode & S_IXUSR) mode_sfs |= XrdCl::Access::UX;

  if (mode & S_IRGRP) mode_sfs |= XrdCl::Access::GR;

  if (mode & S_IWGRP) mode_sfs |= XrdCl::Access::GW;

  if (mode & S_IXGRP) mode_sfs |= XrdCl::Access::GX;

  if (mode & S_IROTH) mode_sfs |= XrdCl::Access::OR;

  if (mode & S_IWOTH) mode_sfs |= XrdCl::Access::OW;

  if (mode & S_IXOTH) mode_sfs |= XrdCl::Access::OX;

  if ((t0 = spath.find("/proc/")) != STR_NPOS)
  {
    //..........................................................................
    // Clean the path
    //..........................................................................
    int t1 = spath.find("//");
    int t2 = spath.find("//", t1 + 2);
    spath.erase(t2 + 2, t0 - t2 - 2);

    while (spath.replace("///", "//"))
    {
    };

    //..........................................................................
    // Force a reauthentication to the head node
    //..........................................................................
    if (spath.endswith("/proc/reconnect"))
    {
      XrdSysMutexHelper cLock(connectionIdMutex);
      connectionId++;
      errno = ECONNABORTED;
      return -1;
    }

    //..........................................................................
    // Return the 'whoami' information in that file
    //..........................................................................
    if (spath.endswith("/proc/whoami"))
    {
      spath.replace("/proc/whoami", "/proc/user/");
      spath += "?mgm.cmd=whoami&mgm.format=fuse&eos.app=fuse";
      eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                         eos::common::LayoutId::kXrdCl);
      retc = file->Open(spath.c_str(), flags_sfs, mode_sfs, "");
      eos_static_info("open returned retc=%d", retc);
      if (retc)
      {
        eos_static_err("error=open failed for %s", spath.c_str());
        return xrd_error_retc_map(errno);
      }
      else
      {
        retc = xrd_add_fd2file(file);
        return retc;
      }
    }

    if (spath.endswith("/proc/who"))
    {
      spath.replace("/proc/who", "/proc/user/");
      spath += "?mgm.cmd=who&mgm.format=fuse&eos.app=fuse";
      eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                         eos::common::LayoutId::kXrdCl);
      retc = file->Open(spath.c_str(), flags_sfs, mode_sfs, "");

      if (retc)
      {
        eos_static_err("error=open failed for %s", spath.c_str());
        return xrd_error_retc_map(errno);
      }
      else
      {
        retc = xrd_add_fd2file(file);
        return retc;
      }

    }

    if (spath.endswith("/proc/quota"))
    {
      spath.replace("/proc/quota", "/proc/user/");
      spath += "?mgm.cmd=quota&mgm.subcmd=lsuser&mgm.format=fuse&eos.app=fuse";
      eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                         eos::common::LayoutId::kXrdCl);
      retc = file->Open(spath.c_str(), flags_sfs, mode_sfs, "");

      if (retc)
      {
        eos_static_err("error=open failed for %s", spath.c_str());
        return xrd_error_retc_map(errno);
      }
      else
      {
        retc = xrd_add_fd2file(file);
        return retc;
      }
    }
  }

  //............................................................................
  // Try to open file using pio ( parallel io ) only in read mode
  //............................................................................
  if ((!getenv("EOS_FUSE_NOPIO")) && (flags_sfs == SFS_O_RDONLY))
  {
    XrdCl::Buffer arg;
    XrdCl::Buffer* response = 0;
    XrdCl::XRootDStatus status;
    std::string file_path = path;
    size_t spos = file_path.rfind("//");

    if (spos != std::string::npos)
    {
      file_path.erase(0, spos + 1);
    }

    std::string request = file_path;
    request += "?mgm.pcmd=open";
    arg.FromString(request);
    status = fs->Query(XrdCl::QueryCode::OpaqueFile, arg, response);

    if (status.IsOK())
    {
      //........................................................................
      // Parse output
      //........................................................................
      XrdOucString tag;
      XrdOucString stripePath;
      std::vector<std::string> stripeUrls;

      XrdOucString origResponse = response->GetBuffer();
      XrdOucString stringOpaque = response->GetBuffer();

      while (stringOpaque.replace("?", "&"))
      {
      }

      while (stringOpaque.replace("&&", "&"))
      {
      }

      XrdOucEnv* openOpaque = new XrdOucEnv(stringOpaque.c_str());
      char* opaqueInfo = (char*) strstr(origResponse.c_str(), "&&mgm.logid");

      if (opaqueInfo)
      {
        opaqueInfo += 2;
        LayoutId::layoutid_t layout = openOpaque->GetInt("mgm.lid");

        for (unsigned int i = 0; i <= eos::common::LayoutId::GetStripeNumber(layout); i++)
        {
          tag = "pio.";
          tag += static_cast<int> (i);
          stripePath = "root://";
          stripePath += openOpaque->Get(tag.c_str());
          stripePath += "/";
          stripePath += file_path.c_str();
          stripeUrls.push_back(stripePath.c_str());
        }

        eos::fst::RaidMetaLayout* file;

        if (LayoutId::GetLayoutType(layout) == LayoutId::kRaidDP)
        {
          file = new eos::fst::RaidDpLayout(NULL, layout, NULL, NULL,
                                            eos::common::LayoutId::kXrdCl);
        }
        else if ((LayoutId::GetLayoutType(layout) == LayoutId::kRaid6) || (LayoutId::GetLayoutType(layout) == LayoutId::kArchive))
        {
          file = new eos::fst::ReedSLayout(NULL, layout, NULL, NULL,
                                           eos::common::LayoutId::kXrdCl);
        }
        else
        {
          eos_static_warning("warning=no such supported layout for PIO");
          file = 0;
        }

        if (file)
        {
          retc = file->OpenPio(stripeUrls,
                               flags_sfs,
                               mode_sfs,
                               opaqueInfo);
          if (retc)
          {
            eos_static_err("error=failed open for pio red, path=%s", spath.c_str());
            delete file;
            return xrd_error_retc_map(errno);
          }
          else
          {
            retc = xrd_add_fd2file(file);
            return retc;
          }
        }
      }
      else
      {
        eos_static_debug("error=opque info not what we expected");
      }
    }
    else
    {
      eos_static_err("error=failed get request for pio read");
    }
  }

  spath += "?eos.app=fuse&";
  eos_static_debug("the spath is:%s", spath.c_str());

  eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                     eos::common::LayoutId::kXrdCl);
  retc = file->Open(spath.c_str(), flags_sfs, mode_sfs, "");

  if (retc)
  {
    eos_static_err("error=open failed for %s.", spath.c_str());
    delete file;
    return xrd_error_retc_map(errno);
  }
  else
  {
    retc = xrd_add_fd2file(file);
    return retc;
  }
}


//------------------------------------------------------------------------------
// Release is called when FUSE is completely done with a file; at that point,
// you can free up any temporarily allocated data structures.
//------------------------------------------------------------------------------

int
xrd_close (int fildes, unsigned long inode)
{
  eos_static_info("fd=%d inode=%lu", fildes, inode);
  int ret = 0;

  if (XFC && inode)
  {
    FileAbstraction* fAbst = XFC->GetFileObj(inode, 0);
    if (fAbst && (fAbst->GetSizeWrites() != 0))
    {
      XFC->WaitWritesAndRemove(*fAbst);
    }
  }

  eos::fst::Layout* file = xrd_get_file(fildes);
  if (file)
  {
    ret = file->Close();
    if (ret)
    {
      return -errno;
    }
  }
  else
  {
    eos_static_debug("File was already closed and removed from the map.");
  }
  return ret;
}


//------------------------------------------------------------------------------
// Flush file data to disk
//------------------------------------------------------------------------------

int
xrd_flush (int fd, unsigned long inode)
{
  int errc = 0;
  eos_static_info("fd=%d ", fd);

  if (XFC && inode)
  {
    FileAbstraction* fAbst = XFC->GetFileObj(inode, false);
    if (fAbst)
    {
      XFC->WaitFinishWrites(*fAbst);
      ConcurrentQueue<error_type> err_queue = fAbst->GetErrorQueue();
      error_type error;

      if (err_queue.try_pop(error))
      {
        eos_static_info("Extract error from queue ");
        errc = error.first;
      }

      fAbst->DecrementNoReferences();
    }
  }

  return errc;
}


//------------------------------------------------------------------------------
// Truncate file
//------------------------------------------------------------------------------

int
xrd_truncate (int fildes, off_t offset, unsigned long inode)
{
  eos_static_info("fd=%d offset=%llu inode=%lu",
                  fildes, (unsigned long long) offset, inode);
  int ret = 0;

  if (XFC && inode)
  {
    FileAbstraction* fAbst = XFC->GetFileObj(inode, false);
    if (fAbst)
    {
      XFC->WaitFinishWrites(*fAbst);
      fAbst->DecrementNoReferences();
    }
  }

  errno = 0;
  eos::fst::Layout* file = xrd_get_file(fildes);
  if (file)
  {
    ret = file->Truncate(offset);

    if (ret)
    {
      eos_static_err("error=return is NOT ok with value %i. \n", errno);
    }
  }
  else
  {
    errno = EFAULT;
  }

  return errno;
}


//------------------------------------------------------------------------------
// Read from file. Returns the number of bytes transferred, or 0 if offset
// was at or beyond the end of the file
//------------------------------------------------------------------------------

ssize_t
xrd_pread (int fildes,
           void* buf,
           size_t nbyte,
           off_t offset,
           unsigned long inode)
{
  eos::common::Timing xpr("xrd_pread");
  COMMONTIMING("start", &xpr);

  eos_static_debug("fd=%d nbytes=%lu offset=%llu inode=%lu",
                   fildes, (unsigned long) nbyte,
                   (unsigned long long) offset,
                   (unsigned long) inode);
  int64_t ret;
  eos::fst::Layout* file;

  if (XFC && fuse_cache_read && inode)
  {
    FileAbstraction* fAbst = 0;
    fAbst = XFC->GetFileObj(inode, 1);
    XFC->WaitFinishWrites(*fAbst);
    COMMONTIMING("Wait writes", &xpr);

    if ((ret = XFC->GetRead(*fAbst, buf, offset, nbyte)) != (int64_t) nbyte)
    {
      COMMONTIMING("read in", &xpr);
      eos_static_debug("Block not found in cache: off=%zu, len=%zu", offset, nbyte);
      file = xrd_get_file(fildes);

      if (file)
      {
        ret = file->Read(offset, static_cast<char*> (buf), nbyte);

        if (ret != -1)
        {
          COMMONTIMING("read out", &xpr);
          XFC->PutRead(file, *fAbst, buf, offset, nbyte);
          COMMONTIMING("put read", &xpr);
        }

        fAbst->DecrementNoReferences();
      }
      else
      {
        eos_static_err("error=file pointer is NULL");
        ret = -1;
      }
    }
    else
    {
      eos_static_debug("Block found in cache: off=%zu, len=%zu", offset, nbyte);
      COMMONTIMING("block in cache", &xpr);
    }
  }
  else
  {
    file = xrd_get_file(fildes);
    if (file)
    {
      ret = file->Read(offset, static_cast<char*> (buf), nbyte);
    }
    else
    {
      ret = EIO;
    }
  }

  COMMONTIMING("end", &xpr);

  if (ret == -1)
  {
    eos_static_err("error=failed to do read");
  }

  if (EOS_LOGS_DEBUG)
  {
    xpr.Print();
  }

  return ret;
}


//------------------------------------------------------------------------------
// Write to file
//------------------------------------------------------------------------------

ssize_t
xrd_pwrite (int fildes,
            const void* buf,
            size_t nbyte,
            off_t offset,
            unsigned long inode)
{
  eos::common::Timing xpw("xrd_pwrite");

  COMMONTIMING("start", &xpw);
  eos_static_debug("fd=%d nbytes=%lu inode=%lu cache=%d cache-w=%d",
                   fildes, (unsigned long) nbyte, (unsigned long) inode,
                   XFC ? 1 : 0, fuse_cache_write);
  int64_t ret = 0;
  XrdCl::XRootDStatus status;
  eos::fst::Layout* file = xrd_get_file(fildes);

  if (XFC && fuse_cache_write && inode)
  {
    XFC->SubmitWrite(file, inode, const_cast<void*> (buf), offset, nbyte);
    ret = nbyte;
  }
  else
  {
    ret = file->Write(offset, static_cast<const char*> (buf), nbyte);

    if (ret == -1)
    {
      errno = EIO;
    }
  }

  COMMONTIMING("end", &xpw);

  if (EOS_LOGS_DEBUG)
  {
    xpw.Print();
  }

  return ret;
}


//------------------------------------------------------------------------------
// Flush any dirty information about the file to disk
//------------------------------------------------------------------------------

int
xrd_fsync (int fildes, unsigned long inode)
{
  eos_static_info("fd=%d inode=%lu", fildes, (unsigned long) inode);
  int ret = 0;

  if (XFC && inode)
  {
    FileAbstraction* fAbst = XFC->GetFileObj(inode, false);
    if (fAbst)
    {
      XFC->WaitFinishWrites(*fAbst);
      fAbst->DecrementNoReferences();
    }
  }

  eos::fst::Layout* file = xrd_get_file(fildes);
  if (file)
  {
    ret = file->Sync();
    if (ret)
      return -1;
  }

  return ret;
}


//------------------------------------------------------------------------------
// Remove (delete) the given file, symbolic link, hard link, or special node
//------------------------------------------------------------------------------

int
xrd_unlink (const char* path,
            uid_t uid,
            pid_t pid)
{
  eos_static_info("path=%s uid=%u, pid=%u", path, uid, pid);
  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Rm(path);
  xrd_unlock_environment();

  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}


//------------------------------------------------------------------------------
// Rename file/dir
//------------------------------------------------------------------------------

int
xrd_rename (const char* oldpath,
            const char* newpath,
            uid_t uid,
            pid_t pid)
{
  eos_static_info("oldpath=%s newpath=%s", oldpath, newpath, uid, pid);
  xrd_lock_environment();
  XrdCl::URL Url(xrd_user_url(uid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Mv(oldpath, newpath);
  xrd_unlock_environment();

  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}


//------------------------------------------------------------------------------
// Get user name from the uid and change the effective user ID of the thread
//------------------------------------------------------------------------------

const char*
xrd_mapuser (uid_t uid, pid_t pid)
{
  eos_static_debug("uid=%lu pid=%lu", (unsigned long) uid, (unsigned long) pid);
  struct passwd* pw;
  XrdOucString sid = "";
  XrdOucString* spw = NULL;
  sid += (int) (uid);
  passwdstoremutex.Lock();

  if (!(spw = passwdstore->Find(sid.c_str())))
  {
    pw = getpwuid(uid);

    if (pw)
    {
      spw = new XrdOucString(pw->pw_name);
      passwdstore->Add(sid.c_str(), spw, 60);
      passwdstoremutex.UnLock();
    }
    else
    {
      passwdstoremutex.UnLock();
      return NULL;
    }
  }

  passwdstoremutex.UnLock();

  time_t now = time(NULL);

  if (fuse_environment_lock)
  {
    //............................................................................
    // export X509_USER_PROXY && KRB5CCNAME from the calling process
    // change the effective user id in this case
    //............................................................................
    static XrdSysMutex exportCacheMutex;
    static std::map <uid_t, std::map< std::string, std::string> > exportCache;
    static std::map <uid_t, time_t> exportCacheRefreshTime;

    XrdSysMutexHelper cLock(exportCacheMutex);

    if (exportCacheRefreshTime.count(uid) && (exportCacheRefreshTime[uid] > now))
    {
      if (exportCache.count(uid) && exportCache[uid].count("KRB5CCNAME"))
        setenv("KRB5CCNAME", exportCache[uid]["KRB5CCNAME"].c_str(), 1);
      if (exportCache.count(uid) && exportCache[uid].count("X509_USER_PROXY"))
        setenv("X509_USER_PROXY", exportCache[uid]["X509_USER_PROXY"].c_str(), 1);
    }
    else
    {
      // we do some gymnastic for the export variables

      char envBuffer[65536];
      envBuffer[0] = 0;
      // read the environment of the process and extract the krb5/x509 setting
      XrdOucString envFile = "/proc/";
      envFile += (int) pid;
      envFile += "/environ";

      if (exportCache.count(uid))
        exportCache[uid].erase("KRB5CCNAME");
      if (exportCache.count(uid))
        exportCache[uid].erase("X509_USER_PROXY");

      int fd = open(envFile.c_str(), O_RDONLY);
      eos_static_info("env-file=%s", envFile.c_str());
      if (fd > 0)
      {
        std::string krb5cc;
        std::string x509userproxy;

        // read the stuff
        read(fd, envBuffer, sizeof (envBuffer));
        envBuffer[65535] = 0;
        int krb5ccnamespos = -1;
        int x509proxypos = -1;
        for (size_t i = 0; i< sizeof (envBuffer) - strlen("KRB5CCNAME="); i++)
        {

          if (!strncmp(envBuffer + i, "KRB5CCNAME=", strlen("KRB5CCNAME=")))
          {
            krb5ccnamespos = i + strlen("KRB5CCNAME=");
            krb5cc = (envBuffer + krb5ccnamespos);
          }

          if (!strncmp(envBuffer + i, "X509_USER_PROXY", strlen("X509_USER_PROXY")))
          {
            x509proxypos = i + strlen("X509_USER_PROXY=");
            x509userproxy = (envBuffer + x509proxypos);
          }
        }

        if (krb5ccnamespos == -1)
        {
          krb5cc = "FILE:/tmp/krb5cc_";
          char suid[16];
          snprintf(suid, sizeof (suid) - 1, "%u", uid);
          krb5cc += suid;
        }

        if (x509proxypos == -1)
        {
          x509userproxy = "/tmp/x509u_";
          char suid[16];
          snprintf(suid, sizeof (suid) - 1, "%u", uid);
          x509userproxy += suid;
        }

        eos_static_info("export KRB5CCNAME=%s", krb5cc.c_str());
        setenv("KRB5CCNAME", krb5cc.c_str(), 1);
        exportCache[uid]["KRB5CCNAME"] = krb5cc.c_str();
        exportCacheRefreshTime[uid] = now + 300;

        eos_static_info("export X509_USER_PROXY=", x509userproxy.c_str());
        setenv("X509_USER_PROXY", x509userproxy.c_str(), 1);
        exportCache[uid]["X509_USER_PROXY"] = x509userproxy.c_str();
        exportCacheRefreshTime[uid] = now + 300;

        close(fd);
      }
    }
  }

  XrdOucString role = spw->c_str();

  {
    XrdSysMutexHelper cLock(connectionIdMutex);
    if (connectionId)
    {
      role = "";
      role += connectionId;
    }
  }
  //............................................................................
  return STRINGSTORE(role.c_str());
}

//------------------------------------------------------------------------------
// Get a user private physical connection URL like root://<user>@<host>
// - if we are a user private mount we don't need to specify that
//------------------------------------------------------------------------------

const char*
xrd_user_url (uid_t uid, pid_t pid)
{
  XrdOucString url = "root://";
  if (fuse_shared)
  {
    url += xrd_mapuser(uid, pid);
    url += "@";
  }

  url += MgmHost.c_str();
  url += "/";

  eos_static_debug("uid=%lu pid=%lu url=%s", (unsigned long) uid, (unsigned long) pid, url.c_str());
  return STRINGSTORE(url.c_str());
}

//------------------------------------------------------------------------------
// Init function
//------------------------------------------------------------------------------

void
xrd_init ()
{
  FILE* fstderr;

  //............................................................................
  // Open log file
  //..........................................................................
  if (getuid())
  {
    fuse_shared = false; //eosfsd
    char logfile[1024];
    snprintf(logfile, sizeof ( logfile) - 1, "/tmp/eos-fuse.%d.log", getuid());

    //..........................................................................
    // Running as a user ... we log into /tmp/eos-fuse.$UID.log
    //..........................................................................
    if (!(fstderr = freopen(logfile, "a+", stderr)))
    {
      fprintf(stderr, "error: cannot open log file %s\n", logfile);
    }
  }
  else
  {
    fuse_shared = true; //eosfsd
    if (getenv("EOS_FUSE_LOCK_ENVIRONMENT") && (!strcmp(getenv("EOS_FUSE_LOCK_ENVIRONMENT"), "1")))
    {
      fuse_environment_lock = true;
    }
    //..........................................................................
    // Running as root ... we log into /var/log/eos/fuse
    //..........................................................................
    eos::common::Path cPath("/var/log/eos/fuse/fuse.log");
    cPath.MakeParentPath(S_IRWXU | S_IRGRP | S_IROTH);

    if (!(fstderr = freopen(cPath.GetPath(), "a+", stderr)))
    {
      fprintf(stderr, "error: cannot open log file %s\n", cPath.GetPath());
    }
  }

  setvbuf(fstderr, (char*) NULL, _IONBF, 0);
  //............................................................................
  // Initialize hashes
  //............................................................................
  path2inode.set_empty_key("");
  path2inode.set_deleted_key("#__deleted__#");

  inode2path.set_empty_key(0);
  inode2path.set_deleted_key(0xffffffffll);

  dir2inodelist.set_empty_key(0);
  dir2inodelist.set_deleted_key(0xffffffffll);

  dir2dirbuf.set_empty_key(0);
  dir2dirbuf.set_deleted_key(0xffffffffll);

  inode2cache.set_empty_key(0);
  inode2cache.set_deleted_key(0xffffffffll);

  inodeuser2fd.set_empty_key("");
  inodeuser2fd.set_deleted_key("#__deleted__#");

  fd2fobj.set_empty_key(-1);
  fd2fobj.set_deleted_key(-2);

  //............................................................................
  // Create the root entry
  //............................................................................
  path2inode["/"] = 1;
  inode2path[1] = "/";
  eos::common::Mapping::VirtualIdentity_t vid;
  eos::common::Mapping::Root(vid);
  eos::common::Logging::Init();
  eos::common::Logging::SetUnit("FUSE@localhost");
  eos::common::Logging::gShortFormat = true;
  XrdOucString fusedebug = getenv("EOS_FUSE_DEBUG");

  if ((getenv("EOS_FUSE_DEBUG")) && (fusedebug != "0"))
  {
    eos::common::Logging::SetLogPriority(LOG_DEBUG);
  }
  else
  {
    eos::common::Logging::SetLogPriority(LOG_INFO);
  }
  //............................................................................
  // Initialise the XrdClFileSystem object
  //............................................................................
  if (fs)
  {
    delete fs;
    fs = 0;
  }

  std::string address = getenv("EOS_RDRURL");
  if (address == "")
  {
    fprintf(stderr, "error: EOS_RDRURL is not defined so we fall back to  "
            "root://localhost:1094// \n");
    address = "root://localhost:1094//";
  }

  XrdCl::URL url(address);

  if (!url.IsValid())
  {
    eos_static_info("URL is not valid.");
    exit(-1);
  }

  MgmHost = address.c_str();
  MgmHost.replace("root://", "");

  fs = new XrdCl::FileSystem(url);

  if (fs)
  {
    eos_static_info("Got new FileSystem object.");
  }

  //............................................................................
  // Check if we should set files executable
  //............................................................................
  if (getenv("EOS_FUSE_EXEC") && (!strcmp(getenv("EOS_FUSE_EXEC"), "1")))
  {
    fuse_exec = true;
  }

  //............................................................................
  // Initialise the XrdFileCache
  //............................................................................
  fuse_cache_read = false;
  fuse_cache_write = false;

  if ((!(getenv("EOS_FUSE_CACHE"))) || (getenv("EOS_FUSE_CACHE") && (!strcmp(getenv("EOS_FUSE_CACHE"), "0"))))
  {
    eos_static_notice("cache=false");
    XFC = NULL;
  }
  else
  {
    if (!getenv("EOS_FUSE_CACHE_SIZE"))
    {
      setenv("EOS_FUSE_CACHE_SIZE", "30000000", 1); // ~300MB
    }

    eos_static_notice("cache=true size=%s cache-read=%s cache-write=%s exec=%d",
                      getenv("EOS_FUSE_CACHE_SIZE"),
                      getenv("EOS_FUSE_CACHE_READ"),
                      getenv("EOS_FUSE_CACHE_WRITE"),
                      fuse_exec);

    XFC = XrdFileCache::GetInstance(static_cast<size_t> (atol(getenv("EOS_FUSE_CACHE_SIZE"))));

    if (getenv("EOS_FUSE_CACHE_READ") && atoi(getenv("EOS_FUSE_CACHE_READ")))
    {
      fuse_cache_read = true;
    }

    if (getenv("EOS_FUSE_CACHE_WRITE") && atoi(getenv("EOS_FUSE_CACHE_WRITE")))
    {
      fuse_cache_write = true;
    }
  }

  passwdstore = new XrdOucHash<XrdOucString > ();
  stringstore = new XrdOucHash<XrdOucString > ();
}

