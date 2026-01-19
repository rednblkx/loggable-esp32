if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/loggable.cpp"
        INCLUDE_DIRS "include"
    )
else()
    project(loggable)
    set(CMAKE_CXX_STANDARD 20)
    add_library(loggable STATIC src/loggable.cpp)
    target_include_directories(loggable PUBLIC include)
    target_compile_features(loggable PUBLIC cxx_std_20)
endif()
