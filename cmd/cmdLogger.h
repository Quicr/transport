#pragma once
#include <mutex>
#include <transport/logger.h>

namespace qtransport {

class cmdLogger : public LogHandler {
public:
  void log(LogLevel level, const std::string &string) override;

private:
  std::mutex mutex;
};

}; // namespace qtransport
