/*

Copyright (c) 2003, Arvid Norberg
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

#ifndef TORRENT_BT_PEER_CONNECTION_HPP_INCLUDED
#define TORRENT_BT_PEER_CONNECTION_HPP_INCLUDED

#include <ctime>
#include <algorithm>
#include <vector>
#include <deque>
#include <string>

#include "libtorrent/debug.hpp"

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/smart_ptr.hpp>
#include <boost/noncopyable.hpp>
#include <boost/array.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/optional.hpp>
#include <boost/cstdint.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "libtorrent/buffer.hpp"
#include "libtorrent/peer_connection.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/peer_id.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/stat.hpp"
#include "libtorrent/alert.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/allocate_resources.hpp"
#include "libtorrent/peer_request.hpp"
#include "libtorrent/piece_block_progress.hpp"
#include "libtorrent/config.hpp"

namespace libtorrent
{
	class torrent;

	namespace detail
	{
		struct session_impl;
	}

	class TORRENT_EXPORT bt_peer_connection
		: public peer_connection
	{
	friend class invariant_access;
	public:

		// this is the constructor where the we are the active part.
		// The peer_conenction should handshake and verify that the
		// other end has the correct id
		bt_peer_connection(
			detail::session_impl& ses
			, boost::weak_ptr<torrent> t
			, boost::shared_ptr<stream_socket> s
			, tcp::endpoint const& remote);

		// with this constructor we have been contacted and we still don't
		// know which torrent the connection belongs to
		bt_peer_connection(
			detail::session_impl& ses
			, boost::shared_ptr<stream_socket> s);

		~bt_peer_connection();

		// called from the main loop when this connection has any
		// work to do.

		void on_sent(asio::error const& error
			, std::size_t bytes_transferred);
		void on_receive(asio::error const& error
			, std::size_t bytes_transferred);
		
		virtual void get_peer_info(peer_info& p) const;

		bool support_extensions() const { return m_supports_extensions; }

		bool supports_extension(extension_index ex) const
		{ return m_extension_messages[ex] > 0; }

		bool has_metadata() const;

		// the message handlers are called
		// each time a recv() returns some new
		// data, the last time it will be called
		// is when the entire packet has been
		// received, then it will no longer
		// be called. i.e. most handlers need
		// to check how much of the packet they
		// have received before any processing
		void on_keepalive();
		void on_choke(int received);
		void on_unchoke(int received);
		void on_interested(int received);
		void on_not_interested(int received);
		void on_have(int received);
		void on_bitfield(int received);
		void on_request(int received);
		void on_piece(int received);
		void on_cancel(int received);
		void on_dht_port(int received);

		void on_extended(int received);

		void on_extended_handshake();
		void on_chat();
		void on_metadata();
		void on_peer_exchange();

		typedef void (bt_peer_connection::*message_handler)(int received);

		// the following functions appends messages
		// to the send buffer
		void write_choke();
		void write_unchoke();
		void write_interested();
		void write_not_interested();
		void write_request(peer_request const& r);
		void write_cancel(peer_request const& r);
		void write_bitfield(std::vector<bool> const& bitfield);
		void write_have(int index);
		void write_piece(peer_request const& r);
		void write_handshake();
		void write_extensions();
		void write_chat_message(const std::string& msg);
		void write_metadata(std::pair<int, int> req);
		void write_metadata_request(std::pair<int, int> req);
		void write_keepalive();
		void write_dht_port(int listen_port);
		void on_connected() {}
		void on_tick();

#ifndef NDEBUG
		void check_invariant() const;
		boost::posix_time::ptime m_last_choke;
#endif

	private:

		bool dispatch_message(int received);
		// returns the block currently being
		// downloaded. And the progress of that
		// block. If the peer isn't downloading
		// a piece for the moment, the boost::optional
		// will be invalid.
		boost::optional<piece_block_progress> downloading_piece_progress() const;

		// if we don't have all metadata
		// this function will request a part of it
		// from this peer
		void request_metadata();

		enum state
		{
			read_protocol_length = 0,
			read_protocol_string,
			read_info_hash,
			read_peer_id,

			read_packet_size,
			read_packet
		};
		
		std::string m_client_version;

		state m_state;

		// the timeout in seconds
		int m_timeout;

		enum message_type
		{
	// standard messages
			msg_choke = 0,
			msg_unchoke,
			msg_interested,
			msg_not_interested,
			msg_have,
			msg_bitfield,
			msg_request,
			msg_piece,
			msg_cancel,
			msg_dht_port,
	// extension protocol message
			msg_extended = 20,

			num_supported_messages
		};

		static const message_handler m_message_handler[num_supported_messages];

		// this is a queue of ranges that describes
		// where in the send buffer actual payload
		// data is located. This is currently
		// only used to be able to gather statistics
		// seperately on payload and protocol data.
		struct range
		{
			range(int s, int l)
				: start(s)
				, length(l)
			{
				assert(s >= 0);
				assert(l > 0);
			}
			int start;
			int length;
		};
		static bool range_below_zero(const range& r)
		{ return r.start < 0; }
		std::deque<range> m_payloads;

		// this is set to true if the handshake from
		// the peer indicated that it supports the
		// extension protocol
		bool m_supports_extensions;
		bool m_supports_dht_port;

		static const char* extension_names[num_supported_extensions];
		// contains the indices of the extension messages for each extension
		// supported by the other end. A value of <= 0 means that the extension
		// is not supported.
		int m_extension_messages[num_supported_extensions];

		// this is set to the current time each time we get a
		// "I don't have metadata" message.
		boost::posix_time::ptime m_no_metadata;

		// this is set to the time when we last sent
		// a request for metadata to this peer
		boost::posix_time::ptime m_metadata_request;

		// this is set to true when we send a metadata
		// request to this peer, and reset to false when
		// we receive a reply to our request.
		bool m_waiting_metadata_request;

		// if we're waiting for a metadata request
		// this was the request we sent
		std::pair<int, int> m_last_metadata_request;

		// the number of bytes of metadata we have received
		// so far from this per, only counting the current
		// request. Any previously finished requests
		// that have been forwarded to the torrent object
		// do not count.
		int m_metadata_progress;

#ifndef NDEBUG
		bool m_in_constructor;
#endif
	};
}

#endif // TORRENT_BT_PEER_CONNECTION_HPP_INCLUDED

