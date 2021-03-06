/*

Copyright (c) 2003 - 2006, Arvid Norberg
Copyright (c) 2007, Arvid Norberg, Un Shyam
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

#include "libtorrent/pch.hpp"

#include <vector>
#include <iostream>
#include <iomanip>
#include <limits>
#include <boost/bind.hpp>

#include "libtorrent/bt_peer_connection.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/identify_client.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/version.hpp"
#include "libtorrent/extensions.hpp"
#include "libtorrent/aux_/session_impl.hpp"

#ifndef TORRENT_DISABLE_ENCRYPTION
#include "libtorrent/pe_crypto.hpp"
#include "libtorrent/hasher.hpp"
#endif

using boost::bind;
using boost::shared_ptr;
using libtorrent::aux::session_impl;

namespace libtorrent
{
	const bt_peer_connection::message_handler
	bt_peer_connection::m_message_handler[] =
	{
		&bt_peer_connection::on_choke,
		&bt_peer_connection::on_unchoke,
		&bt_peer_connection::on_interested,
		&bt_peer_connection::on_not_interested,
		&bt_peer_connection::on_have,
		&bt_peer_connection::on_bitfield,
		&bt_peer_connection::on_request,
		&bt_peer_connection::on_piece,
		&bt_peer_connection::on_cancel,
		&bt_peer_connection::on_dht_port,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		&bt_peer_connection::on_extended
	};


	bt_peer_connection::bt_peer_connection(
		session_impl& ses
		, boost::weak_ptr<torrent> tor
		, shared_ptr<socket_type> s
		, tcp::endpoint const& remote
		, policy::peer* peerinfo)
		: peer_connection(ses, tor, s, remote
			, peerinfo)
		, m_state(read_protocol_identifier)
#ifndef TORRENT_DISABLE_EXTENSIONS
		, m_supports_extensions(false)
#endif
		, m_supports_dht_port(false)
#ifndef TORRENT_DISABLE_ENCRYPTION
		, m_encrypted(false)
		, m_rc4_encrypted(false)
		, m_sync_bytes_read(0)
		, m_enc_send_buffer(0, 0)
#endif
#ifndef NDEBUG
		, m_sent_bitfield(false)
		, m_in_constructor(true)
#endif
	{
#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << "*** bt_peer_connection\n";
#endif

#ifndef TORRENT_DISABLE_ENCRYPTION
		
		pe_settings::enc_policy const& out_enc_policy = m_ses.get_pe_settings().out_enc_policy;

		if (out_enc_policy == pe_settings::forced)
		{
			write_pe1_2_dhkey();

			m_state = read_pe_dhkey;
			reset_recv_buffer(dh_key_len);
			setup_receive();
		}
		else if (out_enc_policy == pe_settings::enabled)
		{
			assert(peer_info_struct());

			policy::peer* pi = peer_info_struct();
			if (pi->pe_support == true)
			{
				// toggle encryption support flag, toggled back to
				// true if encrypted portion of the handshake
				// completes correctly
				pi->pe_support = !(pi->pe_support);

				write_pe1_2_dhkey();
				m_state = read_pe_dhkey;
				reset_recv_buffer(dh_key_len);
				setup_receive();
			}
			else // pi->pe_support == false
			{
				// toggled back to false if standard handshake
				// completes correctly (without encryption)
				pi->pe_support = !(pi->pe_support);

				write_handshake();
				reset_recv_buffer(20);
				setup_receive();
			}
		}
		else if (out_enc_policy == pe_settings::disabled)
#endif
		{
			write_handshake();
			
			// start in the state where we are trying to read the
			// handshake from the other side
			reset_recv_buffer(20);
			setup_receive();
		}
		
#ifndef NDEBUG
		m_in_constructor = false;
#endif
	}

	bt_peer_connection::bt_peer_connection(
		session_impl& ses
		, boost::shared_ptr<socket_type> s
		, policy::peer* peerinfo)
		: peer_connection(ses, s, peerinfo)
		, m_state(read_protocol_identifier)
#ifndef TORRENT_DISABLE_EXTENSIONS
		, m_supports_extensions(false)
#endif
		, m_supports_dht_port(false)
#ifndef TORRENT_DISABLE_ENCRYPTION
		, m_encrypted(false)
		, m_rc4_encrypted(false)
		, m_sync_bytes_read(0)
		, m_enc_send_buffer(0, 0)
#endif		
#ifndef NDEBUG
		, m_sent_bitfield(false)
		, m_in_constructor(true)
#endif
	{

		// we are not attached to any torrent yet.
		// we have to wait for the handshake to see
		// which torrent the connector want's to connect to


		// upload bandwidth will only be given to connections
		// that are part of a torrent. Since this is an incoming
		// connection, we have to give it some initial bandwidth
		// to send the handshake.
#ifndef TORRENT_DISABLE_ENCRYPTION
		m_bandwidth_limit[download_channel].assign(2048);
		m_bandwidth_limit[upload_channel].assign(2048);
#else
		m_bandwidth_limit[download_channel].assign(80);
		m_bandwidth_limit[upload_channel].assign(80);
#endif

		// start in the state where we are trying to read the
		// handshake from the other side
		reset_recv_buffer(20);
		setup_receive();
#ifndef NDEBUG
		m_in_constructor = false;
#endif
	}

	bt_peer_connection::~bt_peer_connection()
	{
	}

	void bt_peer_connection::on_metadata()
	{
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);
		write_bitfield(t->pieces());
	}

	void bt_peer_connection::write_dht_port(int listen_port)
	{
		INVARIANT_CHECK;
#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string()
			<< " ==> DHT_PORT [ " << listen_port << " ]\n";
#endif
		buffer::interval packet = allocate_send_buffer(7);
		detail::write_uint32(3, packet.begin);
		detail::write_uint8(msg_dht_port, packet.begin);
		detail::write_uint16(listen_port, packet.begin);
		assert(packet.begin == packet.end);
		setup_send();
	}

	void bt_peer_connection::get_specific_peer_info(peer_info& p) const
	{
		assert(!associated_torrent().expired());

		if (is_interesting()) p.flags |= peer_info::interesting;
		if (is_choked()) p.flags |= peer_info::choked;
		if (is_peer_interested()) p.flags |= peer_info::remote_interested;
		if (has_peer_choked()) p.flags |= peer_info::remote_choked;
		if (support_extensions()) p.flags |= peer_info::supports_extensions;
		if (is_local()) p.flags |= peer_info::local_connection;

#ifndef TORRENT_DISABLE_ENCRYPTION
		if (m_encrypted)
		{
			m_rc4_encrypted ? 
				p.flags |= peer_info::rc4_encrypted :
				p.flags |= peer_info::plaintext_encrypted;
		}
#endif

		if (!is_connecting() && in_handshake())
			p.flags |= peer_info::handshake;
		if (is_connecting() && !is_queued()) p.flags |= peer_info::connecting;
		if (is_queued()) p.flags |= peer_info::queued;

		p.client = m_client_version;
		p.connection_type = peer_info::standard_bittorrent;

	}
	
	bool bt_peer_connection::in_handshake() const
	{
		return m_state < read_packet_size;
	}

#ifndef TORRENT_DISABLE_ENCRYPTION

	void bt_peer_connection::write_pe1_2_dhkey()
	{
		INVARIANT_CHECK;

		assert(!m_encrypted);
		assert(!m_rc4_encrypted);
		assert(!m_DH_key_exchange.get());

#ifdef TORRENT_VERBOSE_LOGGING
		if (is_local())
			(*m_logger) << " initiating encrypted handshake\n";
#endif

		m_DH_key_exchange.reset(new DH_key_exchange);

		int pad_size = std::rand() % 512;
		buffer::interval send_buf = allocate_send_buffer(dh_key_len + pad_size);

		std::copy (m_DH_key_exchange->get_local_key(),
				   m_DH_key_exchange->get_local_key() + dh_key_len,
				   send_buf.begin);

		std::generate(send_buf.begin + dh_key_len, send_buf.end, std::rand);
		setup_send();

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << " sent DH key\n";
#endif
	}

	void bt_peer_connection::write_pe3_sync()
	{
		INVARIANT_CHECK;

		assert (!m_encrypted);
		assert (!m_rc4_encrypted);
		assert (is_local());
		
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);
		
		hasher h;
		sha1_hash const& info_hash = t->torrent_file().info_hash();
		char const* const secret = m_DH_key_exchange->get_secret();

		int pad_size = 0; // rand() % 512; // Keep 0 for now

		// synchash,skeyhash,vc,crypto_provide,len(pad),pad,len(ia)
		buffer::interval send_buf = 
			allocate_send_buffer (20 + 20 + 8 + 4 + 2 + pad_size + 2);

		// sync hash (hash('req1',S))
		h.reset();
		h.update("req1",4);
		h.update(secret, dh_key_len);
		sha1_hash sync_hash = h.final();

		std::copy (sync_hash.begin(), sync_hash.end(), send_buf.begin);
		send_buf.begin += 20;

		// stream key obfuscated hash [ hash('req2',SKEY) xor hash('req3',S) ]
		h.reset();
		h.update("req2",4);
		h.update((const char*)info_hash.begin(), 20);
		sha1_hash streamkey_hash = h.final();

		h.reset();
		h.update("req3",4);
		h.update(secret, dh_key_len);
		sha1_hash obfsc_hash = h.final();
		obfsc_hash ^= streamkey_hash;

		std::copy (obfsc_hash.begin(), obfsc_hash.end(), send_buf.begin);
		send_buf.begin += 20;

		// Discard DH key exchange data, setup RC4 keys
		init_pe_RC4_handler(secret, info_hash);
		m_DH_key_exchange.reset(); // secret should be invalid at this point
	
		// write the verification constant and crypto field
		assert(send_buf.left() == 8 + 4 + 2 + pad_size + 2);
		int encrypt_size = send_buf.left();

		int crypto_provide = 0;
		pe_settings::enc_level const& allowed_enc_level = m_ses.get_pe_settings().allowed_enc_level;

		if (allowed_enc_level == pe_settings::both) 
			crypto_provide = 0x03;
		else if (allowed_enc_level == pe_settings::rc4) 
			crypto_provide = 0x02;
		else if (allowed_enc_level == pe_settings::plaintext)
			crypto_provide = 0x01;

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << " crypto provide : [ ";
		if (allowed_enc_level == pe_settings::both)
			(*m_logger) << "plaintext rc4 ]\n";
		else if (allowed_enc_level == pe_settings::rc4)
			(*m_logger) << "rc4 ]\n";
		else if (allowed_enc_level == pe_settings::plaintext)
			(*m_logger) << "plaintext ]\n";
#endif

		write_pe_vc_cryptofield(send_buf, crypto_provide, pad_size);
		m_RC4_handler->encrypt(send_buf.end - encrypt_size, encrypt_size);

		assert(send_buf.begin == send_buf.end);
		setup_send();
	}

	void bt_peer_connection::write_pe4_sync(int crypto_select)
	{
		INVARIANT_CHECK;

		assert(!is_local());
		assert(!m_encrypted);
		assert(!m_rc4_encrypted);
		assert(crypto_select == 0x02 || crypto_select == 0x01);

		int pad_size = 0; // rand() % 512; // Keep 0 for now

		const int buf_size = 8+4+2+pad_size;
		buffer::interval send_buf = allocate_send_buffer(buf_size);
		write_pe_vc_cryptofield(send_buf, crypto_select, pad_size);

		m_RC4_handler->encrypt(send_buf.end - buf_size, buf_size);
		setup_send();

		// encryption method has been negotiated
		if (crypto_select == 0x02) 
			m_rc4_encrypted = true;
		else // 0x01
			m_rc4_encrypted = false;

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << " crypto select : [ ";
		if (crypto_select == 0x01)
			(*m_logger) << "plaintext ]\n";
		else
			(*m_logger) << "rc4 ]\n";
#endif
	}

 	void bt_peer_connection::write_pe_vc_cryptofield(buffer::interval& write_buf,
													 int crypto_field, int pad_size)
 	{
		INVARIANT_CHECK;

		assert(crypto_field <= 0x03 && crypto_field > 0);
		assert(pad_size == 0); // pad not used yet
		// vc,crypto_field,len(pad),pad, (len(ia))
		assert( (write_buf.left() == 8+4+2+pad_size+2 && is_local()) ||
				(write_buf.left() == 8+4+2+pad_size   && !is_local()) );

		// encrypt(vc, crypto_provide/select, len(Pad), len(IA))
		// len(pad) is zero for now, len(IA) only for outgoing connections
		
		// vc
		std::fill(write_buf.begin, write_buf.begin + 8, 0);
		write_buf.begin += 8;

		detail::write_uint32(crypto_field, write_buf.begin);
		detail::write_uint16(pad_size, write_buf.begin); // len (pad)

		// fill pad with zeroes
		// std::fill(write_buf.begin, write_buf.begin+pad_size, 0);
		// write_buf.begin += pad_size;

		// append len(ia) if we are initiating
		if (is_local())
			detail::write_uint16(handshake_len, write_buf.begin); // len(IA)
		
		assert (write_buf.begin == write_buf.end);
 	}

	void bt_peer_connection::init_pe_RC4_handler(char const* secret, sha1_hash const& stream_key)
	{
		INVARIANT_CHECK;

		assert(secret);
		
		hasher h;
		const char keyA[] = "keyA";
		const char keyB[] = "keyB";

		// encryption rc4 longkeys
		// outgoing connection : hash ('keyA',S,SKEY)
		// incoming connection : hash ('keyB',S,SKEY)
		
		is_local() ? h.update(keyA, 4) : h.update(keyB, 4);
		h.update(secret, dh_key_len);
		h.update((char const*)stream_key.begin(), 20);
		const sha1_hash local_key = h.final();

		h.reset();

		// decryption rc4 longkeys
		// outgoing connection : hash ('keyB',S,SKEY)
		// incoming connection : hash ('keyA',S,SKEY)
		
		is_local() ? h.update(keyB, 4) : h.update(keyA, 4);
		h.update(secret, dh_key_len);
		h.update((char const*)stream_key.begin(), 20);
		const sha1_hash remote_key = h.final();
		
		assert(!m_RC4_handler.get());
		m_RC4_handler.reset (new RC4_handler (local_key, remote_key));

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << " computed RC4 keys\n";
#endif
	}

	void bt_peer_connection::send_buffer(char* begin, char* end)
	{
		assert (begin);
		assert (end);
		assert (end > begin);
		assert (!m_rc4_encrypted || m_encrypted);

		if (m_rc4_encrypted)
			m_RC4_handler->encrypt(begin, end - begin);
		
		peer_connection::send_buffer(begin, end);
	}

	buffer::interval bt_peer_connection::allocate_send_buffer(int size)
	{
		assert(!m_rc4_encrypted || m_encrypted);

		if (m_rc4_encrypted)
		{
			m_enc_send_buffer = peer_connection::allocate_send_buffer(size);
			return m_enc_send_buffer;
		}
		else
		{
			buffer::interval i = peer_connection::allocate_send_buffer(size);
			return i;
		}
	}
	
	void bt_peer_connection::setup_send()
	{
		assert(!m_rc4_encrypted || m_encrypted);

 		if (m_rc4_encrypted)
		{
			assert (m_enc_send_buffer.begin);
			assert (m_enc_send_buffer.end);
			assert (m_enc_send_buffer.left() > 0);
			
 			m_RC4_handler->encrypt (m_enc_send_buffer.begin, m_enc_send_buffer.left());
		}
		peer_connection::setup_send();
	}

	int bt_peer_connection::get_syncoffset(char const* src, int src_size,
										   char const* target, int target_size) const
	{
		assert (target_size >= src_size);
		assert (src_size > 0);
		assert (src);
		assert (target);

		int traverse_limit = target_size - src_size;

		// TODO: this could be optimized using knuth morris pratt
		for (int i = 0; i < traverse_limit; ++i)
		{
			char const* target_ptr = target + i;
			if (std::equal(src, src+src_size, target_ptr))
				return i;
		}

//	    // Partial sync
// 		for (int i = 0; i < target_size; ++i)
// 		{
// 			// first is iterator in src[] at which mismatch occurs
// 			// second is iterator in target[] at which mismatch occurs
// 			std::pair<const char*, const char*> ret;
// 			int src_sync_size;
//  			if (i > traverse_limit) // partial sync test
//  			{
//  				ret = std::mismatch(src, src + src_size - (i - traverse_limit), &target[i]);
//  				src_sync_size = ret.first - src;
//  				if (src_sync_size == (src_size - (i - traverse_limit)))
//  					return i;
//  			}
//  			else // complete sync test
// 			{
// 				ret = std::mismatch(src, src + src_size, &target[i]);
// 				src_sync_size = ret.first - src;
// 				if (src_sync_size == src_size)
// 					return i;
// 			}
// 		}

        // no complete sync
		return -1;
	}
#endif // #ifndef TORRENT_DISABLE_ENCRYPTION
	
	void bt_peer_connection::write_handshake()
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		// add handshake to the send buffer
		const char version_string[] = "BitTorrent protocol";
		const int string_len = sizeof(version_string)-1;

		buffer::interval i = allocate_send_buffer(1 + string_len + 8 + 20 + 20);
		// length of version string
		*i.begin = string_len;
		++i.begin;

		// version string itself
		std::copy(
			version_string
			, version_string + string_len
			, i.begin);
		i.begin += string_len;

		// 8 zeroes
		std::fill(i.begin, i.begin + 8, 0);

#ifndef TORRENT_DISABLE_DHT
		// indicate that we support the DHT messages
		*(i.begin + 7) = 0x01;
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		// we support extensions
		*(i.begin + 5) = 0x10;
#endif

		i.begin += 8;

		// info hash
		sha1_hash const& ih = t->torrent_file().info_hash();
		std::copy(ih.begin(), ih.end(), i.begin);
		i.begin += 20;

		// peer id
		std::copy(
			m_ses.get_peer_id().begin()
			, m_ses.get_peer_id().end()
			, i.begin);
		i.begin += 20;
		assert(i.begin == i.end);

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " ==> HANDSHAKE\n";
#endif
		setup_send();
	}

	boost::optional<piece_block_progress> bt_peer_connection::downloading_piece_progress() const
	{
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		buffer::const_interval recv_buffer = receive_buffer();
		// are we currently receiving a 'piece' message?
		if (m_state != read_packet
			|| recv_buffer.left() < 9
			|| recv_buffer[0] != msg_piece)
			return boost::optional<piece_block_progress>();

		const char* ptr = recv_buffer.begin + 1;
		peer_request r;
		r.piece = detail::read_int32(ptr);
		r.start = detail::read_int32(ptr);
		r.length = packet_size() - 9;

		// is any of the piece message header data invalid?
		if (!verify_piece(r))
			return boost::optional<piece_block_progress>();

		piece_block_progress p;

		p.piece_index = r.piece;
		p.block_index = r.start / t->block_size();
		p.bytes_downloaded = recv_buffer.left() - 9;
		p.full_block_bytes = r.length;

		return boost::optional<piece_block_progress>(p);
	}


	// message handlers

	// -----------------------------
	// --------- KEEPALIVE ---------
	// -----------------------------

	void bt_peer_connection::on_keepalive()
	{
		INVARIANT_CHECK;

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " <== KEEPALIVE\n";
#endif
		incoming_keepalive();
	}

	// -----------------------------
	// ----------- CHOKE -----------
	// -----------------------------

	void bt_peer_connection::on_choke(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 1)
			throw protocol_error("'choke' message size != 1");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		incoming_choke();
	}

	// -----------------------------
	// ---------- UNCHOKE ----------
	// -----------------------------

	void bt_peer_connection::on_unchoke(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 1)
			throw protocol_error("'unchoke' message size != 1");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		incoming_unchoke();
	}

	// -----------------------------
	// -------- INTERESTED ---------
	// -----------------------------

	void bt_peer_connection::on_interested(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 1)
			throw protocol_error("'interested' message size != 1");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		incoming_interested();
	}

	// -----------------------------
	// ------ NOT INTERESTED -------
	// -----------------------------

	void bt_peer_connection::on_not_interested(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 1)
			throw protocol_error("'not interested' message size != 1");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		incoming_not_interested();
	}

	// -----------------------------
	// ----------- HAVE ------------
	// -----------------------------

	void bt_peer_connection::on_have(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 5)
			throw protocol_error("'have' message size != 5");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		buffer::const_interval recv_buffer = receive_buffer();

		const char* ptr = recv_buffer.begin + 1;
		int index = detail::read_int32(ptr);

		incoming_have(index);
	}

	// -----------------------------
	// --------- BITFIELD ----------
	// -----------------------------

	void bt_peer_connection::on_bitfield(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		// if we don't have the metedata, we cannot
		// verify the bitfield size
		if (t->valid_metadata()
			&& packet_size() - 1 != ((int)get_bitfield().size() + 7) / 8)
			throw protocol_error("bitfield with invalid size");

		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		buffer::const_interval recv_buffer = receive_buffer();

		std::vector<bool> bitfield;
		
		if (!t->valid_metadata())
			bitfield.resize((packet_size() - 1) * 8);
		else
			bitfield.resize(get_bitfield().size());

		// if we don't have metadata yet
		// just remember the bitmask
		// don't update the piecepicker
		// (since it doesn't exist yet)
		for (int i = 0; i < (int)bitfield.size(); ++i)
			bitfield[i] = (recv_buffer[1 + (i>>3)] & (1 << (7 - (i&7)))) != 0;

		incoming_bitfield(bitfield);
	}

	// -----------------------------
	// ---------- REQUEST ----------
	// -----------------------------

	void bt_peer_connection::on_request(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 13)
			throw protocol_error("'request' message size != 13");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		buffer::const_interval recv_buffer = receive_buffer();

		peer_request r;
		const char* ptr = recv_buffer.begin + 1;
		r.piece = detail::read_int32(ptr);
		r.start = detail::read_int32(ptr);
		r.length = detail::read_int32(ptr);
		
		incoming_request(r);
	}

	// -----------------------------
	// ----------- PIECE -----------
	// -----------------------------

	void bt_peer_connection::on_piece(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		
		buffer::const_interval recv_buffer = receive_buffer();
		int recv_pos = recv_buffer.end - recv_buffer.begin;

		// classify the received data as protocol chatter
		// or data payload for the statistics
		if (recv_pos <= 9)
			// only received protocol data
			m_statistics.received_bytes(0, received);
		else if (recv_pos - received >= 9)
			// only received payload data
			m_statistics.received_bytes(received, 0);
		else
		{
			// received a bit of both
			assert(recv_pos - received < 9);
			assert(recv_pos > 9);
			assert(9 - (recv_pos - received) <= 9);
			m_statistics.received_bytes(
				recv_pos - 9
				, 9 - (recv_pos - received));
		}

		incoming_piece_fragment();
		if (!packet_finished()) return;

		const char* ptr = recv_buffer.begin + 1;
		peer_request p;
		p.piece = detail::read_int32(ptr);
		p.start = detail::read_int32(ptr);
		p.length = packet_size() - 9;

		incoming_piece(p, recv_buffer.begin + 9);
	}

	// -----------------------------
	// ---------- CANCEL -----------
	// -----------------------------

	void bt_peer_connection::on_cancel(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 13)
			throw protocol_error("'cancel' message size != 13");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		buffer::const_interval recv_buffer = receive_buffer();

		peer_request r;
		const char* ptr = recv_buffer.begin + 1;
		r.piece = detail::read_int32(ptr);
		r.start = detail::read_int32(ptr);
		r.length = detail::read_int32(ptr);

		incoming_cancel(r);
	}

	// -----------------------------
	// --------- DHT PORT ----------
	// -----------------------------

	void bt_peer_connection::on_dht_port(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		if (packet_size() != 3)
			throw protocol_error("'dht_port' message size != 3");
		m_statistics.received_bytes(0, received);
		if (!packet_finished()) return;

		buffer::const_interval recv_buffer = receive_buffer();

		const char* ptr = recv_buffer.begin + 1;
		int listen_port = detail::read_uint16(ptr);
		
		incoming_dht_port(listen_port);
	}

	// -----------------------------
	// --------- EXTENDED ----------
	// -----------------------------

	void bt_peer_connection::on_extended(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);
		m_statistics.received_bytes(0, received);
		if (packet_size() < 2)
			throw protocol_error("'extended' message smaller than 2 bytes");

		if (associated_torrent().expired())
			throw protocol_error("'extended' message sent before proper handshake");

		buffer::const_interval recv_buffer = receive_buffer();
		if (recv_buffer.left() < 2) return;

		assert(*recv_buffer.begin == msg_extended);
		++recv_buffer.begin;

		int extended_id = detail::read_uint8(recv_buffer.begin);

		if (extended_id == 0)
		{
			on_extended_handshake();
			return;
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end; ++i)
		{
			if ((*i)->on_extended(packet_size() - 2, extended_id
				, recv_buffer))
				return;
		}
#endif

		throw protocol_error("unknown extended message id: "
			+ boost::lexical_cast<std::string>(extended_id));
	}

	void bt_peer_connection::on_extended_handshake()
	{
		if (!packet_finished()) return;

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		buffer::const_interval recv_buffer = receive_buffer();

		entry root;
		try
		{
			root = bdecode(recv_buffer.begin + 2, recv_buffer.end);
		}
		catch (std::exception& exc)
		{
#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << "invalid extended handshake: " << exc.what() << "\n";
#endif
			return;
		}

#ifdef TORRENT_VERBOSE_LOGGING
		std::stringstream ext;
		root.print(ext);
		(*m_logger) << "<== EXTENDED HANDSHAKE: \n" << ext.str();
#endif

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (extension_list_t::iterator i = m_extensions.begin()
			, end(m_extensions.end()); i != end;)
		{
			// a false return value means that the extension
			// isn't supported by the other end. So, it is removed.
			if (!(*i)->on_extension_handshake(root))
				i = m_extensions.erase(i);
			else
				++i;
		}
#endif

		// there is supposed to be a remote listen port
		if (entry* listen_port = root.find_key("p"))
		{
			if (listen_port->type() == entry::int_t)
			{
				tcp::endpoint adr(remote().address()
					, (unsigned short)listen_port->integer());
				t->get_policy().peer_from_tracker(adr, pid(), 0, 0);
			}
		}
		// there should be a version too
		// but where do we put that info?
		
		if (entry* client_info = root.find_key("v"))
		{
			if (client_info->type() == entry::string_t)
				m_client_version = client_info->string();
		}

		if (entry* reqq = root.find_key("reqq"))
		{
			if (reqq->type() == entry::int_t)
				m_max_out_request_queue = reqq->integer();
			if (m_max_out_request_queue < 1)
				m_max_out_request_queue = 1;
		}
	}

	bool bt_peer_connection::dispatch_message(int received)
	{
		INVARIANT_CHECK;

		assert(received > 0);

		// this means the connection has been closed already
		if (associated_torrent().expired()) return false;

		buffer::const_interval recv_buffer = receive_buffer();

		int packet_type = recv_buffer[0];
		if (packet_type < 0
			|| packet_type >= num_supported_messages
			|| m_message_handler[packet_type] == 0)
		{
#ifndef TORRENT_DISABLE_EXTENSIONS
			for (extension_list_t::iterator i = m_extensions.begin()
				, end(m_extensions.end()); i != end; ++i)
			{
				if ((*i)->on_unknown_message(packet_size(), packet_type
					, buffer::const_interval(recv_buffer.begin+1
					, recv_buffer.end)))
					return packet_finished();
			}
#endif

			throw protocol_error("unknown message id: "
				+ boost::lexical_cast<std::string>(packet_type)
				+ " size: " + boost::lexical_cast<std::string>(packet_size()));
		}

		assert(m_message_handler[packet_type] != 0);

		// call the correct handler for this packet type
		(this->*m_message_handler[packet_type])(received);

		return packet_finished();
	}

	void bt_peer_connection::write_keepalive()
	{
		INVARIANT_CHECK;

		char buf[] = {0,0,0,0};
		send_buffer(buf, buf + sizeof(buf));
	}

	void bt_peer_connection::write_cancel(peer_request const& r)
	{
		INVARIANT_CHECK;

		assert(associated_torrent().lock()->valid_metadata());

		char buf[] = {0,0,0,13, msg_cancel};

		buffer::interval i = allocate_send_buffer(17);

		std::copy(buf, buf + 5, i.begin);
		i.begin += 5;

		// index
		detail::write_int32(r.piece, i.begin);
		// begin
		detail::write_int32(r.start, i.begin);
		// length
		detail::write_int32(r.length, i.begin);
		assert(i.begin == i.end);

		setup_send();
	}

	void bt_peer_connection::write_request(peer_request const& r)
	{
		INVARIANT_CHECK;

		assert(associated_torrent().lock()->valid_metadata());

		char buf[] = {0,0,0,13, msg_request};

		buffer::interval i = allocate_send_buffer(17);

		std::copy(buf, buf + 5, i.begin);
		i.begin += 5;

		// index
		detail::write_int32(r.piece, i.begin);
		// begin
		detail::write_int32(r.start, i.begin);
		// length
		detail::write_int32(r.length, i.begin);
		assert(i.begin == i.end);

		setup_send();
	}

	void bt_peer_connection::write_bitfield(std::vector<bool> const& bitfield)
	{
		INVARIANT_CHECK;

		assert(!in_handshake());
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);
		assert(m_sent_bitfield == false);
		assert(t->valid_metadata());

		int num_pieces = bitfield.size();
		int lazy_pieces[50];
		int num_lazy_pieces = 0;
		int lazy_piece = 0;

		assert(t->is_seed() == (std::count(bitfield.begin(), bitfield.end(), true) == num_pieces));
		if (t->is_seed() && m_ses.settings().lazy_bitfields)
		{
			num_lazy_pieces = std::min(50, num_pieces / 10);
			if (num_lazy_pieces < 1) num_lazy_pieces = 1;
			for (int i = 0; i < num_pieces; ++i)
			{
				if (rand() % (num_pieces - i) >= num_lazy_pieces - lazy_piece) continue;
				lazy_pieces[lazy_piece++] = i;
			}
			assert(lazy_piece == num_lazy_pieces);
			lazy_piece = 0;
		}

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " ==> BITFIELD ";

		std::stringstream bitfield_string;
		for (int i = 0; i < (int)get_bitfield().size(); ++i)
		{
			if (lazy_piece < num_lazy_pieces
				&& lazy_pieces[lazy_piece] == i)
			{
				bitfield_string << "0";
				++lazy_piece;
				continue;
			}
			if (bitfield[i]) bitfield_string << "1";
			else bitfield_string << "0";
		}
		bitfield_string << "\n";
		(*m_logger) << bitfield_string.str();
		lazy_piece = 0;
#endif
		const int packet_size = (num_pieces + 7) / 8 + 5;
	
		buffer::interval i = allocate_send_buffer(packet_size);	

		detail::write_int32(packet_size - 4, i.begin);
		detail::write_uint8(msg_bitfield, i.begin);

		std::fill(i.begin, i.end, 0);
		for (int c = 0; c < num_pieces; ++c)
		{
			if (lazy_piece < num_lazy_pieces
				&& lazy_pieces[lazy_piece] == c)
			{
				++lazy_piece;
				continue;
			}
			if (bitfield[c])
				i.begin[c >> 3] |= 1 << (7 - (c & 7));
		}
		assert(i.end - i.begin == (num_pieces + 7) / 8);

#ifndef NDEBUG
		m_sent_bitfield = true;
#endif
		setup_send();

		if (num_lazy_pieces > 0)
		{
			for (int i = 0; i < num_lazy_pieces; ++i)
			{
				write_have(lazy_pieces[i]);
#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << time_now_string()
					<< " ==> HAVE    [ piece: " << lazy_pieces[i] << "]\n";
#endif
			}
		}
	}

#ifndef TORRENT_DISABLE_EXTENSIONS
	void bt_peer_connection::write_extensions()
	{
		INVARIANT_CHECK;

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << time_now_string() << " ==> EXTENSIONS\n";
#endif
		assert(m_supports_extensions);

		entry handshake(entry::dictionary_t);
		entry extension_list(entry::dictionary_t);

		handshake["m"] = extension_list;

		// only send the port in case we bade the connection
		// on incoming connections the other end already knows
		// our listen port
		if (is_local()) handshake["p"] = m_ses.listen_port();
		handshake["v"] = m_ses.settings().user_agent;
		std::string remote_address;
		std::back_insert_iterator<std::string> out(remote_address);
		detail::write_address(remote().address(), out);
		handshake["ip"] = remote_address;
		handshake["reqq"] = m_ses.settings().max_allowed_in_request_queue;

		// loop backwards, to make the first extension be the last
		// to fill in the handshake (i.e. give the first extensions priority)
		for (extension_list_t::reverse_iterator i = m_extensions.rbegin()
			, end(m_extensions.rend()); i != end; ++i)
		{
			(*i)->add_handshake(handshake);
		}

		std::vector<char> msg;
		bencode(std::back_inserter(msg), handshake);

		// make room for message
		buffer::interval i = allocate_send_buffer(6 + msg.size());
		
		// write the length of the message
		detail::write_int32((int)msg.size() + 2, i.begin);
		detail::write_uint8(msg_extended, i.begin);
		// signal handshake message
		detail::write_uint8(0, i.begin);

		std::copy(msg.begin(), msg.end(), i.begin);
		i.begin += msg.size();
		assert(i.begin == i.end);

#ifdef TORRENT_VERBOSE_LOGGING
		std::stringstream ext;
		handshake.print(ext);
		(*m_logger) << "==> EXTENDED HANDSHAKE: \n" << ext.str();
#endif

		setup_send();
	}
#endif

	void bt_peer_connection::write_choke()
	{
		INVARIANT_CHECK;

		if (is_choked()) return;
		char msg[] = {0,0,0,1,msg_choke};
		send_buffer(msg, msg + sizeof(msg));
	}

	void bt_peer_connection::write_unchoke()
	{
		INVARIANT_CHECK;

		char msg[] = {0,0,0,1,msg_unchoke};
		send_buffer(msg, msg + sizeof(msg));
	}

	void bt_peer_connection::write_interested()
	{
		INVARIANT_CHECK;

		char msg[] = {0,0,0,1,msg_interested};
		send_buffer(msg, msg + sizeof(msg));
	}

	void bt_peer_connection::write_not_interested()
	{
		INVARIANT_CHECK;

		char msg[] = {0,0,0,1,msg_not_interested};
		send_buffer(msg, msg + sizeof(msg));
	}

	void bt_peer_connection::write_have(int index)
	{
		assert(associated_torrent().lock()->valid_metadata());
		assert(index >= 0);
		assert(index < associated_torrent().lock()->torrent_file().num_pieces());
		INVARIANT_CHECK;

		const int packet_size = 9;
		char msg[packet_size] = {0,0,0,5,msg_have};
		char* ptr = msg + 5;
		detail::write_int32(index, ptr);
		send_buffer(msg, msg + packet_size);
	}

	void bt_peer_connection::write_piece(peer_request const& r, char const* buffer)
	{
		INVARIANT_CHECK;

		const int packet_size = 4 + 5 + 4 + r.length;

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		buffer::interval i = allocate_send_buffer(packet_size);
		
		detail::write_int32(packet_size-4, i.begin);
		detail::write_uint8(msg_piece, i.begin);
		detail::write_int32(r.piece, i.begin);
		detail::write_int32(r.start, i.begin);
		std::memcpy(i.begin, buffer, r.length);

		assert(i.begin + r.length == i.end);

		m_payloads.push_back(range(send_buffer_size() - r.length, r.length));
		setup_send();
	}

	namespace
	{
		struct match_peer_id
		{
			match_peer_id(peer_id const& id, peer_connection const* pc)
				: m_id(id), m_pc(pc)
			{ assert(pc); }

			bool operator()(policy::peer const& p) const
			{
				return p.connection != m_pc
					&& p.connection
					&& p.connection->pid() == m_id
					&& !p.connection->pid().is_all_zeros()
					&& p.ip.address() == m_pc->remote().address();
			}

			peer_id const& m_id;
			peer_connection const* m_pc;
		};
	}

	// --------------------------
	// RECEIVE DATA
	// --------------------------

	// throws exception when the client should be disconnected
	void bt_peer_connection::on_receive(asio::error_code const& error
		, std::size_t bytes_transferred)
	{
		INVARIANT_CHECK;

		if (error) return;
		boost::shared_ptr<torrent> t = associated_torrent().lock();

		if (in_handshake()) 
			m_statistics.received_bytes(0, bytes_transferred);
	
#ifndef TORRENT_DISABLE_ENCRYPTION
		assert(in_handshake() || !m_rc4_encrypted || m_encrypted);
		if (m_rc4_encrypted && m_encrypted)
		{
			buffer::interval wr_buf = wr_recv_buffer();
			m_RC4_handler->decrypt((wr_buf.end - bytes_transferred), bytes_transferred);
		}
#endif

		buffer::const_interval recv_buffer = receive_buffer();

#ifndef TORRENT_DISABLE_ENCRYPTION
		// m_state is set to read_pe_dhkey in initial state
		// (read_protocol_identifier) for incoming, or in constructor
		// for outgoing
		if (m_state == read_pe_dhkey)
		{
			assert (!m_encrypted);
			assert (!m_rc4_encrypted);
			assert (packet_size() == dh_key_len);
			assert (recv_buffer == receive_buffer());

			if (!packet_finished()) return;
			
			// write our dh public key. m_DH_key_exchange is
			// initialized in write_pe1_2_dhkey()
			if (!is_local())
				write_pe1_2_dhkey();
			
			// read dh key, generate shared secret
			m_DH_key_exchange->compute_secret (recv_buffer.begin); // TODO handle errors

#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << " received DH key\n";
#endif
						
			// PadA/B can be a max of 512 bytes, and 20 bytes more for
			// the sync hash (if incoming), or 8 bytes more for the
			// encrypted verification constant (if outgoing). Instead
			// of requesting the maximum possible, request the maximum
			// possible to ensure we do not overshoot the standard
			// handshake.

			if (is_local())
			{
				m_state = read_pe_syncvc;
				write_pe3_sync();

				// initial payload is the standard handshake, this is
				// always rc4 if sent here. m_rc4_encrypted is flagged
				// again according to peer selection.
				m_rc4_encrypted = true;
				m_encrypted = true;
				write_handshake();
				m_rc4_encrypted = false;
				m_encrypted = false;

				// vc,crypto_select,len(pad),pad, encrypt(handshake)
				// 8+4+2+0+handshake_len
			   	reset_recv_buffer(8+4+2+0+handshake_len);
			}
			else
			{
				// already written dh key
				m_state = read_pe_synchash;
				// synchash,skeyhash,vc,crypto_provide,len(pad),pad,encrypt(handshake)
				reset_recv_buffer(20+20+8+4+2+0+handshake_len);
			}
			assert(!packet_finished());
			return;
		}

		// cannot fall through into
		if (m_state == read_pe_synchash)
		{
			assert(!m_encrypted);
			assert(!m_rc4_encrypted);
			assert(!is_local());
			assert(recv_buffer == receive_buffer());
		   
 			if (recv_buffer.left() < 20)
			{
				if (packet_finished())
				{
					throw protocol_error ("sync hash not found");
				}
				// else 
				return;
			}

			if (!m_sync_hash.get())
			{
				assert(m_sync_bytes_read == 0);
				hasher h;

				// compute synchash (hash('req1',S))
				h.update("req1", 4);
				h.update(m_DH_key_exchange->get_secret(), dh_key_len);

				m_sync_hash.reset(new sha1_hash(h.final()));
			}

			int syncoffset = get_syncoffset((char*)m_sync_hash->begin(), 20
				, recv_buffer.begin, recv_buffer.left());

			// No sync 
			if (syncoffset == -1)
			{
				std::size_t bytes_processed = recv_buffer.left() - 20;
				m_sync_bytes_read += bytes_processed;
				if (m_sync_bytes_read >= 512)
					throw protocol_error("sync hash not found within 532 bytes");

				cut_receive_buffer(bytes_processed, std::min(packet_size(), (512+20) - m_sync_bytes_read));

				assert(!packet_finished());
				return;
			}
			// found complete sync
			else
			{
				std::size_t bytes_processed = syncoffset + 20;
#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " sync point (hash) found at offset " 
					<< m_sync_bytes_read + bytes_processed - 20 << "\n";
#endif
				m_state = read_pe_skey_vc;
				// skey,vc - 28 bytes
				m_sync_hash.reset();
				cut_receive_buffer(bytes_processed, 28);
			}
		}

		if (m_state == read_pe_skey_vc)
		{
			assert(!m_encrypted);
			assert(!m_rc4_encrypted);
			assert(!is_local());
			assert(packet_size() == 28);

			if (!packet_finished()) return;

			recv_buffer = receive_buffer();

			// only calls info_hash() on the torrent_handle's, which
			// never throws.
			session_impl::mutex_t::scoped_lock l(m_ses.m_mutex);
			
			std::vector<torrent_handle> active_torrents = m_ses.get_torrents();
			std::vector<torrent_handle>::const_iterator i;
			hasher h;
			sha1_hash skey_hash, obfs_hash;

			for (i = active_torrents.begin(); i != active_torrents.end(); ++i)
			{
				torrent_handle const& t_h = *i; // TODO possible errors
				sha1_hash const& info_hash = t_h.info_hash();
				// TODO Does info_hash need to be checked for validity?
				
				h.reset();
				h.update("req2", 4);
				h.update((char*)info_hash.begin(), 20);

			    skey_hash = h.final();
				
				h.reset();
				h.update("req3", 4);
				h.update(m_DH_key_exchange->get_secret(), dh_key_len);

				obfs_hash = h.final();
				obfs_hash ^= skey_hash;

				if (std::equal (recv_buffer.begin, recv_buffer.begin + 20,
								(char*)obfs_hash.begin()))
				{
					if (!t)
					{
						attach_to_torrent(info_hash);
						t = associated_torrent().lock();
						assert(t);
					}

					init_pe_RC4_handler(m_DH_key_exchange->get_secret(), info_hash);
#ifdef TORRENT_VERBOSE_LOGGING
					(*m_logger) << " stream key found, torrent located.\n";
#endif
					continue; // TODO Check flow control with multiple torrents
				}
			}

			if (!m_RC4_handler.get())
				throw protocol_error("invalid streamkey identifier (info hash) in encrypted handshake");

			// verify constant
			buffer::interval wr_recv_buf = wr_recv_buffer();
			m_RC4_handler->decrypt(wr_recv_buf.begin + 20, 8);
			wr_recv_buf.begin += 28;

			const char sh_vc[] = {0,0,0,0, 0,0,0,0};
			if (!std::equal(sh_vc, sh_vc+8, recv_buffer.begin + 20))
			{
				throw protocol_error("unable to verify constant");
			}

#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << " verification constant found\n";
#endif
			m_state = read_pe_cryptofield;
			reset_recv_buffer(4 + 2);
		}

		// cannot fall through into
		if (m_state == read_pe_syncvc)
		{
			assert(is_local());
			assert(!m_encrypted);
			assert(!m_rc4_encrypted);
 			assert(recv_buffer == receive_buffer());
			
			if (recv_buffer.left() < 8)
			{
				if (packet_finished())
				{
					throw protocol_error ("sync verification constant not found");
				}
				// else 
				return;
			}

			// generate the verification constant
			if (!m_sync_vc.get()) 
			{
				assert(m_sync_bytes_read == 0);

				m_sync_vc.reset (new char[8]);
				std::fill(m_sync_vc.get(), m_sync_vc.get() + 8, 0);
				m_RC4_handler->decrypt(m_sync_vc.get(), 8);
			}

			assert(m_sync_vc.get());
			int syncoffset = get_syncoffset(m_sync_vc.get(), 8
				, recv_buffer.begin, recv_buffer.left());

			// No sync 
			if (syncoffset == -1)
			{
				std::size_t bytes_processed = recv_buffer.left() - 8;
				m_sync_bytes_read += bytes_processed;
				if (m_sync_bytes_read >= 512)
					throw protocol_error("sync verification constant not found within 520 bytes");

				cut_receive_buffer(bytes_processed, std::min(packet_size(), (512+8) - m_sync_bytes_read));

				assert(!packet_finished());
				return;
			}
			// found complete sync
			else
			{
				std::size_t bytes_processed = syncoffset + 8;
#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " sync point (verification constant) found at offset " 
							<< m_sync_bytes_read + bytes_processed - 8 << "\n";
#endif
				cut_receive_buffer (bytes_processed, 4 + 2);

				// delete verification constant
				m_sync_vc.reset();
				m_state = read_pe_cryptofield;
				// fall through
			}
		}

		if (m_state == read_pe_cryptofield) // local/remote
		{
			assert(!m_encrypted);
			assert(!m_rc4_encrypted);
			assert(packet_size() == 4+2);
			
			if (!packet_finished()) return;

			buffer::interval wr_buf = wr_recv_buffer();
			m_RC4_handler->decrypt(wr_buf.begin, packet_size());

			recv_buffer = receive_buffer();
			
			int crypto_field = detail::read_int32(recv_buffer.begin);

#ifdef TORRENT_VERBOSE_LOGGING
			if (!is_local())
				(*m_logger) << " crypto provide : [ ";
			else
				(*m_logger) << " crypto select : [ ";

			if (crypto_field & 0x01)
				(*m_logger) << "plaintext ";
			if (crypto_field & 0x02)
				(*m_logger) << "rc4 ";
			(*m_logger) << "]\n";
#endif

			if (!is_local())
			{
				int crypto_select = 0;
				// select a crypto method
				switch (m_ses.get_pe_settings().allowed_enc_level)
				{
				case (pe_settings::plaintext):
				{
					if (!(crypto_field & 0x01))
						throw protocol_error("plaintext not provided");
					crypto_select = 0x01;
				}
				break;
				case (pe_settings::rc4):
				{
					if (!(crypto_field & 0x02))
						throw protocol_error("rc4 not provided");
					crypto_select = 0x02;
				}
				break;
				case (pe_settings::both):
				{
					if (m_ses.get_pe_settings().prefer_rc4)
					{
						if (crypto_field & 0x02) 
							crypto_select = 0x02;
						else if (crypto_field & 0x01)
							crypto_select = 0x01;
					}
					else
					{
						if (crypto_field & 0x01)
							crypto_select = 0x01;
						else if (crypto_field & 0x02)
							crypto_select = 0x02;
					}
					if (!crypto_select)
						throw protocol_error("rc4/plaintext not provided");
				}
				} // switch
				
				// write the pe4 step
				write_pe4_sync(crypto_select);
			}
			else // is_local()
			{
				// check if crypto select is valid
				pe_settings::enc_level const& allowed_enc_level = m_ses.get_pe_settings().allowed_enc_level;

				if (crypto_field == 0x02)
				{
					if (allowed_enc_level == pe_settings::plaintext)
						throw protocol_error("rc4 selected by peer when not provided");
					m_rc4_encrypted = true;
				}
				else if (crypto_field == 0x01)
				{
					if (allowed_enc_level == pe_settings::rc4)
						throw protocol_error("plaintext selected by peer when not provided");
					m_rc4_encrypted = false;
				}
				else
					throw protocol_error("unsupported crypto method selected by peer");
			}

			int len_pad = detail::read_int16(recv_buffer.begin);
			if (len_pad < 0 || len_pad > 512)
				throw protocol_error("invalid pad length");
			
			m_state = read_pe_pad;
			if (!is_local())
				reset_recv_buffer(len_pad + 2); // len(IA) at the end of pad
			else
			{
				if (len_pad == 0)
				{
					m_encrypted = true;
					m_state = init_bt_handshake;
				}
				else
					reset_recv_buffer(len_pad);
			}
		}

		if (m_state == read_pe_pad)
		{
			assert(!m_encrypted);
			if (!packet_finished()) return;

			int pad_size = is_local() ? packet_size() : packet_size() - 2;

			buffer::interval wr_buf = wr_recv_buffer();
			m_RC4_handler->decrypt(wr_buf.begin, packet_size());

			recv_buffer = receive_buffer();
				
			if (!is_local())
			{
				recv_buffer.begin += pad_size;
				int len_ia = detail::read_int16(recv_buffer.begin);
				
				if (len_ia < 0) throw protocol_error("invalid len_ia in handshake");

#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " len(IA) : " << len_ia << "\n";
#endif
				if (len_ia == 0)
				{
					// everything after this is Encrypt2
					m_encrypted = true;
					m_state = init_bt_handshake;
				}
				else
				{
					m_state = read_pe_ia;
					reset_recv_buffer(len_ia);
				}
			}
			else // is_local()
			{
				// everything that arrives after this is Encrypt2
				m_encrypted = true;
				m_state = init_bt_handshake;
			}
		}

		if (m_state == read_pe_ia)
		{
			assert(!is_local());
			assert(!m_encrypted);

			if (!packet_finished()) return;

			// ia is always rc4, so decrypt it
			buffer::interval wr_buf = wr_recv_buffer();
			m_RC4_handler->decrypt(wr_buf.begin, packet_size());

#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << " decrypted ia : " << packet_size() << " bytes\n";
#endif

			if (!m_rc4_encrypted)
			{
				m_RC4_handler.reset();
#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " destroyed rc4 keys\n";
#endif
			}

			// everything that arrives after this is Encrypt2
			m_encrypted = true;

			m_state = read_protocol_identifier;
			cut_receive_buffer(0, 20);
		}

		if (m_state == init_bt_handshake)
		{
			assert(m_encrypted);

			// decrypt remaining received bytes
			if (m_rc4_encrypted)
			{
				buffer::interval wr_buf = wr_recv_buffer();
				wr_buf.begin += packet_size();
				m_RC4_handler->decrypt(wr_buf.begin, wr_buf.left());
#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " decrypted remaining " << wr_buf.left() << " bytes\n";
#endif
			}
			else // !m_rc4_encrypted
			{
				m_RC4_handler.reset();
#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " destroyed rc4 keys\n";
#endif
			}

			// payload stream, start with 20 handshake bytes
			m_state = read_protocol_identifier;
			reset_recv_buffer(20);

			// encrypted portion of handshake completed, toggle
			// peer_info pe_support flag back to true
			if (is_local() &&
				m_ses.get_pe_settings().out_enc_policy == pe_settings::enabled)
			{
				policy::peer* pi = peer_info_struct();
				assert(pi);
				assert(pi->pe_support == false);
				
				pi->pe_support = !(pi->pe_support);
			}
		}

#endif // #ifndef TORRENT_DISABLE_ENCRYPTION
		
		if (m_state == read_protocol_identifier)
		{
			assert (packet_size() == 20);

			if (!packet_finished()) return;
			recv_buffer = receive_buffer();

			int packet_size = recv_buffer[0];
			const char protocol_string[] = "BitTorrent protocol";

			if (packet_size != 19 ||
				!std::equal(recv_buffer.begin + 1, recv_buffer.begin + 19, protocol_string))
			{
#ifndef TORRENT_DISABLE_ENCRYPTION
				if (!is_local() && m_ses.get_pe_settings().in_enc_policy == pe_settings::disabled)
					throw protocol_error("encrypted incoming connections disabled");

				// Don't attempt to perform an encrypted handshake
				// within an encrypted connection
				if (!m_encrypted && !is_local())
				{
#ifdef TORRENT_VERBOSE_LOGGING
 					(*m_logger) << " attempting encrypted connection\n";
#endif
 					m_state = read_pe_dhkey;
					cut_receive_buffer(0, dh_key_len);
					assert(!packet_finished());
 					return;
				}
				
				assert ((!is_local() && m_encrypted) || is_local());
#endif // #ifndef TORRENT_DISABLE_ENCRYPTION
				throw protocol_error("incorrect protocol identifier");
			}

#ifndef TORRENT_DISABLE_ENCRYPTION
			assert (m_state != read_pe_dhkey);

			if (!is_local() && 
				(m_ses.get_pe_settings().in_enc_policy == pe_settings::forced) &&
				!m_encrypted) 
				throw protocol_error("non encrypted incoming connections disabled");
#endif

#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << " BitTorrent protocol\n";
#endif

			m_state = read_info_hash;
			reset_recv_buffer(28);
		}

		// fall through
		if (m_state == read_info_hash)
		{
			assert(packet_size() == 28);

			if (!packet_finished()) return;
			recv_buffer = receive_buffer();


#ifdef TORRENT_VERBOSE_LOGGING	
			for (int i=0; i < 8; ++i)
			{
				for (int j=0; j < 8; ++j)
				{
					if (recv_buffer[i] & (0x80 >> j)) (*m_logger) << "1";
					else (*m_logger) << "0";
				}
			}
			(*m_logger) << "\n";
			if (recv_buffer[7] & 0x01)
				(*m_logger) << "supports DHT port message\n";
			if (recv_buffer[7] & 0x04)
				(*m_logger) << "supports FAST extensions\n";
			if (recv_buffer[5] & 0x10)
				(*m_logger) << "supports extensions protocol\n";
#endif

#ifndef DISABLE_EXTENSIONS
			if ((recv_buffer[5] & 0x10))
				m_supports_extensions = true;
#endif
			if (recv_buffer[7] & 0x01)
				m_supports_dht_port = true;

			// ok, now we have got enough of the handshake. Is this connection
			// attached to a torrent?
			if (!t)
			{
				// now, we have to see if there's a torrent with the
				// info_hash we got from the peer
				sha1_hash info_hash;
				std::copy(recv_buffer.begin + 8, recv_buffer.begin + 28
					, (char*)info_hash.begin());

				attach_to_torrent(info_hash);
			}
			else
			{
				// verify info hash
				if (!std::equal(recv_buffer.begin + 8, recv_buffer.begin + 28
					, (const char*)t->torrent_file().info_hash().begin()))
				{
#ifdef TORRENT_VERBOSE_LOGGING
					(*m_logger) << " received invalid info_hash\n";
#endif
					throw protocol_error("invalid info-hash in handshake");
				}

#ifdef TORRENT_VERBOSE_LOGGING
				(*m_logger) << " info_hash received\n";
#endif
			}

			t = associated_torrent().lock();
			assert(t);
			
			if (!is_local())
			{
				write_handshake();
			}

			assert(t->get_policy().has_connection(this));

			m_state = read_peer_id;
 			reset_recv_buffer(20);
		}

		// fall through
		if (m_state == read_peer_id)
		{
  			if (!t)
			{
				assert(!packet_finished()); // TODO
				return;
			}
			assert(packet_size() == 20);
			
 			if (!packet_finished()) return;
			recv_buffer = receive_buffer();

#ifdef TORRENT_VERBOSE_LOGGING
			{
				peer_id tmp;
				std::copy(recv_buffer.begin, recv_buffer.begin + 20, (char*)tmp.begin());
				std::stringstream s;
				s << "received peer_id: " << tmp << " client: " << identify_client(tmp) << "\n";
				s << "as ascii: ";
				for (peer_id::iterator i = tmp.begin(); i != tmp.end(); ++i)
				{
					if (std::isprint(*i)) s << *i;
					else s << ".";
				}
				s << "\n";
				(*m_logger) << s.str();
			}
#endif
			peer_id pid;
			std::copy(recv_buffer.begin, recv_buffer.begin + 20, (char*)pid.begin());
			set_pid(pid);
 
			if (t->settings().allow_multiple_connections_per_ip)
			{
				// now, let's see if this connection should be closed
				policy& p = t->get_policy();
				policy::iterator i = std::find_if(p.begin_peer(), p.end_peer()
					, match_peer_id(pid, this));
				if (i != p.end_peer())
				{
					assert(i->connection->pid() == pid);
					// we found another connection with the same peer-id
					// which connection should be closed in order to be
					// sure that the other end closes the same connection?
					// the peer with greatest peer-id is the one allowed to
					// initiate connections. So, if our peer-id is greater than
					// the others, we should close the incoming connection,
					// if not, we should close the outgoing one.
					if (pid < m_ses.get_peer_id() && is_local())
					{
						i->connection->disconnect();
					}
					else
					{
						throw protocol_error("duplicate peer-id, connection closed");
					}
				}
			}

			if (pid == m_ses.get_peer_id())
			{
				throw protocol_error("closing connection to ourself");
			}
 
#ifndef TORRENT_DISABLE_DHT
			if (m_supports_dht_port && m_ses.m_dht)
				write_dht_port(m_ses.get_dht_settings().service_port);
#endif

			m_client_version = identify_client(pid);
			boost::optional<fingerprint> f = client_fingerprint(pid);
			if (f && std::equal(f->name, f->name + 2, "BC"))
			{
				// if this is a bitcomet client, lower the request queue size limit
				if (m_max_out_request_queue > 50) m_max_out_request_queue = 50;
			}

			// disconnect if the peer has the same peer-id as ourself
			// since it most likely is ourself then
			if (pid == m_ses.get_peer_id())
				throw std::runtime_error("closing connection to ourself");

#ifndef TORRENT_DISABLE_EXTENSIONS
			for (extension_list_t::iterator i = m_extensions.begin()
				, end(m_extensions.end()); i != end;)
			{
				if (!(*i)->on_handshake())
				{
					i = m_extensions.erase(i);
				}
				else
				{
					++i;
				}
			}

			if (m_supports_extensions) write_extensions();
#endif

#ifdef TORRENT_VERBOSE_LOGGING
			(*m_logger) << time_now_string() << " <== HANDSHAKE\n";
#endif
			// consider this a successful connection, reset the failcount
			if (peer_info_struct()) peer_info_struct()->failcount = 0;
			
#ifndef TORRENT_DISABLE_ENCRYPTION
			// Toggle pe_support back to false if this is a
			// standard successful connection
			if (is_local() && !m_encrypted &&
				m_ses.get_pe_settings().out_enc_policy == pe_settings::enabled)
			{
				policy::peer* pi = peer_info_struct();
				assert(pi);
				assert(pi->pe_support == true);

				pi->pe_support = !(pi->pe_support);
			}
#endif

			m_state = read_packet_size;
			reset_recv_buffer(4);
			if (t->valid_metadata())
				write_bitfield(t->pieces());

			assert(!packet_finished());
			return;
		}

		// cannot fall through into
		if (m_state == read_packet_size)
		{
			// Make sure this is not fallen though into
			assert (recv_buffer == receive_buffer());

			if (!t) return;
			m_statistics.received_bytes(0, bytes_transferred);
			if (!packet_finished()) return;

			const char* ptr = recv_buffer.begin;
			int packet_size = detail::read_int32(ptr);

			// don't accept packets larger than 1 MB
			if (packet_size > 1024*1024 || packet_size < 0)
			{
				// packet too large
				throw std::runtime_error("packet > 1 MB ("
					+ boost::lexical_cast<std::string>(
					(unsigned int)packet_size) + " bytes)");
			}
					
			if (packet_size == 0)
			{
				incoming_keepalive();
				// keepalive message
				m_state = read_packet_size;
				reset_recv_buffer(4);
			}
			else
			{
				m_state = read_packet;
				reset_recv_buffer(packet_size);
			}
			assert(!packet_finished());
			return;
		}

		if (m_state == read_packet)
		{
			assert(recv_buffer == receive_buffer());
			if (!t) return;
			if (dispatch_message(bytes_transferred))
			{
				m_state = read_packet_size;
				reset_recv_buffer(4);
			}
			assert(!packet_finished());
			return;
		}
		
		assert(!packet_finished());
	}	

	// --------------------------
	// SEND DATA
	// --------------------------

	// throws exception when the client should be disconnected
	void bt_peer_connection::on_sent(asio::error_code const& error
		, std::size_t bytes_transferred)
	{
		INVARIANT_CHECK;

		if (error) return;

		// manage the payload markers
		int amount_payload = 0;
		if (!m_payloads.empty())
		{
			for (std::deque<range>::iterator i = m_payloads.begin();
				i != m_payloads.end(); ++i)
			{
				i->start -= bytes_transferred;
				if (i->start < 0)
				{
					if (i->start + i->length <= 0)
					{
						amount_payload += i->length;
					}
					else
					{
						amount_payload += -i->start;
						i->length -= -i->start;
						i->start = 0;
					}
				}
			}
		}

		// TODO: move the erasing into the loop above
		// remove all payload ranges that has been sent
		m_payloads.erase(
			std::remove_if(m_payloads.begin(), m_payloads.end(), range_below_zero)
			, m_payloads.end());

		assert(amount_payload <= (int)bytes_transferred);
		m_statistics.sent_bytes(amount_payload, bytes_transferred - amount_payload);
	}

#ifndef NDEBUG
	void bt_peer_connection::check_invariant() const
	{
#ifndef TORRENT_DISABLE_ENCRYPTION
		assert( (bool(m_state != read_pe_dhkey) || m_DH_key_exchange.get())
				|| !is_local());

		assert(!m_rc4_encrypted || m_RC4_handler.get());
#endif
		if (!m_in_constructor)
			peer_connection::check_invariant();

		if (!m_payloads.empty())
		{
			for (std::deque<range>::const_iterator i = m_payloads.begin();
				i != m_payloads.end() - 1; ++i)
			{
				assert(i->start + i->length <= (i+1)->start);
			}
		}
	}
#endif

}

