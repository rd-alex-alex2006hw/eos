/*----------------------------------------------------------------------------*/
#include "mq/XrdMqSharedObject.hh"
#include "mq/XrdMqMessaging.hh"
#include "mq/XrdMqStringConversion.hh"
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
// system includes

/*----------------------------------------------------------------------------*/

bool XrdMqSharedObjectManager::debug=0;

unsigned long long XrdMqSharedHash::SetCounter=0;
unsigned long long XrdMqSharedHash::SetNLCounter=0;
unsigned long long XrdMqSharedHash::GetCounter=0;

/*----------------------------------------------------------------------------*/
XrdMqSharedObjectManager::XrdMqSharedObjectManager() 
{
  EnableQueue    = false;
  DumperFile     = "";
  AutoReplyQueue = "";
  AutoReplyQueueDerive = false;
  IsMuxTransaction = false;
  MuxTransactions.clear();
}

/*----------------------------------------------------------------------------*/
XrdMqSharedObjectManager::~XrdMqSharedObjectManager() 
{

}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedObjectManager::SetAutoReplyQueue(const char* queue) 
{
  AutoReplyQueue = queue;
}

/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedObjectManager::CreateSharedHash(const char* subject, const char* broadcastqueue, XrdMqSharedObjectManager* som) 
{
  std::string ss = subject;

  HashMutex.LockWrite();
  
  if (hashsubjects.count(ss)>0) {
    hashsubjects[ss]->SetBroadCastQueue(broadcastqueue);
    HashMutex.UnLockWrite();
    return false;
  } else {
    XrdMqSharedHash* newhash = new XrdMqSharedHash(subject, broadcastqueue, som);

    hashsubjects.insert( std::pair<std::string, XrdMqSharedHash*> (ss, newhash));
    
    HashMutex.UnLockWrite();

    if (EnableQueue) {
      SubjectsMutex.Lock();
      CreationSubjects.push_back(ss);
      SubjectsMutex.UnLock();
      SubjectsSem.Post();
    }

    return true;
  }
}

/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedObjectManager::CreateSharedQueue(const char* subject, const char* broadcastqueue, XrdMqSharedObjectManager* som) 
{
  std::string ss = subject;

  ListMutex.LockWrite();
  
  if (queuesubjects.count(ss)>0) {
    ListMutex.UnLockWrite();
    return false;
  } else {
    XrdMqSharedQueue newlist(subject, broadcastqueue, som);

    queuesubjects.insert( std::pair<std::string, XrdMqSharedQueue> (ss, newlist));
    
    ListMutex.UnLockWrite();

    if (EnableQueue) {
      SubjectsMutex.Lock();
      CreationSubjects.push_back(ss);
      SubjectsMutex.UnLock();
      SubjectsSem.Post();
    }
    return true;
  }
}


/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedObjectManager::DeleteSharedHash(const char* subject, bool broadcast) 
{
  std::string ss = subject;
  
  HashMutex.LockWrite();
  
  if ((hashsubjects.count(ss)>0)) {
    if (broadcast) {
      XrdOucString txmessage="";
      hashsubjects[ss]->MakeRemoveEnvHeader(txmessage);
      XrdMqMessage message("XrdMqSharedHashMessage");
      message.SetBody(txmessage.c_str());
      message.MarkAsMonitor();
      XrdMqMessaging::gMessageClient.SendMessage(message);
    }
    delete (hashsubjects[ss]);
    hashsubjects.erase(ss);
    HashMutex.UnLockWrite();

    if (EnableQueue) {
      SubjectsMutex.Lock();
      DeletionSubjects.push_back(ss);
      SubjectsMutex.UnLock();
      SubjectsSem.Post();
    }    

    return true;
  } else {
    HashMutex.UnLockWrite();
    return true;
  }
}

/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedObjectManager::DeleteSharedQueue(const char* subject, bool broadcast) 
{
  std::string ss = subject;
  
  ListMutex.LockWrite();
  
  if ((queuesubjects.count(ss)>0)) {
    if (broadcast) {
      XrdOucString txmessage="";
      hashsubjects[ss]->MakeRemoveEnvHeader(txmessage);
      XrdMqMessage message("XrdMqSharedHashMessage");
      message.SetBody(txmessage.c_str());
      message.MarkAsMonitor();
      XrdMqMessaging::gMessageClient.SendMessage(message);
    }

    queuesubjects.erase(ss);
    ListMutex.UnLockWrite();

    if (EnableQueue) {
      SubjectsMutex.Lock();
      DeletionSubjects.push_back(ss);
      SubjectsMutex.UnLock();
      SubjectsSem.Post();
    }

    return true;
  } else {
    ListMutex.UnLockWrite();
    return true;
  }
}

/*----------------------------------------------------------------------------*/
void 
XrdMqSharedObjectManager::DumpSharedObjects(XrdOucString& out) 
{
  out="";

  XrdMqRWMutexReadLock lock(HashMutex);
  std::map<std::string , XrdMqSharedHash*>::iterator it_hash;
  std::map<std::string , XrdMqSharedQueue>::iterator it_queue;
  for (it_hash=hashsubjects.begin(); it_hash!= hashsubjects.end(); it_hash++) {
    out += "===================================================\n";
    out += it_hash->first.c_str(); out += " [ hash=>  "; out += it_hash->second->GetBroadCastQueue();
    out += " ]\n";
    out += "---------------------------------------------------\n";
    it_hash->second->Dump(out);
  }

  for (it_queue=queuesubjects.begin(); it_queue!= queuesubjects.end(); it_queue++) {
    out += "===================================================\n";
    out += it_queue->first.c_str(); out += " [ queue=> "; out += it_queue->second.GetBroadCastQueue();
    out += " ]\n";
    out += "---------------------------------------------------\n";
    it_queue->second.Dump(out);
  }
}

/*----------------------------------------------------------------------------*/
void 
XrdMqSharedObjectManager::DumpSharedObjectList(XrdOucString& out) 
{
  out="";
  char formatline[1024];

  XrdMqRWMutexReadLock lock(HashMutex);

  std::map<std::string , XrdMqSharedHash*>::iterator it_hash;
  for (it_hash=hashsubjects.begin(); it_hash!= hashsubjects.end(); it_hash++) {
    snprintf(formatline,sizeof(formatline)-1,"subject=%32s broadcastqueue=%32s size=%u changeid=%llu\n",it_hash->first.c_str(), it_hash->second->GetBroadCastQueue(),(unsigned int) it_hash->second->GetSize(), it_hash->second->GetChangeId());
    out += formatline;
  }

  //  ListMutex.LockRead();
  //  std::map<std::string , XrdMqSharedQueue>::const_iterator it_list;
  //  for (it_list=queuesubjects.begin(); it_list!= queuesubjects.end(); it_list++) {
  //    snprintf(formatline,sizeof(formatline)-1,"subject=%32s broadcastqueue=%32s size=%u changeid=%llu\n",it_hash.first.c_str(), it_hash.second.GetBroadCastQueue(),(unsigned int) it_hash.second.GetSize(), it_hash.second.GetChangeId());
  //out += formatline;
  //  }
  //  ListMutex.UnLockRead();
}

/*----------------------------------------------------------------------------*/
void 
XrdMqSharedObjectManager::StartDumper(const char* file) 
{
  pthread_t tid;
  int rc=0;
  DumperFile = file;
  if ((rc = XrdSysThread::Run(&tid, XrdMqSharedObjectManager::StartHashDumper, static_cast<void *>(this),
                              0, "HashDumper"))) {
    fprintf(stderr,"XrdMqSharedObjectManager::StartDumper=> failed to run dumper thread\n");
  }
}

/*----------------------------------------------------------------------------*/
void*
XrdMqSharedObjectManager::StartHashDumper(void* pp) 
{
  XrdMqSharedObjectManager* man = (XrdMqSharedObjectManager*) pp;
  man->FileDumper();
  // should never return
  return 0;
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedObjectManager::FileDumper()
{
  while (1) {
    XrdOucString s;
    DumpSharedObjects(s);
    std::string df = DumperFile;
    df += ".tmp";
    FILE* f = fopen(df.c_str(),"w+");
    if (f) {
      fprintf(f,"%s\n", s.c_str());
      fclose(f);
    }
    if (rename(df.c_str(),DumperFile.c_str())) {
      fprintf(stderr,"XrdMqSharedObjectManager::FileDumper=> unable to write dumper file %s\n", DumperFile.c_str());
    }
    for (size_t i=0; i< 1000; i++) {
      usleep(10000);
    }
  }
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedObjectManager::PostModificationTempSubjects() 
{
  std::deque<std::string>::iterator it;
  if (debug)fprintf(stderr,"XrdMqSharedObjectManager::PostModificationTempSubjects=> posting now\n");
  SubjectsMutex.Lock();
  for (it=ModificationTempSubjects.begin(); it!=ModificationTempSubjects.end(); it++) {
    if (debug)fprintf(stderr,"XrdMqSharedObjectManager::PostModificationTempSubjects=> %s\n", it->c_str());
    ModificationSubjects.push_back(*it);
    SubjectsSem.Post();
  }
  ModificationTempSubjects.clear();
  SubjectsMutex.UnLock();
}


/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedObjectManager::ParseEnvMessage(XrdMqMessage* message, XrdOucString &error) 
{
  error = "";
  std::string subject = "";
  std::string reply   = "";
  std::string type = "";

  if (!message) {
    error = "no message provided";
    return false;
  }

  XrdOucEnv env(message->GetBody());

  int envlen;
  env.Env(envlen);
  if (debug){env.Env(envlen);fprintf(stderr,"XrdMqSharedObjectManager::ParseEnvMessage=> size=%d text=%s\n",envlen, env.Env(envlen));}

  if (env.Get(XRDMQSHAREDHASH_SUBJECT)) {
    subject = env.Get(XRDMQSHAREDHASH_SUBJECT);
  } else {
    error = "no subject in message body";
    return false;
  }

  if (env.Get(XRDMQSHAREDHASH_REPLY)) {
    reply = env.Get(XRDMQSHAREDHASH_REPLY);
  } else {
    reply = "";
  }

  if (env.Get(XRDMQSHAREDHASH_TYPE)) {
    type = env.Get(XRDMQSHAREDHASH_TYPE);
  } else {
    error = "no hash type in message body";
    return false;
  }

  if (env.Get(XRDMQSHAREDHASH_CMD)) {
    HashMutex.LockRead();
    XrdMqSharedHash* sh = 0;

    std::vector<std::string> subjectlist;

    // support 'wild card' broadcasts with <name>/*
    int wpos=0;
    if ((wpos = subject.find("/*")) != STR_NPOS) {
      XrdOucString wmatch = subject.c_str();
      wmatch.erase(wpos);
      {
        std::map<std::string, XrdMqSharedHash*>::iterator it;
        for (it=hashsubjects.begin(); it != hashsubjects.end(); it++) {
          XrdOucString hs = it->first.c_str();
          if (hs.beginswith(wmatch)) {
            subjectlist.push_back(hs.c_str());
          }
        }
      }
      {
        std::map<std::string, XrdMqSharedQueue>::iterator it;
        for (it=queuesubjects.begin(); it != queuesubjects.end(); it++) {
          XrdOucString hs = it->first.c_str();
          if (hs.beginswith(wmatch)) {
            subjectlist.push_back(hs.c_str());
          }
        }
      }
    } else {
      std::string delimiter = "%";
      // we support also multiplexed subject updates and split the list
      XrdMqStringConversion::Tokenize(subject,subjectlist,delimiter);
    }

    XrdOucString ftag = XRDMQSHAREDHASH_CMD; ftag += "="; ftag += env.Get(XRDMQSHAREDHASH_CMD);

    if (subjectlist.size()>0)
      sh = GetObject(subjectlist[0].c_str(),type.c_str());

    if ( (ftag == XRDMQSHAREDHASH_BCREQUEST) || (ftag == XRDMQSHAREDHASH_DELETE) || (ftag == XRDMQSHAREDHASH_REMOVE) ) {
      // if we don't know the subject, we don't create it with a BCREQUEST
      if ( ( ftag == XRDMQSHAREDHASH_BCREQUEST) && (reply == "")) {
        HashMutex.UnLockRead();
        error = "bcrequest: no reply address present";
        return false;
      }
      
      if (!sh) {
        if (ftag == XRDMQSHAREDHASH_BCREQUEST) {
          error = "bcrequest: don't know this subject";
        } 
        if (ftag == XRDMQSHAREDHASH_DELETE) {
          error = "delete: don't know this subject";
        }
	if (ftag == XRDMQSHAREDHASH_REMOVE) {
	  error = "remove: don't know this subject";
	}
        HashMutex.UnLockRead();
        return false;
      } else {
        HashMutex.UnLockRead();
      }
    } else {
      // automatically create the subject, if it does not exist
      if (!sh) {
        HashMutex.UnLockRead();
        if (AutoReplyQueueDerive) {
          AutoReplyQueue = subject.c_str();
          int pos=0;
          for (int i=0; i< 4; i++) {
            pos= subject.find("/",pos); 
            if (i<3) {
              if (pos== STR_NPOS) {
                AutoReplyQueue="";
                error = "cannot derive the reply queue from "; error += subject.c_str();
                return false;
              } else {
                pos++;
              }
            } else {
              AutoReplyQueue.erase(pos);
            }
          }
        }

        // create the list of subjects
        for (size_t i=0; i< subjectlist.size(); i++ ) {
          if (!CreateSharedObject(subjectlist[i].c_str(),AutoReplyQueue.c_str() ,type.c_str())) {
            error = "cannot create shared object for "; error += subject.c_str(); error += " and type "; error += type.c_str();
            return false;
          }
        }

	{
	  XrdMqRWMutexReadLock lock(HashMutex);
	  sh = GetObject(subject.c_str(), type.c_str());
	}
      } else {
        HashMutex.UnLockRead();
      }
    }

    {
      XrdMqRWMutexReadLock lock(HashMutex);
      // from here on we have a read lock on 'sh'
      
      if ( (ftag == XRDMQSHAREDHASH_UPDATE) || (ftag == XRDMQSHAREDHASH_BCREPLY) ) {
	std::string val = (env.Get(XRDMQSHAREDHASH_PAIRS)?env.Get(XRDMQSHAREDHASH_PAIRS):"");
	if ( val.length() <=0 ) {
	  error = "no pairs in message body";
	  return false;
	}
	
	if (ftag == XRDMQSHAREDHASH_BCREPLY) {
          // we don't have to broad cast this clear => it is a broad cast reply
          sh->Clear(false);
	}

        std::string key;
        std::string value;
        std::string cid;
        std::vector<int> keystart;
        std::vector<int> valuestart;
        std::vector<int> cidstart;
        
        for (unsigned int i=0; i< val.length(); i++) {
          if (val.c_str()[i] == '|') {
            keystart.push_back(i);
          }
          if (val.c_str()[i] == '~') {
            valuestart.push_back(i);
          }
          if (val.c_str()[i] == '%') {
            cidstart.push_back(i);
          }
        }
        
        if (keystart.size() != valuestart.size()) {
          error = "update: parsing error in pairs tag";
          return false;
        }
        
        if (keystart.size() != cidstart.size()) {
          error = "update: parsing error in pairs tag";
          return false;
        }
        
        
        int parseindex=0;
        for (size_t s=0; s< subjectlist.size(); s++) {
          sh = GetObject(subjectlist[s].c_str(),type.c_str());	
          if (!sh) {
            error = "update: subject does not exist (FATAL!)";
            return false;
          }          
          
          
          std::string sstr;

          //          XrdSysMutexHelper(TransactionMutex);
            
          {
            sh->StoreMutex.LockWrite();
            SubjectsMutex.Lock();
            for (unsigned int i=parseindex ; i< keystart.size(); i++) {
              key.assign(val,keystart[i]+1, valuestart[i]-1 - (keystart[i]));
              value.assign(val,valuestart[i]+1, cidstart[i]-1 - (valuestart[i]));
              if (i == (keystart.size()-1)) {
                cid.assign(val,cidstart[i]+1, val.length()-cidstart[i]-1);
              } else {
                cid.assign(val,cidstart[i]+1,keystart[i+1]-1 - (cidstart[i]));
              }
              if (debug)fprintf(stderr,"XrdMqSharedObjectManager::ParseEnvMessage=>Setting [%s] %s=> %s\n", subject.c_str(),key.c_str(), value.c_str());
              if (subjectlist.size()>1) {
                // this is a multiplexed update, where we have to remove the subject from the key if there is a match with the current subject
                // MUX transactions have the #<subject-index># as key prefix
                XrdOucString skey="#"; skey+= (int)s; skey+= "#";
                if (!key.compare(0, skey.length(),skey.c_str())) {
                  // this is the right key for the subject we are dealing with
                  key.erase(0, skey.length());
                } else {
                  parseindex=i;
                  break;
                }
              } else {
                // this can be the case for a single multiplexed message, so we have also to remove the prefix in that case
                XrdOucString skey="#"; skey+= (int)s; skey+= "#";
                if (!key.compare(0, skey.length(), skey.c_str())) {
                  // this is the right key for the subject we are dealing with
                  key.erase(0, skey.length());
                }
              }
              
              sh->SetNoLockNoBroadCast(key.c_str(), value.c_str(), true);
              //sh->Set(key, value, false, true);            
            }
            sh->StoreMutex.UnLockWrite();
            SubjectsMutex.UnLock();
          }
          PostModificationTempSubjects();
        }
        return true;
      }
      
      if (ftag == XRDMQSHAREDHASH_BCREQUEST) {
	bool success = true;
	for (unsigned int l=0; l< subjectlist.size(); l++) {
	  // try 'queue' and 'hash' to have wildcard broadcasts for both
	  sh = GetObject(subjectlist[l].c_str(), "hash");
	  if (!sh) {
	    sh = GetObject(subjectlist[l].c_str(),"queue");
	  }
	  
	  if (sh) {
	    success *= sh->BroadCastEnvString(reply.c_str());
	  }
	}
	return success;
      }
      
      if (ftag == XRDMQSHAREDHASH_DELETE) {
	std::string val = (env.Get(XRDMQSHAREDHASH_KEYS)?env.Get(XRDMQSHAREDHASH_KEYS):"");
	if ( val.length() <=1 ) {
	  error = "no keys in message body : ";
          error += env.Env(envlen);
	  return false;
	}
      
	std::string key;
	std::vector<int> keystart;
	
	for (unsigned int i=0; i< val.length(); i++) {
	  if (val.c_str()[i] == '|') {
          keystart.push_back(i);
	  }
	}
	
	XrdSysMutexHelper(TransactionMutex);
	std::string sstr;
	for (unsigned int i=0 ; i< keystart.size(); i++) {
	  if (i < (keystart.size()-1)) 
	    sstr = val.substr(keystart[i]+1, keystart[i+1]-1 - (keystart[i]));
	  else 
	    sstr = val.substr(keystart[i]+1);
	  
	  key = sstr;
          //          message->Print();
          //	  fprintf(stderr,"XrdMqSharedObjectManager::ParseEnvMessage=>Deleting [%s] %s\n", subject.c_str(),key.c_str());
	  sh->Delete(key.c_str(), false);
	  
	}
      }
    } // end of read mutex on HashMutex

    if (ftag == XRDMQSHAREDHASH_REMOVE) {
      for (unsigned int l=0; l< subjectlist.size(); l++) {
	if (!DeleteSharedObject(subjectlist[l].c_str(),type.c_str(),false)) {
	  error = "cannot delete subject "; error += subjectlist[l].c_str();
	  return false;
	}
      }
    }
    return true;

  }
  error="unknown message: ";error += message->GetBody();
  return false;
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedObjectManager::Clear() 
{
  XrdMqRWMutexReadLock lock(HashMutex);
  std::map<std::string , XrdMqSharedHash*>::iterator it_hash;
  for (it_hash=hashsubjects.begin(); it_hash!= hashsubjects.end(); it_hash++) {
    it_hash->second->Clear();
  }
  std::map<std::string , XrdMqSharedQueue>::iterator it_queue;
  for (it_queue=queuesubjects.begin(); it_queue!= queuesubjects.end(); it_queue++) {
    it_queue->second.Clear();
  }
}

/*----------------------------------------------------------------------------*/
bool
XrdMqSharedObjectManager::CloseMuxTransaction() 
{
  // Mux Transactions can only update values with the same broadcastqueue, no deletions of subjects

  if (MuxTransactions.size()) {
    XrdOucString txmessage="";
    MakeMuxUpdateEnvHeader(txmessage);
    AddMuxTransactionEnvString(txmessage);
    XrdMqMessage message("XrdMqSharedHashMessage");
    message.SetBody(txmessage.c_str());
    message.MarkAsMonitor();
    XrdMqMessaging::gMessageClient.SendMessage(message, MuxTransactionBroadCastQueue.c_str());
  }

  IsMuxTransaction = false;
  MuxTransactions.clear();
  MuxTransactionMutex.UnLock();
  return true;
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedObjectManager::MakeMuxUpdateEnvHeader(XrdOucString &out)
{
  std::string subjects="";
  std::map<std::string,std::set <std::string> >::const_iterator it;
  for (it = MuxTransactions.begin(); it != MuxTransactions.end(); it++) {
    subjects += it->first;
    subjects += "%";
  }
  // remove trailing '%'
  if (subjects.length() > 0)
    subjects.erase(subjects.length()-1,1);
  
  out = XRDMQSHAREDHASH_UPDATE; out += "&"; out += XRDMQSHAREDHASH_SUBJECT; out += "="; out += subjects.c_str();
  out += "&"; out += XRDMQSHAREDHASH_TYPE;    out += "="; out += MuxTransactionType.c_str();
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedObjectManager::AddMuxTransactionEnvString(XrdOucString &out)
{
  // encoding works as "mysh.pairs=|<key1>~<value1>%<changeid1>|<key2>~<value2>%<changeid2 ...."
  out += "&"; out += XRDMQSHAREDHASH_PAIRS; out += "=";
  std::map< std::string, std::set<std::string> >::const_iterator subjectit;
  
  size_t index=0;
  for (subjectit = MuxTransactions.begin(); subjectit != MuxTransactions.end(); subjectit++) {
    XrdOucString sindex="";
    sindex += (int)index;
    // loop over subjects
    std::set<std::string>::const_iterator it;

    XrdMqSharedHash* hash = GetObject(subjectit->first.c_str(), MuxTransactionType.c_str());

    if (hash) {
      XrdMqRWMutexReadLock lock(hash->StoreMutex);
      for (it = subjectit->second.begin(); it != subjectit->second.end(); it++) {
        // loop over variables
        
        if ( (hash->Store.count(it->c_str() ))) {
          out += "|";
          // the subject is a prefix to the key as #<subject-index>#
          out += "#"; out += sindex.c_str(); out += "#";
          out += it->c_str();
          out += "~";
          out += hash->Store[it->c_str()].entry.c_str();
          out += "%";
          char cid[1024];snprintf(cid,sizeof(cid)-1,"%llu",hash->Store[it->c_str()].ChangeId);
          out += cid;
        }
      }
    }
    index++;
  }
}


/*----------------------------------------------------------------------------*/
XrdMqSharedHash::XrdMqSharedHash(const char* subject, const char* broadcastqueue, XrdMqSharedObjectManager* som) 
{
  BroadCastQueue = broadcastqueue;
  Subject        = subject;
  ChangeId       = 0;
  IsTransaction  = false;
  Type           = "hash";
  SOM            = som;
}

/*----------------------------------------------------------------------------*/
XrdMqSharedHash::~XrdMqSharedHash() 
{

}

/*----------------------------------------------------------------------------*/
std::string 
XrdMqSharedHash::StoreAsString(const char* notprefix)
{
  std::string s="";
  StoreMutex.LockRead();
  std::map<std::string, XrdMqSharedHashEntry>::iterator it;
  for (it=Store.begin(); it!= Store.end(); it++) {
    XrdOucString key = it->first.c_str();
    if ( (!notprefix) || (notprefix && (!strlen(notprefix))) || (!key.beginswith(notprefix))) {
      s+= it->first.c_str(); s+= "=";
      s+= it->second.GetEntry(); s+= " ";
    }
  }
  StoreMutex.UnLockRead();
  return s;
}

/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedObjectManager::OpenMuxTransaction(const char* type, const char* broadcastqueue) {
  MuxTransactionType = type;
  if (MuxTransactionType != "hash") {
    return false;
  }
  if (!broadcastqueue) {
    if (AutoReplyQueue.length())
      MuxTransactionBroadCastQueue=AutoReplyQueue;
    else
      return false;
  } else {
    MuxTransactionBroadCastQueue = broadcastqueue;
  }
  MuxTransactionMutex.Lock();
  MuxTransactions.clear();
  
  IsMuxTransaction= true;
  return true;
}

/*----------------------------------------------------------------------------*/
bool
XrdMqSharedHash::CloseTransaction() 
{
  bool retval=true;
  if (Transactions.size()) {
    XrdOucString txmessage="";
    MakeUpdateEnvHeader(txmessage);
    AddTransactionEnvString(txmessage, false);

    if (txmessage.length() > (2*1000*1000)) {
      // we set the message size limit to 2M, if the message is bigger, we just send transaction item by item
      std::set<std::string>::const_iterator transit;
      for (transit= Transactions.begin(); transit != Transactions.end(); transit++) {
	txmessage="";
	MakeUpdateEnvHeader(txmessage);
	
        // encoding works as "mysh.pairs=|<key1>~<value1>%<changeid1>|<key2>~<value2>%<changeid2 ...."
	txmessage += "&"; txmessage += XRDMQSHAREDHASH_PAIRS; txmessage += "=";

	XrdMqRWMutexReadLock lock(StoreMutex);
	if ( (Store.count(transit->c_str() ))) {
	  txmessage += "|";
	  txmessage += transit->c_str();
	  txmessage += "~";
	  txmessage += Store[transit->c_str()].entry.c_str();
	  txmessage += "%";
	  char cid[1024];snprintf(cid,sizeof(cid)-1,"%llu",Store[transit->c_str()].ChangeId);
	  txmessage += cid;
	}
	XrdMqMessage message("XrdMqSharedHashMessage");
	message.SetBody(txmessage.c_str());
	message.MarkAsMonitor();
	retval &= XrdMqMessaging::gMessageClient.SendMessage(message, BroadCastQueue.c_str());
      }
      Transactions.clear();
    } else {
      Transactions.clear();
      XrdMqMessage message("XrdMqSharedHashMessage");
      message.SetBody(txmessage.c_str());
      message.MarkAsMonitor();
      retval &= XrdMqMessaging::gMessageClient.SendMessage(message, BroadCastQueue.c_str());
    }
  }

  if (Deletions.size()) {
    XrdOucString txmessage="";
    MakeDeletionEnvHeader(txmessage);
    AddDeletionEnvString(txmessage);
    XrdMqMessage message("XrdMqSharedHashMessage");
    message.SetBody(txmessage.c_str());
    message.MarkAsMonitor();
    retval &= XrdMqMessaging::gMessageClient.SendMessage(message, BroadCastQueue.c_str());
  }

  IsTransaction = false;
  TransactionMutex.UnLock();
  return retval;
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::MakeBroadCastEnvHeader(XrdOucString &out)
{
  out = XRDMQSHAREDHASH_BCREPLY; out += "&"; out += XRDMQSHAREDHASH_SUBJECT; out += "="; out += Subject.c_str();
  out += "&"; out += XRDMQSHAREDHASH_TYPE;    out += "="; out += Type.c_str();
}


/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::MakeUpdateEnvHeader(XrdOucString &out)
{
  out = XRDMQSHAREDHASH_UPDATE; out += "&"; out += XRDMQSHAREDHASH_SUBJECT; out += "="; out += Subject.c_str();
  out += "&"; out += XRDMQSHAREDHASH_TYPE;    out += "="; out += Type.c_str();
}


/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::MakeDeletionEnvHeader(XrdOucString &out)
{
  out = XRDMQSHAREDHASH_DELETE; out += "&"; out += XRDMQSHAREDHASH_SUBJECT; out += "="; out += Subject.c_str();
  out += "&"; out += XRDMQSHAREDHASH_TYPE;    out += "="; out += Type.c_str();
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::MakeRemoveEnvHeader(XrdOucString &out)
{
  out = XRDMQSHAREDHASH_REMOVE; out += "&"; out += XRDMQSHAREDHASH_SUBJECT; out += "="; out += Subject.c_str();
  out += "&"; out += XRDMQSHAREDHASH_TYPE;    out += "="; out += Type.c_str();
}

/*----------------------------------------------------------------------------*/
bool
XrdMqSharedHash::BroadCastEnvString(const char* receiver)
{
  TransactionMutex.Lock();
  Transactions.clear();
  IsTransaction = true;

  std::map<std::string, XrdMqSharedHashEntry>::iterator it;

  StoreMutex.LockRead();
  for (it=Store.begin(); it!=Store.end(); it++) {
    Transactions.insert(it->first);
  }
  StoreMutex.UnLockRead();

  XrdOucString txmessage="";
  MakeBroadCastEnvHeader(txmessage);
  AddTransactionEnvString(txmessage);
  IsTransaction = false;
  TransactionMutex.UnLock();

  XrdMqMessage message("XrdMqSharedHashMessage");
  message.SetBody(txmessage.c_str());
  message.MarkAsMonitor();
  if (XrdMqSharedObjectManager::debug)fprintf(stderr,"XrdMqSharedObjectManager::BroadCastEnvString=>[%s]=>%s \n", Subject.c_str(),receiver);
  return XrdMqMessaging::gMessageClient.SendMessage(message,receiver);
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::AddTransactionEnvString(XrdOucString &out, bool clearafter)
{
  // encoding works as "mysh.pairs=|<key1>~<value1>%<changeid1>|<key2>~<value2>%<changeid2 ...."
  out += "&"; out += XRDMQSHAREDHASH_PAIRS; out += "=";
  std::set<std::string>::const_iterator transit;

  XrdMqRWMutexReadLock lock(StoreMutex);
  for (transit= Transactions.begin(); transit != Transactions.end(); transit++) {
    if ( (Store.count(transit->c_str() ))) {
      out += "|";
      out += transit->c_str();
      out += "~";
      out += Store[transit->c_str()].entry.c_str();
      out += "%";
      char cid[1024];snprintf(cid,sizeof(cid)-1,"%llu",Store[transit->c_str()].ChangeId);
      out += cid;
    }
  }
  if (clearafter)
    Transactions.clear();
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::AddDeletionEnvString(XrdOucString &out)
{
  // encoding works as "mysh.keys=|<key1>|<key2> ...."
  out += "&"; out += XRDMQSHAREDHASH_KEYS; out += "=";

  std::set<std::string>::const_iterator delit;

  for (delit = Deletions.begin(); delit != Deletions.end(); delit++) {
    out += "|";
    out += delit->c_str();
  }
  Deletions.clear();
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::Dump(XrdOucString &out) 
{
  std::map<std::string, XrdMqSharedHashEntry>::iterator it;
  char keyprint[64];
  StoreMutex.LockRead();
  for (it=Store.begin(); it!=Store.end(); it++) {
    snprintf(keyprint,sizeof(keyprint)-1,"key=%-24s", it->first.c_str());
    out += keyprint;
    out += " ";
    it->second.Dump(out);
    out += "\n";
  }
  StoreMutex.UnLockRead();
}

/*----------------------------------------------------------------------------*/

bool 
XrdMqSharedHash::BroadCastRequest(const char* requesttarget) {
  XrdOucString out;
  XrdMqMessage message("XrdMqSharedHashMessage");
  out += XRDMQSHAREDHASH_BCREQUEST; 
  out += "&"; out += XRDMQSHAREDHASH_SUBJECT; out += "="; out += Subject.c_str();
  out += "&"; out += XRDMQSHAREDHASH_REPLY;   out += "="; out += XrdMqMessaging::gMessageClient.GetClientId();
  out += "&"; out += XRDMQSHAREDHASH_TYPE;    out += "="; out += Type.c_str();
  message.SetBody(out.c_str());
  message.MarkAsMonitor();
  return XrdMqMessaging::gMessageClient.SendMessage(message, requesttarget);  
}

/*----------------------------------------------------------------------------*/
bool
XrdMqSharedHash::Set(const char* key, const char* value, bool broadcast, bool tempmodsubjects)
{
  SetCounter++;

  if (!value)
    return false;
  
  std::string skey = key;

  { 
    
    bool callback=false;

    {
      XrdMqRWMutexWriteLock lock(StoreMutex);
      
      if (!Store.count(skey)) {
        callback=true;
      }
      
      Store[skey].Set(value,key);
      if (callback) {
        CallBackInsert(&Store[skey], skey.c_str());
      }
    }
    
    
    if (broadcast) {
      if (SOM->IsMuxTransaction) {
        SOM->MuxTransactions[Subject].insert(skey);
      } else {
        // we emulate a transaction for a single Set
        if (!IsTransaction) {
          TransactionMutex.Lock(); 
          Transactions.clear();
        }
        
        Transactions.insert(skey);
      } 
    }
    
    // check if we have to do posts for this subject
    if (SOM) {
      SOM->SubjectsMutex.Lock();
      if (SOM->ModificationWatchKeys.size()) {
	if (SOM->ModificationWatchKeys.count(skey)) {
	  std::string fkey = Subject.c_str();
	  fkey += ";" ; fkey+= skey;
	  if (XrdMqSharedObjectManager::debug)fprintf(stderr,"XrdMqSharedObjectManager::Set=>[%s:%s]=>%s notified\n", Subject.c_str(),skey.c_str(),value);
	  if (tempmodsubjects) 
	    SOM->ModificationTempSubjects.push_back(fkey);
	  else {
	    SOM->ModificationSubjects.push_back(fkey);
	    SOM->SubjectsSem.Post();
	  }
	}
      }
      SOM->SubjectsMutex.UnLock();
    }
  }

  if (broadcast) {
    if (!SOM->IsMuxTransaction)
      if (!IsTransaction)
        CloseTransaction();
  }
  
  return true;
}


/*----------------------------------------------------------------------------*/
bool
XrdMqSharedHash::SetNoLockNoBroadCast(const char* key, const char* value, bool tempmodsubjects)
{
  // do outside XrdMqRWMutexWriteLock lock(StoreMutex);
  // do outside SOM->SubjectsMutex.Lock();

  SetNLCounter++;

  if (!value)
    return false;
  
  std::string skey = key;

  {
    bool callback=false;
    
    if (!Store.count(skey)) {
      callback=true;
    }
    
    Store[skey].Set(value,key);
    if (callback) {
      CallBackInsert(&Store[skey], skey.c_str());
    }
    
    // check if we have to do posts for this subject
    if (SOM) {
      if (SOM->ModificationWatchKeys.size()) {
	if (SOM->ModificationWatchKeys.count(skey)) {
	  std::string fkey = Subject.c_str();
	  fkey += ";" ; fkey+= skey;
	  if (XrdMqSharedObjectManager::debug)fprintf(stderr,"XrdMqSharedObjectManager::Set=>[%s:%s]=>%s notified\n", Subject.c_str(),skey.c_str(),value);
	  if (tempmodsubjects) 
	    SOM->ModificationTempSubjects.push_back(fkey);
	  else {
	    SOM->ModificationSubjects.push_back(fkey);
	    SOM->SubjectsSem.Post();
	  }
	}
      }
    }
  }
  return true;
}

/*----------------------------------------------------------------------------*/
bool 
XrdMqSharedHash::Delete(const char* key, bool broadcast)
{
  bool deleted = false;
  XrdMqRWMutexWriteLock lock(StoreMutex);
  if (Store.count(key)) {
    CallBackDelete(&Store[key]);
    Store.erase(key);
    deleted = true;
    if (IsTransaction && broadcast) {
      Deletions.insert(key);
      Transactions.erase(key);
    }
  }
  return deleted;
}

/*----------------------------------------------------------------------------*/
void
XrdMqSharedHash::Print(std::string &out, std::string format) 
{
  //-------------------------------------------------------------------------------
  // listformat
  //-------------------------------------------------------------------------------
  // format has to be provided as a chain (separated by "|" ) of the following tags
  // "key=<key>:width=<width>:format=[+][-][slfo]:unit=<unit>:tag=<tag>:condition=<key>=<val>" -> to print a key of the attached children
  // "sep=<seperator>"                                          -> to put a seperator
  // "header=1"                                                 -> to put a header with description on top! This must be the first format tag!
  // "indent=<n>"                                               -> indent the output
  // "headeronly=1"                                             -> only prints the header and nothnig else
  // the formats are:
  // 's' : print as string
  // 'l' : print as long long
  // 'f' : print as double
  // 'o' : print as <key>=<val>
  // '-' : left align the printout
  // '+' : convert numbers into k,M,G,T,P ranges
  // the unit is appended to every number:
  // e.g. 1500 with unit=B would end up as '1.5 kB'
  // the command only appends to <out> and DOES NOT initialize it
  // "tag=<tag>"                                                  -> use <tag> instead of the variable name to print the header

  std::vector<std::string> formattoken;
  bool buildheader=false;
  std::string indent="";

  std::string header="";
  std::string body = "";

  std::string conditionkey="";
  std::string conditionval="";
  bool headeronly=false;

  XrdMqStringConversion::Tokenize(format, formattoken, "|");

  for (unsigned int i=0; i< formattoken.size(); i++) {
    std::vector<std::string> tagtoken;
    std::map<std::string, std::string> formattags;

    XrdMqStringConversion::Tokenize(formattoken[i],tagtoken,":");
    for (unsigned int j=0; j< tagtoken.size(); j++) {
      std::vector<std::string> keyval;
      XrdMqStringConversion::Tokenize(tagtoken[j], keyval,"=");
      if (keyval.size()==3) {
        conditionkey=keyval[1];
        conditionval=keyval[2];
      } else {
        formattags[keyval[0]] = keyval[1];
      }
    }

    //---------------------------------------------------------------------------------------
    // "key=<key>:width=<width>:format=[slfo]:uniq=<unit>"
    
    bool alignleft=false;
    if ( (formattags["format"].find("-") != std::string::npos) ) {
      alignleft = true;
    }

    if (formattags.count("header") ) {
      // add the desired seperator
      if (formattags.count("header") == 1) {
        buildheader=true;
      }
    }

    if (formattags.count("headeronly") ) {
      headeronly=true;
    }

    if (formattags.count("indent") ) {
      for (int i=0; i< atoi(formattags["indent"].c_str()); i++) {
	indent+= " ";
      }
    }
    
    if (formattags.count("width") && formattags.count("format")) {
      unsigned int width = atoi(formattags["width"].c_str());
      // string
      char line[1024];
      char tmpline[1024];
      char lformat[1024];
      char lenformat[1024];
      line[0]=0;
      
      if ((formattags["format"].find("s"))!= std::string::npos) 
        snprintf(lformat,sizeof(lformat)-1, "%%s");
      
      if ((formattags["format"].find("l"))!= std::string::npos) {
        snprintf(lformat,sizeof(lformat)-1, "%%lld");
      }

      if ((formattags["format"].find("f"))!= std::string::npos)
        snprintf(lformat,sizeof(lformat)-1, "%%.02f");
      
      if (width==0) {
        if (alignleft) {
          snprintf(lenformat, sizeof(lenformat)-1, "%%-s");
        } else {
          snprintf(lenformat, sizeof(lenformat)-1, "%%s");
        }
      } else {
        if (alignleft) {
          snprintf(lenformat,sizeof(lenformat)-1, "%%-%ds",width);
        } else {
          snprintf(lenformat,sizeof(lenformat)-1, "%%%ds",width);
        }
      }
      
      // normal member printout
      if (formattags.count("key")) {
        if ((formattags["format"].find("s"))!= std::string::npos) 
          snprintf(tmpline,sizeof(tmpline)-1,lformat,Get(formattags["key"].c_str()).c_str());
        if ((formattags["format"].find("l"))!= std::string::npos) {
          if ( ((formattags["format"].find("+")) != std::string::npos) ) {
            std::string ssize;
            XrdMqStringConversion::GetReadableSizeString(ssize,(unsigned long long)GetLongLong(formattags["key"].c_str()), formattags["unit"].c_str());
            snprintf(tmpline,sizeof(tmpline)-1,"%s", ssize.c_str());
          } else {
            snprintf(tmpline,sizeof(tmpline)-1,lformat,GetLongLong(formattags["key"].c_str()));
          }
        }
        if ((formattags["format"].find("f"))!= std::string::npos) 
          snprintf(tmpline,sizeof(tmpline)-1,lformat,GetDouble(formattags["key"].c_str()));

        if (buildheader) {
          char headline[1024];
          char lenformat[1024];
          snprintf(lenformat, sizeof(lenformat)-1, "%%%ds", width-1);
          XrdOucString name=formattags["key"].c_str();
          name.replace("stat.","");
          name.replace("stat.statfs.","");
          if (formattags.count("tag")) {
            name = formattags["tag"].c_str();
          }

          snprintf(headline,sizeof(headline)-1, lenformat,name.c_str());
          std::string sline = headline;
          if (sline.length() > (width-1)) {
            sline.erase(0, ((sline.length()-width+1+3)>0)?(sline.length()-width+1+3):0);
            sline.insert(0,"...");
          }
          header += "#";
          header += sline;
        }

        snprintf(line,sizeof(line)-1,lenformat,tmpline);
      }
      body += indent;
      if ( (formattags["format"].find("o")!= std::string::npos) ) {
        char keyval[4096];
        buildheader = false; // auto disable header
        if (formattags.count("key")) {
          snprintf(keyval,sizeof(keyval)-1,"%s=%s", formattags["key"].c_str(), line);
        }
        body += keyval;
      }  else {
        std::string sline = line;
        if (width) {
          if (sline.length() > width) {
            sline.erase(0, ((sline.length()-width+3)>0)?(sline.length()-width+3):0);
            sline.insert(0,"...");
          }
        }
        body += sline;
      }
    }

    if (formattags.count("sep") ) {
      // add the desired seperator
      body += formattags["sep"];
      if (buildheader) {
        header += formattags["sep"];
      }
    }
  }

  body += "\n";

  bool accepted = true;

  // here we can match an EXACT condition or the beginning if '*' is 
  if (conditionkey!="") {
    XrdOucString condval = conditionval.c_str();
    XrdOucString condisval = "";
    if (condval.endswith("*")) {
      condval.erase(condval.length()-1);
      condisval = Get(conditionkey.c_str()).c_str();
      if (!(condisval.beginswith(condval)))
        accepted = false;
    } else {
      if (condval.beginswith("!")) {
        condisval = Get(conditionkey.c_str()).c_str();
        condval.erase(0,1);
        if (!condisval.length())
          accepted = false;
        if ((condisval == condval)) {
          accepted = false;
        }
      } else {
        if ( Get(conditionkey.c_str()) != conditionval) {
          accepted = false;
        }
      }
    }
  }


  if (buildheader) {
    std::string line ="";
    line += "#";
    for (unsigned int i=0; i< (header.length()-1); i++) {
      line += ".";
    }
    line += "\n";
    out += line;
    out += indent;
    out += header; out += "\n";
    out += indent;
    out += line;
    if (!headeronly && accepted) {
      out += body;
    }
  } else {
    if (accepted) {
      out += body;
    }
  }
}


/*----------------------------------------------------------------------------*/
void
XrdMqSharedQueue::CallBackInsert(XrdMqSharedHashEntry *entry, const char* key) 
{
  entry->SetKey(key);
  QueueMutex.Lock();
  Queue.push_back(entry);
  QueueMutex.UnLock();
  LastObjectId++;
  //  fprintf(stderr,"XrdMqSharedObjectManager::CallBackInsert=> on %s => LOID=%llu\n", key, LastObjectId);
}
 
/*----------------------------------------------------------------------------*/
void
XrdMqSharedQueue::CallBackDelete(XrdMqSharedHashEntry *entry)
{
  std::deque<XrdMqSharedHashEntry*>::iterator it;
  QueueMutex.Lock();
  for (it = Queue.begin(); it != Queue.end(); it++) {
    if (*it == entry) {
      Queue.erase(it);
      break;
    }
  }
  QueueMutex.UnLock();
  //fprintf(stderr,"XrdMqSharedObjectManager::CallBackDelete=> on %s \n", entry->GetKey());
}

/*----------------------------------------------------------------------------*/
