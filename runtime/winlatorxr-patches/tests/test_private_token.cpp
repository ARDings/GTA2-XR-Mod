// SPDX-License-Identifier: GPL-3.0-or-later
// CPU regression of the production token policy with an injected file adapter.
// All values are synthetic; no real private file or token is inspected/logged.
#include "../overlay/gcc_private_token.h"
#include <cassert>
#include <cstdio>

struct FakeFile {
    gcc_host::TokenFileState state=gcc_host::TokenFileState::Missing;
    std::string contents;
    unsigned reads=0,generates=0,creates=0;
    bool createSucceeds=true;
    gcc_host::TokenFileRead Read() {++reads;return {state,contents};}
    std::string Generate() {++generates;return std::string(32,'a');}
    bool CreateExclusive(const std::string& value) {
        ++creates;
        if(!createSucceeds || state!=gcc_host::TokenFileState::Missing) return false;
        contents=value;state=gcc_host::TokenFileState::Valid;return true;
    }
};
int main() {
    FakeFile file;std::string cached;
    assert(gcc_host::EnsurePrivateToken(cached,file));
    assert(file.generates==1 && file.creates==1 && file.contents==cached);
    const auto original=cached;
    assert(gcc_host::EnsurePrivateToken(cached,file));
    assert(file.reads==2 && file.generates==1 && file.creates==1);
    // Exact launch failure: native init succeeded, then XrAPI/launch clearedtmp.
    file.state=gcc_host::TokenFileState::Missing;file.contents.clear();
    assert(gcc_host::EnsurePrivateToken(cached,file));
    assert(file.contents==original && cached==original && file.generates==1 && file.creates==2);
    // Missing directory/create race: retain the secret for retry, report false.
    file.state=gcc_host::TokenFileState::Missing;file.contents.clear();file.createSucceeds=false;
    assert(!gcc_host::EnsurePrivateToken(cached,file));assert(cached==original);
    file.createSucceeds=true;assert(gcc_host::EnsurePrivateToken(cached,file));assert(file.contents==original);
    const auto creates=file.creates;
    file.state=gcc_host::TokenFileState::Unsafe;file.contents="unexpected existing file";
    assert(!gcc_host::EnsurePrivateToken(cached,file));
    assert(cached.empty() && file.creates==creates && file.contents=="unexpected existing file");
    file.state=gcc_host::TokenFileState::Valid;file.contents="bad";
    assert(!gcc_host::EnsurePrivateToken(cached,file) && file.creates==creates);
    // A valid existing private file is adopted without overwrite or rotation.
    file.contents=std::string(32,'b');assert(gcc_host::EnsurePrivateToken(cached,file));
    assert(cached==file.contents && file.creates==creates);
    std::puts("PASS: private token survives launch deletion, retries safely and rejects unexpected files. Synthetic values only.");
}
