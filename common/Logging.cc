/*----------------------------------------------------------------------------*/
#include "common/Namespace.hh"
#include "common/Logging.hh"
/*----------------------------------------------------------------------------*/
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_BEGIN

int Logging::gLogMask=0;
int Logging::gPriorityLevel=0;

Logging::LogArray         Logging::gLogMemory;
Logging::LogCircularIndex Logging::gLogCircularIndex;
unsigned long                      Logging::gCircularIndexSize;
XrdSysMutex Logging::gMutex;
XrdOucString Logging::gUnit="none";
XrdOucString Logging::gFilter="";

Mapping::VirtualIdentity Logging::gZeroVid;


/*----------------------------------------------------------------------------*/
void
Logging::log(const char* func, const char* file, int line, const char* logid, const Mapping::VirtualIdentity &vid, const char* cident, int priority, const char *msg, ...) 
{
  if (!((LOG_MASK(priority) & gLogMask)))
    return;

  // apply filter to avoid message flooding for debug messages
  if (priority >= LOG_INFO)
    if ( (gFilter.find(func))!=STR_NPOS) {
      return;
    }

  static char* buffer=0;
  if (!buffer) {
    // 1 M print buffer
    buffer = (char*) malloc(1024*1024);
  }
    
  XrdOucString File = file;

  if (File.length() > 16) {
    int up = File.length() - 13;
    File.erase(3, up);
    File.insert("...",3);
  }


  static time_t current_time;
  static struct timeval tv;
  static struct timezone tz;
  static struct tm *tm;
  gMutex.Lock();

  va_list args;
  va_start (args, msg);


  time (&current_time);
  gettimeofday(&tv, &tz);

  static char linen[16];
  sprintf(linen,"%d",line);

  static char fcident[1024];
  
  XrdOucString truncname = vid.name;
  if (truncname.length() > 16) {
    truncname.insert("..",0);
    truncname.erase(0,truncname.length()-16);
  }
    
  sprintf(fcident,"%s %d/%d [%16s]",cident,vid.uid,vid.gid,truncname.c_str());

  tm = localtime (&current_time);
  sprintf (buffer, "%02d%02d%02d %02d:%02d:%02d %lu.%06lu %s| %-36s %-24s %014lu %16s:%-4s \t.....| %-50s %-10s | ", tm->tm_year-100, tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, current_time, (unsigned long)tv.tv_usec, GetPriorityString(priority),logid, gUnit.c_str(), (unsigned long)XrdSysThread::ID(), File.c_str(), linen, fcident, func);

  char*  ptr = buffer + strlen(buffer);
  vsprintf(ptr, msg, args);
  
  fprintf(stderr,"%s",buffer);
  fprintf(stderr,"\n");
  fflush(stderr);
  va_end(args);
  gLogMemory[priority][(gLogCircularIndex[priority]++)%gCircularIndexSize] = buffer;
  gMutex.UnLock();
}

/*----------------------------------------------------------------------------*/
void
Logging::Init() 
{
  // initialize the log array and sets the log circular size
  gLogCircularIndex.resize(LOG_DEBUG+1);
  gLogMemory.resize(LOG_DEBUG+1);
  gCircularIndexSize=EOSCOMMONLOGGING_CIRCULARINDEXSIZE;
  for (int i = 0; i<= LOG_DEBUG; i++ ) {
    gLogCircularIndex[i] = 0;
    gLogMemory[i].resize(gCircularIndexSize);
  }
}

EOSCOMMONNAMESPACE_END

