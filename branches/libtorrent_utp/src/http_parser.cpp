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

#include "libtorrent/pch.hpp"

#include <cctype>
#include <algorithm>
#include <stdlib.h>

#include "libtorrent/config.hpp"
#include "libtorrent/http_parser.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/escape_string.hpp"

using namespace libtorrent;

namespace libtorrent
{

	bool is_ok_status(int http_status)
	{
		return http_status == 206 // partial content
			|| http_status == 200 // OK
			|| (http_status >= 300 // redirect
				&& http_status < 400);
	}

	bool is_redirect(int http_status)
	{
		return http_status >= 300
			&& http_status < 400;
	}

	http_parser::http_parser()
		: m_recv_pos(0)
		, m_status_code(-1)
		, m_content_length(-1)
		, m_range_start(-1)
		, m_range_end(-1)
		, m_state(read_status)
		, m_recv_buffer(0, 0)
		, m_body_start_pos(0)
		, m_chunked_encoding(false)
		, m_finished(false)
	{}

	boost::tuple<int, int> http_parser::incoming(
		buffer::const_interval recv_buffer, bool& error)
	{
		TORRENT_ASSERT(recv_buffer.left() >= m_recv_buffer.left());
		boost::tuple<int, int> ret(0, 0);
		int start_pos = m_recv_buffer.left();

		// early exit if there's nothing new in the receive buffer
		if (start_pos == recv_buffer.left()) return ret;
		m_recv_buffer = recv_buffer;

		if (m_state == error_state)
		{
			error = true;
			return ret;
		}

		char const* pos = recv_buffer.begin + m_recv_pos;

restart_response:

		if (m_state == read_status)
		{
			TORRENT_ASSERT(!m_finished);
			char const* newline = std::find(pos, recv_buffer.end, '\n');
			// if we don't have a full line yet, wait.
			if (newline == recv_buffer.end)
			{
				boost::get<1>(ret) += m_recv_buffer.left() - start_pos;
				return ret;
			}

			if (newline == pos)
			{
				m_state = error_state;
				error = true;
				return ret;
			}

			char const* line_end = newline;
			if (pos != line_end && *(line_end - 1) == '\r') --line_end;

			char const* line = pos;
			++newline;
			int incoming = int(newline - pos);
			m_recv_pos += incoming;
			boost::get<1>(ret) += newline - (m_recv_buffer.begin + start_pos);
			pos = newline;

			m_protocol = read_until(line, ' ', line_end);
			if (m_protocol.substr(0, 5) == "HTTP/")
			{
				m_status_code = atoi(read_until(line, ' ', line_end).c_str());
				m_server_message = read_until(line, '\r', line_end);
			}
			else
			{
				m_method = m_protocol;
				std::transform(m_method.begin(), m_method.end(), m_method.begin(), &to_lower);
				// the content length is assumed to be 0 for requests
				m_content_length = 0;
				m_protocol.clear();
				m_path = read_until(line, ' ', line_end);
				m_protocol = read_until(line, ' ', line_end);
				m_status_code = 0;
			}
			m_state = read_header;
			start_pos = pos - recv_buffer.begin;
		}

		if (m_state == read_header)
		{
			TORRENT_ASSERT(!m_finished);
			char const* newline = std::find(pos, recv_buffer.end, '\n');
			std::string line;

			while (newline != recv_buffer.end && m_state == read_header)
			{
				// if the LF character is preceeded by a CR
				// charachter, don't copy it into the line string.
				char const* line_end = newline;
				if (pos != line_end && *(line_end - 1) == '\r') --line_end;
				line.assign(pos, line_end);
				++newline;
				m_recv_pos += newline - pos;
				pos = newline;

				std::string::size_type separator = line.find(':');
				if (separator == std::string::npos)
				{
					if (m_status_code == 100)
					{
						// for 100 Continue, we need to read another response header
						// before reading the body
						m_state = read_status;
						goto restart_response;
					}
					// this means we got a blank line,
					// the header is finished and the body
					// starts.
					m_state = read_body;
					// if this is a request (not a response)
					// we're done once we reach the end of the headers
					if (!m_method.empty()) m_finished = true;
					m_body_start_pos = m_recv_pos;
					break;
				}

				std::string name = line.substr(0, separator);
				std::transform(name.begin(), name.end(), name.begin(), &to_lower);
				++separator;
				// skip whitespace
				while (separator < line.size()
					&& (line[separator] == ' ' || line[separator] == '\t'))
					++separator;
				std::string value = line.substr(separator, std::string::npos);
				m_header.insert(std::make_pair(name, value));

				if (name == "content-length")
				{
					m_content_length = strtoll(value.c_str(), 0, 10);
				}
				else if (name == "content-range")
				{
					bool success = true;
					char const* ptr = value.c_str();

					// apparently some web servers do not send the "bytes"
					// in their content-range. Don't treat it as an error
					// if we can't find it, just assume the byte counters
					// start immediately
					if (string_begins_no_case("bytes ", ptr)) ptr += 6;
					char* end;
					m_range_start = strtoll(ptr, &end, 10);
					if (end == ptr) success = false;
					else if (*end != '-') success = false;
					else
					{
						ptr = end + 1;
						m_range_end = strtoll(ptr, &end, 10);
						if (end == ptr) success = false;
					}

					if (!success || m_range_end < m_range_start)
					{
						m_state = error_state;
						error = true;
						return ret;
					}
					// the http range is inclusive
					m_content_length = m_range_end - m_range_start + 1;
				}
				else if (name == "transfer-encoding")
				{
					m_chunked_encoding = string_begins_no_case("chunked", value.c_str());
				}

				TORRENT_ASSERT(m_recv_pos <= (int)recv_buffer.left());
				newline = std::find(pos, recv_buffer.end, '\n');
			}
			boost::get<1>(ret) += newline - (m_recv_buffer.begin + start_pos);
		}

		if (m_state == read_body)
		{
			int incoming = recv_buffer.end - pos;
			if (m_recv_pos - m_body_start_pos + incoming > m_content_length
				&& m_content_length >= 0)
				incoming = m_content_length - m_recv_pos + m_body_start_pos;

			TORRENT_ASSERT(incoming >= 0);
			m_recv_pos += incoming;
			boost::get<0>(ret) += incoming;

			if (m_content_length >= 0
				&& m_recv_pos - m_body_start_pos >= m_content_length)
			{
				m_finished = true;
			}
		}
		return ret;
	}
	
	bool http_parser::parse_chunk_header(buffer::const_interval buf
		, size_type* chunk_size, int* header_size)
	{
		char const* pos = buf.begin;

		// ignore one optional new-line. This is since each chunk
		// is terminated by a newline. we're likely to see one
		// before the actual header.

		if (pos[0] == '\r' && pos[1] == '\n') pos += 2;
		else if (pos[0] == '\n') pos += 1;

		char const* newline = std::find(pos, buf.end, '\n');
		if (newline == buf.end) return false;
		++newline;

		// the chunk header is a single line, a hex length of the
		// chunk followed by an optional semi-colon with a comment
		// in case the length is 0, the stream is terminated and
		// there are extra tail headers, which is terminated by an
		// empty line

		// first, read the chunk length
		*chunk_size = strtoll(pos, 0, 16);
		if (*chunk_size != 0)
		{
			*header_size = newline - buf.begin;
			// the newline alone is two bytes
			TORRENT_ASSERT(newline - buf.begin > 2);
			return true;
		}

		// this is the terminator of the stream. Also read headers
		std::map<std::string, std::string> tail_headers;
		pos = newline;
		newline = std::find(pos, buf.end, '\n');

		std::string line;
		while (newline != buf.end)
		{
			// if the LF character is preceeded by a CR
			// charachter, don't copy it into the line string.
			char const* line_end = newline;
			if (pos != line_end && *(line_end - 1) == '\r') --line_end;
			line.assign(pos, line_end);
			++newline;
			pos = newline;

			std::string::size_type separator = line.find(':');
			if (separator == std::string::npos)
			{
				// this means we got a blank line,
				// the header is finished and the body
				// starts.
				*header_size = newline - buf.begin;

				// the newline alone is two bytes
				TORRENT_ASSERT(newline - buf.begin > 2);

				// we were successfull in parsing the headers.
				// add them to the headers in the parser
				for (std::map<std::string, std::string>::const_iterator i = tail_headers.begin();
					i != tail_headers.end(); ++i)
					m_header[i->first] = i->second;

				return true;
			}

			std::string name = line.substr(0, separator);
			std::transform(name.begin(), name.end(), name.begin(), &to_lower);
			++separator;
			// skip whitespace
			while (separator < line.size()
				&& (line[separator] == ' ' || line[separator] == '\t'))
				++separator;
			std::string value = line.substr(separator, std::string::npos);
			tail_headers.insert(std::make_pair(name, value));

			newline = std::find(pos, buf.end, '\n');
		}
		return false;
	}

	buffer::const_interval http_parser::get_body() const
	{
		TORRENT_ASSERT(m_state == read_body);
		if (m_content_length >= 0)
			return buffer::const_interval(m_recv_buffer.begin + m_body_start_pos
				, m_recv_buffer.begin + (std::min)(size_type(m_recv_pos)
				, m_body_start_pos + m_content_length));
		else
			return buffer::const_interval(m_recv_buffer.begin + m_body_start_pos
				, m_recv_buffer.begin + m_recv_pos);
	}
	
	void http_parser::reset()
	{
		m_recv_pos = 0;
		m_body_start_pos = 0;
		m_status_code = -1;
		m_content_length = -1;
		m_range_start = -1;
		m_range_end = -1;
		m_finished = false;
		m_state = read_status;
		m_recv_buffer.begin = 0;
		m_recv_buffer.end = 0;
		m_header.clear();
	}
	
}

