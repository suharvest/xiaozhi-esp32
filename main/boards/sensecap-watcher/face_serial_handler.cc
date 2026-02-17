#include "face_serial_handler.h"
#include "face_database.h"
#include "sscma_camera.h"
#include "board.h"

#include <esp_log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const char* TAG = "FaceSerial";

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
            .help = "Add face: face_add <name> <f0,f1,...,f127>",
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
        printf("{\"ok\":false,\"error\":\"Usage: face_add <name> <f0,f1,...,f127>\"}\n");
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

    // Parse comma-separated floats
    float embedding[FACE_EMBEDDING_DIM];
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
