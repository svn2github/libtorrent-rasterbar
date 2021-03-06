/*

Copyright (c) 2005, Arvid Norberg
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

#ifndef TORRENT_IP_FILTER_HPP
#define TORRENT_IP_FILTER_HPP

#ifdef _MSC_VER
#pragma warning(push, 1)
#endif

#include <boost/limits.hpp>
#include <boost/utility.hpp>
#include <boost/tuple/tuple.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif


#include "libtorrent/config.hpp"
#include "libtorrent/socket.hpp"
#include <set>
#include <iostream>

namespace libtorrent
{

inline bool operator<=(address const& lhs
	, address const& rhs)
{
	return lhs < rhs || lhs == rhs;
}

template <class Addr>
struct ip_range
{
	Addr first;
	Addr last;
	int flags;
};

namespace detail
{

	// this is the generic implementation of
	// a filter for a specific address type.
	// it works with IPv4 and IPv6
	template<class Addr>
	class filter_impl
	{
	public:

		filter_impl()
		{
			typename Addr::bytes_type zero;
			std::fill(zero.begin(), zero.end(), 0);
			// make the entire ip-range non-blocked
			m_access_list.insert(range(Addr(zero), 0));
		}

		void add_rule(Addr first, Addr last, int flags)
		{
			using boost::next;
			using boost::prior;

			assert(!m_access_list.empty());
			assert(first < last || first == last);
			
			typename range_t::iterator i = m_access_list.upper_bound(first);
			typename range_t::iterator j = m_access_list.upper_bound(last);

			if (i != m_access_list.begin()) --i;
			
			assert(j != m_access_list.begin());
			assert(j != i);
			
			int first_access = i->access;
			int last_access = prior(j)->access;

			if (i->start != first && first_access != flags)
			{
				i = m_access_list.insert(i, range(first, flags));
			}
			else if (i != m_access_list.begin() && prior(i)->access == flags)
			{
				--i;
				first_access = i->access;
			}
			assert(!m_access_list.empty());
			assert(i != m_access_list.end());

			if (i != j) m_access_list.erase(next(i), j);
			if (i->start == first)
			{
				// we can do this const-cast because we know that the new
				// start address will keep the set correctly ordered
				const_cast<Addr&>(i->start) = first;
				const_cast<int&>(i->access) = flags;
			}
			else if (first_access != flags)
			{
				m_access_list.insert(i, range(first, flags));
			}
			
			if ((j != m_access_list.end()
					&& minus_one(j->start) != last)
				|| (j == m_access_list.end()
					&& last != max_addr()))
			{
				assert(j == m_access_list.end() || last < minus_one(j->start));
				if (last_access != flags)
					j = m_access_list.insert(j, range(plus_one(last), last_access));
			}

			if (j != m_access_list.end() && j->access == flags) m_access_list.erase(j);
			assert(!m_access_list.empty());
		}

		int access(Addr const& addr) const
		{
			assert(!m_access_list.empty());
			typename range_t::const_iterator i = m_access_list.upper_bound(addr);
			if (i != m_access_list.begin()) --i;
			assert(i != m_access_list.end());
			assert(i->start <= addr && (boost::next(i) == m_access_list.end()
				|| addr < boost::next(i)->start));
			return i->access;
		}

		std::vector<ip_range<Addr> > export_filter() const
		{
			std::vector<ip_range<Addr> > ret;
			ret.reserve(m_access_list.size());

			for (typename range_t::const_iterator i = m_access_list.begin()
				, end(m_access_list.end()); i != end;)
			{
				ip_range<Addr> r;
				r.first = i->start;
				r.flags = i->access;

				++i;
				if (i == end)
					r.last = max_addr();
				else
					r.last = minus_one(i->start);
			
				ret.push_back(r);
			}
			return ret;
		}

	private:
	
		Addr plus_one(Addr const& a) const
		{
			typename Addr::bytes_type tmp(a.to_bytes());
			typedef typename Addr::bytes_type::reverse_iterator iter;
			for (iter i = tmp.rbegin()
				, end(tmp.rend()); i != end; ++i)
			{
				if (*i < (std::numeric_limits<typename iter::value_type>::max)())
				{
					*i += 1;
					break;
				}
				*i = 0;
			}
			return Addr(tmp);
		}
		
		Addr minus_one(Addr const& a) const
		{
			typename Addr::bytes_type tmp(a.to_bytes());
			typedef typename Addr::bytes_type::reverse_iterator iter;
			for (iter i = tmp.rbegin()
				, end(tmp.rend()); i != end; ++i)
			{
				if (*i > 0)
				{
					*i -= 1;
					break;
				}
				*i = (std::numeric_limits<typename iter::value_type>::max)();
			}
			return Addr(tmp);
		}

		Addr max_addr() const
		{
			typename Addr::bytes_type tmp;
			std::fill(tmp.begin(), tmp.end()
				, (std::numeric_limits<typename Addr::bytes_type::value_type>::max)());
			return Addr(tmp);
		}
	
		struct range
		{
			range(Addr addr, int access = 0): start(addr), access(access) {}
			bool operator<(range const& r) const
			{ return start < r.start; }
			bool operator<(Addr const& a) const
			{ return start < a; }
			Addr start;
			// the end of the range is implicit
			// and given by the next entry in the set
			int access;
		};

		typedef std::set<range> range_t;
		range_t m_access_list;
	
	};

}

class TORRENT_EXPORT ip_filter
{
public:

	enum access_flags
	{
		blocked = 1
	};

	// both addresses MUST be of the same type (i.e. both must
	// be either IPv4 or both must be IPv6)
	void add_rule(address first, address last, int flags);
	int access(address const& addr) const;

	typedef boost::tuple<std::vector<ip_range<address_v4> >
		, std::vector<ip_range<address_v6> > > filter_tuple_t;
	
	filter_tuple_t export_filter() const;

//	void print() const;
	
private:

	detail::filter_impl<address_v4> m_filter4;
	detail::filter_impl<address_v6> m_filter6;
};

}

#endif

