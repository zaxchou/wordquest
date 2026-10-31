idf_component_register(
    SRCS "main.c" "wordquest_ui.c" "wq_model.c" "board_display.c" "wq_audio.c"
        "wq_dict.c" "wq_progress.c" "miniz_tinfl.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd esp_codec_dev heap)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")

set_source_files_properties(miniz_tinfl.c PROPERTIES
    COMPILE_OPTIONS "-Wno-error;-Wno-unused-parameter")
