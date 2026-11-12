// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string>

namespace gcc_host {
enum class TokenFileState { Missing, Valid, Unsafe };
struct TokenFileRead { TokenFileState state; std::string value; };
inline bool ValidPrivateToken(const std::string& token) {
    return token.size()==32 && std::all_of(token.begin(),token.end(),[](unsigned char c) {
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}
// Shared policy; the native adapter enforces canonical private path, nofollow,
// owner/mode checks and O_EXCL0600. Always re-read even when a token is cached:
// Winlator's normal launch clears tmp after the native XR session initializes.
template<class FileAccess>
bool EnsurePrivateToken(std::string& cached,FileAccess& file) {
    const auto existing=file.Read();
    if(existing.state==TokenFileState::Valid && ValidPrivateToken(existing.value)) {
        cached=existing.value;return true;
    }
    if(existing.state!=TokenFileState::Missing) {
        cached.clear();return false; // Never replace an unexpected existing file.
    }
    if(!ValidPrivateToken(cached)) cached=file.Generate();
    if(!ValidPrivateToken(cached)) {cached.clear();return false;}
    // Retain this secret across a transient missing-directory/create failure,
    // but report not-ready until the private file is actually written safely.
    return file.CreateExclusive(cached);
}
}
