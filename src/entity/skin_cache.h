#pragma once
// Profile textures (SkinManager / SkinTextureDownloader): the skin and cape a profile's "textures" property names on
// the game's texture server, downloaded in the background to %LOCALAPPDATA%\minecraftCplusplus\skins and loaded as
// RGBA (skins through processLegacySkin). Main thread.
#include "core/common.h"

#include <string>
#include <vector>

namespace ent {

struct ProfileTextures {
    std::string skinUrl, capeUrl;  // only textures.minecraft.net URLs (the property may be unsigned data)
    bool slim = false;             // SKIN metadata model "slim"
};
// The base64 "textures" property value -> its URLs; false when it names none.
bool parseProfileTextures(const std::string& property, ProfileTextures& out);

// A texture by URL: 0 not asked, 1 downloading, 2 ready (rgba filled), 3 failed. Starts the download on first ask.
// skin: processLegacySkin (64 x 32 skins grow to 64 x 64, opaque base layers); capes as they are.
int fetchProfileTexture(const std::string& url, bool skin, std::vector<u8>* rgba = nullptr, int* w = nullptr, int* h = nullptr);
// The URL's file name: the texture's hash (a stable key for caches).
std::string profileTextureHash(const std::string& url);

// ProfileResolver for profiles given only by name or id (ResolvableProfile.Dynamic): their "textures" property from
// Mojang's public profile service (name -> id -> session profile), looked up in the background and kept for the
// session. state: 1 looking up, 2 found (the property returned), 3 none. uuid: 16 bytes or nullptr.
std::string resolveProfileTextures(const std::string& name, const u8* uuid, int* state = nullptr);

}  // namespace ent
