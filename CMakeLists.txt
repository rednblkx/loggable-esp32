idf_component_register(SRCS "src/loggable.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos log driver
                       PRIV_REQUIRES esp_timer)