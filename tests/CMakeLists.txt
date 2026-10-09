add_executable(qtzpl_tests test_qtzpl.cpp)
target_link_libraries(qtzpl_tests PRIVATE QtZpl::QtZpl Qt6::Test)
qt_add_resources(qtzpl_tests "qtzpl_test_goldens"
  PREFIX "/qtzpl/tests/golden"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden"
  FILES
    golden/code128-extended/code128-font-a-caption.zpl
    golden/code128-extended/code128-font-a-caption-labelary-bitonal.png
    golden/code128-gs1-n.zpl
    golden/code128-gs1-n-labelary-bitonal.png
    golden/code128-gs1-b.zpl
    golden/code128-gs1-b-labelary-bitonal.png
    golden/font0-block-b.zpl
    golden/font0-block-b-labelary-bitonal.png
    golden/font0-h26-w33.zpl
    golden/font0-h26-w33-labelary-bitonal.png
    golden/font0-h27-w34.zpl
    golden/font0-h27-w34-labelary-bitonal.png
    golden/font0-h25-w38.zpl
    golden/font0-h25-w38-labelary-bitonal.png
    golden/font0-h40-w40.zpl
    golden/font0-h40-w40-labelary-bitonal.png
    golden/font0-h70-w251.zpl
    golden/font0-h70-w251-labelary-bitonal.png
    golden/font0-h16-w9.zpl
    golden/font0-h16-w9-labelary-bitonal.png
    golden/font0-h14-w22.zpl
    golden/font0-h14-w22-labelary-bitonal.png
    golden/code128-caption-after-field.zpl
    golden/code128-caption-after-field-labelary-bitonal.png
    golden/font0.zpl
    golden/font0-labelary-bitonal.png
    golden/ocr-b.zpl
    golden/ocr-b-labelary-bitonal.png
    golden/datamatrix.zpl
    golden/datamatrix-labelary-bitonal.png
    golden/datamatrix-rotation-n.zpl
    golden/datamatrix-rotation-n-labelary-bitonal.png
    golden/datamatrix-rotation-r.zpl
    golden/datamatrix-rotation-r-labelary-bitonal.png
    golden/datamatrix-rotation-i.zpl
    golden/datamatrix-rotation-i-labelary-bitonal.png
    golden/datamatrix-rotation-b.zpl
    golden/datamatrix-rotation-b-labelary-bitonal.png
    golden/ean13.zpl
    golden/ean13-labelary-bitonal.png
    golden/ean-module-1.zpl
    golden/ean-module-1-labelary-bitonal.png
    golden/ean-module-2.zpl
    golden/ean-module-2-labelary-bitonal.png
    golden/ean-module-3.zpl
    golden/ean-module-3-labelary-bitonal.png
    golden/ean-module-4.zpl
    golden/ean-module-4-labelary-bitonal.png
    golden/ean-module-5.zpl
    golden/ean-module-5-labelary-bitonal.png
    golden/ean-above.zpl
    golden/ean-above-labelary-bitonal.png
    golden/ean-rotation-n.zpl
    golden/ean-rotation-n-labelary-bitonal.png
    golden/ean-rotation-r.zpl
    golden/ean-rotation-r-labelary-bitonal.png
    golden/ean-rotation-i.zpl
    golden/ean-rotation-i-labelary-bitonal.png
    golden/ean-rotation-b.zpl
    golden/ean-rotation-b-labelary-bitonal.png
    golden/dpi.zpl
    golden/dpi-203-labelary-bitonal.png
    golden/dpi-300-labelary-bitonal.png
    golden/dpi-600-labelary-bitonal.png
)
# QtTest's Windows GUI executable does not reliably expose stdout to CTest.
# Always keep the detailed result in the build tree, including failed rows.
add_test(NAME qtzpl_tests COMMAND qtzpl_tests -o "${CMAKE_CURRENT_BINARY_DIR}/test_results.txt,txt")

add_executable(qtzpl_command_extensions_tests test_command_extensions.cpp ../src/linear_encoders.cpp)
target_link_libraries(qtzpl_command_extensions_tests PRIVATE QtZpl::QtZpl Qt6::Test)
file(GLOB QTZPL_EXTENSION_GOLDENS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/golden/extensions/*.zpl" "${CMAKE_CURRENT_SOURCE_DIR}/golden/extensions/*.png")
qt_add_resources(qtzpl_command_extensions_tests "extensions" PREFIX "/qtzpl/extensions" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/extensions" FILES ${QTZPL_EXTENSION_GOLDENS})
add_test(NAME qtzpl_command_extensions_tests COMMAND qtzpl_command_extensions_tests -o "${CMAKE_CURRENT_BINARY_DIR}/extensions_results.txt,txt")

foreach(encoder IN ITEMS aztec databar micropdf417 maxicode_extended datamatrix_rectangular)
  set(encoder_test "test_${encoder}.cpp")
  if(encoder MATCHES "^(aztec|databar|micropdf417)$")
    set(encoder_test "test_${encoder}_encoder.cpp")
  endif()
  add_executable(qtzpl_${encoder}_tests ${encoder_test})
  target_compile_definitions(qtzpl_${encoder}_tests PRIVATE QTZPL_STATIC)
  target_link_libraries(qtzpl_${encoder}_tests PRIVATE QtZpl::QtZpl Qt6::Test)
  add_test(NAME qtzpl_${encoder}_tests COMMAND qtzpl_${encoder}_tests -o "${CMAKE_CURRENT_BINARY_DIR}/${encoder}_results.txt,txt")
endforeach()
target_sources(qtzpl_aztec_tests PRIVATE ../src/aztec_encoder.cpp)
target_sources(qtzpl_databar_tests PRIVATE ../src/databar_encoder.cpp)
target_sources(qtzpl_micropdf417_tests PRIVATE ../src/micropdf417_encoder.cpp)
target_sources(qtzpl_maxicode_extended_tests PRIVATE ../src/maxicode_encoder.cpp)
foreach(group IN ITEMS aztec databar micropdf417 maxicode-extended datamatrix-rectangular datamatrix-diagnostics pdf417-truncated)
  file(GLOB group_goldens CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/golden/${group}/*.zpl" "${CMAKE_CURRENT_SOURCE_DIR}/golden/${group}/*.png")
  string(REPLACE "-" "_" target_suffix "${group}")
  if(group STREQUAL "pdf417-truncated")
    set(target_suffix micropdf417)
  elseif(group STREQUAL "datamatrix-diagnostics")
    set(target_suffix datamatrix_rectangular)
  endif()
  qt_add_resources(qtzpl_${target_suffix}_tests "${target_suffix}_${group}_goldens" PREFIX "/qtzpl/tests/golden" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden" FILES ${group_goldens})
endforeach()

add_executable(qtzpl_code128_extended_tests test_code128_extended.cpp)
target_link_libraries(qtzpl_code128_extended_tests PRIVATE QtZpl::QtZpl Qt6::Test)
file(GLOB QTZPL_CODE128_EXTENDED_GOLDENS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/golden/code128-extended/*.zpl" "${CMAKE_CURRENT_SOURCE_DIR}/golden/code128-extended/*.png")
qt_add_resources(qtzpl_code128_extended_tests "code128_extended" PREFIX "/qtzpl/tests/golden" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden" FILES ${QTZPL_CODE128_EXTENDED_GOLDENS})
add_test(NAME qtzpl_code128_extended_tests COMMAND qtzpl_code128_extended_tests -o "${CMAKE_CURRENT_BINARY_DIR}/code128_extended_results.txt,txt")

add_executable(qtzpl_barcode_anchors_tests test_barcode_anchors.cpp)
target_link_libraries(qtzpl_barcode_anchors_tests PRIVATE QtZpl::QtZpl Qt6::Test)
file(GLOB QTZPL_ANCHOR_GOLDENS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/golden/barcode-anchors/*.zpl" "${CMAKE_CURRENT_SOURCE_DIR}/golden/barcode-anchors/*.png")
qt_add_resources(qtzpl_barcode_anchors_tests "barcode_anchors" PREFIX "/qtzpl/tests/golden" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden" FILES ${QTZPL_ANCHOR_GOLDENS})
add_test(NAME qtzpl_barcode_anchors_tests COMMAND qtzpl_barcode_anchors_tests -o "${CMAKE_CURRENT_BINARY_DIR}/barcode_anchors_results.txt,txt")

foreach(group IN ITEMS retail-native-fonts font-glyphs)
  if(group STREQUAL "retail-native-fonts")
    set(test_name retail_font)
  else()
    set(test_name font_glyphs)
  endif()
  add_executable(qtzpl_${test_name}_tests test_${test_name}.cpp)
  target_link_libraries(qtzpl_${test_name}_tests PRIVATE QtZpl::QtZpl Qt6::Test)
  file(GLOB glyph_goldens CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/${group}/*.zpl"
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/${group}/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/${group}/*.json")
  qt_add_resources(qtzpl_${test_name}_tests "${test_name}_goldens"
    PREFIX "/qtzpl/tests/golden" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden" FILES ${glyph_goldens})
  add_test(NAME qtzpl_${test_name}_tests COMMAND qtzpl_${test_name}_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/${test_name}_results.txt,txt")
endforeach()

# Correctness guards for measured optimization paths. Timings remain opt-in.
add_executable(qtzpl_font0_raster_tests test_font0_raster.cpp)
target_link_libraries(qtzpl_font0_raster_tests PRIVATE QtZpl::QtZpl Qt6::Test)
qt_add_resources(qtzpl_font0_raster_tests "northline_raster" PREFIX "/northline"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/designer-default"
  FILES golden/designer-default/northline.zpl
        golden/designer-default/northline-graphics.zpl
        golden/designer-default/northline-graphics-labelary-bitonal.png)
add_test(NAME qtzpl_font0_raster_tests COMMAND qtzpl_font0_raster_tests
  -o "${CMAKE_CURRENT_BINARY_DIR}/font0_raster_results.txt,txt")
qt_add_resources(qtzpl_font0_raster_tests "font0_grid_semantics" PREFIX "/font0-semantics"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-grid-semantics"
  FILES golden/font0-grid-semantics/minimum-size.zpl
        golden/font0-grid-semantics/minimum-size-labelary-bitonal.png)

foreach(area IN ITEMS encoder parser renderer qr)
  add_executable(qtzpl_${area}_performance_tests test_${area}_performance.cpp)
  target_link_libraries(qtzpl_${area}_performance_tests PRIVATE QtZpl::QtZpl Qt6::Test)
  add_test(NAME qtzpl_${area}_performance_tests COMMAND qtzpl_${area}_performance_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/${area}_performance_results.txt,txt")
endforeach()

file(GLOB QTZPL_DEMO_CORPUS_FIXTURES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/corpus/go-zpl-demo/*.zpl"
)
qt_add_resources(qtzpl_tests "qtzpl_demo_corpus"
  PREFIX "/qtzpl/tests/corpus/go-zpl-demo"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/corpus/go-zpl-demo"
  FILES
    corpus/go-zpl-demo/manifest.json
    ${QTZPL_DEMO_CORPUS_FIXTURES}
)

file(GLOB QTZPL_DEMO_CORPUS_GOLDENS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/go-zpl-demo/*-labelary-bitonal.png"
)
qt_add_resources(qtzpl_tests "qtzpl_demo_corpus_goldens"
  PREFIX "/qtzpl/tests/golden/go-zpl-demo"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/go-zpl-demo"
  FILES ${QTZPL_DEMO_CORPUS_GOLDENS}
)

# This large corpus is an optional offline integrity check, not a renderer
# parity test. Keep its resources out of the ordinary test executables.
option(QTZPL_BUILD_FONT0_SWEEP_TESTS "Embed and verify the complete Font 0 size-sweep references" OFF)
if(QTZPL_BUILD_FONT0_SWEEP_TESTS)
  add_executable(qtzpl_font0_sweep_references_tests test_font0_sweep_references.cpp)
  target_link_libraries(qtzpl_font0_sweep_references_tests PRIVATE Qt6::Core Qt6::Test)
  file(GLOB_RECURSE QTZPL_FONT0_SWEEP_REFERENCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-size-sweep/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-size-sweep/*.zpl"
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-size-sweep/*.png")
  qt_add_resources(qtzpl_font0_sweep_references_tests "font0_size_sweep_references"
    BIG_RESOURCES
    PREFIX "/qtzpl/tests/golden/font0-size-sweep"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-size-sweep"
    FILES ${QTZPL_FONT0_SWEEP_REFERENCES})
  add_test(NAME qtzpl_font0_sweep_references_tests
    COMMAND qtzpl_font0_sweep_references_tests
      -o "${CMAKE_CURRENT_BINARY_DIR}/font0_sweep_references_results.txt,txt")
  set_tests_properties(qtzpl_font0_sweep_references_tests PROPERTIES LABELS "font0-sweep;reference-integrity")
endif()

add_executable(qtzpl_field_data_tests test_field_data.cpp)
target_link_libraries(qtzpl_field_data_tests PRIVATE QtZpl::QtZpl Qt6::Test)
add_test(NAME qtzpl_field_data_tests COMMAND qtzpl_field_data_tests
  -o "${CMAKE_CURRENT_BINARY_DIR}/field_data_results.txt,txt")

add_executable(qtzpl_parser_contract_tests test_parser_contract.cpp)
target_link_libraries(qtzpl_parser_contract_tests PRIVATE QtZpl::QtZpl Qt6::Test)
add_test(NAME qtzpl_parser_contract_tests COMMAND qtzpl_parser_contract_tests
  -o "${CMAKE_CURRENT_BINARY_DIR}/parser_contract_results.txt,txt")

add_executable(qtzpl_designer_api_tests test_designer_api.cpp)
target_link_libraries(qtzpl_designer_api_tests PRIVATE QtZpl::QtZpl Qt6::Test)
add_test(NAME qtzpl_designer_api_tests COMMAND qtzpl_designer_api_tests
  -o "${CMAKE_CURRENT_BINARY_DIR}/designer_api_results.txt,txt")

# Original Font 0 advance rulers and independent FO/FT anchor fixtures.
add_executable(qtzpl_font0_metrics_tests test_font0_metrics.cpp)
target_link_libraries(qtzpl_font0_metrics_tests PRIVATE QtZpl::QtZpl Qt6::Test)
file(GLOB font0_metric_goldens CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-metrics/*.zpl"
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-metrics/*.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-metrics/*.json")
qt_add_resources(qtzpl_font0_metrics_tests "font0_metric_goldens"
  PREFIX "/font0-metrics" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-metrics"
  FILES ${font0_metric_goldens})
file(GLOB font0_anchor_goldens CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-h*-w*.zpl"
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-h*-w*-labelary-bitonal.png")
qt_add_resources(qtzpl_font0_metrics_tests "font0_anchor_goldens"
  PREFIX "/font0-metrics" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden"
  FILES ${font0_anchor_goldens})
qt_add_resources(qtzpl_font0_metrics_tests "font0_metric_font"
  PREFIX "/font0-metrics" BASE "${PROJECT_SOURCE_DIR}/resources/fonts"
  FILES "${PROJECT_SOURCE_DIR}/resources/fonts/font0.ttf")
add_test(NAME qtzpl_font0_metrics_tests COMMAND qtzpl_font0_metrics_tests
  -o "${CMAKE_CURRENT_BINARY_DIR}/font0_metrics_results.txt,txt")

# Direct design-data contracts: no GUI/platform font backend is required.
add_executable(qtzpl_font0_face_tests test_font0_face.cpp)
target_link_libraries(qtzpl_font0_face_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test ${QTZPL_FREETYPE_TARGETS})
qt_add_resources(qtzpl_font0_face_tests "font0_face_font"
  PREFIX "/font0-face" BASE "${PROJECT_SOURCE_DIR}/resources/fonts"
  FILES "${PROJECT_SOURCE_DIR}/resources/fonts/font0.ttf")
qt_add_resources(qtzpl_font0_face_tests "font0_face_coverage"
  PREFIX "/font0-face" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-metrics"
  FILES golden/font0-metrics/embedded-font-coverage.json)
add_test(NAME qtzpl_font0_face_tests COMMAND qtzpl_font0_face_tests
  -o "${CMAKE_CURRENT_BINARY_DIR}/font0_face_results.txt,txt")

qt_add_resources(qtzpl_font0_raster_tests "font0_rotation_anchors"
  PREFIX "/font0-anchors" BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden/font0-anchors"
  FILES golden/font0-anchors/anchors-h17-w17.zpl
        golden/font0-anchors/anchors-h17-w17-labelary-bitonal.png
        golden/font0-anchors/anchors-h64-w23.zpl
        golden/font0-anchors/anchors-h64-w23-labelary-bitonal.png
        golden/font0-anchors/block-anchors.zpl
        golden/font0-anchors/block-anchors-labelary-bitonal.png)
