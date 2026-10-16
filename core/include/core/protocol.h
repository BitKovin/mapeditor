#pragma once

// Message "type" strings shared by Core and Shell (the Shell mirrors them in
// its own protocol.ts; there is no codegen yet).
//
// Deliberately tiny (DESIGN.md §10). There is exactly ONE message for every
// kind of edit -- `document_update` carrying the whole document -- so adding
// an editing tool never adds a wire message. The rest is filesystem work,
// which is what the browser can't do.
namespace core::proto {

// Shell -> Core
inline constexpr const char* kDocumentUpdate = "document_update";  // { doc }
inline constexpr const char* kOpenDocument = "open_document";      // { requestId, path }
inline constexpr const char* kSaveDocument = "save_document";      // { requestId, path? }
inline constexpr const char* kListTextures = "list_textures";      // { requestId }            -> response { textures: [path] }
inline constexpr const char* kReadTexture = "read_texture";        // { requestId, path }      -> response { ext, data: base64, props?: <texture>.json contents }
                                                                    //   a skybox (name ends in "cube") -> response { sky: { panorama, images: [{ ext, data }] } } (faces lf rt up dn ft bk)
inline constexpr const char* kWriteTextureProps = "write_texture_props";  // { requestId, path, props } -> response { props } (sanitised, merged into <texture file>.json)
inline constexpr const char* kListTexGroups = "list_texgroups";    // { requestId }            -> response { texgroups: [name] }
inline constexpr const char* kReadTexGroup = "read_texgroup";      // { requestId, path }      -> response { path, group: <the file's JSON> }
inline constexpr const char* kWriteTexGroup = "write_texgroup";    // { requestId, path, group } -> response { path }   (creates or replaces <texGroupRoot>/<path>.texgroup.json)
inline constexpr const char* kDeleteTexGroup = "delete_texgroup";  // { requestId, path }      -> response { path }
inline constexpr const char* kReadPrefab = "read_prefab";          // { requestId, path }      -> response { path, doc }  (one prefab file, for previews; inside the assets root)
inline constexpr const char* kListDirectory = "list_directory";    // { requestId, path? }     -> response { path, entries: [{name, dir}] }  (inside the assets root)
inline constexpr const char* kBrowseFilesystem = "browse_filesystem";  // { requestId, path? } -> response { path, parent, dirs: [name], drives? }  (folders only; to pick the assets root)
inline constexpr const char* kSetAssetsRoot = "set_assets_root";  // { requestId, path }      -> response, then profile + entity_defs + document_sync + prefab_library + settings to everyone
inline constexpr const char* kNativeDialog = "native_dialog";  // { requestId, kind: folder|open|save, title?, startDir?, fileName?, filters?: [{name, pattern}], defaultExt?, relative?: bool }
                                                              //   -> response { cancelled?, path, absolutePath } (relative: start dir and result relative to the assets root, which the result must be inside),
                                                              //      or { ok: false, unsupported: true } where Core has no native dialogs. Answered when the person closes the dialog.
inline constexpr const char* kSetSettings = "set_settings";    // { requestId, game?: { exe, args, workingDir } (for the current assets root), editor?: {...} } -> response, then settings to everyone
inline constexpr const char* kReadGameProfile = "read_game_profile";    // { requestId } -> response { file: game_profile.json as written ({} if none), profile: what is in effect }
inline constexpr const char* kWriteGameProfile = "write_game_profile";  // { requestId, profile: changes } -> response { profile }; then profile + entity_defs + settings to everyone
inline constexpr const char* kReadLightingConfig = "read_lighting_config";    // { requestId } -> response { config (every key, in effect), file }
inline constexpr const char* kWriteLightingConfig = "write_lighting_config";  // { requestId, config: changes } -> response { config, warning? }  (live view: from the next Core start; export: now)
inline constexpr const char* kLaunchGame = "launch_game";      // { requestId, map: .bsp relative to the assets root } -> response  (starts the game set in settings)

// Core -> Shell
inline constexpr const char* kDocumentSync = "document_sync";  // { rev, path, doc }  (to everyone EXCEPT an updating sender)
inline constexpr const char* kUpdateAck = "update_ack";        // { rev }             (to the updating sender only)
inline constexpr const char* kProfile = "profile";                // { profile }  (on connect, and when the assets root or game_profile.json changes)
inline constexpr const char* kEntityDefs = "entity_defs";        // { classes: [...], warnings: [...] }  (from the profile's .ent file)
// Live lighting (DESIGN §6.6). The Shell asks with `lighting` { enabled?, exposure?, camera?: [x,y,z], requestId? }; the Core answers with
// lit_mesh (geometry with lightmap coordinates, sent when the layout changes), lightmap_tiles (changed pixels, RGBA16F linear HDR) and lighting_status.
inline constexpr const char* kLighting = "lighting";
// The relief of textures as displaced geometry per authored face (relief_geometry.h): the bake's occluders and texel positions. Replaces what was sent before.
inline constexpr const char* kReliefGeometry = "relief_geometry";
inline constexpr const char* kLitMesh = "lit_mesh";                // { rev, atlas, pageCount, triangleCount, vertices: base64 float32 x 12 per vertex (pos, normal, u, v, page, style mask x3: styles 1..63 = bit style-1, 21 bits per float),
                                                                    //   faces: [{ i: instance, e: entityId, bi: brushInstance, b: brushId, f: faceId, t: texture, n: triangles, m?: { u: [x,y,z,w], v: [x,y,z,w], pg: page, k: [mask x3] } }] in vertex order;
                                                                    //   m = the face's lightmap mapping (first piece of each face only): uv = (dot(p, u.xyz) + u.w, dot(p, v.xyz) + v.w), for decals }
inline constexpr const char* kLightStyles = "light_styles";          // { patterns: [64 strings, "" = constant 1], hz }  light style patterns (light_styles.h), sent with the first lit mesh
inline constexpr const char* kLightmapTiles = "lightmap_tiles";    // { rev, atlas, style (0 = base layer), tiles: [{ page, x, y, w, h, rgba: base64 of little endian float16 x 4 per pixel (linear, as baked; alpha 0 = no value) }] }
inline constexpr const char* kLightvol = "lightvol";                // { rev, origin: [x,y,z], cell, dims: [nx,ny,nz], probes: base64 (style 0), 9 bytes per probe (ambient rgb, directed rgb, lat, lng, flags), x2 range, layers: [{ s: style, probes }] other styles }
inline constexpr const char* kEntityLight = "entity_light";          // { rev, entities: [{ i: instance, e: entityId, dir, lights: [{ s: style, ambient, directed }] }] }  linear floats (decoded bytes), one entry per style layer; ONE direction per entity (REQUIREMENTS §27 G4)
inline constexpr const char* kLightingStatus = "lighting_status";  // { enabled, rev, chunks, stale, minPasses, maxPasses, converged, texels, pages, exposure, styles: [ids], styleWarnings: [text] }
inline constexpr const char* kDerivedGeometry = "derived_geometry";  // { rev, carved: [...], skipped: [...] }  boolean results (DESIGN §5.6)
inline constexpr const char* kSettings = "settings";            // { assetsRoot (absolute), recentMaps: [paths in it], game: { exe, args, workingDir }, editor: {...}, nativeDialogs: bool,
                                                                //   settingsFile, lightingFile }  (persisted in settings.json)
inline constexpr const char* kPrefabLibrary = "prefab_library";  // { prefabs: {path: doc|{error}} }
// BSP export (REQUIREMENTS §25): `export_bsp` { path (assets relative .bsp), bake?: bool (default true), requestId } -> response { ok, error? };
// then, to every session, export_progress { stage, fraction } about once a second and export_result { ok, cancelled, error, warnings, stats, bsp, lightmaps,
// seconds, bakeSeconds, device } at the end. `export_cancel` { } stops a running export.
inline constexpr const char* kExportBsp = "export_bsp";
inline constexpr const char* kExportCancel = "export_cancel";
inline constexpr const char* kExportProgress = "export_progress";
inline constexpr const char* kExportResult = "export_result";
inline constexpr const char* kResponse = "response";           // { requestId, ok, ... }
inline constexpr const char* kError = "error";                 // { message }         (to the sender only)

}  // namespace core::proto
