yum install -y cmake3
yum install --disablerepo=epel -y xrootd-server-devel xrootd-private-devel xrootd-client xrootd-client-devel
yum install -y sparsehash-devel
yum install -y ncurses ncurses-devel ncurses-static openssl openssl-devel openssl-static
yum install -y readline readline-devel
yum install -y libuuid libuuid-devel
yum install -y zeromq3 zeromq3-devel
yum install -y protobuf protobuf-devel leveldb leveldb-devel
yum install -y cppunit cppunit-devel
yum install -y fuse-libs fuse fuse-devel
yum install -y libattr libattr-devel
yum install -y openldap openldap-devel
yum install -y libmicrohttp libmicrohttpd-devel
yum install -y zlib zlib-devel zlib-static
yum install -y xfsprogs xfsprogs-devel
yum install -y e2fsprogs-devel
yum install -y perl-Time-HiRes
yum install -y json-c json-c-devel
yum install -y jsoncpp jsoncpp-devel
yum install -y libcurl libcurl-devel
yum install -y hiredis hiredis-devel
yum install -y libevent libevent-devel
yum install -y git
( cd /tmp/; git clone  https://github.com/zeromq/cppzmq; cp cppzmq/zmq.hpp /usr/include/ )
