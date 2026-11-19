#include "tagviewcontracts.h"

namespace {
/// Set by the program at startup (tqe installs its ctags index; gallery_info
/// installs nothing). One per process, which is what "the source every editor
/// uses" means.
SuggestionSource* g_source=nullptr;
}

SuggestionSource* suggestionSource()
{
    return g_source;
}

void setSuggestionSource(SuggestionSource* source)
{
    g_source=source;
}
