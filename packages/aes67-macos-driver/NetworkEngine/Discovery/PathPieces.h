//
// PathPieces.h
// AES67 macOS Driver
// A URL path split into the parts a router matches on.
//
// The connection API server and the node API router both walk a path piece by
// piece, and both had written this out. aes67-ravenna has the same function
// again under another name, in a package this one links; it stays separate
// because that one belongs to a session layer this driver does not route
// through, and a header of four lines is not worth a dependency between two
// routers that happen to agree.
//
#pragma once

#include <string>
#include <vector>

namespace AES67 {

/// The path split on '/', with the empty pieces a leading or trailing slash
/// leaves behind dropped.
inline std::vector<std::string> pathPieces(const std::string& path) {
    std::vector<std::string> pieces;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > start) pieces.push_back(path.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return pieces;
}

} // namespace AES67
