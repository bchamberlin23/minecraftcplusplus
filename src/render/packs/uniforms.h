#pragma once
// Shader pack uniforms: a named value store (Iris standard uniforms, per-draw values, custom uniforms), the custom
// uniform expression language of shaders.properties (uniform.<type>.<name> / variable.<type>.<name> = expr), and
// writing a stage's default uniform block (std140 layout from SPIRV-Cross reflection) from the store.
#include "core/common.h"
#include "render/packs/shader_compiler.h"

#include <memory>
#include <unordered_map>

namespace packs {

struct UValue {
    float f[16] = {};  // floats, vectors, matrices (column-major)
    i32 i[4] = {};     // ints, ivecs, bools
    bool isInt = false;
    u8 count = 1;      // components (16 for mat4, 9 for mat3)
    bool set = false;  // written at least once (else the pack reads a uniform we do not provide: zero)
};

class UniformStore {
public:
    UValue& at(const std::string& name);  // created (zero) if missing; the reference stays valid
    const UValue* find(const std::string& name) const;
    void setFloat(const std::string& n, float v);
    void setInt(const std::string& n, i32 v);
    void setBool(const std::string& n, bool v) { setInt(n, v ? 1 : 0); }
    void setVec(const std::string& n, float x, float y, float z = 0, float w = 0, u8 count = 4);
    void setIvec(const std::string& n, i32 x, i32 y, i32 z = 0, i32 w = 0, u8 count = 2);
    void setMat4(const std::string& n, const float* colMajor);
    void setMat3(const std::string& n, const float* colMajor9);

private:
    std::unordered_map<std::string, std::unique_ptr<UValue>> values_;
};

// One block member bound to a stored value.
struct UniformSlot {
    const UValue* value;
    UniformMember member;
};
// Resolves a stage's block members against the store (unknown names are created as zeros and reported).
void resolveUniforms(const StageReflection& refl, UniformStore& store, std::vector<UniformSlot>& slots,
                     std::vector<std::string>* unknown);
void writeUniforms(const std::vector<UniformSlot>& slots, u8* dst);

// Custom uniforms: parsed once, evaluated every frame in declaration order.
class CustomUniforms {
public:
    CustomUniforms();
    ~CustomUniforms();
    // type: bool, int, float, vec2, vec3, vec4. Errors are collected (the uniform stays zero).
    void add(const std::string& type, const std::string& name, const std::string& expr, bool isVariable);
    void evaluate(UniformStore& store, float frameTime);
    const std::vector<std::string>& errors() const { return errors_; }
    struct Node;
    struct Entry;

private:
    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<std::string> errors_;
};

// Iris biome constants (BIOME_*, CAT_*, PPT_*) for custom uniform expressions; registry ids follow the vanilla order.
int biomeConstant(const std::string& name, bool& found);
int biomeIndex(std::string_view name);  // the `biome` uniform for a biome id ("minecraft:plains"), -1 unknown

}  // namespace packs
