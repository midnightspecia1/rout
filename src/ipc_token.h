#pragma once

#include <string>

// Adapters prove they run as the same user by presenting a token they
// can only get from a file only that user can read.
//
// Creates a fresh random token for this run (hex, 64 chars) and writes
// it to %LOCALAPPDATA%\rout\ipc-token with a protected DACL (owner and
// SYSTEM only). A new token per run means a token leaked from an old
// session is worthless. Returns an empty string on failure; the caller
// then leaves IPC off rather than run it unauthenticated.
std::string CreateIpcToken();

// Compares in time independent of where the strings differ.
bool TokenEquals(const std::string& a, const std::string& b);
