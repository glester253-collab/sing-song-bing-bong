// SessionDocument.cpp
// MESSAGE THREAD ONLY.
//
// JSON construction: manual string building — no external library.
// JSON parsing: simple key-value string search — relies on our own output
// format, so the parser can be kept minimal while remaining exact for the
// round-trip contract.
//
// All int64 values are written with std::to_string (exact), read with
// std::stoll.  Double values are written with fixed-precision (6 dp), read
// with std::stod.  These conversions preserve the values that arise in
// practice (44100.0 Hz, sample offsets within a session).

#include "SessionDocument.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace ssbb {

namespace {

// ---- JSON string helpers -------------------------------------------------

/// Escape a raw string for JSON: backslash and double-quote only.
std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s)
    {
        if      (c == '\\') out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else                out += c;
    }
    return out;
}

/// Reverse of jsonEscape.
std::string jsonUnescape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            ++i;
            if      (s[i] == '\\') out += '\\';
            else if (s[i] == '"')  out += '"';
            else { out += '\\'; out += s[i]; }
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

// ---- JSON value extractors -----------------------------------------------
//
// Each function searches `block` for the pattern "key": <value>.
// They return true on success and write the result to `out`.

bool extractString(const std::string& block,
                   const std::string& key,
                   std::string&       out)
{
    const std::string needle = "\"" + key + "\": \"";
    const auto pos0 = block.find(needle);
    if (pos0 == std::string::npos) return false;

    const auto start = pos0 + needle.size();
    // Find the closing quote, skipping escaped quotes.
    std::size_t i = start;
    while (i < block.size())
    {
        if (block[i] == '\\') { i += 2; continue; }
        if (block[i] == '"')  break;
        ++i;
    }
    if (i >= block.size()) return false;
    out = jsonUnescape(block.substr(start, i - start));
    return true;
}

bool extractInt64(const std::string& block,
                  const std::string& key,
                  int64_t&           out)
{
    const std::string needle = "\"" + key + "\": ";
    const auto pos0 = block.find(needle);
    if (pos0 == std::string::npos) return false;

    const auto start = pos0 + needle.size();
    try
    {
        std::size_t len = 0;
        out = std::stoll(block.substr(start), &len);
        return len > 0;
    }
    catch (...) { return false; }
}

bool extractInt32(const std::string& block,
                  const std::string& key,
                  int&               out)
{
    int64_t v = 0;
    if (!extractInt64(block, key, v)) return false;
    out = static_cast<int>(v);
    return true;
}

bool extractDouble(const std::string& block,
                   const std::string& key,
                   double&            out)
{
    const std::string needle = "\"" + key + "\": ";
    const auto pos0 = block.find(needle);
    if (pos0 == std::string::npos) return false;

    const auto start = pos0 + needle.size();
    try
    {
        std::size_t len = 0;
        out = std::stod(block.substr(start), &len);
        return len > 0;
    }
    catch (...) { return false; }
}

// ---- Array object extractor ---------------------------------------------
//
// Returns the JSON object strings (including braces) found inside the named
// top-level array.  Handles nested braces correctly.

std::vector<std::string> extractArrayObjects(const std::string& json,
                                             const std::string& arrayKey)
{
    std::vector<std::string> result;

    const std::string needle = "\"" + arrayKey + "\": [";
    const auto arrStart = json.find(needle);
    if (arrStart == std::string::npos) return result;

    std::size_t pos = arrStart + needle.size();

    while (pos < json.size())
    {
        // Skip whitespace / newlines / commas between objects.
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\n' ||
                json[pos] == '\r' || json[pos] == '\t' ||
                json[pos] == ','))
            ++pos;

        if (pos >= json.size() || json[pos] == ']')
            break;  // end of array

        if (json[pos] != '{')
            break;  // unexpected character — bail out

        // Collect the object by matching braces.
        const auto objStart = pos;
        int depth = 1;
        ++pos;
        while (pos < json.size() && depth > 0)
        {
            if      (json[pos] == '{') ++depth;
            else if (json[pos] == '}') --depth;
            ++pos;
        }
        if (depth == 0)
            result.push_back(json.substr(objStart, pos - objStart));
    }
    return result;
}

// ---- JSON serialisers ---------------------------------------------------

std::string serialiseTake(const TakeEntry& t)
{
    std::ostringstream o;
    o << "    {\n"
      << "      \"path\": \""          << jsonEscape(t.path)         << "\",\n"
      << "      \"sampleRate\": "      << std::fixed << std::setprecision(6)
                                       << t.sampleRate               << ",\n"
      << "      \"numChannels\": "     << t.numChannels              << ",\n"
      << "      \"isoTimestamp\": \""  << jsonEscape(t.isoTimestamp) << "\"\n"
      << "    }";
    return o.str();
}

std::string serialiseClip(const ClipEntry& c)
{
    std::ostringstream o;
    o << "    {\n"
      << "      \"takePath\": \""          << jsonEscape(c.takePath)           << "\",\n"
      << "      \"offsetSamples\": "       << c.offsetSamples                  << ",\n"
      << "      \"trimStartSamples\": "    << c.trimStartSamples               << ",\n"
      << "      \"trimEndSamples\": "      << c.trimEndSamples                 << ",\n"
      << "      \"sourceLengthSamples\": " << c.sourceLengthSamples            << "\n"
      << "    }";
    return o.str();
}

// ---- JSON deserialiser --------------------------------------------------

bool parseSession(const std::string& json, SessionData& data)
{
    data = SessionData{};  // reset

    if (!extractInt32(json, "version", data.version)) return false;

    // Takes
    for (const auto& block : extractArrayObjects(json, "takes"))
    {
        TakeEntry t;
        if (!extractString(block, "path",         t.path))         return false;
        if (!extractDouble(block, "sampleRate",   t.sampleRate))   return false;
        if (!extractInt32 (block, "numChannels",  t.numChannels))  return false;
        if (!extractString(block, "isoTimestamp", t.isoTimestamp)) return false;
        data.takes.push_back(std::move(t));
    }

    // Clips
    for (const auto& block : extractArrayObjects(json, "clips"))
    {
        ClipEntry c;
        if (!extractString(block, "takePath",            c.takePath))            return false;
        if (!extractInt64 (block, "offsetSamples",       c.offsetSamples))       return false;
        if (!extractInt64 (block, "trimStartSamples",    c.trimStartSamples))    return false;
        if (!extractInt64 (block, "trimEndSamples",      c.trimEndSamples))      return false;
        if (!extractInt64 (block, "sourceLengthSamples", c.sourceLengthSamples)) return false;
        data.clips.push_back(std::move(c));
    }

    return true;
}

// ---- Shared writer -------------------------------------------------------

bool writeJsonToFile(const std::filesystem::path& path, const SessionData& data)
{
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;

    f << "{\n"
      << "  \"version\": " << data.version << ",\n"
      << "  \"takes\": [\n";

    for (std::size_t i = 0; i < data.takes.size(); ++i)
    {
        f << serialiseTake(data.takes[i]);
        if (i + 1 < data.takes.size()) f << ',';
        f << '\n';
    }

    f << "  ],\n"
      << "  \"clips\": [\n";

    for (std::size_t i = 0; i < data.clips.size(); ++i)
    {
        f << serialiseClip(data.clips[i]);
        if (i + 1 < data.clips.size()) f << ',';
        f << '\n';
    }

    f << "  ]\n"
      << "}\n";

    return f.good();
}

bool readJsonFromFile(const std::filesystem::path& path, SessionData& data)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string json;
    json.assign(std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>());

    return parseSession(json, data);
}

// ---- Recovery path helper -----------------------------------------------

std::filesystem::path recoveryPath(const std::filesystem::path& sessionPath)
{
    // E.g. "session.json" → "session.recovery.json"
    auto stem   = sessionPath.stem().string();     // "session"
    auto parent = sessionPath.parent_path();
    return parent / (stem + ".recovery.json");
}

} // namespace

// ---- Public API ----------------------------------------------------------

bool SessionDocument::save(const std::filesystem::path& path,
                           const SessionData&           data)
{
    return writeJsonToFile(path, data);
}

bool SessionDocument::load(const std::filesystem::path& path,
                           SessionData&                 data)
{
    return readJsonFromFile(path, data);
}

bool SessionDocument::saveRecovery(const std::filesystem::path& sessionPath,
                                   const SessionData&           data)
{
    return writeJsonToFile(recoveryPath(sessionPath), data);
}

bool SessionDocument::loadRecovery(const std::filesystem::path& sessionPath,
                                   SessionData&                 data)
{
    return readJsonFromFile(recoveryPath(sessionPath), data);
}

bool SessionDocument::migrate(SessionData& data) noexcept
{
    // Version 1 is the only known schema; nothing to migrate.
    // If a future version is introduced, add conversion steps here and bump
    // kSchemaVersion accordingly.
    if (data.version == kSchemaVersion)
        return true;

    // Unknown version — cannot safely migrate.
    return false;
}

} // namespace ssbb
