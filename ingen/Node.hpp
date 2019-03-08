/*
  This file is part of Ingen.
  Copyright 2007-2015 David Robillard <http://drobilla.net/>

  Ingen is free software: you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free
  Software Foundation, either version 3 of the License, or any later version.

  Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for details.

  You should have received a copy of the GNU Affero General Public License
  along with Ingen.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef INGEN_NODE_HPP
#define INGEN_NODE_HPP

#include "ingen/Resource.hpp"
#include "ingen/ingen.h"
#include "ingen/paths.hpp"
#include "ingen/types.hpp"
#include "lilv/lilv.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace Raul {
class Path;
class Symbol;
}

namespace ingen {

class Arc;
class FilePath;
class Store;
class URIs;

/** A node in the audio graph.
 *
 * The key property of nodes is that all nodes have a path and a symbol, as
 * well as a URI.
 *
 * To avoid ugly inheritance issues and the need for excessive use of
 * dynamic_cast, this class contains some members which are only applicable to
 * certain types of node.  There is a type tag which can be used to determine
 * the type of any Node.
 *
 * @ingroup Ingen
 */
class INGEN_API Node : public Resource
{
public:
	enum class GraphType {
		GRAPH,
		BLOCK,
		PORT
	};

	typedef std::pair<const Node*, const Node*> ArcsKey;
	typedef std::map< ArcsKey, SPtr<Arc> > Arcs;

	// Graphs only
	Arcs&       arcs()       { return _arcs; }
	const Arcs& arcs() const { return _arcs; }

	// Blocks and graphs only
	virtual uint32_t        num_ports()          const { return 0; }
	virtual Node*           port(uint32_t index) const { return nullptr; }
	virtual const Resource* plugin()             const { return nullptr; }

	// Plugin blocks only
	virtual LilvInstance* instance() { return nullptr; }
	virtual bool save_state(const FilePath& dir) const { return false; }

	// All objects
	virtual GraphType           graph_type()   const = 0;
	virtual const Raul::Path&   path()         const = 0;
	virtual const Raul::Symbol& symbol()       const = 0;
	virtual Node*               graph_parent() const = 0;

	URI base_uri() const {
		if (uri().string()[uri().string().size() - 1] == '/') {
			return uri();
		}
		return URI(uri().string() + '/');
	}

protected:
	friend class Store;
	virtual void set_path(const Raul::Path& p) = 0;

	Node(const URIs& uris, const Raul::Path& path)
		: Resource(uris, path_to_uri(path))
	{}

	Arcs _arcs;  ///< Graphs only
};

} // namespace ingen

#endif // INGEN_NODE_HPP
