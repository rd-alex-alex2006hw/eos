//------------------------------------------------------------------------------
//! @file IoCmd.hh
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
#include "mgm/Namespace.hh"
#include "proto/Io.pb.h"
#include "mgm/proc/ProcCommand.hh"
#include "namespace/interface/IContainerMD.hh"
#include <list>

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class IoCmd - class handling io commands
//------------------------------------------------------------------------------
class IoCmd: public IProcCommand
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param req client ProtocolBuffer request
  //! @param vid client virtual identity
  //----------------------------------------------------------------------------
  explicit IoCmd(eos::console::RequestProto&& req,
                 eos::common::Mapping::VirtualIdentity& vid):
    IProcCommand(std::move(req), vid, false)
  {}

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~IoCmd() = default;

  //----------------------------------------------------------------------------
  //! Method implementing the specific behaviour of the command executed by the
  //! asynchronous thread
  //----------------------------------------------------------------------------
  eos::console::ReplyProto ProcessRequest() override;

private:


  //----------------------------------------------------------------------------
  //! Execute ls command
  //!
  //! @param ls ls subcommand proto object
  //!
  //! @return string representing ls output
  //----------------------------------------------------------------------------
  std::string LsSubcmd(const eos::console::GroupProto_LsProto& ls);

  //----------------------------------------------------------------------------
  //! Execute ls command
  //!
  //! @param ls ls subcommand proto object
  //! @param reply reply proto object
  //----------------------------------------------------------------------------

  void LsSubcmd(const eos::console::GroupProto_LsProto& ls,
                eos::console::ReplyProto& reply);


};



EOSMGMNAMESPACE_END
