/*

Copyright (c) 2008, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/session.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/file_pool.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/bencode.hpp"
#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>

#include "test.hpp"
#include "setup_transfer.hpp"

using namespace boost::filesystem;
using namespace libtorrent;

void add_files(
	torrent_info& t
	, path const& p
	, path const& l)
{
#if BOOST_VERSION < 103600
	std::string const& leaf = l.leaf();
#else
	std::string const& leaf = l.filename();
#endif
	if (leaf[0] == '.') return;
	path f(p / l);
	if (is_directory(f))
	{
		for (directory_iterator i(f), end; i != end; ++i)
#if BOOST_VERSION < 103600
			add_files(t, p, l / i->leaf());
#else
			add_files(t, p, l / i->filename());
#endif
	}
	else
	{
		std::cerr << "adding \"" << l.string() << "\"\n";
		t.add_file(l, file_size(f));
	}
}

// proxy: 0=none, 1=socks4, 2=socks5, 3=socks5_pw 4=http 5=http_pw
void test_transfer(torrent_info torrent_file, int proxy)
{
	using namespace libtorrent;

	session ses;
	session_settings settings;
	settings.ignore_limits_on_local_network = false;
	ses.set_settings(settings);
	ses.set_severity_level(alert::debug);
	ses.listen_on(std::make_pair(51000, 52000));
	ses.set_download_rate_limit(torrent_file.total_size() / 10);
	remove_all("./tmp1");

	char const* test_name[] = {"no", "SOCKS4", "SOCKS5", "SOCKS5 password", "HTTP", "HTTP password"};

	std::cerr << "  ==== TESTING " << test_name[proxy] << " proxy ====" << std::endl;
	
	if (proxy)
	{
		start_proxy(8002, proxy);
		proxy_settings ps;
		ps.hostname = "127.0.0.1";
		ps.port = 8002;
		ps.username = "testuser";
		ps.password = "testpass";
		ps.type = (proxy_settings::proxy_type)proxy;
		ses.set_web_seed_proxy(ps);
	}

	torrent_handle th = ses.add_torrent(torrent_file, "./tmp1");

	std::vector<announce_entry> empty;
	th.replace_trackers(empty);

	const size_type total_size = torrent_file.total_size();

	float rate_sum = 0.f;
	float ses_rate_sum = 0.f;

	for (int i = 0; i < 30; ++i)
	{
		torrent_status s = th.status();
		session_status ss = ses.status();
		std::cerr << (s.progress * 100.f) << " %"
			<< " torrent rate: " << (s.download_rate / 1000.f) << " kB/s"
			<< " session rate: " << (ss.download_rate / 1000.f) << " kB/s"
			<< " session total: " << ss.total_payload_download
			<< " torrent total: " << s.total_payload_download
			<< std::endl;
		rate_sum += s.download_payload_rate;
		ses_rate_sum += ss.payload_download_rate;

		print_alerts(ses, "ses");

		if (th.is_seed() && ss.download_rate == 0.f)
		{
			TEST_CHECK(ses.status().total_payload_download == total_size);
			TEST_CHECK(th.status().total_payload_download == total_size);
			break;
		}
		test_sleep(1000);
	}

	std::cerr << "total_size: " << total_size
		<< " rate_sum: " << rate_sum
		<< " session_rate_sum: " << ses_rate_sum
		<< std::endl;

	// the rates for each second should sum up to the total, with a 10% error margin
	TEST_CHECK(fabs(rate_sum - total_size) < total_size * .1f);
	TEST_CHECK(fabs(ses_rate_sum - total_size) < total_size * .1f);

	TEST_CHECK(th.is_seed());

	if (proxy) stop_proxy(8002);

	remove_all("./tmp1");
}

int test_main()
{
	using namespace libtorrent;
	using namespace boost::filesystem;

	boost::intrusive_ptr<torrent_info> torrent_file(new torrent_info);
	torrent_file->add_url_seed("http://127.0.0.1:8000/");

	create_directory("test_torrent");
	char random_data[300000];
	std::srand(std::time(0));
	std::generate(random_data, random_data + sizeof(random_data), &std::rand);
	std::ofstream("./test_torrent/test1").write(random_data, 35);
	std::ofstream("./test_torrent/test2").write(random_data, 16536 - 35);
	std::ofstream("./test_torrent/test3").write(random_data, 16536);
	std::ofstream("./test_torrent/test4").write(random_data, 17);
	std::ofstream("./test_torrent/test5").write(random_data, 16536);
	std::ofstream("./test_torrent/test6").write(random_data, 300000);
	std::ofstream("./test_torrent/test7").write(random_data, 300000);

	add_files(*torrent_file, complete("."), "test_torrent");

	start_web_server(8000);

	file_pool fp;
	boost::scoped_ptr<storage_interface> s(default_storage_constructor(
		torrent_file, ".", fp));
	// calculate the hash for all pieces
	int num = torrent_file->num_pieces();
	std::vector<char> buf(torrent_file->piece_length());
	for (int i = 0; i < num; ++i)
	{
		s->read(&buf[0], i, 0, torrent_file->piece_size(i));
		hasher h(&buf[0], torrent_file->piece_size(i));
		torrent_file->set_hash(i, h.final());
	}
	
	// to calculate the info_hash
	entry te = torrent_file->create_torrent();


	for (int i = 0; i < 6; ++i)
		test_transfer(*torrent_file, i);


	
	stop_web_server(8000);
	remove_all("./test_torrent");
	return 0;
}

