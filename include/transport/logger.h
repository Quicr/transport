#pragma once
#include <optional>

namespace qtransport {

	/**
	 * @brief Transport logger level
	 */
	enum struct LogLevel : uint8_t {
		fatal = 1,
		error = 2,
		info = 3,
		warn = 4,
		debug = 5,
	};

	struct LogHandler {
		// log() provides logs to the application.  The default implementation is a
		// noop; the inputs are ignored.
		virtual void log(LogLevel /*level*/, const std::string & /*message*/) {}
	};
}