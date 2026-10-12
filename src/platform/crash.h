#pragma once
// On an unhandled exception (access violation etc.): logs the exception and a symbolized stack (the PDB next to the
// exe), and writes a minidump to %LOCALAPPDATA%\minecraftCplusplus\crashes. Call first thing in wmain.
#include "core/common.h"

void installCrashHandler();
// Extra lines a crash logs ("Custom Server Details" of ClientboundCustomReportDetailsPacket); any thread.
void setCrashDetails(const std::string& text);
