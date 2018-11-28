# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Elvin-Alin Sindrilaru <esindril@cern.ch>
# ----------------------------------------------------------------------

# ************************************************************************
# * EOS - the CERN Disk Storage System                                   *
# * Copyright (C) 2011 CERN/Switzerland                                  *
# *                                                                      *
# * This program is free software: you can redistribute it and/or modify *
# * it under the terms of the GNU General Public License as published by *
# * the Free Software Foundation, either version 3 of the License, or    *
# * (at your option) any later version.                                  *
# *                                                                      *
# * This program is distributed in the hope that it will be useful,      *
# * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
# * GNU General Public License for more details.                         *
# *                                                                      *
# * You should have received a copy of the GNU General Public License    *
# * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
# ************************************************************************

#-------------------------------------------------------------------------------
# Search for dependencies
#-------------------------------------------------------------------------------
option(PACKAGEONLY "Build without dependencies" OFF)
option(FUSEXCLIENT "Build FUSEX client" OFF)
option(CLIENT "Build only client packages" OFF)
option(BUILD_XRDCL_RAIN_PLUGIN "Enable XrdCl RAIN plugin" OFF)
set(EOS_RPATH "/usr/lib64/;/lib64/")

if(NOT PACKAGEONLY)
  if(Linux)
    find_package(glibc REQUIRED)
    find_package(attr REQUIRED)
    find_package(xfs REQUIRED)
    find_package(libbfd REQUIRED)
    find_package(richacl)
    find_package(help2man)
  endif()
  find_package(PythonSitePkg REQUIRED)
  find_package(XRootD REQUIRED)
  find_package(fuse REQUIRED)
  find_package(fuse3)
  find_package(Threads REQUIRED)
  find_package(z REQUIRED)
  find_package(readline REQUIRED)
  find_package(CURL REQUIRED)
  find_package(uuid REQUIRED)
  find_package(openssl REQUIRED)
  find_package(ncurses REQUIRED)
  find_package(leveldb REQUIRED)
  find_package(ZMQ REQUIRED)
  find_package(krb5 REQUIRED)
  find_package(Sphinx)
  find_package(SparseHash REQUIRED)
  find_package(libevent REQUIRED)
  find_package(jsoncpp REQUIRED)
  find_package(hiredis REQUIRED)
  find_package(bz2 REQUIRED)

  if (Linux)
    find_package(Protobuf3 REQUIRED)
    # Protobuf3 needs to be added to the RPATH of the libraries and binaries
    # built since it's not installed in the normal system location
    set(CMAKE_SKIP_RPATH FALSE)
    set(CMAKE_SKIP_BUILD_RPATH FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    get_filename_component(EOS_PROTOBUF_RPATH ${PROTOBUF_LIBRARY} DIRECTORY)
    set(CMAKE_INSTALL_RPATH "${EOS_PROTOBUF_RPATH}")
  else ()
    find_package(Protobuf REQUIRED)
  endif()

  # The server build also requires
  if (NOT CLIENT)
    find_package(eosfolly REQUIRED)
    find_package(davix)
    find_package(ldap REQUIRED)
    # @TODO (esindril): Completely drop cppunit once everything is moved to gtest
    find_package(CPPUnit REQUIRED)
    find_package(microhttpd)
    if (NOT MICROHTTPD_FOUND)
      message ("Notice: MicroHttpd not found, no httpd access available")
    else ()
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DEOS_MICRO_HTTPD=1")
    endif ()
  endif()
else()
  message(STATUS "Running CMake in package only mode.")
  # Fake function for building the SRPMS in build system
  function(PROTOBUF_GENERATE_CPP SRCS HDRS)
    # This is just a hack to be able to run cmake >= 3.11 with -DPACKAGEONLY
    # enabled. Otherwise the protobuf libraries built using add_library will
    # complain as they have no SOURCE files.
    set(${SRCS} "${CMAKE_SOURCE_DIR}/common/Logging.cc" PARENT_SCOPE)
    set(${HDRS} "${CMAKE_SOURCE_DIR}/common/Logging.hh" PARENT_SCOPE)
    return()
  endfunction()
endif()
