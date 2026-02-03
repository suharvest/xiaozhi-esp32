#include "sscma_camera.h"
#include "face_database.h"
#include "face_recognition.h"
#include "mcp_server.h"
#include "lvgl_display.h"
#include "lvgl_image.h"
#include "board.h"
#include "system_info.h"
#include "config.h"
#include "settings.h"
#include "remote_display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include "application.h"
#include "sscma_client_commands.h"

#define TAG "SscmaCamera"

#define IMG_JPEG_BUF_SIZE   48 * 1024

static bool __himax_keepalive_check(sscma_client_handle_t client)
{
    esp_err_t ret = ESP_OK;
    sscma_client_reply_t reply = {0};
    int retry = 3;
    while(retry--) {
        ret = sscma_client_request(client, CMD_PREFIX CMD_AT_ID CMD_QUERY CMD_SUFFIX, &reply, true, pdMS_TO_TICKS(2000));
        if (reply.payload != NULL) {
            sscma_client_reply_clear(&reply);
        }
        if( ret != ESP_OK ) {
            ESP_LOGE(TAG, "Himax keepalive check failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            return true;
        }
    }
    return false;
}

SscmaCamera::SscmaCamera(esp_io_expander_handle_t io_exp_handle) {
    sscma_client_io_spi_config_t spi_io_config = {0};
    spi_io_config.sync_gpio_num = BSP_SSCMA_CLIENT_SPI_SYNC;
    spi_io_config.cs_gpio_num = BSP_SSCMA_CLIENT_SPI_CS;
    spi_io_config.pclk_hz = BSP_SSCMA_CLIENT_SPI_CLK;
    spi_io_config.spi_mode = 0;
    spi_io_config.wait_delay = 10; //两个transfer之间至少延时4ms,但当前 FREERTOS_HZ=100, 延时精度只能达到10ms, 
    spi_io_config.user_ctx = NULL;
    spi_io_config.io_expander = io_exp_handle;
    spi_io_config.flags.sync_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new_io_spi_bus((sscma_client_spi_bus_handle_t)BSP_SSCMA_CLIENT_SPI_NUM, &spi_io_config, &sscma_client_io_handle_);

    sscma_client_config_t sscma_client_config = SSCMA_CLIENT_CONFIG_DEFAULT();
    sscma_client_config.event_queue_size = CONFIG_SSCMA_EVENT_QUEUE_SIZE;
    sscma_client_config.tx_buffer_size = CONFIG_SSCMA_TX_BUFFER_SIZE;
    sscma_client_config.rx_buffer_size = CONFIG_SSCMA_RX_BUFFER_SIZE;
    sscma_client_config.process_task_stack = CONFIG_SSCMA_PROCESS_TASK_STACK_SIZE;
    sscma_client_config.process_task_affinity = CONFIG_SSCMA_PROCESS_TASK_AFFINITY;
    sscma_client_config.process_task_priority = CONFIG_SSCMA_PROCESS_TASK_PRIORITY;
    sscma_client_config.monitor_task_stack = CONFIG_SSCMA_MONITOR_TASK_STACK_SIZE;
    sscma_client_config.monitor_task_affinity = CONFIG_SSCMA_MONITOR_TASK_AFFINITY;
    sscma_client_config.monitor_task_priority = CONFIG_SSCMA_MONITOR_TASK_PRIORITY;
    sscma_client_config.reset_gpio_num = BSP_SSCMA_CLIENT_RST;
    sscma_client_config.io_expander = io_exp_handle;
    sscma_client_config.flags.reset_use_expander = BSP_SSCMA_CLIENT_RST_USE_EXPANDER;

    sscma_client_new(sscma_client_io_handle_, &sscma_client_config, &sscma_client_handle_);

    sscma_data_queue_ = xQueueCreate(1, sizeof(SscmaData));

    sscma_client_callback_t callback = {0};

    detection_state = SscmaCamera::IDLE;
    state_start_time = 0;
    need_start_cooldown = false;
    callback.on_event = [](sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
        SscmaCamera* self = static_cast<SscmaCamera*>(user_ctx);
        if (!self) return;
        
        char *img = NULL;
        int img_size = 0;
        int box_count = 0;
        sscma_client_box_t     *boxes = NULL;
        int class_count = 0;
        sscma_client_class_t *classes = NULL;
        int point_count = 0;
        sscma_client_point_t  *points = NULL;
        int model_type = 0;
        int obj_cnt = 0;

        int width = 0, height = 0;
        cJSON *data = cJSON_GetObjectItem(reply->payload, "data");
        if (data != NULL && cJSON_IsObject(data)) {
            cJSON *resolution = cJSON_GetObjectItem(data, "resolution");
            if (data != NULL && cJSON_IsArray(resolution) && cJSON_GetArraySize(resolution) == 2) {
                width = cJSON_GetArrayItem(resolution, 0)->valueint;
                height = cJSON_GetArrayItem(resolution, 1)->valueint;
            }

            // Check for face recognition mode data
            cJSON *mode = cJSON_GetObjectItem(data, "mode");
            if (mode != NULL && cJSON_IsString(mode) && strcmp(mode->valuestring, "face") == 0) {
                // Parse face recognition data
                cJSON *faces = cJSON_GetObjectItem(data, "faces");
                if (faces != NULL && cJSON_IsArray(faces)) {
                    int face_count = cJSON_GetArraySize(faces);
                    ESP_LOGD(TAG, "Received %d faces in face mode", face_count);

                    auto& face_rec = FaceRecognition::GetInstance();

                    for (int i = 0; i < face_count; i++) {
                        cJSON *face = cJSON_GetArrayItem(faces, i);
                        if (face == NULL) continue;

                        HimaxFaceData face_data;
                        memset(&face_data, 0, sizeof(face_data));

                        // Parse bounding box
                        cJSON *box = cJSON_GetObjectItem(face, "box");
                        if (box != NULL && cJSON_IsArray(box) && cJSON_GetArraySize(box) == 4) {
                            face_data.box_x = cJSON_GetArrayItem(box, 0)->valueint;
                            face_data.box_y = cJSON_GetArrayItem(box, 1)->valueint;
                            face_data.box_w = cJSON_GetArrayItem(box, 2)->valueint;
                            face_data.box_h = cJSON_GetArrayItem(box, 3)->valueint;
                        }

                        // Parse score
                        cJSON *score = cJSON_GetObjectItem(face, "score");
                        if (score != NULL && cJSON_IsNumber(score)) {
                            face_data.score = score->valueint;
                        }

                        // Parse quality
                        cJSON *quality = cJSON_GetObjectItem(face, "quality");
                        if (quality != NULL && cJSON_IsNumber(quality)) {
                            face_data.quality = (float)quality->valuedouble;
                        }

                        // Parse embedding
                        cJSON *embedding = cJSON_GetObjectItem(face, "embedding");
                        if (embedding != NULL && cJSON_IsArray(embedding) &&
                            cJSON_GetArraySize(embedding) == FACE_EMBEDDING_DIM) {
                            for (int j = 0; j < FACE_EMBEDDING_DIM; j++) {
                                cJSON *val = cJSON_GetArrayItem(embedding, j);
                                if (val != NULL && cJSON_IsNumber(val)) {
                                    face_data.embedding[j] = (float)val->valuedouble;
                                }
                            }
                            face_data.has_embedding = true;
                        }

                        ESP_LOGI(TAG, "[face %d]: box=[%d,%d,%d,%d], score=%d, quality=%.2f, has_emb=%d",
                                 i, face_data.box_x, face_data.box_y, face_data.box_w, face_data.box_h,
                                 face_data.score, face_data.quality, face_data.has_embedding);

                        // Process face data
                        face_rec.ProcessFaceData(face_data);
                    }
                }
                return;  // Don't process as regular detection data
            }
        }

        switch ((width+height)) {
            case (416+416): 
            {
                bool is_object_detected = false;
                bool is_need_wake = false;
                
                // 定期更新检测配置参数，避免频繁NVS访问
                int64_t cur_tm = esp_timer_get_time();

                // 尝试获取检测框数据（目标检测模型）
                if (sscma_utils_fetch_boxes_from_reply(reply, &boxes, &box_count) == ESP_OK && box_count > 0) {
                    for (int i = 0; i < box_count; i++) {
                        ESP_LOGI(TAG, "[box %d]: x=%d, y=%d, w=%d, h=%d, score=%d, target=%d", i,  \
                                boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h, boxes[i].score, boxes[i].target);
                        if (boxes[i].target == self->detect_target && boxes[i].score > self->detect_threshold) {
                           is_object_detected = true;
                           model_type = 0;
                           obj_cnt++;
                           break;
                        }
                    }
                    free(boxes);
                } else if (sscma_utils_fetch_classes_from_reply(reply, &classes, &class_count) == ESP_OK && class_count > 0) {
                    // 尝试获取分类数据（分类模型）
                    for (int i = 0; i < class_count; i++) {
                        ESP_LOGI(TAG, "[class %d]: target=%d, score=%d", i,
                                classes[i].target, classes[i].score);
                        if (classes[i].target == self->detect_target && classes[i].score > self->detect_threshold) {
                           is_object_detected = true;
                           model_type = 1;
                           obj_cnt++;
                        }
                    }
                    free(classes);
                } else if (sscma_utils_fetch_points_from_reply(reply, &points, &point_count) == ESP_OK && point_count > 0) {
                     // 尝试获取关键点数据（姿态估计模型）
                    for (int i = 0; i < point_count; i++) {
                        ESP_LOGI(TAG, "[point %d]: x=%d, y=%d, z=%d, score=%d, target=%d", i, 
                                points[i].x, points[i].y, points[i].z, points[i].score, points[i].target);
                        if (points[i].target == self->detect_target && points[i].score > self->detect_threshold) {
                           is_object_detected = true;
                           model_type = 2;
                           obj_cnt++;
                        }
                    }
                    free(points);
                }

                // 如果需要开始冷却期，现在开始计时
                if (self->need_start_cooldown) { // 回调暂停，标志保持，等待回调恢复后开始计时
                    self->state_start_time = cur_tm;
                    self->need_start_cooldown = false;
                    ESP_LOGI(TAG, "Starting cooldown timer");
                }
                
                // 状态机驱动的检测逻辑 - 只在人员出现时触发
                switch (self->detection_state) {
                    case SscmaCamera::IDLE:
                        if (is_object_detected) {
                            // 人员出现，开始验证（这是从无到有的转换）
                            self->detection_state = SscmaCamera::VALIDATING;
                            self->state_start_time = cur_tm; // 记录物体出现时间
                            self->last_detected_time = cur_tm; // 初始化最后检测时间
                            ESP_LOGI(TAG, "object appeared, starting validation");
                        }
                        break;
                        
                    case SscmaCamera::VALIDATING:
                        if (is_object_detected) {
                            // 更新最后检测到的时间
                            self->last_detected_time = cur_tm;
                            // 检查是否验证足够时间
                            if ((cur_tm - self->state_start_time) >= (self->detect_duration_sec * 1000000)) {
                                is_need_wake = true;
                            }
                        } else {
                            // 验证期间人员离开，检查去抖动时间
                            if (self->last_detected_time > 0 && 
                                (cur_tm - self->last_detected_time) >= self->detect_debounce_sec * 1000000LL) {
                                // 去抖动时间已过，确认人员已离开，回到空闲
                                self->detection_state = SscmaCamera::IDLE;
                                self->last_detected_time = 0;
                                ESP_LOGI(TAG, "object left during validation (debounced), back to idle");
                            }
                        }
                        break;
                        
                    case SscmaCamera::COOLDOWN:
                        // 冷却期，需要满足两个条件：1)object离开 2)过了15秒
                        if (!is_object_detected && 
                            (cur_tm - self->state_start_time) >= (self->detect_invoke_interval_sec * 1000000LL)) {
                            // object离开且冷却时间到，回到空闲状态
                            self->detection_state = SscmaCamera::IDLE;
                            ESP_LOGI(TAG, "Cooldown complete and object left, back to idle - ready for next appearance");
                        }
                        // 其他情况继续保持冷却状态
                        break;
                }


                if( is_need_wake ) {
                    ESP_LOGI(TAG, "Validation complete, triggering conversation (type=%d, res=%dx%d)", 
                             self->detect_target, width, height);
                    
                    // 触发对话
                    std::string wake_word;
                    if ( model_type  == 0 ) {
                        std::string cached_target_name = "object";
                        if( self->model != NULL && self->model->classes[self->detect_target] != NULL ) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " + cached_target_name + " detected </detect>";
                    } else if ( model_type  == 1 ) {
                        std::string cached_target_name = "object";
                        if( self->model != NULL && self->model->classes[self->detect_target] != NULL ) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " + cached_target_name + " detected </detect>";
                    } else if ( model_type  == 2 ) {
                        std::string cached_target_name = "object";
                        if( self->model != NULL && self->model->classes[self->detect_target] != NULL ) {
                            cached_target_name = self->model->classes[self->detect_target];
                        }
                        wake_word = "<detect>" + std::to_string(obj_cnt) + " " + cached_target_name + " detected </detect>";
                    }
                    printf("wake_word:%s\n", wake_word.c_str());
                    Application::GetInstance().WakeWordInvoke(wake_word);
                    
                    // 进入冷却状态，标记需要开始冷却期；如下变量将在会话结束后被使用，等待回调恢复后开始计时
                    self->detection_state = SscmaCamera::COOLDOWN;
                    self->need_start_cooldown = true;
                }
            }
                break;
            case (640+480):

                if (sscma_utils_fetch_image_from_reply(reply, &img, &img_size) == ESP_OK)
                {
                    ESP_LOGI(TAG, "image_size: %d\n", img_size);
                    // 将数据通过队列发送出去
                    SscmaData data;
                    data.img = (uint8_t*)img;
                    data.len = img_size;

                    // 清空队列，保证只保存最新的数据
                    SscmaData dummy;
                    while (xQueueReceive(self->sscma_data_queue_, &dummy, 0) == pdPASS) {
                        if (dummy.img) {
                            heap_caps_free(dummy.img);
                        }
                    }
                    xQueueSend(self->sscma_data_queue_, &data, 0);
                    // 注意：img 的释放由接收方负责
                }
                break;
            default:
                ESP_LOGI(TAG, "unknown resolution");
                break;
        }
    };
    callback.on_connect = [](sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
        ESP_LOGI(TAG, "SSCMA client connected");
        SscmaCamera* self = static_cast<SscmaCamera*>(user_ctx);
        if (self) {
            self->sscma_restarted_ = true;
        }
    };

    callback.on_log = [](sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx) {
        ESP_LOGI(TAG, "log: %s\n", reply->data);
    };

    sscma_client_register_callback(sscma_client_handle_, &callback, this);

    sscma_client_init(sscma_client_handle_);

    ESP_LOGI(TAG, "SSCMA client initialized");
    // 设置分辨率
    // 3 = 640x480
    if (sscma_client_set_sensor(sscma_client_handle_, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        sscma_client_del(sscma_client_handle_);
        sscma_client_handle_ = NULL;
        return;
    }

    // 获取设备信息
    sscma_client_info_t *info;
    if (sscma_client_get_info(sscma_client_handle_, &info, true) == ESP_OK) {
        ESP_LOGI(TAG, "Device Info - ID: %s, Name: %s", 
            info->id ? info->id : "NULL", 
            info->name ? info->name : "NULL");
    }
    // 初始化JPEG数据的内存
    jpeg_data_.len = 0;
    jpeg_data_.buf = (uint8_t*)heap_caps_malloc(IMG_JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM);;
    if ( jpeg_data_.buf == nullptr ) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG buffer");
        return;
    }

    //初始化JPEG解码
    jpeg_error_t err;
    jpeg_dec_config_t config = { .output_type = JPEG_PIXEL_FORMAT_RGB565_LE, .rotate = JPEG_ROTATE_0D };
    err = jpeg_dec_open(&config, &jpeg_dec_);
    if ( err != JPEG_ERR_OK ) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        return;
    }
    jpeg_io_ = (jpeg_dec_io_t*)heap_caps_malloc(sizeof(jpeg_dec_io_t), MALLOC_CAP_SPIRAM);
    if (!jpeg_io_) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG IO");
        jpeg_dec_close(jpeg_dec_);
        return;
    }
    memset(jpeg_io_, 0, sizeof(jpeg_dec_io_t));

    jpeg_out_ = (jpeg_dec_header_info_t*)heap_caps_aligned_alloc(16, sizeof(jpeg_dec_header_info_t), MALLOC_CAP_SPIRAM);
    if (!jpeg_out_) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output header");
        heap_caps_free(jpeg_io_);
        jpeg_dec_close(jpeg_dec_);
        return;
    }
    memset(jpeg_out_, 0, sizeof(jpeg_dec_header_info_t));

    // 初始化预览图片的内存
    memset(&preview_image_, 0, sizeof(preview_image_));
    preview_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_image_.header.cf = LV_COLOR_FORMAT_RGB565;
    preview_image_.header.flags = LV_IMAGE_FLAGS_ALLOCATED | LV_IMAGE_FLAGS_MODIFIABLE;
    preview_image_.header.w = 640;
    preview_image_.header.h = 480;

    preview_image_.header.stride = preview_image_.header.w * 2;
    preview_image_.data_size = preview_image_.header.w * preview_image_.header.h * 2;
    preview_image_.data =(uint8_t*)jpeg_calloc_align(preview_image_.data_size, 16);
    if (preview_image_.data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for preview image");
        return;
    }

    sscma_client_set_model(sscma_client_handle_, 4);
    model_class_cnt = 0;
    if (sscma_client_get_model(sscma_client_handle_, &model, true) == ESP_OK) {
        printf("ID: %d\n", model->id ? model->id : -1);
        printf("UUID: %s\n", model->uuid ? model->uuid : "N/A");
        printf("Name: %s\n", model->name ? model->name : "N/A");
        printf("Version: %s\n", model->ver ? model->ver : "N/A");
        printf("URL: %s\n", model->url ? model->url : "N/A");
        printf("Checksum: %s\n", model->checksum ? model->checksum : "N/A");
        printf("Classes:\n");
        if (model->classes[0] != NULL)
        {
            for (int i = 0; model->classes[i] != NULL; i++)
            {
                printf("  - %s\n", model->classes[i]);
                model_class_cnt++;
            }
        } else {
            printf("  N/A\n");
        }
    } else {
        printf("get model failed\n");
    }

    ESP_LOGI(TAG, "initialize mcp tools");
    InitializeMcpTools();
    InitializeFaceMcpTools();

    xTaskCreate([](void* arg) {
        auto this_ = (SscmaCamera*)arg;
        bool is_inference = false;
        bool is_face_mode = false;
        int64_t last_keepalive_time = esp_timer_get_time();
        while (true)
        {
            if (this_->sscma_restarted_) {
                ESP_LOGI(TAG, "SSCMA restarted detected");
                this_->sscma_restarted_ = false;
                is_inference = false;
                is_face_mode = false;
            }

            if (esp_timer_get_time() - last_keepalive_time > 10 * 1000000) {
                last_keepalive_time = esp_timer_get_time();
                if (!__himax_keepalive_check(this_->sscma_client_handle_)) {
                    ESP_LOGE(TAG, "restart himax");
                    sscma_client_reset(this_->sscma_client_handle_);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }

            bool is_idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
            auto& face_rec = FaceRecognition::GetInstance();
            int face_count = FaceDatabase::GetInstance().GetFaceCount();

            // Debug: print face recognition status every 10 seconds
            static int64_t last_debug_time = 0;
            if (esp_timer_get_time() - last_debug_time > 10000000) {
                last_debug_time = esp_timer_get_time();
                ESP_LOGI(TAG, "[FaceDebug] face_en=%d, is_idle=%d, face_count=%d, is_face_mode=%d, registering=%d",
                         this_->face_recognition_en_, is_idle, face_count, is_face_mode, face_rec.IsRegistering());
            }

            // Check if face recognition mode should be active
            // Enter face mode when: enabled (even if no faces registered) OR registering
            bool want_face_mode = is_idle && (
                this_->face_recognition_en_ || face_rec.IsRegistering()
            );

            // Check if object detection mode should be active
            bool want_object_mode = this_->inference_en && is_idle && !want_face_mode;

            // Handle face recognition mode
            if (want_face_mode) {
                if (!is_face_mode) {
                    ESP_LOGI(TAG, "Start face recognition mode");
                    sscma_client_break(this_->sscma_client_handle_);

                    // Send AT+FACE=1 to enable face mode on Himax
                    if (this_->SendFaceModeCommand(true)) {
                        this_->camera_mode_ = SscmaCamera::MODE_FACE_RECOGNITION;
                        face_rec.SetEnabled(true);
                        is_face_mode = true;
                        is_inference = false;

                        // Set sensor resolution for face mode (same as object detection)
                        sscma_client_set_sensor(this_->sscma_client_handle_, 1, 1, true);

                        // Wait for Himax to process face mode command
                        vTaskDelay(pdMS_TO_TICKS(200));

                        // Start face inference
                        esp_err_t ret = sscma_client_invoke(this_->sscma_client_handle_, -1, false, true);
                        if (ret != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to start face inference: %d", ret);
                        } else {
                            ESP_LOGI(TAG, "Face inference started");
                        }
                    }
                }
            } else if (is_face_mode) {
                ESP_LOGI(TAG, "Stop face recognition mode");
                sscma_client_break(this_->sscma_client_handle_);

                // Send AT+FACE=0 to disable face mode on Himax
                this_->SendFaceModeCommand(false);
                this_->camera_mode_ = SscmaCamera::MODE_OBJECT_DETECT;
                face_rec.SetEnabled(false);
                is_face_mode = false;
            }

            // Handle object detection mode (only if not in face mode)
            if (want_object_mode && !is_face_mode) {
                if (!is_inference) {
                    ESP_LOGI(TAG, "Start inference (enable=1)");
                    sscma_client_break(this_->sscma_client_handle_);
                    sscma_client_set_model(this_->sscma_client_handle_, 4);
                    sscma_client_set_sensor(this_->sscma_client_handle_, 1, 1, true); // 设置分辨率 416X416
                    sscma_client_invoke(this_->sscma_client_handle_, -1, false, true);
                    is_inference = true;
                }
            } else if (is_inference && !want_object_mode) {
                ESP_LOGI(TAG, "Stop inference (enable=%d state=%d)", this_->inference_en, Application::GetInstance().GetDeviceState());
                is_inference = false;
                sscma_client_break(this_->sscma_client_handle_);
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }, "sscma_camera", 4096, this, 1, nullptr);

}

SscmaCamera::~SscmaCamera() {
    if (preview_image_.data) {
        heap_caps_free((void*)preview_image_.data);
        preview_image_.data = nullptr;
    }
    if (sscma_client_handle_) {
        sscma_client_del(sscma_client_handle_);
    }
    if (sscma_data_queue_) {
        vQueueDelete(sscma_data_queue_);
    }
    if (jpeg_data_.buf) {
        heap_caps_free(jpeg_data_.buf);
        jpeg_data_.buf = nullptr;
    }
    if (jpeg_dec_) {
        jpeg_dec_close(jpeg_dec_);
        jpeg_dec_ = nullptr;
    }
    if (jpeg_io_) {
        heap_caps_free(jpeg_io_);
        jpeg_io_ = nullptr;
    }
    if (jpeg_out_) {
        heap_caps_free(jpeg_out_);
        jpeg_out_ = nullptr;
    }
}

bool SscmaCamera::SendFaceModeCommand(bool enable) {
    sscma_client_reply_t reply = {0};
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+FACE=%d\r\n", enable ? 1 : 0);

    esp_err_t ret = sscma_client_request(sscma_client_handle_, cmd, &reply, true, pdMS_TO_TICKS(2000));
    if (reply.payload != NULL) {
        sscma_client_reply_clear(&reply);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set face mode: %d", ret);
        return false;
    }

    ESP_LOGI(TAG, "Face mode %s", enable ? "enabled" : "disabled");
    return true;
}

void SscmaCamera::InitializeMcpTools() {
    
    Settings settings("model", false);
    detect_threshold = settings.GetInt("threshold", 75);
    detect_invoke_interval_sec = settings.GetInt("interval", 8);
    detect_duration_sec = settings.GetInt("duration", 2);
    detect_target = settings.GetInt("target", 0);
    inference_en = settings.GetInt("enable", 0);

    auto& mcp_server = McpServer::GetInstance();
        // 获取模型参数配置
    mcp_server.AddTool("self.model.param_get",
        "获取当前视觉模型检测的参数配置信息。\n"
        "返回结果包含：\n"
        "  `threshold`: 检测置信度阈值 (0-100)，低于此值的检测结果将被忽略；\n"
        "  `interval`: 触发对话后的冷却时间(秒)，防止频繁打断；\n"
        "  `duration`: 持续检测确认时间(秒)；\n"
        "  `target`: 当前关注的检测目标索引。",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", false);
            int threshold = settings.GetInt("threshold", 75);
            int interval = settings.GetInt("interval", 8);
            int duration = settings.GetInt("duration", 2);
            int target_type = settings.GetInt("target", 0);
            
            std::string result = "{\"threshold\":" + std::to_string(threshold) + 
                            ",\"interval\":" + std::to_string(interval) + 
                            ",\"duration\":" + std::to_string(duration) + 
                            ",\"target_type\":" + std::to_string(target_type) + "}";
            return result;
    });

    
    // 设置模型参数配置
    mcp_server.AddTool("self.model.param_set",
        "配置视觉模型检测参数。当用户希望调整检测灵敏度、频率或特定目标时使用。\n"
        "参数(均为可选，未提供的参数将保持当前设置不变)：\n"
        "  `threshold`: 置信度阈值 (0-100)。提高此值可减少误报，但可能漏检；\n"
        "  `interval`: 冷却时间(秒)。设置对话结束后多久内不再触发检测；\n"
        "  `duration`: 持续检测时间(秒)。\n"
        "  `target`: 设置检测目标的索引 ID。",
        PropertyList({
            Property("threshold", kPropertyTypeInteger, -1, -1, 100),
            Property("interval", kPropertyTypeInteger, -1, -1, 60),
            Property("duration", kPropertyTypeInteger, -1, -1, 60),
            Property("target", kPropertyTypeInteger, -1, -1, this->model_class_cnt > 0 ? this->model_class_cnt - 1 : 255)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", true);
            try {
                const Property& threshold_prop = properties["threshold"];
                int threshold = threshold_prop.value<int>();
                if (threshold != -1) {
                    settings.SetInt("threshold", threshold);
                    this->detect_threshold = threshold;
                    ESP_LOGI(TAG, "Set detection threshold to %d", threshold);
                }
            } catch (const std::runtime_error&) {
                // threshold parameter not provided, skip
            }
            
            try {
                const Property& interval_prop = properties["interval"];
                int interval = interval_prop.value<int>();
                if (interval != -1) {
                    settings.SetInt("interval", interval);
                    this->detect_invoke_interval_sec = interval;
                    ESP_LOGI(TAG, "Set detection interval to %d", interval);
                }
            } catch (const std::runtime_error&) {
                // interval parameter not provided, skip
            }
            
            try {
                const Property& duration_prop = properties["duration"];
                int duration = duration_prop.value<int>();
                if (duration != -1) {
                    settings.SetInt("duration", duration);
                    this->detect_duration_sec = duration;
                }
            } catch (const std::runtime_error&) {
                // duration parameter not provided, skip
            }
            
            try {
                const Property& target_prop = properties["target"];
                int target = target_prop.value<int>();
                if (target != -1) {
                    settings.SetInt("target", target);
                    this->detect_target = target;
                    ESP_LOGI(TAG, "Set detection target to %d", target);
                }
            } catch (const std::runtime_error&) {
                // target_type parameter not provided, skip
            }

            return "{\"status\": \"success\", \"message\": \"Detection configuration updated\"}";
        });

    // 推理开关获取
    mcp_server.AddTool("self.model.enable",
        "控制视觉推理(摄像头检测)功能的开启与关闭，或查询当前状态。\n"
        "当用户指令涉及'开启/关闭推理'、'开始/停止检测'时使用。\n"
        "参数：\n"
        "  `enable`: (可选) 整数。1=开启推理，0=关闭推理。若省略则返回当前开关状态。",
        PropertyList({
            Property("enable", kPropertyTypeInteger, inference_en, 0, 1)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("model", true);
            try {
                const Property& enable_prop = properties["enable"];
                int en = enable_prop.value<int>();
                settings.SetInt("enable", en);
                this->inference_en = en;
                ESP_LOGI(TAG, "Set inference enable to %d", en);
            } catch (const std::runtime_error&) {
                // enable not provided -> treat as query
            }
            // 返回当前配置
            int cur_en = settings.GetInt("enable", this->inference_en);
            return std::string("{\"enable\":") + std::to_string(cur_en) + "}";
        });

    // Face recognition tool
    mcp_server.AddTool("self.camera.face_rec",
        "Perform face recognition.\n"
        "Args:\n"
        "  `question`: Optional question from the user (not used in face recognition).\n"
        "Return:\n"
        "  XML-formatted recognition result: <rec>name, confidence: 0.85</rec> or <rec>error</rec>",
        PropertyList({
            Property("question", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            // Lower the priority to do the camera capture
            TaskPriorityReset priority_reset(1);

            if (!this->Capture()) {
                throw std::runtime_error("Failed to capture photo");
            }

            // Perform face recognition
            return this->FaceRecognition();
        });
}

void SscmaCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool SscmaCamera::Capture() {

    SscmaData data;
    int ret = 0;
    
    if (sscma_client_handle_ == nullptr) {
        ESP_LOGE(TAG, "SSCMA client handle is not initialized");
        return false;
    }

    if (sscma_client_set_sensor(sscma_client_handle_, 1, 3, true)) {
        ESP_LOGE(TAG, "Failed to set sensor");
        return false;
    }
    ESP_LOGI(TAG, "Capturing image...");
    // himax 可能有缓存数据, 只获取最新的照片即可.
    if (sscma_client_sample(sscma_client_handle_, 1) ) {
        ESP_LOGE(TAG, "Failed to capture image from SSCMA client");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500)); // 等待SSCMA客户端处理数据
    if (xQueueReceive(sscma_data_queue_, &data, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to receive JPEG data from SSCMA client");
        return false;
    }

    if (jpeg_data_.buf == nullptr) {
        heap_caps_free(data.img);
        return false;
    }

    ret = mbedtls_base64_decode(jpeg_data_.buf, IMG_JPEG_BUF_SIZE, &jpeg_data_.len, data.img, data.len);
    if (ret != 0 || jpeg_data_.len == 0) {
        ESP_LOGE(TAG, "Failed to decode base64 image data, ret: %d, output_len: %zu", ret, jpeg_data_.len);
        heap_caps_free(data.img);
        return false;
    }
    heap_caps_free(data.img);

    // 发送到远程显示
    auto* remote = RemoteDisplay::GetInstance();
    if (remote->IsRunning()) {
        remote->SendPreviewImage(jpeg_data_.buf, jpeg_data_.len);
    }

    //DECODE JPEG
    if (!jpeg_dec_ || !jpeg_io_ || !jpeg_out_ || !preview_image_.data) {
        return true;
    }
    jpeg_io_->inbuf = jpeg_data_.buf;
    jpeg_io_->inbuf_len = jpeg_data_.len;
    ret = jpeg_dec_parse_header(jpeg_dec_, jpeg_io_, jpeg_out_);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to parse JPEG header, ret: %d", ret);
        return true;
    }
    jpeg_io_->outbuf = (unsigned char*)preview_image_.data;
    int inbuf_consumed = jpeg_io_->inbuf_len - jpeg_io_->inbuf_remain;
    jpeg_io_->inbuf =  jpeg_data_.buf + inbuf_consumed;
    jpeg_io_->inbuf_len = jpeg_io_->inbuf_remain;

    ret = jpeg_dec_process(jpeg_dec_, jpeg_io_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG image, ret: %d", ret);
        return true;
    }

    // 显示预览图片
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        uint16_t w = preview_image_.header.w;
        uint16_t h = preview_image_.header.h;
        size_t image_size = w * h * 2;
        size_t stride = preview_image_.header.w * 2;

        uint8_t* data = (uint8_t*)heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for display image");
            return true;
        }
        memcpy(data, preview_image_.data, image_size);
        
        auto image = std::make_unique<LvglAllocatedImage>(data, image_size, w, h, stride, LV_COLOR_FORMAT_RGB565);
        display->SetPreviewImage(std::move(image));
    }
    return true;
}
bool SscmaCamera::SetHMirror(bool enabled) {
    return false;
}

bool SscmaCamera::SetVFlip(bool enabled) {
    return false;
}

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 * 
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 * 
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return std::string 服务器返回的JSON格式响应字符串
 *         成功时包含AI分析结果，失败时包含错误信息
 *         格式示例：{"success": true, "result": "分析结果"}
 *                  {"success": false, "message": "错误信息"}
 * 
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
std::string SscmaCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return "{\"success\": false, \"message\": \"Image explain URL or token is not set\"}";
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    // 构造multipart/form-data请求体
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";
    
    // 构造question字段
    std::string question_field;
    question_field += "--" + boundary + "\r\n";
    question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
    question_field += "\r\n";
    question_field += question + "\r\n";
    
    // 构造文件字段头部
    std::string file_header;
    file_header += "--" + boundary + "\r\n";
    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
    file_header += "Content-Type: image/jpeg\r\n";
    file_header += "\r\n";
    
    // 构造尾部
    std::string multipart_footer;
    multipart_footer += "\r\n--" + boundary + "--\r\n";

    // 配置HTTP客户端，使用分块传输编码
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        return "{\"success\": false, \"message\": \"Failed to connect to explain URL\"}";
    }
    
    // 第一块：question字段
    http->Write(question_field.c_str(), question_field.size());
    
    // 第二块：文件字段头部
    http->Write(file_header.c_str(), file_header.size());
    
    // 第三块：JPEG数据
    http->Write((const char*)jpeg_data_.buf, jpeg_data_.len);

    // 第四块：multipart尾部
    http->Write(multipart_footer.c_str(), multipart_footer.size());
    
    // 结束块
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        return "{\"success\": false, \"message\": \"Failed to upload photo\"}";
    }

    std::string result = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Explain image size=%d, question=%s\n%s", jpeg_data_.len, question.c_str(), result.c_str());
    return result;
}

std::string SscmaCamera::FaceRecognition() {
    // Check if we have a captured image
    if (jpeg_data_.len == 0) {
        ESP_LOGE(TAG, "No image captured for face recognition");
        return "<rec>No image captured</rec>";
    }

    // Base64 encode the JPEG data
    size_t base64_len = 0;
    mbedtls_base64_encode(NULL, 0, &base64_len, jpeg_data_.buf, jpeg_data_.len);

    // Allocate aligned memory for base64 buffer (following latest memory management pattern)
    uint8_t* base64_buf = (uint8_t*)heap_caps_aligned_alloc(16, base64_len + 1, MALLOC_CAP_SPIRAM);
    if (!base64_buf) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer (%zu bytes)", base64_len + 1);
        return "<rec>Memory allocation failed</rec>";
    }

    if (mbedtls_base64_encode(base64_buf, base64_len + 1, &base64_len,
                               jpeg_data_.buf, jpeg_data_.len) != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed");
        heap_caps_free(base64_buf);
        return "<rec>Base64 encoding failed</rec>";
    }
    base64_buf[base64_len] = '\0';

    // Construct JSON request body
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        heap_caps_free(base64_buf);
        return "<rec>JSON creation failed</rec>";
    }

    cJSON_AddStringToObject(json, "image_base64", (const char*)base64_buf);
    cJSON_AddNumberToObject(json, "confidence_threshold", 0.5);
    char *json_str = cJSON_PrintUnformatted(json);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        heap_caps_free(base64_buf);
        cJSON_Delete(json);
        return "<rec>JSON serialization failed</rec>";
    }

    // Create HTTP client and send request
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);

    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    http->SetContent(std::string(json_str));

    std::string face_rec_url = "http://192.168.10.181:8001/recognize";

    ESP_LOGI(TAG, "Sending face recognition request to %s (image size: %zu, base64 size: %zu)",
             face_rec_url.c_str(), jpeg_data_.len, base64_len);

    if (!http->Open("POST", face_rec_url)) {
        ESP_LOGE(TAG, "Failed to connect to face recognition API");
        heap_caps_free(base64_buf);
        cJSON_Delete(json);
        free(json_str);
        return "<rec>Failed to connect to API</rec>";
    }

    // Check HTTP status code
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Face recognition API error, status code: %d", status_code);
        heap_caps_free(base64_buf);
        cJSON_Delete(json);
        free(json_str);
        http->Close();
        char error_msg[64];
        snprintf(error_msg, sizeof(error_msg), "<rec>API error: %d</rec>", status_code);
        return std::string(error_msg);
    }

    // Read response
    std::string response_str = http->ReadAll();
    http->Close();

    // Clean up request memory
    heap_caps_free(base64_buf);
    cJSON_Delete(json);
    free(json_str);

    // Parse JSON response
    cJSON *response = cJSON_Parse(response_str.c_str());
    if (!response) {
        ESP_LOGE(TAG, "Failed to parse JSON response: %s", response_str.c_str());
        return "<rec>Invalid API response</rec>";
    }

    // Construct XML-formatted return value
    std::string result;
    cJSON *matched = cJSON_GetObjectItem(response, "matched");
    cJSON *name = cJSON_GetObjectItem(response, "name");
    cJSON *confidence = cJSON_GetObjectItem(response, "confidence");

    // Check if we have the expected fields
    if (!cJSON_IsBool(matched) || !cJSON_IsNumber(confidence)) {
        ESP_LOGE(TAG, "Invalid response format - missing matched or confidence field");
        cJSON_Delete(response);
        return "<rec>Invalid API response format</rec>";
    }

    if (cJSON_IsTrue(matched) && cJSON_IsString(name)) {
        // Successfully matched a face
        char output[128];
        snprintf(output, sizeof(output), "<rec>%s, confidence: %.2f</rec>",
                name->valuestring, confidence->valuedouble);
        result = std::string(output);
        ESP_LOGI(TAG, "Face recognized: %s (confidence: %.2f)",
                name->valuestring, confidence->valuedouble);
    } else {
        // No match found (confidence below threshold)
        char output[128];
        snprintf(output, sizeof(output), "<rec>No face detected</rec>");
        result = std::string(output);
        ESP_LOGI(TAG, "Face not recognized (confidence: %.2f)", confidence->valuedouble);
    }

    // Clean up response memory
    cJSON_Delete(response);

    return result;
}

bool SscmaCamera::SetCameraMode(CameraMode mode) {
    if (mode == camera_mode_) {
        return true;
    }

    ESP_LOGI(TAG, "Switching camera mode to %s",
             mode == MODE_OBJECT_DETECT ? "OBJECT_DETECT" : "FACE_RECOGNITION");

    // Stop current inference
    sscma_client_break(sscma_client_handle_);

    if (mode == MODE_FACE_RECOGNITION) {
        // Switch to face recognition mode
        if (!SendFaceModeCommand(true)) {
            return false;
        }
        camera_mode_ = MODE_FACE_RECOGNITION;
    } else {
        // Switch back to object detection mode
        SendFaceModeCommand(false);
        camera_mode_ = MODE_OBJECT_DETECT;
        sscma_client_set_model(sscma_client_handle_, 4);
    }

    return true;
}

void SscmaCamera::InitializeFaceMcpTools() {
    auto& mcp_server = McpServer::GetInstance();
    auto& face_db = FaceDatabase::GetInstance();
    auto& face_rec = FaceRecognition::GetInstance();

    // Load face recognition settings
    Settings settings("face", false);
    face_recognition_en_ = settings.GetInt("enable", 0);
    float threshold = (float)settings.GetInt("threshold", 60) / 100.0f;
    face_rec.SetMatchThreshold(threshold);

    // Tool: Register a face
    mcp_server.AddTool("self.face.register",
        "录入一张新的人脸到本地数据库。\n"
        "使用场景：当用户说'记住我的脸'、'录入人脸'、'把我的脸存起来叫xxx'时调用。\n"
        "参数：\n"
        "  `name`: 要录入的人脸名称（必填，最长31个字符）\n"
        "返回：录入成功/失败信息",
        PropertyList({
            Property("name", kPropertyTypeString)
        }),
        [this, &face_rec](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();

            if (name.empty()) {
                return std::string("{\"success\": false, \"message\": \"请提供要录入的名字\"}");
            }

            if (name.length() >= FACE_NAME_MAX_LEN) {
                return std::string("{\"success\": false, \"message\": \"名字太长，请使用较短的名字\"}");
            }

            // Start registration mode
            if (!face_rec.StartRegistration(name)) {
                auto& db = FaceDatabase::GetInstance();
                auto faces = db.ListFaces();
                for (const auto& face : faces) {
                    if (face == name) {
                        return std::string("{\"success\": false, \"message\": \"名字 '" + name + "' 已存在\"}");
                    }
                }
                if (db.GetFaceCount() >= FACE_MAX_COUNT) {
                    return std::string("{\"success\": false, \"message\": \"人脸数据库已满，最多" +
                                       std::to_string(FACE_MAX_COUNT) + "张\"}");
                }
                return std::string("{\"success\": false, \"message\": \"无法开始录入\"}");
            }

            // Switch to face mode and capture
            SetCameraMode(MODE_FACE_RECOGNITION);

            // For now, return a message that registration has started
            // The actual registration will be completed when face data is received
            return std::string("{\"success\": true, \"message\": \"请正对摄像头，开始录入人脸: " + name + "\"}");
        });

    // Tool: Delete a face
    mcp_server.AddTool("self.face.delete",
        "从本地数据库删除一张已录入的人脸。\n"
        "使用场景：当用户说'删除人脸xxx'、'忘记xxx的脸'时调用。\n"
        "参数：\n"
        "  `name`: 要删除的人脸名称（必填）\n"
        "返回：删除成功/失败信息",
        PropertyList({
            Property("name", kPropertyTypeString)
        }),
        [&face_db](const PropertyList& properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();

            if (name.empty()) {
                return std::string("{\"success\": false, \"message\": \"请提供要删除的名字\"}");
            }

            if (face_db.DeleteFace(name)) {
                return std::string("{\"success\": true, \"message\": \"已删除人脸: " + name + "\"}");
            } else {
                return std::string("{\"success\": false, \"message\": \"未找到名为 '" + name + "' 的人脸\"}");
            }
        });

    // Tool: List all faces
    mcp_server.AddTool("self.face.list",
        "列出本地数据库中所有已录入的人脸。\n"
        "使用场景：当用户问'有哪些人脸'、'你认识谁'时调用。\n"
        "返回：已录入人脸的名称列表",
        PropertyList(),
        [&face_db](const PropertyList& properties) -> ReturnValue {
            auto faces = face_db.ListFaces();

            cJSON* result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "count", (int)faces.size());

            cJSON* names = cJSON_CreateArray();
            for (const auto& name : faces) {
                cJSON_AddItemToArray(names, cJSON_CreateString(name.c_str()));
            }
            cJSON_AddItemToObject(result, "faces", names);

            return result;
        });

    // Tool: Enable/disable face recognition
    mcp_server.AddTool("self.face.enable",
        "开启或关闭待命时的人脸识别功能。\n"
        "使用场景：当用户说'打开人脸识别'、'关闭人脸识别'时调用。\n"
        "参数：\n"
        "  `enable`: (可选) 1=开启，0=关闭。省略则返回当前状态。\n"
        "返回：当前开关状态",
        PropertyList({
            Property("enable", kPropertyTypeInteger, face_recognition_en_, 0, 1)
        }),
        [this, &face_rec](const PropertyList& properties) -> ReturnValue {
            Settings settings("face", true);
            try {
                const Property& enable_prop = properties["enable"];
                int en = enable_prop.value<int>();
                settings.SetInt("enable", en);
                this->face_recognition_en_ = en;
                face_rec.SetEnabled(en != 0);
                ESP_LOGI(TAG, "Set face recognition enable to %d", en);
            } catch (const std::runtime_error&) {
                // enable not provided -> treat as query
            }
            int cur_en = settings.GetInt("enable", this->face_recognition_en_);
            return std::string("{\"enable\":" + std::to_string(cur_en) + "}");
        });

    // Tool: Set face recognition threshold
    mcp_server.AddTool("self.face.threshold",
        "设置人脸识别的置信度阈值。\n"
        "参数：\n"
        "  `threshold`: 置信度阈值 (0-100)，越高越严格。默认60。\n"
        "返回：当前阈值设置",
        PropertyList({
            Property("threshold", kPropertyTypeInteger, 60, 0, 100)
        }),
        [&face_rec](const PropertyList& properties) -> ReturnValue {
            Settings settings("face", true);
            try {
                const Property& threshold_prop = properties["threshold"];
                int threshold = threshold_prop.value<int>();
                settings.SetInt("threshold", threshold);
                face_rec.SetMatchThreshold((float)threshold / 100.0f);
                ESP_LOGI(TAG, "Set face recognition threshold to %d%%", threshold);
            } catch (const std::runtime_error&) {
                // threshold not provided -> treat as query
            }
            int cur_threshold = settings.GetInt("threshold", 60);
            return std::string("{\"threshold\":" + std::to_string(cur_threshold) + "}");
        });

    ESP_LOGI(TAG, "Face recognition MCP tools initialized");
}
