add_executable(qtzpl_tests test_qtzpl.cpp)
target_link_libraries(qtzpl_tests PRIVATE QtZpl::QtZpl Qt6::Test)
qt_add_resources(qtzpl_tests "qtzpl_test_goldens"
  PREFIX "/qtzpl/tests/golden"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/golden"
  FILES
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
add_test(NAME qtzpl_tests COMMAND qtzpl_tests)

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
