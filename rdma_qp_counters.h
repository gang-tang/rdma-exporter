#pragma once
#include "raw_rotating_logger.h"

// Count RDMA QPs per ib_device and per QP state ("rdma resource show qp") and
// flush them as influx lines: ib_qp_counters,ib_device=X total=N,STATE=n,... <ts_ns>
void exportRdmaQpCounters(RotatingFileWriter &logger);
