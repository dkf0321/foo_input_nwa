#include "input_nwa.h"

DECLARE_COMPONENT_VERSION(
    "NWA Audio Decoder",
    "0.1",
    "This is a FB2K component for decoding NWA files.\n\nauthor: dkf0321\nhttps://github.com/dkf0321\n\nfoobar2000 SDK 2025-03-07\nCopyright (c) 2002-2025, Peter Pawlowski\n\nMSVC v19.51.36246\nCopyright (c) 2026 Microsoft Corporation\n\nThis component references the following project(s):\n\nnwa2wav\nCopyright © 2020 Leonhart231\nhttps://gitlab.com/Leonhart231/nwa2wav\n"
);

VALIDATE_COMPONENT_FILENAME("foo_input_nwa.dll");

input_factory_t<input_nwa> g_input_nwa_factory;
