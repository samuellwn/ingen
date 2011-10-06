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

#include "ingen/Patch.hpp"

#include "Buffer.hpp"
#include "NodeImpl.hpp"
#include "OutputPort.hpp"
#include "ProcessContext.hpp"
#include "shared/LV2URIMap.hpp"

using namespace std;

namespace Ingen {
namespace Server {

OutputPort::OutputPort(BufferFactory&      bufs,
                       NodeImpl*           parent,
                       const Raul::Symbol& symbol,
                       uint32_t            index,
                       uint32_t            poly,
                       PortType            type,
                       const Raul::Atom&   value,
                       size_t              buffer_size)
	: PortImpl(bufs, parent, symbol, index, poly, type, value, buffer_size)
{
	if (!dynamic_cast<Patch*>(parent))
		add_property(bufs.uris().rdf_type, bufs.uris().lv2_OutputPort);

	_broadcast = true;

	setup_buffers(bufs, poly);
}

bool
OutputPort::get_buffers(BufferFactory&                   bufs,
                        Raul::Array<BufferFactory::Ref>* buffers,
                        uint32_t                         poly) const
{
	for (uint32_t v = 0; v < poly; ++v)
		buffers->at(v) = bufs.get(buffer_type(), _buffer_size);

	return true;
}

void
OutputPort::pre_process(Context& context)
{
	for (uint32_t v = 0; v < _poly; ++v)
		_buffers->at(v)->prepare_write(context);
}

void
OutputPort::post_process(Context& context)
{
	for (uint32_t v = 0; v < _poly; ++v)
		_buffers->at(v)->prepare_read(context);

	if (_broadcast)
		broadcast_value(context, false);
}

} // namespace Server
} // namespace Ingen
