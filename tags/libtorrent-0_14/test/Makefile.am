test_programs = test_hasher test_bencoding test_ip_filter test_piece_picker \
test_storage test_metadata_extension test_buffer test_swarm test_pe_crypto test_primitives \
test_bandwidth_limiter test_upnp test_fast_extension test_pex test_web_seed \
test_http_connection test_torrent test_transfer test_lsd

if ENABLE_TESTS
# no indent here! - BSD make issue
check_PROGRAMS = $(test_programs)
noinst_LTLIBRARIES = libtest.la
endif
TESTS = $(check_PROGRAMS)

EXTRA_DIST = Jamfile
EXTRA_PROGRAMS = $(test_programs)

noinst_HEADERS = test.hpp setup_transfer.hpp

libtest_la_SOURCES = main.cpp setup_transfer.cpp

test_lsd_SOURCES = test_lsd.cpp
test_http_connection_SOURCES = test_http_connection.cpp
test_web_seed_SOURCES = test_web_seed.cpp
test_pex_SOURCES = test_pex.cpp
test_fast_extension_SOURCES = test_fast_extension.cpp
test_upnp_SOURCES = test_upnp.cpp
test_hasher_SOURCES = test_hasher.cpp
test_bencoding_SOURCES = test_bencoding.cpp
test_ip_filter_SOURCES = test_ip_filter.cpp
test_piece_picker_SOURCES = test_piece_picker.cpp
test_storage_SOURCES = test_storage.cpp
test_buffer_SOURCES = test_buffer.cpp
test_metadata_extension_SOURCES = test_metadata_extension.cpp
test_swarm_SOURCES = test_swarm.cpp
test_pe_crypto_SOURCES = test_pe_crypto.cpp
test_primitives_SOURCES = test_primitives.cpp
test_bandwidth_limiter_SOURCES = test_bandwidth_limiter.cpp
test_torrent_SOURCES = test_torrent.cpp
test_transfer_SOURCES = test_transfer.cpp

LDADD = $(top_builddir)/src/libtorrent-rasterbar.la libtest.la

AM_CXXFLAGS=-ftemplate-depth-50 -I$(top_srcdir)/include -I$(top_srcdir)/include/libtorrent @DEBUGFLAGS@ @PTHREAD_CFLAGS@ -DBOOST_MULTI_INDEX_DISABLE_SERIALIZATION
AM_LDFLAGS= @BOOST_SYSTEM_LIB@ @BOOST_FILESYSTEM_LIB@ @BOOST_THREAD_LIB@ @PTHREAD_LIBS@ @SSL_LDFLAGS@ @SSL_LIBS@

