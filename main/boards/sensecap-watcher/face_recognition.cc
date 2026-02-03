#include "face_recognition.h"
#include "application.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>

static const char* TAG = "FaceRecognition";

// Registration timeout in microseconds (10 seconds)
#define REGISTRATION_TIMEOUT_US 10000000

// Cooldown duration in microseconds (10 seconds between notifications for same person)
#define DEFAULT_COOLDOWN_US 10000000

// ============== FaceVotingBuffer Implementation ==============

FaceVotingBuffer::FaceVotingBuffer()
    : write_index_(0), valid_count_(0) {
    mutex_ = xSemaphoreCreateMutex();
    Clear();
}

FaceVotingBuffer::~FaceVotingBuffer() {
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

void FaceVotingBuffer::Clear() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < FACE_VOTING_BUFFER_SIZE; i++) {
        buffer_[i].valid = false;
    }
    write_index_ = 0;
    valid_count_ = 0;
    xSemaphoreGive(mutex_);
}

void FaceVotingBuffer::PruneOldEntries(int64_t current_time_us) {
    for (int i = 0; i < FACE_VOTING_BUFFER_SIZE; i++) {
        if (buffer_[i].valid) {
            if (current_time_us - buffer_[i].timestamp_us > FACE_VOTING_WINDOW_US) {
                buffer_[i].valid = false;
                valid_count_--;
            }
        }
    }
}

void FaceVotingBuffer::AddEmbedding(const float* embedding, int64_t timestamp_us) {
    if (timestamp_us == 0) {
        timestamp_us = esp_timer_get_time();
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Prune old entries first
    PruneOldEntries(timestamp_us);

    // Add new entry
    if (buffer_[write_index_].valid) {
        valid_count_--;  // We're overwriting a valid entry
    }

    memcpy(buffer_[write_index_].embedding, embedding, FACE_EMBEDDING_SIZE);
    buffer_[write_index_].timestamp_us = timestamp_us;
    buffer_[write_index_].valid = true;
    valid_count_++;

    write_index_ = (write_index_ + 1) % FACE_VOTING_BUFFER_SIZE;

    xSemaphoreGive(mutex_);

    ESP_LOGD(TAG, "Added embedding to voting buffer (valid: %d)", valid_count_);
}

bool FaceVotingBuffer::GetConsensusEmbedding(float* out_embedding) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    int64_t current_time = esp_timer_get_time();
    PruneOldEntries(current_time);

    if (valid_count_ < FACE_VOTING_MIN_VOTES) {
        xSemaphoreGive(mutex_);
        ESP_LOGD(TAG, "Not enough votes (%d < %d)", valid_count_, FACE_VOTING_MIN_VOTES);
        return false;
    }

    // Calculate average embedding
    memset(out_embedding, 0, FACE_EMBEDDING_SIZE);

    int count = 0;
    for (int i = 0; i < FACE_VOTING_BUFFER_SIZE; i++) {
        if (buffer_[i].valid) {
            for (int j = 0; j < FACE_EMBEDDING_DIM; j++) {
                out_embedding[j] += buffer_[i].embedding[j];
            }
            count++;
        }
    }

    if (count > 0) {
        for (int j = 0; j < FACE_EMBEDDING_DIM; j++) {
            out_embedding[j] /= count;
        }
    }

    // Normalize the result
    FaceDatabase::NormalizeEmbedding(out_embedding);

    xSemaphoreGive(mutex_);

    ESP_LOGI(TAG, "Got consensus embedding from %d votes", count);
    return true;
}

int FaceVotingBuffer::GetVoteCount() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    PruneOldEntries(esp_timer_get_time());
    int count = valid_count_;
    xSemaphoreGive(mutex_);
    return count;
}

// ============== FaceRecognition Implementation ==============

FaceRecognition& FaceRecognition::GetInstance() {
    static FaceRecognition instance;
    return instance;
}

FaceRecognition::FaceRecognition()
    : enabled_(false)
    , registering_(false)
    , registration_start_time_(0)
    , match_threshold_(0.6f)
    , last_notification_time_(0)
    , cooldown_duration_us_(DEFAULT_COOLDOWN_US) {
    last_match_.matched = false;
    last_match_.similarity = 0.0f;
    last_match_.index = -1;
}

void FaceRecognition::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        voting_buffer_.Clear();
        last_notified_name_.clear();
    }
    ESP_LOGI(TAG, "Face recognition %s", enabled ? "enabled" : "disabled");
}

bool FaceRecognition::StartRegistration(const std::string& name) {
    if (name.empty()) {
        ESP_LOGE(TAG, "Cannot start registration with empty name");
        return false;
    }

    // Check if name already exists
    auto& db = FaceDatabase::GetInstance();
    auto faces = db.ListFaces();
    for (const auto& face : faces) {
        if (face == name) {
            ESP_LOGE(TAG, "Face '%s' already exists", name.c_str());
            return false;
        }
    }

    if (db.GetFaceCount() >= FACE_MAX_COUNT) {
        ESP_LOGE(TAG, "Face database is full");
        return false;
    }

    registering_ = true;
    registering_name_ = name;
    registration_start_time_ = esp_timer_get_time();
    voting_buffer_.Clear();

    ESP_LOGI(TAG, "Started face registration for: %s", name.c_str());
    return true;
}

void FaceRecognition::CancelRegistration() {
    if (registering_) {
        ESP_LOGI(TAG, "Cancelled face registration for: %s", registering_name_.c_str());
        registering_ = false;
        registering_name_.clear();
        voting_buffer_.Clear();
    }
}

FaceQualityResult FaceRecognition::CheckFaceQuality(const HimaxFaceData& face_data) {
    FaceQualityResult result;
    result.is_acceptable = false;
    result.quality_score = 0.0f;

    // Check detection confidence
    if (face_data.score < (int)(FACE_MIN_CONFIDENCE * 100)) {
        result.message = "Detection confidence too low";
        return result;
    }

    // Check face size
    int face_size = (face_data.box_w + face_data.box_h) / 2;
    if (face_size < FACE_MIN_BOX_SIZE) {
        result.message = "Face too small, please move closer";
        return result;
    }

    if (face_size > FACE_MAX_BOX_SIZE) {
        result.message = "Face too close, please move back";
        return result;
    }

    // Check quality score (based on pose)
    if (face_data.quality < FACE_MIN_QUALITY) {
        result.message = "Please face the camera directly";
        return result;
    }

    // All checks passed
    result.is_acceptable = true;
    result.quality_score = face_data.quality;
    result.message = "Face quality OK";

    return result;
}

void FaceRecognition::ProcessFaceData(const HimaxFaceData& face_data) {
    if (!face_data.has_embedding) {
        ESP_LOGD(TAG, "No embedding in face data");
        return;
    }

    if (registering_) {
        HandleRegistrationFrame(face_data);
        return;
    }

    if (!enabled_) {
        return;
    }

    // Add to voting buffer
    voting_buffer_.AddEmbedding(face_data.embedding);

    // Try to get consensus embedding
    float consensus_emb[FACE_EMBEDDING_DIM];
    if (voting_buffer_.GetConsensusEmbedding(consensus_emb)) {
        // Match against database
        auto& db = FaceDatabase::GetInstance();
        FaceMatchResult match = db.Match(consensus_emb, match_threshold_);

        if (match.matched) {
            HandleRecognitionResult(match);
        }
    }
}

void FaceRecognition::HandleRegistrationFrame(const HimaxFaceData& face_data) {
    // Check timeout
    int64_t current_time = esp_timer_get_time();
    if (current_time - registration_start_time_ > REGISTRATION_TIMEOUT_US) {
        ESP_LOGW(TAG, "Registration timeout for: %s", registering_name_.c_str());
        CancelRegistration();
        return;
    }

    // Check face quality
    FaceQualityResult quality = CheckFaceQuality(face_data);
    if (!quality.is_acceptable) {
        ESP_LOGD(TAG, "Registration: %s", quality.message.c_str());
        return;
    }

    // Add to voting buffer
    voting_buffer_.AddEmbedding(face_data.embedding);

    // Try to get consensus embedding
    float consensus_emb[FACE_EMBEDDING_DIM];
    if (voting_buffer_.GetConsensusEmbedding(consensus_emb)) {
        // We have a stable embedding, add to database
        auto& db = FaceDatabase::GetInstance();
        if (db.AddFace(registering_name_, consensus_emb)) {
            ESP_LOGI(TAG, "Successfully registered face: %s", registering_name_.c_str());

            // Notify via wake word
            std::string wake_word = "<face>" + registering_name_ + " registered</face>";
            Application::GetInstance().WakeWordInvoke(wake_word);
        } else {
            ESP_LOGE(TAG, "Failed to add face to database: %s", registering_name_.c_str());
        }

        // End registration mode
        registering_ = false;
        registering_name_.clear();
        voting_buffer_.Clear();
    }
}

void FaceRecognition::HandleRecognitionResult(const FaceMatchResult& result) {
    last_match_ = result;

    // Check cooldown for this person
    int64_t current_time = esp_timer_get_time();
    if (last_notified_name_ == result.name &&
        (current_time - last_notification_time_) < cooldown_duration_us_) {
        ESP_LOGD(TAG, "Skipping notification for %s (in cooldown)", result.name.c_str());
        return;
    }

    // Notify via wake word
    ESP_LOGI(TAG, "Recognized: %s (similarity: %.2f)", result.name.c_str(), result.similarity);

    std::string wake_word = "<face>" + result.name + " detected</face>";
    Application::GetInstance().WakeWordInvoke(wake_word);

    // Update cooldown tracking
    last_notification_time_ = current_time;
    last_notified_name_ = result.name;

    // Clear voting buffer after successful recognition
    voting_buffer_.Clear();
}

bool FaceRecognition::IsInCooldown() const {
    int64_t current_time = esp_timer_get_time();
    return (current_time - last_notification_time_) < cooldown_duration_us_;
}

void FaceRecognition::StartCooldown() {
    last_notification_time_ = esp_timer_get_time();
}
