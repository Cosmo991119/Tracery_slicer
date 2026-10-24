#ifndef slic3r_TraceryDocumentMetadata_hpp_
#define slic3r_TraceryDocumentMetadata_hpp_

#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {

// Export only needs the top-level wire version. Stream the JSON without building
// a second DOM, mesh, and polygon collection. Geometry is validated when imported
// or edited; this reader validates JSON syntax and the version, not the geometry.
inline int tracery_document_version(const std::string& data)
{
    struct VersionReader : nlohmann::json_sax<nlohmann::json>
    {
        int depth = 0;
        int version = 0;
        bool version_key = false;
        bool seen_version = false;
        bool null() override { return scalar(); }
        bool boolean(bool) override { return scalar(); }
        bool string(string_t&) override { return scalar(); }
        bool binary(binary_t&) override { return false; }
        bool number_float(number_float_t, const string_t&) override { return scalar(); }
        bool number_integer(number_integer_t value) override { return integer(value); }
        bool number_unsigned(number_unsigned_t value) override { return integer(value); }
        bool start_object(std::size_t) override
        {
            if (version_key) return false;
            ++depth;
            return true;
        }
        bool end_object() override { --depth; return true; }
        bool start_array(std::size_t) override
        {
            if (depth == 0 || version_key) return false;
            ++depth;
            return true;
        }
        bool end_array() override { --depth; return true; }
        bool key(string_t& key) override
        {
            version_key = depth == 1 && key == "version";
            if (version_key) {
                if (seen_version) return false;
                seen_version = true;
            }
            return true;
        }
        bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
        bool scalar() const { return depth != 0 && !version_key; }
        bool integer(number_integer_t value)
        {
            if (depth == 0) return false;
            if (version_key) {
                if (value != 1 && value != 2 && value != 3) return false;
                version = int(value);
                version_key = false;
            }
            return true;
        }
    } reader;
    if (!nlohmann::json::sax_parse(data, &reader) || reader.version == 0)
        throw std::runtime_error("Invalid or unsupported stained-glass editing data version");
    return reader.version;
}

} // namespace Slic3r
#endif
