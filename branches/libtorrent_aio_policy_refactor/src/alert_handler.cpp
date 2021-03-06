/*

Copyright (c) 2012, Arvid Norberg
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

#include "libtorrent/config.hpp"
#include "libtorrent/alert_handler.hpp"
#include "libtorrent/alert_observer.hpp"

#include <algorithm>
#include <stdarg.h>

namespace libtorrent
{

	void alert_handler::subscribe(alert_observer* o, int flags, ...)
	{
		int types[20];
		memset(types, 0, sizeof(types));
		va_list l;
		va_start(l, flags);
		int t = va_arg(l, int);
		int i = 0;
		while (t != 0 && i < 20)
		{
			types[i] = t;
			++i;
			t = va_arg(l, int);
		}
		va_end(l);
		subscribe_impl(types, i, o, flags);
	}

	void alert_handler::dispatch_alerts(std::deque<alert*>& alerts) const
	{
		for (std::deque<alert*>::const_iterator i = alerts.begin()
			, end(alerts.end()); i != end; ++i)
		{
			alert* a = *i;
			int type = a->type();
			std::vector<alert_observer*> const& alert_dispatchers = m_observers[type];
			{
				for (std::vector<alert_observer*>::const_iterator k = alert_dispatchers.begin()
					, end(alert_dispatchers.end()); k != end; ++k)
				{
					(*k)->handle_alert(a);
				}
			}
			delete a;
		}
		alerts.clear();
	}

	void alert_handler::unsubscribe(alert_observer* o)
	{
		for (int i = 0; i < o->num_types; ++i)
		{
			int type = o->types[i];
			if (type == 0) continue;
			TORRENT_ASSERT(type >= 0);
			TORRENT_ASSERT(type < sizeof(m_observers)/sizeof(m_observers[0]));
			if (type < 0 || type >= sizeof(m_observers)/sizeof(m_observers[0])) continue;
			std::vector<alert_observer*>& alert_observers = m_observers[type];
			std::vector<alert_observer*>::iterator j = std::find(alert_observers.begin()
				, alert_observers.end(), o);
			if (j != alert_observers.end()) alert_observers.erase(j);
		}
		o->num_types = 0;
	}

	void alert_handler::subscribe_impl(int const* type_list, int num_types, alert_observer* o, int flags)
	{
		memset(o->types, 0, sizeof(o->types));
		o->flags = flags;
		for (int i = 0; i < num_types; ++i)
		{
			int type = type_list[i];
			if (type == 0) break;

			// only subscribe once per observer per type
			if (std::count(o->types, o->types + o->num_types, type) > 0) continue;

			TORRENT_ASSERT(type >= 0);
			TORRENT_ASSERT(type < sizeof(m_observers)/sizeof(m_observers[0]));
			if (type < 0 || type >= sizeof(m_observers)/sizeof(m_observers[0])) continue;

			o->types[o->num_types++] = type;
			m_observers[type].push_back(o);
			TORRENT_ASSERT(o->num_types < 20);
		}
	}
}

