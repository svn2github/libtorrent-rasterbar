/*

Copyright (c) 2003, Arvid Norberg, Daniel Wallin
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

#include <ctime>
#include <algorithm>
#include <set>
#include <functional>

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/ref.hpp>
#include <boost/bind.hpp>
#include <boost/version.hpp>
#include <boost/scoped_array.hpp>
#if BOOST_VERSION >= 103500
#include <boost/system/system_error.hpp>
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "libtorrent/config.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/peer_id.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/file_pool.hpp"
#include "libtorrent/aux_/session_impl.hpp"
#include "libtorrent/disk_buffer_holder.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/allocator.hpp" // page_size

#include <cstdio>

//#define TORRENT_PARTIAL_HASH_LOG

#if defined(__APPLE__)
// for getattrlist()
#include <sys/attr.h>
#include <unistd.h>
// for statfs()
#include <sys/param.h>
#include <sys/mount.h>
#endif

#if defined(__linux__)
#include <sys/statfs.h>
#endif

#if defined(__FreeBSD__)
// for statfs()
#include <sys/param.h>
#include <sys/mount.h>
#endif

// for convert_to_wstring and convert_to_native
#include "libtorrent/escape_string.hpp"

namespace libtorrent
{

	// wrap job handlers to free the job itself
	// this is called in the network thread when a job completes
	void complete_job(void* user, aiocb_pool* pool, disk_io_job* j)
	{
		aux::session_impl* ses = (aux::session_impl*)user;
#ifdef TORRENT_STATS
		if (ses) ++ses->m_num_messages[aux::session_impl::on_disk_counter];
#endif
		while (j)
		{
			TORRENT_ASSERT(j->callback_called == true);
			if (j->callback) j->callback(j->ret, *j);
			disk_io_job* to_free = (disk_io_job*)j;
			j = (disk_io_job*)j->next;
			pool->free_job(to_free);
		}
		// uncork all peers who received a disk event. This is
		// to coalesce all the socket writes caused by the events.
		if (ses) ses->do_delayed_uncork();
	}

	void recursive_copy(std::string const& old_path, std::string const& new_path, error_code& ec)
	{
		TORRENT_ASSERT(!ec);
		if (is_directory(old_path, ec))
		{
			create_directory(new_path, ec);
			if (ec) return;
			for (directory i(old_path, ec); !i.done(); i.next(ec))
			{
				std::string f = i.file();
				recursive_copy(f, combine_path(new_path, f), ec);
				if (ec) return;
			}
		}
		else if (!ec)
		{
			copy_file(old_path, new_path, ec);
		}
	}

	void recursive_remove(std::string const& old_path)
	{
		error_code ec;
		if (is_directory(old_path, ec))
		{
			for (directory i(old_path, ec); !i.done(); i.next(ec))
				recursive_remove(combine_path(old_path, i.file()));
			remove(old_path, ec);
		}
		else
		{
			remove(old_path, ec);
		}
	}

	int copy_bufs(file::iovec_t const* bufs, int bytes, file::iovec_t* target)
	{
		int size = 0;
		int ret = 1;
		for (;;)
		{
			*target = *bufs;
			size += bufs->iov_len;
			if (size >= bytes)
			{
				target->iov_len -= size - bytes;
				return ret;
			}
			++bufs;
			++target;
			++ret;
		}
	}

	void advance_bufs(file::iovec_t*& bufs, int bytes)
	{
		int size = 0;
		for (;;)
		{
			size += bufs->iov_len;
			if (size >= bytes)
			{
				((char*&)bufs->iov_base) += bufs->iov_len - (size - bytes);
				bufs->iov_len = size - bytes;
				return;
			}
			++bufs;
		}
	}

	TORRENT_EXPORT int bufs_size(file::iovec_t const* bufs, int num_bufs)
	{
		int size = 0;
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i < end; ++i)
			size += i->iov_len;
		return size;
	}
	
	void clear_bufs(file::iovec_t const* bufs, int num_bufs)
	{
		for (file::iovec_t const* i = bufs, *end(bufs + num_bufs); i < end; ++i)
			std::memset(i->iov_base, 0, i->iov_len);
	}

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	int count_bufs(file::iovec_t const* bufs, int bytes)
	{
		int size = 0;
		int count = 1;
		if (bytes == 0) return 0;
		for (file::iovec_t const* i = bufs;; ++i, ++count)
		{
			size += i->iov_len;
			TORRENT_ASSERT(size <= bytes);
			if (size >= bytes) return count;
		}
	}
#endif

	default_storage::default_storage(file_storage const& fs, file_storage const* mapped
		, std::string const& path, file_pool& fp, std::vector<boost::uint8_t> const& file_prio)
		: m_files(fs)
		, m_file_priority(file_prio)
		, m_pool(fp)
		, m_page_size(page_size())
		, m_allocate_files(false)
	{
		if (mapped) m_mapped_files.reset(new file_storage(*mapped));

		TORRENT_ASSERT(m_files.begin() != m_files.end());
		m_save_path = complete(path);
	}

	default_storage::~default_storage()
	{
		// this may be called from a different
		// thread than the disk thread
//		m_pool.release(this);
	}

	void default_storage::initialize(bool allocate_files, storage_error& ec)
	{
		if (m_stat_cache.empty()) m_stat_cache.reserve(files().num_files());

		m_allocate_files = allocate_files;
		// first, create all missing directories
		std::string last_path;
		int file_index = 0;
		std::string file_path;
		for (file_storage::iterator file_iter = files().begin(),
			end_iter = files().end(); file_iter != end_iter; ++file_iter, ++file_index)
		{
			// ignore files that have priority 0
			if (int(m_file_priority.size()) > file_index
				&& m_file_priority[file_index] == 0)
			{
				if (m_stat_cache.size() < file_index)
				{
					TORRENT_ASSERT(m_stat_cache.size() == file_index);
					m_stat_cache.push_back(stat_cache_t(-2));
				}
				continue;
			}

			// ignore pad files
			if (file_iter->pad_file)
			{
				if (m_stat_cache.size() < file_index)
				{
					TORRENT_ASSERT(m_stat_cache.size() == file_index);
					m_stat_cache.push_back(stat_cache_t(-2));
				}
				continue;
			}

			if (m_stat_cache.size() <= file_index)
			{
				file_path = combine_path(m_save_path, files().file_path(*file_iter));

				file_status s;
				stat_file(file_path, &s, ec.ec);
				if (ec && ec.ec != boost::system::errc::no_such_file_or_directory
					&& ec.ec != boost::system::errc::not_a_directory)
				{
					if (m_stat_cache.size() < file_index)
					{
						TORRENT_ASSERT(m_stat_cache.size() == file_index);
						m_stat_cache.push_back(stat_cache_t(-1));
					}
					ec.file = file_index;
					ec.operation = storage_error::stat;
					break;
				}
				TORRENT_ASSERT(m_stat_cache.size() == file_index);
				m_stat_cache.push_back(stat_cache_t(s.file_size, s.mtime));
			}

			// ec is either ENOENT or the file existed and s is valid
			// allocate file only if it is not exist and (allocate_files == true)
			// if the file already exists, but is larger than what
			// it's supposed to be, also truncate it
			// if the file is empty, just create it either way.
			if ((ec && allocate_files)
				|| (!ec && m_stat_cache[file_index].file_size > file_iter->size)
				|| file_iter->size == 0)
			{
				std::string dir = parent_path(file_path);

				if (dir != last_path)
				{
					last_path = dir;

					create_directories(last_path, ec.ec);
					if (ec.ec)
					{
						ec.file = file_index;
						ec.operation = storage_error::mkdir;
						break;
					}
				}
				ec.ec.clear();
				boost::intrusive_ptr<file> f = open_file(file_iter, file::read_write, 0, ec.ec);
				if (ec || !f)
				{
					ec.file = file_index;
					ec.operation = storage_error::open;
					return;
				}
				f->set_size(file_iter->size, ec.ec);
				if (ec)
				{
					ec.file = file_index;
					ec.operation = storage_error::fallocate;
					break;
				}
			}
			ec.ec.clear();
		}

		std::vector<boost::uint8_t>().swap(m_file_priority);
		// close files that were opened in write mode
		m_pool.release(this);
	}

	void default_storage::finalize_file(int index, storage_error& ec)
	{
		TORRENT_ASSERT(index >= 0 && index < files().num_files());
		if (index < 0 || index >= files().num_files()) return;
	
		boost::intrusive_ptr<file> f = open_file(files().begin() + index, file::read_write, 0, ec.ec);
		if (ec || !f)
		{
			ec.file = index;
			ec.operation = storage_error::open;
			return;
		}

		f->finalize();
	}

	bool default_storage::has_any_file(storage_error& ec)
	{
		file_storage::iterator i = files().begin();
		file_storage::iterator end = files().end();

		if (m_stat_cache.empty()) m_stat_cache.reserve(files().num_files());

		std::string file_path;
		int index = 0;
		for (; i != end; ++i, ++index)
		{
			file_status s;
			if (m_stat_cache.size() <= index
				|| m_stat_cache[index].file_size == -2
				|| m_stat_cache[index].file_size == -1)
			{
				file_path = combine_path(m_save_path, files().file_path(*i));
				stat_file(file_path, &s, ec.ec);
				size_type r = s.file_size;
				if (ec.ec || !(s.mode & file_status::regular_file)) r = -1;

				if (ec && ec.ec == boost::system::errc::no_such_file_or_directory)
				{
					ec.ec.clear();
					r = -3;
				}

				if (m_stat_cache.size() <= index)
					m_stat_cache.push_back(stat_cache_t(r, s.mtime));
				else
					m_stat_cache[index] = stat_cache_t(r, s.mtime);

				if (ec)
				{
					ec.file = index;
					ec.operation = storage_error::stat;
					return false;
				}
			}
	
			// if we didn't find the file, check the next one
			if (m_stat_cache[index].file_size == -3) continue;

			if (m_stat_cache[index].file_size > 0)
				return true;
		}
		return false;
	}

	void default_storage::rename_file(int index, std::string const& new_filename, storage_error& ec)
	{
		if (index < 0 || index >= files().num_files()) return;
		std::string old_name = combine_path(m_save_path, files().file_path(files().at(index)));
		m_pool.release(this, index);

		rename(old_name, combine_path(m_save_path, new_filename), ec.ec);
		
		if (ec.ec == boost::system::errc::no_such_file_or_directory)
			ec.ec.clear();

		if (ec)
		{
			ec.file = index;
			ec.operation = storage_error::rename;
			return;
		}

		// if old path doesn't exist, just rename the file
		// in our file_storage, so that when it is created
		// it will get the new name
		if (!m_mapped_files)
		{ m_mapped_files.reset(new file_storage(m_files)); }
		m_mapped_files->rename_file(index, new_filename);
	}

	void default_storage::release_files(storage_error& ec)
	{
		m_pool.release(this);
	}

	void default_storage::delete_one_file(std::string const& p, error_code& ec)
	{
		remove(p, ec);
		
		if (ec == boost::system::errc::no_such_file_or_directory)
			ec.clear();
	}

	void default_storage::delete_files(storage_error& ec)
	{
		// make sure we don't have the files open
		m_pool.release(this);

		// delete the files from disk
		std::set<std::string> directories;
		typedef std::set<std::string>::iterator iter_t;
		for (file_storage::iterator i = files().begin()
			, end(files().end()); i != end; ++i)
		{
			std::string fp = files().file_path(*i);
			std::string p = combine_path(m_save_path, fp);
			std::string bp = parent_path(fp);
			std::pair<iter_t, bool> ret;
			ret.second = true;
			while (ret.second && !bp.empty())
			{
				ret = directories.insert(combine_path(m_save_path, bp));
				bp = parent_path(bp);
			}
			delete_one_file(p, ec.ec);
			if (ec) { ec.file = i - files().begin(); ec.operation = storage_error::remove; }
		}

		// remove the directories. Reverse order to delete
		// subdirectories first

		for (std::set<std::string>::reverse_iterator i = directories.rbegin()
			, end(directories.rend()); i != end; ++i)
		{
			delete_one_file(*i, ec.ec);
			if (ec) { ec.file = -1; ec.operation = storage_error::remove; }
		}
	}

	void default_storage::write_resume_data(entry& rd, storage_error& ec) const
	{
		TORRENT_ASSERT(rd.type() == entry::dictionary_t);

		entry::list_type& fl = rd["file sizes"].list();

		file_storage const& fs = files();
		int index = 0;
		for (file_storage::iterator i = fs.begin(); i != fs.end(); ++i, ++index)
		{
			size_type file_size = 0;
			time_t file_time = 0;
			if (m_stat_cache.size() > index && m_stat_cache[index].file_size != -2)
			{
				if (m_stat_cache[index].file_size >= 0)
				{
					file_size = m_stat_cache[index].file_size;
					file_time = m_stat_cache[index].file_time;
				}
			}
			else
			{
				file_status s;
				error_code ec;
				stat_file(combine_path(m_save_path, fs.file_path(*i)), &s, ec);
				if (m_stat_cache.size() <= index) m_stat_cache.resize(index+1, stat_cache_t(-2));
				if (!ec)
				{
					file_size = s.file_size;
					file_time = s.mtime;
				}
				else
				{
					if (ec == boost::system::errc::no_such_file_or_directory)
						m_stat_cache[index].file_size = -3;
					else
						m_stat_cache[index].file_size = -1;
				}
			}

			fl.push_back(entry(entry::list_t));
			entry::list_type& p = fl.back().list();
			p.push_back(entry(file_size));
			p.push_back(entry(file_time));
		}
	}

	int default_storage::sparse_end(int slot) const
	{
		TORRENT_ASSERT(slot >= 0);
		TORRENT_ASSERT(slot < m_files.num_pieces());

		size_type file_offset = (size_type)slot * m_files.piece_length();
		file_storage::iterator file_iter;

		for (file_iter = files().begin();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
			TORRENT_ASSERT(file_iter != files().end());
		}
	
		error_code ec;
		boost::intrusive_ptr<file> file_handle = open_file(file_iter, file::read_only, 0, ec);
		if (!file_handle || ec) return slot;

		size_type data_start = file_handle->sparse_end(file_offset);
		return int((data_start + m_files.piece_length() - 1) / m_files.piece_length());
	}

	bool default_storage::verify_resume_data(lazy_entry const& rd, storage_error& ec)
	{
		// TODO: make this more generic to not just work if files have been
		// renamed, but also if they have been merged into a single file for instance
		// maybe use the same format as .torrent files and reuse some code from torrent_info
		lazy_entry const* mapped_files = rd.dict_find_list("mapped_files");
		if (mapped_files && mapped_files->list_size() == m_files.num_files())
		{
			m_mapped_files.reset(new file_storage(m_files));
			for (int i = 0; i < m_files.num_files(); ++i)
			{
				std::string new_filename = mapped_files->list_string_value_at(i);
				if (new_filename.empty()) continue;
				m_mapped_files->rename_file(i, new_filename);
				if (m_stat_cache.size() > i) m_stat_cache[i].file_size = -2;
			}
		}
		
		lazy_entry const* file_priority = rd.dict_find_list("file_priority");
		if (file_priority && file_priority->list_size()
			== files().num_files())
		{
			m_file_priority.resize(file_priority->list_size());
			for (int i = 0; i < file_priority->list_size(); ++i)
				m_file_priority[i] = boost::uint8_t(file_priority->list_int_value_at(i, 1));
		}

//		std::vector<std::pair<size_type, std::time_t> > file_sizes;
		lazy_entry const* file_sizes_ent = rd.dict_find_list("file sizes");
		if (file_sizes_ent == 0)
		{
			ec.ec = errors::missing_file_sizes;
			return false;
		}
		
		if (file_sizes_ent->list_size() == 0)
		{
			ec.ec = errors::no_files_in_resume_data;
			return false;
		}
		
		file_storage const& fs = files();
		if (file_sizes_ent->list_size() != fs.num_files())
		{
			ec.ec = errors::mismatching_number_of_files;
			ec.file = -1;
			ec.operation = storage_error::none;
			return false;
		}

		bool seed = false;
		lazy_entry const* slots = rd.dict_find_list("slots");
		if (slots)
		{
			if (int(slots->list_size()) == m_files.num_pieces())
			{
				seed = true;
				for (int i = 0; i < slots->list_size(); ++i)
				{
					if (slots->list_int_value_at(i, -1) >= 0) continue;
					seed = false;
					break;
				}
			}
		}
		else if (lazy_entry const* pieces = rd.dict_find_string("pieces"))
		{
			if (int(pieces->string_length()) == m_files.num_pieces())
			{
				seed = true;
				char const* p = pieces->string_ptr();
				for (int i = 0; i < pieces->string_length(); ++i)
				{
					if ((p[i] & 1) == 1) continue;
					seed = false;
					break;
				}
			}
		}
		else
		{
			ec.ec = errors::missing_pieces;
			return false;
		}

		file_storage::iterator file_iter = fs.begin();
		for (int i = 0; i < file_sizes_ent->list_size(); ++i, ++file_iter)
		{
			if (file_iter->pad_file) continue;
			lazy_entry const* e = file_sizes_ent->list_at(i);
			if (e->type() != lazy_entry::list_t
				|| e->list_size() < 2
				|| e->list_at(0)->type() != lazy_entry::int_t
				|| e->list_at(1)->type() != lazy_entry::int_t)
			{
				ec.ec = errors::missing_file_sizes;
				ec.file = i;
				ec.operation = storage_error::none;
				return false;
			}

			size_type expected_size = e->list_int_value_at(0);
			time_t expected_time = e->list_int_value_at(1);

			// if we're a seed, the expected size should match
			// the actual full size according to the torrent
			if (seed && expected_size < file_iter->size)
			{
				ec.ec = errors::mismatching_file_size;
				ec.file = i;
				ec.operation = storage_error::none;
				return false;
			}

			size_type file_size;
			time_t file_time;
			if (i < m_stat_cache.size() && m_stat_cache[i].file_size != -2)
			{
				file_size = m_stat_cache[i].file_size;
				file_time = m_stat_cache[i].file_time;
			}
			else
			{
				file_status s;
				error_code error;
				std::string file_path = combine_path(m_save_path, fs.file_path(*file_iter));
				stat_file(file_path, &s, error);
				if (m_stat_cache.size() <= i) m_stat_cache.resize(i + 1, stat_cache_t(-2));
				file_size = s.file_size;
				file_time = s.mtime;
				if (error)
				{
					if (error != boost::system::errc::no_such_file_or_directory)
					{
						m_stat_cache[i].file_size = -1;
						ec.ec = error;
						ec.file = i;
						ec.operation = storage_error::stat;
						return false;
					}
					m_stat_cache[i].file_size = -3;
					if (expected_size != 0)
					{
						ec.ec = errors::mismatching_file_size;
						ec.file = i;
						ec.operation = storage_error::none;
						return false;
					}
				}
			}

			if (expected_size > file_size)
			{
				ec.ec = errors::mismatching_file_size;
				ec.file = i;
				ec.operation = storage_error::none;
				return false;
			}

			if (settings().ignore_resume_timestamps) continue;

			// allow some slack, because of FAT volumes
			if (file_time > expected_time + 5 * 60 || file_time < expected_time - 5)
			{
				ec.ec = errors::mismatching_file_timestamp;
				ec.file = i;
				ec.operation = storage_error::stat;
				return false;
			}
		}

		bool full_allocation_mode = false;
		if (rd.dict_find_string_value("allocation") != "compact")
			full_allocation_mode = true;

		return true;
	}

	// returns true on success
	void default_storage::move_storage(std::string const& sp, storage_error& ec)
	{
		std::string save_path = complete(sp);

		file_status s;
		stat_file(save_path, &s, ec.ec);
		if (ec.ec == boost::system::errc::no_such_file_or_directory)
		{
			create_directories(save_path, ec.ec);
			if (ec)
			{
				ec.file = -1;
				ec.operation = storage_error::mkdir;
				return;
			}
		}
		else if (ec)
		{
			ec.file = -1;
			ec.operation = storage_error::mkdir;
			return;
		}
		ec.ec.clear();

		m_pool.release(this);

		std::map<std::string, int> to_move;
		file_storage const& f = files();

		for (file_storage::iterator i = f.begin()
			, end(f.end()); i != end; ++i)
		{
			std::string split = split_path(f.file_path(*i));
			to_move.insert(to_move.begin(), std::make_pair(split, int(i - f.begin())));
		}

		for (std::map<std::string, int>::const_iterator i = to_move.begin()
			, end(to_move.end()); i != end; ++i)
		{
			std::string old_path = combine_path(m_save_path, i->first);
			std::string new_path = combine_path(save_path, i->first);

			rename(old_path, new_path, ec.ec);
			if (ec.ec == boost::system::errc::no_such_file_or_directory)
				ec.ec.clear();

			if (ec)
			{
				ec.ec.clear();
				recursive_copy(old_path, new_path, ec.ec);
				if (!ec) recursive_remove(old_path);
				else { ec.file = i->second; ec.operation = storage_error::copy; }
				break;
			}
		}

		if (!ec) m_save_path = save_path;
	}

	size_type default_storage::physical_offset(int slot, int offset)
	{
		TORRENT_ASSERT(slot >= 0);
		TORRENT_ASSERT(slot < m_files.num_pieces());
		TORRENT_ASSERT(offset >= 0);

		// find the file and file
		size_type tor_off = size_type(slot)
			* files().piece_length() + offset;
		file_storage::iterator file_iter = files().file_at_offset(tor_off);
		while (file_iter->pad_file)
		{
			++file_iter;
			if (file_iter == files().end())
				return size_type(slot) * files().piece_length() + offset;
			// update offset as well, since we're moving it up ahead
			tor_off = file_iter->offset;
		}
		TORRENT_ASSERT(!file_iter->pad_file);

		size_type file_offset = tor_off - file_iter->offset;
		TORRENT_ASSERT(file_offset >= 0);

		// open the file read only to avoid re-opening
		// it in case it's already opened in read-only mode
		error_code ec;
		boost::intrusive_ptr<file> f = open_file(file_iter, file::read_only, 0, ec);

		size_type ret = 0;
		if (f && !ec) ret = f->phys_offset(file_offset);

		if (ret == 0)
		{
			// this means we don't support true physical offset
			// just make something up
			return size_type(slot) * files().piece_length() + offset;
		}
		return ret;
	}

	void default_storage::hint_read(int slot, int offset, int size)
	{
		size_type start = slot * (size_type)m_files.piece_length() + offset;
		TORRENT_ASSERT(start + size <= m_files.total_size());

		size_type file_offset = start;
		file_storage::iterator file_iter;

		// TODO: use binary search!
		for (file_iter = files().begin();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
			TORRENT_ASSERT(file_iter != files().end());
		}

		boost::intrusive_ptr<file> file_handle;
		int bytes_left = size;
		int slot_size = static_cast<int>(m_files.piece_size(slot));

		if (offset + bytes_left > slot_size)
			bytes_left = slot_size - offset;

		TORRENT_ASSERT(bytes_left >= 0);

		int file_bytes_left;
		for (;bytes_left > 0; ++file_iter, bytes_left -= file_bytes_left)
		{
			TORRENT_ASSERT(file_iter != files().end());

			file_bytes_left = bytes_left;
			if (file_offset + file_bytes_left > file_iter->size)
				file_bytes_left = (std::max)(static_cast<int>(file_iter->size - file_offset), 0);

			if (file_bytes_left == 0) continue;

			if (file_iter->pad_file) continue;

			error_code ec;
			file_handle = open_file(file_iter, file::read_only, 0, ec);

			// failing to hint that we want to read is not a big deal
			// just swollow the error and keep going
			if (!file_handle || ec) continue;

			file_handle->hint_read(file_offset, file_bytes_left);
			file_offset = 0;
		}
	}

	file::aiocb_t* default_storage::async_readv(file::iovec_t const* bufs, int num_bufs
		, int slot, int offset, int flags, async_handler* a)
	{
		if (m_settings)
		{
			flags |= settings().coalesce_reads ? file::coalesce_buffers : 0;
			flags |= settings().allow_reordered_disk_operations ? file::resolve_phys_offset : 0;
		}

		fileop op = { &file::async_readv, a, 0, m_settings ? settings().disk_io_read_mode : 0
			, file::read_only, flags, storage_error::async_readv };
		storage_error ec;
		readwritev(bufs, slot, offset, num_bufs, op, ec);
		a->error = ec;
		return op.ret;
	}

	file::aiocb_t* default_storage::async_writev(file::iovec_t const* bufs, int num_bufs
		, int slot, int offset, int flags, async_handler* a)
	{
		if (m_settings)
			flags |= settings().coalesce_writes ? file::coalesce_buffers : 0;

		fileop op = { &file::async_writev, a, 0, m_settings ? settings().disk_io_write_mode : 0
			, file::read_write, flags, storage_error::async_writev };
		storage_error ec;
		readwritev(bufs, slot, offset, num_bufs, op, ec);
		a->error = ec;
		return op.ret;
	}

	// much of what needs to be done when reading and writing 
	// is buffer management and piece to file mapping. Most
	// of that is the same for reading and writing. This function
	// is a template, and the fileop decides what to do with the
	// file and the buffers.
	int default_storage::readwritev(file::iovec_t const* bufs, int slot, int offset
		, int num_bufs, fileop& op, storage_error& ec)
	{
		TORRENT_ASSERT(bufs != 0);
		TORRENT_ASSERT(slot >= 0);
		TORRENT_ASSERT(slot < m_files.num_pieces());
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(offset < m_files.piece_size(slot));
		TORRENT_ASSERT(num_bufs > 0);

		// this is the last element in the chain, that we hook new
		// aiocb's to
		file::aiocb_t* last = 0;
		op.ret = 0;

		int size = bufs_size(bufs, num_bufs);
		TORRENT_ASSERT(size > 0);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		std::vector<file_slice> slices
			= files().map_block(slot, offset, size);
		TORRENT_ASSERT(!slices.empty());
#endif

		size_type start = slot * (size_type)m_files.piece_length() + offset;
		TORRENT_ASSERT(start + size <= m_files.total_size());

		// find the file iterator and file offset
		size_type file_offset = start;
		file_storage::iterator file_iter;

		// TODO: use binary search!
		for (file_iter = files().begin();;)
		{
			if (file_offset < file_iter->size)
				break;

			file_offset -= file_iter->size;
			++file_iter;
			TORRENT_ASSERT(file_iter != files().end());
		}

		int buf_pos = 0;

		boost::intrusive_ptr<file> file_handle;
		int bytes_left = size;
		int slot_size = static_cast<int>(m_files.piece_size(slot));

		if (offset + bytes_left > slot_size)
			bytes_left = slot_size - offset;

		TORRENT_ASSERT(bytes_left >= 0);

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		int counter = 0;
#endif

		file::iovec_t* tmp_bufs = TORRENT_ALLOCA(file::iovec_t, num_bufs);
		file::iovec_t* current_buf = TORRENT_ALLOCA(file::iovec_t, num_bufs);
		copy_bufs(bufs, size, current_buf);
		TORRENT_ASSERT(count_bufs(current_buf, size) == num_bufs);
		int file_bytes_left;
		for (;bytes_left > 0; ++file_iter, bytes_left -= file_bytes_left
			, buf_pos += file_bytes_left)
		{
			TORRENT_ASSERT(file_iter != files().end());
			TORRENT_ASSERT(buf_pos >= 0);

			file_bytes_left = bytes_left;
			if (file_offset + file_bytes_left > file_iter->size)
				file_bytes_left = (std::max)(static_cast<int>(file_iter->size - file_offset), 0);

			if (file_bytes_left == 0) continue;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
			TORRENT_ASSERT(int(slices.size()) > counter);
			size_type slice_size = slices[counter].size;
			TORRENT_ASSERT(slice_size == file_bytes_left);
			TORRENT_ASSERT((files().begin() + slices[counter].file_index)
				== file_iter);
			++counter;
#endif

			if (file_iter->pad_file)
			{
				if (op.mode == file::read_only)
				{
					int num_tmp_bufs = copy_bufs(current_buf, file_bytes_left, tmp_bufs);
					TORRENT_ASSERT(count_bufs(tmp_bufs, file_bytes_left) == num_tmp_bufs);
					TORRENT_ASSERT(num_tmp_bufs <= num_bufs);
					clear_bufs(tmp_bufs, num_tmp_bufs);
				}
				advance_bufs(current_buf, file_bytes_left);
				TORRENT_ASSERT(count_bufs(current_buf, bytes_left - file_bytes_left) <= num_bufs);
				file_offset = 0;
				continue;
			}

			int file_index = file_iter - files().begin();
			if (op.mode == file::read_write && m_stat_cache.size() > file_index)
			{
				// if we're writing to this file, we have to clear the
				// stats cache for it, since its modification time will change
				// and its size may change too
				m_stat_cache[file_index].file_size = -2;
			}

			file_handle = open_file(file_iter, op.mode, op.flags, ec.ec);
			if ((op.mode == file::read_write) && ec.ec == boost::system::errc::no_such_file_or_directory)
			{
				// this means the directory the file is in doesn't exist.
				// so create it
				ec.ec.clear();
				std::string path = combine_path(m_save_path, files().file_path(*file_iter));
				create_directories(parent_path(path), ec.ec);
				// if the directory creation failed, don't try to open the file again
				// but actually just fail
				if (!ec) file_handle = open_file(file_iter, op.mode, op.flags, ec.ec);
			}

			if (!file_handle || ec)
			{
				ec.file = file_iter - files().begin();
				TORRENT_ASSERT(ec);
				return -1;
			}

			int num_tmp_bufs = copy_bufs(current_buf, file_bytes_left, tmp_bufs);
			TORRENT_ASSERT(count_bufs(tmp_bufs, file_bytes_left) == num_tmp_bufs);
			TORRENT_ASSERT(num_tmp_bufs <= num_bufs);
			int bytes_transferred = 0;
			// if the file is opened in no_buffer mode, and the
			// read is unaligned, we need to fall back on a slow
			// special read that reads aligned buffers and copies
			// it into the one supplied
			size_type adjusted_offset = files().file_base(*file_iter) + file_offset;
			TORRENT_ASSERT(op.op);
			TORRENT_ASSERT(op.handler);

			file::aiocb_t* aio = ((*file_handle).*op.op)(adjusted_offset
				, tmp_bufs, num_tmp_bufs, *aiocbs(), op.flags);

			if (op.ret == 0) op.ret = aio;

			// add this to the chain
			while (aio)
			{
				bytes_transferred += aio->nbytes();
				aio->handler = op.handler;
				++op.handler->references;
				if (last) last->next = aio;
				aio->prev = last;
				last = aio;
				aio = aio->next;
				TORRENT_ASSERT(bytes_transferred <= bufs_size(tmp_bufs, num_tmp_bufs));
			}

			file_offset = 0;

			if (ec)
			{
				ec.file = file_iter - files().begin();
				ec.operation = op.operation_type;
				return -1;
			}

			TORRENT_ASSERT(file_bytes_left >= bytes_transferred);
			if (file_bytes_left != bytes_transferred)
				return bytes_transferred;

			advance_bufs(current_buf, bytes_transferred);
			TORRENT_ASSERT(count_bufs(current_buf, bytes_left - file_bytes_left) <= num_bufs);
		}
		return size;
	}

	boost::intrusive_ptr<file> default_storage::open_file(file_storage::iterator fe, int mode
		, int flags, error_code& ec) const
	{
		// io_submit only works on files opened with O_DIRECT, so this
		// is not optional if we're using io_submit
#if !TORRENT_USE_IOSUBMIT
		int cache_setting = m_settings ? settings().disk_io_write_mode : 0;
		if (cache_setting == session_settings::disable_os_cache
			|| (cache_setting == session_settings::disable_os_cache_for_aligned_files
			&& ((fe->offset + files().file_base(*fe)) & (m_page_size-1)) == 0))
#endif
			mode |= file::no_buffer;
		if (!(flags & file::sequential_access))
			mode |= file::random_access;

		bool lock_files = m_settings ? settings().lock_files : false;
		if (lock_files) mode |= file::lock_file;
		if (!m_allocate_files) mode |= file::sparse;
		if (m_settings && settings().no_atime_storage) mode |= file::no_atime;

		return m_pool.open_file(const_cast<default_storage*>(this), m_save_path, fe, files(), mode, ec);
	}

	storage_interface* default_storage_constructor(file_storage const& fs
		, file_storage const* mapped, std::string const& path, file_pool& fp
		, std::vector<boost::uint8_t> const& file_prio)
	{
		return new default_storage(fs, mapped, path, fp, file_prio);
	}

	file::aiocb_t* disabled_storage::async_readv(file::iovec_t const* bufs
		, int num_bufs, int slot, int offset, int flags, async_handler* a)
	{
		return 0;
	}

	file::aiocb_t* disabled_storage::async_writev(file::iovec_t const* bufs
		, int num_bufs, int slot, int offset, int flags, async_handler* a)
	{
		return 0;
	}

	storage_interface* disabled_storage_constructor(file_storage const& fs
		, file_storage const* mapped, std::string const& path, file_pool& fp
		, std::vector<boost::uint8_t> const&)
	{
		return new disabled_storage(fs.piece_length());
	}

	// -- zero_storage ------------------------------------------------------

	file::aiocb_t* zero_storage::async_readv(file::iovec_t const* bufs, int num_bufs
		, int piece, int offset, int flags, async_handler* a)
	{
		a->transferred = 0;
		for (int i = 0; i < num_bufs; ++i)
		{
			memset(bufs[i].iov_base, 0, bufs[i].iov_len);
			a->transferred += bufs[i].iov_len;
		}
		return 0;
	}

	file::aiocb_t* zero_storage::async_writev(file::iovec_t const* bufs, int num_bufs
		, int piece, int offset, int flags, async_handler* a)
	{
		a->transferred = 0;
		for (int i = 0; i < num_bufs; ++i)
			a->transferred += bufs[i].iov_len;
		return 0;
	}

	storage_interface* zero_storage_constructor(file_storage const& fs
		, file_storage const* mapped, std::string const& path, file_pool& fp
		, std::vector<boost::uint8_t> const&)
	{
		return new zero_storage;
	}

	// -- piece_manager -----------------------------------------------------

	piece_manager::piece_manager(
		boost::shared_ptr<void> const& torrent
		, file_storage* files
		, file_storage const* orig_files
		, std::string const& save_path
		, disk_io_thread& io
		, storage_constructor_type sc
		, storage_mode_t sm
		, std::vector<boost::uint8_t> const& file_prio)
		: m_files(*files)
		, m_storage(sc(*files, orig_files, save_path, io.files(), file_prio))
		, m_abort_job(0)
		, m_storage_mode(sm)
		, m_storage_constructor(sc)
		, m_io_thread(io)
		, m_torrent(torrent)
	{
		m_storage->m_disk_pool = m_io_thread.cache();
		m_storage->m_aiocb_pool = m_io_thread.aiocbs();
	}

	piece_manager::~piece_manager()
	{
		TORRENT_ASSERT(!has_fence());
		TORRENT_ASSERT(m_abort_job == 0);
	}

	void piece_manager::add_piece(cached_piece_entry* p)
	{
		TORRENT_ASSERT(m_cached_pieces.count(p) == 0);
		m_cached_pieces.insert(p);
	}

	bool piece_manager::has_piece(cached_piece_entry* p) const
	{
		return m_cached_pieces.count(p) > 0;
	}

	void piece_manager::remove_piece(cached_piece_entry* p)
	{
		TORRENT_ASSERT(m_cached_pieces.count(p) == 1);
		m_cached_pieces.erase(p);
	}

	// TODO: it doesn't make any sense for the piece_manager to
	// contain this wrapper around posting jobs to the disk thread
	// piece_manager can probably be removed
	void piece_manager::async_finalize_file(int file)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::finalize_file);
		j->storage = this;
		j->piece = file;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_get_cache_info(cache_status* ret
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::get_cache_info);
		j->storage = this;
		j->buffer = (char*)ret;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_file_status(std::vector<pool_file_status>* ret
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::file_status);
		j->storage = this;
		j->buffer = (char*)ret;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_save_resume_data(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::save_resume_data);
		j->buffer = 0;
		j->storage = this;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_clear_piece(int piece)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::clear_piece);
		j->storage = this;
		j->piece = piece;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_sync_piece(int piece
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::sync_piece);
		j->storage = this;
		j->piece = piece;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_flush_piece(int piece)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::flush_piece);
		j->storage = this;
		j->piece = piece;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_clear_read_cache(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::clear_read_cache);
		j->storage = this;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_release_files(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::release_files);
		j->storage = this;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::abort_disk_io(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::abort_torrent);
		j->storage = this;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_delete_files(
		boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::delete_files);
		j->storage = this;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_move_storage(std::string const& p
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::move_storage);
		j->storage = this;
		j->buffer = strdup(p.c_str());
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_check_fastresume(lazy_entry const* resume_data
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		TORRENT_ASSERT(resume_data != 0);
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::check_fastresume);
		j->storage = this;
		j->buffer = (char*)resume_data;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_rename_file(int index, std::string const& name
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::rename_file);
		j->storage = this;
		j->piece = index;
		j->buffer = strdup(name.c_str());
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_cache(int piece
		, boost::function<void(int, disk_io_job const&)> const& handler)
	{
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::cache_piece);
		j->storage = this;
		j->piece = piece;
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_read(
		peer_request const& r
		, boost::function<void(int, disk_io_job const&)> const& handler
		, void* requester
		, int flags
		, int cache_line_size)
	{
		TORRENT_ASSERT(requester != 0);
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::read);
		j->storage = this;
		j->piece = r.piece;
		j->d.io.offset = r.start;
		j->d.io.buffer_size = r.length;
		j->buffer = 0;
		j->d.io.max_cache_line = cache_line_size;
		j->flags = flags;
		j->requester = requester;

		// if a buffer is not specified, only one block can be read
		// since that is the size of the pool allocator's buffers
		TORRENT_ASSERT(r.length <= 16 * 1024);
		j->callback = handler;
		m_io_thread.add_job(j);
	}

	void piece_manager::async_write(
		peer_request const& r
		, disk_buffer_holder& buffer
		, boost::function<void(int, disk_io_job const&)> const& handler
		, int flags)
	{
		TORRENT_ASSERT(r.length <= 16 * 1024);
		// the buffer needs to be allocated through the io_thread
		TORRENT_ASSERT(buffer.get());

		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::write);
		j->storage = this;
		j->action = disk_io_job::write;
		j->piece = r.piece;
		j->d.io.offset = r.start;
		j->d.io.buffer_size = r.length;
		j->buffer = buffer.get();
		j->callback = handler;
		j->flags = flags;
		m_io_thread.add_job(j);
		buffer.release();
	}

	void piece_manager::async_hash(int piece, int flags
		, boost::function<void(int, disk_io_job const&)> const& handler
		, void* requester)
	{
		TORRENT_ASSERT(requester != 0);
		TORRENT_ASSERT(piece >= 0 && piece < files()->num_pieces());
		disk_io_job* j = m_io_thread.aiocbs()->allocate_job(disk_io_job::hash);
		j->flags = flags;
		j->storage = this;
		j->piece = piece;
		j->requester = requester;
		j->callback = handler;
		j->d.io.buffer_size = 0;
		m_io_thread.add_job(j);
	}

	// used in torrent_handle.cpp
	void piece_manager::write_resume_data(entry& rd, storage_error& ec) const
	{
		INVARIANT_CHECK;
		m_storage->write_resume_data(rd, ec);
	}

	int piece_manager::check_no_fastresume(storage_error& ec)
	{
		bool has_files = false;
		if (!m_storage->settings().no_recheck_incomplete_resume)
		{
			storage_error se;
			has_files = m_storage->has_any_file(se);

			if (se)
			{
				ec = se;
				return fatal_disk_error; 
			}

			if (has_files) return need_full_check;
		}

		return check_init_storage(ec);
	}
	
	int piece_manager::check_init_storage(storage_error& ec)
	{
		storage_error se;
		m_storage->initialize(m_storage_mode == storage_mode_allocate, se);
		if (se)
		{
			ec = se;
			return fatal_disk_error; 
		}

		return no_error;
	}

	// check if the fastresume data is up to date
	// if it is, use it and return true. If it 
	// isn't return false and the full check
	// will be run
	int piece_manager::check_fastresume(
		lazy_entry const& rd, storage_error& ec)
	{
		INVARIANT_CHECK;

		TORRENT_ASSERT(m_files.piece_length() > 0);
		
		// if we don't have any resume data, return
		if (rd.type() == lazy_entry::none_t) return check_no_fastresume(ec);

		if (rd.type() != lazy_entry::dict_t)
		{
			ec.ec = errors::not_a_dictionary;
			return check_no_fastresume(ec);
		}

		int block_size = (std::min)(16 * 1024, m_files.piece_length());
		int blocks_per_piece = int(rd.dict_find_int_value("blocks per piece", -1));
		if (blocks_per_piece != -1
			&& blocks_per_piece != m_files.piece_length() / block_size)
		{
			ec.ec = errors::invalid_blocks_per_piece;
			return check_no_fastresume(ec);
		}

		if (!m_storage->verify_resume_data(rd, ec))
			return check_no_fastresume(ec);

		return check_init_storage(ec);
	}

	// ====== disk_job_fence implementation ========

	disk_job_fence::disk_job_fence()
		: m_has_fence(false)
		, m_outstanding_jobs(0)
	{}

	void disk_job_fence::new_job(disk_io_job* j)
	{
		++m_outstanding_jobs;
		TORRENT_ASSERT((j->flags & disk_io_job::async_operation) == 0);
		j->flags |= disk_io_job::async_operation;
	}

	int disk_job_fence::job_complete(disk_io_job* j, tailqueue& jobs)
	{
		TORRENT_ASSERT(j->storage);
		TORRENT_ASSERT(j->flags & disk_io_job::async_operation);
		TORRENT_ASSERT(m_outstanding_jobs > 0);
		--m_outstanding_jobs;
		j->flags &= ~disk_io_job::async_operation;

		// there are still outstanding jobs, even if we have a
		// fence, it's not time to lower it yet
		// also, if we don't have a fence, we're done
		if (m_outstanding_jobs > 0 || !m_has_fence) return 0;

		int ret = m_blocked_jobs.size();
		m_has_fence = false;
		if (m_blocked_jobs.size()) jobs.prepend(m_blocked_jobs);
		return ret;
	}

	bool disk_job_fence::is_blocked(disk_io_job* j)
	{
		if (!has_fence()) return false;
		m_blocked_jobs.push_back(j);
		return true;
	}

	void disk_job_fence::raise_fence(disk_io_job* j)
	{
		TORRENT_ASSERT(m_outstanding_jobs > 0);
		m_has_fence = true;
		m_blocked_jobs.push_back(j);
	}

#ifdef TORRENT_DEBUG
	void piece_manager::check_invariant() const {}
#endif
} // namespace libtorrent

