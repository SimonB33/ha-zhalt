// passthrough.h
#pragma once

namespace passthrough {
void begin();   // listen on TCP :81 (Ethernet side)
void poll();    // from loop(): accept a client and queue a Passthrough job
void run();     // from the executor: join, splice to the unit, leave
bool active();
}  // namespace passthrough
