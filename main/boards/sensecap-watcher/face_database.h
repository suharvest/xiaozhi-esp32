#ifndef FACE_DATABASE_H
#define FACE_DATABASE_H

#include <string>
#include <vector>
#include <mutex>

// Face embedding dimension (MobileFaceNet output)
#define FACE_EMBEDDING_DIM 128
#define FACE_EMBEDDING_SIZE (FACE_EMBEDDING_DIM * sizeof(float))  // 512 bytes
#define FACE_NAME_MAX_LEN 32
#define FACE_MAX_COUNT 20

struct FaceEntry {
    std::string name;
    float embedding[FACE_EMBEDDING_DIM];
};

struct FaceMatchResult {
    bool matched;
    std::string name;
    float similarity;
    int index;  // Index in database, -1 if not matched
};

class FaceDatabase {
public:
    static FaceDatabase& GetInstance();

    // Database operations
    bool AddFace(const std::string& name, const float* embedding);
    bool DeleteFace(const std::string& name);
    bool DeleteFaceByIndex(int index);
    bool RenameFace(const std::string& old_name, const std::string& new_name);
    std::vector<std::string> ListFaces();
    int GetFaceCount();

    // Face matching
    FaceMatchResult Match(const float* embedding, float threshold = 0.6f);

    // Utility
    static float CosineSimilarity(const float* a, const float* b, int dim = FACE_EMBEDDING_DIM);
    static void NormalizeEmbedding(float* embedding, int dim = FACE_EMBEDDING_DIM);

    // Decode hex-encoded UTF-8 names (e.g. "u_e88b8fe7a6be" -> "苏禾")
    static std::string DecodeName(const std::string& name);

private:
    bool DeleteFaceByIndexLocked(int index);

    FaceDatabase();
    ~FaceDatabase() = default;

    // Disable copy
    FaceDatabase(const FaceDatabase&) = delete;
    FaceDatabase& operator=(const FaceDatabase&) = delete;

    // NVS operations
    bool LoadFromNvs();
    bool SaveToNvs();
    bool LoadFaceEntry(int index, FaceEntry& entry);
    bool SaveFaceEntry(int index, const FaceEntry& entry);
    bool DeleteFaceEntry(int index);

    // In-memory cache
    std::vector<FaceEntry> faces_;
    bool loaded_;
    std::mutex mutex_;
};

#endif // FACE_DATABASE_H
