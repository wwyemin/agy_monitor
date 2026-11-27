idf_component_register(SRCS "app_agy_ui.c" "app_agy_proto.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json")
