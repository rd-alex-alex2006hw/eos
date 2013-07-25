// ----------------------------------------------------------------------
// File: PropFindResponse.cc
// Author: Justin Lewis Salmon - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2013 CERN/Switzerland                                  *
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
#include "mgm/http/webdav/PropFindResponse.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/XrdMgmOfsDirectory.hh"
#include "common/Logging.hh"
#include "common/Timing.hh"
#include "common/Path.hh"
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
eos::common::HttpResponse*
PropFindResponse::BuildResponse(eos::common::HttpRequest *request)
{
  using namespace rapidxml;

  // Get the namespaces (if any)
  ParseNamespaces();

  // Root node <propfind/>
  xml_node<> *rootNode = mXMLRequestDocument.first_node();
  if (!rootNode)
  {
    SetResponseCode(ResponseCodes::BAD_REQUEST);
    return this;
  }

  // Get the requested property types
  ParseRequestPropertyTypes(rootNode);

  // Build the response
  // xml declaration
  xml_node<> *decl = mXMLResponseDocument.allocate_node(node_declaration);
  decl->append_attribute(AllocateAttribute("version", "1.0"));
  decl->append_attribute(AllocateAttribute("encoding", "utf-8"));
  mXMLResponseDocument.append_node(decl);

  // <multistatus/> node
  xml_node<> *multistatusNode = AllocateNode("d:multistatus");
  multistatusNode->append_attribute(AllocateAttribute("xmlns:d", "DAV:"));
  mXMLResponseDocument.append_node(multistatusNode);

  // Is the requested resource a file or directory?
  XrdOucErrInfo error;
  struct stat   statInfo;
  gOFS->_stat(request->GetUrl().c_str(), &statInfo, error, *mVirtualIdentity,
             (const char*) 0);

  // Figure out what we actually need to do
  std::string depth = request->GetHeaders()["Depth"];

  xml_node<> *responseNode = 0;
  if (depth == "0" || !S_ISDIR(statInfo.st_mode))
  {
    // Simply stat the file or directory
    responseNode = BuildResponseNode(request->GetUrl());
    if (responseNode)
    {
      multistatusNode->append_node(responseNode);
    }
    else
    {
      return this;
    }
  }

  else if (depth == "1")
  {
    // Stat the resource and all child resources
    XrdMgmOfsDirectory directory;
    int listrc = directory.open(request->GetUrl().c_str(), *mVirtualIdentity,
                                (const char*) 0);

    responseNode = BuildResponseNode(request->GetUrl());
    if (responseNode)
    {
      multistatusNode->append_node(responseNode);
    }

    if (!listrc)
    {
      const char *val;
      while ((val = directory.nextEntry()))
      {
        XrdOucString entryname = val;
        if (entryname.beginswith("."))
        {
          // skip over . .. and hidden files
          continue;
        }

        // one response node for each file...
        eos::common::Path path((request->GetUrl() + std::string(val)).c_str());
        responseNode = BuildResponseNode(path.GetPath());
        if (responseNode)
        {
          multistatusNode->append_node(responseNode);
        }
        else
        {
          return this;
        }
      }
    }
    else
    {
      eos_static_warning("msg=\"error opening directory\"");
      SetResponseCode(HttpResponse::BAD_REQUEST);
      return this;
    }
  }

  else if (depth == "1,noroot")
  {
    // Stat all child resources but not the requested resource
    SetResponseCode(HttpResponse::NOT_IMPLEMENTED);
    return this;
  }

  else if (depth == "infinity" || depth == "")
  {
    // Recursively stat the resource and all child resources
    SetResponseCode(HttpResponse::NOT_IMPLEMENTED);
    return this;
  }

  std::string responseString;
  rapidxml::print(std::back_inserter(responseString), mXMLResponseDocument);
  mXMLResponseDocument.clear();

  SetResponseCode(HttpResponse::MULTI_STATUS);
  AddHeader("Content-Length", std::to_string((long long) responseString.size()));
  AddHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  SetBody(responseString);

  return this;
}

/*----------------------------------------------------------------------------*/
void
PropFindResponse::ParseRequestPropertyTypes(rapidxml::xml_node<> *node)
{
  using namespace rapidxml;

  // <prop/> node (could be multiple, could be <allprop/>)
  xml_node<> *allpropNode = GetNode(node, "allprop");
  if (allpropNode)
  {
    mRequestPropertyTypes |= PropertyTypes::GET_CONTENT_LENGTH;
    mRequestPropertyTypes |= PropertyTypes::GET_CONTENT_TYPE;
    mRequestPropertyTypes |= PropertyTypes::GET_LAST_MODIFIED;
    mRequestPropertyTypes |= PropertyTypes::GET_ETAG;
    mRequestPropertyTypes |= PropertyTypes::CREATION_DATE;
    mRequestPropertyTypes |= PropertyTypes::DISPLAY_NAME;
    mRequestPropertyTypes |= PropertyTypes::RESOURCE_TYPE;
    mRequestPropertyTypes |= PropertyTypes::CHECKED_IN;
    mRequestPropertyTypes |= PropertyTypes::CHECKED_OUT;
    return;
  }

  // It wasn't <allprop/>
  xml_node<> *propNode = GetNode(node, "prop");
  if (!propNode) {
    eos_static_err("msg=\"no <prop/> node found in tree\"");
    return;
  }

  xml_node<> *property = propNode->first_node();

  // Find all the request properties
  while (property)
  {
    XrdOucString propertyName = property->name();
    eos_static_debug("msg=\"ound xml property: %s\"", propertyName.c_str());

    int colon = 0;
    if ((colon = propertyName.find(':')) != STR_NPOS)
    {
      // Split node name into <ns>:<nodename>
      // Ignore non DAV: namespaces for now
      for (auto it = mDAVNamespaces.begin(); it != mDAVNamespaces.end(); ++it)
      {
        std::string ns = it->first;
        if (propertyName.beginswith(ns.c_str()))
        {
          std::string prop(std::string(propertyName.c_str()), colon + 1);
          mRequestPropertyTypes |= MapRequestPropertyType(prop);
        }
      }
    }
    else
    {
      std::string prop(propertyName.c_str());
      mRequestPropertyTypes |= MapRequestPropertyType(prop);
    }

    property = property->next_sibling();
  }
}

/*----------------------------------------------------------------------------*/
rapidxml::xml_node<>*
PropFindResponse::BuildResponseNode (const std::string &url)
{
  using namespace rapidxml;

  XrdMgmOfsDirectory directory;
  XrdOucErrInfo      error;
  struct stat        statInfo;

  // Is the requested resource a file or directory?
  if (gOFS->_stat(url.c_str(), &statInfo, error, *mVirtualIdentity,
                 (const char*) 0))
  {
    eos_static_err("msg=\"error stating %s: %s\"", url.c_str(),
                                                   error.getErrText());
    SetResponseCode(ResponseCodes::NOT_FOUND);
    return NULL;
  }

  // <response/> node
  xml_node<> *responseNode = AllocateNode("d:response");

  // <href/> node
  xml_node<> *href = AllocateNode("d:href");
  SetValue(href, url.c_str());
  responseNode->append_node(href);

  // <propstat/> node for "found" properties
  xml_node<> *propstatFound = AllocateNode("d:propstat");
  responseNode->append_node(propstatFound);

  // <status/> "found" node
  xml_node<> *statusFound = AllocateNode("d:status");
  SetValue(statusFound, "HTTP/1.1 200 OK");
  propstatFound->append_node(statusFound);

  // <prop/> "found" node
  xml_node<> *propFound = AllocateNode("d:prop");
  propstatFound->append_node(propFound);

  // <propstat/> node for "not found" properties
  xml_node<> *propstatNotFound = AllocateNode("d:propstat");
  responseNode->append_node(propstatNotFound);

  // <status/> "not found" node
  xml_node<> *statusNotFound = AllocateNode("d:status");
  SetValue(statusNotFound, "HTTP/1.1 404 Not Found");
  propstatNotFound->append_node(statusNotFound);

  // <prop/> "not found" node
  xml_node<> *propNotFound = AllocateNode("d:prop");
  propstatNotFound->append_node(propNotFound);

  xml_node<> *contentLength = 0;
  xml_node<> *lastModified  = 0;
  xml_node<> *resourceType  = 0;
  xml_node<> *checkedIn     = 0;
  xml_node<> *checkedOut    = 0;
  xml_node<> *creationDate  = 0;
  xml_node<> *eTag          = 0;
  xml_node<> *displayName   = 0;
  xml_node<> *contentType   = 0;

  if (mRequestPropertyTypes & PropertyTypes::GET_CONTENT_LENGTH)
    contentLength = AllocateNode("d:getcontentlength");
  if (mRequestPropertyTypes & PropertyTypes::GET_CONTENT_TYPE)
    contentType   = AllocateNode("d:getcontenttype");
  if (mRequestPropertyTypes & PropertyTypes::GET_LAST_MODIFIED)
    lastModified  = AllocateNode("d:getlastmodified");
  if (mRequestPropertyTypes & PropertyTypes::CREATION_DATE)
    creationDate  = AllocateNode("d:creationdate");
  if (mRequestPropertyTypes & PropertyTypes::RESOURCE_TYPE)
    resourceType  = AllocateNode("d:resourcetype");
  if (mRequestPropertyTypes & PropertyTypes::DISPLAY_NAME)
    displayName   = AllocateNode("d:displayname");
  if (mRequestPropertyTypes & PropertyTypes::GET_ETAG)
    eTag          = AllocateNode("d:etag");
  if (mRequestPropertyTypes & PropertyTypes::CHECKED_IN)
    checkedIn     = AllocateNode("d:checked-in");
  if (mRequestPropertyTypes & PropertyTypes::CHECKED_OUT)
    checkedOut    = AllocateNode("d:checked-out");

  // getlastmodified, creationdate, displayname and getetag properties are
  // common to all resources
  if (lastModified)
  {
    std::string lm = eos::common::Timing::UnixTimstamp_to_ISO8601(
                                          statInfo.st_mtim.tv_sec);
    SetValue(lastModified, lm.c_str());
    propFound->append_node(lastModified);
  }

  if (creationDate)
  {
    std::string cd = eos::common::Timing::UnixTimstamp_to_ISO8601(
                                          statInfo.st_ctim.tv_sec);
    SetValue(creationDate, cd.c_str());
    propFound->append_node(creationDate);
  }

  if (eTag)
  {
    std::string etag;
    SetValue(eTag, eos::common::StringConversion::GetSizeString(etag,
                                (unsigned long long) statInfo.st_ino));
    propFound->append_node(eTag);
  }

  if (displayName)
  {
    eos::common::Path path(url.c_str());
    eos_static_debug("msg=\"display name: %s\"", path.GetName());
    SetValue(displayName, path.GetName());
    propFound->append_node(displayName);
  }

  // Directory
  if (S_ISDIR(statInfo.st_mode))
  {
    if (resourceType)
    {
      xml_node<> *container = AllocateNode("d:collection");
      resourceType->append_node(container);
      propFound->append_node(resourceType);
    }
    if (contentLength) propNotFound->append_node(contentLength);
    if (contentType)
    {
      SetValue(contentType, "httpd/unix-directory");
      propFound->append_node(contentType);
    }
  }

  // File
  else
  {
    if (resourceType) propNotFound->append_node(resourceType);
    if (contentLength)
    {
      SetValue(contentLength, std::to_string((long long) statInfo.st_size).c_str());
      propFound->append_node(contentLength);
    }
    if (contentType)
    {
      SetValue(contentType, HttpResponse::ContentType(url).c_str());
      propFound->append_node(contentType);
    }
  }

  // We don't use these (yet)
  if (checkedIn)  propNotFound->append_node(checkedIn);
  if (checkedOut) propNotFound->append_node(checkedOut);

  return responseNode;
}

/*----------------------------------------------------------------------------*/
EOSMGMNAMESPACE_END
