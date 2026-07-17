#include "remote_display_http_server.h"
#include "remote_display.h"
#include "sscma_camera.h"
#include "face_database.h"
#include "face_recognition.h"
#include "settings.h"

#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mbedtls/base64.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char* TAG = "RemoteDisplayHttp";

RemoteDisplayHttpServer* RemoteDisplayHttpServer::instance_ = nullptr;

// Max request body size (256 bytes is plenty for {"ws_url":"ws://..."})
#define MAX_REQ_BODY 256

// ---- /api/face/batch-update limits ----------------------------------------
// The on-device face/NPU model identity. The batch-update full-sync MUST carry
// a matching model_tag, otherwise the device would ingest embeddings from a
// different model (incompatible vector space) and silently mis-match faces.
// MobileFaceNet, 128-D. Wire embeddings are fp16 (256B/face) by default; a
// float32 (512B) legacy path is still accepted. Bump the suffix if the embedder
// (vector space) ever changes — the byte ENCODING is carried separately in the
// `embedding_format` field, not the model_tag.
#define DEVICE_FACE_MODEL_TAG "we2-mfnr6-128-v1"
// Upper bound on how many faces a single batch may carry (matches FACE_MAX_COUNT).
#define FACE_BATCH_MAX_FACES FACE_MAX_COUNT
// Dedicated body cap for the batch handler — MAX_REQ_BODY=256 is far too small.
// fp16 path: 20 faces * (~344 b64 chars for 256B + name + JSON envelope) ≈ 8KB.
// 16KB ceiling keeps margin (incl. the legacy float32 512B/face path at ~16KB)
// while bounding the SPIRAM allocation to prevent OOM heaping.
#define FACE_BATCH_MAX_BODY (16 * 1024)

// Rate limit for /api/face/embed: minimum interval between *accepted* calls.
// Caps hammering of Himax / internal SRAM. Tune as needed; verify-once callers
// never hit this (their spacing is far above 1s).
#define FACE_EMBED_MIN_INTERVAL_US (1000 * 1000)  // 1s

// UDP beacon port and interval
#define BEACON_UDP_PORT 12321
#define BEACON_INTERVAL_US (2 * 1000 * 1000)  // 2 seconds

RemoteDisplayHttpServer::~RemoteDisplayHttpServer() {
    Stop();
}

// ---------- HTTP handlers (static) ----------

esp_err_t RemoteDisplayHttpServer::HandleStartCast(httpd_req_t* req) {
    char buf[MAX_REQ_BODY];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* ws_url = cJSON_GetObjectItem(root, "ws_url");
    if (!ws_url || !cJSON_IsString(ws_url) || strlen(ws_url->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ws_url");
        return ESP_FAIL;
    }

    auto* remote = RemoteDisplay::GetInstance();
    bool ok = false;

    if (remote->IsRunning()) {
        // Already casting — stop first then reconnect
        remote->Stop();
    }

    // Stop beacon before WebSocket connect — RPi already found us
    if (instance_) {
        instance_->StopBeacon();
    }

    ok = remote->Start(ws_url->valuestring, 3000);

    if (ok) {
        // Save to NVS so auto-reconnect works after reboot
        auto config = RemoteDisplay::LoadConfig();
        config.server_url = ws_url->valuestring;
        config.enabled = true;
        RemoteDisplay::SaveConfig(config);
    }

    cJSON_Delete(root);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", ok);
    if (!ok) {
        cJSON_AddStringToObject(resp, "error", "Failed to connect");
    }
    char* resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str);
    free(resp_str);
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t RemoteDisplayHttpServer::HandleStopCast(httpd_req_t* req) {
    auto* remote = RemoteDisplay::GetInstance();
    remote->Stop();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

esp_err_t RemoteDisplayHttpServer::HandleStatus(httpd_req_t* req) {
    auto* remote = RemoteDisplay::GetInstance();
    auto config = RemoteDisplay::LoadConfig();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "casting", remote->IsRunning());
    cJSON_AddStringToObject(resp, "server_url", config.server_url.c_str());

    char* resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp_str);
    free(resp_str);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ---------- Face embedding (on-device, for cloud callers) ----------

esp_err_t RemoteDisplayHttpServer::HandleFaceEmbed(httpd_req_t* req) {
    SscmaCamera* camera = instance_ ? instance_->camera_ : nullptr;
    if (camera == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"camera_unavailable\"}");
        return ESP_OK;
    }

    // State gate: reject during OTA ("upgrading"), while passive greeting is
    // actively running ("greeting_active"), or when Himax is otherwise busy
    // ("busy"). Allowed during a voice conversation (intended use → warm path).
    const char* block = camera->FaceEmbedBlockReason();
    if (block != nullptr) {
        // upgrading / greeting_active are state conflicts (409); busy is transient (503).
        const char* status = (strcmp(block, "busy") == 0)
                                 ? "503 Service Unavailable"
                                 : "409 Conflict";
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        char body[64];
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", block);
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    // Rate limit: cap call frequency. Only *accepted* calls advance the
    // timestamp, so a fast poller is throttled but never permanently starved.
    int64_t now_us = esp_timer_get_time();
    int64_t last_us = instance_->last_face_call_us_.load();
    if (last_us != 0 && (now_us - last_us) < FACE_EMBED_MIN_INTERVAL_US) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"rate_limited\"}");
        return ESP_OK;
    }
    instance_->last_face_call_us_.store(now_us);

    // SRAM instrumentation: this endpoint runs on the (small) httpd worker and
    // drives the deep sscma_client + SPI chain. Log internal-SRAM headroom so we
    // can size the stack / socket budget from real data instead of guessing.
    size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    float embedding[FACE_EMBEDDING_DIM];
    SscmaCamera::SingleShotTiming t;
    bool ok = camera->BenchSingleShotFaceEmbedding(embedding, &t);

    size_t sram_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t sram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    UBaseType_t stack_left = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG,
        "[FaceEmbed] ok=%d total=%ldus SRAM before=%u after=%u min=%u stack_hwm=%u",
        ok, (long)t.total_us, (unsigned)sram_before, (unsigned)sram_after,
        (unsigned)sram_min, (unsigned)stack_left);

    httpd_resp_set_type(req, "application/json");

    if (!ok) {
        // No face detected / timeout — structured error, HTTP 200 (same as the
        // MCP tool) so the caller can deny rather than retry blindly.
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_face_or_timeout\"}");
        return ESP_OK;
    }

    // Encode the 128 float32 LE (512 bytes) as base64 — identical wire format to
    // self.face.capture_embedding so the server side needs zero conversion.
    const size_t raw_len = sizeof(float) * FACE_EMBEDDING_DIM;  // 512
    const size_t b64_buf = ((raw_len + 2) / 3) * 4 + 16;
    uint8_t* b64 = static_cast<uint8_t*>(heap_caps_malloc(b64_buf, MALLOC_CAP_SPIRAM));
    if (b64 == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
        return ESP_OK;
    }
    size_t b64_len = 0;
    int rc = mbedtls_base64_encode(b64, b64_buf, &b64_len,
                                   reinterpret_cast<const uint8_t*>(embedding), raw_len);
    if (rc != 0) {
        heap_caps_free(b64);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"b64_encode_failed\"}");
        return ESP_OK;
    }

    std::string resp;
    resp.reserve(b64_len + 96);
    resp.append("{\"ok\":true,\"format\":\"float32_le_b64\",\"dim\":");
    resp.append(std::to_string(FACE_EMBEDDING_DIM));
    resp.append(",\"score\":");
    resp.append(std::to_string(t.face_score));
    resp.append(",\"embedding_b64\":\"");
    resp.append(reinterpret_cast<const char*>(b64), b64_len);
    resp.append("\"}");
    heap_caps_free(b64);

    httpd_resp_sendstr(req, resp.c_str());
    return ESP_OK;
}

// ---------- Face DB full sync (server library push) ----------

esp_err_t RemoteDisplayHttpServer::HandleFaceBatchUpdate(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");

    // 1) Bounded read into a SPIRAM buffer (MAX_REQ_BODY=256 is far too small).
    int total = req->content_len;
    if (total <= 0 || total > FACE_BATCH_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"body_too_large\"}");
        return ESP_OK;
    }
    char* body = static_cast<char*>(heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
        return ESP_OK;
    }
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) {
            heap_caps_free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"recv_failed\"}");
            return ESP_OK;
        }
        off += r;
    }
    body[total] = '\0';

    cJSON* root = cJSON_Parse(body);
    heap_caps_free(body);
    if (root == nullptr) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid_json\"}");
        return ESP_OK;
    }

    // 2) model_tag validation — reject the WHOLE batch (4xx) on mismatch so we
    //    never mix vector spaces from different models.
    cJSON* mt = cJSON_GetObjectItem(root, "model_tag");
    if (!cJSON_IsString(mt) || strcmp(mt->valuestring, DEVICE_FACE_MODEL_TAG) != 0) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"model_tag_mismatch\"}");
        return ESP_OK;
    }

    // 2b) embedding_format dispatch. The decoded byte width per face depends on
    //     the encoding; we never reinterpret bytes as the wrong width.
    //       "fp16"    -> 256 bytes = 128 IEEE-754 binary16 LE  (current pinned contract)
    //       "float32" -> 512 bytes = 128 float32 LE            (legacy compat)
    //       (absent)  -> "float32" (old clients predate the field)
    //       unknown   -> 409 Conflict
    //     FUTURE int8 extension point: add an `else if "int8"` branch here that
    //     sets expected_bytes = 128 (+ a per-vector scale read elsewhere) and a
    //     FMT_INT8 mode; the per-face loop below would dequantize int8 -> float32
    //     exactly like fp16 does. faces_ / NVS-save paths need no change because
    //     ReplaceAll receives canonical float32 regardless of wire encoding.
    enum EmbFmt { FMT_FP16, FMT_FLOAT32 };
    EmbFmt emb_fmt = FMT_FLOAT32;
    size_t expected = FACE_EMBEDDING_DIM * sizeof(float);  // float32 default = 512
    {
        cJSON* jfmt = cJSON_GetObjectItem(root, "embedding_format");
        const char* fmt = (cJSON_IsString(jfmt) && jfmt->valuestring) ? jfmt->valuestring : "float32";
        if (strcmp(fmt, "fp16") == 0) {
            emb_fmt = FMT_FP16;
            expected = FACE_EMBEDDING_DIM * sizeof(uint16_t);  // 256
        } else if (strcmp(fmt, "float32") == 0) {
            emb_fmt = FMT_FLOAT32;
            expected = FACE_EMBEDDING_DIM * sizeof(float);     // 512
        // } else if (strcmp(fmt, "int8") == 0) { /* FUTURE: 128B + scale -> float32 */ }
        } else {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown_embedding_format\"}");
            return ESP_OK;
        }
    }

    cJSON* faces = cJSON_GetObjectItem(root, "faces");
    if (!cJSON_IsArray(faces)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_faces\"}");
        return ESP_OK;
    }
    int n = cJSON_GetArraySize(faces);
    if (n > FACE_BATCH_MAX_FACES) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"too_many_faces\"}");
        return ESP_OK;
    }

    // 3) Decode each face → FaceEntry list. The decoded byte width is `expected`
    //    (256 fp16 / 512 float32, set by the dispatch above). Either way the
    //    FaceEntry embedding is filled with CANONICAL float32 before ReplaceAll.
    //    Validate name length + embedding width up front so ReplaceAll never has
    //    to truncate/skip.
    std::vector<FaceEntry> entries;
    entries.reserve(n);
    const char* perr = nullptr;
    for (int i = 0; i < n; i++) {
        cJSON* f = cJSON_GetArrayItem(faces, i);
        cJSON* jname = cJSON_GetObjectItem(f, "name");
        cJSON* jsid  = cJSON_GetObjectItem(f, "subject_id");
        cJSON* jemb  = cJSON_GetObjectItem(f, "embedding_b64");
        if (!cJSON_IsString(jname) || !cJSON_IsString(jemb)) { perr = "bad_face_fields"; break; }
        size_t nlen = strlen(jname->valuestring);
        if (nlen == 0 || nlen >= FACE_NAME_MAX_LEN) { perr = "bad_name_len"; break; }

        FaceEntry e;
        e.name = jname->valuestring;
        e.subject_id = cJSON_IsNumber(jsid) ? jsid->valueint : 0;

        // 512B buffer covers the larger (float32) format; fp16 uses the first 256B.
        uint8_t decoded[FACE_EMBEDDING_DIM * sizeof(float)];
        size_t dlen = 0;
        int rc = mbedtls_base64_decode(decoded, sizeof(decoded), &dlen,
                    reinterpret_cast<const unsigned char*>(jemb->valuestring),
                    strlen(jemb->valuestring));
        if (rc != 0 || dlen != expected) { perr = "bad_embedding"; break; }

        if (emb_fmt == FMT_FP16) {
            // 128 binary16 LE -> float32 (byte-wise unpack: alignment-safe).
            for (int j = 0; j < FACE_EMBEDDING_DIM; j++) {
                uint16_t half = (uint16_t)decoded[2 * j] |
                                ((uint16_t)decoded[2 * j + 1] << 8);
                e.embedding[j] = FaceDatabase::HalfToFloat32(half);
            }
        } else {  // FMT_FLOAT32
            memcpy(e.embedding, decoded, FACE_EMBEDDING_DIM * sizeof(float));
        }
        entries.push_back(std::move(e));
    }

    // 3b) Optional device-side face config pushed alongside the library (the
    //     warehouse platform's face config page is the source of truth).
    //     Parsed here while root is alive; applied only after ReplaceAll
    //     succeeds so a rejected batch never half-applies config.
    //       match_threshold   int 0-100 -> NVS face.threshold (applies live)
    //       identify_mode     "local"|"lan" -> NVS face.id_mode
    //       identify_endpoint string       -> NVS face.id_url
    //       identify_token    string       -> NVS face.id_token
    int cfg_threshold = -1;
    std::string cfg_id_mode, cfg_id_url, cfg_id_token, cfg_pull_token;
    bool has_id_cfg = false;
    bool has_pull_token = false;
    {
        cJSON* jt = cJSON_GetObjectItem(root, "match_threshold");
        if (cJSON_IsNumber(jt) && jt->valueint >= 0 && jt->valueint <= 100) {
            cfg_threshold = jt->valueint;
        }
        cJSON* jm = cJSON_GetObjectItem(root, "identify_mode");
        if (cJSON_IsString(jm) && (strcmp(jm->valuestring, "local") == 0 ||
                                   strcmp(jm->valuestring, "lan") == 0)) {
            has_id_cfg = true;
            cfg_id_mode = jm->valuestring;
            cJSON* ju = cJSON_GetObjectItem(root, "identify_endpoint");
            cJSON* jk = cJSON_GetObjectItem(root, "identify_token");
            if (cJSON_IsString(ju) && strlen(ju->valuestring) < 160) cfg_id_url = ju->valuestring;
            if (cJSON_IsString(jk) && strlen(jk->valuestring) < 128) cfg_id_token = jk->valuestring;
        }
        // pull_token (§8d): independent of identify_mode — both local and lan
        // modes must receive it so the backend-direct current-speaker pull
        // authenticates regardless of whether a remote endpoint is configured.
        cJSON* jp = cJSON_GetObjectItem(root, "pull_token");
        if (cJSON_IsString(jp) && strlen(jp->valuestring) < 128) {
            has_pull_token = true;
            cfg_pull_token = jp->valuestring;
        }
    }
    cJSON_Delete(root);

    if (perr != nullptr) {
        httpd_resp_set_status(req, "400 Bad Request");
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", perr);
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    // 4) Apply under an inference pause so the write window can't race the
    //    passive-greeting / Match path. Resume runs on EVERY exit path below
    //    (finally-equivalent), even when ReplaceAll fails.
    SscmaCamera* camera = instance_ ? instance_->camera_ : nullptr;
    if (camera != nullptr) camera->PauseInference();
    bool applied = FaceDatabase::GetInstance().ReplaceAll(entries);
    if (camera != nullptr) camera->ResumeInference();

    if (!applied) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"persist_failed\"}");
        return ESP_OK;
    }

    // 5) Persist pushed config; threshold applies live. A re-push overwrites
    //    NVS wholesale (platform config is authoritative, no merge).
    if (cfg_threshold >= 0 || has_id_cfg || has_pull_token) {
        Settings settings("face", true);
        if (cfg_threshold >= 0) {
            settings.SetInt("threshold", cfg_threshold);
            FaceRecognition::GetInstance().SetMatchThreshold(cfg_threshold / 100.0f);
            ESP_LOGI(TAG, "face threshold <- %d (pushed)", cfg_threshold);
        }
        if (has_id_cfg) {
            settings.SetString("id_mode", cfg_id_mode);
            settings.SetString("id_url", cfg_id_url);
            settings.SetString("id_token", cfg_id_token);
            ESP_LOGI(TAG, "face identify mode <- %s url=%s (pushed)",
                     cfg_id_mode.c_str(), cfg_id_url.c_str());
        }
        if (has_pull_token) {
            // §8d: independent per-device pull_token for current-speaker auth.
            settings.SetString("pull_token", cfg_pull_token);
            ESP_LOGI(TAG, "face pull_token <- (pushed, len=%d)",
                     (int)cfg_pull_token.size());
        }
    }

    std::string resp = "{\"ok\":true,\"applied_count\":";
    resp += std::to_string(entries.size());
    resp += "}";
    httpd_resp_sendstr(req, resp.c_str());
    return ESP_OK;
}

// ---------- Backend-direct identity pull (§8c/§9) ----------

// Build the {valid,name,subject_id,similarity,mode,age_ms} response body, reusing
// the same minimal JSON string escaping as the MCP face tools.
static std::string BuildSpeakerJson(const FaceRecognition::SpeakerIdentity& s,
                                    const std::string& mode, int64_t age_ms) {
    std::string name;  // minimal JSON string escaping
    for (char c : s.name) {
        if (c == '"' || c == '\\') name += '\\';
        name += c;
    }
    char sim[16];
    snprintf(sim, sizeof(sim), "%.4f", s.similarity);
    std::string r = "{\"valid\":";
    r += (s.valid ? "true" : "false");
    r += ",\"name\":\"" + name + "\",\"subject_id\":";
    r += std::to_string(s.subject_id);
    r += ",\"similarity\":";
    r += sim;
    r += ",\"mode\":\"" + mode + "\",\"age_ms\":";
    r += std::to_string(age_ms);
    // conv_seq: bumped each conversation rising edge. Backend keys 仅首次
    // (verify-once-per-conversation) caching on it.
    r += ",\"conv_seq\":";
    r += std::to_string(FaceRecognition::GetInstance().GetConversationSeq());
    r += "}";
    return r;
}

esp_err_t RemoteDisplayHttpServer::HandleFaceCurrentSpeaker(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");

    // --- Auth: X-Face-Token must equal the NVS-pushed pull_token (batch-update
    //     pull_token, §8d). Independent of id_token/identify_token so it works in
    //     both local and lan modes. Fail-closed: missing header, or an
    //     unprovisioned (empty) device token, both -> 401. Prevents same-LAN
    //     hosts from impersonating the backend to pull an identity.
    std::string expected;
    {
        Settings settings("face", false);
        expected = settings.GetString("pull_token", "");
    }
    std::string got;
    size_t tlen = httpd_req_get_hdr_value_len(req, "X-Face-Token");
    if (tlen > 0) {
        got.resize(tlen + 1);
        if (httpd_req_get_hdr_value_str(req, "X-Face-Token", &got[0], tlen + 1) == ESP_OK) {
            got.resize(tlen);
        } else {
            got.clear();
        }
    }
    if (expected.empty() || got != expected) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"valid\":false,\"error\":\"unauthorized\"}");
        return ESP_OK;
    }

    // --- Parse fresh=0|1 from the query string (default 0).
    int fresh = 0;
    {
        char query[64];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char val[8];
            if (httpd_query_key_value(query, "fresh", val, sizeof(val)) == ESP_OK) {
                fresh = atoi(val);
            }
        }
    }

    auto& rec = FaceRecognition::GetInstance();
    std::string mode;
    {
        Settings settings("face", false);
        mode = settings.GetString("id_mode", "local");
    }

    // --- fresh=0: return the in-memory frozen speaker (zero hardware action).
    //     age_ms = now - GetLastMatchTimeUs (age of the frozen identity); -1 if
    //     no match has ever been recorded.
    if (fresh == 0) {
        FaceRecognition::SpeakerIdentity s = rec.GetCurrentSpeaker();
        int64_t last_us = rec.GetLastMatchTimeUs();
        int64_t age_ms = (last_us > 0)
                             ? (esp_timer_get_time() - last_us) / 1000
                             : -1;
        httpd_resp_sendstr(req, BuildSpeakerJson(s, mode, age_ms).c_str());
        return ESP_OK;
    }

    // --- fresh=1: drive one live identify via the shared IdentifyOnce path.
    SscmaCamera* camera = instance_ ? instance_->camera_ : nullptr;
    if (camera == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"valid\":false,\"error\":\"camera_unavailable\"}");
        return ESP_OK;
    }

    // Rate limit: reuse the /api/face/embed cadence (only accepted calls advance
    // the timestamp) so a fast poller is throttled, not permanently starved.
    int64_t now_us = esp_timer_get_time();
    int64_t last_call = instance_->last_face_call_us_.load();
    if (last_call != 0 && (now_us - last_call) < FACE_EMBED_MIN_INTERVAL_US) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, "{\"valid\":false,\"error\":\"rate_limited\"}");
        return ESP_OK;
    }
    instance_->last_face_call_us_.store(now_us);

    // §8e F4: allow_preview=true — the backend-direct pull should show the live
    // photo on screen. The httpd worker no longer touches LVGL directly; the
    // preview upshow is marshaled to the main loop via Application::Schedule
    // inside CaptureImpl/BenchSingleShot, so this is thread-safe.
    SscmaCamera::IdentifyStatus st = SscmaCamera::IdentifyStatus::kOk;
    const char* reason = nullptr;
    FaceRecognition::SpeakerIdentity s =
        camera->IdentifyOnce(/*allow_preview=*/true, &st, &reason);

    if (st == SscmaCamera::IdentifyStatus::kBusy) {
        // F3: op lock held -> transient busy.
        httpd_resp_set_status(req, "503 Service Unavailable");
        char body[64];
        snprintf(body, sizeof(body), "{\"valid\":false,\"error\":\"%s\"}",
                 reason ? reason : "busy");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }
    if (st == SscmaCamera::IdentifyStatus::kBlocked) {
        // Same mapping as /api/face/embed: busy -> 503 (transient), other
        // reasons (upgrading / greeting_active) -> 409 (state conflict).
        const char* status = (reason && strcmp(reason, "busy") == 0)
                                 ? "503 Service Unavailable"
                                 : "409 Conflict";
        httpd_resp_set_status(req, status);
        char body[64];
        snprintf(body, sizeof(body), "{\"valid\":false,\"error\":\"%s\"}",
                 reason ? reason : "conflict");
        httpd_resp_sendstr(req, body);
        return ESP_OK;
    }

    // kOk: identify ran (s.valid may be false = no face / no match). Just-captured,
    // so age_ms = 0.
    httpd_resp_sendstr(req, BuildSpeakerJson(s, mode, 0).c_str());
    return ESP_OK;
}

// ---------- UDP Beacon ----------

void RemoteDisplayHttpServer::StartBeacon(int port) {
    // Idempotent: if a beacon is already running, leave it as-is. Prevents
    // leaking the socket/timer when StartDiscovery() is called on an
    // already-discovering server.
    if (beacon_timer_ != nullptr || beacon_sock_ >= 0) {
        ESP_LOGI(TAG, "Beacon already running, skip");
        return;
    }
    beacon_port_ = port;

    // Create UDP socket
    beacon_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (beacon_sock_ < 0) {
        ESP_LOGE(TAG, "Failed to create beacon socket: errno %d", errno);
        return;
    }

    // Enable broadcast
    int broadcast = 1;
    setsockopt(beacon_sock_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Create periodic timer
    esp_timer_create_args_t timer_args = {
        .callback = BeaconTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "beacon",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &beacon_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create beacon timer: %s", esp_err_to_name(err));
        close(beacon_sock_);
        beacon_sock_ = -1;
        return;
    }

    esp_timer_start_periodic(beacon_timer_, BEACON_INTERVAL_US);

    // Send first beacon immediately
    SendBeacon();

    ESP_LOGI(TAG, "UDP beacon started on port %d (broadcast every 2s)", BEACON_UDP_PORT);
}

void RemoteDisplayHttpServer::SendBeacon() {
    if (beacon_sock_ < 0) return;

    // Get own IP address
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return;
    if (ip_info.ip.addr == 0) return;

    // Format: XZWATCH|name|ip|port|board|version
    char packet[128];
    int len = snprintf(packet, sizeof(packet),
        "XZWATCH|SenseCAP Watcher|" IPSTR "|%d|sensecap-watcher|1.0",
        IP2STR(&ip_info.ip), beacon_port_);

    // Send broadcast
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(BEACON_UDP_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    int ret = sendto(beacon_sock_, packet, len, 0, (struct sockaddr*)&dest, sizeof(dest));

    // Log first 3 beacons and then every 30th (~1 min)
    static int count = 0;
    count++;
    if (count <= 3 || count % 30 == 0) {
        ESP_LOGI(TAG, "Beacon #%d sent (%d bytes, ret=%d): %s", count, len, ret, packet);
    }
}

void RemoteDisplayHttpServer::StopBeacon() {
    if (beacon_timer_) {
        esp_timer_stop(beacon_timer_);
        esp_timer_delete(beacon_timer_);
        beacon_timer_ = nullptr;
    }
    if (beacon_sock_ >= 0) {
        close(beacon_sock_);
        beacon_sock_ = -1;
    }
    ESP_LOGI(TAG, "UDP beacon stopped");
}

void RemoteDisplayHttpServer::BeaconTimerCallback(void* arg) {
    auto* self = static_cast<RemoteDisplayHttpServer*>(arg);
    self->SendBeacon();
}

// ---------- Start / Stop ----------

bool RemoteDisplayHttpServer::Start(int port, bool with_discovery) {
    if (server_) {
        ESP_LOGW(TAG, "HTTP server already running");
        return true;
    }

    instance_ = this;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // SRAM-frugal: internal SRAM is tight (~32KB free observed). Cap concurrent
    // sockets low and enable LRU purge so an idle/stuck client can't pin a slot.
    // Stack: measured on-device — the face endpoint drives the deep sscma_client
    // + SPI chain on this worker and left only stack_hwm=340 bytes free at 3072
    // (peak ~2732B). 4608 gives a comfortable ~1.9KB margin (incl. the success
    // path's base64/JSON build) while costing only ~1.5KB more of the SRAM floor.
    config.stack_size = 4608;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.open_fn = [](httpd_handle_t hd, int sockfd) -> esp_err_t {
        // Reduce LWIP socket buffers to save ~6KB internal SRAM per connection
        int sndbuf = 2920;
        int rcvbuf = 2920;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        return ESP_OK;
    };

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return false;
    }

    // POST /api/start_cast
    httpd_uri_t start_uri = {
        .uri = "/api/start_cast",
        .method = HTTP_POST,
        .handler = HandleStartCast,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &start_uri);

    // POST /api/stop_cast
    httpd_uri_t stop_uri = {
        .uri = "/api/stop_cast",
        .method = HTTP_POST,
        .handler = HandleStopCast,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &stop_uri);

    // GET /api/status
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = HandleStatus,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &status_uri);

    // POST /api/face/embed — on-device single-shot face embedding for cloud
    // callers. LAN-only; auth is expected at the cloud wrapper layer.
    httpd_uri_t face_embed_uri = {
        .uri = "/api/face/embed",
        .method = HTTP_POST,
        .handler = HandleFaceEmbed,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &face_embed_uri);

    // POST /api/face/batch-update — full face-DB sync from the server library.
    // LAN-only; model_tag gated; no token auth this round.
    httpd_uri_t face_batch_uri = {
        .uri = "/api/face/batch-update",
        .method = HTTP_POST,
        .handler = HandleFaceBatchUpdate,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &face_batch_uri);

    // GET /api/face/current-speaker?fresh=0|1 — backend-direct identity pull.
    // Header X-Face-Token gated (must match NVS face.id_token). See §8c/§9.
    httpd_uri_t face_speaker_uri = {
        .uri = "/api/face/current-speaker",
        .method = HTTP_GET,
        .handler = HandleFaceCurrentSpeaker,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &face_speaker_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", port);

    // Start UDP beacon for discovery (only for first-time pairing)
    if (with_discovery) {
        StartBeacon(port);
    }

    return true;
}

void RemoteDisplayHttpServer::Stop() {
    StopBeacon();
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    // Avoid a dangling static pointer: handlers resolve the camera via instance_.
    if (instance_ == this) {
        instance_ = nullptr;
    }
}
