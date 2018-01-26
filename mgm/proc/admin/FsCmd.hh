//------------------------------------------------------------------------------
// File: FsCmd.hh
// Author: Jozsef Makai - CERN
//------------------------------------------------------------------------------

/************************************************************************
* EOS - the CERN Disk Storage System                                   *
* Copyright (C) 2017 CERN/Switzerland                                  *
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

#pragma once
#include "mgm/proc/IProcCommand.hh"
#include "mgm/Namespace.hh"
#include "common/ConsoleRequest.pb.h"

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class FsCmd
//------------------------------------------------------------------------------
class FsCmd : public IProcCommand
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param req client ProtocolBuffer request
  //! @param vid client virtual identity
  //----------------------------------------------------------------------------
  FsCmd(eos::console::RequestProto&& req,
        eos::common::Mapping::VirtualIdentity& vid):
    IProcCommand(std::move(req), vid, true) {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~FsCmd() override = default;

  //----------------------------------------------------------------------------
  //! Method implementing the specific behaviour of the command executed by the
  //! asynchronous thread
  //----------------------------------------------------------------------------
  eos::console::ReplyProto ProcessRequest() override;

private:
  //! Methods implementing individual subcommands
  std::string List(const eos::console::FsProto::LsProto& lsProto);

  int Config(const eos::console::FsProto::ConfigProto& configProto);

  int Mv(const eos::console::FsProto::MvProto& mvProto);

  int Rm(const eos::console::FsProto::RmProto& rmProto);

  int DropDeletion(const eos::console::FsProto::DropDeletionProto& dropdelProto);

  int Add(const eos::console::FsProto::AddProto& addProto);

  int Boot(const eos::console::FsProto::BootProto& bootProto);

  int DumpMd(const eos::console::FsProto::DumpMdProto& dumpmdProto);

  int Status(const eos::console::FsProto::StatusProto& statusProto);

  std::string DisplayModeToString(eos::console::FsProto::LsProto::DisplayMode
                                  mode);

  static XrdSysSemaphore mSemaphore;
  std::string mOut; ///< Command output string
  std::string mErr; ///< Command error output string
};

EOSMGMNAMESPACE_END
