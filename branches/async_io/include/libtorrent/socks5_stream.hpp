/*

Copyright (c) 2007, Arvid Norberg
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

#ifndef TORRENT_SOCKS5_STREAM_HPP_INCLUDED
#define TORRENT_SOCKS5_STREAM_HPP_INCLUDED

#include "libtorrent/proxy_base.hpp"

namespace libtorrent {

class socks5_stream : public proxy_base
{
public:

	explicit socks5_stream(asio::io_service& io_service)
		: proxy_base(io_service)
	{}

	void set_username(std::string const& user
		, std::string const& password)
	{
		m_user = user;
		m_password = password;
	}

	typedef boost::function<void(asio::error_code const&)> handler_type;

	template <class Handler>
	void async_connect(endpoint_type const& endpoint, Handler const& handler)
	{
		m_remote_endpoint = endpoint;

		// the connect is split up in the following steps:
		// 1. resolve name of proxy server
		// 2. connect to proxy server
		// 3. send SOCKS5 authentication method message
		// 4. read SOCKS5 authentication response
		// 5. send username+password
		// 6. send SOCKS5 CONNECT message

		// to avoid unnecessary copying of the handler,
		// store it in a shaed_ptr
		boost::shared_ptr<handler_type> h(new handler_type(handler));

		tcp::resolver::query q(m_hostname
			, boost::lexical_cast<std::string>(m_port));
		m_resolver.async_resolve(q, boost::bind(
			&socks5_stream::name_lookup, this, _1, _2, h));
	}

private:

	void name_lookup(asio::error_code const& e, tcp::resolver::iterator i
		, boost::shared_ptr<handler_type> h);
	void connected(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void handshake1(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void handshake2(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void handshake3(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void handshake4(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void socks_connect(boost::shared_ptr<handler_type> h);
	void connect1(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void connect2(asio::error_code const& e, boost::shared_ptr<handler_type> h);
	void connect3(asio::error_code const& e, boost::shared_ptr<handler_type> h);

	// send and receive buffer
	std::vector<char> m_buffer;
	// proxy authentication
	std::string m_user;
	std::string m_password;
};

}

#endif
