idf_component_register(SRC_DIRS          "."
                       PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils")

idf_component_get_property(_httpd esp_http_server COMPONENT_LIB)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${_httpd})

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
