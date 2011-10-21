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

#include <glibmm/thread.h>

#include "raul/Path.hpp"

#include "ClientBroadcaster.hpp"
#include "Driver.hpp"
#include "Engine.hpp"
#include "EngineStore.hpp"
#include "NodeImpl.hpp"
#include "PatchImpl.hpp"
#include "Request.hpp"
#include "events/Move.hpp"

using namespace std;
using namespace Raul;

namespace Ingen {
namespace Server {
namespace Events {

Move::Move(Engine& engine, SharedPtr<Request> request, SampleCount timestamp, const Path& path, const Path& new_path)
	: Event(engine, request, timestamp)
	, _old_path(path)
	, _new_path(new_path)
	, _parent_patch(NULL)
	, _store_iterator(engine.engine_store()->end())
{
}

Move::~Move()
{
}

void
Move::pre_process()
{
	Glib::RWLock::WriterLock lock(_engine.engine_store()->lock());

	if (!_old_path.parent().is_parent_of(_new_path)) {
		_error = PARENT_DIFFERS;
		Event::pre_process();
		return;
	}
	_store_iterator = _engine.engine_store()->find(_old_path);
	if (_store_iterator == _engine.engine_store()->end())  {
		_error = OBJECT_NOT_FOUND;
		Event::pre_process();
		return;
	}

	if (_engine.engine_store()->find_object(_new_path))  {
		_error = OBJECT_EXISTS;
		Event::pre_process();
		return;
	}

	SharedPtr< Table<Path, SharedPtr<GraphObject> > > removed
			= _engine.engine_store()->remove(_store_iterator);

	assert(removed->size() > 0);

	for (Table<Path, SharedPtr<GraphObject> >::iterator i = removed->begin(); i != removed->end(); ++i) {
		const Path& child_old_path = i->first;
		assert(Path::descendant_comparator(_old_path, child_old_path));

		Path child_new_path;
		if (child_old_path == _old_path)
			child_new_path = _new_path;
		else
			child_new_path = Path(_new_path).base() + child_old_path.substr(_old_path.length()+1);

		PtrCast<GraphObjectImpl>(i->second)->set_path(child_new_path);
		i->first = child_new_path;
	}

	_engine.engine_store()->add(*removed.get());

	Event::pre_process();
}

void
Move::execute(ProcessContext& context)
{
	Event::execute(context);

	SharedPtr<PortImpl> port = PtrCast<PortImpl>(_store_iterator->second);
	if (port && port->parent()->parent() == NULL) {
		DriverPort* driver_port = _engine.driver()->driver_port(_new_path);
		if (driver_port)
			driver_port->move(_new_path);
	}
}

void
Move::post_process()
{
	string msg = "Unable to rename object - ";

	if (_error == NO_ERROR) {
		_request->respond_ok();
		_engine.broadcaster()->move(_old_path, _new_path);
	} else {
		if (_error == OBJECT_EXISTS)
			msg.append("Object already exists at ").append(_new_path.str());
		else if (_error == OBJECT_NOT_FOUND)
			msg.append("Could not find object ").append(_old_path.str());
		else if (_error == OBJECT_NOT_RENAMABLE)
			msg.append(_old_path.str()).append(" is not renamable");
		else if (_error == PARENT_DIFFERS)
			msg.append(_new_path.str()).append(" is a child of a different patch");

		_request->respond_error(msg);
	}
}

} // namespace Server
} // namespace Ingen
} // namespace Events
