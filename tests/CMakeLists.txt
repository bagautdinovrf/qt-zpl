add_executable(qtzpl_tests test_qtzpl.cpp)
target_link_libraries(qtzpl_tests PRIVATE QtZpl::QtZpl Qt6::Test)
qt_add_resources(qtzpl_tests "qtzpl_test_goldens"
  PREFIX "/qtzpl/tests/golden"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden"
  FILES
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

# Correctness guards for measured optimization paths. Timings remain opt-in.
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
