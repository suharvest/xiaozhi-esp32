#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#include "face_database.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mutex>

// Multi-frame voting configuration
#define FACE_VOTING_BUFFER_SIZE 5
#define FACE_VOTING_MIN_VOTES 3
#define FACE_VOTING_WINDOW_US_DEFAULT 2000000  // 2 seconds default (maps to detect_duration_sec)

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

// Multi-frame voting buffer for stable recognition
class FaceVotingBuffer {
public:
    FaceVotingBuffer();
    ~FaceVotingBuffer();

    void AddEmbedding(const float* embedding, int64_t timestamp_us = 0);
    bool GetConsensusEmbedding(float* out_embedding);
    void Clear();
    int GetVoteCount();
    void SetWindow(int64_t window_us);

private:
    struct VoteEntry {
        float embedding[FACE_EMBEDDING_DIM];
        int64_t timestamp_us;
        bool valid;
    };

    VoteEntry buffer_[FACE_VOTING_BUFFER_SIZE];
    int write_index_;
    int valid_count_;
    int64_t window_us_ = FACE_VOTING_WINDOW_US_DEFAULT;
    SemaphoreHandle_t mutex_;

    void PruneOldEntries(int64_t current_time_us);
};

// Face recognition state machine (mirrors object detection)
enum class FaceDetectionState {
    DETECTING,  // Normal: accumulating votes, matching
    COOLDOWN,   // After trigger: waiting for interval + face to leave
};

// Main face recognition controller
class FaceRecognition {
public:
    static FaceRecognition& GetInstance();

    // Face detection mode
    bool IsEnabled() const;
    void SetEnabled(bool enabled);

    // Process face data from Himax (ignored during COOLDOWN)
    void ProcessFaceData(const HimaxFaceData& face_data);

    // Face presence tracking (called from on_event callback every frame)
    void NotifyFacePresent();
    void NotifyNoFace();

    // Get last recognition result
    FaceMatchResult GetLastMatch() const;
    // Monotonic time (esp_timer_get_time, us) when last_match_ was last set.
    // 0 if no match has ever been recorded. Use to gate on freshness.
    int64_t GetLastMatchTimeUs() const;

    // Conversation speaker identity. The camera task freezes who we are talking
    // to when a conversation starts (CaptureCurrentSpeaker) and clears it when it
    // ends (ClearCurrentSpeaker). Queried by the self.conversation.speaker MCP
    // tool so permission-gated commands can check "who is talking".
    struct SpeakerIdentity {
        bool valid = false;       // false = unknown (stranger / no recent face)
        std::string name;         // decoded display name
        int subject_id = 0;       // warehouse subject id; 0 = unknown / not set
        float similarity = 0.0f;  // face match cosine similarity
    };
    // Freeze the speaker from the most recent fresh, confident match (<=15s old).
    void CaptureCurrentSpeaker();
    void ClearCurrentSpeaker();
    SpeakerIdentity GetCurrentSpeaker() const;
    // Monotonic conversation counter, bumped every time CaptureCurrentSpeaker
    // runs (i.e. each new conversation's rising edge). The backend uses it to
    // implement verify-once-per-conversation (仅首次): a verify whose conv_seq
    // matches the last verified one is served from cache (免验), a new conv_seq
    // triggers a fresh device verify. Never 0 after the first conversation.
    uint32_t GetConversationSeq() const;
    // Overwrite the frozen speaker with an on-demand match (self.face.identify),
    // so later tools in the same conversation see the refreshed identity.
    void SetCurrentSpeaker(const SpeakerIdentity& speaker);

    // Deferred notification: stores wake word and delivers when device returns to idle.
    void SetPendingNotification(const std::string& wake_word);
    bool DeliverPendingNotification();

    // Configuration - timing parameters (shared with object detection)
    void SetValidationDuration(int seconds);
    void SetCooldownInterval(int seconds);

    // Configuration - face-specific
    void SetMatchThreshold(float threshold);
    float GetMatchThreshold() const;

    // Familiar DND mode (熟人免打扰)
    void SetFamiliarMode(bool enabled);
    bool IsFamiliarMode() const;

    // Cooldown management (avoid repeated notifications)
    bool IsInCooldown() const;
    void StartCooldown();

private:
    FaceRecognition();
    ~FaceRecognition() = default;

    FaceRecognition(const FaceRecognition&) = delete;
    FaceRecognition& operator=(const FaceRecognition&) = delete;

    void TriggerNotification(const std::string& wake_word);
    void SuppressCurrentFace();
    void HandleRecognitionResult(const FaceMatchResult& result);
    void HandleUnknownPersonDetected(bool is_stranger_alert);

    bool enabled_;
    bool familiar_mode_;

    FaceVotingBuffer voting_buffer_;
    FaceMatchResult last_match_;
    int64_t last_match_time_us_ = 0;
    SpeakerIdentity current_speaker_;  // frozen for the active conversation
    uint32_t conversation_seq_ = 0;    // bumped on each CaptureCurrentSpeaker
    static constexpr int64_t kSpeakerMaxAgeUs = 15LL * 1000000;  // 15s freshness

    float match_threshold_;

    // State machine (mirrors object detection IDLE/VALIDATING/COOLDOWN)
    FaceDetectionState state_ = FaceDetectionState::DETECTING;
    int64_t cooldown_start_time_ = 0;
    int64_t cooldown_duration_us_;
    int64_t last_face_seen_us_ = 0;
    bool need_start_cooldown_ = false;  // Defer cooldown start until face mode resumes
    static constexpr int64_t kFaceGoneDebounceUs = 2000000;  // 2s: face must be gone this long

    std::string last_notified_name_;

    // Deferred notification state (accessed from SSCMA process task + camera task)
    SemaphoreHandle_t notification_mutex_;
    std::string pending_wake_word_;
    bool has_pending_notification_;
    mutable std::mutex state_mutex_;
};

#endif // FACE_RECOGNITION_H
