#include "Window.h"
#include "Input.h"
#include "Clock.h"
#include "Shader.h"
#include "Mesh.h"
#include "Camera.h"
#include "Player.h"
#include "World.h"
#include "gl.h"

#include "AudioEngine.h"
#include "AssetLibrary.h"
#include "EditorLayer.h"
#include "Model.h"
#include "SceneSerializer.h"
#include "Grid.h"
#include "Sky.h"

static const char* kScenePath = "scene.json";

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <filesystem>
#include <string>

static const char* kPrimitiveVertexSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vNormal;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    gl_Position = uProj * uView * world;
}
)";

static const char* kPrimitiveFragmentSrc = R"(
#version 330 core
in vec3 vNormal;
out vec4 FragColor;

uniform vec3 uColor;
uniform vec3 uLightDir;

void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, -normalize(uLightDir)), 0.0);
    vec3 ambient = 0.25 * uColor;
    vec3 diffuse = diff * uColor;
    FragColor = vec4(ambient + diffuse, 1.0);
}
)";

// PBR model shader (metallic-roughness workflow, Cook-Torrance, single directional
// light + constant ambient term — no IBL/environment reflections). Supports optional
// GPU skinning (up to 4 bone influences/vertex) for imported, animated FBX/glTF assets.
static const char* kModelVertexSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 6) in float aTangentSign;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning;
uniform mat4 uBones[100];

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out mat3 vTBN;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent;

    if (uUseSkinning == 1) {
        mat4 skinMat = mat4(0.0);
        float totalWeight = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) {
                skinMat += uBones[aBoneIDs[i]] * aWeights[i];
                totalWeight += aWeights[i];
            }
        }
        if (totalWeight <= 0.0001) {
            skinMat = mat4(1.0);
        }
        localPos = skinMat * localPos;
        localNormal = mat3(skinMat) * aNormal;
        localTangent = mat3(skinMat) * aTangent;
    }

    vec4 world = uModel * localPos;
    vWorldPos = world.xyz;

    mat3 normalMat = mat3(transpose(inverse(uModel)));
    vNormal = normalize(normalMat * localNormal);
    // Tangents transform with the model matrix's linear part directly (not the
    // inverse-transpose used for normals) — using normalMat here would skew tangents
    // under non-uniform scale.
    vec3 T = normalize(mat3(uModel) * localTangent);
    T = normalize(T - dot(T, vNormal) * vNormal);
    vec3 B = cross(vNormal, T) * aTangentSign;
    vTBN = mat3(T, B, vNormal);

    vUV = aUV;
    gl_Position = uProj * uView * world;
}
)";

static const char* kModelFragmentSrc = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in mat3 vTBN;
out vec4 FragColor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;   // direction light travels (points away from the light)
uniform vec3 uLightColor;

uniform vec3 uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uEmissiveColor;

uniform int uHasAlbedoMap;             uniform sampler2D uAlbedoMap;
uniform int uHasNormalMap;             uniform sampler2D uNormalMap;
uniform int uHasMetallicRoughnessMap;  uniform sampler2D uMetallicRoughnessMap;
uniform int uHasMetallicMap;           uniform sampler2D uMetallicMap;
uniform int uHasRoughnessMap;          uniform sampler2D uRoughnessMap;
uniform int uHasAOMap;                 uniform sampler2D uAOMap;
uniform int uHasEmissiveMap;           uniform sampler2D uEmissiveMap;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec3 albedo = (uHasAlbedoMap == 1 ? texture(uAlbedoMap, vUV).rgb : vec3(1.0)) * uBaseColor;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessMap == 1) {
        vec3 mr = texture(uMetallicRoughnessMap, vUV).rgb;
        roughness = mr.g;
        metallic = mr.b;
    } else {
        if (uHasRoughnessMap == 1) roughness = texture(uRoughnessMap, vUV).r;
        if (uHasMetallicMap == 1) metallic = texture(uMetallicMap, vUV).r;
    }
    float ao = uHasAOMap == 1 ? texture(uAOMap, vUV).r : 1.0;

    vec3 N = normalize(vNormal);
    if (uHasNormalMap == 1) {
        vec3 tangentNormal = texture(uNormalMap, vUV).rgb * 2.0 - 1.0;
        N = normalize(vTBN * tangentNormal);
    }

    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 L = normalize(-uLightDir);
    vec3 H = normalize(V + L);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denom = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4;
    vec3 specular = numerator / denom;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    vec3 Lo = (kD * albedo / PI + specular) * uLightColor * NdotL;

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 emissive = uHasEmissiveMap == 1 ? texture(uEmissiveMap, vUV).rgb : uEmissiveColor;

    vec3 color = ambient + Lo + emissive;
    color = color / (color + vec3(1.0)); // Reinhard tonemap
    color = pow(color, vec3(1.0 / 2.2));  // gamma correct

    FragColor = vec4(color, 1.0);
}
)";

// Selection outline (editor-only): the classic "inverted hull" technique — draw the object
// again, offset a little along its normal, with front-face culling so only the silhouette
// peeking out from behind the normal draw survives. One flat fragment shader shared by both
// variants below; only the vertex stage differs, matching each mesh's own attribute layout.
static const char* kOutlineFragmentSrc = R"(
#version 330 core
out vec4 FragColor;
uniform vec3 uOutlineColor;
void main() {
    FragColor = vec4(uOutlineColor, 1.0);
}
)";

static const char* kOutlineBoxVertexSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform float uThickness;

void main() {
    vec3 worldPos = vec3(uModel * vec4(aPos, 1.0));
    vec3 worldNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    worldPos += worldNormal * uThickness;
    gl_Position = uProj * uView * vec4(worldPos, 1.0);
}
)";

// Mirrors kModelVertexSrc's skinning block so an animated model's outline deforms with it,
// then offsets along the (skinned) normal instead of computing UV/TBN — outline doesn't need them.
static const char* kOutlineModelVertexSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 6) in float aTangentSign;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning;
uniform mat4 uBones[100];
uniform float uThickness;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;

    if (uUseSkinning == 1) {
        mat4 skinMat = mat4(0.0);
        float totalWeight = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) {
                skinMat += uBones[aBoneIDs[i]] * aWeights[i];
                totalWeight += aWeights[i];
            }
        }
        if (totalWeight <= 0.0001) skinMat = mat4(1.0);
        localPos = skinMat * localPos;
        localNormal = mat3(skinMat) * aNormal;
    }

    vec4 world = uModel * localPos;
    vec3 worldNormal = normalize(mat3(transpose(inverse(uModel))) * localNormal);
    world.xyz += worldNormal * uThickness;
    gl_Position = uProj * uView * world;
}
)";

// Simple fly-camera controls used only while the editor overlay is open.
static void UpdateEditorCamera(Camera& cam, float dt, bool allowLook) {
    float speed = 8.0f * dt * (Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
    glm::vec3 move{0.0f};
    if (Input::IsKeyDown(GLFW_KEY_W)) move += cam.Front();
    if (Input::IsKeyDown(GLFW_KEY_S)) move -= cam.Front();
    if (Input::IsKeyDown(GLFW_KEY_D)) move += cam.Right();
    if (Input::IsKeyDown(GLFW_KEY_A)) move -= cam.Right();
    if (Input::IsKeyDown(GLFW_KEY_E)) move += glm::vec3(0, 1, 0);
    if (Input::IsKeyDown(GLFW_KEY_Q)) move -= glm::vec3(0, 1, 0);
    if (glm::length(move) > 0.0001f) {
        cam.Position += glm::normalize(move) * speed;
    }

    if (allowLook && Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());
    }
}

int main() {
    try {
        Window window(1280, 720, "Tartarus Engine");
        Input::Init(window.Handle());
        window.SetCursorLocked(true);
        window.Maximize(); // opens maximized (not true fullscreen, no monitor video-mode switch); F11 still enters fullscreen

        AudioEngine::Init();

        Shader primitiveShader(kPrimitiveVertexSrc, kPrimitiveFragmentSrc);
        Shader modelShader(kModelVertexSrc, kModelFragmentSrc);
        Shader outlineBoxShader(kOutlineBoxVertexSrc, kOutlineFragmentSrc);
        Shader outlineModelShader(kOutlineModelVertexSrc, kOutlineFragmentSrc);
        Mesh* cube = Mesh::CreateCube(1.0f);
        Grid grid;
        Sky sky;

        World world;
        Player player;
        player.Cam.Position = glm::vec3(0, 2.0f, 0);

        AssetLibrary assets;
        if (SceneSerializer::Load(world, assets, kScenePath)) {
            std::cout << "Loaded scene from " << kScenePath << std::endl;
        }

        EditorLayer editor;
        editor.Init(window.Handle());

        Camera editorCamera;
        bool editorMode = true; // start directly on the editor screen instead of requiring F1
        editorCamera.Position = player.Cam.Position;
        editorCamera.Yaw = player.Cam.Yaw;
        editorCamera.Pitch = player.Cam.Pitch;
        window.SetCursorLocked(false);
        bool prevF1 = false;
        bool prevF11 = false;
        bool prevEscape = false;

        bool prevLeftMouse = false;
        std::string lastTitle;

        // Shared by the F1 key and the editor's floating Play/Stop button, so both paths
        // hand off the camera and cursor lock identically.
        auto toggleEditorMode = [&]() {
            editorMode = !editorMode;
            if (editorMode) {
                editorCamera.Position = player.Cam.Position;
                editorCamera.Yaw = player.Cam.Yaw;
                editorCamera.Pitch = player.Cam.Pitch;
            }
            window.SetCursorLocked(!editorMode);
        };

        while (!window.ShouldClose()) {
            Clock::Update();
            float dt = Clock::DeltaTime();

            window.PollEvents();
            Input::Update();

            std::string desiredTitle = "Tartarus Engine \xE2\x80\x94 " +
                std::filesystem::path(editor.CurrentScenePath()).filename().string() +
                (editor.IsDirty() ? "*" : "");
            if (desiredTitle != lastTitle) {
                window.SetTitle(desiredTitle);
                lastTitle = desiredTitle;
            }

            bool f1Now = Input::IsKeyDown(GLFW_KEY_F1);
            if (f1Now && !prevF1) toggleEditorMode();
            prevF1 = f1Now;

            bool f11Now = Input::IsKeyDown(GLFW_KEY_F11);
            if (f11Now && !prevF11) {
                window.ToggleFullscreen();
            }
            prevF11 = f11Now;

            if (!editorMode) {
                bool escNow = Input::IsKeyDown(GLFW_KEY_ESCAPE);
                if (escNow && !prevEscape) {
                    window.SetCursorLocked(!window.IsCursorLocked());
                }
                prevEscape = escNow;
            }

            editor.BeginFrame();

            Camera* activeCam = &player.Cam;

            if (editorMode) {
                UpdateEditorCamera(editorCamera, dt, !editor.WantsCaptureMouse() && !editor.GizmoEngaged());
                activeCam = &editorCamera;
                for (auto& pm : world.Models) {
                    pm.ModelRef->UpdateAnimation(dt);
                }
                editor.Draw(world, assets, editorCamera, dt);
            } else if (window.IsCursorLocked()) {
                player.Update(dt, world, window.Handle());

                bool leftNow = Input::IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT);
                if (leftNow && !prevLeftMouse) {
                    player.Shoot(world);
                }
                prevLeftMouse = leftNow;

                for (auto& pm : world.Models) {
                    pm.ModelRef->UpdateAnimation(dt);
                }
            }

            // Drawn every frame in both modes (unlike editor.Draw(), which is editor-mode-only)
            // so there's always an on-screen way back, not just F1. Drawn AFTER editor.Draw()
            // above so it layers on top of the toolbar strip instead of being painted over by it.
            editor.DrawPlayStopButton(editorMode);
            if (editor.ConsumePlayStopRequest()) toggleEditorMode();

            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // In editor mode the 3D scene renders into the dockspace's actual central node —
            // whatever's left after the Hierarchy/Inspector/Asset Browser panels take their
            // share — instead of always filling the whole window underneath them. The docked
            // panels paint over the rest, so nothing is left showing outside this rect anyway;
            // this just keeps the aspect ratio (and picking/gizmo math in EditorLayer) correct
            // as those panels get resized. In play mode there's no editor UI, so it's simply
            // the full window.
            float aspect;
            if (editorMode) {
                glm::vec2 vpPos = editor.ViewportPos();
                glm::vec2 vpSize = editor.ViewportSize();
                if (vpSize.x > 0.0f && vpSize.y > 0.0f) {
                    // ImGui's dock-node rect is top-left-origin; glViewport is bottom-left-origin.
                    int vx = (int)vpPos.x;
                    int vy = window.GetHeight() - (int)(vpPos.y + vpSize.y);
                    glViewport(vx, vy, (int)vpSize.x, (int)vpSize.y);
                    aspect = vpSize.x / vpSize.y;
                } else {
                    aspect = (float)window.GetWidth() / (float)window.GetHeight();
                }
            } else {
                glViewport(0, 0, window.GetWidth(), window.GetHeight());
                aspect = (float)window.GetWidth() / (float)window.GetHeight();
            }
            glm::mat4 view = activeCam->ViewMatrix();
            glm::mat4 proj = activeCam->ProjectionMatrix(aspect);
            glm::vec3 lightDir(-0.4f, -1.0f, -0.3f);

            sky.Draw(view, proj, world.SkyHorizonColor, world.SkyZenithColor);

            primitiveShader.Bind();
            primitiveShader.SetMat4("uView", view);
            primitiveShader.SetMat4("uProj", proj);
            primitiveShader.SetVec3("uLightDir", lightDir);

            for (const auto& box : world.Boxes) {
                if (!box.Alive) continue;
                glm::mat4 model = ComposeTransform(box.Center, box.RotationEuler, box.Size);
                primitiveShader.SetMat4("uModel", model);
                primitiveShader.SetVec3("uColor", box.Color);
                cube->Draw();
            }

            modelShader.Bind();
            modelShader.SetMat4("uView", view);
            modelShader.SetMat4("uProj", proj);
            modelShader.SetVec3("uLightDir", lightDir);
            modelShader.SetVec3("uLightColor", glm::vec3(3.0f));
            modelShader.SetVec3("uViewPos", activeCam->Position);

            for (auto& pm : world.Models) {
                glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
                modelShader.SetMat4("uModel", model);
                pm.ModelRef->Draw(modelShader);
            }

            if (editorMode) {
                // Selection outline: inverted-hull technique — draw each selected object again,
                // pushed out along its own normal, keeping only the back faces (front-face
                // culled) so a thin silhouette rim survives around the normal draw. Depth-tested
                // against the rest of the scene like everything else, so it's still occluded
                // correctly by other objects in front of it.
                auto selection = editor.GetSelectedItems();
                if (!selection.empty()) {
                    const glm::vec3 kOutlineColor(1.0f, 0.55f, 0.1f);
                    const float kOutlineThickness = 0.015f;
                    glCullFace(GL_FRONT);

                    for (const auto& [isModel, idx] : selection) {
                        if (isModel) {
                            if (idx < 0 || idx >= (int)world.Models.size()) continue;
                            PlacedModel& pm = world.Models[idx];
                            glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
                            outlineModelShader.Bind();
                            outlineModelShader.SetMat4("uModel", model);
                            outlineModelShader.SetMat4("uView", view);
                            outlineModelShader.SetMat4("uProj", proj);
                            outlineModelShader.SetFloat("uThickness", kOutlineThickness);
                            outlineModelShader.SetVec3("uOutlineColor", kOutlineColor);
                            pm.ModelRef->Draw(outlineModelShader); // also uploads bone matrices; unused material uniforms are harmless no-ops here
                        } else {
                            if (idx < 0 || idx >= (int)world.Boxes.size()) continue;
                            WorldBox& box = world.Boxes[idx];
                            glm::mat4 model = ComposeTransform(box.Center, box.RotationEuler, box.Size);
                            outlineBoxShader.Bind();
                            outlineBoxShader.SetMat4("uModel", model);
                            outlineBoxShader.SetMat4("uView", view);
                            outlineBoxShader.SetMat4("uProj", proj);
                            outlineBoxShader.SetFloat("uThickness", kOutlineThickness);
                            outlineBoxShader.SetVec3("uOutlineColor", kOutlineColor);
                            cube->Draw();
                        }
                    }

                    glCullFace(GL_BACK);
                }
            }

            if (editorMode && editor.ShowGrid()) {
                grid.Draw(view, proj, activeCam->Position, editor.GridMinorSpacing(), 10.0f, 80.0f);
            }

            editor.EndFrame(); // flushes the ImGui frame (empty when not in editor mode)

            window.SwapBuffers();
        }

        SceneSerializer::Save(world, assets, editor.CurrentScenePath());

        editor.Shutdown();
        AudioEngine::Shutdown();
        delete cube;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
