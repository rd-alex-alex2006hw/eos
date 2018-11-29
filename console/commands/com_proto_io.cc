//------------------------------------------------------------------------------
// File: com_proto_io.cc
// Author: Fabio Luchetti - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your token) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/


#include "common/StringTokenizer.hh"
#include "common/Path.hh"
#include "console/ConsoleMain.hh"
#include "console/commands/ICmdHelper.hh"

// extern int com_io(char *);

void com_io_help();

//------------------------------------------------------------------------------
//! Class IoHelper
//------------------------------------------------------------------------------
class IoHelper : public ICmdHelper
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  IoHelper()
  {
    mIsSilent = false;
    mHighlight = true;
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~IoHelper() = default;

  //----------------------------------------------------------------------------
  //! Parse command line input
  //!
  //! @param arg input
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool ParseCommand(const char* arg) override;
};


bool IoHelper::ParseCommand(const char* arg)
{
  eos::console::IoProto* io = mReq.mutable_io();
  eos::common::StringTokenizer tokenizer(arg);
  tokenizer.GetLine();
  XrdOucString token;

  if (!next_token(tokenizer, token)) {
    return false;
  }

  /* one of { stat, ns, report, enable, disable } */
  if (token == "stat") {
    eos::console::IoProto_StatProto* stat = io->mutable_stat();

    while (next_token(tokenizer, token)) {
      if (token == "-a") {
        stat.set_details(true);
      } else if (token == "-m") {
        stat.set_monitoring(true);
      } else if (token == "-n") {
        stat.set_numerical(true);
      } else if (token == "-t") {
        stat.set_top(true);
      } else if (token == "-d") {
        stat.set_domain(true);
      } else if (token == "-x") {
        stat.set_apps(true);
      } else if (token == "-l") {
        stat.set_summary(true);
      } else {
        return false;
      }
    }
  } else if (token == "ns") {
    eos::console::IoProto_NsProto* ns = io->mutable_ns();

    while (next_token(tokenizer, token)) {
      if (token == "-m") {
        ns.set_details(true);
      } else if (token == "-b") {
        ns.set_rank_by_byte(true);
      } else if (token == "-n") {
        ns.set_rank_by_access(true);
      } else if (token == "-w") {
        ns.set_last_week(true);
      } else if (token == "-f") {
        ns.set_hotfiles(true);
      } else if (token == "-a") { // #TOCK can it be included in Count?
        ns.set_all(true);
      } else if (token == "-100") { // #TODO group mutually exclusive tokens
        ns.set_count(eos::console::IoProto_NsProto::ONEHUNDRED);
      } else if (token == "-1000") {
        ns.set_count(eos::console::IoProto_NsProto::ONETHOUSAND);
      } else if (token == "-10000") {
        ns.set_count(eos::console::IoProto_NsProto::TENTHOUSAND);
      } else {
        return false;
      }
    }
  } else if (token == "report") {
    if (!next_token(tokenizer, token)) {
      return false;
    }

    eos::console::IoProto_ReportProto* report = io->mutable_report();
    report->set_path(token.c_str());
  } else if (token == "enable") {
    eos::console::IoProto_EnableProto* enable = io->mutable_enable();

    while (next_token(tokenizer, token)) {
      if (token == "-r") {
        enable.set_reports(true);
      } else if (token == "-p") {
        enable.set_popularity(true);
      } else if (token == "-n") {
        enable.set_namespace(true);
      } else if (token == "--udp") {
        if (!(next_token(tokenizer, token)) || (token.beginswith("-"))) {
          return false;
        } else {
          enable->set_upd_address(token.c_str());
        }
      } else {
        return false;
      }
    }
  } else if (token == "disable") { // #TODO merge with enable
    eos::console::IoProto_DisableProto* disable = io->mutable_enable();

    while (next_token(tokenizer, token)) {
      if (token == "-r") {
        disable.set_reports(true);
      } else if (token == "-p") {
        disable.set_popularity(true);
      } else if (token == "-n") {
        disable.set_namespace(true);
      } else if (token == "--udp") {
        if (!next_token(tokenizer, token) || (token.beginswith("-"))) {
          return false;
        } else {
          disable->set_upd_address(token.c_str());
        }
      } else {
        return false;
      }
    }
  } else { // no proper subcommand
    return false;
  }

  return true;
}


//------------------------------------------------------------------------------
// io command entry point
//------------------------------------------------------------------------------
int com_protogroup(char* arg)
{
  if (wants_help(arg)) {
    com_io_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  IoHelper io;

  if (!io.ParseCommand(arg)) {
    com_group_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  global_retc = io.Execute();
  return global_retc;
}

void com_group_help()
{
  fprintf(stdout,
          "usage: io stat [-l] [-a] [-m] [-n] [-t] [-d] [-x]               :  print io statistics\n");
  fprintf(stdout,
          "                -l                                                   -  show summary information (this is the default if -t,-d,-x is not selected)\n");
  fprintf(stdout,
          "                -a                                                   -  break down by uid/gid\n");
  fprintf(stdout,
          "                -m                                                   -  print in <key>=<val> monitoring format\n");
  fprintf(stdout,
          "                -n                                                   -  print numerical uid/gids\n");
  fprintf(stdout,
          "                -t                                                   -  print top user stats\n");
  fprintf(stdout,
          "                -d                                                   -  break down by domains\n");
  fprintf(stdout,
          "                -x                                                   -  break down by application\n");
  fprintf(stdout,
          "       io enable [-r] [-p] [-n] [--udp <address>]                 :  enable collection of io statistics\n");
  fprintf(stdout,
          "                                                               -r    enable collection of io reports\n");
  fprintf(stdout,
          "                                                               -p    enable popularity accounting\n");
  fprintf(stdout,
          "                                                               -n    enable report namespace\n");
  fprintf(stdout,
          "                                                               --udp <address> add a UDP message target for io UDP packtes (the configured targets are shown by 'io stat -l'\n");
  fprintf(stdout,
          "       io disable [-r] [-p] [-n]                                       :  disable collection of io statistics\n");
  fprintf(stdout,
          "                                                               -r    disable collection of io reports\n");
  fprintf(stdout,
          "                                                               -p    disable popularity accounting\n");
  fprintf(stdout,
          "                                                               --udp <address> remove a UDP message target for io UDP packtes\n");
  fprintf(stdout,
          "                                                               -n    disable report namespace\n");
  fprintf(stdout,
          "       io report <path>                                           :  show contents of report namespace for <path>\n");
  fprintf(stdout,
          "       io ns [-a] [-n] [-b] [-100|-1000|-10000] [-w] [-f]         :  show namespace IO ranking (popularity)\n");
  fprintf(stdout,
          "                                                               -a    don't limit the output list\n");
  fprintf(stdout,
          "                                                               -n :  show ranking by number of accesses \n");
  fprintf(stdout,
          "                                                               -b :  show ranking by number of bytes\n");
  fprintf(stdout,
          "                                                             -100 :  show the first 100 in the ranking\n");
  fprintf(stdout,
          "                                                            -1000 :  show the first 1000 in the ranking\n");
  fprintf(stdout,
          "                                                           -10000 :  show the first 10000 in the ranking\n");
  fprintf(stdout,
          "                                                               -w :  show history for the last 7 days\n");
  fprintf(stdout,
          "                                                               -f :  show the 'hotfiles' which are the files with highest number of present file opens\n");
  global_retc = EINVAL;
  return;
}
