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

#include "libtorrent/pch.hpp"

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

#include "libtorrent/piece_picker.hpp"
#include "libtorrent/aux_/session_impl.hpp"
#include "libtorrent/bitfield.hpp"

#ifndef NDEBUG
#include "libtorrent/peer_connection.hpp"
#include "libtorrent/torrent.hpp"
#endif

//#define TORRENT_PIECE_PICKER_INVARIANT_CHECK INVARIANT_CHECK
//#define TORRENT_NO_EXPENSIVE_INVARIANT_CHECK
#define TORRENT_PIECE_PICKER_INVARIANT_CHECK

//#define TORRENT_PICKER_LOG

namespace libtorrent
{

	piece_picker::piece_picker()
		: m_seeds(0)
		, m_priority_boundries(1, int(m_pieces.size()))
		, m_num_filtered(0)
		, m_num_have_filtered(0)
		, m_num_have(0)
		, m_sequential_download(-1)
		, m_dirty(false)
	{
#ifdef TORRENT_PICKER_LOG
		std::cerr << "new piece_picker" << std::endl;
#endif
#ifndef NDEBUG
		check_invariant();
#endif
	}

	void piece_picker::init(int blocks_per_piece, int total_num_blocks)
	{
		TORRENT_ASSERT(blocks_per_piece > 0);
		TORRENT_ASSERT(total_num_blocks >= 0);

		// allocate the piece_map to cover all pieces
		// and make them invalid (as if we don't have a single piece)
		m_piece_map.resize((total_num_blocks + blocks_per_piece-1) / blocks_per_piece
			, piece_pos(0, 0));

		// the piece index is stored in 20 bits, which limits the allowed
		// number of pieces somewhat
		if (m_piece_map.size() >= piece_pos::we_have_index)
			throw std::runtime_error("too many pieces in torrent");
		
		m_blocks_per_piece = blocks_per_piece;
		m_blocks_in_last_piece = total_num_blocks % blocks_per_piece;
		if (m_blocks_in_last_piece == 0) m_blocks_in_last_piece = blocks_per_piece;

		TORRENT_ASSERT(m_blocks_in_last_piece <= m_blocks_per_piece);
	}

	void piece_picker::sequential_download(bool sd)
	{
		if (sd == sequential_download()) return;

		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		if (sd)
		{
			std::vector<int>().swap(m_pieces);
			std::vector<int>().swap(m_priority_boundries);
		
			// initialize m_sdquential_download
			m_sequential_download = 0;
			for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin()
				, end(m_piece_map.end()); i != end && (i->have() || i->filtered());
				++i, ++m_sequential_download);
		}
		else
		{
			m_sequential_download = -1;
			m_dirty = true;
		}
	}

	void piece_picker::piece_info(int index, piece_picker::downloading_piece& st) const
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < int(m_piece_map.size()));

		if (m_piece_map[index].downloading)
		{
			std::vector<downloading_piece>::const_iterator piece = std::find_if(
				m_downloads.begin(), m_downloads.end()
				, bind(&downloading_piece::index, _1) == index);
			TORRENT_ASSERT(piece != m_downloads.end());
			st = *piece;
			st.info = 0;
			return;
		}
		st.info = 0;
		st.index = index;
		st.writing = 0;
		st.requested = 0;
		if (m_piece_map[index].have())
		{
			st.finished = blocks_in_piece(index);
			return;
		}
		st.finished = 0;
	}

	piece_picker::downloading_piece& piece_picker::add_download_piece()
	{
		int num_downloads = m_downloads.size();
		int block_index = num_downloads * m_blocks_per_piece;
		if (int(m_block_info.size()) < block_index + m_blocks_per_piece)
		{
			block_info* base = 0;
			if (!m_block_info.empty()) base = &m_block_info[0];
			m_block_info.resize(block_index + m_blocks_per_piece);
			if (!m_downloads.empty() && &m_block_info[0] != base)
			{
				// this means the memory was reallocated, update the pointers
				for (int i = 0; i < int(m_downloads.size()); ++i)
					m_downloads[i].info = &m_block_info[m_downloads[i].info - base];
			}
		}
		m_downloads.push_back(downloading_piece());
		downloading_piece& ret = m_downloads.back();
		ret.info = &m_block_info[block_index];
		for (int i = 0; i < m_blocks_per_piece; ++i)
		{
			ret.info[i].num_peers = 0;
			ret.info[i].state = block_info::state_none;
			ret.info[i].peer = 0;
		}
		return ret;
	}

	void piece_picker::erase_download_piece(std::vector<downloading_piece>::iterator i)
	{
		std::vector<downloading_piece>::iterator other = std::find_if(
			m_downloads.begin(), m_downloads.end()
			, bind(&downloading_piece::info, _1)
			== &m_block_info[(m_downloads.size() - 1) * m_blocks_per_piece]);
		TORRENT_ASSERT(other != m_downloads.end());

		if (i != other)
		{
			std::copy(other->info, other->info + m_blocks_per_piece, i->info);
			other->info = i->info;
		}
		m_downloads.erase(i);
	}

#ifndef NDEBUG

	void piece_picker::verify_pick(std::vector<piece_block> const& picked
		, bitfield const& bits) const
	{
		TORRENT_ASSERT(bits.size() == m_piece_map.size());
		for (std::vector<piece_block>::const_iterator i = picked.begin()
			, end(picked.end()); i != end; ++i)
		{
			TORRENT_ASSERT(i->piece_index >= 0);
			TORRENT_ASSERT(i->piece_index < int(bits.size()));
			TORRENT_ASSERT(bits[i->piece_index]);
			TORRENT_ASSERT(!m_piece_map[i->piece_index].have());
		}
	}
	
	void piece_picker::verify_priority(int range_start, int range_end, int prio) const
	{
		TORRENT_ASSERT(range_start <= range_end);
		TORRENT_ASSERT(range_end <= int(m_pieces.size()));
		for (std::vector<int>::const_iterator i = m_pieces.begin() + range_start
			, end(m_pieces.begin() + range_end); i != end; ++i)
		{
			int index = *i;
			TORRENT_ASSERT(index >= 0);
			TORRENT_ASSERT(index < int(m_piece_map.size()));
			int p = m_piece_map[index].priority(this);
			TORRENT_ASSERT(p == prio);
		}
	}

#ifdef TORRENT_PICKER_LOG
	void piece_picker::print_pieces() const
	{
		for (std::vector<int>::const_iterator i = m_priority_boundries.begin()
			, end(m_priority_boundries.end()); i != end; ++i)
		{
			std::cerr << *i << " ";
		}
		std::cout << std::endl;
		int index = 0;
		std::vector<int>::const_iterator j = m_priority_boundries.begin();
		for (std::vector<int>::const_iterator i = m_pieces.begin()
			, end(m_pieces.end()); i != end; ++i, ++index)
		{
			if (*i == -1) break;
			while (j != m_priority_boundries.end() && *j <= index)
			{
				std::cerr << "| ";
				++j;
			}
			std::cerr << *i << "(" << m_piece_map[*i].index << ") ";
		}
		std::cerr << std::endl;
	}
#endif

	void piece_picker::check_invariant(const torrent* t) const
	{
		TORRENT_ASSERT(sizeof(piece_pos) == 4);
		TORRENT_ASSERT(m_num_have >= 0);
		TORRENT_ASSERT(m_num_have_filtered >= 0);
		TORRENT_ASSERT(m_num_filtered >= 0);
		TORRENT_ASSERT(m_seeds >= 0);

		if (!m_downloads.empty())
		{
			for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin();
				i != m_downloads.end() - 1; ++i)
			{
				downloading_piece const& dp = *i;
				downloading_piece const& next = *(i + 1);
				TORRENT_ASSERT(dp.finished + dp.writing >= next.finished + next.writing);
			}
		}

		if (t != 0)
			TORRENT_ASSERT((int)m_piece_map.size() == t->torrent_file().num_pieces());

		for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin()
			, end(m_downloads.end()); i != end; ++i)
		{
			bool blocks_requested = false;
			int num_blocks = blocks_in_piece(i->index);
			int num_requested = 0;
			int num_finished = 0;
			int num_writing = 0;
			for (int k = 0; k < num_blocks; ++k)
			{
				if (i->info[k].state == block_info::state_finished)
				{
					++num_finished;
					TORRENT_ASSERT(i->info[k].num_peers == 0);
				}
				else if (i->info[k].state == block_info::state_requested)
				{
					++num_requested;
					blocks_requested = true;
					TORRENT_ASSERT(i->info[k].num_peers > 0);
				}
				else if (i->info[k].state == block_info::state_writing)
				{
					++num_writing;
					TORRENT_ASSERT(i->info[k].num_peers == 0);
				}
			}
			TORRENT_ASSERT(blocks_requested == (i->state != none));
			TORRENT_ASSERT(num_requested == i->requested);
			TORRENT_ASSERT(num_writing == i->writing);
			TORRENT_ASSERT(num_finished == i->finished);
		}
#ifdef TORRENT_NO_EXPENSIVE_INVARIANT_CHECK
		return;
#endif

		if (m_sequential_download == -1 && !m_dirty)
		{
			TORRENT_ASSERT(!m_priority_boundries.empty());
			int prio = 0;
			int start = 0;
			for (std::vector<int>::const_iterator i = m_priority_boundries.begin()
				, end(m_priority_boundries.end()); i != end; ++i)
			{
				verify_priority(start, *i, prio);
				++prio;
				start = *i;
			}
			TORRENT_ASSERT(m_priority_boundries.back() == int(m_pieces.size()));
		}
		else if (m_sequential_download >= 0)
		{
			int index = 0;
			for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin()
				, end(m_piece_map.end()); i != end && (i->have() || i->filtered());
				++i, ++index);
			TORRENT_ASSERT(m_sequential_download == index);
		}

		int num_filtered = 0;
		int num_have_filtered = 0;
		int num_have = 0;
		for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin();
			i != m_piece_map.end(); ++i)
		{
			int index = static_cast<int>(i - m_piece_map.begin());
			piece_pos const& p = *i;

			if (p.filtered())
			{
				if (p.index != piece_pos::we_have_index)
					++num_filtered;
				else
					++num_have_filtered;
			}
			if (p.index == piece_pos::we_have_index)
				++num_have;

#if 0
			if (t != 0)
			{
				int actual_peer_count = 0;
				for (torrent::const_peer_iterator peer = t->begin();
					peer != t->end(); ++peer)
				{
					if (peer->second->has_piece(index)) actual_peer_count++;
				}

				TORRENT_ASSERT((int)i->peer_count == actual_peer_count);
/*
				int num_downloaders = 0;
				for (std::vector<peer_connection*>::const_iterator peer = t->begin();
					peer != t->end();
					++peer)
				{
					const std::vector<piece_block>& queue = (*peer)->download_queue();
					if (std::find_if(queue.begin(), queue.end(), has_index(index)) == queue.end()) continue;

					++num_downloaders;
				}

				if (i->downloading)
				{
					TORRENT_ASSERT(num_downloaders == 1);
				}
				else
				{
					TORRENT_ASSERT(num_downloaders == 0);
				}
*/
			}
#endif

			if (p.index == piece_pos::we_have_index)
			{
				TORRENT_ASSERT(t == 0 || t->have_piece(index));
				TORRENT_ASSERT(p.downloading == 0);
			}

			if (t != 0)
				TORRENT_ASSERT(!t->have_piece(index));

			if (m_sequential_download == -1 && !m_dirty)
			{
				int prio = p.priority(this);
				TORRENT_ASSERT(prio < int(m_priority_boundries.size())
					|| m_dirty);
				if (prio >= 0)
				{
					TORRENT_ASSERT(p.index < m_pieces.size());
					TORRENT_ASSERT(m_pieces[p.index] == index);
				}
				else
				{
					TORRENT_ASSERT(prio == -1);
					// make sure there's no entry
					// with this index. (there shouldn't
					// be since the priority is -1)
					TORRENT_ASSERT(std::find(m_pieces.begin(), m_pieces.end(), index)
						== m_pieces.end());
				}
			}
			else if (m_sequential_download >= 0)
			{
				TORRENT_ASSERT(m_pieces.empty());
				TORRENT_ASSERT(m_priority_boundries.empty());
			}

			int count = std::count_if(m_downloads.begin(), m_downloads.end()
				, has_index(index));
			if (i->downloading == 1)
			{
				TORRENT_ASSERT(count == 1);
			}
			else
			{
				TORRENT_ASSERT(count == 0);
			}
		}
		TORRENT_ASSERT(num_have == m_num_have);
		TORRENT_ASSERT(num_filtered == m_num_filtered);
		TORRENT_ASSERT(num_have_filtered == m_num_have_filtered);

		if (!m_dirty)
		{
			for (std::vector<int>::const_iterator i = m_pieces.begin()
				, end(m_pieces.end()); i != end; ++i)
			{
				TORRENT_ASSERT(m_piece_map[*i].priority(this) >= 0);
			}
		}
	}
#endif

	float piece_picker::distributed_copies() const
	{
		TORRENT_ASSERT(m_seeds >= 0);
		const float num_pieces = static_cast<float>(m_piece_map.size());

		int min_availability = piece_pos::max_peer_count;
		// find the lowest availability count
		// count the number of pieces that have that availability
		// and also the number of pieces that have more than that.
		int integer_part = 0;
		int fraction_part = 0;
		for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin()
			, end(m_piece_map.end()); i != end; ++i)
		{
			int peer_count = int(i->peer_count);
			// take ourself into account
			if (i->have()) ++peer_count;
			if (min_availability > peer_count)
			{
				min_availability = peer_count;
				fraction_part += integer_part;
				integer_part = 1;
			}
			else if (peer_count == min_availability)
			{
				++integer_part;
			}
			else
			{
				TORRENT_ASSERT(peer_count > min_availability);
				++fraction_part;
			}
		}
		TORRENT_ASSERT(integer_part + fraction_part == num_pieces);
		return float(min_availability + m_seeds) + (fraction_part / num_pieces);
	}

	void piece_picker::priority_range(int prio, int* start, int* end)
	{
		TORRENT_ASSERT(prio >= 0);
		TORRENT_ASSERT(prio < int(m_priority_boundries.size())
			|| m_dirty);
		if (prio == 0) *start = 0;
		else *start = m_priority_boundries[prio - 1];
		*end = m_priority_boundries[prio];
		TORRENT_ASSERT(*start <= *end);
	}

	void piece_picker::add(int index)
	{
		TORRENT_ASSERT(!m_dirty);
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < int(m_piece_map.size()));
		piece_pos& p = m_piece_map[index];
		TORRENT_ASSERT(!p.filtered());
		TORRENT_ASSERT(!p.have());
		TORRENT_ASSERT(m_sequential_download == -1);

		int priority = p.priority(this);
		TORRENT_ASSERT(priority >= 0);
		if (int(m_priority_boundries.size()) <= priority)
			m_priority_boundries.resize(priority + 1, m_pieces.size());

		TORRENT_ASSERT(int(m_priority_boundries.size()) >= priority);

		int range_start, range_end;
		priority_range(priority, &range_start, &range_end);
		int new_index;
		if (range_end == range_start) new_index = range_start;
		else new_index = rand() % (range_end - range_start + 1) + range_start;

#ifdef TORRENT_PICKER_LOG
		std::cerr << "add " << index << " (" << priority << ")" << std::endl;
		print_pieces();
#endif
		m_pieces.push_back(-1);

		for (;;)
		{
			TORRENT_ASSERT(new_index < int(m_pieces.size()));
			int temp = m_pieces[new_index];
			m_pieces[new_index] = index;
			m_piece_map[index].index = new_index;
			index = temp;
			do
			{
				temp = m_priority_boundries[priority]++;
				++priority;
			} while (temp == new_index && priority < int(m_priority_boundries.size()));
			new_index = temp;
#ifdef TORRENT_PICKER_LOG
			print_pieces();
			std::cerr << " index: " << index
				<< " prio: " << priority
				<< " new_index: " << new_index
				<< std::endl;
#endif
			if (priority >= int(m_priority_boundries.size())) break;
			TORRENT_ASSERT(temp >= 0);
		}
		if (index != -1)
		{
			TORRENT_ASSERT(new_index == int(m_pieces.size() - 1));
			m_pieces[new_index] = index;
			m_piece_map[index].index = new_index;

#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
		}
	}

	void piece_picker::remove(int priority, int elem_index)
	{
		TORRENT_ASSERT(!m_dirty);
		TORRENT_ASSERT(priority >= 0);
		TORRENT_ASSERT(m_sequential_download == -1);

#ifdef TORRENT_PICKER_LOG
		std::cerr << "remove " << m_pieces[elem_index] << " (" << priority << ")" << std::endl;
#endif
		int next_index = elem_index;
		TORRENT_ASSERT(m_piece_map[m_pieces[elem_index]].priority(this) == -1);
		for (;;)
		{
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			TORRENT_ASSERT(elem_index < int(m_pieces.size()));
			int temp;
			do
			{
				temp = --m_priority_boundries[priority];
				++priority;
			} while (next_index == temp && priority < int(m_priority_boundries.size()));
			if (next_index == temp) break;
			next_index = temp;

			int piece = m_pieces[next_index];
			m_pieces[elem_index] = piece;
			m_piece_map[piece].index = elem_index;
			TORRENT_ASSERT(m_piece_map[piece].priority(this) == priority - 1);
			TORRENT_ASSERT(elem_index < int(m_pieces.size() - 1));
			elem_index = next_index;

			if (priority == int(m_priority_boundries.size()))
				break;
		}
		m_pieces.pop_back();
		TORRENT_ASSERT(next_index == int(m_pieces.size()));
#ifdef TORRENT_PICKER_LOG
		print_pieces();
#endif
	}

	// will update the piece with the given properties (priority, elem_index)
	// to place it at the correct position
	void piece_picker::update(int priority, int elem_index)
	{
		TORRENT_ASSERT(!m_dirty);
		TORRENT_ASSERT(priority >= 0);
		TORRENT_ASSERT(elem_index >= 0);
		TORRENT_ASSERT(m_sequential_download == -1);

		TORRENT_ASSERT(int(m_priority_boundries.size()) > priority);

		int index = m_pieces[elem_index];
		// update the piece_map
		piece_pos& p = m_piece_map[index];
		TORRENT_ASSERT(int(p.index) == elem_index || p.have());		

		int new_priority = p.priority(this);

		if (new_priority == priority) return;

		if (new_priority == -1)
		{
			remove(priority, elem_index);
			return;
		}

		if (int(m_priority_boundries.size()) <= new_priority)
			m_priority_boundries.resize(new_priority + 1, m_pieces.size());

#ifdef TORRENT_PICKER_LOG
		std::cerr << "update " << index << " (" << priority << "->" << new_priority << ")" << std::endl;
#endif
		if (priority > new_priority)
		{
			int new_index;
			int temp = index;
			for (;;)
			{
#ifdef TORRENT_PICKER_LOG
				print_pieces();
#endif
				--priority;
				new_index = m_priority_boundries[priority]++;
				TORRENT_ASSERT(new_index < int(m_pieces.size()));
				if (temp != m_pieces[new_index])
				{
					temp = m_pieces[new_index];
					m_pieces[elem_index] = temp;
					m_piece_map[temp].index = elem_index;
					TORRENT_ASSERT(elem_index < int(m_pieces.size()));
				}
				elem_index = new_index;
				if (priority == new_priority) break;
			}
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			m_pieces[elem_index] = index;
			m_piece_map[index].index = elem_index;
			TORRENT_ASSERT(elem_index < int(m_pieces.size()));
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			shuffle(priority, elem_index);
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			TORRENT_ASSERT(m_piece_map[index].priority(this) == priority);
		}
		else
		{
			int new_index;
			int temp = index;
			for (;;)
			{
#ifdef TORRENT_PICKER_LOG
				print_pieces();
#endif
				new_index = --m_priority_boundries[priority];
				TORRENT_ASSERT(new_index < int(m_pieces.size()));
				if (temp != m_pieces[new_index])
				{
					temp = m_pieces[new_index];
					m_pieces[elem_index] = temp;
					m_piece_map[temp].index = elem_index;
					TORRENT_ASSERT(elem_index < int(m_pieces.size()));
				}
				elem_index = new_index;
				++priority;
				if (priority == new_priority) break;
			}
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			m_pieces[elem_index] = index;
			m_piece_map[index].index = elem_index;
			TORRENT_ASSERT(elem_index < int(m_pieces.size()));
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			shuffle(priority, elem_index);
#ifdef TORRENT_PICKER_LOG
			print_pieces();
#endif
			TORRENT_ASSERT(m_piece_map[index].priority(this) == priority);
		}
	}

	void piece_picker::shuffle(int priority, int elem_index)
	{
		TORRENT_ASSERT(!m_dirty);
		TORRENT_ASSERT(priority >= 0);
		TORRENT_ASSERT(elem_index >= 0);
		TORRENT_ASSERT(m_sequential_download == -1);
		
		int range_start, range_end;
		priority_range(priority, &range_start, &range_end);
		TORRENT_ASSERT(range_start < range_end);
		int other_index = rand() % (range_end - range_start) + range_start;

		if (other_index == elem_index) return;

		// swap other_index with elem_index
		piece_pos& p1 = m_piece_map[m_pieces[other_index]];
		piece_pos& p2 = m_piece_map[m_pieces[elem_index]];

		int temp = p1.index;
		p1.index = p2.index;
		p2.index = temp;
		std::swap(m_pieces[other_index], m_pieces[elem_index]);
	}

	void piece_picker::sort_piece(std::vector<downloading_piece>::iterator dp)
	{
		TORRENT_ASSERT(m_piece_map[dp->index].downloading);
		if (dp == m_downloads.begin()) return;
		int complete = dp->writing + dp->finished;
		for (std::vector<downloading_piece>::iterator i = dp, j(dp-1);
			i != m_downloads.begin(); --i, --j)
		{
			TORRENT_ASSERT(j >= m_downloads.begin());
			if (j->finished + j->writing >= complete) return;
			using std::swap;
			swap(*j, *i);
			if (j == m_downloads.begin()) break;
		}
	}

	void piece_picker::restore_piece(int index)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < (int)m_piece_map.size());

		TORRENT_ASSERT(m_piece_map[index].downloading == 1);

		std::vector<downloading_piece>::iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end()
			, has_index(index));

		TORRENT_ASSERT(i != m_downloads.end());
		erase_download_piece(i);

		piece_pos& p = m_piece_map[index];
		int prev_priority = p.priority(this);
		p.downloading = 0;
		int new_priority = p.priority(this);

		if (new_priority == prev_priority) return;
		if (m_dirty) return;
		if (m_sequential_download >= 0)
		{
			m_dirty = true;
			return;
		}
		if (prev_priority == -1)
		{
			add(index);
		}
		else
		{
			update(prev_priority, p.index);
		}
	}

	void piece_picker::inc_refcount_all()
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		++m_seeds;
		if (m_seeds == 1)
		{
			// when m_seeds is increased from 0 to 1
			// we may have to add pieces that previously
			// didn't have any peers
			m_dirty = true;
		}
	}

	void piece_picker::dec_refcount_all()
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		if (m_seeds > 0)
		{
			--m_seeds;
			if (m_seeds == 0)
			{
				// when m_seeds is decreased from 1 to 0
				// we may have to remove pieces that previously
				// didn't have any peers
				m_dirty = true;
			}
			return;
		}
		TORRENT_ASSERT(m_seeds == 0);

		for (std::vector<piece_pos>::iterator i = m_piece_map.begin()
			, end(m_piece_map.end()); i != end; ++i)
		{
			TORRENT_ASSERT(i->peer_count > 0);
			--i->peer_count;
		}

		m_dirty = true;
	}

	void piece_picker::inc_refcount(int index)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		piece_pos& p = m_piece_map[index];
		if (m_sequential_download >= 0)
		{
			++p.peer_count;
			m_dirty = true;
			return;
		}
	
		int prev_priority = p.priority(this);
		++p.peer_count;
		if (m_dirty) return;
		int new_priority = p.priority(this);
		if (prev_priority == new_priority) return;
		if (prev_priority == -1)
			add(index);
		else
			update(prev_priority, p.index);
	}

	void piece_picker::dec_refcount(int index)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		piece_pos& p = m_piece_map[index];
		if (m_sequential_download >= 0)
		{
			TORRENT_ASSERT(p.peer_count > 0);
			--p.peer_count;
			m_dirty = true;
			return;
		}
		int prev_priority = p.priority(this);
		TORRENT_ASSERT(p.peer_count > 0);
		--p.peer_count;
		if (m_dirty) return;
		if (prev_priority >= 0) update(prev_priority, p.index);
	}

	void piece_picker::inc_refcount(bitfield const& bitmask)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		TORRENT_ASSERT(bitmask.size() == m_piece_map.size());

		int index = 0;
		bool updated = false;
		for (bitfield::const_iterator i = bitmask.begin()
			, end(bitmask.end()); i != end; ++i, ++index)
		{
			if (*i)
			{
				++m_piece_map[index].peer_count;
				updated = true;
			}
		}

		if (updated && m_sequential_download == -1) m_dirty = true;
	}

	void piece_picker::dec_refcount(bitfield const& bitmask)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		TORRENT_ASSERT(bitmask.size() == m_piece_map.size());

		int index = 0;
		bool updated = false;
		for (bitfield::const_iterator i = bitmask.begin()
			, end(bitmask.end()); i != end; ++i, ++index)
		{
			if (*i)
			{
				--m_piece_map[index].peer_count;
				updated = true;
			}
		}

		if (updated && m_sequential_download == -1) m_dirty = true;
	}

	void piece_picker::update_pieces() const
	{
		TORRENT_ASSERT(m_dirty);
		TORRENT_ASSERT(m_sequential_download == -1);
		if (m_priority_boundries.empty()) m_priority_boundries.resize(1, 0);
#ifdef TORRENT_PICKER_LOG
		std::cerr << "update_pieces" << std::endl;
#endif
		std::fill(m_priority_boundries.begin(), m_priority_boundries.end(), 0);
		for (std::vector<piece_pos>::iterator i = m_piece_map.begin()
			, end(m_piece_map.end()); i != end; ++i)
		{
			int prio = i->priority(this);
			if (prio == -1) continue;
			if (prio >= int(m_priority_boundries.size()))
				m_priority_boundries.resize(prio + 1, 0);
			i->index = m_priority_boundries[prio];
			++m_priority_boundries[prio];
		}

#ifdef TORRENT_PICKER_LOG
		print_pieces();
#endif

		int index = 0;
		for (std::vector<int>::iterator i = m_priority_boundries.begin()
			, end(m_priority_boundries.end()); i != end; ++i)
		{
			*i += index;
			index = *i;
		}
		m_pieces.resize(index, 0);

#ifdef TORRENT_PICKER_LOG
		print_pieces();
#endif

		index = 0;
		for (std::vector<piece_pos>::iterator i = m_piece_map.begin()
			, end(m_piece_map.end()); i != end; ++i, ++index)
		{
			piece_pos& p = *i;
			int prio = p.priority(this);
			if (prio == -1) continue;
			int new_index = (prio == 0 ? 0 : m_priority_boundries[prio - 1]) + p.index;
			m_pieces[new_index] = index;
		}

		int start = 0;
		for (std::vector<int>::iterator i = m_priority_boundries.begin()
			, end(m_priority_boundries.end()); i != end; ++i)
		{
			if (start == *i) continue;
			std::random_shuffle(&m_pieces[0] + start, &m_pieces[0] + *i);
			start = *i;
		}

		index = 0;
		for (std::vector<int>::const_iterator i = m_pieces.begin()
			, end(m_pieces.end()); i != end; ++i, ++index)
		{
			TORRENT_ASSERT(*i >= 0 && *i < int(m_piece_map.size()));
			m_piece_map[*i].index = index;
		}

		m_dirty = false;
#ifdef TORRENT_PICKER_LOG
		print_pieces();
#endif

#ifndef NDEBUG
		check_invariant();
#endif
	}

	void piece_picker::we_dont_have(int index)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < (int)m_piece_map.size());

		piece_pos& p = m_piece_map[index];
		TORRENT_ASSERT(p.downloading == 0);

		if (!p.have()) return;

		if (m_sequential_download > index)
			m_sequential_download = index;

		if (p.filtered())
		{
			++m_num_filtered;
			--m_num_have_filtered;
		}

		--m_num_have;
		p.set_not_have();

		if (m_dirty) return;
		if (p.priority(this) >= 0) add(index);
	}

	// this is used to indicate that we succesfully have
	// downloaded a piece, and that no further attempts
	// to pick that piece should be made. The piece will
	// be removed from the available piece list.
	void piece_picker::we_have(int index)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < (int)m_piece_map.size());

		piece_pos& p = m_piece_map[index];
		int info_index = p.index;
		int priority = p.priority(this);
		TORRENT_ASSERT(priority < int(m_priority_boundries.size()));

		if (p.downloading)
		{
			std::vector<downloading_piece>::iterator i
				= std::find_if(m_downloads.begin()
				, m_downloads.end()
				, has_index(index));
			TORRENT_ASSERT(i != m_downloads.end());
			erase_download_piece(i);
			p.downloading = 0;
		}

		TORRENT_ASSERT(std::find_if(m_downloads.begin(), m_downloads.end()
			, has_index(index)) == m_downloads.end());

		if (m_sequential_download == index)
		{
			++m_sequential_download;
			for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin() + m_sequential_download
				, end(m_piece_map.end()); i != end && (i->have() || i->filtered());
				++i, ++m_sequential_download);
		}
		if (p.have()) return;
		if (p.filtered())
		{
			--m_num_filtered;
			++m_num_have_filtered;
		}
		++m_num_have;
		p.set_have();
		if (priority == -1) return;
		if (m_dirty) return;
		remove(priority, info_index);
		TORRENT_ASSERT(p.priority(this) == -1);
	}

	bool piece_picker::set_piece_priority(int index, int new_piece_priority)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		TORRENT_ASSERT(new_piece_priority >= 0);
		TORRENT_ASSERT(new_piece_priority <= 7);
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < (int)m_piece_map.size());
		
		piece_pos& p = m_piece_map[index];

		// if the priority isn't changed, don't do anything
		if (new_piece_priority == int(p.piece_priority)) return false;
		
		int prev_priority = p.priority(this);
		TORRENT_ASSERT(prev_priority < int(m_priority_boundries.size()));

		bool ret = false;
		if (new_piece_priority == piece_pos::filter_priority
			&& p.piece_priority != piece_pos::filter_priority)
		{
			// the piece just got filtered
			if (p.have()) ++m_num_have_filtered;
			else ++m_num_filtered;
			ret = true;
		}
		else if (new_piece_priority != piece_pos::filter_priority
			&& p.piece_priority == piece_pos::filter_priority)
		{
			// the piece just got unfiltered
			if (p.have()) --m_num_have_filtered;
			else --m_num_filtered;
			ret = true;
		}
		TORRENT_ASSERT(m_num_filtered >= 0);
		TORRENT_ASSERT(m_num_have_filtered >= 0);
		
		p.piece_priority = new_piece_priority;
		int new_priority = p.priority(this);

		if (prev_priority == new_priority) return false;

		TORRENT_ASSERT(prev_priority < int(m_priority_boundries.size()));

		if (m_dirty) return ret;
		if (prev_priority == -1)
		{
			add(index);
		}
		else
		{
			update(prev_priority, p.index);
		}
		return ret;
	}

	int piece_picker::piece_priority(int index) const
	{
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < (int)m_piece_map.size());

		return m_piece_map[index].piece_priority;
	}

	void piece_picker::piece_priorities(std::vector<int>& pieces) const
	{
		pieces.resize(m_piece_map.size());
		std::vector<int>::iterator j = pieces.begin();
		for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin(),
			end(m_piece_map.end()); i != end; ++i, ++j)
		{
			*j = i->piece_priority;
		}
	}

	// ============ start deprecation ==============

	void piece_picker::filtered_pieces(std::vector<bool>& mask) const
	{
		mask.resize(m_piece_map.size());
		std::vector<bool>::iterator j = mask.begin();
		for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin(),
			end(m_piece_map.end()); i != end; ++i, ++j)
		{
			*j = i->filtered();
		}
	}

	// ============ end deprecation ==============

	// pieces describes which pieces the peer we're requesting from
	// has.
	// interesting_blocks is an out parameter, and will be filled
	// with (up to) num_blocks of interesting blocks that the peer has.
	// prefer_whole_pieces can be set if this peer should download
	// whole pieces rather than trying to download blocks from the
	// same piece as other peers.
	//	the void* is the pointer to the policy::peer of the peer we're
	// picking pieces from. This is used when downloading whole pieces,
	// to only pick from the same piece the same peer is downloading
	// from. state is supposed to be set to fast if the peer is downloading
	// relatively fast, by some notion. Slow peers will prefer not
	// to pick blocks from the same pieces as fast peers, and vice
	// versa. Downloading pieces are marked as being fast, medium
	// or slow once they're started.
	void piece_picker::pick_pieces(bitfield const& pieces
		, std::vector<piece_block>& interesting_blocks
		, int num_blocks, int prefer_whole_pieces
		, void* peer, piece_state_t speed, bool rarest_first
		, bool on_parole, std::vector<int> const& suggested_pieces) const
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
		TORRENT_ASSERT(num_blocks > 0);
		TORRENT_ASSERT(pieces.size() == m_piece_map.size());

		TORRENT_ASSERT(!m_priority_boundries.empty()
			|| m_sequential_download >= 0
			|| m_dirty);

		// this will be filled with blocks that we should not request
		// unless we can't find num_blocks among the other ones.
		// blocks that belong to pieces with a mismatching speed
		// category for instance, or if we prefer whole pieces,
		// blocks belonging to a piece that others have
		// downloaded to
		std::vector<piece_block> backup_blocks;
		const std::vector<int> empty_vector;
	
		// When prefer_whole_pieces is set (usually set when downloading from
		// fast peers) the partial pieces will not be prioritized, but actually
		// ignored as long as possible. All blocks found in downloading
		// pieces are regarded as backup blocks

		num_blocks = add_blocks_downloading(pieces
			, interesting_blocks, backup_blocks, num_blocks
			, prefer_whole_pieces, peer, speed, on_parole);

		if (num_blocks <= 0) return;

		if (!suggested_pieces.empty())
		{
			num_blocks = add_blocks(suggested_pieces, pieces
				, interesting_blocks, num_blocks
				, prefer_whole_pieces, peer, empty_vector);
			if (num_blocks == 0) return;
		}

		if (m_sequential_download >= 0)
		{
			for (int i = m_sequential_download;
				i < int(m_piece_map.size()) && num_blocks > 0; ++i)
			{
				if (!can_pick(i, pieces)) continue;
				int num_blocks_in_piece = blocks_in_piece(i);
				if (prefer_whole_pieces == 0 && num_blocks_in_piece > num_blocks)
					num_blocks_in_piece = num_blocks;
				for (int j = 0; j < num_blocks_in_piece; ++j)
				{
					interesting_blocks.push_back(piece_block(i, j));
					--num_blocks;
				}
			}
		}
		else if (rarest_first)
		{
			if (m_dirty) update_pieces();
			num_blocks = add_blocks(m_pieces, pieces
				, interesting_blocks, num_blocks
				, prefer_whole_pieces, peer, suggested_pieces);
			TORRENT_ASSERT(num_blocks >= 0);
		}
		else
		{
			// we're not using rarest first (only for the first
			// bucket, since that's where the currently downloading
			// pieces are)
			int start_piece = rand() % m_piece_map.size();

			// if we have suggested pieces, try to find one of those instead
			for (std::vector<int>::const_iterator i = suggested_pieces.begin()
				, end(suggested_pieces.end()); i != end; ++i)
			{
				if (!can_pick(*i, pieces)) continue;
				start_piece = *i;
				break;
			}
			int piece = start_piece;
			while (num_blocks > 0)
			{
				while (!can_pick(piece, pieces))
				{
					++piece;
					if (piece == int(m_piece_map.size())) piece = 0;
					// could not find any more pieces
					if (piece == start_piece) return;
				}

				int start, end;
				boost::tie(start, end) = expand_piece(piece, prefer_whole_pieces, pieces);
				for (int k = start; k < end; ++k)
				{
					TORRENT_ASSERT(m_piece_map[piece].downloading == false);
					TORRENT_ASSERT(m_piece_map[k].priority(this) >= 0);
					int num_blocks_in_piece = blocks_in_piece(k);
					if (prefer_whole_pieces == 0 && num_blocks_in_piece > num_blocks)
						num_blocks_in_piece = num_blocks;
					for (int j = 0; j < num_blocks_in_piece; ++j)
					{
						interesting_blocks.push_back(piece_block(k, j));
						--num_blocks;
					}
				}
				piece = end;
				if (piece == int(m_piece_map.size())) piece = 0;
				// could not find any more pieces
				if (piece == start_piece) return;
			}
		
		}

		if (num_blocks <= 0) return;

		if (!backup_blocks.empty())
			interesting_blocks.insert(interesting_blocks.end()
				, backup_blocks.begin(), backup_blocks.end());
	}

	bool piece_picker::can_pick(int piece, bitfield const& bitmask) const
	{
		TORRENT_ASSERT(piece >= 0 && piece < int(m_piece_map.size()));
		return bitmask[piece]
			&& !m_piece_map[piece].have()
			&& !m_piece_map[piece].downloading
			&& !m_piece_map[piece].filtered();
	}

	void piece_picker::clear_peer(void* peer)
	{
		for (std::vector<block_info>::iterator i = m_block_info.begin()
			, end(m_block_info.end()); i != end; ++i)
			if (i->peer == peer) i->peer = 0;
	}

	namespace
	{
		// the first bool is true if this is the only peer that has requested and downloaded
		// blocks from this piece.
		// the second bool is true if this is the only active peer that is requesting
		// and downloading blocks from this piece. Active means having a connection.
		boost::tuple<bool, bool> requested_from(piece_picker::downloading_piece const& p
			, int num_blocks_in_piece, void* peer)
		{
			bool exclusive = true;
			bool exclusive_active = true;
			for (int j = 0; j < num_blocks_in_piece; ++j)
			{
				piece_picker::block_info const& info = p.info[j];
				if (info.state != piece_picker::block_info::state_none
					&& info.peer != peer)
				{
					exclusive = false;
					if (info.state == piece_picker::block_info::state_requested
						&& info.peer != 0)
					{
						exclusive_active = false;
						return boost::make_tuple(exclusive, exclusive_active);
					}
				}
			}
			return boost::make_tuple(exclusive, exclusive_active);
		}
	}

	int piece_picker::add_blocks(std::vector<int> const& piece_list
		, bitfield const& pieces
		, std::vector<piece_block>& interesting_blocks
		, int num_blocks, int prefer_whole_pieces
		, void* peer, std::vector<int> const& ignore) const
	{
		for (std::vector<int>::const_iterator i = piece_list.begin();
			i != piece_list.end(); ++i)
		{
			TORRENT_ASSERT(*i >= 0);
			TORRENT_ASSERT(*i < (int)m_piece_map.size());

			// if the peer doesn't have the piece
			// or if it's set to 0 priority
			// skip it
			if (!can_pick(*i, pieces)) continue;

			// ignore pieces found in the ignore list
			if (std::find(ignore.begin(), ignore.end(), *i) != ignore.end()) continue;

			TORRENT_ASSERT(m_piece_map[*i].priority(this) >= 0);
			TORRENT_ASSERT(m_piece_map[*i].downloading == 0);
	
			int num_blocks_in_piece = blocks_in_piece(*i);

			// pick a new piece
			if (prefer_whole_pieces == 0)
			{
				if (num_blocks_in_piece > num_blocks)
					num_blocks_in_piece = num_blocks;
				for (int j = 0; j < num_blocks_in_piece; ++j)
					interesting_blocks.push_back(piece_block(*i, j));
				num_blocks -= num_blocks_in_piece;
			}
			else
			{
				int start, end;
				boost::tie(start, end) = expand_piece(*i, prefer_whole_pieces, pieces);
				for (int k = start; k < end; ++k)
				{
					TORRENT_ASSERT(m_piece_map[k].priority(this) > 0);
					num_blocks_in_piece = blocks_in_piece(k);
					for (int j = 0; j < num_blocks_in_piece; ++j)
					{
						interesting_blocks.push_back(piece_block(k, j));
						--num_blocks;
					}
				}
			}
			if (num_blocks <= 0)
			{
#ifndef NDEBUG
				verify_pick(interesting_blocks, pieces);
#endif
				return 0;
			}
		}
#ifndef NDEBUG
		verify_pick(interesting_blocks, pieces);
#endif
		return num_blocks;
	}

	int piece_picker::add_blocks_downloading(bitfield const& pieces
		, std::vector<piece_block>& interesting_blocks
		, std::vector<piece_block>& backup_blocks
		, int num_blocks, int prefer_whole_pieces
		, void* peer, piece_state_t speed, bool on_parole) const
	{
		for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin()
			, end(m_downloads.end()); i != end; ++i)
		{
			if (!pieces[i->index]) continue;

			int num_blocks_in_piece = blocks_in_piece(i->index);

			// is true if all the other pieces that are currently
			// requested from this piece are from the same
			// peer as 'peer'.
			bool exclusive;
			bool exclusive_active;
			boost::tie(exclusive, exclusive_active)
				= requested_from(*i, num_blocks_in_piece, peer);

			// peers on parole are only allowed to pick blocks from
			// pieces that only they have downloaded/requested from
			if (on_parole && !exclusive) continue;

			if (prefer_whole_pieces > 0 && !exclusive_active) continue;

			// don't pick too many back-up blocks
			if (i->state != none
				&& i->state != speed
				&& !exclusive_active
				&& int(backup_blocks.size()) >= num_blocks)
				continue;

			for (int j = 0; j < num_blocks_in_piece; ++j)
			{
				// ignore completed blocks and already requested blocks
				block_info const& info = i->info[j];
				if (info.state != block_info::state_none)
					continue;

				TORRENT_ASSERT(i->info[j].state == block_info::state_none);

				// if the piece is fast and the peer is slow, or vice versa,
				// add the block as a backup.
				// override this behavior if all the other blocks
				// have been requested from the same peer or
				// if the state of the piece is none (the
				// piece will in that case change state).
				if (i->state != none && i->state != speed
					&& !exclusive_active)
				{
					backup_blocks.push_back(piece_block(i->index, j));
					continue;
				}
				
				// this block is interesting (we don't have it
				// yet).
				interesting_blocks.push_back(piece_block(i->index, j));
				// we have found a block that's free to download
				num_blocks--;
				// if we prefer whole pieces, continue picking from this
				// piece even though we have num_blocks
				if (prefer_whole_pieces > 0) continue;
				TORRENT_ASSERT(num_blocks >= 0);
				if (num_blocks <= 0) break;
			}
			if (num_blocks <= 0) break;
		}
	
		TORRENT_ASSERT(num_blocks >= 0 || prefer_whole_pieces > 0);

#ifndef NDEBUG
		verify_pick(interesting_blocks, pieces);
		verify_pick(backup_blocks, pieces);
#endif

		if (num_blocks <= 0) return 0;
		if (on_parole) return num_blocks;

		int to_copy;
		if (prefer_whole_pieces == 0)
			to_copy = (std::min)(int(backup_blocks.size()), num_blocks);
		else
			to_copy = int(backup_blocks.size());

		interesting_blocks.insert(interesting_blocks.end()
			, backup_blocks.begin(), backup_blocks.begin() + to_copy);
		num_blocks -= to_copy;
		backup_blocks.clear();

		if (num_blocks <= 0) return 0;

		if (prefer_whole_pieces > 0)
		{
			for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin()
				, end(m_downloads.end()); i != end; ++i)
			{
				if (!pieces[i->index]) continue;
				int num_blocks_in_piece = blocks_in_piece(i->index);
				bool exclusive;
				bool exclusive_active;
				boost::tie(exclusive, exclusive_active)
					= requested_from(*i, num_blocks_in_piece, peer);

				if (exclusive_active) continue;
				
				for (int j = 0; j < num_blocks_in_piece; ++j)
				{
					block_info const& info = i->info[j];
					if (info.state != block_info::state_none) continue;
					backup_blocks.push_back(piece_block(i->index, j));
				}
			}
		}

		if (int(backup_blocks.size()) >= num_blocks) return num_blocks;


#ifndef NDEBUG
//		make sure that we at this point has added requests to all unrequested blocks
//		in all downloading pieces

		for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin()
			, end(m_downloads.end()); i != end; ++i)
		{
			if (!pieces[i->index]) continue;
				
			int num_blocks_in_piece = blocks_in_piece(i->index);
			for (int j = 0; j < num_blocks_in_piece; ++j)
			{
				block_info const& info = i->info[j];
				if (info.state != block_info::state_none) continue;
				std::vector<piece_block>::iterator k = std::find(
					interesting_blocks.begin(), interesting_blocks.end()
					, piece_block(i->index, j));
				if (k != interesting_blocks.end()) continue;
				
				k = std::find(backup_blocks.begin()
					, backup_blocks.end(), piece_block(i->index, j));
				if (k != backup_blocks.end()) continue;

				std::cerr << "interesting blocks:" << std::endl;
				for (k = interesting_blocks.begin(); k != interesting_blocks.end(); ++k)
					std::cerr << "(" << k->piece_index << ", " << k->block_index << ") ";
				std::cerr << std::endl;
				std::cerr << "backup blocks:" << std::endl;
				for (k = backup_blocks.begin(); k != backup_blocks.end(); ++k)
					std::cerr << "(" << k->piece_index << ", " << k->block_index << ") ";
				std::cerr << std::endl;
				std::cerr << "num_blocks: " << num_blocks << std::endl;
				
				for (std::vector<downloading_piece>::const_iterator l = m_downloads.begin()
					, end(m_downloads.end()); l != end; ++l)
				{
					std::cerr << l->index << " : ";
					int num_blocks_in_piece = blocks_in_piece(l->index);
					for (int m = 0; m < num_blocks_in_piece; ++m)
						std::cerr << l->info[m].state;
					std::cerr << std::endl;
				}

				TORRENT_ASSERT(false);
			}
		}
#endif

		for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin()
			, end(m_downloads.end()); i != end; ++i)
		{
			if (!pieces[i->index]) continue;

			int num_blocks_in_piece = blocks_in_piece(i->index);

			// fill in with blocks requested from other peers
			// as backups
			for (int j = 0; j < num_blocks_in_piece; ++j)
			{
				block_info const& info = i->info[j];
				if (info.state != block_info::state_requested
					|| info.peer == peer)
					continue;
				backup_blocks.push_back(piece_block(i->index, j));
			}
		}
#ifndef NDEBUG
		verify_pick(backup_blocks, pieces);
#endif
		return num_blocks;
	}
	
	std::pair<int, int> piece_picker::expand_piece(int piece, int whole_pieces
		, bitfield const& have) const
	{
		if (whole_pieces == 0) return std::make_pair(piece, piece + 1);

		int start = piece - 1;
		int lower_limit = piece - whole_pieces;
		if (lower_limit < -1) lower_limit = -1;
		while (start > lower_limit
			&& can_pick(start, have))
			--start;
		++start;
		TORRENT_ASSERT(start >= 0);
		int end = piece + 1;
		int upper_limit = start + whole_pieces;
		if (upper_limit > int(m_piece_map.size())) upper_limit = int(m_piece_map.size());
		while (end < upper_limit
			&& can_pick(end, have))
			++end;
		return std::make_pair(start, end);
	}

	bool piece_picker::is_piece_finished(int index) const
	{
		TORRENT_ASSERT(index < (int)m_piece_map.size());
		TORRENT_ASSERT(index >= 0);

		if (m_piece_map[index].downloading == 0)
		{
			TORRENT_ASSERT(std::find_if(m_downloads.begin(), m_downloads.end()
				, has_index(index)) == m_downloads.end());
			return false;
		}
		std::vector<downloading_piece>::const_iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(index));
		TORRENT_ASSERT(i != m_downloads.end());
		TORRENT_ASSERT((int)i->finished <= m_blocks_per_piece);
		int max_blocks = blocks_in_piece(index);
		if ((int)i->finished < max_blocks) return false;

#ifndef NDEBUG
		for (int k = 0; k < max_blocks; ++k)
		{
			TORRENT_ASSERT(i->info[k].state == block_info::state_finished);
		}
#endif

		TORRENT_ASSERT((int)i->finished == max_blocks);
		return true;
	}

	bool piece_picker::is_requested(piece_block block) const
	{
		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());

		if (m_piece_map[block.piece_index].downloading == 0) return false;
		std::vector<downloading_piece>::const_iterator i
			= std::find_if(
				m_downloads.begin()
				, m_downloads.end()
				, has_index(block.piece_index));

		TORRENT_ASSERT(i != m_downloads.end());
		return i->info[block.block_index].state == block_info::state_requested;
	}

	bool piece_picker::is_downloaded(piece_block block) const
	{
		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());

		if (m_piece_map[block.piece_index].index == piece_pos::we_have_index) return true;
		if (m_piece_map[block.piece_index].downloading == 0) return false;
		std::vector<downloading_piece>::const_iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
		TORRENT_ASSERT(i != m_downloads.end());
		return i->info[block.block_index].state == block_info::state_finished
			|| i->info[block.block_index].state == block_info::state_writing;
	}

	bool piece_picker::is_finished(piece_block block) const
	{
		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());

		if (m_piece_map[block.piece_index].index == piece_pos::we_have_index) return true;
		if (m_piece_map[block.piece_index].downloading == 0) return false;
		std::vector<downloading_piece>::const_iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
		TORRENT_ASSERT(i != m_downloads.end());
		return i->info[block.block_index].state == block_info::state_finished;
	}

	bool piece_picker::mark_as_downloading(piece_block block
		, void* peer, piece_state_t state)
	{
		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());
		TORRENT_ASSERT(block.block_index < blocks_in_piece(block.piece_index));
		TORRENT_ASSERT(!m_piece_map[block.piece_index].have());

		piece_pos& p = m_piece_map[block.piece_index];
		if (p.downloading == 0)
		{
			TORRENT_PIECE_PICKER_INVARIANT_CHECK;
			int prio = p.priority(this);
			TORRENT_ASSERT(prio < int(m_priority_boundries.size())
				|| m_sequential_download >= 0
				|| m_dirty);
			TORRENT_ASSERT(prio > 0);
			p.downloading = 1;
			if (prio >= 0 && m_sequential_download == -1 && !m_dirty) update(prio, p.index);

			downloading_piece& dp = add_download_piece();
			dp.state = state;
			dp.index = block.piece_index;
			block_info& info = dp.info[block.block_index];
			info.state = block_info::state_requested;
			info.peer = peer;
			info.num_peers = 1;
			++dp.requested;
		}
		else
		{
			TORRENT_PIECE_PICKER_INVARIANT_CHECK;
			std::vector<downloading_piece>::iterator i
				= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
			TORRENT_ASSERT(i != m_downloads.end());
			block_info& info = i->info[block.block_index];
			if (info.state == block_info::state_writing
				|| info.state == block_info::state_finished)
				return false;
			TORRENT_ASSERT(info.state == block_info::state_none
				|| (info.state == block_info::state_requested
					&& (info.num_peers > 0)));
			info.peer = peer;
			if (info.state != block_info::state_requested)
			{
				info.state = block_info::state_requested;
				++i->requested;
			}
			++info.num_peers;
			if (i->state == none) i->state = state;
		}
		return true;
	}

	int piece_picker::num_peers(piece_block block) const
	{
		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());
		TORRENT_ASSERT(block.block_index < blocks_in_piece(block.piece_index));

		piece_pos const& p = m_piece_map[block.piece_index];
		if (!p.downloading) return 0;

		std::vector<downloading_piece>::const_iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
		TORRENT_ASSERT(i != m_downloads.end());

		block_info const& info = i->info[block.block_index];
		return info.num_peers;
	}
	
	void piece_picker::get_availability(std::vector<int>& avail) const
	{
		TORRENT_ASSERT(m_seeds >= 0);
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;
	
		avail.resize(m_piece_map.size());
		std::vector<int>::iterator j = avail.begin();
		for (std::vector<piece_pos>::const_iterator i = m_piece_map.begin()
			, end(m_piece_map.end()); i != end; ++i, ++j)
			*j = i->peer_count + m_seeds;
	}

	void piece_picker::mark_as_writing(piece_block block, void* peer)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());
		TORRENT_ASSERT(block.block_index < blocks_in_piece(block.piece_index));

		TORRENT_ASSERT(m_piece_map[block.piece_index].downloading);

		std::vector<downloading_piece>::iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
		TORRENT_ASSERT(i != m_downloads.end());
		block_info& info = i->info[block.block_index];

		info.peer = peer;
		TORRENT_ASSERT(info.state == block_info::state_requested);
		if (info.state == block_info::state_requested) --i->requested;
		TORRENT_ASSERT(i->requested >= 0);
		TORRENT_ASSERT(info.state != block_info::state_writing);
		++i->writing;
		info.state = block_info::state_writing;
		TORRENT_ASSERT(info.num_peers > 0);
		info.num_peers = 0;

		if (i->requested == 0)
		{
			// there are no blocks requested in this piece.
			// remove the fast/slow state from it
			i->state = none;
		}
		sort_piece(i);
	}

	void piece_picker::write_failed(piece_block block)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		std::vector<downloading_piece>::iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
		TORRENT_ASSERT(i != m_downloads.end());
		block_info& info = i->info[block.block_index];
		TORRENT_ASSERT(info.state == block_info::state_writing);

		--i->writing;
		if (info.num_peers > 0)
		{
			// there are other peers on this block
			// turn it back into requested
			++i->requested;
			info.state = block_info::state_requested;
		}
		else
		{
			info.state = block_info::state_none;
		}
		info.peer = 0;
	}
	
	void piece_picker::mark_as_finished(piece_block block, void* peer)
	{
		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());
		TORRENT_ASSERT(block.block_index < blocks_in_piece(block.piece_index));

		piece_pos& p = m_piece_map[block.piece_index];

		if (p.downloading == 0)
		{
			TORRENT_PIECE_PICKER_INVARIANT_CHECK;
			
			TORRENT_ASSERT(peer == 0);
			int prio = p.priority(this);
			TORRENT_ASSERT(prio < int(m_priority_boundries.size())
				|| m_dirty);
			p.downloading = 1;
			if (prio >= 0 && !m_dirty) update(prio, p.index);

			downloading_piece& dp = add_download_piece();
			dp.state = none;
			dp.index = block.piece_index;
			block_info& info = dp.info[block.block_index];
			info.peer = peer;
			TORRENT_ASSERT(info.state == block_info::state_none);
			TORRENT_ASSERT(info.num_peers == 0);
			if (info.state != block_info::state_finished)
			{
				++dp.finished;
				sort_piece(m_downloads.end() - 1);
			}
			info.state = block_info::state_finished;
		}
		else
		{
			TORRENT_PIECE_PICKER_INVARIANT_CHECK;
			
			std::vector<downloading_piece>::iterator i
				= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(block.piece_index));
			TORRENT_ASSERT(i != m_downloads.end());
			block_info& info = i->info[block.block_index];
			TORRENT_ASSERT(info.num_peers == 0);
			info.peer = peer;
			TORRENT_ASSERT(info.state == block_info::state_writing
				|| peer == 0);
			TORRENT_ASSERT(i->writing >= 0);
			++i->finished;
			if (info.state == block_info::state_writing)
			{
				--i->writing;
				info.state = block_info::state_finished;
			}
			else
			{
				info.state = block_info::state_finished;
				sort_piece(i);
			}
		}
	}

	void piece_picker::get_downloaders(std::vector<void*>& d, int index) const
	{
		TORRENT_ASSERT(index >= 0 && index <= (int)m_piece_map.size());
		std::vector<downloading_piece>::const_iterator i
			= std::find_if(m_downloads.begin(), m_downloads.end(), has_index(index));
		TORRENT_ASSERT(i != m_downloads.end());

		d.clear();
		for (int j = 0; j < blocks_in_piece(index); ++j)
		{
			d.push_back(i->info[j].peer);
		}
	}

	void* piece_picker::get_downloader(piece_block block) const
	{
		std::vector<downloading_piece>::const_iterator i = std::find_if(
			m_downloads.begin()
			, m_downloads.end()
			, has_index(block.piece_index));

		if (i == m_downloads.end()) return 0;

		TORRENT_ASSERT(block.block_index >= 0);

		if (i->info[block.block_index].state == block_info::state_none)
			return 0;

		return i->info[block.block_index].peer;
	}

	// this is called when a request is rejected or when
	// a peer disconnects. The piece might be in any state
	void piece_picker::abort_download(piece_block block)
	{
		TORRENT_PIECE_PICKER_INVARIANT_CHECK;

		TORRENT_ASSERT(block.piece_index >= 0);
		TORRENT_ASSERT(block.block_index >= 0);
		TORRENT_ASSERT(block.piece_index < (int)m_piece_map.size());
		TORRENT_ASSERT(block.block_index < blocks_in_piece(block.piece_index));

		if (m_piece_map[block.piece_index].downloading == 0)
		{
			TORRENT_ASSERT(std::find_if(m_downloads.begin(), m_downloads.end()
				, has_index(block.piece_index)) == m_downloads.end());
			return;
		}

		std::vector<downloading_piece>::iterator i = std::find_if(m_downloads.begin()
			, m_downloads.end(), has_index(block.piece_index));
		TORRENT_ASSERT(i != m_downloads.end());

		block_info& info = i->info[block.block_index];

		if (i->info[block.block_index].state != block_info::state_requested)
			return;

		if (info.num_peers > 0) --info.num_peers;

		TORRENT_ASSERT(block.block_index < blocks_in_piece(block.piece_index));

		// if there are other peers, leave the block requested
		if (info.num_peers > 0) return;

		// clear the downloader of this block
		info.peer = 0;

		// clear this block as being downloaded
		info.state = block_info::state_none;
		--i->requested;
		
		// if there are no other blocks in this piece
		// that's being downloaded, remove it from the list
		if (i->requested + i->finished + i->writing == 0)
		{
			erase_download_piece(i);
			piece_pos& p = m_piece_map[block.piece_index];
			int prev_prio = p.priority(this);
			TORRENT_ASSERT(prev_prio < int(m_priority_boundries.size())
				|| m_dirty);
			p.downloading = 0;
			if (m_sequential_download == -1 && !m_dirty)
			{
				int prio = p.priority(this);
				if (prev_prio == -1 && prio >= 0) add(block.piece_index);
				else if (prev_prio >= 0) update(prev_prio, p.index);
			}

			TORRENT_ASSERT(std::find_if(m_downloads.begin(), m_downloads.end()
				, has_index(block.piece_index)) == m_downloads.end());
		}
		else if (i->requested == 0)
		{
			// there are no blocks requested in this piece.
			// remove the fast/slow state from it
			i->state = none;
		}
	}

	int piece_picker::unverified_blocks() const
	{
		int counter = 0;
		for (std::vector<downloading_piece>::const_iterator i = m_downloads.begin();
			i != m_downloads.end(); ++i)
		{
			counter += (int)i->finished;
		}
		return counter;
	}

}

