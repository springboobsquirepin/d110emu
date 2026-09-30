// Builds of the plugins without a window (the tests' on Linux): the host sees no editor.

#include "PluginCore.h"

std::unique_ptr<PluginEditor> createPluginEditor(PluginCore& /*core*/) {
    return nullptr;
}
