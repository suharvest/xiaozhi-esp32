#include "face_database.h"
#include <nvs_flash.h>
#include <esp_log.h>
#include <cstring>
#include <cmath>

static const char* TAG = "FaceDatabase";
static const char* NVS_NAMESPACE = "face_db";

FaceDatabase& FaceDatabase::GetInstance() {
    static FaceDatabase instance;
    return instance;
}

FaceDatabase::FaceDatabase() : loaded_(false) {
    LoadFromNvs();
}

float FaceDatabase::CosineSimilarity(const float* a, const float* b, int dim) {
    float dot = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;

    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = sqrtf(norm_a) * sqrtf(norm_b);
    if (denom < 1e-8f) {
        return 0.0f;
    }

    return dot / denom;
}

void FaceDatabase::NormalizeEmbedding(float* embedding, int dim) {
    float norm = 0.0f;
    for (int i = 0; i < dim; i++) {
        norm += embedding[i] * embedding[i];
    }

    norm = sqrtf(norm);
    if (norm > 1e-8f) {
        for (int i = 0; i < dim; i++) {
            embedding[i] /= norm;
        }
    }
}

bool FaceDatabase::LoadFromNvs() {
    std::lock_guard<std::mutex> lock(mutex_);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Face database not found, starting with empty database");
        loaded_ = true;
        return true;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return false;
    }

    int32_t count = 0;
    err = nvs_get_i32(nvs_handle, "count", &count);
    if (err != ESP_OK) {
        count = 0;
    }

    ESP_LOGI(TAG, "Loading %d faces from NVS", (int)count);

    faces_.clear();
    for (int i = 0; i < count; i++) {
        FaceEntry entry;
        if (LoadFaceEntry(i, entry)) {
            faces_.push_back(entry);
            ESP_LOGI(TAG, "Loaded face %d: %s", i, entry.name.c_str());
        }
    }

    nvs_close(nvs_handle);
    loaded_ = true;

    ESP_LOGI(TAG, "Face database loaded with %zu entries", faces_.size());
    return true;
}

bool FaceDatabase::SaveToNvs() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }

    // Save count
    err = nvs_set_i32(nvs_handle, "count", (int32_t)faces_.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save face count: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Stamp the persistence format version (v2 = per-face subject_id sid_%d key).
    // Best-effort: a missing/old db_ver is treated as legacy v1 on load and is
    // still fully readable (sid_%d simply absent -> sentinel 0).
    nvs_set_i32(nvs_handle, "db_ver", (int32_t)FACE_DB_FORMAT_VERSION);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    return err == ESP_OK;
}

bool FaceDatabase::LoadFaceEntry(int index, FaceEntry& entry) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err != ESP_OK) {
        return false;
    }

    // Load name
    char name_key[16];
    snprintf(name_key, sizeof(name_key), "name_%d", index);

    size_t name_len = FACE_NAME_MAX_LEN;
    char name_buf[FACE_NAME_MAX_LEN];
    err = nvs_get_str(nvs_handle, name_key, name_buf, &name_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    entry.name = std::string(name_buf);

    // Load embedding
    char emb_key[16];
    snprintf(emb_key, sizeof(emb_key), "emb_%d", index);

    size_t emb_len = FACE_EMBEDDING_SIZE;
    err = nvs_get_blob(nvs_handle, emb_key, entry.embedding, &emb_len);
    if (err != ESP_OK || emb_len != FACE_EMBEDDING_SIZE) {
        ESP_LOGE(TAG, "Failed to load embedding for %s: %s", name_key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Load subject_id (v2+). Independent typed key: legacy v1 records simply lack
    // sid_%d, so NVS_NOT_FOUND -> sentinel 0. The embedding blob is strictly
    // length-checked above, so there is no way to misread its tail as a subject_id.
    char sid_key[16];
    snprintf(sid_key, sizeof(sid_key), "sid_%d", index);
    int32_t sid = 0;
    if (nvs_get_i32(nvs_handle, sid_key, &sid) != ESP_OK) {
        sid = 0;
    }
    entry.subject_id = (int)sid;

    nvs_close(nvs_handle);
    return true;
}

bool FaceDatabase::SaveFaceEntry(int index, const FaceEntry& entry) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }

    // Save name
    char name_key[16];
    snprintf(name_key, sizeof(name_key), "name_%d", index);
    err = nvs_set_str(nvs_handle, name_key, entry.name.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save name: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Save embedding
    char emb_key[16];
    snprintf(emb_key, sizeof(emb_key), "emb_%d", index);
    err = nvs_set_blob(nvs_handle, emb_key, entry.embedding, FACE_EMBEDDING_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save embedding: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Save subject_id (v2 format) as an independent i32 key.
    char sid_key[16];
    snprintf(sid_key, sizeof(sid_key), "sid_%d", index);
    err = nvs_set_i32(nvs_handle, sid_key, (int32_t)entry.subject_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save subject_id: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    return err == ESP_OK;
}

bool FaceDatabase::DeleteFaceEntry(int index) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        return false;
    }

    char name_key[16];
    snprintf(name_key, sizeof(name_key), "name_%d", index);
    nvs_erase_key(nvs_handle, name_key);

    char emb_key[16];
    snprintf(emb_key, sizeof(emb_key), "emb_%d", index);
    nvs_erase_key(nvs_handle, emb_key);

    char sid_key[16];
    snprintf(sid_key, sizeof(sid_key), "sid_%d", index);
    nvs_erase_key(nvs_handle, sid_key);

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    return true;
}

bool FaceDatabase::AddFace(const std::string& name, const float* embedding, int subject_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (name.empty() || name.length() >= FACE_NAME_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid name length");
        return false;
    }

    if (faces_.size() >= FACE_MAX_COUNT) {
        ESP_LOGE(TAG, "Face database is full (max %d)", FACE_MAX_COUNT);
        return false;
    }

    // Check for duplicate names
    for (const auto& face : faces_) {
        if (face.name == name) {
            ESP_LOGE(TAG, "Face with name '%s' already exists", name.c_str());
            return false;
        }
    }

    FaceEntry entry;
    entry.name = name;
    entry.subject_id = subject_id;
    memcpy(entry.embedding, embedding, FACE_EMBEDDING_SIZE);

    // Normalize the embedding
    NormalizeEmbedding(entry.embedding);

    int index = (int)faces_.size();

    // Save to NVS first
    if (!SaveFaceEntry(index, entry)) {
        ESP_LOGE(TAG, "Failed to save face entry to NVS");
        return false;
    }

    // Add to memory cache
    faces_.push_back(entry);

    // Update count in NVS
    if (!SaveToNvs()) {
        ESP_LOGE(TAG, "Failed to update face count in NVS");
        // Entry is saved, just count update failed
    }

    ESP_LOGI(TAG, "Added face: %s (subject_id=%d, total: %zu)",
             name.c_str(), subject_id, faces_.size());
    return true;
}

bool FaceDatabase::DeleteFace(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < faces_.size(); i++) {
        if (faces_[i].name == name) {
            return DeleteFaceByIndexLocked((int)i);
        }
    }

    ESP_LOGW(TAG, "Face not found: %s", name.c_str());
    return false;
}

bool FaceDatabase::DeleteFaceByIndex(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DeleteFaceByIndexLocked(index);
}

bool FaceDatabase::DeleteFaceByIndexLocked(int index) {
    if (index < 0 || index >= (int)faces_.size()) {
        return false;
    }

    std::string name = faces_[index].name;

    // Remove from memory
    faces_.erase(faces_.begin() + index);

    // Rewrite all entries to NVS (to maintain compact indexing)
    // First, delete all old entries
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        // Erase all face entries
        for (int i = 0; i <= (int)faces_.size(); i++) {
            char name_key[16];
            snprintf(name_key, sizeof(name_key), "name_%d", i);
            nvs_erase_key(nvs_handle, name_key);

            char emb_key[16];
            snprintf(emb_key, sizeof(emb_key), "emb_%d", i);
            nvs_erase_key(nvs_handle, emb_key);

            char sid_key[16];
            snprintf(sid_key, sizeof(sid_key), "sid_%d", i);
            nvs_erase_key(nvs_handle, sid_key);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    // Rewrite all remaining entries
    for (size_t i = 0; i < faces_.size(); i++) {
        SaveFaceEntry((int)i, faces_[i]);
    }

    SaveToNvs();

    ESP_LOGI(TAG, "Deleted face: %s (remaining: %zu)", name.c_str(), faces_.size());
    return true;
}

bool FaceDatabase::RenameFace(const std::string& old_name, const std::string& new_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (new_name.empty() || new_name.length() >= FACE_NAME_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid new name length");
        return false;
    }

    // Check new name doesn't already exist
    for (const auto& face : faces_) {
        if (face.name == new_name) {
            ESP_LOGE(TAG, "Face with name '%s' already exists", new_name.c_str());
            return false;
        }
    }

    // Find and rename
    for (size_t i = 0; i < faces_.size(); i++) {
        if (faces_[i].name == old_name) {
            faces_[i].name = new_name;
            if (!SaveFaceEntry((int)i, faces_[i])) {
                ESP_LOGE(TAG, "Failed to save renamed entry to NVS");
                faces_[i].name = old_name;  // Rollback
                return false;
            }
            ESP_LOGI(TAG, "Renamed face: %s -> %s", old_name.c_str(), new_name.c_str());
            return true;
        }
    }

    ESP_LOGW(TAG, "Face not found: %s", old_name.c_str());
    return false;
}

std::vector<std::string> FaceDatabase::ListFaces() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> names;
    for (const auto& face : faces_) {
        names.push_back(face.name);
    }
    return names;
}

int FaceDatabase::GetFaceCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return (int)faces_.size();
}

std::vector<FaceEntry> FaceDatabase::GetAllEntries() {
    std::lock_guard<std::mutex> lock(mutex_);
    return faces_;  // copy under lock
}

FaceMatchResult FaceDatabase::Match(const float* embedding, float threshold) {
    std::lock_guard<std::mutex> lock(mutex_);

    FaceMatchResult result;
    result.matched = false;
    result.name = "";
    result.subject_id = 0;
    result.similarity = 0.0f;
    result.index = -1;

    if (faces_.empty()) {
        return result;
    }

    // Normalize input embedding
    float normalized_emb[FACE_EMBEDDING_DIM];
    memcpy(normalized_emb, embedding, FACE_EMBEDDING_SIZE);
    NormalizeEmbedding(normalized_emb);

    float best_similarity = -1.0f;
    int best_index = -1;

    for (size_t i = 0; i < faces_.size(); i++) {
        float sim = CosineSimilarity(normalized_emb, faces_[i].embedding);
        ESP_LOGD(TAG, "Similarity with %s: %.3f", faces_[i].name.c_str(), sim);

        if (sim > best_similarity) {
            best_similarity = sim;
            best_index = (int)i;
        }
    }

    if (best_index >= 0 && best_similarity >= threshold) {
        result.matched = true;
        result.name = faces_[best_index].name;
        result.subject_id = faces_[best_index].subject_id;
        result.similarity = best_similarity;
        result.index = best_index;
        ESP_LOGD(TAG, "Matched: %s (similarity: %.3f)", result.name.c_str(), result.similarity);
    } else {
        ESP_LOGD(TAG, "No match found (best similarity: %.3f, threshold: %.3f)",
                 best_similarity, threshold);
    }

    return result;
}

std::string FaceDatabase::DecodeName(const std::string& name) {
    if (name.size() > 2 && name[0] == 'u' && name[1] == '_') {
        std::string decoded;
        for (size_t i = 2; i + 1 < name.size(); i += 2) {
            char hi = name[i], lo = name[i + 1];
            auto hex_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex_val(hi), l = hex_val(lo);
            if (h < 0 || l < 0) return name;  // invalid hex, return as-is
            decoded += static_cast<char>((h << 4) | l);
        }
        return decoded;
    }
    return name;
}
