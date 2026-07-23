set(COMMON_DIR "../../../common")

idf_component_register(
    SRC_DIRS
        "."
        "${COMMON_DIR}/adsb"
        "${COMMON_DIR}/adsb/nasa_cpr"
        "${COMMON_DIR}/adsb/nasa_cpr/airborne"
        "${COMMON_DIR}/adsb/nasa_cpr/coarse"
        "${COMMON_DIR}/adsb/nasa_cpr/surface"
        "${COMMON_DIR}/adsb/nasa_cpr/intent"
        # "${COMMON_DIR}/comms" # Currently no src files in here.
        "${COMMON_DIR}/coprocessor"
        "${COMMON_DIR}/utils"
        "${COMMON_DIR}/utils/crc"
        "${COMMON_DIR}/utils/fec"
        "${COMMON_DIR}/utils/geo"
        "${COMMON_DIR}/comms"
        "${COMMON_DIR}/comms/beast"
        "${COMMON_DIR}/comms/gdl90"
        "${COMMON_DIR}/comms/json"
        "${COMMON_DIR}/comms/mavlink"
        "${COMMON_DIR}/comms/raw"
        "${COMMON_DIR}/settings"
        "comms"
        "peripherals/pico"
        "peripherals/display"
        "server"
        "target_test"
    INCLUDE_DIRS
        "."
        "${COMMON_DIR}"
        "${COMMON_DIR}/adsb"
        "${COMMON_DIR}/adsb/nasa_cpr"
        "${COMMON_DIR}/comms"
        "${COMMON_DIR}/comms/aircraftjson"
        "${COMMON_DIR}/comms/beast"
        "${COMMON_DIR}/comms/csbee"
        "${COMMON_DIR}/comms/gdl90"
        "${COMMON_DIR}/comms/json"
        "${COMMON_DIR}/comms/mavlink"
        "${COMMON_DIR}/comms/raw"
        "${COMMON_DIR}/coprocessor"
        "${COMMON_DIR}/utils"
        "${COMMON_DIR}/utils/crc"
        "${COMMON_DIR}/utils/fec"
        "${COMMON_DIR}/utils/geo"
        "${COMMON_DIR}/settings"
        "comms"
        "peripherals/pico"
        "peripherals/display"
        "server"
        "target_test"
    EMBED_TXTFILES
        "server/web/index.html"
        "server/web/style.css"
        "server/web/adsbee.js"
        "server/web/favicon.png"
)
# LovyanGFX (GC9A01 display) is vendored as a local component at esp/components/LovyanGFX
# (git submodule, pulled by `just setup`). ESP-IDF auto-discovers it, and `main` implicitly
# depends on all components, so no explicit REQUIRES is needed here. It is only referenced from
# the display sources, which are compiled out unless WITH_DISPLAY; the linker's --gc-sections
# drops it from non-display images.

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-packed-bitfield-compat)

message("PROJECT_SOURCE_DIR: ${PROJECT_SOURCE_DIR}")