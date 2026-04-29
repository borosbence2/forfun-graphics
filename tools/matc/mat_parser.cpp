#include "mat_parser.h"

#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace matc {

using json = nlohmann::json;

// ---- shared helpers ----

static bool parseParamType(const std::string& s, ParamType& out, std::string* err) {
    if (s == "float")  { out = ParamType::Float;  return true; }
    if (s == "float2") { out = ParamType::Float2; return true; }
    if (s == "float3") { out = ParamType::Float3; return true; }
    if (s == "float4") { out = ParamType::Float4; return true; }
    if (s == "int")    { out = ParamType::Int;    return true; }
    if (s == "bool")   { out = ParamType::Bool;   return true; }
    if (err) *err = "unknown parameter type '" + s + "'";
    return false;
}

static bool parseSamplerType(const std::string& s, SamplerType& out, std::string* err) {
    if (s == "sampler2d" || s == "sampler2d_unorm") { out = SamplerType::Sampler2D;     return true; }
    if (s == "sampler2d_srgb")                      { out = SamplerType::Sampler2DSrgb; return true; }
    if (s == "samplercube")                         { out = SamplerType::SamplerCube;   return true; }
    if (err) *err = "unknown sampler type '" + s + "'";
    return false;
}

static bool parseVertexAttr(const std::string& s, VertexAttr& out, std::string* err) {
    if (s == "POSITION") { out = VertexAttr::Position; return true; }
    if (s == "NORMAL")   { out = VertexAttr::Normal;   return true; }
    if (s == "TANGENT")  { out = VertexAttr::Tangent;  return true; }
    if (s == "UV0")      { out = VertexAttr::UV0;      return true; }
    if (s == "UV1")      { out = VertexAttr::UV1;      return true; }
    if (s == "COLOR")    { out = VertexAttr::Color;    return true; }
    if (err) *err = "unknown vertex attribute '" + s + "'";
    return false;
}

// ============================================================
// JSON parser (legacy — used when file starts with '{')
// ============================================================

static bool parseMatJson(const std::string& src, MaterialDescription& out,
                         std::string* outError) {
    auto fail = [&](std::string msg) -> bool {
        if (outError) *outError = std::move(msg);
        return false;
    };

    json j;
    try { j = json::parse(src); }
    catch (const json::parse_error& e) { return fail(std::string("JSON parse error: ") + e.what()); }

    if (!j.is_object()) return fail("top-level JSON value must be an object");

    if (!j.contains("material") || !j["material"].is_object())
        return fail("missing or invalid 'material' object");
    const auto& mat = j["material"];

    if (!mat.contains("name") || !mat["name"].is_string())
        return fail("material.name is required");
    out.name = mat["name"].get<std::string>();

    const std::string sm = mat.value("shadingModel", std::string("lit"));
    if      (sm == "lit")                out.shadingModel = ShadingModel::Lit;
    else if (sm == "unlit")              out.shadingModel = ShadingModel::Unlit;
    else if (sm == "cloth")              out.shadingModel = ShadingModel::Cloth;
    else if (sm == "subsurface")         out.shadingModel = ShadingModel::Subsurface;
    else if (sm == "specularGlossiness") out.shadingModel = ShadingModel::SpecularGlossiness;
    else return fail("unknown shadingModel '" + sm + "'");

    const std::string bm = mat.value("blendMode", std::string("opaque"));
    if      (bm == "opaque")      out.blendMode = BlendMode::Opaque;
    else if (bm == "transparent") out.blendMode = BlendMode::Transparent;
    else if (bm == "add")         out.blendMode = BlendMode::Add;
    else if (bm == "masked")      out.blendMode = BlendMode::Masked;
    else return fail("unknown blendMode '" + bm + "'");

    if (j.contains("parameters")) {
        if (!j["parameters"].is_array()) return fail("'parameters' must be an array");
        for (const auto& pe : j["parameters"]) {
            if (!pe.is_object())                return fail("each parameter entry must be an object");
            if (!pe.contains("name") || !pe["name"].is_string()) return fail("parameter missing string 'name'");
            if (!pe.contains("type") || !pe["type"].is_string()) return fail("parameter missing string 'type'");
            ParameterDef p;
            p.name = pe["name"].get<std::string>();
            if (!parseParamType(pe["type"].get<std::string>(), p.type, outError)) return false;
            if (pe.contains("default")) {
                const auto& d = pe["default"];
                if (d.is_number()) {
                    p.defaults[0] = d.get<float>();
                } else if (d.is_array()) {
                    for (size_t i = 0; i < d.size() && i < 4; ++i)
                        p.defaults[i] = d[i].get<float>();
                }
            }
            out.parameters.push_back(std::move(p));
        }
    }

    if (j.contains("samplers")) {
        if (!j["samplers"].is_array()) return fail("'samplers' must be an array");
        for (const auto& se : j["samplers"]) {
            if (!se.is_object())                return fail("each sampler entry must be an object");
            if (!se.contains("name") || !se["name"].is_string()) return fail("sampler missing string 'name'");
            if (!se.contains("type") || !se["type"].is_string()) return fail("sampler missing string 'type'");
            SamplerDef s;
            s.name = se["name"].get<std::string>();
            if (!parseSamplerType(se["type"].get<std::string>(), s.type, outError)) return false;
            out.samplers.push_back(std::move(s));
        }
    }

    if (j.contains("requires")) {
        if (!j["requires"].is_array()) return fail("'requires' must be an array");
        for (const auto& re : j["requires"]) {
            if (!re.is_string()) return fail("'requires' elements must be strings");
            VertexAttr va;
            if (!parseVertexAttr(re.get<std::string>(), va, outError)) return false;
            out.requires_.push_back(va);
        }
    }

    if (j.contains("vertex")) {
        if (!j["vertex"].is_string()) return fail("'vertex' must be a string");
        out.vertexBlock = j["vertex"].get<std::string>();
    }

    if (!j.contains("fragment") || !j["fragment"].is_string())
        return fail("'fragment' is required and must be a string");
    out.fragmentBlock = j["fragment"].get<std::string>();

    return true;
}

// ============================================================
// DSL lexer
// ============================================================

struct DslLexer {
    const char* p;
    int         line   = 1;
    std::string errMsg;

    void skipWs() {
        while (*p) {
            if (*p == '\n')                            { ++line; ++p; }
            else if (std::isspace((unsigned char)*p))  { ++p; }
            else if (p[0] == '/' && p[1] == '/')       { while (*p && *p != '\n') ++p; }
            else break;
        }
    }

    bool fail(const std::string& msg) {
        errMsg = "line " + std::to_string(line) + ": " + msg;
        return false;
    }

    // Read [a-zA-Z_][a-zA-Z0-9_]* identifier.
    bool readIdent(std::string& out) {
        skipWs();
        if (!*p || (!std::isalpha((unsigned char)*p) && *p != '_')) return false;
        const char* s = p;
        while (*p && (std::isalnum((unsigned char)*p) || *p == '_')) ++p;
        out.assign(s, p);
        return true;
    }

    bool expect(char c) {
        skipWs();
        if (*p != c) return fail(std::string("expected '") + c + "', got '" + (*p ? *p : '?') + "'");
        ++p;
        return true;
    }

    // Consume optional trailing comma.
    void optComma() { skipWs(); if (*p == ',') ++p; }

    bool readNumber(float& out) {
        skipWs();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end == p) return fail("expected number");
        out = v; p = end;
        return true;
    }

    // Extract raw content of a brace-delimited block (called after '{' consumed).
    std::string readRawBlock() {
        std::string result;
        int depth = 1;
        while (*p && depth > 0) {
            if      (*p == '{') ++depth;
            else if (*p == '}') { --depth; if (depth == 0) { ++p; break; } }
            if (*p == '\n') ++line;
            result += *p++;
        }
        return result;
    }

    // Skip an arbitrary value (ident/number/vector literal/list/struct).
    void skipValue() {
        skipWs();
        if (*p == '[' || *p == '{') {
            const char open = *p, close = (*p == '[') ? ']' : '}';
            ++p; int d = 1;
            while (*p && d > 0) {
                if (*p == open) ++d; else if (*p == close) --d;
                if (*p == '\n') ++line;
                ++p;
            }
        } else if (std::isalpha((unsigned char)*p) || *p == '_') {
            std::string tmp; readIdent(tmp);
            skipWs();
            if (*p == '(') { // vector literal — skip parens
                ++p; int d = 1;
                while (*p && d > 0) {
                    if (*p == '(') ++d; else if (*p == ')') --d;
                    ++p;
                }
            }
        } else {
            float tmp; readNumber(tmp);
        }
    }
};

// ============================================================
// DSL material-block parser
// ============================================================

static bool parseMaterialBlock(DslLexer& lex, MaterialDescription& out,
                                std::string* outError) {
    while (true) {
        lex.skipWs();
        if (!*lex.p || *lex.p == '}') { if (*lex.p == '}') ++lex.p; break; }

        std::string key;
        if (!lex.readIdent(key)) {
            if (outError) *outError = lex.errMsg.empty() ? "expected key in material block" : lex.errMsg;
            return false;
        }
        if (!lex.expect(':')) { if (outError) *outError = lex.errMsg; return false; }

        if (key == "name") {
            if (!lex.readIdent(out.name)) {
                if (outError) *outError = lex.errMsg.empty() ? "expected identifier for name" : lex.errMsg;
                return false;
            }
        } else if (key == "shadingModel") {
            std::string v;
            if (!lex.readIdent(v)) { if (outError) *outError = "expected identifier for shadingModel"; return false; }
            if      (v == "lit")                out.shadingModel = ShadingModel::Lit;
            else if (v == "unlit")              out.shadingModel = ShadingModel::Unlit;
            else if (v == "cloth")              out.shadingModel = ShadingModel::Cloth;
            else if (v == "subsurface")         out.shadingModel = ShadingModel::Subsurface;
            else if (v == "specularGlossiness") out.shadingModel = ShadingModel::SpecularGlossiness;
            else { if (outError) *outError = "unknown shadingModel '" + v + "'"; return false; }
        } else if (key == "blendMode") {
            std::string v;
            if (!lex.readIdent(v)) { if (outError) *outError = "expected identifier for blendMode"; return false; }
            if      (v == "opaque")      out.blendMode = BlendMode::Opaque;
            else if (v == "transparent") out.blendMode = BlendMode::Transparent;
            else if (v == "add")         out.blendMode = BlendMode::Add;
            else if (v == "masked")      out.blendMode = BlendMode::Masked;
            else { if (outError) *outError = "unknown blendMode '" + v + "'"; return false; }
        } else if (key == "requires") {
            if (!lex.expect('[')) { if (outError) *outError = lex.errMsg; return false; }
            while (true) {
                lex.skipWs();
                if (!*lex.p || *lex.p == ']') { if (*lex.p == ']') ++lex.p; break; }
                std::string attr;
                if (!lex.readIdent(attr)) { if (outError) *outError = "expected vertex attribute"; return false; }
                VertexAttr va;
                if (!parseVertexAttr(attr, va, outError)) return false;
                out.requires_.push_back(va);
                lex.optComma();
            }
        } else if (key == "parameters") {
            if (!lex.expect('[')) { if (outError) *outError = lex.errMsg; return false; }
            while (true) {
                lex.skipWs();
                if (!*lex.p || *lex.p == ']') { if (*lex.p == ']') ++lex.p; break; }
                if (!lex.expect('{')) { if (outError) *outError = lex.errMsg; return false; }

                ParameterDef param{};
                bool gotType = false, gotName = false;
                while (true) {
                    lex.skipWs();
                    if (!*lex.p || *lex.p == '}') { if (*lex.p == '}') ++lex.p; break; }
                    std::string fk;
                    if (!lex.readIdent(fk)) { if (outError) *outError = "expected field name"; return false; }
                    if (!lex.expect(':')) { if (outError) *outError = lex.errMsg; return false; }

                    if (fk == "type") {
                        std::string ts;
                        if (!lex.readIdent(ts)) { if (outError) *outError = "expected type"; return false; }
                        if (!parseParamType(ts, param.type, outError)) return false;
                        gotType = true;
                    } else if (fk == "name") {
                        if (!lex.readIdent(param.name)) { if (outError) *outError = "expected name"; return false; }
                        gotName = true;
                    } else if (fk == "default") {
                        lex.skipWs();
                        if (std::isalpha((unsigned char)*lex.p) || *lex.p == '_') {
                            // floatN(...) vector literal or bare boolean ident
                            std::string hint; lex.readIdent(hint);
                            lex.skipWs();
                            if (*lex.p == '(') {
                                ++lex.p; // consume '('
                                size_t cnt = 0;
                                while (true) {
                                    lex.skipWs();
                                    if (*lex.p == ')') { ++lex.p; break; }
                                    float v;
                                    if (!lex.readNumber(v)) { if (outError) *outError = lex.errMsg; return false; }
                                    if (cnt < 4) param.defaults[cnt++] = v;
                                    lex.optComma();
                                }
                            } else {
                                param.defaults[0] = (hint == "true") ? 1.0f : 0.0f;
                            }
                        } else {
                            if (!lex.readNumber(param.defaults[0])) { if (outError) *outError = lex.errMsg; return false; }
                        }
                    } else {
                        lex.skipValue();
                    }
                    lex.optComma();
                }
                if (!gotType || !gotName) {
                    if (outError) *outError = "parameter entry missing 'type' or 'name'";
                    return false;
                }
                out.parameters.push_back(std::move(param));
                lex.optComma();
            }
        } else if (key == "samplers") {
            if (!lex.expect('[')) { if (outError) *outError = lex.errMsg; return false; }
            while (true) {
                lex.skipWs();
                if (!*lex.p || *lex.p == ']') { if (*lex.p == ']') ++lex.p; break; }
                if (!lex.expect('{')) { if (outError) *outError = lex.errMsg; return false; }

                SamplerDef samp{};
                bool gotType = false, gotName = false;
                while (true) {
                    lex.skipWs();
                    if (!*lex.p || *lex.p == '}') { if (*lex.p == '}') ++lex.p; break; }
                    std::string fk;
                    if (!lex.readIdent(fk)) { if (outError) *outError = "expected field name"; return false; }
                    if (!lex.expect(':')) { if (outError) *outError = lex.errMsg; return false; }

                    if (fk == "type") {
                        std::string ts;
                        if (!lex.readIdent(ts)) { if (outError) *outError = "expected sampler type"; return false; }
                        if (!parseSamplerType(ts, samp.type, outError)) return false;
                        gotType = true;
                    } else if (fk == "name") {
                        if (!lex.readIdent(samp.name)) { if (outError) *outError = "expected name"; return false; }
                        gotName = true;
                    } else {
                        lex.skipValue();
                    }
                    lex.optComma();
                }
                if (!gotType || !gotName) {
                    if (outError) *outError = "sampler entry missing 'type' or 'name'";
                    return false;
                }
                out.samplers.push_back(std::move(samp));
                lex.optComma();
            }
        } else {
            lex.skipValue();
        }
        lex.optComma();
    }
    return true;
}

// ============================================================
// DSL top-level parser
// ============================================================

static bool parseMatDsl(const std::string& src, MaterialDescription& out,
                        std::string* outError) {
    out = {};
    DslLexer lex{ src.c_str() };
    bool hasMaterial = false, hasFragment = false;

    while (true) {
        lex.skipWs();
        if (!*lex.p) break;

        std::string blockName;
        if (!lex.readIdent(blockName)) {
            if (outError) *outError = lex.errMsg.empty() ? "expected block name" : lex.errMsg;
            return false;
        }
        if (!lex.expect('{')) { if (outError) *outError = lex.errMsg; return false; }

        if (blockName == "material") {
            hasMaterial = true;
            if (!parseMaterialBlock(lex, out, outError)) return false;
        } else if (blockName == "fragment") {
            hasFragment = true;
            out.fragmentBlock = lex.readRawBlock();
        } else if (blockName == "vertex") {
            out.vertexBlock = lex.readRawBlock();
        } else {
            lex.readRawBlock(); // skip unknown block
        }
    }

    if (!hasMaterial) { if (outError) *outError = "missing 'material { }' block"; return false; }
    if (!hasFragment)  { if (outError) *outError = "missing 'fragment { }' block"; return false; }
    return true;
}

// ============================================================
// Top-level dispatcher — auto-detects JSON vs DSL
// ============================================================

bool parseMat(const char* path, MaterialDescription& out, std::string* outError) {
    out = {};
    std::ifstream f(path);
    if (!f) {
        if (outError) *outError = std::string("cannot open '") + path + "'";
        return false;
    }
    std::string src((std::istreambuf_iterator<char>(f)), {});

    // First non-whitespace '{' means JSON (legacy).
    const char* p = src.c_str();
    while (*p && std::isspace((unsigned char)*p)) ++p;
    if (*p == '{')
        return parseMatJson(src, out, outError);
    return parseMatDsl(src, out, outError);
}

} // namespace matc
