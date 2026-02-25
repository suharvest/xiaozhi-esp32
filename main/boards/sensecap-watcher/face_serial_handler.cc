#include "face_serial_handler.h"
#include "face_database.h"
#include "sscma_camera.h"
#include "board.h"

#include <esp_log.h>
#include <mbedtls/base64.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

static const char* TAG = "FaceSerial";

/**
 * Convert IEEE 754 half-precision (float16) to single-precision (float32).
 * Pure bit manipulation, no hardware FP16 support needed.
 */
static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t f;

    if (exp == 0) {
        if (mant == 0) {
            f = sign;  // ±0
        } else {
            // Denormalized: convert to normalized float32
            exp = 127 - 14;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FF;
            f = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000 | (mant << 13);  // Inf/NaN
    } else {
        f = sign | ((exp + 112) << 23) | (mant << 13);  // Normal
    }

    float result;
    memcpy(&result, &f, sizeof(result));
    return result;
}

FaceSerialHandler& FaceSerialHandler::GetInstance() {
    static FaceSerialHandler instance;
    return instance;
}

void FaceSerialHandler::RegisterCommands() {
    const esp_console_cmd_t cmds[] = {
        {
            .command = "face_list",
            .help = "List all registered faces",
            .hint = nullptr,
            .func = CmdList,
            .argtable = nullptr,
            .context = nullptr,
        },
        {
            .command = "face_add",
            .help = "Add face: face_add <name> <csv_floats|base64_fp16>",
            .hint = "<name> <embedding>",
            .func = CmdAdd,
            .argtable = nullptr,
            .context = nullptr,
        },
        {
            .command = "face_delete",
            .help = "Delete face: face_delete <name>",
            .hint = "<name>",
            .func = CmdDelete,
            .argtable = nullptr,
            .context = nullptr,
        },
        {
            .command = "face_rename",
            .help = "Rename face: face_rename <old_name> <new_name>",
            .hint = "<old_name> <new_name>",
            .func = CmdRename,
            .argtable = nullptr,
            .context = nullptr,
        },
        {
            .command = "inference_pause",
            .help = "Pause SPI inference so UART enrollment can use Himax",
            .hint = nullptr,
            .func = CmdInferencePause,
            .argtable = nullptr,
            .context = nullptr,
        },
        {
            .command = "inference_resume",
            .help = "Resume SPI inference after enrollment",
            .hint = nullptr,
            .func = CmdInferenceResume,
            .argtable = nullptr,
            .context = nullptr,
        },
    };

    for (const auto& cmd : cmds) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    ESP_LOGI(TAG, "Face CRUD commands registered");
}

int FaceSerialHandler::CmdList(int argc, char** argv) {
    auto& db = FaceDatabase::GetInstance();
    auto faces = db.ListFaces();

    // Build JSON response
    printf("{\"ok\":true,\"faces\":[");
    for (size_t i = 0; i < faces.size(); i++) {
        if (i > 0) printf(",");
        printf("{\"name\":\"%s\",\"index\":%d}", faces[i].c_str(), (int)i);
    }
    printf("],\"count\":%d,\"max\":%d}\n", (int)faces.size(), FACE_MAX_COUNT);
    return 0;
}

int FaceSerialHandler::CmdAdd(int argc, char** argv) {
    if (argc < 3) {
        printf("{\"ok\":false,\"error\":\"Usage: face_add <name> <csv_floats|base64_fp16>\"}\n");
        return 1;
    }

    const char* name = argv[1];
    const char* emb_str = argv[2];
    auto& db = FaceDatabase::GetInstance();

    // Check duplicate name
    auto faces = db.ListFaces();
    for (const auto& face : faces) {
        if (face == name) {
            printf("{\"ok\":false,\"error\":\"Name already exists\"}\n");
            return 1;
        }
    }

    if (db.GetFaceCount() >= FACE_MAX_COUNT) {
        printf("{\"ok\":false,\"error\":\"Database full\"}\n");
        return 1;
    }

    float embedding[FACE_EMBEDDING_DIM];

    // Detect format: comma present → legacy CSV, otherwise → base64(float16)
    if (strchr(emb_str, ',') != nullptr) {
        // Legacy CSV: parse comma-separated float32 values
        int count = 0;
        const char* p = emb_str;
        while (count < FACE_EMBEDDING_DIM && *p) {
            char* end;
            embedding[count] = strtof(p, &end);
            if (end == p) {
                printf("{\"ok\":false,\"error\":\"Invalid float at position %d\"}\n", count);
                return 1;
            }
            count++;
            p = end;
            if (*p == ',') p++;
        }

        if (count != FACE_EMBEDDING_DIM) {
            printf("{\"ok\":false,\"error\":\"Expected %d floats, got %d\"}\n", FACE_EMBEDDING_DIM, count);
            return 1;
        }
    } else {
        // Base64-encoded float16: decode → 256 bytes → 128 half-floats
        const size_t expected_bytes = FACE_EMBEDDING_DIM * sizeof(uint16_t);  // 256
        uint8_t decoded[256];
        size_t decoded_len = 0;

        int ret = mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len,
                                        (const unsigned char*)emb_str, strlen(emb_str));
        if (ret != 0) {
            printf("{\"ok\":false,\"error\":\"Base64 decode failed (ret=%d)\"}\n", ret);
            return 1;
        }

        if (decoded_len != expected_bytes) {
            printf("{\"ok\":false,\"error\":\"Expected %d bytes, got %d\"}\n",
                   (int)expected_bytes, (int)decoded_len);
            return 1;
        }

        // Convert float16 → float32
        const uint16_t* fp16 = (const uint16_t*)decoded;
        for (int i = 0; i < FACE_EMBEDDING_DIM; i++) {
            embedding[i] = half_to_float(fp16[i]);
        }
    }

    if (db.AddFace(name, embedding)) {
        printf("{\"ok\":true}\n");
    } else {
        printf("{\"ok\":false,\"error\":\"Failed to add face\"}\n");
    }
    return 0;
}

int FaceSerialHandler::CmdDelete(int argc, char** argv) {
    if (argc < 2) {
        printf("{\"ok\":false,\"error\":\"Usage: face_delete <name>\"}\n");
        return 1;
    }

    auto& db = FaceDatabase::GetInstance();
    if (db.DeleteFace(argv[1])) {
        printf("{\"ok\":true}\n");
    } else {
        printf("{\"ok\":false,\"error\":\"Name not found\"}\n");
    }
    return 0;
}

int FaceSerialHandler::CmdRename(int argc, char** argv) {
    if (argc < 3) {
        printf("{\"ok\":false,\"error\":\"Usage: face_rename <old_name> <new_name>\"}\n");
        return 1;
    }

    const char* old_name = argv[1];
    const char* new_name = argv[2];
    auto& db = FaceDatabase::GetInstance();

    if (db.RenameFace(old_name, new_name)) {
        printf("{\"ok\":true}\n");
    } else {
        // Determine specific error
        auto faces = db.ListFaces();
        bool old_found = false;
        for (const auto& face : faces) {
            if (face == old_name) old_found = true;
            if (face == new_name) {
                printf("{\"ok\":false,\"error\":\"New name already exists\"}\n");
                return 1;
            }
        }
        if (!old_found) {
            printf("{\"ok\":false,\"error\":\"Name not found\"}\n");
        } else {
            printf("{\"ok\":false,\"error\":\"Rename failed\"}\n");
        }
    }
    return 0;
}

int FaceSerialHandler::CmdInferencePause(int argc, char** argv) {
    auto* camera = static_cast<SscmaCamera*>(Board::GetInstance().GetCamera());
    if (!camera) {
        printf("{\"ok\":false,\"error\":\"Camera not available\"}\n");
        return 1;
    }
    camera->PauseInference();
    printf("{\"ok\":true}\n");
    return 0;
}

int FaceSerialHandler::CmdInferenceResume(int argc, char** argv) {
    auto* camera = static_cast<SscmaCamera*>(Board::GetInstance().GetCamera());
    if (!camera) {
        printf("{\"ok\":false,\"error\":\"Camera not available\"}\n");
        return 1;
    }
    camera->ResumeInference();
    printf("{\"ok\":true}\n");
    return 0;
}
