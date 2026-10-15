// Standalone dxso: DXVK defines its logger instance per DLL; here, once.
#include "log/log.h"
namespace dxvk {
  Logger Logger::s_instance("dxso.log");
}
