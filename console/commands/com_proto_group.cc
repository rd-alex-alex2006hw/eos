//------------------------------------------------------------------------------
// File: com_proto_group.cc
// Author: Fabio Luchetti - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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


#include "common/StringTokenizer.hh"
#include "common/Path.hh"
#include "console/ConsoleMain.hh"
#include "console/commands/ICmdHelper.hh"

extern int com_group(char *);

void com_group_help();

//------------------------------------------------------------------------------
//! Class GroupHelper
//------------------------------------------------------------------------------
class GroupHelper : public ICmdHelper {
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  GroupHelper() {
    mIsSilent = false;
    mHighlight = true;
  }

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~GroupHelper() = default;

  //----------------------------------------------------------------------------
  //! Parse command line input
  //!
  //! @param arg input
  //!
  //! @return true if successful, otherwise false
  //----------------------------------------------------------------------------
  bool ParseCommand(const char *arg) override;
};


bool GroupHelper::ParseCommand(const char *arg) {

  eos::console::GroupProto* group = mReq.mutable_group();

  //
  XrdOucEnv *result = 0; //
  bool ok = false; //
  bool sel = false; //
  //

  std::string subcommand;
  std::string option;

  eos::common::StringTokenizer tokenizer(arg);
  tokenizer.GetLine();


  if (!(subcommand = tokenizer.GetToken())) return false; //if ( !(subcommand=tokenizer.GetToken(false).length()>0) )


  /* one of { ls, rm, set } */
  if (subcommand == "ls") {
    
    eos::console::GroupProto_LsProto* ls = group->mutable_ls();

    if (!(option = tokenizer.GetToken())) {
      return true; // just "group ls" // #TOCK anything to do? 
    } else {

      do {

        if (option == "-s") {
          mIsSilent = true; //ls->set_silent(true);

        } else if (option == "-g") {

          std::string geodepth = subtokenizer.GetToken();

          if (!geodepth.length()) {
            fprintf(stderr, "Error: geodepth is not provided\n");
            return false;
          }
          if (!geodepth.isdigit() || geodepth.atoi() < 0) {
            fprintf(stderr, "Error: geodepth should be a positive integer\n");
            return false; //??? was return 0; 
          }
          ls->set_outdepth(geodepth.atoi());

        } else if (option == "-b" || option == "--brief") {
          ls->set_outhost(true);

        } else if (option == "-m" || option == "-l" || option == "--io" || option == "--IO") {
          option.erase(std::remove(option.begin(), option.end(), '-'), option.end());
          ls->set_outformat(option); 

        } else if (!option.beginswith("-")) {
          ls->set_selection(option);
          //#TOCK
          // if (!sel) {
          //   ok = true;
          // }
          // sel = true;
        }

      } while (option = tokenizer.GetToken());

      return true;
    }

  } else if (subcommand == "rm") {

    if (!(option = tokenizer.GetToken())) {
      return false;
    } else {
      eos::console::GroupProto_RmProto* rm = group->mutable_rm();
      rm->set_group(option);
    }
    return true;

  } else if (subcommand == "set") {

    if (!(option = tokenizer.GetToken())) {
      return false;
    } else {
      eos::console::GroupProto_SetProto* set = group->mutable_set();
      set->set_group(option);
      if (!(option = tokenizer.GetToken())) {
        return false;
      } else {
        if (option == "on") {
          set->set_group_state(true);
        } else if (option == "off") {
          set->set_group_state(false);
        } else {
          return false;
        }
      }
    }
    return true;

  } else { // no proper subcommand
    return false;
  }

}


//------------------------------------------------------------------------------
// Group command entry point
//------------------------------------------------------------------------------
int com_protogroup(char *arg) {
  if (wants_help(arg)) {
    com_group_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  GroupHelper group;

  if (!group.ParseCommand(arg)) {
    com_group_help();
    global_retc = EINVAL;
    return EINVAL;
  }

  global_retc = group.Execute();
  return global_retc;
}

void com_group_help() {
  std::ostringstream oss;
  oss
      << "usage: group ls [-s] [-g] [-b|--brief] [-m|-l|--io] [<groups>] : list groups" << std::endl
      << "\t<groups> : list <groups> only, where <groups> is a substring match and can be a comma seperated list"
      << std::endl
      << "\t  -s : silent mode" << std::endl
      << "\t  -g : geo output - aggregate group information along the instance geotree down to <depth>" << std::endl
      << "\t  -b : @@@" << std::endl
      << "\t  -m : monitoring key=value output format" << std::endl
      << "\t  -l : long output - list also file systems after each group" << std::endl
      << "\t--io : print IO statistics for the group" << std::endl
      << "\t--IO : print IO statistics for each filesystem" << std::endl
      << std::endl
      << "usage: group rm <group-name> : remove group" << std::endl
      << std::endl
      << "usage: group set <group-name> on|off : activate/deactivate group" << std::endl
      << "\t=> when a group is (re-)enabled, the drain pull flag is recomputed for all filesystems within a group"
      << std::endl
      << "\t=> when a group is (re-)disabled, the drain pull flag is removed from all members in the group"
      << std::endl;
  std::cerr << oss.str() << std::endl;

}
