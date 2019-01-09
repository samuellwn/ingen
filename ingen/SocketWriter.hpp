/*
  This file is part of Ingen.
  Copyright 2012-2015 David Robillard <http://drobilla.net/>

  Ingen is free software: you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free
  Software Foundation, either version 3 of the License, or any later version.

  Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for details.

  You should have received a copy of the GNU Affero General Public License
  along with Ingen.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef INGEN_SOCKET_WRITER_HPP
#define INGEN_SOCKET_WRITER_HPP

#include <cstddef>
#include <cstdint>

#include "ingen/TurtleWriter.hpp"
#include "ingen/ingen.h"

namespace Raul {
class Socket;
}

namespace ingen {

class URI;
class URIMap;
class URIs;

/** An Interface that writes Turtle messages to a socket.
 */
class INGEN_API SocketWriter : public TurtleWriter
{
public:
	SocketWriter(URIMap&            map,
	             URIs&              uris,
	             const URI&         uri,
	             SPtr<Raul::Socket> sock);

	void message(const Message& message) override;

	size_t text_sink(const void* buf, size_t len) override;

protected:
	SPtr<Raul::Socket> _socket;
};

}  // namespace ingen

#endif  // INGEN_SOCKET_WRITER_HPP
