/* This file is part of Ingen.
 * Copyright 2007-2011 David Robillard <http://drobilla.net>
 *
 * Ingen is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef INGEN_EVENTS_MOVE_HPP
#define INGEN_EVENTS_MOVE_HPP

#include "raul/Path.hpp"
#include "Event.hpp"
#include "EngineStore.hpp"

namespace Ingen {
namespace Server {

class PatchImpl;

namespace Events {

/** \page methods
 * <h2>MOVE</h2>
 * As per WebDAV (RFC4918 S9.9).
 *
 * Move an object from its current location and insert it at a new location
 * in a single operation.
 *
 * MOVE to a path with a different parent is currently not supported.
 */

/** MOVE a graph object to a new path (see \ref methods).
 * \ingroup engine
 */
class Move : public Event
{
public:
	Move(Engine&           engine,
	     ClientInterface*  client,
	     int32_t           id,
	     SampleCount       timestamp,
	     const Raul::Path& old_path,
	     const Raul::Path& new_path);

	~Move();

	void pre_process();
	void execute(ProcessContext& context);
	void post_process();

private:
	Raul::Path            _old_path;
	Raul::Path            _new_path;
	PatchImpl*            _parent_patch;
	EngineStore::iterator _store_iterator;
};

} // namespace Server
} // namespace Ingen
} // namespace Events

#endif // INGEN_EVENTS_MOVE_HPP
