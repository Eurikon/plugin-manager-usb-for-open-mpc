#pragma once
#include <string>
#include <string_view>

namespace pmu {
std::string sha256Text(std::string_view text);
bool verifyEd25519(const std::string& publicKey,std::string_view message,const std::string& signature);
void verifyAuthorization(const std::string& publicKey,const std::string& kind,const std::string& generation,const std::string& proof);
}
