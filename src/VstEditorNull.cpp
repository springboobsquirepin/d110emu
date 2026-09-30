// Builds of the plugin without a window (the tests' on Linux): the host sees no editor.

#include "VstPlugin.h"

std::unique_ptr<VstEditor> createVstEditor(VstPlugin& /*plugin*/) {
    return nullptr;
}
