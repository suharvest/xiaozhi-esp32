#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#include "face_database.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Multi-frame voting configuration
#define FACE_VOTING_BUFFER_SIZE 5
#define FACE_VOTING_MIN_VOTES 3
#define FACE_VOTING_WINDOW_US 3000000  // 3 seconds

// Face quality thresholds
#define FACE_MIN_CONFIDENCE 0.7f
#define FACE_MIN_QUALITY 0.6f
#define FACE_MIN_BOX_SIZE 60       // Minimum face box size in pixels
#define FACE_MAX_BOX_SIZE 300      // Maximum face box size (too close)

// Parsed face data from Himax
struct HimaxFaceData {
    int box_x;
    int box_y;
    int box_w;
    int box_h;
    int score;           // Detection confidence (0-100)
    float quality;       // Quality score based on pose angles
    float embedding[FACE_EMBEDDING_DIM];
    bool has_embedding;
};

// Face quality assessment result
struct FaceQualityResult {
    bool is_acceptable;
    std::string message;
    float quality_score;
};

// Multi-frame voting buffer for stable recognition
class FaceVotingBuffer {
public:
    FaceVotingBuffer();
    ~FaceVotingBuffer();

    void AddEmbedding(const float* embedding, int64_t timestamp_us = 0);
    bool GetConsensusEmbedding(float* out_embedding);
    void Clear();
    int GetVoteCount();

private:
    struct VoteEntry {
        float embedding[FACE_EMBEDDING_DIM];
        int64_t timestamp_us;
        bool valid;
    };

    VoteEntry buffer_[FACE_VOTING_BUFFER_SIZE];
    int write_index_;
    int valid_count_;
    SemaphoreHandle_t mutex_;

    void PruneOldEntries(int64_t current_time_us);
};

// Main face recognition controller
class FaceRecognition {
public:
    static FaceRecognition& GetInstance();

    // Face detection mode
    bool IsEnabled() const { return enabled_; }
    void SetEnabled(bool enabled);

    // Registration mode control
    bool StartRegistration(const std::string& name);
    void CancelRegistration();
    bool IsRegistering() const { return registering_; }
    const std::string& GetRegisteringName() const { return registering_name_; }

    // Process face data from Himax
    void ProcessFaceData(const HimaxFaceData& face_data);

    // Check face quality for registration
    FaceQualityResult CheckFaceQuality(const HimaxFaceData& face_data);

    // Get last recognition result
    FaceMatchResult GetLastMatch() const { return last_match_; }

    // Configuration
    void SetMatchThreshold(float threshold) { match_threshold_ = threshold; }
    float GetMatchThreshold() const { return match_threshold_; }

    // Cooldown management (avoid repeated notifications)
    bool IsInCooldown() const;
    void StartCooldown();

private:
    FaceRecognition();
    ~FaceRecognition() = default;

    FaceRecognition(const FaceRecognition&) = delete;
    FaceRecognition& operator=(const FaceRecognition&) = delete;

    void HandleRecognitionResult(const FaceMatchResult& result);
    void HandleRegistrationFrame(const HimaxFaceData& face_data);

    bool enabled_;
    bool registering_;
    std::string registering_name_;
    int64_t registration_start_time_;

    FaceVotingBuffer voting_buffer_;
    FaceMatchResult last_match_;

    float match_threshold_;
    int64_t last_notification_time_;
    int64_t cooldown_duration_us_;

    std::string last_notified_name_;
};

#endif // FACE_RECOGNITION_H
