#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct MeshFace {
    std::vector<int> indices;
    ccColor4F baseColor{0.72f, 0.82f, 0.96f, 1.f};
};

struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<MeshFace> faces;
    // Flat normals drive hard-edge detection; averaged vertex normals provide
    // smooth illumination for dense meshes such as UV spheres.
    std::vector<Vec3> faceNormals;
    std::vector<Vec3> smoothFaceNormals;
    std::vector<std::vector<uint8_t>> outlineEdges;
};

struct FaceDraw {
    std::vector<CCPoint> points;
    ccColor4F color{1.f, 1.f, 1.f, 1.f};
    float averageDepth = 0.f;
    size_t meshFaceIndex = 0;
};

static void rebuildMeshSurfaceData(Mesh& mesh, bool assignOrientationColors);
static std::shared_ptr<Mesh> g_importedMesh;

struct BakeResult {
    bool success = false;
    size_t objectCount = 0;
    int frameCount = 0;
    std::string error;
};

static Vec3 rotateCubePoint(Vec3 p, float angleX, float angleY, float angleZ) {
    const float cx = std::cos(angleX);
    const float sx = std::sin(angleX);
    const float cy = std::cos(angleY);
    const float sy = std::sin(angleY);
    const float cz = std::cos(angleZ);
    const float sz = std::sin(angleZ);

    Vec3 xRot{p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx};
    Vec3 yRot{xRot.x * cy + xRot.z * sy, xRot.y, -xRot.x * sy + xRot.z * cy};
    return Vec3{
        yRot.x * cz - yRot.y * sz,
        yRot.x * sz + yRot.y * cz,
        yRot.z
    };
}

static Vec3 subtract(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 crossProduct(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static float dotProduct(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 normalized(Vec3 value) {
    const float length = std::sqrt(dotProduct(value, value));
    if (length < 0.00001f) return {0.f, 0.f, 1.f};
    return {value.x / length, value.y / length, value.z / length};
}

static constexpr float kHardEdgeCosine = 0.78f; // Draw lines only across edges sharper than ~39 degrees.

static ccColor4F shadeFace(ccColor4F base, Vec3 normal) {
    // Fixed view-space light from the upper-right/front of the model.
    // Smooth normals keep high-poly spheres shaded instead of making each
    // tiny triangle look like an individually outlined black tile.
    const Vec3 lightDirection = normalized({0.56f, 0.76f, 0.52f});
    const float diffuse = std::max(0.f, dotProduct(normalized(normal), lightDirection));
    const float intensity = 0.22f + 0.78f * diffuse;
    return {
        std::min(1.f, base.r * intensity),
        std::min(1.f, base.g * intensity),
        std::min(1.f, base.b * intensity),
        base.a
    };
}

// Geometry Dash stores per-object HSV adjustments relative to a color channel.
// The bake assigns a fresh custom channel and initializes it to pure red with a
// normal Color trigger, so per-object HSV values produce stable rendered colors.
static std::string rgbToHsv(ccColor4F color) {
    const float r = std::clamp(color.r, 0.f, 1.f);
    const float g = std::clamp(color.g, 0.f, 1.f);
    const float b = std::clamp(color.b, 0.f, 1.f);
    const float maximum = std::max({r, g, b});
    const float minimum = std::min({r, g, b});
    const float delta = maximum - minimum;
    float hue = 0.f;

    if (delta > 0.00001f) {
        if (maximum == r) hue = std::fmod(60.f * ((g - b) / delta) + 360.f, 360.f);
        else if (maximum == g) hue = 60.f * ((b - r) / delta) + 120.f;
        else hue = 60.f * ((r - g) / delta) + 240.f;
    }
    // GD stores hue offsets in the signed -180..180 range. Keeping hue signed
    // avoids values above 180 being clamped by the game when rendering blues/purples.
    if (hue > 180.f) hue -= 360.f;
    const float saturation = maximum <= 0.00001f ? 0.f : delta / maximum;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << hue << 'a'
        << std::setprecision(5) << saturation << 'a' << maximum << "a0a0";
    return out.str();
}

static bool pointInPolygon(std::vector<CCPoint> const& polygon, float x, float y) {
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i];
        const auto& b = polygon[j];
        const bool crosses = ((a.y > y) != (b.y > y)) &&
            (x < (b.x - a.x) * (y - a.y) / ((b.y - a.y) == 0.f ? 0.0001f : (b.y - a.y)) + a.x);
        if (crosses) inside = !inside;
    }
    return inside;
}

static std::string number(float value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

static std::string numberPrecise(float value, int precision = 4) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}


static Mesh makeCubeMesh() {
    Mesh mesh;
    mesh.vertices = {
        {-1.f, -1.f, -1.f}, { 1.f, -1.f, -1.f},
        { 1.f,  1.f, -1.f}, {-1.f,  1.f, -1.f},
        {-1.f, -1.f,  1.f}, { 1.f, -1.f,  1.f},
        { 1.f,  1.f,  1.f}, {-1.f,  1.f,  1.f}
    };
    mesh.faces = {
        {{0, 1, 2, 3}, {0.24f, 0.53f, 0.98f, 1.f}},
        {{4, 7, 6, 5}, {0.15f, 0.82f, 0.68f, 1.f}},
        {{0, 4, 5, 1}, {0.95f, 0.35f, 0.31f, 1.f}},
        {{3, 2, 6, 7}, {0.96f, 0.75f, 0.25f, 1.f}},
        {{0, 3, 7, 4}, {0.68f, 0.40f, 0.95f, 1.f}},
        {{1, 5, 6, 2}, {0.24f, 0.78f, 0.36f, 1.f}}
    };
    rebuildMeshSurfaceData(mesh, false);
    return mesh;
}

static Mesh const& currentMesh() {
    static Mesh cube = makeCubeMesh();
    return g_importedMesh ? *g_importedMesh : cube;
}

static ccColor4F orientationColor(Vec3 normal) {
    // Color-code the dominant local face direction so an untextured OBJ is
    // immediately readable before material/MTL support is added.
    const float ax = std::abs(normal.x);
    const float ay = std::abs(normal.y);
    const float az = std::abs(normal.z);
    if (ax >= ay && ax >= az) {
        return normal.x >= 0.f
            ? ccColor4F{0.92f, 0.30f, 0.27f, 1.f}
            : ccColor4F{0.98f, 0.58f, 0.20f, 1.f};
    }
    if (ay >= ax && ay >= az) {
        return normal.y >= 0.f
            ? ccColor4F{0.27f, 0.82f, 0.43f, 1.f}
            : ccColor4F{0.95f, 0.78f, 0.22f, 1.f};
    }
    return normal.z >= 0.f
        ? ccColor4F{0.27f, 0.57f, 0.98f, 1.f}
        : ccColor4F{0.66f, 0.38f, 0.92f, 1.f};
}

static Vec3 faceNormal(Mesh const& mesh, MeshFace const& face) {
    if (face.indices.size() < 3) return {0.f, 0.f, 1.f};
    const Vec3 a = mesh.vertices[face.indices[0]];
    const Vec3 b = mesh.vertices[face.indices[1]];
    const Vec3 c = mesh.vertices[face.indices[2]];
    Vec3 normal = normalized(crossProduct(subtract(b, a), subtract(c, a)));
    Vec3 center{0.f, 0.f, 0.f};
    for (int index : face.indices) {
        center.x += mesh.vertices[index].x / static_cast<float>(face.indices.size());
        center.y += mesh.vertices[index].y / static_cast<float>(face.indices.size());
        center.z += mesh.vertices[index].z / static_cast<float>(face.indices.size());
    }
    if (dotProduct(normal, center) < 0.f) {
        normal = {-normal.x, -normal.y, -normal.z};
    }
    return normal;
}

static uint64_t undirectedEdgeKey(int a, int b) {
    const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
    const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
}

static void rebuildMeshSurfaceData(Mesh& mesh, bool assignOrientationColors) {
    mesh.faceNormals.clear();
    mesh.smoothFaceNormals.clear();
    mesh.outlineEdges.clear();
    mesh.faceNormals.reserve(mesh.faces.size());
    mesh.smoothFaceNormals.reserve(mesh.faces.size());
    mesh.outlineEdges.resize(mesh.faces.size());

    std::vector<Vec3> vertexNormalSums(mesh.vertices.size(), Vec3{0.f, 0.f, 0.f});
    for (size_t faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
        auto const& face = mesh.faces[faceIndex];
        const Vec3 normal = faceNormal(mesh, face);
        mesh.faceNormals.push_back(normal);
        for (int vertexIndex : face.indices) {
            auto& sum = vertexNormalSums[static_cast<size_t>(vertexIndex)];
            sum.x += normal.x; sum.y += normal.y; sum.z += normal.z;
        }
        if (assignOrientationColors) {
            mesh.faces[faceIndex].baseColor = orientationColor(normal);
        }
    }

    std::vector<Vec3> vertexNormals;
    vertexNormals.reserve(vertexNormalSums.size());
    for (auto const& sum : vertexNormalSums) vertexNormals.push_back(normalized(sum));

    for (size_t faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
        auto const& face = mesh.faces[faceIndex];
        Vec3 smooth{0.f, 0.f, 0.f};
        for (int vertexIndex : face.indices) {
            const auto& normal = vertexNormals[static_cast<size_t>(vertexIndex)];
            smooth.x += normal.x; smooth.y += normal.y; smooth.z += normal.z;
        }
        smooth = normalized(smooth);
        if (dotProduct(smooth, mesh.faceNormals[faceIndex]) < 0.f) {
            smooth = {-smooth.x, -smooth.y, -smooth.z};
        }
        mesh.smoothFaceNormals.push_back(smooth);
        mesh.outlineEdges[faceIndex].assign(face.indices.size(), 0);
    }

    // Build edge adjacency once. A line is shown only on open boundaries or
    // genuinely sharp creases. Smooth sphere triangulation seams are omitted.
    struct EdgeUse { size_t faceIndex; size_t edgeIndex; };
    std::unordered_map<uint64_t, std::vector<EdgeUse>> edgeUses;
    edgeUses.reserve(mesh.faces.size() * 3);
    for (size_t faceIndex = 0; faceIndex < mesh.faces.size(); ++faceIndex) {
        auto const& indices = mesh.faces[faceIndex].indices;
        for (size_t edgeIndex = 0; edgeIndex < indices.size(); ++edgeIndex) {
            const int a = indices[edgeIndex];
            const int b = indices[(edgeIndex + 1) % indices.size()];
            edgeUses[undirectedEdgeKey(a, b)].push_back({faceIndex, edgeIndex});
        }
    }

    for (auto const& [key, uses] : edgeUses) {
        (void)key;
        bool isSharp = uses.size() == 1;
        if (uses.size() > 1) {
            for (size_t i = 0; i < uses.size() && !isSharp; ++i) {
                for (size_t j = i + 1; j < uses.size(); ++j) {
                    const float alignment = dotProduct(
                        mesh.faceNormals[uses[i].faceIndex],
                        mesh.faceNormals[uses[j].faceIndex]
                    );
                    if (alignment < kHardEdgeCosine) {
                        isSharp = true;
                        break;
                    }
                }
            }
        }
        if (isSharp) {
            for (auto const& use : uses) {
                mesh.outlineEdges[use.faceIndex][use.edgeIndex] = 1;
            }
        }
    }
}

static void weldCoincidentVertices(Mesh& mesh) {
    // OBJ exporters sometimes duplicate the same geometric point for every
    // triangle or at UV seams. UVs are not used yet, so weld positions to restore
    // shared topology and allow smooth sphere normals / clean crease detection.
    constexpr double epsilon = 0.00001;
    std::map<std::tuple<int64_t, int64_t, int64_t>, int> unique;
    std::vector<Vec3> welded;
    std::vector<int> remap(mesh.vertices.size(), -1);
    welded.reserve(mesh.vertices.size());

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& v = mesh.vertices[i];
        const auto key = std::make_tuple(
            static_cast<int64_t>(std::llround(static_cast<double>(v.x) / epsilon)),
            static_cast<int64_t>(std::llround(static_cast<double>(v.y) / epsilon)),
            static_cast<int64_t>(std::llround(static_cast<double>(v.z) / epsilon))
        );
        auto [it, inserted] = unique.emplace(key, static_cast<int>(welded.size()));
        if (inserted) welded.push_back(v);
        remap[i] = it->second;
    }

    for (auto& face : mesh.faces) {
        for (int& index : face.indices) index = remap[static_cast<size_t>(index)];
    }
    mesh.faces.erase(std::remove_if(mesh.faces.begin(), mesh.faces.end(), [](MeshFace const& face) {
        std::unordered_set<int> uniqueIndices(face.indices.begin(), face.indices.end());
        return uniqueIndices.size() < 3;
    }), mesh.faces.end());
    mesh.vertices = std::move(welded);
}

static void normalizeMesh(Mesh& mesh) {
    if (mesh.vertices.empty()) return;
    Vec3 min = mesh.vertices.front();
    Vec3 max = mesh.vertices.front();
    for (auto const& v : mesh.vertices) {
        min.x = std::min(min.x, v.x); min.y = std::min(min.y, v.y); min.z = std::min(min.z, v.z);
        max.x = std::max(max.x, v.x); max.y = std::max(max.y, v.y); max.z = std::max(max.z, v.z);
    }
    const Vec3 center{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    const float extent = std::max({max.x - min.x, max.y - min.y, max.z - min.z});
    if (extent >= 0.00001f) {
        const float scale = 2.f / extent;
        for (auto& v : mesh.vertices) {
            v.x = (v.x - center.x) * scale;
            v.y = (v.y - center.y) * scale;
            v.z = (v.z - center.z) * scale;
        }
    }
    weldCoincidentVertices(mesh);
    rebuildMeshSurfaceData(mesh, true);
}

static bool parseObj(std::string const& text, Mesh& mesh, std::string& error) {
    mesh = Mesh{};
    std::istringstream input(text);
    std::string line;
    size_t lineNumber = 0;
    constexpr size_t maxVertices = 30000;
    constexpr size_t maxFaces = 5000;
    constexpr size_t maxFaceVertices = 128;

    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string kind;
        row >> kind;
        if (kind == "v") {
            Vec3 vertex{};
            if (!(row >> vertex.x >> vertex.y >> vertex.z) ||
                !std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
                error = "Invalid vertex on OBJ line " + std::to_string(lineNumber) + ".";
                return false;
            }
            if (mesh.vertices.size() >= maxVertices) {
                error = "OBJ has more than 30,000 vertices; simplify the model first.";
                return false;
            }
            mesh.vertices.push_back(vertex);
        } else if (kind == "f") {
            MeshFace face;
            std::string token;
            while (row >> token) {
                const size_t slash = token.find('/');
                const std::string indexText = token.substr(0, slash);
                if (indexText.empty()) continue;
                char* end = nullptr;
                const long rawIndex = std::strtol(indexText.c_str(), &end, 10);
                if (end == indexText.c_str() || *end != '\0' || rawIndex == 0) {
                    error = "Invalid face index on OBJ line " + std::to_string(lineNumber) + ".";
                    return false;
                }
                const long resolved = rawIndex > 0
                    ? rawIndex - 1
                    : static_cast<long>(mesh.vertices.size()) + rawIndex;
                if (resolved < 0 || resolved >= static_cast<long>(mesh.vertices.size())) {
                    error = "Face index is out of range on OBJ line " + std::to_string(lineNumber) + ".";
                    return false;
                }
                face.indices.push_back(static_cast<int>(resolved));
                if (face.indices.size() > maxFaceVertices) {
                    error = "An OBJ face has more than 128 vertices; triangulate the model first.";
                    return false;
                }
            }
            if (face.indices.size() >= 3) {
                mesh.faces.push_back(std::move(face));
                if (mesh.faces.size() > maxFaces) {
                    error = "OBJ has more than 5,000 faces; simplify the model first.";
                    return false;
                }
            }
        }
        // vt, vn, o, g, s, mtllib and usemtl are safely ignored in this first
        // geometry-only importer. Companion MTL textures/materials come later.
    }

    if (mesh.vertices.size() < 3 || mesh.faces.empty()) {
        error = "No renderable faces were found. This importer needs OBJ v and f records.";
        return false;
    }
    normalizeMesh(mesh);
    return true;
}

static std::vector<Vec3> rotateMeshVertices(Mesh const& mesh, float angleX, float angleY, float angleZ) {
    std::vector<Vec3> rotated;
    rotated.reserve(mesh.vertices.size());
    for (auto const& vertex : mesh.vertices) {
        rotated.push_back(rotateCubePoint(vertex, angleX, angleY, angleZ));
    }
    return rotated;
}

// Builds a short looping flipbook from ordinary GD small-block objects and
// Spawn/Toggle triggers. No Geode code is needed when the level is played.
static BakeResult bakeSpinningMesh(LevelEditorLayer* editorLayer, Mesh const& mesh, bool showLines, bool shadingOnly) {
    BakeResult result;
    if (!editorLayer || !editorLayer->m_objectLayer) {
        result.error = "The level editor's object layer is unavailable.";
        return result;
    }
    if (mesh.vertices.empty() || mesh.faces.empty()) {
        result.error = "There is no loaded mesh to bake.";
        return result;
    }

    constexpr int frameCount = 12;
    constexpr int gridSize = 32;
    constexpr float fps = 6.f;
    constexpr float frameDelay = 1.f / fps;
    constexpr float cellSize = 7.5f;
    constexpr float focalLength = 420.f;
    constexpr float cameraDistance = 5.f;
    constexpr float pi = 3.14159265358979323846f;

    std::unordered_set<int> reservedGroups;
    std::vector<int> allocatedGroups;
    allocatedGroups.reserve(frameCount * 2 + 1);
    for (int i = 0; i < frameCount * 2 + 1; ++i) {
        const int group = editorLayer->getNextFreeGroupID(reservedGroups);
        if (group <= 0) {
            result.error = "Couldn't allocate enough free group IDs in this level.";
            return result;
        }
        reservedGroups.insert(group);
        allocatedGroups.push_back(group);
    }

    std::vector<int> frameGroups(allocatedGroups.begin(), allocatedGroups.begin() + frameCount);
    const int setupGroup = allocatedGroups[frameCount];
    std::vector<int> stageGroups(allocatedGroups.begin() + frameCount + 1, allocatedGroups.end());

    const int colorChannel = editorLayer->getNextColorChannel();
    if (colorChannel <= 0 || colorChannel > 999) {
        result.error = "Couldn't allocate an unused custom color channel (1-999).";
        return result;
    }

    auto win = CCDirector::sharedDirector()->getWinSize();
    CCPoint screenCenter = CCPointMake(win.width * 0.5f, win.height * 0.5f);
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (scene) screenCenter = scene->convertToWorldSpace(screenCenter);
    const CCPoint worldCenter = editorLayer->m_objectLayer->convertToNodeSpace(screenCenter);

    std::vector<std::string> objectRecords;
    objectRecords.reserve(frameCount * gridSize * gridSize + 64);
    size_t pixelObjectCount = 0;
    const ccColor4F outlineColor{0.025f, 0.035f, 0.055f, 1.f};
    const std::string outlineHSV = rgbToHsv(outlineColor);

    struct ProjectedFace {
        std::vector<CCPoint> points;
        ccColor4F color;
        Vec3 normal;
        size_t meshFaceIndex = 0;
        float depth = 0.f;
    };

    for (int frame = 0; frame < frameCount; ++frame) {
        const float angleX = 0.34f;
        const float angleY = (2.f * pi * static_cast<float>(frame)) / static_cast<float>(frameCount);
        const float angleZ = 0.12f;
        auto rotated = rotateMeshVertices(mesh, angleX, angleY, angleZ);

        std::vector<ProjectedFace> faces;
        faces.reserve(mesh.faces.size());
        for (size_t meshFaceIndex = 0; meshFaceIndex < mesh.faces.size(); ++meshFaceIndex) {
            auto const& face = mesh.faces[meshFaceIndex];
            if (face.indices.size() < 3) continue;
            ProjectedFace projected;
            projected.meshFaceIndex = meshFaceIndex;
            projected.depth = 0.f;
            Vec3 center{0.f, 0.f, 0.f};
            for (int index : face.indices) {
                const auto& p = rotated[index];
                const float distance = std::max(0.5f, cameraDistance - p.z);
                projected.points.push_back(CCPointMake(p.x * focalLength / distance,
                                                        p.y * focalLength / distance));
                projected.depth += distance;
                center.x += p.x / static_cast<float>(face.indices.size());
                center.y += p.y / static_cast<float>(face.indices.size());
                center.z += p.z / static_cast<float>(face.indices.size());
            }
            projected.depth /= static_cast<float>(face.indices.size());
            Vec3 normal = normalized(crossProduct(
                subtract(rotated[face.indices[1]], rotated[face.indices[0]]),
                subtract(rotated[face.indices[2]], rotated[face.indices[0]])
            ));
            if (dotProduct(normal, center) < 0.f) normal = {-normal.x, -normal.y, -normal.z};
            projected.normal = normal;
            const size_t sourceFaceIndex = projected.meshFaceIndex;
            const Vec3 smoothNormal = sourceFaceIndex < mesh.smoothFaceNormals.size()
                ? rotateCubePoint(mesh.smoothFaceNormals[sourceFaceIndex], angleX, angleY, angleZ)
                : normal;
            const ccColor4F base = shadingOnly
                ? ccColor4F{0.92f, 0.94f, 0.98f, 1.f}
                : face.baseColor;
            projected.color = shadeFace(base, smoothNormal);
            faces.push_back(std::move(projected));
        }
        std::sort(faces.begin(), faces.end(), [](ProjectedFace const& a, ProjectedFace const& b) {
            return a.depth > b.depth;
        });

        const size_t pixelCount = static_cast<size_t>(gridSize * gridSize);
        std::vector<int> pixelFace(pixelCount, -1);
        std::vector<float> pixelDepth(pixelCount, std::numeric_limits<float>::max());
        std::vector<ccColor4F> pixelColors(pixelCount, ccColor4F{0.f, 0.f, 0.f, 0.f});

        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            auto const& face = faces[faceIndex];
            float minX = face.points.front().x;
            float maxX = minX;
            float minY = face.points.front().y;
            float maxY = minY;
            for (auto const& point : face.points) {
                minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
                minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
            }
            const int minCol = std::max(0, static_cast<int>(std::floor(minX / cellSize + gridSize * 0.5f - 0.5f)));
            const int maxCol = std::min(gridSize - 1, static_cast<int>(std::ceil(maxX / cellSize + gridSize * 0.5f - 0.5f)));
            const int minRow = std::max(0, static_cast<int>(std::floor(minY / cellSize + gridSize * 0.5f - 0.5f)));
            const int maxRow = std::min(gridSize - 1, static_cast<int>(std::ceil(maxY / cellSize + gridSize * 0.5f - 0.5f)));

            for (int row = minRow; row <= maxRow; ++row) {
                for (int col = minCol; col <= maxCol; ++col) {
                    const float localX = (static_cast<float>(col) + 0.5f - gridSize * 0.5f) * cellSize;
                    const float localY = (static_cast<float>(row) + 0.5f - gridSize * 0.5f) * cellSize;
                    if (!pointInPolygon(face.points, localX, localY)) continue;
                    const size_t pixel = static_cast<size_t>(row * gridSize + col);
                    if (face.depth < pixelDepth[pixel]) {
                        pixelDepth[pixel] = face.depth;
                        pixelFace[pixel] = static_cast<int>(faceIndex);
                        pixelColors[pixel] = face.color;
                    }
                }
            }
        }

        // Optional outlines only mark silhouettes and sharp creases. Adjacent
        // triangles on a smooth sphere are treated as one surface, not as seams.
        std::vector<uint8_t> edgePixels(pixelCount, 0);
        if (showLines) {
            for (int row = 0; row < gridSize; ++row) {
                for (int col = 0; col < gridSize; ++col) {
                    const size_t pixel = static_cast<size_t>(row * gridSize + col);
                    if (pixelFace[pixel] < 0) continue;
                    const int neighbors[4][2] = {{col - 1, row}, {col + 1, row}, {col, row - 1}, {col, row + 1}};
                    for (auto const& neighbor : neighbors) {
                        const int nx = neighbor[0];
                        const int ny = neighbor[1];
                        if (nx < 0 || ny < 0 || nx >= gridSize || ny >= gridSize) {
                            edgePixels[pixel] = 1;
                            break;
                        }
                        const size_t otherPixel = static_cast<size_t>(ny * gridSize + nx);
                        if (pixelFace[otherPixel] < 0) {
                            edgePixels[pixel] = 1;
                            break;
                        }
                        const Vec3 a = faces[static_cast<size_t>(pixelFace[pixel])].normal;
                        const Vec3 b = faces[static_cast<size_t>(pixelFace[otherPixel])].normal;
                        if (dotProduct(a, b) < kHardEdgeCosine) {
                            edgePixels[pixel] = 1;
                            break;
                        }
                    }
                }
            }
        }

        size_t framePixels = 0;
        for (int row = 0; row < gridSize; ++row) {
            for (int col = 0; col < gridSize; ++col) {
                const size_t pixel = static_cast<size_t>(row * gridSize + col);
                if (pixelFace[pixel] < 0) continue;
                const float x = worldCenter.x +
                    (static_cast<float>(col) + 0.5f - gridSize * 0.5f) * cellSize;
                const float y = worldCenter.y +
                    (static_cast<float>(row) + 0.5f - gridSize * 0.5f) * cellSize;
                const std::string hsv = edgePixels[pixel] ? outlineHSV : rgbToHsv(pixelColors[pixel]);
                objectRecords.push_back(
                    "1,917,2," + number(x) + ",3," + number(y) +
                    ",21," + std::to_string(colorChannel) + ",41,1,43," + hsv + ",57," +
                    std::to_string(frameGroups[frame])
                );
                ++framePixels;
            }
        }
        if (framePixels == 0) {
            result.error = "The mesh rasterizer produced an empty frame. Try a larger or more centered OBJ.";
            return result;
        }
        pixelObjectCount += framePixels;
    }

    auto addTrigger = [&objectRecords](int objectID, std::string const& properties, int group) {
        objectRecords.push_back(
            "1," + std::to_string(objectID) + ",2,0,3,0," + properties +
            ",33," + std::to_string(group) + ",62,1,87,1"
        );
    };
    auto addToggle = [&addTrigger](int targetGroup, bool activate, int parentGroup) {
        addTrigger(1049, "51," + std::to_string(targetGroup) +
                         ",56," + (activate ? "1" : "0"), parentGroup);
    };
    auto addSpawn = [&addTrigger, frameDelay](int targetGroup, int parentGroup) {
        addTrigger(1268, "51," + std::to_string(targetGroup) +
                         ",63," + numberPrecise(frameDelay), parentGroup);
    };

    addTrigger(899, "23," + std::to_string(colorChannel) +
                    ",7,255,8,0,9,0,10,0,17,0,35,1", setupGroup);
    objectRecords.push_back("1,1268,2,0,3,0,51," + std::to_string(setupGroup));
    for (int frame = 1; frame < frameCount; ++frame) {
        addToggle(frameGroups[frame], false, setupGroup);
    }
    addSpawn(stageGroups[1], setupGroup);
    for (int frame = 0; frame < frameCount; ++frame) {
        const int previous = (frame + frameCount - 1) % frameCount;
        const int next = (frame + 1) % frameCount;
        addToggle(frameGroups[previous], false, stageGroups[frame]);
        addToggle(frameGroups[frame], true, stageGroups[frame]);
        addSpawn(stageGroups[next], stageGroups[frame]);
    }

    std::ostringstream levelData;
    for (size_t i = 0; i < objectRecords.size(); ++i) {
        if (i) levelData << ';';
        levelData << objectRecords[i];
    }
    levelData << ';';

    auto created = editorLayer->createObjectsFromString(levelData.str(), false, true);
    if (!created || created->count() == 0) {
        result.error = "Geometry Dash did not create any of the baked objects.";
        return result;
    }
    editorLayer->updateOptions();
    result.success = true;
    result.objectCount = created->count();
    result.frameCount = frameCount;
    log::info("Baked 3D mesh: {} objects ({} image cells), {} frames, 6 FPS",
              result.objectCount, pixelObjectCount, frameCount);
    return result;
}


// Trigger-driven baseplate renderer (v0.4): each mesh edge is one reusable
// stretched GD block. A loop of ordinary Move/Rotate/Scale triggers updates
// those bars through a set of projected poses. The level contains no Geode
// runtime dependency after baking. This is a sampled transform path, not a
// direct Item-ID-to-coordinate dataflow: Item Edit writes Item IDs, while the
// vanilla Move/Rotate/Scale trigger fields accept numeric values or group IDs.
static BakeResult bakeBaseplateRig(LevelEditorLayer* editorLayer, Mesh const& mesh) {
    BakeResult result;
    if (!editorLayer || !editorLayer->m_objectLayer) {
        result.error = "The level editor's object layer is unavailable.";
        return result;
    }
    if (mesh.vertices.empty() || mesh.faces.empty()) {
        result.error = "There is no loaded mesh to build.";
        return result;
    }

    constexpr int frameCount = 24;
    constexpr float fps = 12.f;
    constexpr float frameDelay = 1.f / fps;
    constexpr float baseplateSize = 7.5f; // Object 917 is a 7.5-unit colored square.
    constexpr float barThickness = 2.25f;
    constexpr float cameraDistance = 5.0f;
    constexpr float focalLength = 420.f;
    constexpr size_t maxEdges = 64;
    constexpr float pi = 3.14159265358979323846f;

    struct Edge { int a; int b; };
    std::set<std::pair<int, int>> uniqueEdges;
    for (auto const& face : mesh.faces) {
        if (face.indices.size() < 2) continue;
        for (size_t i = 0; i < face.indices.size(); ++i) {
            int a = face.indices[i];
            int b = face.indices[(i + 1) % face.indices.size()];
            if (a == b || a < 0 || b < 0 ||
                static_cast<size_t>(a) >= mesh.vertices.size() ||
                static_cast<size_t>(b) >= mesh.vertices.size()) continue;
            if (a > b) std::swap(a, b);
            uniqueEdges.emplace(a, b);
        }
    }

    std::vector<Edge> edges;
    edges.reserve(uniqueEdges.size());
    for (auto const& [a, b] : uniqueEdges) edges.push_back({a, b});
    if (edges.empty()) {
        result.error = "The OBJ has no usable polygon edges.";
        return result;
    }

    const size_t originalEdgeCount = edges.size();
    if (edges.size() > maxEdges) {
        // Deterministic uniform sampling keeps dense meshes from generating
        // thousands of trigger objects. Users can simplify a model for better
        // edge coverage, or switch to the legacy raster bake for dense meshes.
        std::vector<Edge> sampled;
        sampled.reserve(maxEdges);
        for (size_t i = 0; i < maxEdges; ++i) {
            const size_t sourceIndex = (i * edges.size()) / maxEdges;
            sampled.push_back(edges[sourceIndex]);
        }
        edges = std::move(sampled);
    }

    // Allocate one group per reusable baseplate and one per transform stage.
    // A fixed pool is reused for the entire loop: group count is independent
    // of animation duration.
    std::unordered_set<int> reservedGroups;
    std::vector<int> edgeGroups;
    edgeGroups.reserve(edges.size());
    for (size_t i = 0; i < edges.size(); ++i) {
        const int group = editorLayer->getNextFreeGroupID(reservedGroups);
        if (group <= 0) {
            result.error = "Couldn't allocate enough free Group IDs for the baseplate bars.";
            return result;
        }
        reservedGroups.insert(group);
        edgeGroups.push_back(group);
    }

    const int setupGroup = editorLayer->getNextFreeGroupID(reservedGroups);
    if (setupGroup <= 0) {
        result.error = "Couldn't allocate a setup Group ID.";
        return result;
    }
    reservedGroups.insert(setupGroup);

    std::vector<int> stageGroups;
    stageGroups.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        const int group = editorLayer->getNextFreeGroupID(reservedGroups);
        if (group <= 0) {
            result.error = "Couldn't allocate enough free Group IDs for the animation loop.";
            return result;
        }
        reservedGroups.insert(group);
        stageGroups.push_back(group);
    }

    const int colorChannel = editorLayer->getNextColorChannel();
    if (colorChannel <= 0 || colorChannel > 999) {
        result.error = "Couldn't allocate an unused custom color channel (1-999).";
        return result;
    }

    auto win = CCDirector::sharedDirector()->getWinSize();
    CCPoint screenCenter = CCPointMake(win.width * 0.5f, win.height * 0.5f);
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (scene) screenCenter = scene->convertToWorldSpace(screenCenter);
    const CCPoint worldCenter = editorLayer->m_objectLayer->convertToNodeSpace(screenCenter);

    struct BarPose {
        CCPoint center;
        float length = baseplateSize;
        float rotation = 0.f; // GD rotation convention: clockwise-positive.
        float scaleX = 1.f;
        float scaleY = barThickness / baseplateSize;
    };

    auto projectVertex = [&](Vec3 p, float angleX, float angleY, float angleZ) -> CCPoint {
        const Vec3 r = rotateCubePoint(p, angleX, angleY, angleZ);
        const float distance = std::max(1.35f, cameraDistance - r.z);
        return CCPointMake(
            worldCenter.x + r.x * focalLength / distance,
            worldCenter.y + r.y * focalLength / distance
        );
    };

    std::vector<std::vector<BarPose>> poses(
        frameCount, std::vector<BarPose>(edges.size())
    );
    for (int frame = 0; frame < frameCount; ++frame) {
        const float phase = (2.f * pi * static_cast<float>(frame)) / static_cast<float>(frameCount);
        const float angleY = phase;
        const float angleX = 0.38f + 0.12f * std::sin(phase * 2.f);
        const float angleZ = 0.10f;
        for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
            const auto& edge = edges[edgeIndex];
            const CCPoint a = projectVertex(mesh.vertices[edge.a], angleX, angleY, angleZ);
            const CCPoint b = projectVertex(mesh.vertices[edge.b], angleX, angleY, angleZ);
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::max(1.5f, std::sqrt(dx * dx + dy * dy));
            auto& pose = poses[frame][edgeIndex];
            pose.center = CCPointMake((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            pose.length = length;
            pose.rotation = -std::atan2(dy, dx) * 180.f / pi;
            pose.scaleX = std::clamp(length / baseplateSize, 0.15f, 40.f);
            pose.scaleY = barThickness / baseplateSize;
        }
    }

    // Axis-colored bars make the 3D structure readable even without a texture.
    auto edgeColor = [&](Edge const& edge) -> ccColor4F {
        const Vec3 d = subtract(mesh.vertices[edge.b], mesh.vertices[edge.a]);
        const float ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
        if (ax >= ay && ax >= az) return {0.18f, 0.78f, 1.00f, 1.f}; // X: cyan
        if (ay >= ax && ay >= az) return {0.38f, 1.00f, 0.42f, 1.f}; // Y: green
        return {1.00f, 0.62f, 0.20f, 1.f};                         // Z: orange
    };

    std::vector<std::string> records;
    records.reserve(edges.size() + frameCount * edges.size() * 3 + frameCount + 4);

    // Initialize the fresh custom channel to red. Each bar's HSV adjustment
    // then supplies its axis color while still using a normal GD color channel.
    auto addGroupedTrigger = [&records](int objectID, std::string const& properties, int group) {
        records.push_back(
            "1," + std::to_string(objectID) + ",2,0,3,0," + properties +
            ",33," + std::to_string(group) + ",62,1,87,1"
        );
    };

    // Reusable stretched baseplates: one object for each selected mesh edge.
    for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
        const auto& pose = poses[0][edgeIndex];
        const std::string hsv = rgbToHsv(edgeColor(edges[edgeIndex]));
        records.push_back(
            "1,917,2," + number(pose.center.x) +
            ",3," + number(pose.center.y) +
            ",6," + number(pose.rotation) +
            ",21," + std::to_string(colorChannel) +
            ",41,1,43," + hsv +
            ",128," + number(pose.scaleX) +
            ",129," + number(pose.scaleY) +
            ",57," + std::to_string(edgeGroups[edgeIndex])
        );
    }

    // Setup is a normal spawn group. It initializes the color channel, then
    // starts the repeating transform-stage chain.
    addGroupedTrigger(
        899,
        "23," + std::to_string(colorChannel) + ",7,255,8,0,9,0,10,0,17,0,35,1",
        setupGroup
    );
    addGroupedTrigger(
        1268,
        "51," + std::to_string(stageGroups[0]) + ",63," + numberPrecise(frameDelay),
        setupGroup
    );

    auto shortestAngleDelta = [](float from, float to) {
        float delta = std::fmod(to - from + 540.f, 360.f) - 180.f;
        if (delta < -180.f) delta += 360.f;
        return delta;
    };

    for (int frame = 0; frame < frameCount; ++frame) {
        const int nextFrame = (frame + 1) % frameCount;
        const int stageGroup = stageGroups[frame];
        for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
            const auto& from = poses[frame][edgeIndex];
            const auto& to = poses[nextFrame][edgeIndex];
            const int edgeGroup = edgeGroups[edgeIndex];

            // Move Trigger values are in tenths of a block (10 = 30 editor units).
            const int moveX = static_cast<int>(std::lround((to.center.x - from.center.x) / 3.f));
            const int moveY = static_cast<int>(std::lround((to.center.y - from.center.y) / 3.f));
            if (moveX != 0 || moveY != 0) {
                addGroupedTrigger(
                    901,
                    "10," + numberPrecise(frameDelay) +
                    ",51," + std::to_string(edgeGroup) +
                    ",28," + std::to_string(moveX) +
                    ",29," + std::to_string(moveY),
                    stageGroup
                );
            }

            const float rotationDelta = shortestAngleDelta(from.rotation, to.rotation);
            if (std::abs(rotationDelta) > 0.05f) {
                addGroupedTrigger(
                    1346,
                    "10," + numberPrecise(frameDelay) +
                    ",68," + number(rotationDelta) +
                    ",69,0,51," + std::to_string(edgeGroup) +
                    ",71,0,70,0",
                    stageGroup
                );
            }

            // Scale Trigger's ScaleX/ScaleY are target multipliers. Relative
            // Scale is left off so each step uses the next absolute bar length.
            addGroupedTrigger(
                2067,
                "10," + numberPrecise(frameDelay) +
                ",150," + number(to.scaleX) +
                ",151," + number(to.scaleY) +
                ",153,0,154,0,51," + std::to_string(edgeGroup) +
                ",71,0,133,0,452,0,577,0",
                stageGroup
            );
        }

        // The stage triggers itself again through the next stage. Every stage
        // and transform is multi-triggered, so the same small pool is reused.
        addGroupedTrigger(
            1268,
            "51," + std::to_string(stageGroups[nextFrame]) + ",63," + numberPrecise(frameDelay),
            stageGroup
        );
    }

    // Level-start starter. All subsequent movement is handled by ordinary GD
    // triggers, so the exported rig does not need this Geode mod at runtime.
    records.push_back(
        "1,1268,2,0,3,0,51," + std::to_string(setupGroup)
    );

    std::ostringstream levelData;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i) levelData << ';';
        levelData << records[i];
    }
    levelData << ';';

    auto created = editorLayer->createObjectsFromString(levelData.str(), false, true);
    if (!created || created->count() == 0) {
        result.error = "Geometry Dash did not create the baseplate rig. No objects were added.";
        return result;
    }
    editorLayer->updateOptions();
    result.success = true;
    result.objectCount = created->count();
    result.frameCount = frameCount;
    std::ostringstream logMessage;
    logMessage << "Created trigger-driven baseplate rig: " << result.objectCount
               << " objects, " << edges.size() << " reusable bars, " << frameCount
               << " transform steps. Sampled " << originalEdgeCount << " mesh edges.";
    log::info("{}", logMessage.str());
    if (originalEdgeCount > edges.size()) {
        log::warn("Baseplate rig simplified {} mesh edges to {} bars.", originalEdgeCount, edges.size());
    }
    return result;
}

class CubeEditorPreview final : public CCLayerColor {
public:
    static CubeEditorPreview* create(LevelEditorLayer* editorLayer) {
        auto ret = new CubeEditorPreview();
        if (!ret) return nullptr;
        ret->m_editorLayer = editorLayer;
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

protected:
    bool init() override {
        auto win = CCDirector::sharedDirector()->getWinSize();
        if (!CCLayerColor::initWithColor(ccc4(0, 0, 0, 92), win.width, win.height)) {
            return false;
        }
        this->setID("gd3d-editor-preview"_spr);

        // Load persistent Geode settings as the preview's starting style.
        // Users can still override both choices with the in-preview toggles.
        const bool legacyStyle = Mod::get()->getSettingValue<bool>("legacy-style");
        m_showLines = Mod::get()->getSettingValue<bool>("legacy-outlines");
        m_shadingOnly = !legacyStyle;

        // Keep the editor visible behind a centered preview card.
        m_panelSize = CCSize(
            std::min(560.f, std::max(300.f, win.width - 36.f)),
            std::min(420.f, std::max(260.f, win.height - 36.f))
        );
        m_panel = CCLayerColor::create(ccc4(8, 14, 27, 244), m_panelSize.width, m_panelSize.height);
        if (!m_panel) return false;
        m_panel->setPosition({(win.width - m_panelSize.width) * 0.5f,
                              (win.height - m_panelSize.height) * 0.5f});
        this->addChild(m_panel, 1);

        m_cube = CCDrawNode::create();
        if (!m_cube) return false;
        m_panel->addChild(m_cube, 5);

        auto title = CCLabelBMFont::create("GD 3D - MODEL PREVIEW", "bigFont.fnt");
        if (title) {
            title->setScale(0.48f);
            title->setPosition({m_panelSize.width * 0.5f, m_panelSize.height - 27.f});
            m_panel->addChild(title, 10);
        }

        const bool legacyBake = Mod::get()->getSettingValue<bool>("legacy-raster-bake");
        auto help = CCLabelBMFont::create(legacyBake ? "IMPORT OBJ  |  LEGACY RASTER BAKE" : "IMPORT OBJ  |  BASEPLATE TRIGGER RIG", "chatFont.fnt");
        if (help) {
            help->setScale(0.45f);
            help->setOpacity(190);
            help->setPosition({m_panelSize.width * 0.5f, 17.f});
            m_panel->addChild(help, 10);
        }

        m_linesStatus = CCLabelBMFont::create("LINES: OFF", "chatFont.fnt");
        if (m_linesStatus) {
            m_linesStatus->setScale(0.52f);
            m_linesStatus->setColor(ccc3(190, 205, 230));
            m_linesStatus->setPosition({m_panelSize.width * 0.27f, 128.f});
            m_panel->addChild(m_linesStatus, 20);
        }
        m_styleStatus = CCLabelBMFont::create("STYLE: SHADING ONLY", "chatFont.fnt");
        if (m_styleStatus) {
            m_styleStatus->setScale(0.52f);
            m_styleStatus->setColor(ccc3(190, 205, 230));
            m_styleStatus->setPosition({m_panelSize.width * 0.73f, 128.f});
            m_panel->addChild(m_styleStatus, 20);
        }

        auto settingsMenu = CCMenu::create();
        auto linesToggle = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("TOGGLE LINES", "bigFont.fnt", "GJ_button_04.png", 0.48f),
            this, menu_selector(CubeEditorPreview::onToggleLines)
        );
        linesToggle->setPosition({m_panelSize.width * 0.27f, 101.f});
        settingsMenu->addChild(linesToggle);
        auto styleToggle = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("TOGGLE STYLE", "bigFont.fnt", "GJ_button_04.png", 0.48f),
            this, menu_selector(CubeEditorPreview::onToggleStyle)
        );
        styleToggle->setPosition({m_panelSize.width * 0.73f, 101.f});
        settingsMenu->addChild(styleToggle);
        settingsMenu->setPosition({0.f, 0.f});
        m_panel->addChild(settingsMenu, 20);

        auto modelMenu = CCMenu::create();
        auto importSprite = ButtonSprite::create("IMPORT OBJ", "bigFont.fnt", "GJ_button_02.png", 0.62f);
        auto importButton = CCMenuItemSpriteExtra::create(
            importSprite, this, menu_selector(CubeEditorPreview::onImportObj)
        );
        importButton->setPosition({m_panelSize.width * 0.27f, 55.f});
        modelMenu->addChild(importButton);

        auto bakeSprite = ButtonSprite::create(legacyBake ? "LEGACY BAKE" : "BAKE 3D RIG", "bigFont.fnt", "GJ_button_01.png", 0.62f);
        auto bakeButton = CCMenuItemSpriteExtra::create(
            bakeSprite, this, menu_selector(CubeEditorPreview::onBake)
        );
        bakeButton->setPosition({m_panelSize.width * 0.73f, 55.f});
        modelMenu->addChild(bakeButton);
        modelMenu->setPosition({0.f, 0.f});
        m_panel->addChild(modelMenu, 20);

        auto closeMenu = CCMenu::create();
        auto closeSprite = ButtonSprite::create("CLOSE", "bigFont.fnt", "GJ_button_06.png", 0.7f);
        auto closeButton = CCMenuItemSpriteExtra::create(
            closeSprite, this, menu_selector(CubeEditorPreview::onClose)
        );
        closeButton->setPosition({m_panelSize.width - 48.f, m_panelSize.height - 30.f});
        closeMenu->addChild(closeButton);
        closeMenu->setPosition({0.f, 0.f});
        m_panel->addChild(closeMenu, 20);

        this->scheduleUpdate();
        this->redrawCube();
        return true;
    }

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

    void onToggleLines(CCObject*) {
        m_showLines = !m_showLines;
        if (m_linesStatus) m_linesStatus->setString(m_showLines ? "LINES: ON" : "LINES: OFF");
        this->redrawCube();
    }

    void onToggleStyle(CCObject*) {
        m_shadingOnly = !m_shadingOnly;
        if (m_styleStatus) {
            m_styleStatus->setString(m_shadingOnly ? "STYLE: SHADING ONLY" : "STYLE: FACE COLORS");
        }
        this->redrawCube();
    }

    void onImportObj(CCObject*) {
        geode::utils::file::FilePickOptions options;
        geode::utils::file::FilePickOptions::Filter objFilter;
        objFilter.description = "Wavefront OBJ";
        objFilter.files.insert("obj");
        options.filters.push_back(std::move(objFilter));
        geode::async::spawn(
            geode::utils::file::pick(geode::utils::file::PickMode::OpenFile, std::move(options)),
            [](geode::utils::file::PickResult result) {
                if (!result.isOk()) {
                    const std::string error = result.unwrapErr();
                    FLAlertLayer::create("OBJ Import", error.c_str(), "OK")->show();
                    return;
                }
                auto path = result.unwrap();
                if (!path) return;
                auto contents = geode::utils::file::readString(*path);
                if (!contents.isOk()) {
                    FLAlertLayer::create("OBJ Import", contents.unwrapErr().toString().c_str(), "OK")->show();
                    return;
                }
                Mesh parsed;
                std::string error;
                if (!parseObj(contents.unwrap(), parsed, error)) {
                    FLAlertLayer::create("OBJ Import Failed", error.c_str(), "OK")->show();
                    return;
                }
                const size_t vertexCount = parsed.vertices.size();
                const size_t faceCount = parsed.faces.size();
                g_importedMesh = std::make_shared<Mesh>(std::move(parsed));
                std::ostringstream message;
                message << "Imported OBJ geometry successfully.\n\n"
                        << vertexCount << " vertices\n" << faceCount << " faces\n\n"
                        << "The model is normalized to fit the preview. Default view is shading-only with an upper-right light; toggle lines or face colors as needed. "
                        << "UVs, textures and MTL materials are not loaded yet.";
                FLAlertLayer::create("OBJ Imported!", message.str().c_str(), "NICE")->show();
            }
        );
    }

    void onBake(CCObject*) {
        const auto& mesh = currentMesh();
        const bool legacyBake = Mod::get()->getSettingValue<bool>("legacy-raster-bake");
        auto result = legacyBake
            ? bakeSpinningMesh(m_editorLayer, mesh, m_showLines, m_shadingOnly)
            : bakeBaseplateRig(m_editorLayer, mesh);
        if (!result.success) {
            FLAlertLayer::create("GD 3D Renderer", result.error.c_str(), "OK")->show();
            return;
        }
        std::ostringstream message;
        if (legacyBake) {
            message << "Legacy raster bake added " << result.objectCount
                    << " ordinary GD objects across " << result.frameCount << " frames.\n\n"
                    << "This is the original 32x32 flipbook renderer at 6 FPS, using "
                    << (m_shadingOnly ? "top-right grayscale lighting" : "orientation colors with lighting")
                    << (m_showLines ? " and optional silhouette/sharp-edge pixels" : " without outlines")
                    << ". This legacy path is preserved as a fallback.";
            FLAlertLayer::create("Legacy 3D bake added!", message.str().c_str(), "NICE")->show();
            return;
        }
        message << "Created " << result.objectCount << " ordinary GD objects.\n\n"
                << "The mesh is represented by reusable stretched baseplates, driven by a looping chain of "
                << "Move, Rotate and Scale triggers (" << result.frameCount << " transform steps at 12 FPS).\n\n"
                << "This is a trigger-only sampled wireframe rig: it keeps working with Geode disabled, "
                << "but it is not yet arbitrary Item-ID-to-coordinate math or filled 3D surfaces. "
                << "Use a low-poly mesh for best results. The previous pixel flipbook is available by enabling "
                << "Legacy raster bake in the mod settings.";
        FLAlertLayer::create("3D baseplate rig added!", message.str().c_str(), "NICE")->show();
    }

    void update(float dt) override {
        // Avoid a large rotation jump after the game is paused or the window is minimized.
        dt = std::min(dt, 0.05f);
        m_angleX += dt * 0.83f;
        m_angleY += dt * 1.13f;
        m_angleZ += dt * 0.27f;
        this->redrawCube();
    }

private:
    LevelEditorLayer* m_editorLayer = nullptr;
    CCLayerColor* m_panel = nullptr;
    CCDrawNode* m_cube = nullptr;
    CCSize m_panelSize{500.f, 380.f};
    CCLabelBMFont* m_linesStatus = nullptr;
    CCLabelBMFont* m_styleStatus = nullptr;
    bool m_showLines = false;       // Off by default: avoids triangle-seam noise on dense meshes.
    bool m_shadingOnly = true;      // White material with top-right directional lighting.
    float m_angleX = 0.f;
    float m_angleY = 0.f;
    float m_angleZ = 0.f;


    void redrawCube() {
        if (!m_cube) return;
        m_cube->clear();
        const Mesh& mesh = currentMesh();
        auto rotated = rotateMeshVertices(mesh, m_angleX, m_angleY, m_angleZ);
        std::vector<FaceDraw> faces;
        faces.reserve(mesh.faces.size());

        for (auto const& face : mesh.faces) {
            if (face.indices.size() < 3) continue;
            FaceDraw draw;
            draw.meshFaceIndex = static_cast<size_t>(&face - mesh.faces.data());
            draw.averageDepth = 0.f;
            Vec3 center{0.f, 0.f, 0.f};
            for (int index : face.indices) {
                const auto& p = rotated[index];
                const float distance = std::max(0.5f, 5.f - p.z);
                draw.averageDepth += distance;
                center.x += p.x / static_cast<float>(face.indices.size());
                center.y += p.y / static_cast<float>(face.indices.size());
                center.z += p.z / static_cast<float>(face.indices.size());
            }
            draw.averageDepth /= static_cast<float>(face.indices.size());

            Vec3 normal = normalized(crossProduct(
                subtract(rotated[face.indices[1]], rotated[face.indices[0]]),
                subtract(rotated[face.indices[2]], rotated[face.indices[0]])
            ));
            if (dotProduct(normal, center) < 0.f) normal = {-normal.x, -normal.y, -normal.z};
            const Vec3 smoothNormal = draw.meshFaceIndex < mesh.smoothFaceNormals.size()
                ? rotateCubePoint(mesh.smoothFaceNormals[draw.meshFaceIndex], m_angleX, m_angleY, m_angleZ)
                : normal;
            const ccColor4F base = m_shadingOnly
                ? ccColor4F{0.92f, 0.94f, 0.98f, 1.f}
                : face.baseColor;
            draw.color = shadeFace(base, smoothNormal);

            const float focal = std::min(m_panelSize.width, m_panelSize.height) * 0.88f;
            for (int index : face.indices) {
                const auto& p = rotated[index];
                const float distance = std::max(0.5f, 5.f - p.z);
                draw.points.push_back(CCPointMake(
                    m_panelSize.width * 0.5f + p.x * focal / distance,
                    m_panelSize.height * 0.57f + p.y * focal / distance
                ));
            }
            faces.push_back(std::move(draw));
        }
        std::sort(faces.begin(), faces.end(), [](FaceDraw const& a, FaceDraw const& b) {
            return a.averageDepth > b.averageDepth;
        });

        const ccColor4F edge{0.012f, 0.018f, 0.032f, 1.f};
        for (auto const& face : faces) {
            if (face.points.size() < 3) continue;
            // Fan-triangulate each polygon for fill, but draw only its original
            // boundary so imported polygon faces do not gain fake diagonal lines.
            for (size_t i = 1; i + 1 < face.points.size(); ++i) {
                CCPoint triangle[3] = {face.points[0], face.points[i], face.points[i + 1]};
                m_cube->drawPolygon(triangle, 3, face.color, 0.f, face.color);
            }
            if (m_showLines && face.meshFaceIndex < mesh.outlineEdges.size()) {
                auto const& edgeFlags = mesh.outlineEdges[face.meshFaceIndex];
                for (size_t i = 0; i < face.points.size() && i < edgeFlags.size(); ++i) {
                    if (!edgeFlags[i]) continue;
                    const CCPoint from = face.points[i];
                    const CCPoint to = face.points[(i + 1) % face.points.size()];
                    m_cube->drawSegment(from, to, 1.8f, edge);
                }
            }
        }
    }
};

} // namespace

class $modify(GD3DEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) return false;

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        auto sprite = ButtonSprite::create("3D", "bigFont.fnt", "GJ_button_01.png", 0.62f);
        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(GD3DEditorUI::onOpen3DPreview)
        );
        // Floating editor shortcut. Kept separate from vanilla menus to avoid
        // relying on internal menu child IDs that can change between GD versions.
        // Moved farther right and slightly down to keep clear of the editor's back/menu controls.
        button->setPosition({94.f, win.height - 108.f});
        button->setID("gd3d-editor-preview-button"_spr);
        menu->addChild(button);
        menu->setPosition({0.f, 0.f});
        menu->setID("gd3d-editor-preview-menu"_spr);
        this->addChild(menu, 10000);
        return true;
    }

    void onOpen3DPreview(CCObject*) {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) {
            FLAlertLayer::create("GD 3D Renderer", "Couldn't find the active editor scene.", "OK")->show();
            return;
        }
        if (scene->getChildByID("gd3d-editor-preview"_spr)) return;

        auto preview = CubeEditorPreview::create(this->m_editorLayer);
        if (!preview) {
            FLAlertLayer::create("GD 3D Renderer", "Couldn't initialize the editor preview.", "OK")->show();
            return;
        }
        scene->addChild(preview, 9999);
    }
};
