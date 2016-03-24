// ----------------------------------------------------------------------
// File: AuthIdManager.hh
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

#ifndef __AUTHIDMANAGER__HH__
#define __AUTHIDMANAGER__HH__

#define sMaxAuthId (2^6)

/*----------------------------------------------------------------------------*/
#include "common/Logging.hh"
#include "common/RWMutex.hh"
#include "common/SymKeys.hh"
#include "ProcCacheC.h"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <list>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

//------------------------------------------------------------------------------
// Class in charge of managing the xroot login (i.e. xroot connection)
// logins are 8 characters long : ABgE73AA23@myrootserver
// it's base 64 , first 6 are userid and 2 lasts are authid
// authid is an idx a pool of identities for the specified user
// if the user comes with a new identity, it's added the pool
// if the identity is already in the pool, the connection is reused
// identity are NEVER removed from the pool
// for a given identity, the SAME conneciton is ALWAYS reused
//------------------------------------------------------------------------------

class AuthIdManager
{
public:

  bool use_user_krb5cc;
  bool use_user_gsiproxy;
  bool tryKrb5First;

  void
  setAuth (bool krb5, bool proxy, bool krb5first)
  {
    use_user_krb5cc = krb5;
    use_user_gsiproxy = proxy;
    tryKrb5First = krb5first;
  }

  void
  resize (ssize_t size)
  {
    proccachemutexes.resize (size);
    pid2StrongLogin.resize (size);
  }

  int connectionId;
  XrdSysMutex connectionIdMutex;

  void
  IncConnectionId ()
  {
    XrdSysMutexHelper l (connectionIdMutex);
    connectionId++;
  }

  enum CredType
  {
    krb5, krk5, x509
  };

  struct CredInfo
  {
    CredType type; // krb5 , krk5 or x509
    std::string lname; // link to credential file
    std::string fname; // credential file
    time_t fmtime; // credential file mtime
    time_t fctime; // credential file ctime
    time_t lmtime; // link to credential file mtime
    time_t lctime; // link to credential file mtime
    std::string identity; // identity in the credential file
    std::string cachedStrongLogin;
  };

  std::vector<eos::common::RWMutex> proccachemutexes;

  //------------------------------------------------------------------------------
  // Lock
  //------------------------------------------------------------------------------

  void
  lock_r_pcache (pid_t pid)
  {
    proccachemutexes[pid].LockRead ();
  }

  void
  lock_w_pcache (pid_t pid)
  {
    proccachemutexes[pid].LockWrite ();
  }


  //------------------------------------------------------------------------------
  // Unlock
  //------------------------------------------------------------------------------

  void
  unlock_r_pcache (pid_t pid)
  {
    proccachemutexes[pid].UnLockRead ();
  }

  void
  unlock_w_pcache (pid_t pid)
  {
    proccachemutexes[pid].UnLockWrite ();
  }

protected:
  // mutex protecting the maps
  eos::common::RWMutex pMutex;
  // maps (userid,sessionid) -> ( credinfo )
  // several threads (each from different process) might concurrently access this
  std::map< std::pair<uid_t, pid_t>, CredInfo > uidsid2credinfo;

  struct IdenAuthIdEntry
  {
    std::map<std::string, uint32_t> strongid2authid;
    std::list<uint8_t> freeAuthIdPool;

    IdenAuthIdEntry ()
    {
      // initialize the free authentication id pool with all the number from 0 to sMaxAuthId-1
      for (uint8_t i = 0; i < sMaxAuthId; i++) freeAuthIdPool.push_back (i);
    }
  };
  // maps userid -> strongid2authid  : ( identity -> authid ) , identity being krb5:<some_identity> or krk5:<some_fileless_param> or gsi:<some_identity>
  //                freeAuthIdPool   : a list with all the available authid
  // several threads (each from different process) might concurrently access this
  std::map<uid_t, IdenAuthIdEntry > uid2IdenAuthid;
  // maps procid -> xrootd_login
  // only one thread per process will access this (protected by one mutex per process)
  std::vector<std::string> pid2StrongLogin;

  bool
  findCred (CredInfo &credinfo, struct stat &linkstat, struct stat &filestat, uid_t uid, pid_t sid, time_t & sst)
  {
    if (!(use_user_gsiproxy || use_user_krb5cc)) return false;

    bool ret = false;
    char buffer[1024];
    char buffer2[1024];
    // first try the session binding if it fails, try the user binding
    const char* formats[2] = {"/var/run/eosd/credentials/uid%d_sid%d_sst%d.%s", "/var/run/eosd/credentials/uid%d.%s"};
    // krb5 -> kerberos 5 credential cache file
    // krk5 -> kerberos 5 credential cache not in a file (e.g. KeyRing)
    // x509 -> gsi authentication
    const char* suffixes[5] = {"krb5", "krk5", "x509", "krb5", "krk5"};
    CredType credtypes[5] = {krb5, krk5, x509, krb5, krk5};
    int sidx = 1, sn = 2;
    if (!use_user_krb5cc && use_user_gsiproxy)
      (sidx = 2) && (sn = 1);
    else if (use_user_krb5cc && !use_user_gsiproxy)
      (sidx = 0) && (sn = 2);
    else if (tryKrb5First)
      (sidx = 0) && (sn = 3);
    else
      (sidx = 2) && (sn = 3);

    // try all the credential types according to settings and stop as soon as a credetnial is found
    bool brk = false;
    for (int f = 0; f < 2; f++)
      {
        for (int i = sidx; i < sidx + sn; i++)
          {
            if (f == 0)
              snprintf (buffer, 1024, formats[f], (int) uid, (int) sid, (int) sst, suffixes[i]);
            else
              snprintf (buffer, 1024, formats[f], (int) uid, suffixes[i]);

            //eos_static_debug("trying to stat %s", buffer);
            if (!::lstat (buffer, &linkstat))
              {
                ret = true;
                credinfo.lname = buffer;
                credinfo.lmtime = linkstat.st_mtim.tv_sec;
                credinfo.lctime = linkstat.st_ctim.tv_sec;
                credinfo.type = credtypes[i];
                size_t bsize = readlink (buffer, buffer2, 1024);
                buffer2[bsize] = 0;
                eos_static_debug ("found credential link %s for uid %d and sid %d", credinfo.lname.c_str (), (int) uid, (int) sid);
                if (credinfo.type == krk5)
                  {
                    credinfo.fname = buffer2;
                    break; // there is no file to stat in that case
                  }
                if (!stat (buffer2, &filestat))
                  {
                    if (bsize > 0)
                      {
                        buffer2[bsize] = 0;
                        credinfo.fname = buffer2;
                        credinfo.fmtime = filestat.st_mtim.tv_sec;
                        credinfo.fctime = filestat.st_ctim.tv_sec;
                        eos_static_debug ("found credential file %s for uid %d and sid %d", credinfo.fname.c_str (), (int) uid, (int) sid);
                      }
                  }
                else
                  {
                    eos_static_debug ("could not stat file %s for uid %d and sid %d", credinfo.fname.c_str (), (int) uid, (int) sid);
                  }
                // we found some credential, we stop searching here
                brk = true;
                break;
              }
          }
        if (brk) break;
      }

    if (!ret)
      eos_static_debug ("could not find any credential for uid %d and sid %d", (int) uid, (int) sid);

    return ret;
  }

  bool
  readCred (CredInfo &credinfo)
  {
    bool ret = false;
    eos_static_debug ("reading %s credential file %s", credinfo.type == krb5 ? "krb5" : (credinfo.type == krb5 ? "krk5" : "x509"), credinfo.fname.c_str ());
    if (credinfo.type == krk5)
      {
        // fileless authentication cannot rely on symlinks to be able to change the cache credential file
        // instead of the identity, we use the keyring information and each has a different xrd login
        credinfo.identity = credinfo.fname;
        ret = true;
      }
    if (credinfo.type == krb5)
      {
        ProcReaderKrb5UserName reader (credinfo.fname);
        if (!reader.ReadUserName (credinfo.identity))
          eos_static_debug ("could not read principal in krb5 cc file %s", credinfo.fname.c_str ());
        else
          ret = true;
      }
    if (credinfo.type == x509)
      {
        ProcReaderGsiIdentity reader (credinfo.fname);
        if (!reader.ReadIdentity (credinfo.identity))
          eos_static_debug ("could not read identity in x509 proxy file %s", credinfo.fname.c_str ());
        else
          ret = true;
      }
    return ret;
  }

  bool
  checkCredSecurity (const struct stat &linkstat, const struct stat &filestat, uid_t uid, CredType credtype)
  {
    //eos_static_debug("linkstat.st_uid=%d  filestat.st_uid=%d  filestat.st_mode=%o  requiredmode=%o",(int)linkstat.st_uid,(int)filestat.st_uid,filestat.st_mode & 0777,reqMode);
    if (
        // check owner ship
        linkstat.st_uid == uid)
      {
        if (credtype == krk5)
          return true;
        else if (filestat.st_uid == uid && (filestat.st_mode & 0077) == 0 // no access to other users/groups
                 && (filestat.st_mode & 0400) != 0 // read allowed for the user
                 ) return true;
      }

    return false;
  }

  int
  updateProcCache (uid_t uid, gid_t gid, pid_t pid, bool reconnect)
  {
    // when entering this function proccachemutexes[pid] must be write locked
    int errCode;
    char buffer[1024];

    // this is useful even in gateway mode because of the recursive deletion protection
    if ((errCode = proccache_InsertEntry (pid)))
      {
        eos_static_err ("updating proc cache information for process %d. Error code is %d", (int) pid, errCode);
        return errCode;
      }

    // check if we are using strong authentication
    if (!(use_user_krb5cc || use_user_gsiproxy))
      return 0;

    // get the startuptime of the process
    time_t processSut;
    proccache_GetStartupTime (pid, &processSut);
    // get the session id
    pid_t sid;
    proccache_GetSid (pid, &sid);
    bool isSessionLeader = (sid == pid);
    // update the proccache of the session leader
    if (!isSessionLeader)
      {
        lock_w_pcache (sid);
        if ((errCode = proccache_InsertEntry (sid)))
          {
            eos_static_err ("updating proc cache information for session leader process %d. Error code is %d", (int) pid, errCode);
            unlock_w_pcache (sid);
            return errCode;
          }
        unlock_w_pcache (sid);
      }

    // get the startuptime of the leader of the session
    time_t sessionSut;
    proccache_GetStartupTime (sid, &sessionSut);

    // find the credentials
    CredInfo credinfo;
    struct stat filestat, linkstat;
    if (!findCred (credinfo, linkstat, filestat, uid, sid, sessionSut))
      {
        eos_static_notice ("could not find any credential");
        return EACCES;
      }

    // check if the credentials in the credential cache cache are up to date
    // TODO: should we implement a TTL , my guess is NO
    bool sessionInCache = false;
    pMutex.LockRead ();
    auto cacheEntry = uidsid2credinfo.find (std::make_pair (uid, sid));
    // skip the cache if reconnecting
    sessionInCache = !reconnect && (cacheEntry != uidsid2credinfo.end ());
    if (sessionInCache)
      {
        sessionInCache = false;
        const CredInfo &ci = cacheEntry->second;
        // we also check ctime to be sure that permission/ownership has not changed
        if (ci.lmtime == credinfo.lmtime
            && ci.lctime == credinfo.lctime)
          {
            if (credinfo.type == krk5) // if this is fileless credential cache, no target file to check
              sessionInCache = true;
            else if (ci.fmtime == credinfo.fmtime
                     && ci.fctime == credinfo.fctime
                     && ci.fname == credinfo.fname)
              sessionInCache = true;
          }
      }
    pMutex.UnLockRead ();

    if (sessionInCache)
      {
        // TODO: could detect from the call to ptoccahce_InsertEntry if the process was changed
        //       then, it would be possible to bypass this part copy, which is probably not the main bottleneck anyway
        // no lock needed as only one thread per process can access this (lock is supposed to be already taken -> beginning of the function)
        eos_static_debug ("uid=%d  sid=%d  pid=%d  found stronglogin in cache %s", (int) uid, (int) sid, (int) pid, cacheEntry->second.cachedStrongLogin.c_str ());
        pid2StrongLogin[pid] = cacheEntry->second.cachedStrongLogin;
        proccache_GetAuthMethod (sid, buffer, 1024);
        proccache_SetAuthMethod (pid, buffer);
        return 0;
      }

    // refresh the credentials in the cache
    // check the credential security
    if (!checkCredSecurity (linkstat, filestat, uid, credinfo.type))
      {
        eos_static_alert ("credentials are not safe");
        return EACCES;
      }
    // check the credential security
    if (!readCred (credinfo))
      return EACCES;
    // update authmethods for session leader and current pid
    uint8_t authid;
    std::string sId;
    if (credinfo.type == krb5) sId = "krb5:";
    else if (credinfo.type == krk5) sId = "krk5:";
    else sId = "x509:";

    std::string newauthmeth;
    if (credinfo.type == krk5)
      {
        // using directly the value of the pointed file (which is the text of the in memory credentials)
        sId.append (credinfo.fname);
        newauthmeth = sId;
      }
    else
      {
        // using the link created by the user
        sId.append (credinfo.lname);
        newauthmeth = sId;
      }

    if (newauthmeth.empty ())
      {
        eos_static_err ("error symlinking credential file ");
        return EACCES;
      }
    proccache_SetAuthMethod (pid, newauthmeth.c_str ());
    proccache_SetAuthMethod (sid, newauthmeth.c_str ());
    // update uid2IdenAuthid
    {
      eos::common::RWMutexWriteLock lock (pMutex);
      // this will create the entry in uid2IdenAuthid if it does not exist already
      auto &uidEntry = uid2IdenAuthid[uid];
      if (credinfo.type == krb5) sId = "krb5:";
      else if (credinfo.type == krk5) sId = "krk5:";
      else sId = "x509:";
      sId.append (credinfo.identity);
      if (!reconnect && uidEntry.strongid2authid.count (sId))
        authid = uidEntry.strongid2authid[sId]; // if this identity already has an authid , use it
      else
        {
          if (uidEntry.freeAuthIdPool.empty ())
            {
              eos_static_err ("reached maximum number of connections for uid %d. cannot bind identity [%s] to a new xrootd connection! "
                              , (int) uid, sId.c_str ());
              return EACCES;
            }
          // get the new connection id
          uint8_t newauthid = uidEntry.freeAuthIdPool.front ();
          // remove the new connection from the connections available to be used
          uidEntry.freeAuthIdPool.pop_front ();
          // recycle the previsous connection number if reconnecting
          if (reconnect)
            {
              eos_static_debug ("dropping authid %d", (int) uidEntry.strongid2authid[sId]);
              uidEntry.freeAuthIdPool.push_back (uidEntry.strongid2authid[sId]);
            }
          // store the new authid
          eos_static_debug ("using newauthid %d", (int) newauthid);
          authid = (uidEntry.strongid2authid[sId] = newauthid); // create a new authid if this id is not registered yet
        }
    }
    // update pid2StrongLogin (no lock needed as only one thread per process can access this)
    map_user xrdlogin (uid, gid, authid);
    std::string mapped = mapuser (uid, gid, 0, authid);
    pid2StrongLogin[pid] = std::string (xrdlogin.base64 (mapped));
    // update uidsid2credinfo
    credinfo.cachedStrongLogin = pid2StrongLogin[pid];
    eos_static_debug ("uid=%d  sid=%d  pid=%d  writing stronglogin in cache %s", (int) uid, (int) sid, (int) pid, credinfo.cachedStrongLogin.c_str ());
    pMutex.LockWrite ();
    uidsid2credinfo[std::make_pair (uid, sid)] = credinfo;
    pMutex.UnLockWrite ();

    eos_static_debug ("qualifiedidentity [%s] used for pid %d, xrdlogin is %s (%d/%d)", sId.c_str (), (int) pid,
                      pid2StrongLogin[pid].c_str (), (int) uid, (int) authid);

    return errCode;
  }

  struct map_user
  {
    uid_t uid;
    gid_t gid;
    uint8_t authid;
    // first 20 bits for user id
    // 16 following bites for authid
    // 6 following bits for auth id (identity)
    char base64buf[9];
    bool base64computed;

    map_user (uid_t _uid, gid_t _gid, uint8_t _authid) :
    uid (_uid), gid (_gid), authid (_authid), base64computed (false) { }

    char*
    base64 (std::string& mapped)
    {
      if (!base64computed)
        {
          // pid is actually meaningless
          strncpy (base64buf, mapped.c_str (), 8);
          base64buf[8] = 0;
          base64computed = true;
        }
      return base64buf;
    }
  };

  //------------------------------------------------------------------------------
  // Get user name from the uid and change the effective user ID of the thread
  //------------------------------------------------------------------------------

  std::string
  mapuser (uid_t uid, gid_t gid, pid_t pid, uint8_t authid);

public:

  inline int
  updateProcCache (uid_t uid, gid_t gid, pid_t pid)
  {
    eos::common::RWMutexWriteLock lock (proccachemutexes[pid]);
    return updateProcCache (uid, gid, pid, false);
  }

  inline int
  reconnectProcCache (uid_t uid, gid_t gid, pid_t pid)
  {
    eos::common::RWMutexWriteLock lock (proccachemutexes[pid]);
    return updateProcCache (uid, gid, pid, true);
  }

  std::string
  getXrdLogin (pid_t pid)
  {
    eos::common::RWMutexReadLock lock (proccachemutexes[pid]);
    return pid2StrongLogin[pid];
  }

  std::string
  getLogin (uid_t uid, gid_t gid, pid_t pid)
  {
    return (use_user_krb5cc || use_user_gsiproxy) ? getXrdLogin (pid) : mapuser (uid, gid, pid, 0);
  }

};

#endif // __AUTHIDMANAGER__HH__