if(CONFIG_CAMERA_OV5647)
    if(CONFIG_CAMERA_OV5647_DEFAULT_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${COMPONENT_PATH}/sensors/ov5647/cfg/ov5647_default.json" APPEND)
    elseif(CONFIG_CAMERA_OV5647_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH ${CONFIG_CAMERA_OV5647_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH} APPEND)
    endif()
endif()

if(CONFIG_CAMERA_SC2336)
    if(CONFIG_CAMERA_SC2336_DEFAULT_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${COMPONENT_PATH}/sensors/sc2336/cfg/sc2336_default.json" APPEND)
    elseif(CONFIG_CAMERA_SC2336_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH ${CONFIG_CAMERA_SC2336_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH} APPEND)
    endif()
endif()

if(CONFIG_CAMERA_OV2710)
    if(CONFIG_CAMERA_OV2710_DEFAULT_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${COMPONENT_PATH}/sensors/ov2710/cfg/ov2710_default.json" APPEND)
    elseif(CONFIG_CAMERA_OV2710_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE)
        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH ${CONFIG_CAMERA_OV2710_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH} APPEND)
    endif()
endif()