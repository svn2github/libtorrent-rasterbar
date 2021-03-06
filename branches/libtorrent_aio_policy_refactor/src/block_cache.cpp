/*

Copyright (c) 2010, Arvid Norberg
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

#include "libtorrent/block_cache.hpp"
#include "libtorrent/disk_buffer_pool.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/disk_io_job.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/io_service_fwd.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/disk_io_thread.hpp" // disk_operation_failed
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/alert_dispatcher.hpp"

/*

	The disk cache mimics ARC (adaptive replacement cache).
	See paper: http://dbs.uni-leipzig.de/file/ARC.pdf
	See slides: http://www-vlsi.stanford.edu/smart_memories/protected/meetings/spring2004/arc-fast.pdf

	This cache has a few modifications to make it fit the bittorrent use
	case better. It has a few more lists lists and it deferres the eviction
	of pieces.

	read_lru1
		This is a plain LRU for items that have been requested once.
		If a piece in this list gets accessed again, by someone other
		than the first accessor, the piece is promoted into LRU2.
		which holds pieces that are more frequently used, and more
		important to keep around as this LRU list takes churn.
	
	read_lru1_ghost
		This is a list of pieces that were least recently evicted from
		read_lru1. These pieces don't hold any actual blocks in the cache,
		they are just here to extend the reach and probability for pieces
		to be promoted into read_lru2. Any piece in this list that
		get one more access is promoted to read_lru2. This is technically
		a cache-miss, since there's no cached blocks here, but for the
		purposes of promoting the piece from infrequently used to frequently
		used), it's considered a cache-hit.

	read_lru2
		TODO

	read_lru2_ghost
		TODO

	volatile_read_lru
		TODO

	write_lru
		TODO

	Cache hits
	..........

	When a piece get a cache hit, it's promoted, either to the beginning of the
	lru2 or into lru2. Since this ARC implementation operates on pieces instead
	of blocks, any one peer requesting blocks from one piece would essentially
	always produce a "cache hit" the second block it requests. In order to make
	the promotions make more sense, and be more in the spirit of the ARC algorithm,
	each access contains a token, unique to each peer. If any access has a different
	token than the last one, it's considered a cache hit. This is because at least
	two peers requested blocks from the same piece.

	Deferred evictions
	..................

	Since pieces and blocks can be pinned in the cache, and it's not always practical,
	or possible, to evict a piece at the point where a new block is allocated (because
	it's not known what the block will be used for), evictions are not done at the time
	of allocating blocks. Instead, whenever an operation requires to add a new piece to
	the cache, it also records the cache event leading to it, in m_last_cache_op. This
	is one of cache_miss (piece did not exist in cache), lru1_ghost_hit (the piece was found
	in lru1_ghost and it was promoted) or lru2_ghost_hit (the piece was found in lru2_ghost
	and it was promoted). This cache operation then guides the cache eviction algorithm
	to know which list to evict from. The volatile list is always the first one to be
	evicted however.

*/


#define DEBUG_CACHE 0

#define DLOG if (DEBUG_CACHE) fprintf

namespace libtorrent {

#if DEBUG_CACHE
void log_refcounts(cached_piece_entry const* pe)
{
	char out[4096];
	char* ptr = out;
	char* end = ptr + sizeof(out);
	ptr += snprintf(ptr, end - ptr, "piece: %d [ ", int(pe->piece));
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		ptr += snprintf(ptr, end - ptr, "%d ", int(pe->blocks[i].refcount));
	}
	strncpy(ptr, "]\n", end - ptr);
	DLOG(stderr, out);
}
#endif

cached_piece_entry::cached_piece_entry()
	: storage()
	, hash(0)
	, last_requester(NULL)
	, blocks()
	, expire(min_time())
	, piece(0)
	, num_dirty(0)
	, num_blocks(0)
	, blocks_in_piece(0)
	, hashing(0)
	, hashing_done(0)
	, marked_for_deletion(false)
	, need_readback(false)
	, cache_state(read_lru1)
	, piece_refcount(0)
	, outstanding_flush(0)
	, padding(0)
	, refcount(0)
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	, hash_passes(0)
#endif
{}

cached_piece_entry::~cached_piece_entry()
{
	TORRENT_ASSERT(piece_refcount == 0);
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	for (int i = 0; i < blocks_in_piece; ++i)
	{
		TORRENT_ASSERT(blocks[i].buf == 0);
		TORRENT_ASSERT(!blocks[i].pending);
		TORRENT_ASSERT(blocks[i].refcount == 0);
		TORRENT_ASSERT(blocks[i].hashing_count == 0);
		TORRENT_ASSERT(blocks[i].flushing_count == 0);
	}
#endif
	delete hash;
}

block_cache::block_cache(int block_size, io_service& ios
	, alert_dispatcher* alert_disp)
	: disk_buffer_pool(block_size, ios, alert_disp)
	, m_last_cache_op(cache_miss)
	, m_ghost_size(8)
	, m_read_cache_size(0)
	, m_write_cache_size(0)
	, m_send_buffer_blocks(0)
	, m_blocks_read(0)
	, m_blocks_read_hit(0)
	, m_pinned_blocks(0)
{}

// returns:
// -1: not in cache
// -2: no memory
int block_cache::try_read(disk_io_job* j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j->buffer == 0);

	cached_piece_entry* p = find_piece(j);

	int ret = 0;

	// if the piece cannot be found in the cache,
	// it's a cache miss
	if (p == 0) return -1;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	p->piece_log.push_back(piece_log_t(j->action, j->d.io.offset / 0x4000));
#endif
	cache_hit(p, j->requester, j->flags & disk_io_job::volatile_read);

	ret = copy_from_piece(p, j);
	if (ret < 0) return ret;

	ret = j->d.io.buffer_size;
	++m_blocks_read;
	++m_blocks_read_hit;
	return ret;
}

void block_cache::bump_lru(cached_piece_entry* p)
{
	// move to the top of the LRU list
	TORRENT_ASSERT(p->cache_state == cached_piece_entry::write_lru);
	linked_list* lru_list = &m_lru[p->cache_state];

	// move to the back (MRU) of the list
	lru_list->erase(p);
	lru_list->push_back(p);
	p->expire = time_now();
}

// this is called for pieces that we're reading from, when they
// are in the cache (including the ghost lists)
void block_cache::cache_hit(cached_piece_entry* p, void* requester, bool volatile_read)
{
// this can be pretty expensive
//	INVARIANT_CHECK;

	// move the piece into this queue. Whenever we have a cahe
	// hit, we move the piece into the lru2 queue (i.e. the most
	// frequently used piece). However, we only do that if the
	// requester is different than the last one. This is to
	// avoid a single requester making it look like a piece is
	// frequently requested, when in fact it's only a single peer
	int target_queue = cached_piece_entry::read_lru2;

	if (p->last_requester == requester || requester == NULL)
	{
		// if it's the same requester and the piece isn't in
		// any of the ghost lists, ignore it
		if (p->cache_state == cached_piece_entry::read_lru1
			|| p->cache_state == cached_piece_entry::read_lru2
			|| p->cache_state == cached_piece_entry::write_lru
			|| p->cache_state == cached_piece_entry::volatile_read_lru)
			return;

		if (p->cache_state == cached_piece_entry::read_lru1_ghost)
			target_queue = cached_piece_entry::read_lru1;
	}

	if (p->cache_state == cached_piece_entry::volatile_read_lru)
	{
		// a volatile read hit on a volatile piece doesn't do anything
		if (volatile_read) return;

		// however, if this is a proper read on a volatile piece
		// we need to promote it to lru1
		target_queue = cached_piece_entry::read_lru1;
	}

	if (requester != NULL)
		p->last_requester = requester;

	// if we have this piece anywhere in L1 or L2, it's a "hit"
	// and it should be bumped to the highest priority in L2
	// i.e. "frequently used"
	if (p->cache_state < cached_piece_entry::read_lru1
		|| p->cache_state > cached_piece_entry::read_lru2_ghost)
		return;

	// if we got a cache hit in a ghost list, that indicates the proper
	// list is too small. Record which ghost list we got the hit in and
	// it will be used to determine which end of the cache we'll evict
	// from, next time we need to reclaim blocks
	if (p->cache_state == cached_piece_entry::read_lru1_ghost)
	{
		m_last_cache_op = ghost_hit_lru1;
		p->storage->add_piece(p);
	}
	else if (p->cache_state == cached_piece_entry::read_lru2_ghost)
	{
		m_last_cache_op = ghost_hit_lru2;
		p->storage->add_piece(p);
	}

	// move into L2 (frequently used)
	m_lru[p->cache_state].erase(p);
	m_lru[target_queue].push_back(p);
	p->cache_state = target_queue;
	p->expire = time_now();
}

// this is used to move pieces primarily from the write cache
// to the read cache. Technically it can move from read to write
// cache as well, it's unclear if that ever happens though
void block_cache::update_cache_state(cached_piece_entry* p)
{
	int state = p->cache_state;
	int desired_state = p->cache_state;
	if (p->num_dirty > 0 || p->hash != 0)
		desired_state = cached_piece_entry::write_lru;
	else if (p->cache_state == cached_piece_entry::write_lru)
		desired_state = cached_piece_entry::read_lru1;

	if (desired_state == state) return;

	TORRENT_ASSERT(state < cached_piece_entry::num_lrus);
	TORRENT_ASSERT(desired_state < cached_piece_entry::num_lrus);
	linked_list* src = &m_lru[state];
	linked_list* dst = &m_lru[desired_state];

	src->erase(p);
	dst->push_back(p);
	p->expire = time_now();
	p->cache_state = desired_state;
}

cached_piece_entry* block_cache::allocate_piece(disk_io_job const* j, int cache_state)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(cache_state < cached_piece_entry::num_lrus);

	// we're assuming we're not allocating a ghost piece
	// a bit further down
	TORRENT_ASSERT(cache_state != cached_piece_entry::read_lru1_ghost
		&& cache_state != cached_piece_entry::read_lru2_ghost);

	cached_piece_entry* p = find_piece(j);
	if (p == 0)
	{
		int piece_size = j->storage->files()->piece_size(j->piece);
		int blocks_in_piece = (piece_size + block_size() - 1) / block_size();

		cached_piece_entry pe;
		pe.piece = j->piece;
		pe.storage = j->storage;
		pe.expire = time_now();
		pe.blocks_in_piece = blocks_in_piece;
		pe.blocks.reset(new (std::nothrow) cached_block_entry[blocks_in_piece]);
		pe.cache_state = cache_state;
		pe.last_requester = j->requester;
		TORRENT_ASSERT(pe.blocks);
		if (!pe.blocks) return 0;
		p = const_cast<cached_piece_entry*>(&*m_pieces.insert(pe).first);

		j->storage->add_piece(p);

		TORRENT_ASSERT(p->cache_state < cached_piece_entry::num_lrus);
		linked_list* lru_list = &m_lru[p->cache_state];
		lru_list->push_back(p);

		// this piece is part of the ARC cache (as opposed to
		// the write cache). Allocating a new read piece indicates
		// that we just got a cache miss. Record this to determine
		// which end to evict blocks from next time we need to
		// evict blocks
		if (cache_state == cached_piece_entry::read_lru1)
			m_last_cache_op = cache_miss;
	}
	else
	{
		// we want to retain the piece now
		p->marked_for_deletion = false;

		// only allow changing the cache state downwards. i.e. turn a ghost
		// piece into a non-ghost, or a read piece into a write piece
		if (p->cache_state > cache_state)
		{
			// this can happen for instance if a piece fails the hash check
			// first it's in the write cache, then it completes and is moved
			// into the read cache, but fails and is cleared (into the ghost list)
			// then we want to add new dirty blocks to it and we need to move
			// it back into the write cache

			// it also happens when pulling a ghost piece back into the proper cache

			if (p->cache_state == cached_piece_entry::read_lru1_ghost
				|| p->cache_state == cached_piece_entry::read_lru2_ghost)
			{
				// since it used to be a ghost piece, but no more,
				// we need to add it back to the storage
				p->storage->add_piece(p);
			}
			m_lru[p->cache_state].erase(p);
			p->cache_state = cache_state;
			m_lru[p->cache_state].push_back(p);
			p->expire = time_now();
		}
	}

	return p;
}

cached_piece_entry* block_cache::add_dirty_block(disk_io_job* j)
{
#if !defined TORRENT_DISABLE_POOL_ALLOCATOR
	TORRENT_ASSERT(is_disk_buffer(j->buffer));
#endif
	INVARIANT_CHECK;

	TORRENT_ASSERT(j->buffer);
	TORRENT_ASSERT(m_write_cache_size + m_read_cache_size + 1 <= in_use());

	cached_piece_entry* pe = allocate_piece(j, cached_piece_entry::write_lru);
	TORRENT_ASSERT(pe);
	if (pe == 0) return pe;

	int block = j->d.io.offset / block_size();
	TORRENT_ASSERT((j->d.io.offset % block_size()) == 0);

	// we should never add a new dirty block on a piece
	// that has checked the hash. Before we add it, the
	// piece need to be cleared (with async_clear_piece)
	TORRENT_ASSERT(pe->hashing_done == 0);

	// this only evicts read blocks

	int evict = num_to_evict(1);
	if (evict > 0) try_evict_blocks(evict, pe);

	TORRENT_ASSERT(block < pe->blocks_in_piece);
	TORRENT_ASSERT(j->piece == pe->piece);
	TORRENT_ASSERT(!pe->marked_for_deletion);

	TORRENT_ASSERT(pe->blocks[block].refcount == 0);

	cached_block_entry& b = pe->blocks[block];

	TORRENT_ASSERT(b.buf != j->buffer);

	// we might have a left-over read block from
	// hash checking
	// we might also have a previous dirty block which
	// we're still waiting for to be written
	if (b.buf != 0 && b.buf != j->buffer)
	{
		TORRENT_ASSERT(b.refcount == 0 && !b.pending);
		free_block(pe, block);
		TORRENT_ASSERT(b.dirty == 0);
	}

	b.buf = j->buffer;
#ifdef TORRENT_BUFFER_STATS
	rename_buffer(j->buffer, "write cache");
#endif

	b.dirty = true;
	++pe->num_blocks;
	++pe->num_dirty;
	++m_write_cache_size;
	j->buffer = 0;
	TORRENT_ASSERT(j->piece == pe->piece);
	TORRENT_ASSERT(j->flags & disk_io_job::in_progress);
	pe->jobs.push_back(j);

	if (block == 0 && pe->hash == NULL && pe->hashing_done == false)
		pe->hash = new partial_hash;

	update_cache_state(pe);

	bump_lru(pe);

	return pe;
}

// flushed is an array of num_flushed integers. Each integer is the block
// index that was flushed. This function marks those blocks as not pending and
// not dirty. It also adjusts its understanding of the read vs. write cache size
// (since these blocks now are part of the read cache)
// the refcounts of the blocks are also decremented by this function. They are
// expected to have been incremented by the caller.
void block_cache::blocks_flushed(cached_piece_entry* pe, int const* flushed, int num_flushed)
{
	for (int i = 0; i < num_flushed; ++i)
	{
		int block = flushed[i];
		TORRENT_ASSERT(block >= 0);
		TORRENT_ASSERT(block < pe->blocks_in_piece);
		TORRENT_ASSERT(pe->blocks[block].dirty);
		TORRENT_ASSERT(pe->blocks[block].pending);
		pe->blocks[block].pending = false;
		pe->blocks[block].dirty = false;
		dec_block_refcount(pe, block, block_cache::ref_flushing);
#ifdef TORRENT_BUFFER_STATS
		rename_buffer(pe->blocks[block].buf, "read cache");
#endif
	}

	m_write_cache_size -= num_flushed;
	m_read_cache_size += num_flushed;
	pe->num_dirty -= num_flushed;

	update_cache_state(pe);
}

std::pair<block_cache::iterator, block_cache::iterator> block_cache::all_pieces()
{
	return std::make_pair(m_pieces.begin(), m_pieces.end());
}

void block_cache::free_block(cached_piece_entry* pe, int block)
{
	TORRENT_ASSERT(pe != 0);
	TORRENT_ASSERT(block < pe->blocks_in_piece);
	TORRENT_ASSERT(block >= 0);

	cached_block_entry& b = pe->blocks[block];

	TORRENT_ASSERT(b.refcount == 0);
	TORRENT_ASSERT(!b.pending);
	TORRENT_ASSERT(b.buf);

	if (b.dirty)
	{
		--pe->num_dirty;
		b.dirty = false;
		TORRENT_ASSERT(m_write_cache_size > 0);
		--m_write_cache_size;
	}
	else
	{
		TORRENT_ASSERT(m_read_cache_size > 0);
		--m_read_cache_size;
	}
	TORRENT_ASSERT(pe->num_blocks > 0);
	--pe->num_blocks;
	free_buffer(b.buf);
	b.buf = 0;
}

bool block_cache::evict_piece(cached_piece_entry* pe, tailqueue& jobs)
{
	INVARIANT_CHECK;

	char** to_delete = TORRENT_ALLOCA(char*, pe->blocks_in_piece);
	int num_to_delete = 0;
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (pe->blocks[i].buf == 0 || pe->blocks[i].refcount > 0) continue;
		TORRENT_ASSERT(!pe->blocks[i].pending);
		TORRENT_ASSERT(pe->blocks[i].buf != 0);
		TORRENT_ASSERT(num_to_delete < pe->blocks_in_piece);
		to_delete[num_to_delete++] = pe->blocks[i].buf;
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		if (!pe->blocks[i].dirty)
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
		else
		{
			TORRENT_ASSERT(pe->num_dirty > 0);
			--pe->num_dirty;
			pe->blocks[i].dirty = false;
			TORRENT_ASSERT(m_write_cache_size > 0);
			--m_write_cache_size;
		}
		if (pe->num_blocks == 0) break;
	}
	if (num_to_delete) free_multiple_buffers(to_delete, num_to_delete);

	if (pe->ok_to_evict(true))
	{
		delete pe->hash;
		pe->hash = NULL;

		jobs.swap(pe->jobs);
		if (pe->cache_state == cached_piece_entry::read_lru1_ghost
			|| pe->cache_state == cached_piece_entry::read_lru2_ghost)
			return true;

		if (pe->cache_state == cached_piece_entry::write_lru
			|| pe->cache_state == cached_piece_entry::volatile_read_lru)
			erase_piece(pe);
		else
			move_to_ghost(pe);
		return true;
	}

	return false;
}

void block_cache::mark_for_deletion(cached_piece_entry* p)
{
	INVARIANT_CHECK;

	DLOG(stderr, "[%p] block_cache mark-for-deletion "
		"piece: %d\n", this, int(p->piece));

	TORRENT_ASSERT(p->jobs.empty());
	tailqueue jobs;
	if (!evict_piece(p, jobs))
	{
		p->marked_for_deletion = true;
	}
}

void block_cache::erase_piece(cached_piece_entry* pe)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(pe->ok_to_evict());
	TORRENT_ASSERT(pe->cache_state < cached_piece_entry::num_lrus);
	TORRENT_ASSERT(pe->jobs.empty());
	linked_list* lru_list = &m_lru[pe->cache_state];
	if (pe->hash)
	{
		TORRENT_ASSERT(pe->hash->offset == 0);
		delete pe->hash;
		pe->hash = NULL;
	}
	if (pe->cache_state != cached_piece_entry::read_lru1_ghost
		&& pe->cache_state != cached_piece_entry::read_lru2_ghost)
		pe->storage->remove_piece(pe);
	lru_list->erase(pe);
	m_pieces.erase(*pe);
}

// this only evicts read blocks. For write blocks, see
// try_flush_write_blocks in disk_io_thread.cpp
int block_cache::try_evict_blocks(int num, cached_piece_entry* ignore)
{
	INVARIANT_CHECK;

	if (num <= 0) return 0;

	DLOG(stderr, "[%p] try_evict_blocks: %d\n", this, num);

	char** to_delete = TORRENT_ALLOCA(char*, num);
	int num_to_delete = 0;

	// There are two ends of the ARC cache we can evict from. There's L1
	// and L2. The last cache operation determines which end we'll evict
	// from. If we go through the entire list from the preferred end, and
	// still need to evict more blocks, we'll go to the other end and start
	// evicting from there. The lru_list is an array of two lists, these
	// are the two ends to evict from, ordered by preference.

	linked_list* lru_list[3];

	// however, before we consider any of the proper LRU lists, we evict
	// pieces from the volatile list. These are low priority pieces that
	// were specifically marked as to not survive long in the cache. These
	// are the first pieces to go when evicting
	lru_list[0] = &m_lru[cached_piece_entry::volatile_read_lru];

	if (m_last_cache_op == cache_miss)
	{
		// when there was a cache miss, evict from the largest
		// list, to tend to keep the lists of equal size when
		// we don't know which one is performing better
		if (m_lru[cached_piece_entry::read_lru2].size()
			> m_lru[cached_piece_entry::read_lru1].size())
		{
			lru_list[1] = &m_lru[cached_piece_entry::read_lru2];
			lru_list[2] = &m_lru[cached_piece_entry::read_lru1];
		}
		else
		{
			lru_list[1] = &m_lru[cached_piece_entry::read_lru1];
			lru_list[2] = &m_lru[cached_piece_entry::read_lru2];
		}
	}
	else if (m_last_cache_op == ghost_hit_lru1)
	{
		// when we insert new items or move things from L1 to L2
		// evict blocks from L2
		lru_list[1] = &m_lru[cached_piece_entry::read_lru2];
		lru_list[2] = &m_lru[cached_piece_entry::read_lru1];
	}
	else
	{
		// when we get cache hits in L2 evict from L1
		lru_list[1] = &m_lru[cached_piece_entry::read_lru1];
		lru_list[2] = &m_lru[cached_piece_entry::read_lru2];
	}

	// end refers to which end of the ARC cache we're evicting
	// from. The LFU or the LRU end
	for (int end = 0; num > 0 && end < 3; ++end)
	{
		// iterate over all blocks in order of last being used (oldest first) and as
		// long as we still have blocks to evict
		for (list_iterator i = lru_list[end]->iterate(); i.get() && num > 0;)
		{
			cached_piece_entry* pe = reinterpret_cast<cached_piece_entry*>(i.get());

			if (pe == ignore)
			{
				i.next();
				continue;
			}

			if (pe->ok_to_evict())
			{
#ifdef TORRENT_DEBUG
				for (int j = 0; j < pe->blocks_in_piece; ++j)
					TORRENT_ASSERT(pe->blocks[j].buf == 0);
#endif
				TORRENT_ASSERT(pe->refcount == 0);
				i.next();
				move_to_ghost(pe);
				continue;
			}

			TORRENT_ASSERT(pe->num_dirty == 0);

			// go through the blocks and evict the ones
			// that are not dirty and not referenced
			for (int j = 0; j < pe->blocks_in_piece && num > 0; ++j)
			{
				cached_block_entry& b = pe->blocks[j];

				if (b.buf == 0 || b.refcount > 0 || b.dirty || b.pending) continue;

				to_delete[num_to_delete++] = b.buf;
				b.buf = 0;
				TORRENT_ASSERT(pe->num_blocks > 0);
				--pe->num_blocks;
				TORRENT_ASSERT(m_read_cache_size > 0);
				--m_read_cache_size;
				--num;
			}

			if (pe->ok_to_evict())
			{
#ifdef TORRENT_DEBUG
				for (int j = 0; j < pe->blocks_in_piece; ++j)
					TORRENT_ASSERT(pe->blocks[j].buf == 0);
#endif
				i.next();

				move_to_ghost(pe);
			}
			else i.next();
		}
	}

	// if we can't evict enough blocks from the read cache, also
	// look at write cache pieces for blocks that have already
	// been written to disk and can be evicted
	// the first pass, we only evict blocks that have
	// been hashed, the second pass we flush anything
	// this is potentially a very expensive operation, since
	// we're likely to have iterate every single block in the
	// cache, and we might not get to evict anything.
	// TODO: this should probably only be done every n:th time
	if (num > 0 && m_read_cache_size > m_pinned_blocks)
	{
		for (int pass = 0; pass < 2 && num > 0; ++pass)
		{
			for (list_iterator i = m_lru[cached_piece_entry::write_lru].iterate(); i.get() && num > 0;)
			{
				cached_piece_entry* pe = reinterpret_cast<cached_piece_entry*>(i.get());

				if (pe == ignore)
				{
					i.next();
					continue;
				}

				if (pe->ok_to_evict())
				{
#ifdef TORRENT_DEBUG
					for (int j = 0; j < pe->blocks_in_piece; ++j)
						TORRENT_ASSERT(pe->blocks[j].buf == 0);
#endif
					TORRENT_ASSERT(pe->refcount == 0);
					i.next();
					erase_piece(pe);
					continue;
				}

				// all blocks in this piece are dirty
				if (pe->num_dirty == pe->num_blocks)
				{
					i.next();
					continue;
				}

				int end = pe->blocks_in_piece;

				// the first pass, only evict blocks that have been
				// hashed
				if (pass == 0 && pe->hash)
				  	end = pe->hash->offset / block_size();

				// go through the blocks and evict the ones
				// that are not dirty and not referenced
				for (int j = 0; j < end && num > 0; ++j)
				{
					cached_block_entry& b = pe->blocks[j];

					if (b.buf == 0 || b.refcount > 0 || b.dirty || b.pending) continue;

					to_delete[num_to_delete++] = b.buf;
					b.buf = 0;
					TORRENT_ASSERT(pe->num_blocks > 0);
					--pe->num_blocks;
					TORRENT_ASSERT(m_read_cache_size > 0);
					--m_read_cache_size;
					--num;
				}

				if (pe->ok_to_evict())
				{
#ifdef TORRENT_DEBUG
					for (int j = 0; j < pe->blocks_in_piece; ++j)
						TORRENT_ASSERT(pe->blocks[j].buf == 0);
#endif
					i.next();

					erase_piece(pe);
				}
				else i.next();
			}
		}
	}

	if (num_to_delete == 0) return num;

	DLOG(stderr, "[%p]    removed %d blocks\n", this, num_to_delete);

	free_multiple_buffers(to_delete, num_to_delete);

	return num;
}

void block_cache::clear(tailqueue& jobs)
{
	INVARIANT_CHECK;

	// this holds all the block buffers we want to free
	// at the end
	std::vector<char*> bufs;

	for (iterator p = m_pieces.begin()
		, end(m_pieces.end()); p != end; ++p)
	{
		// this also removes the jobs from the piece
		tailqueue_node* j = const_cast<tailqueue&>(p->jobs).get_all();
		while (j)
		{
			tailqueue_node* job = j;
			j = j->next;
			job->next = NULL;
			jobs.push_back(job);
		}

		drain_piece_bufs(const_cast<cached_piece_entry&>(*p), bufs);
	}

	if (!bufs.empty()) free_multiple_buffers(&bufs[0], bufs.size());

	// clear lru lists
	for (int i = 0; i < cached_piece_entry::num_lrus; ++i)
		m_lru[i].get_all();

	m_pieces.clear();
}

void block_cache::move_to_ghost(cached_piece_entry* pe)
{
	TORRENT_ASSERT(pe->refcount == 0);
	TORRENT_ASSERT(pe->piece_refcount == 0);
	TORRENT_ASSERT(pe->num_blocks == 0);

	if (pe->cache_state == cached_piece_entry::volatile_read_lru)
	{
		erase_piece(pe);
		return;
	}

	TORRENT_ASSERT(pe->cache_state == cached_piece_entry::read_lru1
		|| pe->cache_state == cached_piece_entry::read_lru2);

	// if the piece is in L1 or L2, move it into the ghost list
	// i.e. recently evicted
	if (pe->cache_state != cached_piece_entry::read_lru1
		&& pe->cache_state != cached_piece_entry::read_lru2)
		return;

	// if the ghost list is growing too big, remove the oldest entry
	linked_list* ghost_list = &m_lru[pe->cache_state + 1];
	while (ghost_list->size() >= m_ghost_size)
	{
		cached_piece_entry* p = (cached_piece_entry*)ghost_list->front();
		TORRENT_ASSERT(p != pe);
		TORRENT_ASSERT(p->num_blocks == 0);
		TORRENT_ASSERT(p->refcount == 0);
		TORRENT_ASSERT(p->piece_refcount == 0);
		erase_piece(p);
	}

	pe->storage->remove_piece(pe);
	m_lru[pe->cache_state].erase(pe);
	pe->cache_state += 1;
	ghost_list->push_back(pe);
}

int block_cache::pad_job(disk_io_job const* j, int blocks_in_piece
	, int read_ahead) const
{
	int block_offset = j->d.io.offset & (block_size()-1);
	int start = j->d.io.offset / block_size();
	int end = block_offset > 0 && (read_ahead > block_size() - block_offset) ? start + 2 : start + 1;

	// take the read-ahead into account
	// make sure to not overflow in this case
	if (read_ahead == INT_MAX) end = blocks_in_piece;
	else end = (std::min)(blocks_in_piece, (std::max)(start + read_ahead, end));

	return end - start;
}

// this function allocates buffers and
// fills in the iovec array with the buffers
int block_cache::allocate_iovec(file::iovec_t* iov, int iov_len)
{
	for (int i = 0; i < iov_len; ++i)
	{
		iov[i].iov_base = allocate_buffer("pending read");
		iov[i].iov_len = block_size();
		if (iov[i].iov_base == NULL)
		{
			// uh oh. We failed to allocate the buffer!
			// we need to roll back and free all the buffers
			// we've already allocated
			for (int j = 0; j < i; ++j)
				free_buffer((char*)iov[j].iov_base);
			return -1;
		}
	}
	return 0;
}

void block_cache::free_iovec(file::iovec_t* iov, int iov_len)
{
	for (int i = 0; i < iov_len; ++i)
		free_buffer((char*)iov[i].iov_base);
}

void block_cache::insert_blocks(cached_piece_entry* pe, int block, file::iovec_t *iov
	, int iov_len, disk_io_job* j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(pe);
	TORRENT_ASSERT(iov_len > 0);
	int start = block;

	cache_hit(pe, j->requester, j->flags & disk_io_job::volatile_read);

	for (int i = 0; i < iov_len; ++i)
	{
		// each iovec buffer has to be the size of a block (or the size of the last block)
		TORRENT_ASSERT(iov[i].iov_len == (std::min)(block_size()
			, pe->storage->files()->piece_size(pe->piece) - (start + i) * block_size()));

#ifdef TORRENT_DEBUG_BUFFERS
		TORRENT_ASSERT(is_disk_buffer((char*)iov[i].iov_base));
#endif

		// either free the block or insert it. Never replace a block
		if (pe->blocks[start + i].buf)
		{
			free_buffer((char*)iov[i].iov_base);
		}
		else
		{
			pe->blocks[start + i].buf = (char*)iov[i].iov_base;

#ifdef TORRENT_BUFFER_STATS
			rename_buffer(pe->blocks[start + i].buf, "read cache");
#endif
			TORRENT_ASSERT(iov[i].iov_base != NULL);
			TORRENT_ASSERT(pe->blocks[start + i].dirty == false);
			++pe->num_blocks;
			++m_read_cache_size;
		}
	}

	TORRENT_ASSERT(pe->cache_state != cached_piece_entry::read_lru1_ghost);
	TORRENT_ASSERT(pe->cache_state != cached_piece_entry::read_lru2_ghost);
}

void block_cache::inc_block_refcount(cached_piece_entry* pe, int block, int reason)
{
	TORRENT_ASSERT(pe->blocks[block].buf != NULL);
	++pe->blocks[block].refcount;
	if (pe->blocks[block].refcount == 1)
		++m_pinned_blocks;
	++pe->refcount;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	switch (reason)
	{
		case ref_hashing: ++pe->blocks[block].hashing_count; break;
		case ref_reading: ++pe->blocks[block].reading_count; break;
		case ref_flushing: ++pe->blocks[block].flushing_count; break;
	};
#endif
}

void block_cache::dec_block_refcount(cached_piece_entry* pe, int block, int reason)
{
	TORRENT_ASSERT(pe->blocks[block].buf != NULL);
	TORRENT_ASSERT(pe->blocks[block].refcount > 0);
	--pe->blocks[block].refcount;
	TORRENT_ASSERT(pe->refcount > 0);
	--pe->refcount;
	if (pe->blocks[block].refcount == 0)
	{
		TORRENT_ASSERT(m_pinned_blocks > 0);
		--m_pinned_blocks;
	}
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
	switch (reason)
	{
		case ref_hashing: --pe->blocks[block].hashing_count; break;
		case ref_reading: --pe->blocks[block].reading_count; break;
		case ref_flushing: --pe->blocks[block].flushing_count; break;
	};
#endif
}

void block_cache::abort_dirty(cached_piece_entry* pe)
{
	INVARIANT_CHECK;

	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (!pe->blocks[i].dirty
			|| pe->blocks[i].refcount > 0
			|| pe->blocks[i].buf == NULL) continue;

		TORRENT_ASSERT(!pe->blocks[i].pending);
		TORRENT_ASSERT(pe->blocks[i].dirty);
		free_buffer(pe->blocks[i].buf);
		pe->blocks[i].buf = 0;
		pe->blocks[i].dirty = false;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		TORRENT_ASSERT(m_write_cache_size > 0);
		--m_write_cache_size;
		TORRENT_ASSERT(pe->num_dirty > 0);
		--pe->num_dirty;
	}

	update_cache_state(pe);
}

// frees all buffers associated with this piece. May only
// be called for pieces with a refcount of 0
void block_cache::free_piece(cached_piece_entry* pe)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(pe->refcount == 0);
	TORRENT_ASSERT(pe->piece_refcount == 0);
	// build a vector of all the buffers we need to free
	// and free them all in one go
	char** to_delete = TORRENT_ALLOCA(char*, pe->blocks_in_piece);
	int num_to_delete = 0;
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (pe->blocks[i].buf == 0) continue;
		TORRENT_ASSERT(pe->blocks[i].pending == false);
		TORRENT_ASSERT(pe->blocks[i].refcount == 0);
		TORRENT_ASSERT(num_to_delete < pe->blocks_in_piece);
		to_delete[num_to_delete++] = pe->blocks[i].buf;
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		if (pe->blocks[i].dirty)
		{
			TORRENT_ASSERT(m_write_cache_size > 0);
			--m_write_cache_size;
			TORRENT_ASSERT(pe->num_dirty > 0);
			--pe->num_dirty;
		}
		else
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
	}
	if (num_to_delete) free_multiple_buffers(to_delete, num_to_delete);
	update_cache_state(pe);
}

int block_cache::drain_piece_bufs(cached_piece_entry& p, std::vector<char*>& buf)
{
	int piece_size = p.storage->files()->piece_size(p.piece);
	int blocks_in_piece = (piece_size + block_size() - 1) / block_size();
	int ret = 0;

	for (int i = 0; i < blocks_in_piece; ++i)
	{
		if (p.blocks[i].buf == 0) continue;
		TORRENT_ASSERT(p.blocks[i].refcount == 0);
		buf.push_back(p.blocks[i].buf);
		++ret;
		p.blocks[i].buf = 0;
		TORRENT_ASSERT(p.num_blocks > 0);
		--p.num_blocks;

		if (p.blocks[i].dirty)
		{
			TORRENT_ASSERT(m_write_cache_size > 0);
			--m_write_cache_size;
			TORRENT_ASSERT(p.num_dirty > 0);
			--p.num_dirty;
		}
		else
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
	}
	update_cache_state(&p);
	return ret;
}

void block_cache::get_stats(cache_status* ret) const
{
	ret->blocks_read_hit = m_blocks_read_hit;
	ret->write_cache_size = m_write_cache_size;
	ret->read_cache_size = m_read_cache_size;
	ret->pinned_blocks = m_pinned_blocks;
#ifndef TORRENT_NO_DEPRECATE
	ret->cache_size = m_read_cache_size + m_write_cache_size;
#endif

	ret->arc_mru_size = m_lru[cached_piece_entry::read_lru1].size();
	ret->arc_mru_ghost_size = m_lru[cached_piece_entry::read_lru1_ghost].size();
	ret->arc_mfu_size = m_lru[cached_piece_entry::read_lru2].size();
	ret->arc_mfu_ghost_size = m_lru[cached_piece_entry::read_lru2_ghost].size();
	ret->arc_write_size = m_lru[cached_piece_entry::write_lru].size();
	ret->arc_volatile_size = m_lru[cached_piece_entry::volatile_read_lru].size();
}

void block_cache::set_settings(aux::session_settings const& sett)
{
	// the ghost size is the number of pieces to keep track of
	// after they are evicted. Since cache_size is blocks, the
	// assumption is that there are about 128 blocks per piece,
	// and there are two ghost lists, so divide by 2.

	m_ghost_size = (std::max)(8, sett.get_int(settings_pack::cache_size)
		/ (std::max)(sett.get_int(settings_pack::read_cache_line_size), 4) / 2);
	disk_buffer_pool::set_settings(sett);
}

#ifdef TORRENT_DEBUG
void block_cache::check_invariant() const
{
	int cached_write_blocks = 0;
	int cached_read_blocks = 0;
	int num_pinned = 0;

	std::set<piece_manager*> storages;

	for (int i = 0; i < cached_piece_entry::num_lrus; ++i)
	{
		ptime timeout = min_time();

		for (list_iterator p = m_lru[i].iterate(); p.get(); p.next())
		{
			cached_piece_entry* pe = (cached_piece_entry*)p.get();
			TORRENT_ASSERT(pe->cache_state == i);
			if (pe->num_dirty > 0)
				TORRENT_ASSERT(i == cached_piece_entry::write_lru);

//			if (i == cached_piece_entry::write_lru)
//				TORRENT_ASSERT(pe->num_dirty > 0);

			if (i != cached_piece_entry::read_lru1_ghost
				&& i != cached_piece_entry::read_lru2_ghost)
			{
				TORRENT_ASSERT(pe->storage->has_piece(pe));
				TORRENT_ASSERT(pe->expire >= timeout);
				timeout = pe->expire;
			}
			else
			{
				// pieces in the ghost lists should never have any blocks
				TORRENT_ASSERT(pe->num_blocks == 0);
				TORRENT_ASSERT(pe->storage->has_piece(pe) == false);
			}

			storages.insert(pe->storage.get());
		}
	}

	for (std::set<piece_manager*>::iterator i = storages.begin()
		, end(storages.end()); i != end; ++i)
	{
		for (boost::unordered_set<cached_piece_entry*>::iterator j = (*i)->cached_pieces().begin()
			, end((*i)->cached_pieces().end()); j != end; ++j)
		{
			cached_piece_entry* pe = *j;
			TORRENT_ASSERT(pe->storage == *i);
		}
	}

	boost::unordered_set<char*> buffers;
	for (iterator i = m_pieces.begin(), end(m_pieces.end()); i != end; ++i)
	{
		cached_piece_entry const& p = *i;
		TORRENT_ASSERT(p.blocks);
		
		TORRENT_ASSERT(p.storage);
		int piece_size = p.storage->files()->piece_size(p.piece);
		int blocks_in_piece = (piece_size + block_size() - 1) / block_size();
		int num_blocks = 0;
		int num_dirty = 0;
		int num_pending = 0;
		int num_refcount = 0;
		TORRENT_ASSERT(blocks_in_piece == p.blocks_in_piece);
		for (int k = 0; k < blocks_in_piece; ++k)
		{
			if (p.blocks[k].buf)
			{
#if !defined TORRENT_DISABLE_POOL_ALLOCATOR && defined TORRENT_EXPENSIVE_INVARIANT_CHECKS
				TORRENT_ASSERT(is_disk_buffer(p.blocks[k].buf));

				// make sure we don't have the same buffer
				// in the cache twice
				TORRENT_ASSERT(buffers.count(p.blocks[k].buf) == 0);
				buffers.insert(p.blocks[k].buf);
#endif
				++num_blocks;
				if (p.blocks[k].dirty)
				{
					++num_dirty;
					++cached_write_blocks;
				}
				else
				{
					++cached_read_blocks;
				}
				if (p.blocks[k].pending) ++num_pending;
				if (p.blocks[k].refcount > 0) ++num_pinned;
			}
			else
			{
				TORRENT_ASSERT(!p.blocks[k].dirty);
				TORRENT_ASSERT(!p.blocks[k].pending);
				TORRENT_ASSERT(p.blocks[k].refcount == 0);
			}
			TORRENT_ASSERT(p.blocks[k].refcount >= 0);
			num_refcount += p.blocks[k].refcount;
		}
		TORRENT_ASSERT(num_blocks == p.num_blocks);
		TORRENT_ASSERT(num_pending <= p.refcount);
		TORRENT_ASSERT(num_refcount == p.refcount);
		TORRENT_ASSERT(num_dirty == p.num_dirty);
	}
	TORRENT_ASSERT(m_read_cache_size == cached_read_blocks);
	TORRENT_ASSERT(m_write_cache_size == cached_write_blocks);
	TORRENT_ASSERT(m_pinned_blocks == num_pinned);
	TORRENT_ASSERT(m_write_cache_size + m_read_cache_size <= in_use());

#ifdef TORRENT_BUFFER_STATS
	int read_allocs = m_categories.find(std::string("read cache"))->second;
	int write_allocs = m_categories.find(std::string("write cache"))->second;
	TORRENT_ASSERT(cached_read_blocks == read_allocs);
	TORRENT_ASSERT(cached_write_blocks == write_allocs);
#endif
}
#endif

// returns
// -1: block not in cache
// -2: out of memory

int block_cache::copy_from_piece(cached_piece_entry* pe, disk_io_job* j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j->buffer == 0);

	// copy from the cache and update the last use timestamp
	int block = j->d.io.offset / block_size();
	int block_offset = j->d.io.offset & (block_size()-1);
	int buffer_offset = 0;
	int size = j->d.io.buffer_size;
	int blocks_to_read = block_offset > 0 && (size > block_size() - block_offset) ? 2 : 1;
	TORRENT_ASSERT(size <= block_size());
	int start_block = block;

	cached_block_entry* bl = &pe->blocks[start_block];

	if (bl->buf != 0
		&& blocks_to_read > 1)
	{
		++start_block;
		++bl;
	}

#ifdef TORRENT_DEBUG	
	int piece_size = j->storage->files()->piece_size(j->piece);
	int blocks_in_piece = (piece_size + block_size() - 1) / block_size();
	TORRENT_ASSERT(start_block < blocks_in_piece);
#endif

	// if there's no buffer, we don't have this block in
	// the cache, and we're not currently reading it in either
	// since it's not pending
	if (pe->blocks[start_block].buf == 0) return -1;

	// if block_offset > 0, we need to read two blocks, and then
	// copy parts of both, because it's not aligned to the block
	// boundaries
	if (blocks_to_read == 1 && (j->flags & disk_io_job::force_copy) == 0)
	{
		// special case for block aligned request
		// don't actually copy the buffer, just reference
		// the existing block
		if (bl->refcount == 0) ++m_pinned_blocks;
		++pe->blocks[start_block].refcount;
		TORRENT_ASSERT(pe->blocks[start_block].refcount > 0); // make sure it didn't wrap
		++pe->refcount;
		TORRENT_ASSERT(pe->refcount > 0); // make sure it didn't wrap
		j->d.io.ref.storage = j->storage.get();
		j->d.io.ref.piece = pe->piece;
		j->d.io.ref.block = start_block;
		j->buffer = bl->buf + (j->d.io.offset & (block_size()-1));
		++m_send_buffer_blocks;
#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		++pe->blocks[start_block].reading_count;
#endif
		return j->d.io.buffer_size;
	}

	j->buffer = allocate_buffer("send buffer");
	if (j->buffer == 0) return -2;

	while (size > 0)
	{
		TORRENT_ASSERT(pe->blocks[block].buf);
		int to_copy = (std::min)(block_size()
			- block_offset, size);
		std::memcpy(j->buffer + buffer_offset
			, pe->blocks[block].buf + block_offset
			, to_copy);
		++pe->blocks[block].hitcount;
		size -= to_copy;
		block_offset = 0;
		buffer_offset += to_copy;
		++block;
	}
	return j->d.io.buffer_size;
}

void block_cache::reclaim_block(block_cache_reference const& ref)
{
	cached_piece_entry* pe = find_piece(ref);
	if (pe == NULL) return;

	TORRENT_ASSERT(pe->blocks[ref.block].buf);
	dec_block_refcount(pe, ref.block, block_cache::ref_reading);

	TORRENT_ASSERT(m_send_buffer_blocks > 0);
	--m_send_buffer_blocks;

	maybe_free_piece(pe);
}

bool block_cache::maybe_free_piece(cached_piece_entry* pe)
{
	if (!pe->ok_to_evict()
		|| !pe->marked_for_deletion
		|| !pe->jobs.empty())
		return false;

	boost::intrusive_ptr<piece_manager> s = pe->storage;

	DLOG(stderr, "[%p] block_cache maybe_free_piece "
		"piece: %d refcount: %d marked_for_deletion: %d\n", this
		, int(pe->piece), int(pe->refcount), int(pe->marked_for_deletion));

	tailqueue jobs;
	bool removed = evict_piece(pe, jobs);
	TORRENT_ASSERT(removed);
	TORRENT_ASSERT(jobs.empty());

	return true;
}

cached_piece_entry* block_cache::find_piece(block_cache_reference const& ref)
{
	return find_piece((piece_manager*)ref.storage, ref.piece);
}

cached_piece_entry* block_cache::find_piece(disk_io_job const* j)
{
	return find_piece(j->storage.get(), j->piece);
}

cached_piece_entry* block_cache::find_piece(piece_manager* st, int piece)
{
	cached_piece_entry model;
	model.storage = st;
	model.piece = piece;
	iterator i = m_pieces.find(model);
	TORRENT_ASSERT(i == m_pieces.end() || (i->storage == st && i->piece == piece));
	if (i == m_pieces.end()) return 0;
	return const_cast<cached_piece_entry*>(&*i);
}

}

