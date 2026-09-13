#include "Ravenna/NmosRoot.h"
#include "Ravenna/ApiReplies.h"

#include "Ravenna/ChannelMappingApi.h"
#include "Ravenna/NodeApi.h"

#include <array>

namespace AES67::Ravenna {
namespace {

constexpr char kNmosRoot[] = "/x-nmos";



/// The name and the version an API root is made of: "/x-nmos/node/v1.3"
/// splits into "node" and "v1.3".
struct ApiPath {
    std::string name;
    std::string version;
};

ApiPath split(const std::string& root) {
    const std::string tail = root.substr(std::string(kNmosRoot).size() + 1);
    const size_t slash = tail.find('/');
    if (slash == std::string::npos) return {tail, {}};
    return {tail.substr(0, slash), tail.substr(slash + 1)};
}

/// The path with any trailing slash taken off, so "/x-nmos/" and "/x-nmos"
/// are the one resource they are.
std::string withoutTrailingSlash(const std::string& path) {
    if (path.size() > 1 && path.back() == '/') return path.substr(0, path.size() - 1);
    return path;
}

}  // namespace

std::optional<ApiResponse> nmosRootListing(const std::string& method, const std::string& path) {
    const std::array<ApiPath, 3> apis{split(kChannelMappingApiRoot), split(kConnectionApiRoot),
                                      split(kNodeApiRoot)};
    const std::string resource = withoutTrailingSlash(path);

    if (resource == kNmosRoot) {
        if (method != "GET") return errorResponse(405, "only GET here");
        std::vector<std::string> names;
        names.reserve(apis.size());
        for (const ApiPath& api : apis) names.push_back(api.name + "/");
        return listResponse(names);
    }

    for (const ApiPath& api : apis) {
        if (resource != std::string(kNmosRoot) + "/" + api.name) continue;
        if (method != "GET") return errorResponse(405, "only GET here");
        // One version each, which is what this device speaks.
        return listResponse({api.version + "/"});
    }

    return std::nullopt;
}

}  // namespace AES67::Ravenna
