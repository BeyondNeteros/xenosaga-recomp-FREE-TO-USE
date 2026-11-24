// Link this fixture once with the runtime header and once with a mismatched
// VU1Interpreter layout. Only the matching registration should link.
#include "runtime/ps2_vu1.h"

int main()
{
    VU1Interpreter::registerCompatibleBlocks(nullptr, 0);
}
