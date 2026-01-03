#pragma once

#include <obs-module.h>
#include <string.h>

// ========== 從 settings 提取 child_ 開頭的設定 ==========
// 返回的 obs_data_t 需要由調用者釋放
static obs_data_t* extract_child_settings(obs_data_t* settings) {
    obs_data_t* child_settings = obs_data_create();
    obs_data_item_t* item = obs_data_first(settings);
    const char* prefix = "child_";
    const size_t prefix_len = 6;
    
    while (item) {
        const char* key = obs_data_item_get_name(item);
        
        // 只處理 child_ 開頭的屬性
        if (strncmp(key, prefix, prefix_len) == 0) {
            const char* real_key = key + prefix_len;
            enum obs_data_type dtype = obs_data_item_gettype(item);
            
            switch ((int)dtype) {
            case OBS_DATA_STRING:
                obs_data_set_string(child_settings, real_key, obs_data_item_get_string(item));
                break;
            case OBS_DATA_NUMBER:
                if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT) {
                    obs_data_set_int(child_settings, real_key, obs_data_item_get_int(item));
                } else {
                    obs_data_set_double(child_settings, real_key, obs_data_item_get_double(item));
                }
                break;
            case OBS_DATA_BOOLEAN:
                obs_data_set_bool(child_settings, real_key, obs_data_item_get_bool(item));
                break;
            default:
                break;
            }
        }
        obs_data_item_next(&item);
    }
    
    return child_settings;
}
