#ifndef RUN_UDP_LISTENER_H
#define RUN_UDP_LISTENER_H
#include "configs.h"
#include "MiningCore.h"

void runUDPListener(void *name, volatile bool &displayDirty, MinerStats (&miners)[MAX_MINERS + 1]);


#endif