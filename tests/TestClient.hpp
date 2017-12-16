/*
  This file is part of Ingen.
  Copyright 2007-2017 David Robillard <http://drobilla.net/>

  Ingen is free software: you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free
  Software Foundation, either version 3 of the License, or any later version.

  Ingen is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for details.

  You should have received a copy of the GNU Affero General Public License
  along with Ingen.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef INGEN_TESTCLIENT_HPP
#define INGEN_TESTCLIENT_HPP

#include <boost/variant.hpp>

#include "ingen/Interface.hpp"

using namespace Ingen;

class TestClient : public Ingen::Interface
{
public:
	explicit TestClient(Log& log) : _log(log) {}
	~TestClient() {}

	Raul::URI uri() const { return Raul::URI("ingen:testClient"); }

	void set_response_id(int32_t id) override {}

	void message(const Message& msg) override {
		if (const Response* const response = boost::get<Response>(&msg)) {
			if (response->status != Status::SUCCESS) {
				_log.error(fmt("error on message %1%: %2% (%3%)\n")
				           % response->id
				           % ingen_status_string(response->status)
				           % response->subject);
				exit(EXIT_FAILURE);
			}
		} else if (const Error* const error = boost::get<Error>(&msg)) {
			_log.error(fmt("error: %1%\n") % error->message);
			exit(EXIT_FAILURE);
		}
	}

private:
	Log& _log;
};

#endif // INGEN_TESTCLIENT_HPP
