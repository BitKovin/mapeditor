// doc/io -- loading and saving a document file.
//
// Owns: parsing a .map.json into a DocumentVersion (one parse, then entities parsed in parallel with core::parallelFor, brush
// content hashes, validation once with core::validateDocument) and writing a version back byte-identically to what the Core's
// saveDocument writes (dump(2) + "\n", keys sorted, temp file + rename). No Qt; every function may run on a worker thread.
// Thread: any (they only touch their arguments).
// Spec: 03_DOCUMENT.md §3, 08_MILESTONES.md T2.2.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "doc/document.h"

namespace native::doc {

struct LoadTimings {
    double readMs = 0;       // file -> text
    double parseMs = 0;      // text -> JSON tree
    double nodesMs = 0;      // typed parse + hashes per entity (parallel)
    double validateMs = 0;   // core::validateDocument
    double totalMs = 0;
};

struct LoadResult {
    std::shared_ptr<const DocumentVersion> version;   // null on error
    std::string error;                                // empty on success; a message fit for a dialog
    LoadTimings timings;
    uint64_t bytes = 0;
    size_t brushes = 0;
};

// `assetsRelativePath` becomes DocumentVersion::path ("" = not from the assets folder, e.g. a test string).
LoadResult loadDocumentText(const std::string& text, const std::string& assetsRelativePath = {});
LoadResult loadDocumentFile(const std::filesystem::path& file, const std::string& assetsRelativePath = {});

// dump(2) + "\n" of toJson(version): the bytes of the saved file.
std::string serializeDocument(const DocumentVersion& version);
// Writes serializeDocument() to `file` (parent folders created) via file + ".tmp" and a rename, so a crash never leaves a half
// written map. Returns "" on success, else the problem.
std::string saveDocumentFile(const DocumentVersion& version, const std::filesystem::path& file);

} // namespace native::doc
