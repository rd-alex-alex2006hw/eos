//------------------------------------------------------------------------------
//! @file ICmdHelper.cc
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

#include "console/commands/ICmdHelper.hh"
#include "common/Logging.hh"
#include "common/SymKeys.hh"
#include <sstream>

//------------------------------------------------------------------------------
// Execute command and display any output information
//------------------------------------------------------------------------------
int
ICmdHelper::Execute(bool printError)
{
  int retc = this->ExecuteWithoutPrint();

  if (retc) {
    if (printError && mMgmExec.GetError().length()) {
      std::cerr << mMgmExec.GetError() << std::endl;
    }
  } else {
    if (!mIsSilent && !mMgmExec.GetResult().empty()) {
      if (mHighlight) {
        TextHighlight(mMgmExec.GetResult());
      }

      std::cout << this->GetResult();
    }
  }

  return retc;
}

int
ICmdHelper::ExecuteWithoutPrint()
{
  if (!mReq.command_case()) {
    std::cerr << "error: generic request object not populated with command"
              << std::endl;
    return EINVAL;
  }

  std::string b64buff;

  if (!eos::common::SymKey::ProtobufBase64Encode(&mReq, b64buff)) {
    std::cerr << "error: failed to base64 encode the request" << std::endl;
    return EINVAL;
  }

  std::string cmd = "mgm.cmd.proto=";
  cmd += b64buff;

  if (getenv("EOS_ROUTE")) {
    XrdOucString route = getenv("EOS_ROUTE");

    while (route.replace("&", "#AND#")) {}

    cmd += "&eos.route=";
    cmd += route.c_str();
    unsetenv("EOS_ROUTE");
  }

  return mMgmExec.ExecuteCommand(cmd.c_str(), mIsAdmin);
}

//------------------------------------------------------------------------------
// Apply highlighting to text
//------------------------------------------------------------------------------
void
ICmdHelper::TextHighlight(std::string& text)
{
  if (global_highlighting) {
    XrdOucString tmp = text.c_str();
    // Color replacements
    tmp.replace("[booted]", "\033[1m[booted]\033[0m");
    tmp.replace("[down]", "\033[49;31m[down]\033[0m");
    tmp.replace("[failed]", "\033[49;31m[failed]\033[0m");
    tmp.replace("[booting]", "\033[49;32m[booting]\033[0m");
    tmp.replace("[compacting]", "\033[49;34m[compacting]\033[0m");
    // Replication highlighting
    tmp.replace("master-rw", "\033[49;31mmaster-rw\033[0m");
    tmp.replace("master-ro", "\033[49;34mmaster-ro\033[0m");
    tmp.replace("slave-ro", "\033[1mslave-ro\033[0m");
    tmp.replace("=ok", "=\033[49;32mok\033[0m");
    tmp.replace("=compacting", "=\033[49;32mcompacting\033[0m");
    tmp.replace("=off", "=\033[49;34moff\033[0m");
    tmp.replace("=blocked", "=\033[49;34mblocked\033[0m");
    tmp.replace("=wait", "=\033[49;34mwait\033[0m");
    tmp.replace("=starting", "=\033[49;34mstarting\033[0m");
    tmp.replace("=true", "=\033[49;32mtrue\033[0m");
    tmp.replace("=false", "=\033[49;31mfalse\033[0m");
    text = tmp.c_str();
  }
}

bool
ICmdHelper::ConfirmOperation()
{
  std::ostringstream out;
  std::string confirmation;
  srand(time(NULL));

  for (int i = 0; i < 10; i++) {
    confirmation += std::to_string((int)(9.0 * rand() / RAND_MAX));
  }

  out << "Confirm operation by typing => " << confirmation << std::endl;
  out << "                            => ";
  std::string userInput;
  std::cout << out.str();
  getline(std::cin, userInput);

  if (userInput == confirmation) {
    std::cout << std::endl << "Operation confirmed" << std::endl;
    return true;
  } else {
    std::cout << std::endl << "Operation not confirmed" << std::endl;
    return false;
  }
}

bool
ICmdHelper::NeedsConfirmation()
{
  return mNeedsConfirmation;
}

std::string
ICmdHelper::GetResult()
{
  // Add new line if necessary
  std::string out = mMgmExec.GetResult();

  if (*out.rbegin() != '\n') {
    out += '\n';
  }

  return out;
}

std::string
ICmdHelper::GetError()
{
  return mMgmExec.GetError();
}

bool
ICmdHelper::next_token(eos::common::StringTokenizer& tokenizer,
                       XrdOucString& token)
{
  if (!(token = tokenizer.GetToken()).length()) {
    return false;
  } else {
    return true;
  }
}

