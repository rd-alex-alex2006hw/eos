# ----------------------------------------------------------------------
# File: CMakeLists.txt
# Author: Andreas-Joachim Peters - CERN
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

include_directories(
  ${CMAKE_SOURCE_DIR}
  ${NCURSES_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIRS}
  ${XROOTD_INCLUDE_DIRS}
  ${FOLLY_INCLUDE_DIRS}
  ${SPARSEHASH_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/namespace/ns_quarkdb/qclient/include
  ${CMAKE_SOURCE_DIR}/namespace/ns_quarkdb/qclient/src)

#-------------------------------------------------------------------------------
# XrdMqClient-Objects
#-------------------------------------------------------------------------------
add_library(XrdMqClient-Objects OBJECT
  XrdMqTiming.hh
  XrdMqClient.cc        XrdMqClient.hh
  XrdMqMessage.cc       XrdMqMessage.hh
  XrdMqMessaging.cc     XrdMqMessaging.hh
  XrdMqSharedObject.cc  XrdMqSharedObject.hh)

set_target_properties(XrdMqClient-Objects PROPERTIES
  POSITION_INDEPENDENT_CODE TRUE)

add_library(XrdMqClient SHARED $<TARGET_OBJECTS:XrdMqClient-Objects>)

set(XRD_MQ_CLIENT_LIBS
  eosCommon
  ${UUID_LIBRARIES}
  ${NCURSES_LIBRARIES}
  ${XROOTD_CL_LIBRARY}
  ${XROOTD_UTILS_LIBRARY}
  ${OPENSSL_CRYPTO_LIBRARY})

target_link_libraries(XrdMqClient PUBLIC ${XRD_MQ_CLIENT_LIBS})

set_target_properties(
  XrdMqClient PROPERTIES
  VERSION ${VERSION}
  SOVERSION ${VERSION_MAJOR}
  MACOSX_RPATH TRUE)

add_library(XrdMqClient-Static STATIC $<TARGET_OBJECTS:XrdMqClient-Objects>)

target_link_libraries(XrdMqClient-Static PUBLIC ${XRD_MQ_CLIENT_LIBS})

set_target_properties(
  XrdMqClient-Static PROPERTIES
  POSITION_INDEPENDENT_CODE TRUE)

#-------------------------------------------------------------------------------
# XrdMqOfs library
#-------------------------------------------------------------------------------
if (NOT CLIENT)
add_library(XrdMqOfs MODULE
  XrdMqOfs.cc       XrdMqOfs.hh
  XrdMqMessage.cc   XrdMqMessage.hh
  ${CMAKE_SOURCE_DIR}/namespace/ns_quarkdb/BackendClient.cc)

target_link_libraries(
  XrdMqOfs PRIVATE
  eosCommon
  qclient
  ${UUID_LIBRARIES}
  ${NCURSES_LIBRARIES}
  ${XROOTD_CL_LIBRARY}
  ${XROOTD_UTILS_LIBRARY}
  ${OPENSSL_CRYPTO_LIBRARY})

install(
  TARGETS XrdMqOfs
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR})
endif ()

#-------------------------------------------------------------------------------
# Other executables
#-------------------------------------------------------------------------------
add_executable(xrdmqclienttest   tests/XrdMqClientTest.cc)
add_executable(xrdmqdumper       tests/XrdMqQueueDumper.cc)
add_executable(xrdmqfeeder       tests/XrdMqQueueFeeder.cc)
add_executable(xrdmqinjection    tests/XrdMqQueueInjection.cc)
add_executable(xrdmqclientmaster tests/XrdMqClientMaster.cc)
add_executable(xrdmqclientworker tests/XrdMqClientWorker.cc)
add_executable(xrdmqcryptotest   tests/XrdMqCryptoTest.cc)
add_executable(xrdmqsharedobjectclient          tests/XrdMqSharedObjectClient.cc)
add_executable(xrdmqsharedobjectqueueclient     tests/XrdMqSharedObjectQueueClient.cc)
add_executable(xrdmqsharedobjectbroadcastclient tests/XrdMqSharedObjectBroadCastClient.cc)

#-------------------------------------------------------------------------------
# Libraries that all the above executables are linked against
#-------------------------------------------------------------------------------
set(XRDMQ_OTHER_LINK_LIBRARIES
  XrdMqClient-Static
  ${CMAKE_THREAD_LIBS_INIT})

target_link_libraries(xrdmqclienttest PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqdumper PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqfeeder PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqinjection PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqclientmaster PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqclientworker PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqcryptotest PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqsharedobjectclient PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqsharedobjectqueueclient PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})
target_link_libraries(xrdmqsharedobjectbroadcastclient PRIVATE ${XRDMQ_OTHER_LINK_LIBRARIES})

install(
  TARGETS XrdMqClient xrdmqdumper
  LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_FULL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR})
